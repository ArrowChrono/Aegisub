#include <main.h>

#include "../../src/ass_tag_scanner.h"
#include "../../src/motion_track/apply_plan.h"
#include "../../src/perspective_ass_bounds.h"
#include "../../src/perspective_ass_state.h"
#include "../../src/perspective_tag_solver.h"

#include <ass_dialogue.h>
#include <ass_file.h>
#include <ass_style.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace aegisub::motion_track;
using namespace perspective;

bool MeasuredText(AssStyle *style, std::string const& text,
				  double& width, double& height, double& descent, double& leading) {
	width = static_cast<double>(text.size()) * (style->fontsize * 0.5 + style->spacing);
	height = style->fontsize;
	descent = style->fontsize * 0.2;
	leading = 0.0;
	return true;
}

bool UnavailableText(AssStyle *, std::string const&,
					 double&, double&, double&, double&) {
	return false;
}

size_t PoseTransformCount(std::string const& text) {
	size_t count = 0;
	AssDialogue event;
	event.Text = text;
	for (auto const& block : event.ParseTags()) {
		if (block->GetType() != AssBlockType::OVERRIDE)
			continue;
		aegisub::ass_tag_scanner::ScanRawTags(block->GetRawText(), [&](auto const& tag) {
			if (tag.name == "t" && (tag.args.find("\\frz") != std::string_view::npos ||
									tag.args.find("\\fsc") != std::string_view::npos))
				++count;
		});
	}
	return count;
}

void SetPose(TrackSample& sample, double scale_percent, double clockwise_degrees) {
	double const angle = clockwise_degrees * std::numbers::pi / 180.0;
	double const scale = scale_percent / 100.0;
	sample.transform.matrix = {scale * std::cos(angle), -scale * std::sin(angle), 0,
							   scale * std::sin(angle), scale * std::cos(angle), 0, 0, 0, 1};
}

struct PixelFixture {
	AssFile file;
	AssDialogue *line = nullptr;
	ApplyPlanInput input;

	explicit PixelFixture(double font_size = 12.0) {
		auto *style = new AssStyle;
		style->name = "Default";
		style->font = "Measured Test Font";
		style->fontsize = font_size;
		style->alignment = 7;
		style->outline_w = 0.0;
		style->shadow_w = 0.0;
		file.Styles.push_back(*style);
		line = new AssDialogue;
		line->Start = 0;
		line->End = 1050;
		line->Text = R"({\an7\pos(0,0)}zoom)";
		file.Events.push_back(*line);
		input.model = TrackModel::Similarity;
		input.storage_width = 1920;
		input.storage_height = 1080;
		input.script_width = 1280;
		input.script_height = 720;
		input.direction_domain = input.decode_interval = {.first = 0, .last = 10};
		input.timecodes = agi::vfr::Framerate(10.0);
		input.video_frame_count = 11;
		input.text_extents = MeasuredText;
		input.options.compact_epsilon = 0.75;
		input.options.position_decimals = 4;
		FillCurvedPose();
	}

	void FillCurvedPose() {
		input.samples.clear();
		for (int frame = input.direction_domain.first; frame <= input.direction_domain.last; ++frame) {
			TrackSample sample;
			sample.frame = frame;
			sample.status = TrackStatus::Ok;
			double const progress = static_cast<double>(frame - input.direction_domain.first) /
									(input.direction_domain.last - input.direction_domain.first);
			double const jitter = progress == 0.0 || progress == 1.0 ? 0.0 : (frame % 2 ? 0.3 : -0.3);
			double const time = input.timecodes.TimeAtFrame(frame);
			sample.center_x = 0.025 * time;
			sample.center_y = -0.012 * time;
			SetPose(sample, 100.0 + 16.0 * progress + 0.6 * std::sin(std::numbers::pi * progress) + jitter,
					0.12 * std::sin(5.0 * std::numbers::pi * progress));
			input.samples.push_back(sample);
		}
	}

	[[nodiscard]] MotionTrackApplyPlan Build() const {
		return BuildApplyPlan(file, {line}, input);
	}

	[[nodiscard]] Resolution PlayResolution() const {
		return {.width = static_cast<double>(input.script_width), .height = static_cast<double>(input.script_height)};
	}

	[[nodiscard]] AssDialogue EventAt(MotionTrackApplyPlan const& plan, int time) const {
		for (auto const& planned : plan.lines) {
			for (auto const& part : planned.parts) {
				int const start = agi::Time(part.start_ms);
				int const end = agi::Time(part.end_ms);
				if (!part.covered || time < start || time >= end)
					continue;
				AssDialogue event(*line);
				event.Start = start;
				event.End = end;
				event.Text = part.text;
				return event;
			}
		}
		ADD_FAILURE() << "No covered serialized event at " << time;
		return AssDialogue(*line);
	}

	[[nodiscard]] std::optional<ForwardInput> SourceGeometry() const {
		ForwardInput forward;
		forward.play_resolution = PlayResolution();
		forward.video_storage_resolution = Resolution{
			.width = static_cast<double>(input.storage_width), .height = static_cast<double>(input.storage_height)};
		int const layout_x = file.GetScriptInfoAsInt("LayoutResX");
		int const layout_y = file.GetScriptInfoAsInt("LayoutResY");
		if (layout_x > 0 && layout_y > 0)
			forward.layout_resolution = Resolution{.width = static_cast<double>(layout_x), .height = static_cast<double>(layout_y)};
		auto const aspect = ResolvePerspectiveLayoutAspect(forward);
		EXPECT_TRUE(aspect.has_value());
		if (!aspect)
			return std::nullopt;
		auto const source = EvaluateEffectiveAssState({.file = &file, .line = line, .play_resolution = PlayResolution(), .capture_time_ms = line->Start.GetMillisecond()});
		EXPECT_TRUE(source) << DescribeAssStateError(source.error);
		if (!source)
			return std::nullopt;
		auto const bounds = EvaluateAssBaseBounds({.line = line, .state = &source.value, .text_extents = MeasuredText, .layout_aspect = *aspect});
		EXPECT_TRUE(bounds) << DescribeAssBoundsError(bounds.error);
		if (!bounds)
			return std::nullopt;
		forward.bounds = bounds.value;
		forward.state = source.value.transform;
		return forward;
	}

	[[nodiscard]] EvaluatedTransformState Expected(ForwardInput const& source, TrackSample const& sample) const {
		auto state = source.state;
		state.event_time_ms = input.timecodes.TimeAtFrame(sample.frame) - static_cast<int>(line->Start);
		state.position = {.x = sample.center_x * input.script_width / input.storage_width,
						  .y = sample.center_y * input.script_height / input.storage_height};
		double const scale = std::hypot(sample.transform.matrix[0], sample.transform.matrix[3]);
		state.scale_x *= scale;
		state.scale_y *= scale;
		state.rotation_z -= std::atan2(sample.transform.matrix[3], sample.transform.matrix[0]) * 180.0 / std::numbers::pi;
		return state;
	}

	[[nodiscard]] std::vector<double> FrameErrors(MotionTrackApplyPlan const& plan) const {
		std::vector<double> errors;
		auto const source = SourceGeometry();
		if (!source)
			return errors;
		for (auto const& sample : input.samples) {
			SCOPED_TRACE(sample.frame);
			auto const target = ForwardQuad(*source, Expected(*source, sample));
			EXPECT_TRUE(target) << DescribeForwardError(target.error);
			if (!target)
				return {};
			int const time = input.timecodes.TimeAtFrame(sample.frame);
			auto const event = EventAt(plan, time);
			auto const actual = EvaluateEffectiveAssState({.file = &file, .line = &event, .play_resolution = PlayResolution(), .capture_time_ms = time});
			EXPECT_TRUE(actual) << DescribeAssStateError(actual.error) << ' ' << event.Text.get();
			if (!actual)
				return {};
			auto candidate = *source;
			candidate.state = actual.value.transform;
			auto const residual = MeasurePerspectiveResidual(candidate, target.quad,
															 {.scale_x = static_cast<double>(input.storage_width) / input.script_width,
															  .scale_y = static_cast<double>(input.storage_height) / input.script_height});
			EXPECT_TRUE(residual) << DescribeResidualError(residual.error);
			if (!residual)
				return {};
			EXPECT_LE(residual.max_error, input.options.compact_epsilon) << event.Text.get();
			errors.push_back(residual.max_error);
		}
		return errors;
	}

	void ExpectStrictPoseForEveryTextRun(MotionTrackApplyPlan const& plan) const {
		for (auto const& sample : input.samples) {
			SCOPED_TRACE(sample.frame);
			int const time = input.timecodes.TimeAtFrame(sample.frame);
			auto event = EventAt(plan, time);
			std::string prefix;
			int runs = 0;
			for (auto const& block : event.ParseTags()) {
				if (block->GetType() == AssBlockType::OVERRIDE)
					prefix += block->GetText();
				else if (block->GetType() == AssBlockType::PLAIN && !block->GetText().empty()) {
					// Keep the original override/reset chain, but evaluate this
					// visible run alone so mixed fonts do not hide its pose.
					AssDialogue run(event);
					run.Text = prefix + block->GetText();
					auto const actual = EvaluateEffectiveAssState({.file = &file, .line = &run, .play_resolution = PlayResolution(), .capture_time_ms = time});
					ASSERT_TRUE(actual) << DescribeAssStateError(actual.error) << ' ' << run.Text.get();
					double const scale = 100.0 * std::hypot(sample.transform.matrix[0], sample.transform.matrix[3]);
					double const rotation = file.Styles.front().angle -
											std::atan2(sample.transform.matrix[3], sample.transform.matrix[0]) * 180.0 / std::numbers::pi;
					EXPECT_NEAR(scale, actual.value.transform.scale_x, 0.05);
					EXPECT_NEAR(scale, actual.value.transform.scale_y, 0.05);
					EXPECT_NEAR(rotation, actual.value.transform.rotation_z, 0.05);
					++runs;
				}
			}
			EXPECT_GT(runs, 0);
		}
	}
};
}

TEST(motion_track_compact_pixel, small_text_uses_one_transform_with_total_pixel_error) {
	PixelFixture fixture;
	auto const plan = fixture.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
	ASSERT_EQ(1U, plan.event_count);
	ASSERT_EQ(1U, plan.lines.size());
	ASSERT_EQ(1U, plan.lines.front().parts.size());
	EXPECT_LE(PoseTransformCount(plan.lines.front().parts.front().text), 1U);
	EXPECT_EQ(fixture.input.samples.size(), fixture.FrameErrors(plan).size());
}

TEST(motion_track_compact_pixel, curved_zoom_uses_one_accelerated_transform_instead_of_strict_pieces) {
	PixelFixture fixture(60.0);
	for (auto& sample : fixture.input.samples) {
		double const progress = sample.frame / 10.0;
		double const jitter = sample.frame == 0 || sample.frame == 10 ? 0.0 : (sample.frame % 2 ? 0.3 : -0.3);
		SetPose(sample, 100.0 + 16.0 * progress * progress + jitter, 0.0);
	}
	auto const plan = fixture.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
	ASSERT_EQ(1U, plan.event_count);
	ASSERT_EQ(1U, plan.lines.size());
	ASSERT_EQ(1U, plan.lines.front().parts.size());
	auto const& text = plan.lines.front().parts.front().text;
	ASSERT_EQ(1U, PoseTransformCount(text)) << text;
	double acceleration = 1.0;
	AssDialogue event;
	event.Text = text;
	for (auto const& block : event.ParseTags()) {
		if (block->GetType() != AssBlockType::OVERRIDE)
			continue;
		aegisub::ass_tag_scanner::ScanRawTags(block->GetRawText(), [&](auto const& tag) {
			if (tag.name != "t")
				return;
			auto const args = aegisub::ass_tag_scanner::SplitLibassArgs(tag.args);
			EXPECT_EQ(4U, args.size()) << tag.bytes;
			if (args.size() == 4)
				acceleration = aegisub::ass_tag_scanner::ArgToDouble(args[2]);
		});
	}
	EXPECT_GT(acceleration, 0.0);
	EXPECT_NE(1.0, acceleration);
	EXPECT_EQ(fixture.input.samples.size(), fixture.FrameErrors(plan).size());

	fixture.input.text_extents = nullptr;
	auto const strict = fixture.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, strict.status) << strict.message;
	ASSERT_EQ(1U, strict.event_count);
	ASSERT_EQ(1U, strict.lines.size());
	ASSERT_EQ(1U, strict.lines.front().parts.size());
	EXPECT_GT(PoseTransformCount(strict.lines.front().parts.front().text), 1U);
	fixture.ExpectStrictPoseForEveryTextRun(strict);
}

TEST(motion_track_compact_pixel, tiny_nonmonotonic_pose_can_remain_static) {
	PixelFixture fixture;
	for (auto& sample : fixture.input.samples) {
		double const phase = sample.frame * std::numbers::pi / 10.0;
		SetPose(sample, 100.0 + 0.2 * std::sin(5.0 * phase), 0.12 * std::sin(3.0 * phase));
	}
	auto const plan = fixture.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
	ASSERT_EQ(1U, plan.event_count);
	ASSERT_EQ(1U, plan.lines.size());
	ASSERT_EQ(1U, plan.lines.front().parts.size());
	EXPECT_EQ(0U, PoseTransformCount(plan.lines.front().parts.front().text));
	EXPECT_EQ(fixture.input.samples.size(), fixture.FrameErrors(plan).size());
	fixture.input.text_extents = nullptr;
	auto const strict = fixture.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, strict.status) << strict.message;
	ASSERT_EQ(1U, strict.lines.size());
	ASSERT_EQ(1U, strict.lines.front().parts.size());
	EXPECT_GT(PoseTransformCount(strict.lines.front().parts.front().text), 1U);
}

TEST(motion_track_compact_pixel, drawing_uses_intrinsic_bounds_without_font_measurement) {
	PixelFixture fixture;
	fixture.line->Text = R"({\an7\pos(0,0)\p1}m 0 0 l 24 0 24 12 0 12)";
	fixture.input.text_extents = nullptr;
	auto const plan = fixture.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
	ASSERT_EQ(1U, plan.event_count);
	ASSERT_EQ(1U, plan.lines.size());
	ASSERT_EQ(1U, plan.lines.front().parts.size());
	EXPECT_LE(PoseTransformCount(plan.lines.front().parts.front().text), 1U);
	EXPECT_EQ(fixture.input.samples.size(), fixture.FrameErrors(plan).size());
}

TEST(motion_track_compact_pixel, large_text_or_tight_budget_keeps_piecewise_pose) {
	for (double const font_size : {12.0, 180.0}) {
		SCOPED_TRACE(font_size);
		PixelFixture fixture(font_size);
		if (font_size == 12.0)
			fixture.input.options.compact_epsilon = 0.02;
		auto const plan = fixture.Build();
		ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
		ASSERT_EQ(1U, plan.event_count);
		ASSERT_EQ(1U, plan.lines.size());
		ASSERT_EQ(1U, plan.lines.front().parts.size());
		EXPECT_GT(PoseTransformCount(plan.lines.front().parts.front().text), 1U);
		if (font_size == 12.0) {
			// Rejected simplification retains the existing independent
			// position/scalar contracts, not a new total-geometry guarantee.
			fixture.ExpectStrictPoseForEveryTextRun(plan);
			fixture.input.text_extents = nullptr;
			auto const fallback = fixture.Build();
			ASSERT_EQ(ApplyPlanStatus::Ok, fallback.status) << fallback.message;
			ASSERT_EQ(1U, fallback.lines.size());
			ASSERT_EQ(1U, fallback.lines.front().parts.size());
			EXPECT_EQ(fallback.lines.front().parts.front().start_ms, plan.lines.front().parts.front().start_ms);
			EXPECT_EQ(fallback.lines.front().parts.front().end_ms, plan.lines.front().parts.front().end_ms);
			EXPECT_EQ(fallback.lines.front().parts.front().text, plan.lines.front().parts.front().text);
			for (auto const& sample : fixture.input.samples) {
				SCOPED_TRACE(sample.frame);
				int const time = fixture.input.timecodes.TimeAtFrame(sample.frame);
				auto const event = fixture.EventAt(plan, time);
				auto const actual = EvaluateEffectiveAssState({.file = &fixture.file, .line = &event, .play_resolution = fixture.PlayResolution(), .capture_time_ms = time});
				ASSERT_TRUE(actual) << DescribeAssStateError(actual.error);
				double const dx = actual.value.transform.position.x * fixture.input.storage_width /
									  fixture.input.script_width -
								  sample.center_x;
				double const dy = actual.value.transform.position.y * fixture.input.storage_height /
									  fixture.input.script_height -
								  sample.center_y;
				EXPECT_LE(std::hypot(dx, dy), fixture.input.options.compact_epsilon);
			}
		}
		else
			EXPECT_EQ(fixture.input.samples.size(), fixture.FrameErrors(plan).size());
	}
}

TEST(motion_track_compact_pixel, rotated_geometry_uses_storage_pixels_at_nonuniform_playres) {
	std::vector<double> baseline_errors;
	for (auto const resolution : {Resolution{.width = 1280, .height = 720},
								  Resolution{.width = 640, .height = 360}, Resolution{.width = 640, .height = 720}}) {
		SCOPED_TRACE(std::to_string(resolution.width) + "x" + std::to_string(resolution.height));
		PixelFixture fixture(12.0 * resolution.height / 720.0);
		fixture.input.script_width = static_cast<int>(resolution.width);
		fixture.input.script_height = static_cast<int>(resolution.height);
		fixture.file.Styles.front().angle = 23.0;
		auto const plan = fixture.Build();
		ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
		ASSERT_EQ(1U, plan.event_count);
		ASSERT_EQ(1U, plan.lines.size());
		ASSERT_EQ(1U, plan.lines.front().parts.size());
		EXPECT_LE(PoseTransformCount(plan.lines.front().parts.front().text), 1U);
		auto const errors = fixture.FrameErrors(plan);
		ASSERT_EQ(fixture.input.samples.size(), errors.size());
		if (baseline_errors.empty())
			baseline_errors = errors;
		else {
			ASSERT_EQ(baseline_errors.size(), errors.size());
			for (size_t frame = 0; frame < errors.size(); ++frame)
				EXPECT_NEAR(baseline_errors[frame], errors[frame], 0.001) << "frame " << frame;
		}
	}
}

TEST(motion_track_compact_pixel, unavailable_or_missing_font_measurement_keeps_strict_pose) {
	for (auto const provider : {UnavailableText, static_cast<AssTextExtentsProvider>(nullptr)}) {
		PixelFixture fixture;
		fixture.input.text_extents = provider;
		auto const plan = fixture.Build();
		ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
		ASSERT_EQ(1U, plan.event_count);
		ASSERT_EQ(1U, plan.lines.size());
		ASSERT_EQ(1U, plan.lines.front().parts.size());
		EXPECT_GT(PoseTransformCount(plan.lines.front().parts.front().text), 1U);
		fixture.ExpectStrictPoseForEveryTextRun(plan);
		// The independent oracle can still use the known synthetic font to
		// check that fallback did not discard the trajectory altogether.
		EXPECT_EQ(fixture.input.samples.size(), fixture.FrameErrors(plan).size());
	}
}

TEST(motion_track_compact_pixel, mixed_font_and_named_reset_preserve_every_run_without_optimization) {
	struct InputCase {
		char const *text;
		size_t visible_runs;
	};
	constexpr std::array expected_text{"zoom", "tail", "end"};
	constexpr std::array expected_font{"Measured Test Font", "Second Measured Font", "Measured Test Font"};
	for (auto const& test : {
			 InputCase{.text = R"({\an7\pos(0,0)}zoom{\fnSecond Measured Font}tail)", .visible_runs = 2},
			 InputCase{.text = R"({\an7\pos(0,0)}zoom{\rAlternate}tail{\r}end)", .visible_runs = 3}}) {
		SCOPED_TRACE(test.text);
		PixelFixture fixture;
		auto *alternate = new AssStyle(fixture.file.Styles.front());
		alternate->name = "Alternate";
		alternate->font = "Second Measured Font";
		fixture.file.Styles.push_back(*alternate);
		fixture.line->Text = test.text;
		auto const source = EvaluateEffectiveAssState({.file = &fixture.file, .line = fixture.line, .play_resolution = fixture.PlayResolution(), .capture_time_ms = 0});
		ASSERT_EQ(AssStateError::MixedGeometryRuns, source.error);
		auto const plan = fixture.Build();
		ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
		ASSERT_EQ(1U, plan.event_count);
		ASSERT_EQ(1U, plan.lines.size());
		ASSERT_EQ(1U, plan.lines.front().parts.size());
		EXPECT_GT(PoseTransformCount(plan.lines.front().parts.front().text), 1U);
		fixture.ExpectStrictPoseForEveryTextRun(plan);
		auto const event = fixture.EventAt(plan, 500);
		std::string prefix;
		size_t visible_runs = 0;
		for (auto const& block : event.ParseTags()) {
			if (block->GetType() == AssBlockType::OVERRIDE)
				prefix += block->GetText();
			else if (block->GetType() == AssBlockType::PLAIN && !block->GetText().empty()) {
				ASSERT_LT(visible_runs, test.visible_runs);
				EXPECT_EQ(expected_text[visible_runs], block->GetText());
				AssDialogue run(event);
				run.Text = prefix + block->GetText();
				auto const state = EvaluateEffectiveAssState({.file = &fixture.file, .line = &run, .play_resolution = fixture.PlayResolution(), .capture_time_ms = 500});
				ASSERT_TRUE(state) << DescribeAssStateError(state.error);
				EXPECT_EQ(expected_font[visible_runs], state.value.text_style.font_name);
				++visible_runs;
			}
		}
		EXPECT_EQ(test.visible_runs, visible_runs);
		fixture.input.text_extents = UnavailableText;
		auto const fallback = fixture.Build();
		ASSERT_EQ(ApplyPlanStatus::Ok, fallback.status) << fallback.message;
		ASSERT_EQ(1U, fallback.lines.size());
		ASSERT_EQ(1U, fallback.lines.front().parts.size());
		EXPECT_EQ(fallback.lines.front().parts.front().text, plan.lines.front().parts.front().text);
		EXPECT_EQ(test.text, fixture.line->Text.get());
	}
}

TEST(motion_track_compact_pixel, later_zoom_crossing_automatic_wrap_uses_strict_fallback) {
	PixelFixture fixture(20.0);
	fixture.input.script_width = 64;
	fixture.input.script_height = 36;
	auto source = EvaluateEffectiveAssState({.file = &fixture.file, .line = fixture.line, .play_resolution = fixture.PlayResolution(), .capture_time_ms = 0});
	ASSERT_TRUE(source) << DescribeAssStateError(source.error);
	auto const seed_bounds = EvaluateAssBaseBounds({.line = fixture.line, .state = &source.value, .text_extents = MeasuredText});
	ASSERT_TRUE(seed_bounds) << DescribeAssBoundsError(seed_bounds.error);
	source.value.transform.scale_x = 116.0;
	auto const enlarged_bounds = EvaluateAssBaseBounds({.line = fixture.line, .state = &source.value, .text_extents = MeasuredText});
	ASSERT_EQ(AssBoundsError::UnsupportedAutomaticWrap, enlarged_bounds.error);

	auto const plan = fixture.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
	ASSERT_EQ(1U, plan.event_count);
	ASSERT_EQ(1U, plan.lines.size());
	ASSERT_EQ(1U, plan.lines.front().parts.size());
	EXPECT_GT(PoseTransformCount(plan.lines.front().parts.front().text), 1U);
	fixture.ExpectStrictPoseForEveryTextRun(plan);
	fixture.input.text_extents = UnavailableText;
	auto const fallback = fixture.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, fallback.status) << fallback.message;
	ASSERT_EQ(1U, fallback.lines.size());
	ASSERT_EQ(1U, fallback.lines.front().parts.size());
	EXPECT_EQ(fallback.lines.front().parts.front().text, plan.lines.front().parts.front().text);
}

TEST(motion_track_compact_pixel, vfr_position_and_pose_must_share_one_total_pixel_budget) {
	for (bool const noisy_position : {false, true}) {
		SCOPED_TRACE(noisy_position);
		PixelFixture fixture(30.0);
		fixture.input.timecodes = agi::vfr::Framerate({0, 43, 89, 151, 209, 267, 311, 367, 443, 499, 557, 601, 650});
		fixture.input.direction_domain = fixture.input.decode_interval = {.first = 1, .last = 11};
		fixture.input.video_frame_count = 12;
		fixture.input.seed_time_ms = 43;
		fixture.line->Start = 20;
		fixture.line->End = 630;
		fixture.input.samples.clear();
		for (int frame = 1; frame <= 11; ++frame) {
			TrackSample sample;
			sample.frame = frame;
			sample.status = TrackStatus::Ok;
			double const elapsed = fixture.input.timecodes.TimeAtFrame(frame) - 43.0;
			double const progress = elapsed / 558.0;
			double const sign = frame == 1 || frame == 11 ? 0.0 : (frame % 2 ? -1.0 : 1.0);
			sample.center_x = 0.08 * elapsed + (noisy_position ? 0.48 * sign : 0.0);
			SetPose(sample, 100.0 + 16.0 * progress + 0.5 * sign, 0.0);
			fixture.input.samples.push_back(sample);
		}

		auto const plan = fixture.Build();
		ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
		ASSERT_EQ(1U, plan.event_count);
		ASSERT_EQ(1U, plan.lines.size());
		ASSERT_EQ(1U, plan.lines.front().parts.size());
		auto const& part = plan.lines.front().parts.front();
		EXPECT_NE(0, part.start_ms % 10);
		EXPECT_EQ(20, static_cast<int>(agi::Time(part.start_ms)));
		if (noisy_position)
			EXPECT_GT(PoseTransformCount(part.text), 1U) << part.text;
		else
			EXPECT_LE(PoseTransformCount(part.text), 1U) << part.text;
		EXPECT_EQ(fixture.input.samples.size(), fixture.FrameErrors(plan).size());
	}
}

TEST(motion_track_compact_pixel, raw_scale_crossing_wrap_boundary_is_not_hidden_by_tag_rounding) {
	PixelFixture fixture(22.0 / 1.10001);
	fixture.input.script_width = 64;
	fixture.input.script_height = 36;
	for (auto& sample : fixture.input.samples) {
		double const progress = sample.frame / 10.0;
		double const jitter = sample.frame == 0 || sample.frame == 10 ? 0.0 : (sample.frame % 2 ? 0.052 : -0.052);
		SetPose(sample, 100.0 + 10.004 * progress + jitter, 0.0);
	}
	auto source = EvaluateEffectiveAssState({.file = &fixture.file, .line = fixture.line, .play_resolution = fixture.PlayResolution(), .capture_time_ms = 0});
	ASSERT_TRUE(source) << DescribeAssStateError(source.error);
	source.value.transform.scale_x = 110.00;
	auto const rounded_bounds = EvaluateAssBaseBounds({.line = fixture.line, .state = &source.value, .text_extents = MeasuredText});
	ASSERT_TRUE(rounded_bounds) << DescribeAssBoundsError(rounded_bounds.error);
	source.value.transform.scale_x = 110.004;
	auto const raw_bounds = EvaluateAssBaseBounds({.line = fixture.line, .state = &source.value, .text_extents = MeasuredText});
	ASSERT_EQ(AssBoundsError::UnsupportedAutomaticWrap, raw_bounds.error);

	auto const wrapped = fixture.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, wrapped.status) << wrapped.message;
	ASSERT_EQ(1U, wrapped.event_count);
	ASSERT_EQ(1U, wrapped.lines.size());
	ASSERT_EQ(1U, wrapped.lines.front().parts.size());
	EXPECT_GT(PoseTransformCount(wrapped.lines.front().parts.front().text), 1U);
	fixture.ExpectStrictPoseForEveryTextRun(wrapped);

	// The same pose can use one transform when wrapping is explicitly off.
	// This distinguishes the raw-scale wrap guard from a candidate that was
	// already too inaccurate to use the optional simplification at all.
	fixture.line->Text = R"({\q2\an7\pos(0,0)}zoom)";
	auto const unwrapped = fixture.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, unwrapped.status) << unwrapped.message;
	ASSERT_EQ(1U, unwrapped.event_count);
	ASSERT_EQ(1U, unwrapped.lines.size());
	ASSERT_EQ(1U, unwrapped.lines.front().parts.size());
	EXPECT_LE(PoseTransformCount(unwrapped.lines.front().parts.front().text), 1U);
	EXPECT_EQ(fixture.input.samples.size(), fixture.FrameErrors(unwrapped).size());
}

TEST(motion_track_compact_pixel, rounded_event_checks_visible_frame_after_its_last_position_knot) {
	PixelFixture fixture(30.0);
	fixture.input.timecodes = agi::vfr::Framerate({0, 10, 20, 29, 33, 38, 43, 50, 60, 70, 80});
	fixture.input.direction_domain = fixture.input.decode_interval = {.first = 0, .last = 9};
	fixture.input.video_frame_count = 10;
	fixture.line->End = 80;
	fixture.input.samples.clear();
	std::vector<double> const scales{100.0, 100.4, 99.6, 100.2, 100.4, 101.0, 101.0, 101.0, 101.0, 101.0};
	for (int frame = 0; frame <= 9; ++frame) {
		TrackSample sample;
		sample.frame = frame;
		sample.status = TrackStatus::Ok;
		double const time = fixture.input.timecodes.TimeAtFrame(frame);
		sample.center_x = time <= 33.0 ? 0.3 * time : 9.9 + 0.01 * (time - 33.0);
		SetPose(sample, scales[frame], 0.0);
		fixture.input.samples.push_back(sample);
	}

	auto const plan = fixture.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
	ASSERT_EQ(2U, plan.event_count);
	ASSERT_EQ(1U, plan.lines.size());
	ASSERT_EQ(2U, plan.lines.front().parts.size());
	auto const& first = plan.lines.front().parts.front();
	EXPECT_EQ(36, first.end_ms);
	EXPECT_EQ(40, static_cast<int>(agi::Time(first.end_ms)));
	EXPECT_EQ(first.text, fixture.EventAt(plan, 38).Text.get());
	// A constant 100% pose is close enough to all knots through 33 ms, but
	// not the 101% sample at 38 ms that ASS's 40 ms end still displays.
	// The legal single linear transform retains the final 100.4% endpoint.
	EXPECT_EQ(1U, PoseTransformCount(first.text)) << first.text;
	EXPECT_EQ(fixture.input.samples.size(), fixture.FrameErrors(plan).size());
}
