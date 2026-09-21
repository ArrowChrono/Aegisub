#include <main.h>

#include "../../src/motion_track/apply_plan.h"
#include "../../src/motion_track/geometry_apply.h"
#include "../../src/perspective_ass_state.h"

#include <ass_dialogue.h>
#include <ass_file.h>
#include <ass_style.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace aegisub::motion_track;

struct CompactFixture {
	AssFile file;
	AssDialogue *line = nullptr;
	ApplyPlanInput input;

	explicit CompactFixture(std::vector<int> times) {
		auto *style = new AssStyle;
		style->name = "Default";
		file.Styles.push_back(*style);
		line = new AssDialogue;
		line->Start = 0;
		line->End = times.back() + 10;
		line->Text = R"({\pos(0,0)}track)";
		file.Events.push_back(*line);
		input.storage_width = input.script_width = 1920;
		input.storage_height = input.script_height = 1080;
		input.video_frame_count = static_cast<int>(times.size());
		input.direction_domain = input.decode_interval =
			FrameInterval{.first = 0, .last = input.video_frame_count - 1};
		input.timecodes = agi::vfr::Framerate(std::move(times));
		input.options.compact_epsilon = 0.01;
		input.options.position_decimals = 4;
		for (int frame = 0; frame < input.video_frame_count; ++frame) {
			TrackSample sample;
			sample.frame = frame;
			sample.status = TrackStatus::Ok;
			input.samples.push_back(sample);
		}
	}

	[[nodiscard]] MotionTrackApplyPlan Build() const {
		return BuildApplyPlan(file, {line}, input);
	}

	[[nodiscard]] perspective::AssStateResult AtFrame(MotionTrackApplyPlan const& plan,
													  int frame) const {
		int const time = input.timecodes.TimeAtFrame(frame);
		for (auto const& part : plan.lines.front().parts) {
			int const start = static_cast<int>(agi::Time(part.start_ms));
			int const end = static_cast<int>(agi::Time(part.end_ms));
			if (part.covered && time >= start && time < end) {
				AssDialogue emitted;
				emitted.Start = start;
				emitted.End = end;
				emitted.Text = part.text;
				return perspective::EvaluateEffectiveAssState({.file = &file, .line = &emitted, .play_resolution = {.width = static_cast<double>(input.script_width), .height = static_cast<double>(input.script_height)}, .capture_time_ms = time});
			}
		}
		ADD_FAILURE() << "No emitted part covers frame " << frame;
		perspective::AssStateResult result;
		result.error = perspective::AssStateError::InvalidCaptureTime;
		return result;
	}

	void ExpectStorageError(MotionTrackApplyPlan const& plan, double tolerance) const {
		ASSERT_EQ(1U, plan.lines.size());
		TrackSample const *expected = nullptr;
		for (auto const& sample : input.samples) {
			SCOPED_TRACE(sample.frame);
			if (sample.status == TrackStatus::Ok)
				expected = &sample;
			ASSERT_NE(nullptr, expected);
			auto const state = AtFrame(plan, sample.frame);
			ASSERT_TRUE(state) << perspective::DescribeAssStateError(state.error);
			double const dx = state.value.transform.position.x * input.storage_width /
								  input.script_width -
							  expected->center_x;
			double const dy = state.value.transform.position.y * input.storage_height /
								  input.script_height -
							  expected->center_y;
			EXPECT_LE(std::hypot(dx, dy), tolerance);
		}
	}
};
}

TEST(motion_track_compact, vfr_constant_velocity_emits_one_time_linear_move) {
	CompactFixture fx({0, 100, 300, 350, 650, 700});
	for (auto& sample : fx.input.samples) {
		double const time = fx.input.timecodes.TimeAtFrame(sample.frame);
		sample.center_x = 0.2 * time;
		sample.center_y = -0.07 * time;
	}

	auto const plan = fx.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	ASSERT_EQ(1u, plan.lines.size());
	ASSERT_EQ(1u, plan.event_count);
	ASSERT_EQ(1u, plan.lines.front().parts.size());
	auto const& part = plan.lines.front().parts.front();
	EXPECT_EQ(0, part.start_ms);
	EXPECT_EQ(710, part.end_ms);
	EXPECT_NE(std::string::npos,
			  part.text.find(R"(\move(0.0000,0.0000,140.0000,-49.0000,0,700))"));
	for (auto const& sample : fx.input.samples) {
		auto const state = fx.AtFrame(plan, sample.frame);
		ASSERT_TRUE(state) << perspective::DescribeAssStateError(state.error);
		EXPECT_NEAR(sample.center_x, state.value.transform.position.x, 1e-6);
		EXPECT_NEAR(sample.center_y, state.value.transform.position.y, 1e-6);
	}
}

TEST(motion_track_compact, feasible_single_segment_respects_coordinate_precision) {
	for (int const decimals : {2, 0}) {
		SCOPED_TRACE(decimals);
		CompactFixture fx({0, 100, 200});
		fx.input.options.compact_epsilon = 0.75;
		fx.input.options.position_decimals = decimals;
		fx.input.samples[1].center_y = 1.4;
		// Least squares picks y=1.4/3, whose middle residual is 0.933...
		// A constant y=0.7 satisfies the actual 0.75 maximum-error budget.
		// With knot-aligned move windows, integer coordinates cannot
		// serialize a feasible single segment.
		auto const plan = fx.Build();
		ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
		EXPECT_EQ(decimals == 2 ? 1U : 2U, plan.event_count);
		fx.ExpectStorageError(plan, 0.75);
	}
}

TEST(motion_track_compact, feasible_vfr_2d_move_uses_nonuniform_storage_mapping) {
	CompactFixture fx({0, 73, 300});
	fx.input.script_width = 960;
	fx.input.script_height = 270;
	fx.input.options.compact_epsilon = 0.75;
	for (auto& sample : fx.input.samples) {
		double const time = fx.input.timecodes.TimeAtFrame(sample.frame);
		double const deviation = sample.frame == 1 ? 1.4 : 0.0;
		sample.center_x = 3.0 + 0.07 * time + 0.6 * deviation;
		sample.center_y = 5.0 - 0.035 * time + 0.8 * deviation;
	}
	// A +0.7*(0.6,0.8) offset from the underlying time-linear motion is
	// feasible in storage pixels. The VFR least-squares peak is about 0.858,
	// so accepting only that fit would unnecessarily split this motion.
	auto const plan = fx.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
	ASSERT_EQ(1U, plan.event_count);
	ASSERT_EQ(1U, plan.lines.size());
	ASSERT_EQ(1U, plan.lines.front().parts.size());
	EXPECT_NE(std::string::npos, plan.lines.front().parts.front().text.find(R"(\move()"));
	fx.ExpectStorageError(plan, 0.75);
}

TEST(motion_track_compact, infeasible_single_segment_keeps_required_split) {
	CompactFixture fx({0, 100, 200});
	fx.input.options.compact_epsilon = 0.75;
	fx.input.samples[1].center_y = 1.6;
	// A line's midpoint is the mean of its endpoints; these observations
	// require at least 0.8 px maximum error for every single-segment fit.
	auto const plan = fx.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
	EXPECT_EQ(2U, plan.event_count);
	fx.ExpectStorageError(plan, 0.75);
}

TEST(motion_track_compact, feasible_runs_do_not_replace_failed_hold_with_fitted_endpoint) {
	CompactFixture fx({0, 100, 200, 300, 400, 500, 600});
	fx.input.options.compact_epsilon = 0.75;
	fx.input.samples[1].center_y = 1.4;
	fx.input.samples[3].status = TrackStatus::Failed;
	fx.input.samples[3].center_y = 999.0;
	fx.input.samples[4].center_y = 0.2;
	fx.input.samples[5].center_y = 1.6;
	fx.input.samples[6].center_y = 0.2;
	auto const plan = fx.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
	ASSERT_EQ(3U, plan.event_count);
	ASSERT_EQ(1U, plan.lines.size());
	ASSERT_EQ(3U, plan.lines.front().parts.size());
	EXPECT_EQ(250, plan.lines.front().parts[1].start_ms);
	EXPECT_EQ(350, plan.lines.front().parts[1].end_ms);
	auto const held = fx.AtFrame(plan, 3);
	ASSERT_TRUE(held) << perspective::DescribeAssStateError(held.error);
	EXPECT_DOUBLE_EQ(0.0, held.value.transform.position.x);
	EXPECT_DOUBLE_EQ(0.0, held.value.transform.position.y);
	fx.ExpectStorageError(plan, 0.75);
}

TEST(motion_track_compact, default_one_pixel_budget_accepts_motion_rejected_at_three_quarters) {
	CompactFixture fx({0, 100, 200});
	fx.input.options = ApplyPlanOptions{};
	ASSERT_DOUBLE_EQ(1.0, fx.input.options.compact_epsilon);
	fx.input.samples[1].center_y = 1.8;
	auto const default_plan = fx.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, default_plan.status) << default_plan.message;
	EXPECT_EQ(1U, default_plan.event_count);
	fx.ExpectStorageError(default_plan, 1.0);

	fx.input.options.compact_epsilon = 0.75;
	auto const explicit_plan = fx.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, explicit_plan.status) << explicit_plan.message;
	EXPECT_EQ(2U, explicit_plan.event_count);
	fx.ExpectStorageError(explicit_plan, 0.75);
}

TEST(motion_track_compact, vfr_pose_subdivision_preserves_fitted_position) {
	CompactFixture fx({0, 100, 300, 350, 650, 700});
	fx.input.model = TrackModel::Similarity;
	for (auto& sample : fx.input.samples) {
		double const time = fx.input.timecodes.TimeAtFrame(sample.frame);
		sample.center_x = 0.2 * time;
		sample.center_y = -0.07 * time;
		double const angle = (sample.frame % 2 ? 25.0 : 0.0) *
							 std::numbers::pi / 180.0;
		double const scale = 1.0 + 0.0025 * sample.frame * sample.frame;
		sample.transform.matrix[0] = scale * std::cos(angle);
		sample.transform.matrix[1] = -scale * std::sin(angle);
		sample.transform.matrix[3] = scale * std::sin(angle);
		sample.transform.matrix[4] = scale * std::cos(angle);
	}

	auto const plan = fx.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	ASSERT_EQ(1u, plan.lines.size());
	// Alternating angles still need pose knots, not separate subtitle events.
	ASSERT_EQ(1u, plan.event_count);
	ASSERT_EQ(1u, plan.lines.front().parts.size());
	for (auto const& sample : fx.input.samples) {
		auto const state = fx.AtFrame(plan, sample.frame);
		ASSERT_TRUE(state) << perspective::DescribeAssStateError(state.error);
		EXPECT_NEAR(sample.center_x, state.value.transform.position.x, 0.0001)
			<< "frame " << sample.frame;
		EXPECT_NEAR(sample.center_y, state.value.transform.position.y, 0.0001)
			<< "frame " << sample.frame;
		EXPECT_NEAR(sample.frame % 2 ? -25.0 : 0.0,
					state.value.transform.rotation_z, 0.01);
		double const scale = 100.0 + 0.25 * sample.frame * sample.frame;
		EXPECT_NEAR(scale, state.value.transform.scale_x, 0.01);
		EXPECT_NEAR(scale, state.value.transform.scale_y, 0.01);
	}
	EXPECT_EQ(0, plan.lines.front().parts.front().start_ms);
	EXPECT_EQ(710, plan.lines.front().parts.front().end_ms);
}

TEST(motion_track_compact, subpixel_motion_survives_position_serialization) {
	CompactFixture fx({0, 100, 200, 300, 400});
	for (auto& sample : fx.input.samples)
		sample.center_x = 0.1 * sample.frame;

	auto const plan = fx.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	ASSERT_EQ(1u, plan.lines.size());
	ASSERT_EQ(1u, plan.event_count);
	EXPECT_NE(std::string::npos,
			  plan.lines.front().parts.front().text.find(R"(\move()"));
	for (auto const& sample : fx.input.samples) {
		auto const state = fx.AtFrame(plan, sample.frame);
		ASSERT_TRUE(state) << perspective::DescribeAssStateError(state.error);
		EXPECT_NEAR(sample.center_x, state.value.transform.position.x, 1e-6);
		EXPECT_NEAR(0.0, state.value.transform.position.y, 1e-6);
	}
}

TEST(motion_track_compact, vfr_curve_keeps_storage_error_and_small_partition) {
	std::vector<int> times{0};
	for (int frame = 1; frame <= 96; ++frame)
		times.push_back(times.back() + (frame % 3 == 0 ? 160 : 40));
	CompactFixture fx(std::move(times));
	fx.input.script_width = 960;
	fx.input.script_height = 270;
	fx.input.options.compact_epsilon = 0.5;
	for (auto& sample : fx.input.samples) {
		double const time = fx.input.timecodes.TimeAtFrame(sample.frame);
		sample.center_x = 0.000001 * time * time;
		sample.center_y = 0.0000005 * time * time;
	}

	auto const plan = fx.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status);
	ASSERT_EQ(1u, plan.lines.size());
	EXPECT_GE(plan.event_count, 2u);
	// Eight nearly equal intervals already satisfy the endpoint-interpolant
	// bound for this parabola; fitting must not inflate it towards 97 events.
	EXPECT_LE(plan.event_count, 8u);
	for (auto const& sample : fx.input.samples) {
		auto const state = fx.AtFrame(plan, sample.frame);
		ASSERT_TRUE(state) << perspective::DescribeAssStateError(state.error);
		double const dx = 2.0 * state.value.transform.position.x - sample.center_x;
		double const dy = 4.0 * state.value.transform.position.y - sample.center_y;
		EXPECT_LE(std::hypot(dx, dy), 0.5003) << "frame " << sample.frame;
	}
	auto const& parts = plan.lines.front().parts;
	EXPECT_EQ(0, parts.front().start_ms);
	EXPECT_EQ(int(fx.line->End), parts.back().end_ms);
	for (size_t i = 1; i < parts.size(); ++i)
		EXPECT_EQ(parts[i - 1].end_ms, parts[i].start_ms);
}

TEST(motion_track_compact, invalid_error_budget_produces_no_mutations) {
	for (double epsilon : {-0.01, std::numeric_limits<double>::infinity(),
						   std::numeric_limits<double>::quiet_NaN()}) {
		CompactFixture fx({0, 100, 200});
		fx.input.options.compact_epsilon = epsilon;
		auto const plan = fx.Build();
		EXPECT_EQ(ApplyPlanStatus::InvalidInput, plan.status);
		EXPECT_TRUE(plan.lines.empty());
		EXPECT_EQ(0u, plan.event_count);
		EXPECT_FALSE(plan.message.empty());
	}
	CompactFixture exact_fit({0, 100, 200});
	exact_fit.input.options.compact_epsilon = 0.0;
	auto const valid = exact_fit.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, valid.status);
	EXPECT_EQ(1u, valid.event_count);
}

TEST(motion_track_compact, duplicate_frame_time_rejects_all_target_mutations) {
	CompactFixture fx({0, 100, 200, 200, 300});
	fx.line->End = 110;
	auto *second = new AssDialogue;
	second->Start = 100;
	second->End = 310;
	second->Text = R"({\pos(10,20)}second)";
	fx.file.Events.push_back(*second);
	// The first target is valid and plans successfully before the second
	// target encounters two different frames at the same ASS timestamp.
	auto const first_only = fx.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, first_only.status);
	ASSERT_TRUE(first_only.has_mutations());
	auto const plan = BuildApplyPlan(fx.file, {fx.line, second}, fx.input);
	EXPECT_EQ(ApplyPlanStatus::InvalidInput, plan.status);
	EXPECT_TRUE(plan.lines.empty());
	EXPECT_EQ(0u, plan.event_count);
	EXPECT_FALSE(plan.has_mutations());
	EXPECT_EQ(R"({\pos(0,0)}track)", fx.line->Text.get());
	EXPECT_EQ(R"({\pos(10,20)}second)", second->Text.get());
}

TEST(motion_track_compact, unloaded_timecodes_are_rejected_before_planning) {
	CompactFixture fx({0, 100, 200});
	fx.input.timecodes = agi::vfr::Framerate{};
	auto const plan = fx.Build();
	EXPECT_EQ(ApplyPlanStatus::InvalidInput, plan.status);
	EXPECT_TRUE(plan.lines.empty());
	EXPECT_EQ(0u, plan.event_count);
}

TEST(motion_track_compact, position_precision_obeys_supported_boundaries) {
	for (int decimals : {-1, 0, 6, 7}) {
		CompactFixture fx({0, 100, 200});
		fx.input.options.position_decimals = decimals;
		auto const plan = fx.Build();
		bool const supported = decimals == 0 || decimals == 6;
		EXPECT_EQ(supported ? ApplyPlanStatus::Ok : ApplyPlanStatus::InvalidInput,
				  plan.status)
			<< "decimals " << decimals;
		EXPECT_EQ(supported ? 1u : 0u, plan.event_count);
		EXPECT_EQ(supported, plan.has_mutations());
	}
}

TEST(motion_track_compact, nested_geometry_is_routed_to_geometry_validation) {
	for (auto const *text : {R"({\t(0,100,\frx30)\pos(0,0)}x)",
							 R"({\t(\org(50,50))\pos(0,0)}x)",
							 R"({\t(0,100,\ fax0.3)\pos(0,0)}x)",
							 R"({\t(\t(\fay0.1))\pos(0,0)}x)",
							 R"({\t(0,100,\ fry30)\pos(0,0)}x)",
							 R"({\t(0,100,\ clip(0,0,100,100))\pos(0,0)}x)",
							 R"({\t(0,100,\ iclip(0,0,100,100))\pos(0,0)}x)"}) {
		AssDialogue line;
		line.Text = text;
		EXPECT_TRUE(NeedsGeometryApply(line, TrackModel::Translation)) << text;
	}
	AssDialogue opacity;
	opacity.Text = R"({\t(0,100,\alpha&HFF&)\pos(0,0)}x)";
	EXPECT_FALSE(NeedsGeometryApply(opacity, TrackModel::Translation));
}

TEST(motion_track_compact, vfr_translation_uses_serialized_start_after_hold) {
	CompactFixture fx({0, 41, 88, 127, 173, 217, 263, 310, 349, 394,
					   441, 489, 535, 578, 619, 667, 712, 759, 805, 853});
	for (auto& sample : fx.input.samples) {
		double const time = fx.input.timecodes.TimeAtFrame(sample.frame);
		sample.center_x = 0.1 * time;
		sample.center_y = -0.05 * time;
	}
	fx.input.samples[4].status = TrackStatus::Failed;
	auto const plan = fx.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, plan.status) << plan.message;
	ASSERT_EQ(1u, plan.lines.size());
	ASSERT_EQ(3u, plan.event_count);
	auto const& parts = plan.lines.front().parts;
	ASSERT_EQ(3u, parts.size());
	EXPECT_EQ(195, parts.back().start_ms);
	EXPECT_EQ(200, static_cast<int>(agi::Time(parts.back().start_ms)));
	EXPECT_EQ(parts[1].end_ms, parts.back().start_ms);
	EXPECT_NE(std::string::npos, parts.back().text.find(R"(\move()"));
	for (auto const& sample : fx.input.samples) {
		auto const state = fx.AtFrame(plan, sample.frame);
		ASSERT_TRUE(state) << perspective::DescribeAssStateError(state.error);
		auto const& expected = sample.frame == 4 ? fx.input.samples[3] : sample;
		double const error = std::hypot(state.value.transform.position.x - expected.center_x,
										state.value.transform.position.y - expected.center_y);
		EXPECT_LE(error, fx.input.options.compact_epsilon) << "frame " << sample.frame;
	}
}

TEST(motion_track_compact, insufficient_position_precision_rejects_all_mutations) {
	CompactFixture fx({0, 100, 200});
	fx.input.options.position_decimals = 0;
	fx.line->Text = R"({\pos(0.6,0)}first)";
	for (auto& sample : fx.input.samples)
		sample.center_x = 0.4;
	auto *second = new AssDialogue;
	second->Start = 0;
	second->End = 210;
	second->Text = R"({\pos(0,0)}second)";
	fx.file.Events.push_back(*second);
	// The first target lands exactly at x=1. The second needs x=0.4, which
	// cannot be represented at integer precision within the 0.01 px budget.
	auto const first_only = fx.Build();
	ASSERT_EQ(ApplyPlanStatus::Ok, first_only.status) << first_only.message;
	ASSERT_TRUE(first_only.has_mutations());
	auto const rejected = BuildApplyPlan(fx.file, {fx.line, second}, fx.input);
	EXPECT_EQ(ApplyPlanStatus::InvalidInput, rejected.status);
	EXPECT_FALSE(rejected.has_mutations());
	EXPECT_TRUE(rejected.lines.empty());
	EXPECT_EQ(0u, rejected.event_count);
	EXPECT_NE(std::string::npos, rejected.message.find("Position decimals"));
	EXPECT_EQ(R"({\pos(0.6,0)}first)", fx.line->Text.get());
	EXPECT_EQ(R"({\pos(0,0)}second)", second->Text.get());

	fx.input.options.position_decimals = 1;
	auto const precise = BuildApplyPlan(fx.file, {fx.line, second}, fx.input);
	ASSERT_EQ(ApplyPlanStatus::Ok, precise.status) << precise.message;
	ASSERT_EQ(2u, precise.lines.size());
	EXPECT_EQ(2u, precise.event_count);
	EXPECT_NE(std::string::npos, precise.lines[1].parts.front().text.find(R"(\pos(0.4,0.0))"));
}
