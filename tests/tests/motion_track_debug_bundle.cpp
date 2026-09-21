#include <main.h>

#include "../../src/motion_track/debug_bundle.h"
#include "../../src/motion_track/apply_source.h"
#include "../../src/motion_track/session.h"

#include <ass_dialogue.h>
#include <ass_info.h>
#include <ass_style.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>

#include <cmath>
#include <limits>
#include <numbers>
#include <sstream>

namespace {
using namespace aegisub::motion_track;

void ExpectSamePlan(MotionTrackApplyPlan const& expected, MotionTrackApplyPlan const& actual) {
	EXPECT_EQ(expected.status, actual.status);
	EXPECT_EQ(expected.message, actual.message);
	EXPECT_EQ(expected.event_count, actual.event_count);
	ASSERT_EQ(expected.lines.size(), actual.lines.size());
	for (size_t i = 0; i < expected.lines.size(); ++i) {
		auto const& before = expected.lines[i].parts;
		auto const& after = actual.lines[i].parts;
		ASSERT_EQ(before.size(), after.size());
		for (size_t j = 0; j < before.size(); ++j) {
			EXPECT_EQ(before[j].start_ms, after[j].start_ms);
			EXPECT_EQ(before[j].end_ms, after[j].end_ms);
			EXPECT_EQ(before[j].text, after[j].text);
			EXPECT_EQ(before[j].covered, after[j].covered);
			EXPECT_DOUBLE_EQ(before[j].x0, after[j].x0);
			EXPECT_DOUBLE_EQ(before[j].y0, after[j].y0);
			EXPECT_DOUBLE_EQ(before[j].x1, after[j].x1);
			EXPECT_DOUBLE_EQ(before[j].y1, after[j].y1);
		}
	}
	ASSERT_EQ(expected.uncovered.size(), actual.uncovered.size());
	for (size_t i = 0; i < expected.uncovered.size(); ++i) {
		ASSERT_EQ(expected.uncovered[i].ranges.size(), actual.uncovered[i].ranges.size());
		for (size_t j = 0; j < expected.uncovered[i].ranges.size(); ++j) {
			EXPECT_EQ(expected.uncovered[i].ranges[j].first, actual.uncovered[i].ranges[j].first);
			EXPECT_EQ(expected.uncovered[i].ranges[j].last, actual.uncovered[i].ranges[j].last);
		}
	}
}

class motion_track_debug_bundle : public testing::Test {
	protected:
	AssFile file;
	MotionTrackApplySource source;
	ApplyPlanInput input;

	void SetUp() override {
		auto style = std::make_unique<AssStyle>();
		style->fontsize = 20.1234567890123;
		style->font = "Diagnostic Sans";
		file.Styles.push_back(*style.release());
		file.SetScriptInfo("PlayResX", "1280");
		file.SetScriptInfo("PlayResY", "720");
		file.SetScriptInfo("LayoutResX", "1920");
		file.SetScriptInfo("LayoutResY", "1080");
		file.SetScriptInfo("ScaledBorderAndShadow", "yes");
		file.SetScriptInfo("WrapStyle", "2");
		auto line = std::make_unique<AssDialogue>();
		line->Text = R"({\pos(720,120)\frz12\fscx110\fscy90}tracked subtitle)";
		line->Start = 7;
		line->End = 2497;
		line->Margin = {11, 22, 33};
		file.Events.push_back(*line.release());
		input.model = TrackModel::Similarity;
		input.origin_center_x = 1080.25;
		input.origin_center_y = 180.125;
		input.decode_interval = {.first = 0, .last = 59};
		input.direction_domain = {.first = 0, .last = 59};
		input.storage_width = 1920;
		input.storage_height = 1080;
		input.script_width = 1280;
		input.script_height = 720;
		input.video_frame_count = 90;
		input.timecodes = agi::vfr::Framerate(24000, 1001);
		input.seed_time_ms = input.timecodes.TimeAtFrame(0, agi::vfr::START);
		input.options.position_decimals = 4;
		input.options.stabilization.enable = true;
		input.options.stabilization.position_floor_storage_px = 0.234567890123;
		input.options.stabilization.scale_floor = 0.0045;
		input.options.stabilization.angle_floor_deg = 0.125;
		for (int frame = 0; frame < 60; ++frame) {
			TrackSample sample;
			sample.frame = frame;
			sample.model = input.model;
			sample.status = TrackStatus::Ok;
			sample.center_x = input.origin_center_x + 0.3 * frame;
			sample.center_y = input.origin_center_y + 0.2 * frame;
			sample.confidence = 0.9876543210123;
			sample.residual = 1.0123456789;
			sample.fade_visibility = 0.95 + 0.0005 * frame;
			double const angle = 0.03 * frame * std::numbers::pi / 180.0;
			double const scale = 1.0 + 0.002 * frame;
			sample.transform.matrix[0] = sample.transform.matrix[4] = scale * std::cos(angle);
			sample.transform.matrix[1] = -scale * std::sin(angle);
			sample.transform.matrix[3] = scale * std::sin(angle);
			input.samples.push_back(sample);
		}
		source.Capture(file, {&file.Events.front()}, input.timecodes, input.video_frame_count);
	}

	std::string Export(MotionTrackApplyPlan const& plan) {
		std::ostringstream output;
		WriteMotionTrackDebugBundle(output, file, source.Targets(), input, plan);
		return output.str();
	}

	MotionTrackDebugBundle Read(std::string const& text) {
		std::istringstream stream(text);
		return ReadMotionTrackDebugBundle(stream);
	}

	json::UnknownElement Document(std::string const& text) {
		json::UnknownElement document;
		std::istringstream stream(text);
		json::Reader::Read(document, stream);
		return document;
	}
};
} // namespace

TEST_F(motion_track_debug_bundle, round_trip_replays_compact_and_exact_from_analyze_baseline_after_apply) {
	for (auto mode : {ApplyMode::Compact, ApplyMode::Exact}) {
		input.options.mode = mode;
		auto plan = BuildApplyPlan(file, source.Targets(), input);
		ASSERT_TRUE(plan.has_mutations()) << plan.message;
		auto text = Export(plan);
		auto bundle = Read(text);
		ASSERT_EQ(1u, bundle.targets.size());
		EXPECT_NE(source.Targets()[0], bundle.targets[0]);
		EXPECT_EQ(source.Targets()[0]->Text.get(), bundle.targets[0]->Text.get());
		EXPECT_EQ(7, bundle.targets[0]->Start.GetMillisecond());
		EXPECT_EQ(2497, bundle.targets[0]->End.GetMillisecond());
		EXPECT_EQ((std::array<int, 3>{11, 22, 33}), bundle.targets[0]->Margin);
		EXPECT_DOUBLE_EQ(file.Styles.front().fontsize, bundle.file->Styles.front().fontsize);
		EXPECT_EQ(input.model, bundle.input.model);
		EXPECT_EQ(input.timecodes.FPSFraction(), bundle.input.timecodes.FPSFraction());
		EXPECT_EQ(input.seed_time_ms, bundle.input.seed_time_ms);
		EXPECT_EQ(input.video_frame_count, bundle.input.video_frame_count);
		EXPECT_EQ(input.options.mode, bundle.input.options.mode);
		EXPECT_DOUBLE_EQ(input.options.stabilization.position_floor_storage_px,
						 bundle.input.options.stabilization.position_floor_storage_px);
		ASSERT_EQ(input.samples.size(), bundle.input.samples.size());
		for (size_t i = 0; i < input.samples.size(); ++i) {
			EXPECT_EQ(input.samples[i].transform.matrix, bundle.input.samples[i].transform.matrix);
			EXPECT_DOUBLE_EQ(input.samples[i].confidence, bundle.input.samples[i].confidence);
			EXPECT_DOUBLE_EQ(input.samples[i].residual, bundle.input.samples[i].residual);
			EXPECT_DOUBLE_EQ(input.samples[i].fade_visibility, bundle.input.samples[i].fade_visibility);
		}
		ExpectSamePlan(plan, bundle.exported_plan);
		ExpectSamePlan(plan, BuildApplyPlan(*bundle.file, bundle.targets, bundle.input));
		std::string message;
		ASSERT_TRUE(source.Apply(file, plan, input.timecodes, [this](auto const&) { file.Commit("apply", AssFile::COMMIT_DIAG_FULL); }, message)) << message;
		EXPECT_EQ(text, Export(plan));
	}
}

TEST_F(motion_track_debug_bundle, exports_only_target_content_referenced_styles_and_rendering_info) {
	file.Properties.video_file = "excluded-video-property";
	file.Properties.audio_file = "excluded-audio-property";
	file.Properties.automation_scripts = "excluded-project-property";
	file.SetScriptInfo("Title", "excluded-title");
	file.SetScriptInfo("Original Script", "excluded-author");
	file.AddExtradata("excluded-extradata-key", "excluded-extradata-value");
	auto other = std::make_unique<AssDialogue>();
	other->Text = "excluded-dialogue";
	file.Events.push_back(*other.release());
	auto style = std::make_unique<AssStyle>();
	style->name = "excluded-style";
	file.Styles.push_back(*style.release());
	style = std::make_unique<AssStyle>();
	style->name = "Reset";
	style->font = "Included Reset Font";
	file.Styles.push_back(*style.release());
	file.Events.front().Text = R"({\pos(720,120)\ rReset}included subtitle)";
	source.Capture(file, {&file.Events.front()}, input.timecodes, input.video_frame_count);
	auto plan = BuildApplyPlan(file, source.Targets(), input);
	ASSERT_TRUE(plan.has_mutations()) << plan.message;
	auto text = Export(plan);
	EXPECT_EQ(std::string::npos, text.find("excluded-"));
	auto bundle = Read(text);
	ASSERT_EQ(1u, bundle.targets.size());
	EXPECT_EQ(file.Events.front().Text.get(), bundle.targets[0]->Text.get());
	EXPECT_EQ(2u, std::distance(bundle.file->Styles.begin(), bundle.file->Styles.end()));
	ASSERT_NE(nullptr, bundle.file->GetStyle("Reset"));
	EXPECT_EQ("Included Reset Font", bundle.file->GetStyle("Reset")->font);
	EXPECT_EQ("1920", bundle.file->GetScriptInfo("LayoutResX"));
	EXPECT_EQ("2", bundle.file->GetScriptInfo("WrapStyle"));
	EXPECT_TRUE(bundle.file->Properties.video_file.empty());
	EXPECT_TRUE(bundle.file->Extradata.empty());
	ExpectSamePlan(plan, BuildApplyPlan(*bundle.file, bundle.targets, bundle.input));
}

TEST_F(motion_track_debug_bundle, vfr_failed_gaps_and_fade_options_replay_without_losing_diagnostics) {
	input.model = TrackModel::Translation;
	input.timecodes = agi::vfr::Framerate({0, 41, 83, 130, 165, 208});
	input.seed_time_ms = input.timecodes.TimeAtFrame(0, agi::vfr::START);
	input.options.smooth_frames = 2;
	input.options.apply_fad = true;
	input.options.scale_border = input.options.scale_shadow = input.options.scale_blur = true;
	input.fade_interval = FadeInterval{
		.fade_in = {.detected = true, .outer_frame = 0, .full_visibility_frame = 3, .confidence = 0.8123456789},
		.fade_out = {.detected = true, .outer_frame = 59, .full_visibility_frame = 54, .confidence = 0.923456789}};
	for (auto& sample : input.samples) {
		sample.model = input.model;
		sample.transform = {};
	}
	input.samples[12].status = TrackStatus::Failed;
	input.samples[12].failure = TrackFailureReason::NccLow;
	input.samples[12].confidence = std::numeric_limits<double>::quiet_NaN();
	input.samples[12].residual = std::numeric_limits<double>::infinity();
	auto plan = BuildApplyPlan(file, source.Targets(), input);
	ASSERT_TRUE(plan.has_mutations()) << plan.message;
	auto bundle = Read(Export(plan));
	ASSERT_TRUE(bundle.input.fade_interval);
	EXPECT_EQ(54, bundle.input.fade_interval->fade_out.full_visibility_frame);
	EXPECT_DOUBLE_EQ(0.8123456789, bundle.input.fade_interval->fade_in.confidence);
	EXPECT_EQ(2, bundle.input.options.smooth_frames);
	EXPECT_TRUE(bundle.input.options.apply_fad);
	EXPECT_TRUE(bundle.input.options.scale_border);
	EXPECT_TRUE(bundle.input.options.scale_shadow);
	EXPECT_TRUE(bundle.input.options.scale_blur);
	EXPECT_EQ(TrackStatus::Failed, bundle.input.samples[12].status);
	EXPECT_EQ(TrackFailureReason::NccLow, bundle.input.samples[12].failure);
	EXPECT_TRUE(std::isnan(bundle.input.samples[12].confidence));
	EXPECT_EQ(std::numeric_limits<double>::infinity(), bundle.input.samples[12].residual);
	for (int frame : {-1, 0, 4, 5, 6, 60, 90})
		for (auto mode : {agi::vfr::EXACT, agi::vfr::START, agi::vfr::END})
			EXPECT_EQ(input.timecodes.TimeAtFrame(frame, mode), bundle.input.timecodes.TimeAtFrame(frame, mode));
	ExpectSamePlan(plan, BuildApplyPlan(*bundle.file, bundle.targets, bundle.input));
}

TEST_F(motion_track_debug_bundle, incomplete_coverage_diagnostics_replay_with_owned_source_references) {
	input.samples.erase(input.samples.begin() + 10);
	auto plan = BuildApplyPlan(file, source.Targets(), input);
	ASSERT_EQ(ApplyPlanStatus::IncompleteCoverage, plan.status);
	auto bundle = Read(Export(plan));
	ASSERT_FALSE(bundle.exported_plan.uncovered.empty());
	EXPECT_EQ(bundle.targets[0], bundle.exported_plan.uncovered[0].source);
	ExpectSamePlan(plan, bundle.exported_plan);
	ExpectSamePlan(plan, BuildApplyPlan(*bundle.file, bundle.targets, bundle.input));
}

TEST_F(motion_track_debug_bundle, rejects_future_schema_malformed_matrix_dangling_source_and_unsafe_time_domain) {
	auto plan = BuildApplyPlan(file, source.Targets(), input);
	ASSERT_TRUE(plan.has_mutations()) << plan.message;
	auto text = Export(plan);
	for (int corruption = 0; corruption < 4; ++corruption) {
		auto document = Document(text);
		auto& root = static_cast<json::Object&>(document);
		if (corruption == 0)
			root["version"] = 999;
		if (corruption == 1) {
			auto& samples = static_cast<json::Array&>(static_cast<json::Object&>(root.at("input")).at("samples"));
			static_cast<json::Object&>(samples[0])["matrix"] = json::Array{};
		}
		if (corruption == 2) {
			auto& lines = static_cast<json::Array&>(static_cast<json::Object&>(root.at("plan")).at("lines"));
			static_cast<json::Object&>(lines[0])["source"] = 12345;
		}
		if (corruption == 3)
			static_cast<json::Object&>(root.at("input"))["video_frame_count"] = std::numeric_limits<int>::max();
		std::ostringstream output;
		agi::JsonWriter::Write(root, output);
		EXPECT_THROW(Read(output.str()), std::runtime_error) << corruption;
	}
}

TEST_F(motion_track_debug_bundle, write_failure_is_reported_without_mutating_source) {
	auto plan = BuildApplyPlan(file, source.Targets(), input);
	ASSERT_TRUE(plan.has_mutations()) << plan.message;
	auto const before = file.Events.front().GetEntryData();
	std::ostringstream output;
	output.setstate(std::ios::badbit);
	EXPECT_THROW(WriteMotionTrackDebugBundle(output, file, source.Targets(), input, plan), std::runtime_error);
	EXPECT_EQ(before, file.Events.front().GetEntryData());
	EXPECT_EQ(1, std::distance(file.Events.begin(), file.Events.end()));
}
