#include "ass_file.h"
#include "ass_dialogue.h"
#include "ass_file_app.h"
#include "audio_provider_factory.h"
#include "export_framerate_transform.h"
#include "options.h"
#include "presentation/subtitle_grid_query_service.h"
#include "provider_index_cache.h"
#include "include/aegisub/subtitles_provider.h"
#include "include/aegisub/video_provider.h"
#include "subtitle_format.h"
#include "ui_services.h"
#include "video_session_core_ops.h"
#include "video_provider_manager.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/audio/provider.h>
#include <libaegisub/fs.h>
#include <libaegisub/option.h>
#include <libaegisub/path.h>
#include <libaegisub/vfr.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

namespace {

class ScopedFile {
	std::filesystem::path path;

public:
	explicit ScopedFile(std::filesystem::path path) : path(std::move(path)) { }
	~ScopedFile() {
		std::error_code ec;
		std::filesystem::remove(path, ec);
	}

	std::filesystem::path const& get() const { return path; }
};

std::filesystem::path MakeTempAssPath() {
	auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	return std::filesystem::temp_directory_path() / ("aegisub_core_smoke_" + std::to_string(stamp) + ".ass");
}

std::filesystem::path MakeTempTxtPath() {
	auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	return std::filesystem::temp_directory_path() / ("aegisub_core_smoke_" + std::to_string(stamp) + ".txt");
}

constexpr char kCoreSmokeOptionDefaults[] = R"({
	"Audio" : {
		"Cache" : {
			"HD" : {
				"Location" : "default"
			},
			"Type" : 0
		},
		"Provider" : "Dummy"
	},
	"Subtitle" : {
		"Default Resolution" : {
			"Auto" : true,
			"Width" : 640,
			"Height" : 480
		},
		"Provider" : "libass"
	},
	"Subtitle Format" : {
		"EBU STL" : {
			"Display Standard" : 0,
			"Inclusive End Times" : true,
			"Line Wrapping Mode" : 1,
			"Max Line Length" : 42,
			"TV Standard" : 0,
			"Text Encoding" : 0,
			"Timecode Offset" : {
				"H" : 0,
				"M" : 0,
				"S" : 0,
				"F" : 0
			},
			"Translate Alignments" : true
		},
		"TXT" : {
			"Default Style Catalog" : ""
		}
	},
	"Tool" : {
		"Import" : {
			"Text" : {
				"Actor Separator" : ":",
				"Comment Starter" : "#",
				"Include Blank" : false
			}
		}
	},
	"Timing" : {
		"Default Duration" : 2000
	},
	"Video" : {
		"Provider" : "Dummy"
	}
})";

constexpr char kCoreSmokePartialOptionDefaults[] = R"({
	"Subtitle" : {
	}
})";

class ScopedCoreSmokeOptions {
	agi::Options options;
	agi::Options *previous = nullptr;

public:
	ScopedCoreSmokeOptions()
	: options("", kCoreSmokeOptionDefaults, agi::Options::FLUSH_SKIP)
	, previous(config::opt) {
		config::opt = &options;
	}

	~ScopedCoreSmokeOptions() {
		config::opt = previous;
	}
};

class ScopedCoreSmokePartialOptions {
	agi::Options options;
	agi::Options *previous = nullptr;

public:
	ScopedCoreSmokePartialOptions()
	: options("", kCoreSmokePartialOptionDefaults, agi::Options::FLUSH_SKIP)
	, previous(config::opt) {
		config::opt = &options;
	}

	~ScopedCoreSmokePartialOptions() {
		config::opt = previous;
	}
};

class ScopedNullCoreSmokeOptions {
	agi::Options *previous = nullptr;

public:
	ScopedNullCoreSmokeOptions()
	: previous(config::opt) {
		config::opt = nullptr;
	}

	~ScopedNullCoreSmokeOptions() {
		config::opt = previous;
	}
};

class ScopedNullCoreSmokeRuntime {
	agi::Options *previous_options = nullptr;
	agi::Path *previous_path = nullptr;

public:
	ScopedNullCoreSmokeRuntime()
	: previous_options(config::opt)
	, previous_path(config::path) {
		config::opt = nullptr;
		config::path = nullptr;
	}

	~ScopedNullCoreSmokeRuntime() {
		config::opt = previous_options;
		config::path = previous_path;
	}
};

class ScopedUnresolvedCoreSmokeLocalPath {
	agi::Path path;
	agi::Path *previous_path = nullptr;

public:
	ScopedUnresolvedCoreSmokeLocalPath()
	: previous_path(config::path) {
		path.SetToken("?local", "");
		config::path = &path;
	}

	~ScopedUnresolvedCoreSmokeLocalPath() {
		config::path = previous_path;
	}
};

class ScopedCoreSmokeDispatch {
	std::thread::id main_thread_id = std::this_thread::get_id();

public:
	ScopedCoreSmokeDispatch() {
		agi::dispatch::Init(
			[](agi::dispatch::Thunk thunk) {
				if (thunk)
					thunk();
			},
			[this] {
				return std::this_thread::get_id() == main_thread_id;
			});
	}

	~ScopedCoreSmokeDispatch() {
		agi::dispatch::Shutdown();
	}
};

void WriteSmokeAss(std::filesystem::path const& path) {
	std::ofstream file(path, std::ios::binary);
	if (!file)
		throw std::runtime_error("failed to create smoke ASS file");

	file <<
		"[Script Info]\n"
		"ScriptType: v4.00+\n"
		"\n"
		"[V4+ Styles]\n"
		"Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
		"Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, "
		"Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
		"Style: Default,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H80000000,"
		"0,0,0,0,100,100,0,0,1,2,2,2,10,10,10,1\n"
		"\n"
		"[Events]\n"
		"Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
		"Dialogue: 0,0:00:01.00,0:00:02.50,Default,Actor,0000,0000,0000,,Hello core\n"
		"Comment: 1,0:00:03.00,0:00:04.00,Default,,0000,0000,0000,fx,Hidden note\n";
}

void WriteSmokeTxt(std::filesystem::path const& path) {
	std::ofstream file(path, std::ios::binary);
	if (!file)
		throw std::runtime_error("failed to create smoke TXT file");

	file <<
		"Alice: Hello from TXT\n"
		"# Internal note\n"
		"\n"
		"Bob: Second line\n";
}

size_t RunEbuTimecodeSmoke(AssFile const& source, std::filesystem::path const& artifact_directory) {
	ScopedCoreSmokeOptions options;
	AssFile file(source);
	auto& first = file.Events.front();
	first.Start = 1000;
	first.End = 2400;
	first.Text = "{\\i1}" + std::string(36, 'A') + "\\N" + std::string(36, 'B') + "\\N" + std::string(36, 'C') + "\\N" + std::string(36, 'D');
	for (auto const& times : std::array<std::array<int, 2>, 2>{{{5000, 6200}, {12000, 13600}}}) {
		auto *line = new AssDialogue(first);
		line->Start = times[0];
		line->End = times[1];
		line->Text = times[0] == 5000 ? "Second cue after rollover" : "Third cue with a distinct interval";
		file.Events.push_back(*line);
	}

	using Timecode = std::array<unsigned char, 4>;
	struct Scenario {
		char const *name;
		int standard;
		Timecode offset;
		char const *tcp;
		char const *tcf;
		char const *dfc;
		std::array<Timecode, 3> in;
		std::array<Timecode, 3> inclusive_out;
		std::array<Timecode, 3> exclusive_out;
	};
	std::array<Scenario, 2> const scenarios{{{.name = "pal25",
											  .standard = 2,
											  .offset = {1, 2, 3, 17},
											  .tcp = "01020317",
											  .tcf = "01020417",
											  .dfc = "STL25.01",
											  .in = {{{1, 2, 4, 17}, {1, 2, 8, 17}, {1, 2, 15, 17}}},
											  .inclusive_out = {{{1, 2, 6, 1}, {1, 2, 9, 21}, {1, 2, 17, 6}}},
											  .exclusive_out = {{{1, 2, 6, 2}, {1, 2, 9, 22}, {1, 2, 17, 7}}}},
											 {.name = "ntsc-drop",
											  .standard = 4,
											  .offset = {0, 9, 59, 17},
											  .tcp = "00095917",
											  .tcf = "00100016",
											  .dfc = "STL30.01",
											  .in = {{{0, 10, 0, 16}, {0, 10, 4, 16}, {0, 10, 11, 16}}},
											  .inclusive_out = {{{0, 10, 1, 27}, {0, 10, 5, 21}, {0, 10, 13, 3}}},
											  .exclusive_out = {{{0, 10, 1, 28}, {0, 10, 5, 22}, {0, 10, 13, 4}}}}}};
	std::filesystem::create_directories(artifact_directory);
	std::ofstream report(artifact_directory / "fields.txt", std::ios::trunc);
	if (!report)
		throw std::runtime_error("failed to create EBU timecode field report");
	auto write_timecode = [&](Timecode const& tc) {
		for (size_t i = 0; i < tc.size(); ++i)
			report << (i ? ":" : "") << static_cast<unsigned int>(tc[i]);
	};
	size_t total_bytes = 0;
	for (auto const& scenario : scenarios) {
		OPT_SET("Subtitle Format/EBU STL/TV Standard")->SetInt(scenario.standard);
		std::array<char const *, 4> const fields{"H", "M", "S", "F"};
		for (size_t i = 0; i < fields.size(); ++i)
			OPT_SET(std::string("Subtitle Format/EBU STL/Timecode Offset/") + fields[i])->SetInt(scenario.offset[i]);
		for (bool inclusive : {true, false}) {
			OPT_SET("Subtitle Format/EBU STL/Inclusive End Times")->SetBool(inclusive);
			auto const name = std::string(scenario.name) + (inclusive ? "-inclusive" : "-exclusive");
			auto const path = artifact_directory / (name + ".stl");
			auto const *writer = SubtitleFormat::GetWriter(path);
			if (!writer)
				throw std::runtime_error("EBU STL writer was not registered");
			writer->WriteFile(&file, path, agi::vfr::Framerate(), "");
			std::ifstream input(path, std::ios::binary);
			std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
			if (!input || bytes.size() != 1024 + 4 * 128)
				throw std::runtime_error(name + ": unexpected STL size or extension block count");
			total_bytes += bytes.size();
			auto const tcp = bytes.substr(256, 8);
			auto const tcf = bytes.substr(264, 8);
			auto const dfc = bytes.substr(3, 8);
			report << name << " bytes=" << bytes.size() << " TCP=" << tcp << " expected=" << scenario.tcp
				   << " TCF=" << tcf << " expected=" << scenario.tcf << " DFC=" << dfc << " expected=" << scenario.dfc << '\n';
			bool matches = tcp == scenario.tcp && tcf == scenario.tcf && dfc == scenario.dfc;
			std::array<size_t, 4> const cue_numbers{0, 0, 1, 2};
			std::array<unsigned char, 4> const extension_numbers{0, 255, 255, 255};
			for (size_t block = 0; block < cue_numbers.size(); ++block) {
				auto const pos = 1024 + block * 128;
				auto const cue = cue_numbers[block];
				Timecode in{}, out{};
				for (size_t i = 0; i < 4; ++i) {
					in[i] = static_cast<unsigned char>(bytes[pos + 5 + i]);
					out[i] = static_cast<unsigned char>(bytes[pos + 9 + i]);
				}
				auto const& expected_out = inclusive ? scenario.inclusive_out[cue] : scenario.exclusive_out[cue];
				report << "  block=" << block << " cue=" << cue
					   << " SN=" << (static_cast<unsigned char>(bytes[pos + 1]) + 256 * static_cast<unsigned char>(bytes[pos + 2]))
					   << " EBN=" << static_cast<unsigned int>(static_cast<unsigned char>(bytes[pos + 3])) << " TCI=";
				write_timecode(in);
				report << " expected=";
				write_timecode(scenario.in[cue]);
				report << " TCO=";
				write_timecode(out);
				report << " expected=";
				write_timecode(expected_out);
				report << '\n';
				matches = matches && in == scenario.in[cue] && out == expected_out && static_cast<unsigned char>(bytes[pos + 1]) == cue && bytes[pos + 2] == 0 && static_cast<unsigned char>(bytes[pos + 3]) == extension_numbers[block];
			}
			report.flush();
			if (!report)
				throw std::runtime_error("failed to write EBU timecode field report");
			if (!matches)
				throw std::runtime_error(name + ": STL fields differ from independent timecode expectations");
		}
	}
	return total_bytes;
}

int RunSmoke(std::filesystem::path const& artifact_directory) {
	ScopedCoreSmokeDispatch dispatch;
	ScopedCoreSmokeOptions options;

	ScopedFile ass_path(MakeTempAssPath());
	WriteSmokeAss(ass_path.get());

	AssFile file;
	auto const* reader = SubtitleFormat::GetReader(ass_path.get(), "utf-8");
	if (!reader)
		throw std::runtime_error("ASS reader was not registered");
	reader->ReadFile(&file, ass_path.get(), agi::vfr::Framerate(), "utf-8", {});

	aegisub::presentation::VisibleSubtitleRowsRequest request;
	request.first_row = 0;
	request.row_count = 10;

	auto window = aegisub::presentation::QueryVisibleSubtitleRows(file, request, 1);
	if (window.total_rows != 2 || window.rows.size() != 2)
		throw std::runtime_error("unexpected projected subtitle row count");
	if (window.rows[0].text != "Hello core")
		throw std::runtime_error("unexpected projected subtitle text");
	if (!window.rows[1].comment)
		throw std::runtime_error("comment dialogue did not project as comment row");

	auto subtitle_provider_catalog = SubtitlesProviderFactory::GetCatalog("libass");
	auto subtitle_provider_names = aegisub::provider_catalog::VisibleProviderNames(subtitle_provider_catalog);
	if (std::find(subtitle_provider_names.begin(), subtitle_provider_names.end(), "libass") == subtitle_provider_names.end())
		throw std::runtime_error("libass subtitles provider was not present in the core provider catalog");

	SubtitleRenderEnvironment render_environment;
	render_environment.preferred_provider = "libass";
	auto subtitles_provider = SubtitlesProviderFactory::GetProvider(render_environment);
	if (!subtitles_provider || subtitles_provider->GetDebugName() != "libass")
		throw std::runtime_error("core subtitles provider factory did not create libass");
	subtitles_provider->LoadSubtitles(&file, -1, nullptr);

	{
		ScopedNullCoreSmokeOptions null_options;
		SubtitleRenderEnvironment default_render_environment;
		auto default_subtitles_provider = SubtitlesProviderFactory::GetProvider(default_render_environment);
		if (!default_subtitles_provider || default_subtitles_provider->GetDebugName() != "libass")
			throw std::runtime_error("core subtitles provider factory did not default to libass without global options");
	}

	auto audio_provider_catalog = GetAudioProviderCatalog("Dummy");
	auto audio_dummy = std::find_if(audio_provider_catalog.providers.begin(), audio_provider_catalog.providers.end(), [](auto const& provider) {
		return provider.name == "Dummy" && provider.hidden && provider.available;
	});
	if (audio_dummy == audio_provider_catalog.providers.end())
		throw std::runtime_error("Dummy audio provider was not present in the core provider catalog");

	agi::Path path_helper;
	agi::NullNotificationSink notification_sink;
	auto choice_sink = std::make_shared<agi::NullSingleChoiceInteractionSink>();
	auto audio_provider = GetAudioProvider("dummy-audio:", path_helper, nullptr, notification_sink, choice_sink);
	if (!audio_provider || audio_provider->GetSampleRate() != 44100 || audio_provider->GetChannels() != 1)
		throw std::runtime_error("core audio provider manager did not create Dummy audio");

	auto video_provider_catalog = VideoProviderFactory::GetCatalog("Dummy");
	auto video_dummy = std::find_if(video_provider_catalog.providers.begin(), video_provider_catalog.providers.end(), [](auto const& provider) {
		return provider.name == "Dummy" && provider.hidden && provider.available;
	});
	if (video_dummy == video_provider_catalog.providers.end())
		throw std::runtime_error("Dummy video provider was not present in the core provider catalog");

	auto video_provider = VideoProviderFactory::GetProvider("?dummy:24:2:16:8:10:20:30:", "", nullptr, choice_sink);
	if (!video_provider || video_provider->GetDecoderName() != "Dummy Video Provider" || video_provider->GetFrameCount() != 2 || video_provider->GetWidth() != 16 || video_provider->GetHeight() != 8)
		throw std::runtime_error("core video provider manager did not create Dummy video");

	{
		ScopedNullCoreSmokeOptions null_options;
		auto audio_without_options = GetAudioProvider("dummy-audio:", path_helper, nullptr, notification_sink, choice_sink);
		if (!audio_without_options || audio_without_options->GetSampleRate() != 44100 || audio_without_options->GetChannels() != 1)
			throw std::runtime_error("core audio provider manager did not create Dummy audio without global options");

		auto video_without_options = VideoProviderFactory::GetProvider("?dummy:24:2:16:8:10:20:30:", "", nullptr, choice_sink);
		if (!video_without_options || video_without_options->GetDecoderName() != "Dummy Video Provider" || video_without_options->GetFrameCount() != 2)
			throw std::runtime_error("core video provider manager did not create Dummy video without global options");
	}

	{
		ScopedNullCoreSmokeRuntime null_runtime;
		auto cache_path = aegisub::provider_index_cache::BuildFilename(ass_path.get(), "?local/core-smoke-cache/", ".idx");
		if (cache_path.empty() || !std::filesystem::exists(cache_path.parent_path()))
			throw std::runtime_error("core provider index cache did not create a fallback cache directory without app runtime");
		aegisub::provider_index_cache::Clean(
			"?local/core-smoke-cache/",
			"*.idx",
			"Provider/FFmpegSource/Cache/Size",
			"Provider/FFmpegSource/Cache/Files");
	}

	{
		ScopedUnresolvedCoreSmokeLocalPath unresolved_local_path;
		auto cache_path = aegisub::provider_index_cache::BuildFilename(ass_path.get(), "?local/core-smoke-cache/", ".idx");
		auto cache_path_text = agi::fs::PathToString(cache_path);
		if (cache_path_text.empty() || cache_path_text[0] == '?' || !std::filesystem::exists(cache_path.parent_path()))
			throw std::runtime_error("core provider index cache did not fall back when the ?local token was unresolved");
	}

	aegisub::video_session_ops::OpenedVideoMetadata video_metadata;
	video_metadata.timecodes = video_provider->GetFPS();
	video_metadata.keyframes = video_provider->GetKeyFrames();
	video_metadata.warning = video_provider->GetWarning();
	video_metadata.has_audio = true;
	auto video_summary = aegisub::video_session_ops::BuildOpenedVideoSummary(
		video_metadata,
		agi::fs::PathFromString("movie.mkv"),
		[](agi::fs::path const&) { return true; });
	auto post_open_plan = aegisub::video_session_ops::PlanPostOpen(
		video_summary,
		true,
		agi::fs::PathFromString("audio.wav"),
		agi::fs::PathFromString("movie.mkv"));
	if (!video_summary.has_subtitles || !post_open_plan.auto_load_linked_audio)
		throw std::runtime_error("core video session policy did not build expected post-open plan");

	ScopedFile txt_path(MakeTempTxtPath());
	WriteSmokeTxt(txt_path.get());

	AssFile txt_file;
	auto const* txt_reader = SubtitleFormat::GetReader(txt_path.get(), "utf-8");
	if (!txt_reader)
		throw std::runtime_error("TXT reader was not registered");
	txt_reader->ReadFile(&txt_file, txt_path.get(), agi::vfr::Framerate(), "utf-8", {});

	auto txt_window = aegisub::presentation::QueryVisibleSubtitleRows(txt_file, request, 1);
	if (txt_window.total_rows != 3 || txt_window.rows.size() != 3)
		throw std::runtime_error("unexpected TXT projected subtitle row count");
	if (txt_window.rows[0].actor != "Alice" || txt_window.rows[0].text != "Hello from TXT")
		throw std::runtime_error("TXT reader did not parse actor/text fields");
	if (!txt_window.rows[1].comment || txt_window.rows[1].text != "Internal note")
		throw std::runtime_error("TXT reader did not parse comment prefix");

	{
		ScopedCoreSmokePartialOptions partial_options;
		AssFile partial_default_file;
		LoadDefaultAssFileWithAppOptions(partial_default_file, true, GetSubtitleFormatDefaultStyleCatalog("TXT"));
		if (partial_default_file.Events.empty())
			throw std::runtime_error("default ASS file did not load with partial global options");

		AssFile partial_txt_file;
		txt_reader->ReadFile(&partial_txt_file, txt_path.get(), agi::vfr::Framerate(), "utf-8", {});
		auto partial_txt_window = aegisub::presentation::QueryVisibleSubtitleRows(partial_txt_file, request, 1);
		if (partial_txt_window.total_rows != 3 || partial_txt_window.rows[0].actor != "Alice" || !partial_txt_window.rows[1].comment)
			throw std::runtime_error("TXT reader did not use import defaults with partial global options");

		auto partial_audio = GetAudioProvider("dummy-audio:", path_helper, nullptr, notification_sink, choice_sink);
		if (!partial_audio || partial_audio->GetSampleRate() != 44100)
			throw std::runtime_error("core audio provider manager did not create Dummy audio with partial global options");

		auto partial_video = VideoProviderFactory::GetProvider("?dummy:24:2:16:8:10:20:30:", "", nullptr, choice_sink);
		if (!partial_video || partial_video->GetDecoderName() != "Dummy Video Provider")
			throw std::runtime_error("core video provider manager did not create Dummy video with partial global options");
	}

	auto const stl_bytes = RunEbuTimecodeSmoke(file, artifact_directory);

	agi::vfr::Framerate source(24.0);
	agi::vfr::Framerate destination(25.0);
	auto const transform = BuildAssFramerateTransform(source, destination, 1000, 2500);
	if (transform.new_end_ms <= transform.new_start_ms)
		throw std::runtime_error("framerate transform produced an invalid interval");

	std::cout
		<< "aegisub_core_smoke: rows=" << window.total_rows
		<< " txt_rows=" << txt_window.total_rows
		<< " stl_bytes=" << stl_bytes
		<< " subtitle_provider=\"" << subtitles_provider->GetDebugName() << "\""
		<< " audio_rate=" << audio_provider->GetSampleRate()
		<< " video_frames=" << video_provider->GetFrameCount()
		<< " first_text=\"" << window.rows[0].text << "\""
		<< " transformed=[" << transform.new_start_ms << ", " << transform.new_end_ms << "]\n";
	return 0;
}

}

int main(int argc, char** argv) {
	try {
		return RunSmoke(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path("artifacts/ebu-timecode-smoke"));
	}
	catch (std::exception const& err) {
		std::cerr << "aegisub_core_smoke failed: " << err.what() << '\n';
		return 1;
	}
	catch (...) {
		std::cerr << "aegisub_core_smoke failed: unknown exception\n";
		return 1;
	}
}
