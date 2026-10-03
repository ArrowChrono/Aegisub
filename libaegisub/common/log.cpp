// Copyright (c) 2010, Amar Takhar <verm@aegisub.org>
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

#include "libaegisub/log.h"

#include "libaegisub/cajun/elements.h"
#include "libaegisub/cajun/writer.h"
#include "libaegisub/dispatch.h"
#include "libaegisub/fs.h"
#include "libaegisub/io.h"
#include "libaegisub/util.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string_view>

namespace agi { namespace log {
namespace {
constexpr size_t kBufferedLogEntryLimit = 64;
constexpr size_t kBufferedLogByteLimit = 64 * 1024;
constexpr int64_t kBufferedLogFlushIntervalNs = 250000000;
std::mutex current_log_file_mutex;
fs::path current_log_file_path;

void append_json_string(std::string& out, std::string_view value) {
	out.push_back('"');
	for (unsigned char ch : value) {
		switch (ch) {
			case '\\': out += "\\\\"; break;
			case '"': out += "\\\""; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (ch < 0x20) {
					char buffer[7];
					snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
					out += buffer;
				}
				else {
					out.push_back(static_cast<char>(ch));
				}
				break;
		}
	}
	out.push_back('"');
}

std::string make_ndjson_line(SinkMessage const& sm) {
	std::string line;
	line.reserve(sm.message.size() + 192);
	line += "{\"sec\":";
	line += std::to_string(sm.time / 1000000000);
	line += ",\"usec\":";
	line += std::to_string(sm.time % 1000000000);
	line += ",\"severity\":";
	line += std::to_string(sm.severity);
	line += ",\"section\":";
	append_json_string(line, sm.section ? sm.section : "");
#ifdef LOG_WITH_FILE
	line += ",\"file\":";
	append_json_string(line, sm.file ? sm.file : "");
#endif
	line += ",\"func\":";
	append_json_string(line, sm.func ? sm.func : "");
	line += ",\"line\":";
	line += std::to_string(sm.line);
	line += ",\"message\":";
	append_json_string(line, sm.message);
	line += "}\n";
	return line;
}
}

/// Global log sink.
LogSink *log;

/// Short Severity ID
/// Keep this ordered the same as Severity
const char *Severity_ID = "EAWID";

LogSink::LogSink() : queue(dispatch::Create()) { }

LogSink::~LogSink() {
	// The destructor for emitters may try to log messages, so disable all the
	// emitters before destructing any
	decltype(emitters) emitters_temp;
	queue->Sync([&]{ swap(emitters_temp, emitters); });
}

void LogSink::Log(SinkMessage const& sm) {
	auto deliver = [=] {
		if (messages.size() < 250)
			messages.push_back(sm);
		else {
			messages[next_idx] = sm;
			if (++next_idx == 250)
				next_idx = 0;
		}
		for (auto& em : emitters) em->log(sm);
	};

	// Keep non-debug logs crash-resilient: run them through the sink
	// synchronously so an imminent fail-fast still leaves the entry on disk.
	if (sm.severity <= Info)
		queue->Sync(deliver);
	else
		queue->Async(deliver);
}

void LogSink::Subscribe(std::unique_ptr<Emitter> em) {
	LOG_D("agi/log/emitter/subscribe") << "Subscribe: " << this;
	auto tmp = em.release();
	queue->Sync([=] { emitters.emplace_back(tmp); });
}

void LogSink::Unsubscribe(Emitter *em) {
		queue->Sync([=] {
			emitters.erase(
				std::remove_if(emitters.begin(), emitters.end(), [=](std::unique_ptr<Emitter> const& e) { return e.get() == em; }),
				emitters.end());
		});
	LOG_D("agi/log/emitter/unsubscribe") << "Un-Subscribe: " << this;
}

decltype(LogSink::messages) LogSink::GetMessages() const {
	decltype(messages) ret;
	queue->Sync([&] {
		ret.reserve(messages.size());
		ret.insert(ret.end(), messages.begin() + next_idx, messages.end());
		ret.insert(ret.end(), messages.begin(), messages.begin() + next_idx);
	});
	return ret;
}

#ifdef LOG_WITH_FILE
Message::Message(const char *section, Severity severity, const char *file, const char *func, int line)
#else
Message::Message(const char* section, Severity severity, const char* func, int line)
#endif
: msg(buffer, sizeof buffer)
{
	using namespace std::chrono;
	sm.section = section;
	sm.severity = severity;
#ifdef LOG_WITH_FILE
	sm.file = file;
#endif
	sm.func = func;
	sm.line = line;
	sm.time = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

Message::~Message() {
	// tellp() reports -1 after a fixed-buffer overflow; never turn that into
	// an enormous size_t from this noexcept destructor. The streambuf keeps
	// its actual write position even when the ostream failbit is set.
	auto written = static_cast<std::streamoff>(msg.rdbuf()->pubseekoff(
		0, std::ios_base::cur, std::ios_base::out));
	auto length = static_cast<std::string::size_type>(
		std::clamp<std::streamoff>(written, 0, sizeof buffer));
	sm.message.assign(buffer, length);
	if (agi::log::log)
		agi::log::log->Log(sm);
}

JsonEmitter::JsonEmitter(fs::path const& directory) {
	fs::CreateDirectory(directory);
	path = fs::UniquePath(directory / fs::PathFromString(util::strftime("%Y-%m-%d-%H-%M-%S-%%%%%%%%.ndjson")));
	auto out = std::make_unique<std::ofstream>();
	io::OpenFileStream(*out, path);
	if (!out || !out->good()) {
		path.clear();
		return;
	}
	fp = std::move(out);

	std::lock_guard<std::mutex> lock(current_log_file_mutex);
	current_log_file_path = path;
}

JsonEmitter::~JsonEmitter() {
	Flush();
	std::lock_guard<std::mutex> lock(current_log_file_mutex);
	if (current_log_file_path == path)
		current_log_file_path.clear();
}

void JsonEmitter::Flush() {
	if (!fp || buffer.empty())
		return;

	(*fp) << buffer;
	fp->flush();
	buffer.clear();
	buffered_count = 0;
}

void JsonEmitter::log(SinkMessage const& sm) {
	if (!fp)
		return;

	buffer += make_ndjson_line(sm);
	++buffered_count;

	bool const flush_immediately = sm.severity <= Info;
	if (last_flush_time == 0)
		last_flush_time = sm.time;

	if (flush_immediately
		|| buffered_count >= kBufferedLogEntryLimit
		|| buffer.size() >= kBufferedLogByteLimit
		|| sm.time - last_flush_time >= kBufferedLogFlushIntervalNs) {
		Flush();
		last_flush_time = sm.time;
	}
}

fs::path GetSessionLogFile() {
	std::lock_guard<std::mutex> lock(current_log_file_mutex);
	return current_log_file_path;
}

} }
