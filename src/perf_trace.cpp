// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "perf_trace.h"

#include "async_video_trace.h"
#include "options.h"

#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>
#include <libaegisub/util.h>

#include <wx/utils.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <mutex>
#include <sstream>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#undef CreateDirectory
#endif

namespace perf_trace {
namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kBufferedEntryLimit = 64;
constexpr size_t kBufferedByteLimit = 64 * 1024;
constexpr auto kBufferedFlushInterval = std::chrono::milliseconds(250);
constexpr auto kVideoMemorySampleInterval = std::chrono::milliseconds(500);
constexpr double kAudioUiTimerRequestedMs = 20.0;
#ifdef _WIN32
// wxTimer(20 ms) on Windows commonly wakes on a coarser cadence unless the
// process opts into a higher timer resolution. Use a more realistic baseline
// for mean_abs_jitter so the summary reflects normal WM_TIMER behavior.
constexpr double kAudioUiTimerJitterTargetMs = 31.25;
#else
constexpr double kAudioUiTimerJitterTargetMs = kAudioUiTimerRequestedMs;
#endif
constexpr double kVideoPlaybackTargetMs = 10.0;

enum class TraceCategory : uint32_t {
	None = 0,
	Ops = 1u << 0,
	Video = 1u << 1,
	Audio = 1u << 2,
	Memory = 1u << 3,
	UiWindow = 1u << 4,
	LuaDialog = 1u << 5,
	Log = 1u << 6,
	All = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6),
};

TraceCategory operator|(TraceCategory left, TraceCategory right) {
	return static_cast<TraceCategory>(static_cast<uint32_t>(left) | static_cast<uint32_t>(right));
}

TraceCategory& operator|=(TraceCategory& left, TraceCategory right) {
	left = left | right;
	return left;
}

bool HasAnyCategory(TraceCategory value, TraceCategory wanted) {
	return (static_cast<uint32_t>(value) & static_cast<uint32_t>(wanted)) != 0;
}

TraceCategory ToTraceCategory(Category category) {
	switch (category) {
		case Category::Ops: return TraceCategory::Ops;
		case Category::Video: return TraceCategory::Video;
		case Category::Audio: return TraceCategory::Audio;
		case Category::Memory: return TraceCategory::Memory;
		case Category::UiWindow: return TraceCategory::UiWindow;
		case Category::LuaDialog: return TraceCategory::LuaDialog;
		case Category::Log: return TraceCategory::Log;
	}
	return TraceCategory::None;
}

std::atomic<bool> trace_active{ false };

struct TraceSelection {
	bool enabled = false;
	TraceCategory categories = TraceCategory::None;
	std::string source_tag;
	std::string selection_tag;
};

int64_t NowNs() {
	using namespace std::chrono;
	return duration_cast<nanoseconds>(Clock::now().time_since_epoch()).count();
}

std::string Trim(std::string value) {
	auto const begin = value.find_first_not_of(" \t\r\n");
	if (begin == std::string::npos)
		return {};
	auto const end = value.find_last_not_of(" \t\r\n");
	return value.substr(begin, end - begin + 1);
}

std::string ToLower(std::string value) {
	for (char& ch : value) {
		if (ch >= 'A' && ch <= 'Z')
			ch = static_cast<char>(ch - 'A' + 'a');
	}
	return value;
}

std::string NormalizeSummaryKey(std::string_view value) {
	std::string normalized;
	normalized.reserve(value.size());
	for (unsigned char ch : value) {
		if ((ch >= 'a' && ch <= 'z')
			|| (ch >= 'A' && ch <= 'Z')
			|| (ch >= '0' && ch <= '9')
			|| ch == '.'
			|| ch == '_'
			|| ch == '-') {
			normalized.push_back(static_cast<char>(ch));
		}
		else {
			normalized.push_back('_');
		}
	}
	if (normalized.empty())
		normalized = "unnamed";
	return normalized;
}

std::string MakeWindowPhaseSummaryKey(char const* window_kind, char const* phase) {
	auto key = NormalizeSummaryKey(window_kind ? window_kind : "window");
	auto phase_key = NormalizeSummaryKey(phase ? phase : "phase");
	if (!phase_key.empty()) {
		key += ".";
		key += phase_key;
	}
	return key;
}

std::string MakeAudioUiPhaseSummaryKey(char const* phase) {
	return NormalizeSummaryKey(phase ? phase : "phase");
}

std::string MakeVideoUiPhaseSummaryKey(char const* phase) {
	return NormalizeSummaryKey(phase ? phase : "phase");
}

bool IsFalseyToken(std::string const& value) {
	auto lowered = ToLower(Trim(value));
	if (lowered.empty()) return true;
	return lowered == "0" || lowered == "false" || lowered == "off" || lowered == "no";
}

bool IsTruthyToken(std::string const& value) {
	auto lowered = ToLower(Trim(value));
	if (lowered.empty()) return false;
	return lowered == "1" || lowered == "true" || lowered == "on" || lowered == "yes";
}

void AppendUnique(std::vector<std::string>& values, std::string value) {
	if (value.empty())
		return;
	if (std::find(values.begin(), values.end(), value) == values.end())
		values.emplace_back(std::move(value));
}

std::vector<std::string> SplitTraceTokens(std::string const& value) {
	std::vector<std::string> tokens;
	std::string current;
	for (char ch : value) {
		switch (ch) {
			case ',':
			case ';':
			case '|':
			case '+':
			case ' ':
			case '\t':
			case '\r':
			case '\n':
				AppendUnique(tokens, ToLower(Trim(current)));
				current.clear();
				break;
			default:
				current.push_back(ch);
				break;
		}
	}
	AppendUnique(tokens, ToLower(Trim(current)));
	return tokens;
}

std::string JoinTokens(std::vector<std::string> const& tokens) {
	std::string joined;
	for (auto const& token : tokens) {
		if (!joined.empty())
			joined += ",";
		joined += token;
	}
	return joined;
}

bool ApplyCategoryToken(std::string const& token, TraceCategory& categories, std::vector<std::string>& normalized_tokens) {
	if (token == "all" || token == "default" || IsTruthyToken(token)) {
		categories = TraceCategory::All;
		AppendUnique(normalized_tokens, "all");
		return true;
	}
	if (token == "audio") {
		categories |= TraceCategory::Audio;
		AppendUnique(normalized_tokens, "audio");
		return true;
	}
	if (token == "video") {
		categories |= TraceCategory::Video;
		AppendUnique(normalized_tokens, "video");
		return true;
	}
	if (token == "memory" || token == "mem" || token == "video-memory") {
		categories |= TraceCategory::Memory;
		AppendUnique(normalized_tokens, "memory");
		return true;
	}
	if (token == "ui-window" || token == "ui_window" || token == "window" || token == "window-open") {
		categories |= TraceCategory::UiWindow;
		AppendUnique(normalized_tokens, "ui-window");
		return true;
	}
	if (token == "lua-dialog" || token == "lua_dialog" || token == "lua") {
		categories |= TraceCategory::LuaDialog;
		AppendUnique(normalized_tokens, "lua-dialog");
		return true;
	}
	if (token == "log" || token == "logs") {
		categories |= TraceCategory::Log;
		AppendUnique(normalized_tokens, "log");
		return true;
	}
	if (token == "ops" || token == "op" || token == "playback") {
		categories |= TraceCategory::Ops;
		AppendUnique(normalized_tokens, "ops");
		return true;
	}
	return false;
}

std::string SelectionTagFromCategories(TraceCategory categories) {
	if (categories == TraceCategory::All)
		return "all";

	std::vector<std::string> tokens;
	if (HasAnyCategory(categories, TraceCategory::Audio))
		tokens.emplace_back("audio");
	if (HasAnyCategory(categories, TraceCategory::Video))
		tokens.emplace_back("video");
	if (HasAnyCategory(categories, TraceCategory::Memory))
		tokens.emplace_back("memory");
	if (HasAnyCategory(categories, TraceCategory::UiWindow))
		tokens.emplace_back("ui-window");
	if (HasAnyCategory(categories, TraceCategory::LuaDialog))
		tokens.emplace_back("lua-dialog");
	if (HasAnyCategory(categories, TraceCategory::Log))
		tokens.emplace_back("log");
	if (HasAnyCategory(categories, TraceCategory::Ops))
		tokens.emplace_back("ops");
	return JoinTokens(tokens);
}

TraceSelection ParseTraceSelection(std::string const& value) {
	TraceSelection selection;
	auto const tokens = SplitTraceTokens(value);
	if (tokens.empty())
		return selection;

	std::vector<std::string> normalized_tokens;
	bool recognized_any = false;
	for (auto const& token : tokens) {
		if (IsFalseyToken(token))
			continue;
		if (ApplyCategoryToken(token, selection.categories, normalized_tokens)) {
			recognized_any = true;
			continue;
		}
		AppendUnique(normalized_tokens, token);
	}

	if (selection.categories == TraceCategory::None) {
		if (!recognized_any && !normalized_tokens.empty())
			selection.categories = TraceCategory::All;
		else
			return selection;
	}

	selection.enabled = true;
	selection.source_tag = normalized_tokens.empty() ? SelectionTagFromCategories(selection.categories) : JoinTokens(normalized_tokens);
	selection.selection_tag = SelectionTagFromCategories(selection.categories);
	return selection;
}

std::string ReadEnvValue(char const* name) {
	if (auto* value = std::getenv(name))
		return value;
	return {};
}

std::string EscapeJson(std::string_view value) {
	std::string escaped;
	escaped.reserve(value.size() + 8);
	for (unsigned char ch : value) {
		switch (ch) {
			case '\\': escaped += "\\\\"; break;
			case '"': escaped += "\\\""; break;
			case '\b': escaped += "\\b"; break;
			case '\f': escaped += "\\f"; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default:
				if (ch < 0x20) {
					char buffer[7];
					snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
					escaped += buffer;
				}
				else {
					escaped += static_cast<char>(ch);
				}
				break;
		}
	}
	return escaped;
}

template <typename T>
std::string ToString(T value) {
	std::ostringstream out;
	out.imbue(std::locale::classic());
	out << value;
	return out.str();
}

std::string ToStringDouble(double value) {
	std::ostringstream out;
	out.imbue(std::locale::classic());
	out << std::setprecision(6) << value;
	return out.str();
}

class JsonObjectBuilder {
	std::string value = "{";
	bool first = true;

	void AddKey(std::string_view key) {
		if (!first)
			value += ",";
		first = false;
		value += "\"";
		value += EscapeJson(key);
		value += "\":";
	}

public:
	void AddString(std::string_view key, std::string_view field_value) {
		AddKey(key);
		value += "\"";
		value += EscapeJson(field_value);
		value += "\"";
	}

	void AddBool(std::string_view key, bool field_value) {
		AddKey(key);
		value += field_value ? "true" : "false";
	}

	void AddInt(std::string_view key, int64_t field_value) {
		AddKey(key);
		value += ToString(field_value);
	}

	void AddUInt(std::string_view key, uint64_t field_value) {
		AddKey(key);
		value += ToString(field_value);
	}

	void AddDouble(std::string_view key, double field_value) {
		AddKey(key);
		value += ToStringDouble(field_value);
	}

	std::string Finish() {
		value += "}";
		return std::move(value);
	}
};

struct IntervalSummary {
	uint64_t count = 0;
	double min_ms = 0.0;
	double max_ms = 0.0;
	double total_ms = 0.0;
	double total_abs_jitter_ms = 0.0;
	bool has_last = false;
	int64_t last_ns = 0;

	void Reset() {
		has_last = false;
		last_ns = 0;
	}

	bool Observe(int64_t now_ns, double target_ms, double& observed_ms) {
		if (!has_last) {
			has_last = true;
			last_ns = now_ns;
			return false;
		}

		observed_ms = static_cast<double>(now_ns - last_ns) / 1000000.0;
		last_ns = now_ns;
		++count;
		total_ms += observed_ms;
		total_abs_jitter_ms += std::abs(observed_ms - target_ms);
		if (count == 1) {
			min_ms = observed_ms;
			max_ms = observed_ms;
		}
		else {
			min_ms = std::min(min_ms, observed_ms);
			max_ms = std::max(max_ms, observed_ms);
		}
		return true;
	}
};

struct DurationSummary {
	uint64_t count = 0;
	double min_ms = 0.0;
	double max_ms = 0.0;
	double total_ms = 0.0;

	void Observe(double duration_ms) {
		++count;
		total_ms += duration_ms;
		if (count == 1) {
			min_ms = duration_ms;
			max_ms = duration_ms;
		}
		else {
			min_ms = std::min(min_ms, duration_ms);
			max_ms = std::max(max_ms, duration_ms);
		}
	}
};

struct AudioDisplayTraceSummary {
	uint64_t samples = 0;
	uint64_t complete_content_frames = 0;
	uint64_t swap_failures = 0;
	uint64_t source_cache_max_bytes = 0;
	uint64_t cpu_tile_max_bytes = 0;
	uint64_t cpu_payload_max_bytes = 0;
	uint64_t fft_max_bytes = 0;
	uint64_t gpu_tile_max_bytes = 0;
	AudioDisplaySnapshot latest;
};

struct Summary {
	uint64_t trace_entries = 0;
	uint64_t buffered_flushes = 0;
	uint64_t immediate_flushes = 0;
	uint64_t frame_requests = 0;
	uint64_t frame_requests_immediate = 0;
	uint64_t frame_delivered = 0;
	uint64_t frame_delivered_immediate = 0;
	uint64_t frame_dropped = 0;
	uint64_t video_render_packet_cache_hit = 0;
	uint64_t video_render_packet_cache_miss = 0;
	uint64_t window_open_success = 0;
	uint64_t window_open_failure = 0;
	uint64_t lua_dialog_success = 0;
	uint64_t lua_dialog_failure = 0;
	uint64_t video_memory_samples = 0;
	std::array<uint64_t, 5> log_counts = { 0, 0, 0, 0, 0 };
	std::map<std::string, uint64_t> op_counts;
	size_t process_working_set_max_bytes = 0;
	size_t process_private_max_bytes = 0;
	size_t provider_cache_bgra_max_bytes = 0;
	size_t provider_cache_native_max_bytes = 0;
	size_t async_source_pool_max_bytes = 0;
	size_t async_composited_pool_max_bytes = 0;
	size_t async_overlay_pool_max_bytes = 0;
	size_t display_pending_packet_ref_max_bytes = 0;
	size_t display_displayed_packet_ref_max_bytes = 0;
	size_t renderer_primary_texture_max_bytes = 0;
	size_t renderer_secondary_texture_max_bytes = 0;
	size_t display_scene_cache_texture_max_bytes = 0;
	size_t audio_storage_max_bytes = 0;
	size_t audio_logical_max_bytes = 0;
	size_t audio_decoded_max_bytes = 0;
	size_t audio_page_size_bytes = 0;
	size_t audio_loading_max_bytes = 0;
	size_t audio_pinned_max_bytes = 0;
	size_t audio_free_max_bytes = 0;
	int64_t audio_resident_pages_max = 0;
	int64_t audio_loading_pages_max = 0;
	int64_t audio_pinned_pages_max = 0;
	int64_t audio_free_pages_max = 0;
	uint64_t audio_output_samples = 0;
	uint64_t audio_output_low_water = 0;
	uint64_t audio_output_starved = 0;
	uint64_t audio_output_recovered = 0;
	uint64_t audio_output_end_of_stream = 0;
	int64_t audio_output_queue_max_buffers = 0;
	double audio_output_queue_max_ms = 0.0;
	uint64_t audio_output_submitted_total_buffers = 0;
	uint64_t audio_output_submitted_total_frames = 0;
	uint64_t audio_output_submitted_total_bytes = 0;
	int64_t audio_output_submitted_max_buffers = 0;
	int64_t audio_output_submitted_max_frames = 0;
	int64_t audio_output_submitted_max_bytes = 0;
	double audio_output_submitted_max_ms = 0.0;
	std::string audio_provider_name;
	std::string audio_storage_kind;
	std::string audio_output_backend;
	std::map<std::string, DurationSummary> window_phase_durations;
	std::vector<std::string> window_phase_order;
	std::map<std::string, DurationSummary> audio_ui_phase_durations;
	std::vector<std::string> audio_ui_phase_order;
	std::map<std::string, DurationSummary> video_ui_phase_durations;
	std::vector<std::string> video_ui_phase_order;
	IntervalSummary audio_ui_timer_interval;
	IntervalSummary video_playback_tick_interval;
	DurationSummary window_open_duration;
	DurationSummary lua_dialog_duration;
	DurationSummary audio_output_fill_duration;
	AudioDisplayTraceSummary audio_display;
};

class TraceLogEmitter final : public agi::log::Emitter {
public:
	void log(agi::log::SinkMessage const& sm) override;
};

class AsyncVideoPerfTraceSink final : public aegisub::async_video_trace::Sink {
public:
	void ObserveFrameResult(int frame, double time, bool delivered, bool immediate) override {
		perf_trace::ObserveFrameResult(frame, time, delivered, immediate);
	}

	void ObserveVideoFrameRenderDuration(int frame, double time, bool delivered, bool immediate, double duration_ms) override {
		perf_trace::ObserveVideoFrameRenderDuration(frame, time, delivered, immediate, duration_ms);
	}
};

struct Session {
	std::mutex mutex;
	bool enabled = false;
	bool closing = false;
	TraceCategory categories = TraceCategory::All;
	bool has_pending_lua_dialog_open = false;
	int64_t pending_lua_dialog_open_started_ns = 0;
	int64_t last_video_memory_sample_ns = 0;
	agi::fs::path directory;
	std::ofstream trace_stream;
	std::vector<std::string> buffered_lines;
	size_t buffered_bytes = 0;
	Clock::time_point last_flush = Clock::now();
	std::string session_id;
	std::string build_label;
	std::string source_tag;
	std::string selection_tag;
	std::string started_local;
	TraceLogEmitter* log_emitter = nullptr;
	Summary summary;
};

AsyncVideoPerfTraceSink async_video_perf_trace_sink;

struct StartupTimingSession {
	std::mutex mutex;
	bool env_checked = false;
	bool enabled = false;
	int64_t started_ns = 0;
	std::string session_id;
	std::string started_local;
	agi::fs::path output_path;
	std::ofstream stream;
	std::vector<std::string> pending_lines;
	bool path_logged = false;
};

Session& GetSession() {
	static Session session;
	return session;
}

StartupTimingSession& GetStartupTimingSession() {
	static StartupTimingSession session;
	return session;
}

char const* SeverityName(agi::log::Severity severity) {
	switch (severity) {
		case agi::log::Exception: return "exception";
		case agi::log::Assert: return "assert";
		case agi::log::Warning: return "warning";
		case agi::log::Info: return "info";
		case agi::log::Debug: return "debug";
	}
	return "unknown";
}

bool IsCategoryEnabledLocked(Session const& session, TraceCategory categories) {
	return HasAnyCategory(session.categories, categories);
}

void UpdateObservedName(std::string& current, std::string const& value) {
	if (value.empty())
		return;
	if (current.empty()) {
		current = value;
		return;
	}
	if (current != value)
		current = "multiple";
}

void FlushLocked(Session& session, bool immediate) {
	if (!session.trace_stream.is_open() || session.buffered_lines.empty())
		return;

	for (auto const& line : session.buffered_lines)
		session.trace_stream << line << '\n';
	session.trace_stream.flush();
	session.buffered_lines.clear();
	session.buffered_bytes = 0;
	session.last_flush = Clock::now();
	if (immediate)
		++session.summary.immediate_flushes;
	else
		++session.summary.buffered_flushes;
}

void AppendEntryLocked(Session& session, char const* kind, std::string const& name, std::string payload, bool immediate, int64_t timestamp_ns) {
	if (!session.trace_stream.is_open())
		return;

	std::string line;
	line.reserve(session.session_id.size() + name.size() + payload.size() + 96);
	line += "{\"session\":\"";
	line += EscapeJson(session.session_id);
	line += "\",\"t_monotonic_ns\":";
	line += ToString(timestamp_ns);
	line += ",\"kind\":\"";
	line += kind;
	line += "\",\"name\":\"";
	line += EscapeJson(name);
	line += "\",\"payload\":";
	line += payload;
	line += "}";

	++session.summary.trace_entries;
	session.buffered_bytes += line.size() + 1;
	session.buffered_lines.emplace_back(std::move(line));

	bool const should_flush =
		immediate
		|| session.buffered_lines.size() >= kBufferedEntryLimit
		|| session.buffered_bytes >= kBufferedByteLimit
		|| Clock::now() - session.last_flush >= kBufferedFlushInterval;
	if (should_flush)
		FlushLocked(session, immediate);
}

bool IsStartupTimingEnabledLocked(StartupTimingSession& session) {
	if (session.env_checked)
		return session.enabled;

	session.env_checked = true;
	auto const value = Trim(ReadEnvValue("AEGISUB_STARTUP_DEBUG_LOG"));
	session.enabled = !value.empty() && !IsFalseyToken(value);
	if (session.enabled) {
		session.started_ns = NowNs();
		session.started_local = agi::util::strftime("%Y-%m-%d-%H-%M-%S");
		session.session_id = session.started_local + "-" + ToString(static_cast<long long>(wxGetProcessId()));
	}
	return session.enabled;
}

agi::fs::path ResolveStartupTimingDirectory(bool allow_temp_fallback) {
	if (config::path)
		return config::path->Decode("?user/log");
	if (!allow_temp_fallback)
		return {};
	try {
		return std::filesystem::temp_directory_path() / "Aegisub";
	}
	catch (...) {
		return {};
	}
}

bool EnsureStartupTimingStreamLocked(StartupTimingSession& session, bool allow_temp_fallback) {
	if (session.stream.is_open())
		return true;

	auto const directory = ResolveStartupTimingDirectory(allow_temp_fallback);
	if (directory.empty())
		return false;

	try {
		agi::fs::CreateDirectory(directory);
		if (session.output_path.empty()) {
			auto filename = agi::fs::PathFromString(
				"startup-timing-" + (session.session_id.empty() ? std::string("session") : session.session_id) + "-%%%%%%%%.ndjson");
			session.output_path = agi::fs::UniquePath(directory / filename);
		}

		session.stream = agi::io::OpenOutputFileStream(session.output_path, std::ios::out | std::ios::trunc);
		if (!session.stream.is_open())
			return false;

		for (auto const& line : session.pending_lines)
			session.stream << line << '\n';
		session.stream.flush();
		session.pending_lines.clear();

		if (!session.path_logged && agi::log::log) {
			session.path_logged = true;
			LOG_I("perf/startup") << "Startup timing log: " << agi::fs::PathToString(session.output_path);
		}
		return true;
	}
	catch (...) {
		return false;
	}
}

void MaybeLogStartupTimingPathLocked(StartupTimingSession& session) {
	if (session.path_logged || !session.stream.is_open() || !agi::log::log)
		return;
	session.path_logged = true;
	LOG_I("perf/startup") << "Startup timing log: " << agi::fs::PathToString(session.output_path);
}

template <typename PayloadBuilder>
void RecordStartupTimingEntry(char const* window_kind, char const* kind, std::string const& name, PayloadBuilder&& fill_payload, bool allow_temp_fallback = false, int64_t timestamp_ns = NowNs()) {
	if (std::string_view(window_kind ? window_kind : "") != "main")
		return;

	auto& session = GetStartupTimingSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!IsStartupTimingEnabledLocked(session))
		return;

	if (session.started_ns == 0)
		session.started_ns = timestamp_ns;

	JsonObjectBuilder payload;
	payload.AddString("window_kind", window_kind ? window_kind : "");
	payload.AddDouble("since_process_start_ms", static_cast<double>(timestamp_ns - session.started_ns) / 1000000.0);
	fill_payload(payload);

	std::string line;
	line.reserve(session.session_id.size() + name.size() + 256);
	line += "{\"session\":\"";
	line += EscapeJson(session.session_id);
	line += "\",\"source\":\"AEGISUB_STARTUP_DEBUG_LOG\",\"t_monotonic_ns\":";
	line += ToString(timestamp_ns);
	line += ",\"kind\":\"";
	line += kind;
	line += "\",\"name\":\"";
	line += EscapeJson(name);
	line += "\",\"payload\":";
	line += payload.Finish();
	line += "}";

	if (!EnsureStartupTimingStreamLocked(session, allow_temp_fallback))
		session.pending_lines.emplace_back(std::move(line));
	else {
		MaybeLogStartupTimingPathLocked(session);
		session.stream << line << '\n';
		session.stream.flush();
	}
}

template <typename PayloadBuilder>
void RecordEntry(TraceCategory categories, char const* kind, std::string const& name, bool immediate, PayloadBuilder&& fill_payload, int64_t timestamp_ns = NowNs()) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, categories))
		return;

	if (kind[0] == 'o')
		++session.summary.op_counts[name];

	JsonObjectBuilder payload;
	fill_payload(payload);
	AppendEntryLocked(session, kind, name, payload.Finish(), immediate, timestamp_ns);
}

void WriteManifest(Session const& session) {
	auto out = agi::io::OpenOutputFileStream(session.directory / "manifest.txt", std::ios::out | std::ios::trunc);
	if (!out.is_open())
		return;

	out.imbue(std::locale::classic());
	out << "session=" << session.session_id << "\n";
	out << "build=" << session.build_label << "\n";
	out << "source=" << session.source_tag << "\n";
	out << "trace_selection=" << session.selection_tag << "\n";
	out << "started_local=" << session.started_local << "\n";
	out << "pid=" << wxGetProcessId() << "\n";
	out << "platform=" << wxGetOsDescription().ToStdString(wxConvUTF8) << "\n";
	out << "session_dir=.\n";
	out << "trace_file=trace.ndjson\n";
	out << "summary_file=summary.txt\n";
	out.flush();
}

void WriteSummaryLocked(Session const& session) {
	auto out = agi::io::OpenOutputFileStream(session.directory / "summary.txt", std::ios::out | std::ios::trunc);
	if (!out.is_open())
		return;

	auto write_double = [&out](char const* key, double value) {
		out << key << "=" << ToStringDouble(value) << "\n";
	};
	auto write_int = [&out](char const* key, uint64_t value) {
		out << key << "=" << value << "\n";
	};
	auto write_mean = [&write_double](char const* key, double total, uint64_t count) {
		write_double(key, count ? total / count : 0.0);
	};

	out.imbue(std::locale::classic());
	out << "session=" << session.session_id << "\n";
	out << "build=" << session.build_label << "\n";
	out << "trace.selection=" << session.selection_tag << "\n";
	write_int("trace.entries", session.summary.trace_entries);
	write_int("trace.flushes.buffered", session.summary.buffered_flushes);
	write_int("trace.flushes.immediate", session.summary.immediate_flushes);
	write_int("frame.request.total", session.summary.frame_requests);
	write_int("frame.request.immediate", session.summary.frame_requests_immediate);
	write_int("frame.delivered.total", session.summary.frame_delivered);
	write_int("frame.delivered.immediate", session.summary.frame_delivered_immediate);
	write_int("frame.dropped.total", session.summary.frame_dropped);
	write_int("video.render_packet_cache.hit", session.summary.video_render_packet_cache_hit);
	write_int("video.render_packet_cache.miss", session.summary.video_render_packet_cache_miss);
	write_int("window_open.success", session.summary.window_open_success);
	write_int("window_open.failure", session.summary.window_open_failure);
	write_int("lua_dialog.success", session.summary.lua_dialog_success);
	write_int("lua_dialog.failure", session.summary.lua_dialog_failure);
	write_int("video_memory.samples", session.summary.video_memory_samples);
	write_int("log.exception", session.summary.log_counts[agi::log::Exception]);
	write_int("log.assert", session.summary.log_counts[agi::log::Assert]);
	write_int("log.warning", session.summary.log_counts[agi::log::Warning]);
	write_int("log.info", session.summary.log_counts[agi::log::Info]);
	write_int("log.debug", session.summary.log_counts[agi::log::Debug]);

	write_double("audio_ui_timer_interval.requested_ms", kAudioUiTimerRequestedMs);
	write_double("audio_ui_timer_interval.jitter_target_ms", kAudioUiTimerJitterTargetMs);
	write_int("audio_ui_timer_interval.count", session.summary.audio_ui_timer_interval.count);
	write_double("audio_ui_timer_interval.min_ms", session.summary.audio_ui_timer_interval.min_ms);
	write_double("audio_ui_timer_interval.max_ms", session.summary.audio_ui_timer_interval.max_ms);
	write_mean("audio_ui_timer_interval.mean_ms", session.summary.audio_ui_timer_interval.total_ms, session.summary.audio_ui_timer_interval.count);
	write_mean("audio_ui_timer_interval.mean_abs_jitter_ms", session.summary.audio_ui_timer_interval.total_abs_jitter_ms, session.summary.audio_ui_timer_interval.count);
	for (auto const& key : session.summary.audio_ui_phase_order) {
		auto const it = session.summary.audio_ui_phase_durations.find(key);
		if (it == session.summary.audio_ui_phase_durations.end())
			continue;
		auto const& phase = it->second;
		out << "audio_ui_phase." << key << ".count=" << phase.count << "\n";
		out << "audio_ui_phase." << key << ".total_ms=" << ToStringDouble(phase.total_ms) << "\n";
		out << "audio_ui_phase." << key << ".min_ms=" << ToStringDouble(phase.min_ms) << "\n";
		out << "audio_ui_phase." << key << ".max_ms=" << ToStringDouble(phase.max_ms) << "\n";
		out << "audio_ui_phase." << key << ".mean_ms=" << ToStringDouble(phase.count ? phase.total_ms / phase.count : 0.0) << "\n";
	}
	auto const& audio_display = session.summary.audio_display;
	write_int("audio_display.snapshot.samples", audio_display.samples);
	write_int("audio_display.content.complete_frames", audio_display.complete_content_frames);
	write_int("audio_display.swap.failures", audio_display.swap_failures);
	write_int("audio_display.source_cache.bytes.max", audio_display.source_cache_max_bytes);
	write_int("audio_display.cpu_tile.bytes.max", audio_display.cpu_tile_max_bytes);
	write_int("audio_display.cpu_payload.bytes.max", audio_display.cpu_payload_max_bytes);
	write_int("audio_display.fft.bytes.max", audio_display.fft_max_bytes);
	write_int("audio_display.gpu_tile.bytes.max", audio_display.gpu_tile_max_bytes);
	out << "audio_display.renderer.latest=" << audio_display.latest.renderer_name << "\n";
	out << "audio_display.content_kind.latest=" << audio_display.latest.content_kind << "\n";
	write_int("audio_display.frame.latest", audio_display.latest.frame_id);
	write_double("audio_display.content_scale.latest", audio_display.latest.content_scale);
	out << "audio_display.cursor.source.latest=" << audio_display.latest.cursor_source << "\n";
	out << "audio_display.cursor.position_ms.latest=" << audio_display.latest.cursor_position_ms << "\n";
	write_double("audio_display.cursor.device_x.latest", audio_display.latest.cursor_device_x);
	write_int("audio_display.cursor.label_visible.latest", audio_display.latest.cursor_label_visible ? 1 : 0);
	write_int("audio_display.bitmap_cache.hits.latest", audio_display.latest.bitmap_cache_hits);
	write_int("audio_display.bitmap_cache.misses.latest", audio_display.latest.bitmap_cache_misses);
	write_int("audio_display.source_cache.budget_bytes.latest", audio_display.latest.source_cache_budget_bytes);
	write_int("audio_display.source_cache.bytes.latest", audio_display.latest.source_cache_bytes);
	write_int("audio_display.source_cache.entries.latest", audio_display.latest.source_cache_entries);
	write_int("audio_display.source_cache.hits.latest", audio_display.latest.source_cache_hits);
	write_int("audio_display.source_cache.misses.latest", audio_display.latest.source_cache_misses);
	write_int("audio_display.source_cache.visible_builds.latest", audio_display.latest.source_cache_visible_builds);
	write_int("audio_display.source_cache.visible_lock_contention.latest", audio_display.latest.source_cache_visible_lock_contention);
	write_int("audio_display.source_cache.prefetch_requests.latest", audio_display.latest.source_cache_prefetch_requests);
	write_int("audio_display.source_cache.prefetch_builds.latest", audio_display.latest.source_cache_prefetch_builds);
	write_int("audio_display.source_cache.prefetch_busy_skips.latest", audio_display.latest.source_cache_prefetch_busy_skips);
	write_int("audio_display.source_cache.stale_drops.latest", audio_display.latest.source_cache_stale_drops);
	write_int("audio_display.source_cache.evictions.latest", audio_display.latest.source_cache_evictions);
	write_int("audio_display.source_cache.prefetch_enabled.latest", audio_display.latest.source_cache_prefetch_enabled ? 1 : 0);
	write_int("audio_display.cpu_tile.budget_bytes.latest", audio_display.latest.cpu_tile_budget_bytes);
	write_int("audio_display.cpu_tile.bytes.latest", audio_display.latest.cpu_tile_bytes);
	write_int("audio_display.cpu_tile.entries.latest", audio_display.latest.cpu_tile_entries);
	write_int("audio_display.cpu_tile.evictions.latest", audio_display.latest.cpu_tile_evictions);
	write_int("audio_display.cpu_tile.hits.latest", audio_display.latest.cpu_tile_hits);
	write_int("audio_display.cpu_tile.misses.latest", audio_display.latest.cpu_tile_misses);
	write_int("audio_display.cpu_payload.budget_bytes.latest", audio_display.latest.cpu_payload_budget_bytes);
	write_int("audio_display.cpu_payload.bytes.latest", audio_display.latest.cpu_payload_bytes);
	write_int("audio_display.cpu_payload.entries.latest", audio_display.latest.cpu_payload_entries);
	write_int("audio_display.cpu_payload.evictions.latest", audio_display.latest.cpu_payload_evictions);
	write_int("audio_display.cpu_payload.hits.latest", audio_display.latest.cpu_payload_hits);
	write_int("audio_display.cpu_payload.misses.latest", audio_display.latest.cpu_payload_misses);
	write_int("audio_display.fft.budget_bytes.latest", audio_display.latest.fft_budget_bytes);
	write_int("audio_display.fft.active_cache_budget_bytes.latest", audio_display.latest.fft_active_cache_budget_bytes);
	write_int("audio_display.fft.bytes.latest", audio_display.latest.fft_bytes);
	write_int("audio_display.fft.entries.latest", audio_display.latest.fft_entries);
	write_int("audio_display.fft.evictions.latest", audio_display.latest.fft_evictions);
	write_int("audio_display.fft.hits.latest", audio_display.latest.fft_hits);
	write_int("audio_display.fft.misses.latest", audio_display.latest.fft_misses);
	write_int("audio_display.fft.visible_builds.latest", audio_display.latest.fft_visible_builds);
	write_int("audio_display.gpu_tile.budget_bytes.latest", audio_display.latest.gpu_tile_budget_bytes);
	write_int("audio_display.gpu_tile.bytes.latest", audio_display.latest.gpu_tile_bytes);
	write_int("audio_display.gpu_tile.entries.latest", audio_display.latest.gpu_tile_entries);
	write_int("audio_display.gpu_tile.evictions.latest", audio_display.latest.gpu_tile_evictions);
	write_int("audio_display.gpu_tile.hits.latest", audio_display.latest.gpu_tile_hits);
	write_int("audio_display.gpu_tile.misses.latest", audio_display.latest.gpu_tile_misses);
	write_int("audio_display.gpu_tile.uploads.latest", audio_display.latest.gpu_tile_uploads);
	write_int("audio_display.gpu_tile.upload_bytes.latest", audio_display.latest.gpu_tile_upload_bytes);
	write_int("audio_display.worker.builds_started.latest", audio_display.latest.worker_builds_started);
	write_int("audio_display.worker.builds_ready.latest", audio_display.latest.worker_builds_ready);
	write_int("audio_display.worker.builds_cancelled.latest", audio_display.latest.worker_builds_cancelled);
	write_int("audio_display.worker.payload_builds_started.latest", audio_display.latest.worker_payload_builds_started);
	write_int("audio_display.worker.payload_builds_ready.latest", audio_display.latest.worker_payload_builds_ready);
	write_int("audio_display.worker.payload_builds_cancelled.latest", audio_display.latest.worker_payload_builds_cancelled);
	write_int("audio_display.worker.superseded_requests.latest", audio_display.latest.worker_superseded_requests);
	for (auto const& key : session.summary.video_ui_phase_order) {
		auto const it = session.summary.video_ui_phase_durations.find(key);
		if (it == session.summary.video_ui_phase_durations.end())
			continue;
		auto const& phase = it->second;
		out << "video_ui_phase." << key << ".count=" << phase.count << "\n";
		out << "video_ui_phase." << key << ".total_ms=" << ToStringDouble(phase.total_ms) << "\n";
		out << "video_ui_phase." << key << ".min_ms=" << ToStringDouble(phase.min_ms) << "\n";
		out << "video_ui_phase." << key << ".max_ms=" << ToStringDouble(phase.max_ms) << "\n";
		out << "video_ui_phase." << key << ".mean_ms=" << ToStringDouble(phase.count ? phase.total_ms / phase.count : 0.0) << "\n";
	}

	write_int("video_playback_tick_interval.count", session.summary.video_playback_tick_interval.count);
	write_double("video_playback_tick_interval.min_ms", session.summary.video_playback_tick_interval.min_ms);
	write_double("video_playback_tick_interval.max_ms", session.summary.video_playback_tick_interval.max_ms);
	write_mean("video_playback_tick_interval.mean_ms", session.summary.video_playback_tick_interval.total_ms, session.summary.video_playback_tick_interval.count);
	write_mean("video_playback_tick_interval.mean_abs_jitter_ms", session.summary.video_playback_tick_interval.total_abs_jitter_ms, session.summary.video_playback_tick_interval.count);

	write_int("window_open_duration.count", session.summary.window_open_duration.count);
	write_double("window_open_duration.min_ms", session.summary.window_open_duration.min_ms);
	write_double("window_open_duration.max_ms", session.summary.window_open_duration.max_ms);
	write_mean("window_open_duration.mean_ms", session.summary.window_open_duration.total_ms, session.summary.window_open_duration.count);
	for (auto const& key : session.summary.window_phase_order) {
		auto const it = session.summary.window_phase_durations.find(key);
		if (it == session.summary.window_phase_durations.end())
			continue;
		auto const& phase = it->second;
		out << "window_phase." << key << ".count=" << phase.count << "\n";
		out << "window_phase." << key << ".total_ms=" << ToStringDouble(phase.total_ms) << "\n";
		out << "window_phase." << key << ".min_ms=" << ToStringDouble(phase.min_ms) << "\n";
		out << "window_phase." << key << ".max_ms=" << ToStringDouble(phase.max_ms) << "\n";
		out << "window_phase." << key << ".mean_ms=" << ToStringDouble(phase.count ? phase.total_ms / phase.count : 0.0) << "\n";
	}

	write_int("lua_dialog_duration.count", session.summary.lua_dialog_duration.count);
	write_double("lua_dialog_duration.min_ms", session.summary.lua_dialog_duration.min_ms);
	write_double("lua_dialog_duration.max_ms", session.summary.lua_dialog_duration.max_ms);
	write_mean("lua_dialog_duration.mean_ms", session.summary.lua_dialog_duration.total_ms, session.summary.lua_dialog_duration.count);
	write_int("process_working_set.max_bytes", session.summary.process_working_set_max_bytes);
	write_int("process_private.max_bytes", session.summary.process_private_max_bytes);
	write_int("provider_cache_bgra.max_bytes", session.summary.provider_cache_bgra_max_bytes);
	write_int("provider_cache_native.max_bytes", session.summary.provider_cache_native_max_bytes);
	write_int("async_source_pool.max_bytes", session.summary.async_source_pool_max_bytes);
	write_int("async_composited_pool.max_bytes", session.summary.async_composited_pool_max_bytes);
	write_int("async_overlay_pool.max_bytes", session.summary.async_overlay_pool_max_bytes);
	write_int("display_pending_packet_ref.max_bytes", session.summary.display_pending_packet_ref_max_bytes);
	write_int("display_displayed_packet_ref.max_bytes", session.summary.display_displayed_packet_ref_max_bytes);
	write_int("renderer_primary_texture.max_bytes", session.summary.renderer_primary_texture_max_bytes);
	write_int("renderer_secondary_texture.max_bytes", session.summary.renderer_secondary_texture_max_bytes);
	write_int("display_scene_cache_texture.max_bytes", session.summary.display_scene_cache_texture_max_bytes);
	write_int("audio_storage.max_bytes", session.summary.audio_storage_max_bytes);
	write_int("audio_logical.max_bytes", session.summary.audio_logical_max_bytes);
	write_int("audio_decoded.max_bytes", session.summary.audio_decoded_max_bytes);
	write_int("audio_cache.page_size_bytes", session.summary.audio_page_size_bytes);
	write_int("audio_cache.loading.max_bytes", session.summary.audio_loading_max_bytes);
	write_int("audio_cache.pinned.max_bytes", session.summary.audio_pinned_max_bytes);
	write_int("audio_cache.free.max_bytes", session.summary.audio_free_max_bytes);
	write_int("audio_cache.resident_pages.max", static_cast<uint64_t>(session.summary.audio_resident_pages_max));
	write_int("audio_cache.loading_pages.max", static_cast<uint64_t>(session.summary.audio_loading_pages_max));
	write_int("audio_cache.pinned_pages.max", static_cast<uint64_t>(session.summary.audio_pinned_pages_max));
	write_int("audio_cache.free_pages.max", static_cast<uint64_t>(session.summary.audio_free_pages_max));
	write_int("audio_output.samples", session.summary.audio_output_samples);
	write_int("audio_output.low_water.count", session.summary.audio_output_low_water);
	write_int("audio_output.starved.count", session.summary.audio_output_starved);
	write_int("audio_output.recovered.count", session.summary.audio_output_recovered);
	write_int("audio_output.end_of_stream.count", session.summary.audio_output_end_of_stream);
	write_int("audio_output.queue.max_buffers", static_cast<uint64_t>(session.summary.audio_output_queue_max_buffers));
	write_double("audio_output.queue.max_ms", session.summary.audio_output_queue_max_ms);
	write_int("audio_output.submitted.total_buffers", session.summary.audio_output_submitted_total_buffers);
	write_int("audio_output.submitted.total_frames", session.summary.audio_output_submitted_total_frames);
	write_int("audio_output.submitted.total_bytes", session.summary.audio_output_submitted_total_bytes);
	write_int("audio_output.submitted.max_buffers", static_cast<uint64_t>(session.summary.audio_output_submitted_max_buffers));
	write_int("audio_output.submitted.max_frames", static_cast<uint64_t>(session.summary.audio_output_submitted_max_frames));
	write_int("audio_output.submitted.max_bytes", static_cast<uint64_t>(session.summary.audio_output_submitted_max_bytes));
	write_double("audio_output.submitted.max_ms", session.summary.audio_output_submitted_max_ms);
	write_int("audio_output.fill_duration.count", session.summary.audio_output_fill_duration.count);
	write_double("audio_output.fill_duration.min_ms", session.summary.audio_output_fill_duration.min_ms);
	write_double("audio_output.fill_duration.max_ms", session.summary.audio_output_fill_duration.max_ms);
	write_mean("audio_output.fill_duration.mean_ms", session.summary.audio_output_fill_duration.total_ms, session.summary.audio_output_fill_duration.count);
	out << "audio_provider=" << session.summary.audio_provider_name << "\n";
	out << "audio_storage_kind=" << session.summary.audio_storage_kind << "\n";
	out << "audio_output_backend=" << session.summary.audio_output_backend << "\n";

	for (auto const& [name, count] : session.summary.op_counts)
		out << "op." << name << "=" << count << "\n";

	out.flush();
}

void TraceLogEntry(agi::log::SinkMessage const& sm) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::Log))
		return;

	++session.summary.log_counts[sm.severity];

	JsonObjectBuilder payload;
	payload.AddString("severity", SeverityName(sm.severity));
	payload.AddString("message", sm.message);
	payload.AddString("func", sm.func ? sm.func : "");
	payload.AddInt("line", sm.line);
	AppendEntryLocked(
		session,
		"log",
		sm.section ? sm.section : "log",
		payload.Finish(),
		sm.severity <= agi::log::Warning,
		sm.time);
}

void TraceLogEmitter::log(agi::log::SinkMessage const& sm) {
	TraceLogEntry(sm);
}

} // namespace

bool IsEnabled() {
	return trace_active.load(std::memory_order_relaxed);
}

bool IsCategoryEnabled(Category category) {
	if (!trace_active.load(std::memory_order_relaxed))
		return false;

	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing)
		return false;
	return IsCategoryEnabledLocked(session, ToTraceCategory(category));
}

AudioUiDurationScope::AudioUiDurationScope(char const* phase, int detail_a, int detail_b) noexcept
: phase(phase)
, detail_a(detail_a)
, detail_b(detail_b)
{
	if (IsEnabled())
		started_ns = NowNs();
}

AudioUiDurationScope::~AudioUiDurationScope() noexcept {
	if (!started_ns)
		return;
	auto const duration_ms = static_cast<double>(NowNs() - started_ns) / 1'000'000.0;
	ObserveAudioUiDuration(phase, duration_ms, detail_a, detail_b, duration_ms >= 8.0);
}

void AudioUiDurationScope::SetDetails(int first, int second) noexcept {
	detail_a = first;
	detail_b = second;
}

VideoUiDurationScope::VideoUiDurationScope(char const* phase, int detail_a, int detail_b) noexcept
: phase(phase)
, detail_a(detail_a)
, detail_b(detail_b)
{
	if (IsEnabled())
		started_ns = NowNs();
}

VideoUiDurationScope::~VideoUiDurationScope() noexcept {
	if (!started_ns)
		return;
	auto const duration_ms = static_cast<double>(NowNs() - started_ns) / 1'000'000.0;
	ObserveVideoUiDuration(phase, duration_ms, detail_a, detail_b, duration_ms >= 8.0);
}

void VideoUiDurationScope::SetDetails(int first, int second) noexcept {
	detail_a = first;
	detail_b = second;
}

bool ShouldSampleVideoMemory(bool force) {
	if (!trace_active.load(std::memory_order_relaxed))
		return false;

	auto const timestamp_ns = NowNs();
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing
		|| !IsCategoryEnabledLocked(session, TraceCategory::Memory | TraceCategory::Audio | TraceCategory::Video))
		return false;
	if (force)
		return true;
	return timestamp_ns - session.last_video_memory_sample_ns
		>= std::chrono::duration_cast<std::chrono::nanoseconds>(kVideoMemorySampleInterval).count();
}

agi::fs::path GetSessionDirectory() {
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	return session.directory;
}

void Initialize(std::string const& build_label) {
	auto const env_value = ReadEnvValue("AEGISUB_PERF_TRACE");
	auto const selection = ParseTraceSelection(env_value);
	if (!selection.enabled || !config::path)
		return;

	auto const root = config::path->Decode("?user/perf-sessions");
	auto const session_name = agi::util::strftime("%Y-%m-%d-%H-%M-%S") + "-" + ToString(static_cast<long long>(wxGetProcessId())) + "-%%%%%%%%";
	InitializeAt(agi::fs::UniquePath(root / session_name), build_label, selection.source_tag);
}

void InitializeAt(agi::fs::path const& session_dir, std::string const& build_label, std::string const& source_tag) {
	Shutdown();

	auto& session = GetSession();
	auto selection = ParseTraceSelection(source_tag);
	if (!selection.enabled) {
		selection.enabled = true;
		selection.categories = TraceCategory::All;
		selection.source_tag = source_tag.empty() ? "manual" : Trim(source_tag);
		selection.selection_tag = "all";
	}
	try {
		agi::fs::CreateDirectory(session_dir);
		auto trace_stream = agi::io::OpenOutputFileStream(session_dir / "trace.ndjson", std::ios::out | std::ios::trunc);
		if (!trace_stream.is_open())
			return;

		TraceLogEmitter* emitter_ptr = nullptr;
		if (agi::log::log) {
			auto emitter = agi::make_unique<TraceLogEmitter>();
			emitter_ptr = emitter.get();
			agi::log::log->Subscribe(std::move(emitter));
		}

		std::lock_guard<std::mutex> lock(session.mutex);
		session.enabled = true;
		session.closing = false;
		session.directory = session_dir;
		session.trace_stream = std::move(trace_stream);
		session.buffered_lines.clear();
		session.buffered_bytes = 0;
		session.last_flush = Clock::now();
		session.has_pending_lua_dialog_open = false;
		session.pending_lua_dialog_open_started_ns = 0;
		session.last_video_memory_sample_ns = 0;
		session.session_id = agi::fs::PathToString(session_dir.filename());
		session.build_label = build_label;
		session.source_tag = selection.source_tag.empty() ? "manual" : selection.source_tag;
		session.selection_tag = selection.selection_tag.empty() ? "all" : selection.selection_tag;
		session.categories = selection.categories;
		session.started_local = agi::util::strftime("%Y-%m-%d %H:%M:%S");
		session.log_emitter = emitter_ptr;
		session.summary = Summary{};

		WriteManifest(session);
		aegisub::async_video_trace::SetSink(&async_video_perf_trace_sink);
		trace_active.store(true, std::memory_order_relaxed);
	}
	catch (...) {
	}
}

void Shutdown() {
	aegisub::async_video_trace::SetSink(nullptr);

	auto& session = GetSession();
	if (!session.enabled && !trace_active.load(std::memory_order_relaxed))
		return;

	TraceLogEmitter* emitter = nullptr;
	{
		std::lock_guard<std::mutex> lock(session.mutex);
		emitter = session.log_emitter;
	}

	if (emitter && agi::log::log)
		agi::log::log->Unsubscribe(emitter);

	{
		std::lock_guard<std::mutex> lock(session.mutex);
		if (!session.enabled)
			return;

		session.closing = true;
		FlushLocked(session, true);
		WriteSummaryLocked(session);
		session.trace_stream.close();
		session.log_emitter = nullptr;
		session.enabled = false;
	}

	trace_active.store(false, std::memory_order_relaxed);
}

void ResetAudioUiTimerInterval() {
	if (!trace_active.load(std::memory_order_relaxed))
		return;
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::Audio))
		return;
	session.summary.audio_ui_timer_interval.Reset();
}

void ResetVideoPlaybackInterval() {
	if (!trace_active.load(std::memory_order_relaxed))
		return;
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::Video))
		return;
	session.summary.video_playback_tick_interval.Reset();
}

void TraceVideoOpen(agi::fs::path const& path, int width, int height, int frame_count, bool has_audio, std::string const& decoder_name, double duration_ms) {
	RecordEntry(TraceCategory::Video | TraceCategory::Ops, "op", "video_open", true, [&](JsonObjectBuilder& payload) {
		payload.AddString("path", agi::fs::PathToString(path));
		payload.AddInt("width", width);
		payload.AddInt("height", height);
		payload.AddInt("frame_count", frame_count);
		payload.AddBool("has_audio", has_audio);
		payload.AddString("decoder", decoder_name);
		payload.AddDouble("duration_ms", duration_ms);
	});
}

void TracePlayStart(int frame, int start_ms) {
	RecordEntry(TraceCategory::Audio | TraceCategory::Video | TraceCategory::Ops, "op", "play_start", true, [&](JsonObjectBuilder& payload) {
		payload.AddInt("frame", frame);
		payload.AddInt("start_ms", start_ms);
	});
}

void TracePlayStop(int frame) {
	RecordEntry(TraceCategory::Audio | TraceCategory::Video | TraceCategory::Ops, "op", "play_stop", true, [&](JsonObjectBuilder& payload) {
		payload.AddInt("frame", frame);
	});
}

void TraceSeek(int frame, bool was_playing) {
	RecordEntry(TraceCategory::Audio | TraceCategory::Video | TraceCategory::Ops, "op", "seek", true, [&](JsonObjectBuilder& payload) {
		payload.AddInt("frame", frame);
		payload.AddBool("was_playing", was_playing);
	});
}

void TraceAudioMiddleSeek(char const* phase, int time_ms, int frame) {
	RecordEntry(TraceCategory::Audio | TraceCategory::Video | TraceCategory::Ops, "op", "audio_middle_seek", false, [&](JsonObjectBuilder& payload) {
		payload.AddString("phase", phase ? phase : "");
		payload.AddInt("time_ms", time_ms);
		payload.AddInt("frame", frame);
	});
}

void TraceVideoStepPreviewConfig(bool enabled, int interval_ms, int interval_backward_ms, int burst_window_ms, int burst_threshold, int release_delay_ms) {
	RecordEntry(TraceCategory::Video | TraceCategory::Ops, "op", "video_step_preview_config", true, [&](JsonObjectBuilder& payload) {
		payload.AddBool("enabled", enabled);
		payload.AddInt("interval_ms", interval_ms);
		payload.AddInt("interval_backward_ms", interval_backward_ms);
		payload.AddInt("burst_window_ms", burst_window_ms);
		payload.AddInt("burst_threshold", burst_threshold);
		payload.AddInt("release_delay_ms", release_delay_ms);
	});
}

void TraceVideoStepPreviewBegin(int start_frame, int delta, int burst_count, int burst_threshold) {
	RecordEntry(TraceCategory::Video | TraceCategory::Ops, "op", "video_step_preview_begin", false, [&](JsonObjectBuilder& payload) {
		payload.AddInt("start_frame", start_frame);
		payload.AddInt("delta", delta);
		payload.AddInt("burst_count", burst_count);
		payload.AddInt("burst_threshold", burst_threshold);
	});
}

void TraceVideoStepPreviewCancel(int frame) {
	RecordEntry(TraceCategory::Video | TraceCategory::Ops, "op", "video_step_preview_cancel", false, [&](JsonObjectBuilder& payload) {
		payload.AddInt("frame", frame);
	});
}

void TraceVideoStepPreviewRelease(int final_target_frame) {
	RecordEntry(TraceCategory::Video | TraceCategory::Ops, "op", "video_step_preview_release", false, [&](JsonObjectBuilder& payload) {
		payload.AddInt("final_target_frame", final_target_frame);
	});
}

void ObserveFrameRequest(int frame, double time, bool immediate) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::Video))
		return;

	++session.summary.frame_requests;
	if (immediate)
		++session.summary.frame_requests_immediate;

	JsonObjectBuilder payload;
	payload.AddInt("frame", frame);
	payload.AddDouble("time_ms", time);
	payload.AddBool("immediate", immediate);
	AppendEntryLocked(session, "metric", "video_frame_request", payload.Finish(), false, NowNs());
}

void ObserveFrameResult(int frame, double time, bool delivered, bool immediate) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::Video))
		return;

	if (delivered) {
		++session.summary.frame_delivered;
		if (immediate)
			++session.summary.frame_delivered_immediate;
	}
	else {
		++session.summary.frame_dropped;
	}

	JsonObjectBuilder payload;
	payload.AddInt("frame", frame);
	payload.AddDouble("time_ms", time);
	payload.AddBool("immediate", immediate);
	AppendEntryLocked(session, "metric", delivered ? "video_frame_delivered" : "video_frame_dropped", payload.Finish(), false, NowNs());
}

void ObserveVideoFrameRenderDuration(int frame, double time, bool delivered, bool immediate, double duration_ms) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::Video))
		return;

	JsonObjectBuilder payload;
	payload.AddInt("frame", frame);
	payload.AddDouble("time_ms", time);
	payload.AddBool("delivered", delivered);
	payload.AddBool("immediate", immediate);
	payload.AddDouble("duration_ms", duration_ms);
	AppendEntryLocked(session, "metric", "video_frame_render_duration", payload.Finish(), false, NowNs());
}

void ObserveVideoRenderPacketCacheLookup(int frame, bool hit, char const* source) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::Video))
		return;

	if (hit)
		++session.summary.video_render_packet_cache_hit;
	else
		++session.summary.video_render_packet_cache_miss;

	JsonObjectBuilder payload;
	payload.AddInt("frame", frame);
	payload.AddBool("hit", hit);
	payload.AddString("source", source ? source : "");
	AppendEntryLocked(session, "metric", hit ? "video_render_packet_cache_hit" : "video_render_packet_cache_miss", payload.Finish(), false, NowNs());
}

void ObserveAudioUiTimerPosition(int ms) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto const timestamp_ns = NowNs();
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::Audio))
		return;

	double interval_ms = 0.0;
	if (!session.summary.audio_ui_timer_interval.Observe(timestamp_ns, kAudioUiTimerJitterTargetMs, interval_ms))
		return;

	JsonObjectBuilder payload;
	payload.AddInt("position_ms", ms);
	payload.AddDouble("delta_ms", interval_ms);
	AppendEntryLocked(session, "metric", "audio_ui_timer_interval", payload.Finish(), false, timestamp_ns);
}

void ObserveVideoUiDuration(char const* phase, double duration_ms, int detail_a, int detail_b, bool immediate) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto const timestamp_ns = NowNs();
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::Video))
		return;

	auto const summary_key = MakeVideoUiPhaseSummaryKey(phase);
	auto [it, inserted] = session.summary.video_ui_phase_durations.emplace(summary_key, DurationSummary{});
	if (inserted)
		session.summary.video_ui_phase_order.emplace_back(summary_key);
	it->second.Observe(duration_ms);

	JsonObjectBuilder payload;
	payload.AddString("phase", phase ? phase : "");
	payload.AddDouble("duration_ms", duration_ms);
	if (detail_a >= 0)
		payload.AddInt("detail_a", detail_a);
	if (detail_b >= 0)
		payload.AddInt("detail_b", detail_b);
	AppendEntryLocked(session, "metric", "video_ui_duration", payload.Finish(), immediate, timestamp_ns);
}

void ObserveAudioUiDuration(char const* phase, double duration_ms, int detail_a, int detail_b, bool immediate) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto const timestamp_ns = NowNs();
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::Audio))
		return;

	auto const summary_key = MakeAudioUiPhaseSummaryKey(phase);
	auto [it, inserted] = session.summary.audio_ui_phase_durations.emplace(summary_key, DurationSummary{});
	if (inserted)
		session.summary.audio_ui_phase_order.emplace_back(summary_key);
	it->second.Observe(duration_ms);

	JsonObjectBuilder payload;
	payload.AddString("phase", phase ? phase : "");
	payload.AddDouble("duration_ms", duration_ms);
	if (detail_a >= 0)
		payload.AddInt("detail_a", detail_a);
	if (detail_b >= 0)
		payload.AddInt("detail_b", detail_b);
	AppendEntryLocked(session, "metric", "audio_ui_duration", payload.Finish(), immediate, timestamp_ns);
}

void ObserveAudioOutputSnapshot(AudioOutputSnapshot const& snapshot) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto const timestamp_ns = NowNs();
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::Audio))
		return;

	++session.summary.audio_output_samples;
	if (snapshot.low_water)
		++session.summary.audio_output_low_water;
	if (snapshot.starved)
		++session.summary.audio_output_starved;
	if (snapshot.recovered)
		++session.summary.audio_output_recovered;
	if (snapshot.end_of_stream)
		++session.summary.audio_output_end_of_stream;
	if (snapshot.queued_buffers >= 0)
		session.summary.audio_output_queue_max_buffers = std::max(session.summary.audio_output_queue_max_buffers, snapshot.queued_buffers);
	if (snapshot.queued_ms >= 0.0)
		session.summary.audio_output_queue_max_ms = std::max(session.summary.audio_output_queue_max_ms, snapshot.queued_ms);
	if (snapshot.submitted_buffers >= 0) {
		session.summary.audio_output_submitted_total_buffers += static_cast<uint64_t>(snapshot.submitted_buffers);
		session.summary.audio_output_submitted_max_buffers = std::max(session.summary.audio_output_submitted_max_buffers, snapshot.submitted_buffers);
	}
	if (snapshot.submitted_frames >= 0) {
		session.summary.audio_output_submitted_total_frames += static_cast<uint64_t>(snapshot.submitted_frames);
		session.summary.audio_output_submitted_max_frames = std::max(session.summary.audio_output_submitted_max_frames, snapshot.submitted_frames);
	}
	if (snapshot.submitted_bytes >= 0) {
		session.summary.audio_output_submitted_total_bytes += static_cast<uint64_t>(snapshot.submitted_bytes);
		session.summary.audio_output_submitted_max_bytes = std::max(session.summary.audio_output_submitted_max_bytes, snapshot.submitted_bytes);
	}
	if (snapshot.submitted_ms >= 0.0)
		session.summary.audio_output_submitted_max_ms = std::max(session.summary.audio_output_submitted_max_ms, snapshot.submitted_ms);
	if (snapshot.fill_duration_ms >= 0.0)
		session.summary.audio_output_fill_duration.Observe(snapshot.fill_duration_ms);
	UpdateObservedName(session.summary.audio_output_backend, snapshot.backend_name);

	JsonObjectBuilder payload;
	payload.AddString("backend", snapshot.backend_name);
	payload.AddString("reason", snapshot.reason);
	if (snapshot.queued_buffers >= 0)
		payload.AddInt("queued_buffers", snapshot.queued_buffers);
	if (snapshot.queued_ms >= 0.0)
		payload.AddDouble("queued_ms", snapshot.queued_ms);
	if (snapshot.submitted_buffers >= 0)
		payload.AddInt("submitted_buffers", snapshot.submitted_buffers);
	if (snapshot.submitted_frames >= 0)
		payload.AddInt("submitted_frames", snapshot.submitted_frames);
	if (snapshot.submitted_bytes >= 0)
		payload.AddInt("submitted_bytes", snapshot.submitted_bytes);
	if (snapshot.submitted_ms >= 0.0)
		payload.AddDouble("submitted_ms", snapshot.submitted_ms);
	if (snapshot.fill_duration_ms >= 0.0)
		payload.AddDouble("fill_duration_ms", snapshot.fill_duration_ms);
	if (snapshot.played_frames >= 0)
		payload.AddInt("played_frames", snapshot.played_frames);
	if (snapshot.engine_latency_frames >= 0)
		payload.AddInt("engine_latency_frames", snapshot.engine_latency_frames);
	if (snapshot.glitch_count >= 0)
		payload.AddInt("glitch_count", snapshot.glitch_count);
	if (snapshot.source_rate_hz >= 0)
		payload.AddInt("source_rate_hz", snapshot.source_rate_hz);
	if (snapshot.mastering_rate_hz >= 0)
		payload.AddInt("mastering_rate_hz", snapshot.mastering_rate_hz);
	payload.AddBool("low_water", snapshot.low_water);
	payload.AddBool("starved", snapshot.starved);
	payload.AddBool("recovered", snapshot.recovered);
	payload.AddBool("end_of_stream", snapshot.end_of_stream);
	AppendEntryLocked(
		session,
		"metric",
		"audio_output_snapshot",
		payload.Finish(),
		snapshot.starved || snapshot.recovered || snapshot.end_of_stream,
		timestamp_ns);
}

void ObserveAudioDisplaySnapshot(AudioDisplaySnapshot const& snapshot) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto const timestamp_ns = NowNs();
	JsonObjectBuilder payload;
	payload.AddString("renderer", snapshot.renderer_name);
	payload.AddString("content_kind", snapshot.content_kind);
	payload.AddUInt("frame_id", snapshot.frame_id);
	payload.AddUInt("provider_generation", snapshot.provider_generation);
	payload.AddUInt("analysis_generation", snapshot.analysis_generation);
	payload.AddUInt("marker_revision", snapshot.marker_revision);
	payload.AddUInt("chrome_revision", snapshot.chrome_revision);
	payload.AddUInt("presentation_revision", snapshot.presentation_revision);
	payload.AddDouble("content_scale", snapshot.content_scale);
	payload.AddUInt("viewport_first_column", snapshot.viewport_first_column);
	payload.AddUInt("viewport_column_count", snapshot.viewport_column_count);
	payload.AddUInt("target_width", snapshot.target_width);
	payload.AddUInt("target_height", snapshot.target_height);
	payload.AddUInt("visible_tile_count", snapshot.visible_tile_count);
	payload.AddUInt("ready_tile_count", snapshot.ready_tile_count);
	payload.AddBool("complete_content_viewport", snapshot.complete_content_viewport);
	payload.AddBool("retained_content_frame", snapshot.retained_content_frame);
	payload.AddBool("cursor_only", snapshot.cursor_only);
	payload.AddBool("retained_layers_reused", snapshot.retained_layers_reused);
	payload.AddBool("focused", snapshot.focused);
	payload.AddBool("middle_seek_active", snapshot.middle_seek_active);
	payload.AddString("cursor_source", snapshot.cursor_source);
	payload.AddInt("cursor_position_ms", snapshot.cursor_position_ms);
	payload.AddDouble("cursor_device_x", snapshot.cursor_device_x);
	payload.AddBool("cursor_label_visible", snapshot.cursor_label_visible);
	payload.AddBool("visible_content_request_called", snapshot.visible_content_request_called);
	payload.AddBool("content_lookup_performed", snapshot.content_lookup_performed);
	payload.AddUInt("content_tiles_drawn_this_frame", snapshot.content_tiles_drawn_this_frame);
	payload.AddUInt("gpu_tile_uploads_this_frame", snapshot.gpu_tile_uploads_this_frame);
	payload.AddBool("swap_attempted", snapshot.swap_attempted);
	payload.AddBool("swapped", snapshot.swapped);

	payload.AddUInt("bitmap_cache_hits", snapshot.bitmap_cache_hits);
	payload.AddUInt("bitmap_cache_misses", snapshot.bitmap_cache_misses);
	payload.AddUInt("source_cache_budget_bytes", snapshot.source_cache_budget_bytes);
	payload.AddUInt("source_cache_bytes", snapshot.source_cache_bytes);
	payload.AddUInt("source_cache_entries", snapshot.source_cache_entries);
	payload.AddUInt("source_cache_hits", snapshot.source_cache_hits);
	payload.AddUInt("source_cache_misses", snapshot.source_cache_misses);
	payload.AddUInt("source_cache_visible_builds", snapshot.source_cache_visible_builds);
	payload.AddUInt("source_cache_visible_lock_contention", snapshot.source_cache_visible_lock_contention);
	payload.AddUInt("source_cache_prefetch_requests", snapshot.source_cache_prefetch_requests);
	payload.AddUInt("source_cache_prefetch_builds", snapshot.source_cache_prefetch_builds);
	payload.AddUInt("source_cache_prefetch_busy_skips", snapshot.source_cache_prefetch_busy_skips);
	payload.AddUInt("source_cache_stale_drops", snapshot.source_cache_stale_drops);
	payload.AddUInt("source_cache_evictions", snapshot.source_cache_evictions);
	payload.AddBool("source_cache_prefetch_enabled", snapshot.source_cache_prefetch_enabled);

	payload.AddUInt("cpu_tile_budget_bytes", snapshot.cpu_tile_budget_bytes);
	payload.AddUInt("cpu_tile_bytes", snapshot.cpu_tile_bytes);
	payload.AddUInt("cpu_tile_entries", snapshot.cpu_tile_entries);
	payload.AddUInt("cpu_tile_hits", snapshot.cpu_tile_hits);
	payload.AddUInt("cpu_tile_misses", snapshot.cpu_tile_misses);
	payload.AddUInt("cpu_tile_evictions", snapshot.cpu_tile_evictions);

	payload.AddUInt("cpu_payload_budget_bytes", snapshot.cpu_payload_budget_bytes);
	payload.AddUInt("cpu_payload_bytes", snapshot.cpu_payload_bytes);
	payload.AddUInt("cpu_payload_entries", snapshot.cpu_payload_entries);
	payload.AddUInt("cpu_payload_hits", snapshot.cpu_payload_hits);
	payload.AddUInt("cpu_payload_misses", snapshot.cpu_payload_misses);
	payload.AddUInt("cpu_payload_evictions", snapshot.cpu_payload_evictions);

	payload.AddUInt("fft_budget_bytes", snapshot.fft_budget_bytes);
	payload.AddUInt("fft_active_cache_budget_bytes", snapshot.fft_active_cache_budget_bytes);
	payload.AddUInt("fft_bytes", snapshot.fft_bytes);
	payload.AddUInt("fft_entries", snapshot.fft_entries);
	payload.AddUInt("fft_hits", snapshot.fft_hits);
	payload.AddUInt("fft_misses", snapshot.fft_misses);
	payload.AddUInt("fft_visible_builds", snapshot.fft_visible_builds);
	payload.AddUInt("fft_evictions", snapshot.fft_evictions);

	payload.AddUInt("gpu_tile_budget_bytes", snapshot.gpu_tile_budget_bytes);
	payload.AddUInt("gpu_tile_bytes", snapshot.gpu_tile_bytes);
	payload.AddUInt("gpu_tile_entries", snapshot.gpu_tile_entries);
	payload.AddUInt("gpu_tile_hits", snapshot.gpu_tile_hits);
	payload.AddUInt("gpu_tile_misses", snapshot.gpu_tile_misses);
	payload.AddUInt("gpu_tile_uploads", snapshot.gpu_tile_uploads);
	payload.AddUInt("gpu_tile_upload_bytes", snapshot.gpu_tile_upload_bytes);
	payload.AddUInt("gpu_tile_evictions", snapshot.gpu_tile_evictions);
	payload.AddUInt("gpu_palette_uploads", snapshot.gpu_palette_uploads);

	payload.AddUInt("worker_builds_started", snapshot.worker_builds_started);
	payload.AddUInt("worker_builds_ready", snapshot.worker_builds_ready);
	payload.AddUInt("worker_builds_cancelled", snapshot.worker_builds_cancelled);
	payload.AddUInt("worker_payload_builds_started", snapshot.worker_payload_builds_started);
	payload.AddUInt("worker_payload_builds_ready", snapshot.worker_payload_builds_ready);
	payload.AddUInt("worker_payload_builds_cancelled", snapshot.worker_payload_builds_cancelled);
	payload.AddUInt("worker_superseded_requests", snapshot.worker_superseded_requests);
	auto serialized_payload = payload.Finish();

	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::Audio))
		return;

	auto& summary = session.summary.audio_display;
	++summary.samples;
	if (snapshot.complete_content_viewport)
		++summary.complete_content_frames;
	if (snapshot.swap_attempted && !snapshot.swapped)
		++summary.swap_failures;
	summary.source_cache_max_bytes = std::max(summary.source_cache_max_bytes, snapshot.source_cache_bytes);
	summary.cpu_tile_max_bytes = std::max(summary.cpu_tile_max_bytes, snapshot.cpu_tile_bytes);
	summary.cpu_payload_max_bytes = std::max(summary.cpu_payload_max_bytes, snapshot.cpu_payload_bytes);
	summary.fft_max_bytes = std::max(summary.fft_max_bytes, snapshot.fft_bytes);
	summary.gpu_tile_max_bytes = std::max(summary.gpu_tile_max_bytes, snapshot.gpu_tile_bytes);
	summary.latest = snapshot;
	AppendEntryLocked(
		session,
		"metric",
		"audio_display_snapshot",
		serialized_payload,
		snapshot.swap_attempted && !snapshot.swapped,
		timestamp_ns);
}

void ObserveAudioContentTileEvent(AudioContentTileEvent const& event) noexcept try {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto const timestamp_ns = NowNs();
	JsonObjectBuilder payload;
	payload.AddString("stage", event.stage ? event.stage : "");
	payload.AddString("outcome", event.outcome ? event.outcome : "");
	payload.AddString("content_kind", event.spectrum ? "spectrum" : "waveform");
	payload.AddUInt("provider_generation", event.provider_generation);
	payload.AddUInt("analysis_generation", event.analysis_generation);
	payload.AddUInt("tile_index", event.tile_index);
	payload.AddUInt("column_count", event.column_count);
	payload.AddUInt("spectrum_bin_count", event.spectrum_bin_count);
	if (event.request_serial)
		payload.AddUInt("request_serial", event.request_serial);
	if (event.bytes)
		payload.AddUInt("bytes", event.bytes);
	if (event.variant_revision)
		payload.AddUInt("variant_revision", event.variant_revision);
	if (event.visible >= 0)
		payload.AddBool("visible", event.visible != 0);
	if (event.include_fft_deltas) {
		payload.AddUInt("fft_cache_hits_delta", event.fft_cache_hits_delta);
		payload.AddUInt("fft_cache_misses_delta", event.fft_cache_misses_delta);
		payload.AddUInt("fft_visible_builds_delta", event.fft_visible_builds_delta);
		payload.AddUInt("fft_cache_evictions_delta", event.fft_cache_evictions_delta);
	}
	if (event.include_diagnostics) {
		std::ostringstream hash;
		hash.imbue(std::locale::classic());
		hash << std::hex << std::setfill('0') << std::setw(16) << event.diagnostic_hash;
		payload.AddString("diagnostic_hash", hash.str());
		payload.AddUInt("diagnostic_elements", event.diagnostic_elements);
		payload.AddUInt("diagnostic_nonfinite", event.diagnostic_nonfinite);
		payload.AddUInt("diagnostic_nonzero_columns", event.diagnostic_nonzero_columns);
		payload.AddDouble("diagnostic_minimum", event.diagnostic_minimum);
		payload.AddDouble("diagnostic_maximum", event.diagnostic_maximum);
		if (event.diagnostic_gl) {
			payload.AddBool("diagnostic_gl_backend_available", event.diagnostic_gl_backend_available);
			payload.AddUInt("diagnostic_gl_texture_id", event.diagnostic_gl_texture_id);
			payload.AddUInt("diagnostic_gl_texture_target", event.diagnostic_gl_texture_target);
			payload.AddUInt("diagnostic_gl_owner_context", event.diagnostic_gl_owner_context);
			payload.AddUInt("diagnostic_gl_current_context", event.diagnostic_gl_current_context);
			payload.AddBool("diagnostic_gl_is_texture", event.diagnostic_gl_is_texture);
			payload.AddInt("diagnostic_gl_width", event.diagnostic_gl_width);
			payload.AddInt("diagnostic_gl_height", event.diagnostic_gl_height);
		}
	}
	auto serialized_payload = payload.Finish();

	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::Audio))
		return;
	AppendEntryLocked(
		session,
		"metric",
		"audio_content_tile_event",
		serialized_payload,
		false,
		timestamp_ns);
}
catch (...) {
}

void ObserveVideoPlaybackTick(int frame) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto const timestamp_ns = NowNs();
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::Video))
		return;

	double interval_ms = 0.0;
	if (!session.summary.video_playback_tick_interval.Observe(timestamp_ns, kVideoPlaybackTargetMs, interval_ms))
		return;

	JsonObjectBuilder payload;
	payload.AddInt("frame", frame);
	payload.AddDouble("delta_ms", interval_ms);
	AppendEntryLocked(session, "metric", "video_playback_tick_interval", payload.Finish(), false, timestamp_ns);
}

void TraceWindowOpenBegin(char const* window_kind) {
	RecordStartupTimingEntry(window_kind, "op", "window_open_begin", [&](JsonObjectBuilder&) {
	});

	RecordEntry(TraceCategory::UiWindow, "op", "window_open_begin", true, [&](JsonObjectBuilder& payload) {
		payload.AddString("window_kind", window_kind ? window_kind : "");
	});
}

void ObserveWindowOpenPhase(char const* window_kind, char const* phase, double duration_ms) {
	RecordStartupTimingEntry(window_kind, "metric", "window_open_phase_duration", [&](JsonObjectBuilder& payload) {
		payload.AddString("phase", phase ? phase : "");
		payload.AddDouble("duration_ms", duration_ms);
	});

	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto const timestamp_ns = NowNs();
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::UiWindow))
		return;

	JsonObjectBuilder payload;
	payload.AddString("window_kind", window_kind ? window_kind : "");
	payload.AddString("phase", phase ? phase : "");
	payload.AddDouble("duration_ms", duration_ms);
	AppendEntryLocked(session, "metric", "window_open_phase_duration", payload.Finish(), false, timestamp_ns);

	auto const summary_key = MakeWindowPhaseSummaryKey(window_kind, phase);
	auto [it, inserted] = session.summary.window_phase_durations.emplace(summary_key, DurationSummary{});
	if (inserted)
		session.summary.window_phase_order.emplace_back(summary_key);
	if (duration_ms >= 0.0)
		it->second.Observe(duration_ms);
}

void TraceWindowOpenEnd(char const* window_kind, double duration_ms, bool succeeded) {
	RecordStartupTimingEntry(window_kind, "op", "window_open_end", [&](JsonObjectBuilder& payload) {
		payload.AddDouble("duration_ms", duration_ms);
		payload.AddBool("succeeded", succeeded);
	}, true);
	RecordStartupTimingEntry(window_kind, "metric", "window_open_duration", [&](JsonObjectBuilder& payload) {
		payload.AddDouble("duration_ms", duration_ms);
		payload.AddBool("succeeded", succeeded);
	}, true);

	RecordEntry(TraceCategory::UiWindow, "op", "window_open_end", true, [&](JsonObjectBuilder& payload) {
		payload.AddString("window_kind", window_kind ? window_kind : "");
		payload.AddDouble("duration_ms", duration_ms);
		payload.AddBool("succeeded", succeeded);
	});

	RecordEntry(TraceCategory::UiWindow, "metric", "window_open_duration", false, [&](JsonObjectBuilder& payload) {
		payload.AddString("window_kind", window_kind ? window_kind : "");
		payload.AddDouble("duration_ms", duration_ms);
		payload.AddBool("succeeded", succeeded);
	});

	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::UiWindow))
		return;

	if (succeeded)
		++session.summary.window_open_success;
	else
		++session.summary.window_open_failure;
	if (duration_ms >= 0.0)
		session.summary.window_open_duration.Observe(duration_ms);
}

void TraceLuaDialogOpenBegin() {
	auto const timestamp_ns = NowNs();
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::LuaDialog))
		return;

	session.has_pending_lua_dialog_open = true;
	session.pending_lua_dialog_open_started_ns = timestamp_ns;
	++session.summary.op_counts["lua_dialog_open_begin"];

	JsonObjectBuilder payload;
	AppendEntryLocked(session, "op", "lua_dialog_open_begin", payload.Finish(), true, timestamp_ns);
}

void ObserveLuaDialogPhase(char const* phase, int control_count, int button_count, double duration_ms) {
	RecordEntry(TraceCategory::LuaDialog, "metric", "lua_dialog_phase_duration", false, [&](JsonObjectBuilder& payload) {
		payload.AddString("phase", phase ? phase : "");
		payload.AddInt("control_count", control_count);
		payload.AddInt("button_count", button_count);
		payload.AddDouble("duration_ms", duration_ms);
	});
}

void ObserveLuaDialogControlTypeSummary(char const* control_type, int control_count, int button_count, int instance_count, int item_count_total, int item_count_max, double duration_ms) {
	RecordEntry(TraceCategory::LuaDialog, "metric", "lua_dialog_control_type_duration", false, [&](JsonObjectBuilder& payload) {
		payload.AddString("phase", "create_controls");
		payload.AddString("control_type", control_type ? control_type : "");
		payload.AddInt("control_count", control_count);
		payload.AddInt("button_count", button_count);
		payload.AddInt("instance_count", instance_count);
		payload.AddInt("item_count_total", item_count_total);
		payload.AddInt("item_count_max", item_count_max);
		payload.AddDouble("duration_ms", duration_ms);
	});
}

void ObserveLuaDialogControlStepSummary(char const* control_type, char const* step, int control_count, int button_count, int instance_count, int item_count_total, int item_count_max, double duration_ms) {
	RecordEntry(TraceCategory::LuaDialog, "metric", "lua_dialog_control_step_duration", false, [&](JsonObjectBuilder& payload) {
		payload.AddString("phase", "create_controls");
		payload.AddString("control_type", control_type ? control_type : "");
		payload.AddString("step", step ? step : "");
		payload.AddInt("control_count", control_count);
		payload.AddInt("button_count", button_count);
		payload.AddInt("instance_count", instance_count);
		payload.AddInt("item_count_total", item_count_total);
		payload.AddInt("item_count_max", item_count_max);
		payload.AddDouble("duration_ms", duration_ms);
	});
}

void TraceLuaDialogOpenEnd(int control_count, int button_count, double duration_ms, bool succeeded) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto const timestamp_ns = NowNs();
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing || !IsCategoryEnabledLocked(session, TraceCategory::LuaDialog))
		return;

	if (!session.has_pending_lua_dialog_open && duration_ms < 0.0)
		return;

	if (duration_ms < 0.0 && session.has_pending_lua_dialog_open)
		duration_ms = static_cast<double>(timestamp_ns - session.pending_lua_dialog_open_started_ns) / 1000000.0;

	session.has_pending_lua_dialog_open = false;
	session.pending_lua_dialog_open_started_ns = 0;

	if (succeeded)
		++session.summary.lua_dialog_success;
	else
		++session.summary.lua_dialog_failure;
	session.summary.lua_dialog_duration.Observe(duration_ms);
	++session.summary.op_counts["lua_dialog_open_end"];

	JsonObjectBuilder op_payload;
	op_payload.AddInt("control_count", control_count);
	op_payload.AddInt("button_count", button_count);
	op_payload.AddDouble("duration_ms", duration_ms);
	op_payload.AddBool("succeeded", succeeded);
	AppendEntryLocked(session, "op", "lua_dialog_open_end", op_payload.Finish(), true, timestamp_ns);

	JsonObjectBuilder metric_payload;
	metric_payload.AddInt("control_count", control_count);
	metric_payload.AddInt("button_count", button_count);
	metric_payload.AddDouble("duration_ms", duration_ms);
	metric_payload.AddBool("succeeded", succeeded);
	AppendEntryLocked(session, "metric", "lua_dialog_open_duration", metric_payload.Finish(), false, timestamp_ns);
}

void ObserveVideoMemorySnapshot(char const* reason, VideoMemorySnapshot const& snapshot_in, bool force) {
	if (!trace_active.load(std::memory_order_relaxed))
		return;

	auto snapshot = snapshot_in;
	auto const timestamp_ns = NowNs();
	auto& session = GetSession();
	std::lock_guard<std::mutex> lock(session.mutex);
	if (!session.enabled || session.closing
		|| !IsCategoryEnabledLocked(session, TraceCategory::Memory | TraceCategory::Audio | TraceCategory::Video))
		return;

	auto const sample_interval_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(kVideoMemorySampleInterval).count();
	if (!force && timestamp_ns - session.last_video_memory_sample_ns < sample_interval_ns)
		return;
	session.last_video_memory_sample_ns = timestamp_ns;

#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS_EX counters = { };
	if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
		snapshot.process_working_set_bytes = static_cast<size_t>(counters.WorkingSetSize);
		snapshot.process_private_bytes = static_cast<size_t>(counters.PrivateUsage);
	}
#endif

	++session.summary.video_memory_samples;
	session.summary.process_working_set_max_bytes = std::max(session.summary.process_working_set_max_bytes, snapshot.process_working_set_bytes);
	session.summary.process_private_max_bytes = std::max(session.summary.process_private_max_bytes, snapshot.process_private_bytes);
	session.summary.provider_cache_bgra_max_bytes = std::max(session.summary.provider_cache_bgra_max_bytes, snapshot.async.provider.cache_bgra_bytes);
	session.summary.provider_cache_native_max_bytes = std::max(session.summary.provider_cache_native_max_bytes, snapshot.async.provider.cache_native_bytes);
	session.summary.async_source_pool_max_bytes = std::max(session.summary.async_source_pool_max_bytes, snapshot.async.source_pool_bytes);
	session.summary.async_composited_pool_max_bytes = std::max(session.summary.async_composited_pool_max_bytes, snapshot.async.composited_pool_bytes);
	session.summary.async_overlay_pool_max_bytes = std::max(session.summary.async_overlay_pool_max_bytes, snapshot.async.subtitle_overlay_pool_bytes);
	session.summary.display_pending_packet_ref_max_bytes = std::max(session.summary.display_pending_packet_ref_max_bytes, snapshot.display.pending_packet_ref_bytes);
	session.summary.display_displayed_packet_ref_max_bytes = std::max(session.summary.display_displayed_packet_ref_max_bytes, snapshot.display.displayed_packet_ref_bytes);
	session.summary.renderer_primary_texture_max_bytes = std::max(session.summary.renderer_primary_texture_max_bytes, snapshot.display.primary_renderer_texture_bytes);
	session.summary.renderer_secondary_texture_max_bytes = std::max(session.summary.renderer_secondary_texture_max_bytes, snapshot.display.secondary_renderer_texture_bytes);
	session.summary.display_scene_cache_texture_max_bytes = std::max(session.summary.display_scene_cache_texture_max_bytes, snapshot.display.scene_cache_texture_bytes);
	session.summary.audio_storage_max_bytes = std::max(session.summary.audio_storage_max_bytes, snapshot.audio.storage_bytes);
	session.summary.audio_logical_max_bytes = std::max(session.summary.audio_logical_max_bytes, snapshot.audio.logical_bytes);
	session.summary.audio_decoded_max_bytes = std::max(session.summary.audio_decoded_max_bytes, snapshot.audio.decoded_bytes);
	session.summary.audio_page_size_bytes = std::max(session.summary.audio_page_size_bytes, snapshot.audio.page_size_bytes);
	session.summary.audio_loading_max_bytes = std::max(session.summary.audio_loading_max_bytes, snapshot.audio.loading_bytes);
	session.summary.audio_pinned_max_bytes = std::max(session.summary.audio_pinned_max_bytes, snapshot.audio.pinned_bytes);
	session.summary.audio_free_max_bytes = std::max(session.summary.audio_free_max_bytes, snapshot.audio.free_bytes);
	session.summary.audio_resident_pages_max = std::max(session.summary.audio_resident_pages_max, snapshot.audio.resident_pages);
	session.summary.audio_loading_pages_max = std::max(session.summary.audio_loading_pages_max, snapshot.audio.loading_pages);
	session.summary.audio_pinned_pages_max = std::max(session.summary.audio_pinned_pages_max, snapshot.audio.pinned_pages);
	session.summary.audio_free_pages_max = std::max(session.summary.audio_free_pages_max, snapshot.audio.free_pages);
	if (!snapshot.audio.provider_name.empty())
		session.summary.audio_provider_name = snapshot.audio.provider_name;
	if (!snapshot.audio.storage_kind.empty())
		session.summary.audio_storage_kind = snapshot.audio.storage_kind;

	JsonObjectBuilder payload;
	payload.AddString("reason", reason ? reason : "video_memory");
	payload.AddInt("process_working_set_bytes", static_cast<int64_t>(snapshot.process_working_set_bytes));
	payload.AddInt("process_private_bytes", static_cast<int64_t>(snapshot.process_private_bytes));
	payload.AddInt("provider_cache_total_bytes", static_cast<int64_t>(snapshot.async.provider.cache_total_bytes));
	payload.AddInt("provider_cache_bgra_bytes", static_cast<int64_t>(snapshot.async.provider.cache_bgra_bytes));
	payload.AddInt("provider_cache_native_bytes", static_cast<int64_t>(snapshot.async.provider.cache_native_bytes));
	payload.AddInt("provider_cache_bgra_frames", snapshot.async.provider.cache_bgra_frames);
	payload.AddInt("provider_cache_native_frames", snapshot.async.provider.cache_native_frames);
	payload.AddInt("async_source_pool_bytes", static_cast<int64_t>(snapshot.async.source_pool_bytes));
	payload.AddInt("async_source_pool_buffers", snapshot.async.source_pool_buffers);
	payload.AddInt("async_composited_pool_bytes", static_cast<int64_t>(snapshot.async.composited_pool_bytes));
	payload.AddInt("async_composited_pool_buffers", snapshot.async.composited_pool_buffers);
	payload.AddInt("async_subtitle_overlay_pool_bytes", static_cast<int64_t>(snapshot.async.subtitle_overlay_pool_bytes));
	payload.AddInt("async_subtitle_overlay_pool_buffers", snapshot.async.subtitle_overlay_pool_buffers);
	payload.AddString("source_mode", SourceFrameOutputModeName(snapshot.async.selected_source_mode));
	payload.AddString("decoder", snapshot.async.decoder_name);
	payload.AddString("subtitles_provider", snapshot.async.subtitles_provider_name);
	payload.AddString("subtitles_render_mode", snapshot.async.subtitles_render_mode);
	payload.AddBool("compatibility_requires_bgra8", snapshot.async.compatibility_requires_bgra8);
	payload.AddBool("subtitles_loaded", snapshot.async.subtitles_loaded);
	payload.AddBool("pending_subtitles_update", snapshot.async.pending_subtitles_update);
	payload.AddInt("subtitles_event_count", snapshot.async.subtitles_event_count);
	payload.AddInt("display_pending_packet_ref_bytes", static_cast<int64_t>(snapshot.display.pending_packet_ref_bytes));
	payload.AddInt("display_displayed_packet_ref_bytes", static_cast<int64_t>(snapshot.display.displayed_packet_ref_bytes));
	payload.AddInt("renderer_primary_texture_bytes", static_cast<int64_t>(snapshot.display.primary_renderer_texture_bytes));
	payload.AddString("renderer_primary", snapshot.display.primary_renderer_name);
	payload.AddInt("renderer_secondary_texture_bytes", static_cast<int64_t>(snapshot.display.secondary_renderer_texture_bytes));
	payload.AddString("renderer_secondary", snapshot.display.secondary_renderer_name);
	payload.AddInt("display_scene_cache_texture_bytes", static_cast<int64_t>(snapshot.display.scene_cache_texture_bytes));
	payload.AddString("audio_provider", snapshot.audio.provider_name);
	payload.AddString("audio_storage_kind", snapshot.audio.storage_kind);
	payload.AddInt("audio_storage_bytes", static_cast<int64_t>(snapshot.audio.storage_bytes));
	payload.AddInt("audio_logical_bytes", static_cast<int64_t>(snapshot.audio.logical_bytes));
	payload.AddInt("audio_decoded_bytes", static_cast<int64_t>(snapshot.audio.decoded_bytes));
	payload.AddInt("audio_page_size_bytes", static_cast<int64_t>(snapshot.audio.page_size_bytes));
	payload.AddInt("audio_loading_bytes", static_cast<int64_t>(snapshot.audio.loading_bytes));
	payload.AddInt("audio_pinned_bytes", static_cast<int64_t>(snapshot.audio.pinned_bytes));
	payload.AddInt("audio_free_bytes", static_cast<int64_t>(snapshot.audio.free_bytes));
	payload.AddInt("audio_num_samples", snapshot.audio.num_samples);
	payload.AddInt("audio_decoded_samples", snapshot.audio.decoded_samples);
	payload.AddInt("audio_resident_pages", snapshot.audio.resident_pages);
	payload.AddInt("audio_loading_pages", snapshot.audio.loading_pages);
	payload.AddInt("audio_pinned_pages", snapshot.audio.pinned_pages);
	payload.AddInt("audio_free_pages", snapshot.audio.free_pages);
	payload.AddInt("audio_sample_rate", snapshot.audio.sample_rate);
	payload.AddInt("audio_bytes_per_sample", snapshot.audio.bytes_per_sample);
	payload.AddInt("audio_channels", snapshot.audio.channels);
	payload.AddBool("audio_float_samples", snapshot.audio.float_samples);
	AppendEntryLocked(session, "metric", "video_memory_snapshot", payload.Finish(), force, timestamp_ns);
}

} // namespace perf_trace
