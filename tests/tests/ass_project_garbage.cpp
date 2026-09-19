#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_io_core.h"
#include "../../src/ass_parse_error.h"
#include "../../src/ass_parser.h"
#include "../../src/ass_style.h"

#include <libaegisub/fs.h>
#include <libaegisub/scope_exit.h>
#include <libaegisub/vfr.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include <gtest/gtest.h>

namespace {
ProjectProperties SampleProjectProperties() {
	return {
		.automation_scripts = "script.lua",
		.export_filters = "Transform Framerate",
		.export_encoding = "UTF-8",
		.style_storage = "Default",
		.audio_file = "media/audio.wav",
		.video_file = "media/video.mkv",
		.timecodes_file = "media/timecodes.txt",
		.keyframes_file = "media/keyframes.txt",
		.secondary_subtitles_file = "secondary.ass",
		.automation_settings = {{"filter", "enabled"}},
		.video_zoom = 0.75,
		.ar_value = 1.5,
		.scroll_position = 12,
		.active_row = 15,
		.ar_mode = 4,
		.video_position = 123};
}

void ExpectPersistedProjectProperties(ProjectProperties const& properties) {
	EXPECT_EQ("script.lua", properties.automation_scripts);
	EXPECT_EQ("Transform Framerate", properties.export_filters);
	EXPECT_EQ("UTF-8", properties.export_encoding);
	EXPECT_EQ("Default", properties.style_storage);
	EXPECT_EQ("media/audio.wav", properties.audio_file);
	EXPECT_EQ("media/video.mkv", properties.video_file);
	EXPECT_EQ("media/timecodes.txt", properties.timecodes_file);
	EXPECT_EQ("media/keyframes.txt", properties.keyframes_file);
	EXPECT_EQ("secondary.ass", properties.secondary_subtitles_file);
	EXPECT_DOUBLE_EQ(0.75, properties.video_zoom);
	EXPECT_DOUBLE_EQ(1.5, properties.ar_value);
	EXPECT_EQ(12, properties.scroll_position);
	EXPECT_EQ(15, properties.active_row);
	EXPECT_EQ(4, properties.ar_mode);
	EXPECT_EQ(123, properties.video_position);
}
}

TEST(ass_project_garbage, copies_and_assigns_project_properties) {
	AssFile original;
	original.Properties = SampleProjectProperties();
	AssFile copy(original);
	AssFile assigned;
	assigned.Properties.video_file = "previous.mkv";
	assigned.Properties.automation_settings["previous"] = "setting";
	assigned = original;

	original.Properties = {};
	ExpectPersistedProjectProperties(copy.Properties);
	ExpectPersistedProjectProperties(assigned.Properties);
	auto const expected_settings = SampleProjectProperties().automation_settings;
	EXPECT_EQ(expected_settings, copy.Properties.automation_settings);
	EXPECT_EQ(expected_settings, assigned.Properties.automation_settings);
}

class ass_project_garbage_snapshot : public testing::TestWithParam<bool> {};

TEST_P(ass_project_garbage_snapshot, save_round_trips_project_properties) {
	AssFile original;
	original.LoadDefault();
	original.Properties = SampleProjectProperties();
	original.Events.front().Text = "saved subtitle";
	if (GetParam()) {
		auto const kept_id = original.AddExtradata("kept", "value");
		original.Events.front().ExtradataIds = std::vector<uint32_t>{kept_id};
		original.AddExtradata("unused", "orphaned");
	}

	AssFile snapshot(original);
	snapshot.CleanExtradata();
	auto const path = agi::fs::UniquePath(
		std::filesystem::temp_directory_path() / "aegisub-project-garbage-%%%%%%%%.ass");
	auto cleanup = agi::make_scope_exit([&] {
		std::error_code error;
		std::filesystem::remove(path, error);
	});
	AssWriteOptions options;
	options.write_project_garbage = true;
	options.write_ui_state = true;
	options.write_extradata = true;
	WriteAssFileForCore(&snapshot, path, agi::vfr::Framerate(), "UTF-8", options);

	AssFile saved;
	ReadAssFileForCore(&saved, path, "UTF-8");
	ExpectPersistedProjectProperties(saved.Properties);
	ASSERT_EQ(1U, saved.Events.size());
	EXPECT_EQ("saved subtitle", saved.Events.front().Text.get());
	if (GetParam()) {
		ASSERT_EQ(1U, saved.Extradata.size());
		EXPECT_EQ("kept", saved.Extradata.front().key);
		EXPECT_EQ("value", saved.Extradata.front().value);
		EXPECT_EQ(std::vector<uint32_t>{saved.Extradata.front().id}, saved.Events.front().ExtradataIds.get());
		ASSERT_EQ(2U, original.Extradata.size());
		EXPECT_EQ("unused", original.Extradata.back().key);
		EXPECT_EQ("orphaned", original.Extradata.back().value);
	}
	else {
		EXPECT_TRUE(saved.Extradata.empty());
	}
	ExpectPersistedProjectProperties(original.Properties);
}

INSTANTIATE_TEST_SUITE_P(with_and_without_extradata, ass_project_garbage_snapshot, testing::Bool());

TEST(ass_project_garbage, export_snapshot_omits_project_garbage) {
	AssFile original;
	original.LoadDefault();
	original.Properties = SampleProjectProperties();
	original.Events.front().Text = "exported subtitle";
	AssFile snapshot(original);
	ASSERT_EQ("media/video.mkv", snapshot.Properties.video_file);
	auto const path = agi::fs::UniquePath(
		std::filesystem::temp_directory_path() / "aegisub-project-garbage-export-%%%%%%%%.ass");
	auto cleanup = agi::make_scope_exit([&] {
		std::error_code error;
		std::filesystem::remove(path, error);
	});
	WriteAssFileForCore(&snapshot, path, agi::vfr::Framerate(), "UTF-8", AssWriteOptions{});

	std::ifstream input(path, std::ios::binary);
	ASSERT_TRUE(input.is_open());
	std::string const text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	EXPECT_EQ(std::string::npos, text.find("[Aegisub Project Garbage]"));
	EXPECT_EQ(std::string::npos, text.find("media/video.mkv"));
	EXPECT_NE(std::string::npos, text.find("exported subtitle"));
}

TEST(ass_project_garbage, parses_secondary_subtitles_file) {
	AssFile file;
	AssParser parser(&file, 1);

	parser.AddLine("[Aegisub Project Garbage]");
	parser.AddLine("Secondary Subtitles File: ../secondary.ass");

	EXPECT_EQ("../secondary.ass", file.Properties.secondary_subtitles_file);
}

TEST(ass_project_garbage, replaces_embedded_nul_with_unicode_replacement_character) {
	AssFile file;
	AssParser parser(&file, 1);
	std::string line = "Secondary Subtitles File: secondary";
	line.push_back('\0');
	line += ".ass";

	parser.AddLine("[Aegisub Project Garbage]");
	parser.AddLine(line);

	EXPECT_EQ(std::string("secondary") + "\xEF\xBF\xBD" + ".ass", file.Properties.secondary_subtitles_file);
}

TEST(ass_extradata, replaces_embedded_nul_with_unicode_replacement_character) {
	AssFile file;
	AssParser parser(&file, 1);
	std::string line = "Data: 7,ow";
	line.push_back('\0');
	line += "ner,eval";
	line.push_back('\0');
	line += "ue";

	parser.AddLine("[Aegisub Extradata]");
	parser.AddLine(line);

	ASSERT_EQ(1u, file.Extradata.size());
	EXPECT_EQ(7u, file.Extradata.front().id);
	EXPECT_EQ(std::string("ow") + "\xEF\xBF\xBD" + "ner", file.Extradata.front().key);
	EXPECT_EQ(std::string("val") + "\xEF\xBF\xBD" + "ue", file.Extradata.front().value);
}

TEST(ass_parser, preserves_embedded_nul_in_event_text) {
	AssFile file;
	AssParser parser(&file, 1);
	std::string line = "Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,before";
	line.push_back('\0');
	line += "after";

	parser.AddLine("[Events]");
	parser.AddLine(line);

	ASSERT_EQ(1u, file.Events.size());
	EXPECT_EQ(std::string("before\0after", 12), file.Events.front().Text.get());
}

TEST(ass_parser, accepts_section_headers_with_surrounding_whitespace) {
	AssFile file;
	AssParser parser(&file, 1);

	parser.AddLine("  [Events]  ");
	parser.AddLine("Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,ok");

	ASSERT_EQ(1u, file.Events.size());
	EXPECT_EQ("ok", file.Events.front().Text.get());
}

TEST(ass_parser, accepts_script_type_with_compatible_suffix) {
	AssFile file;
	AssParser parser(&file, 0);

	parser.AddLine("[Script Info]");
	parser.AddLine("ScriptType: generated by tool v4.00+");
	parser.AddLine("[Events]");
	parser.AddLine("Dialogue: 9,0:00:00.00,0:00:01.00,Default,,0,0,0,,ass");

	ASSERT_EQ(1u, file.Events.size());
	EXPECT_EQ(9, file.Events.front().Layer);
}

TEST(ass_parser, rejects_v4pp_script_type) {
	AssFile file;
	AssParser parser(&file, 1);

	parser.AddLine("[Script Info]");
	EXPECT_THROW(parser.AddLine("ScriptType: v4.00++"), SubtitleFormatParseError);
}

TEST(ass_parser, ignores_custom_event_format_order) {
	AssFile file;
	AssParser parser(&file, 1);

	parser.AddLine("[Events]");
	parser.AddLine("Format: Text, Effect, MarginV, MarginR, MarginL, Name, Style, End, Start, Layer");
	parser.AddLine("Dialogue: 7,0:00:01.00,0:00:02.00,StyleName,Actor,11,22,33,fx,hello");

	ASSERT_EQ(1u, file.Events.size());
	auto const& line = file.Events.front();
	EXPECT_EQ(7, line.Layer);
	EXPECT_EQ(1000, line.Start);
	EXPECT_EQ(2000, line.End);
	EXPECT_EQ("StyleName", line.Style.get());
	EXPECT_EQ("Actor", line.Actor.get());
	EXPECT_EQ(11, line.Margin[0]);
	EXPECT_EQ(22, line.Margin[1]);
	EXPECT_EQ(33, line.Margin[2]);
	EXPECT_EQ("fx", line.Effect.get());
	EXPECT_EQ("hello", line.Text.get());
	ASSERT_EQ(1u, parser.CompatibilityWarnings().size());
	EXPECT_NE(std::string::npos, parser.CompatibilityWarnings().front().find("Custom Event Format"));
}

TEST(ass_parser, ignores_custom_style_format_order) {
	AssFile file;
	AssParser parser(&file, 1);

	parser.AddLine("[V4+ Styles]");
	parser.AddLine("Format: Encoding, MarginV, MarginR, MarginL, Alignment, Shadow, Outline, BorderStyle, Angle, Spacing, ScaleY, ScaleX, StrikeOut, Underline, Italic, Bold, BackColour, OutlineColour, SecondaryColour, PrimaryColour, Fontsize, Fontname, Name");
	parser.AddLine("Style: Fixed,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1");

	ASSERT_EQ(1u, file.Styles.size());
	auto const& style = file.Styles.front();
	EXPECT_EQ("Fixed", style.name);
	EXPECT_EQ("Arial", style.font);
	EXPECT_DOUBLE_EQ(48.0, style.fontsize);
	EXPECT_TRUE(style.bold);
	EXPECT_FALSE(style.italic);
	EXPECT_EQ(10, style.Margin[0]);
	EXPECT_EQ(20, style.Margin[1]);
	EXPECT_EQ(30, style.Margin[2]);
	EXPECT_EQ(1, style.encoding);
	ASSERT_EQ(1u, parser.CompatibilityWarnings().size());
	EXPECT_NE(std::string::npos, parser.CompatibilityWarnings().front().find("Custom Style Format"));
}

TEST(ass_parser, accepts_standard_format_lines_without_compatibility_warning) {
	AssFile file;
	AssParser parser(&file, 1);

	parser.AddLine("[V4+ Styles]");
	parser.AddLine("Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding");
	parser.AddLine("Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,2,2,2,10,20,30,1");
	parser.AddLine("[Events]");
	parser.AddLine("Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text");
	parser.AddLine("Dialogue: 0,0:00:00.00,0:00:01.00,Default,,0,0,0,,ok");

	EXPECT_TRUE(parser.CompatibilityWarnings().empty());
}
