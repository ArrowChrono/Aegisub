#include <main.h>

#include "../../src/visual_tool_motion_track_shared.h"
#include "../../src/motion_track/apply_plan.h"

#include <limits>
#include <utility>

namespace {
using namespace aegisub::motion_track;
using namespace aegisub::visual_motion_track;

OverlaySnapshot Frame() {
	OverlaySnapshot result;
	result.identity = {.source_id = 17, .generation = 3, .frame = 10};
	result.viewport = {.origin = {10, 20}, .extent = {640, 480}, .storage_width = 640, .storage_height = 480};
	result.roi = RoiRect{.x = 100, .y = 80, .w = 120, .h = 80};
	return result;
}

void ExpectRoi(RoiRect const& value, int x, int y, int width, int height) {
	EXPECT_EQ(x, value.x);
	EXPECT_EQ(y, value.y);
	EXPECT_EQ(width, value.w);
	EXPECT_EQ(height, value.h);
}

void Publish(OverlaySnapshot& frame, InteractionResult const& result) {
	ASSERT_TRUE(result.edit);
	frame.roi = result.edit->roi;
}

OverlaySnapshot RidingFrame(double center_x) {
	auto frame = Frame();
	auto track = std::make_shared<MotionTrackSnapshot>();
	track->model = TrackModel::Translation;
	track->samples.push_back({.frame = 10, .status = TrackStatus::Ok, .center_x = center_x, .center_y = 100});
	frame.track = track;
	frame.anchor = {.valid = true, .frame = 5, .center_x = 100, .center_y = 100};
	return frame;
}
}

TEST(visual_tool_motion_track_shared, projective_pose_maps_known_point_and_round_trips) {
	TrackSample ride{.frame = 10, .model = TrackModel::Homography, .status = TrackStatus::Ok, .center_x = 50, .center_y = 60};
	ride.transform.matrix = {1.2, .1, 0, .2, 1.1, 0, .001, .002, 1};
	RoiRideAnchor anchor{.valid = true, .frame = 2};
	auto const pose = PoseFromAnchorRide(ride, anchor, TrackModel::Homography);
	ASSERT_TRUE(pose.valid);
	EXPECT_FALSE(pose.identity);
	auto const storage = PoseToStorage(pose, {10, 20});
	ASSERT_TRUE(storage);
	EXPECT_NEAR(50 + 14 / 1.05, storage->X(), 1e-5);
	EXPECT_NEAR(60 + 24 / 1.05, storage->Y(), 1e-5);
	auto const restored = PoseToAnchor(pose, *storage);
	ASSERT_TRUE(restored);
	EXPECT_NEAR(10, restored->X(), 1e-5);
	EXPECT_NEAR(20, restored->Y(), 1e-5);
	EXPECT_TRUE(PoseSupportsRoi(pose, {0, 0, 80, 40}));
}

TEST(visual_tool_motion_track_shared, projective_pole_and_singular_anchor_are_rejected) {
	EditPose pose;
	pose.identity = false;
	pose.forward.matrix = {1, 0, 0, 0, 1, 0, .01, 0, -1};
	EXPECT_FALSE(PoseSupportsRoi(pose, {90, 20, 20, 30}));
	EXPECT_FALSE(PoseToStorage(pose, {100, 20}));
	TrackSample ride{.status = TrackStatus::Ok};
	RoiRideAnchor anchor{.valid = true, .m00 = 0, .m11 = 0};
	auto const singular = PoseFromAnchorRide(ride, anchor, TrackModel::Homography);
	EXPECT_FALSE(singular.valid);
	EXPECT_FALSE(PoseToStorage(singular, {20, 30}));
	EXPECT_FALSE(PoseToAnchor(singular, {20, 30}));
}

TEST(visual_tool_motion_track_shared, viewport_mapping_respects_letterbox_and_rejects_invalid_geometry) {
	auto frame = Frame();
	frame.viewport.extent = {320, 240};
	auto const point = CanvasToStorage(frame.viewport, {170, 140});
	ASSERT_TRUE(point);
	EXPECT_EQ(Vector2D(320, 240), *point);
	auto const bar = CanvasToStorage(frame.viewport, {5, 20});
	ASSERT_TRUE(bar);
	EXPECT_EQ(Vector2D(-10, 0), *bar);
	frame.viewport.extent = {0, 240};
	EXPECT_FALSE(CanvasToStorage(frame.viewport, {170, 140}));
	frame = Frame();
	frame.viewport.origin = {std::numeric_limits<float>::quiet_NaN(), 20.f};
	EXPECT_FALSE(CanvasToStorage(frame.viewport, {170, 140}));
	frame = Frame();
	EXPECT_FALSE(CanvasToStorage(frame.viewport, {std::numeric_limits<float>::infinity(), 140.f}));
}

TEST(visual_tool_motion_track_shared, hit_test_prioritizes_all_grips_before_interior) {
	RoiRect const roi{.x = 100, .y = 80, .w = 120, .h = 80};
	std::pair<Vector2D, RoiGrab> const cases[] = {
		{{100, 80}, RoiGrab::Nw}, {{160, 80}, RoiGrab::N}, {{220, 80}, RoiGrab::Ne}, {{220, 120}, RoiGrab::E}, {{220, 160}, RoiGrab::Se}, {{160, 160}, RoiGrab::S}, {{100, 160}, RoiGrab::Sw}, {{100, 120}, RoiGrab::W}, {{160, 120}, RoiGrab::Move}, {{160, 60}, RoiGrab::None}};
	for (auto const& [point, expected] : cases)
		EXPECT_EQ(expected, HitTest(point, roi, 5, 5));
	EXPECT_EQ(RoiGrab::Nw, HitTest({104, 84}, {100, 80, 8, 8}, 5, 5));
	EXPECT_EQ(RoiGrab::N, HitTest({160, 72}, roi, 10, 10));
}

TEST(visual_tool_motion_track_shared, band_release_applies_terminal_point_without_motion) {
	auto frame = Frame();
	RoiController control;
	auto const press = control.Press(frame, {310, 220});
	ASSERT_TRUE(press.handled);
	ASSERT_TRUE(press.edit);
	ExpectRoi(press.edit->roi, 300, 200, 8, 8);
	EXPECT_TRUE(press.edit->reanchor);
	Publish(frame, press);
	auto const release = control.Release(frame, {410, 320});
	ASSERT_TRUE(release.edit);
	ExpectRoi(release.edit->roi, 300, 200, 100, 100);
	EXPECT_FALSE(control.Active());
}

TEST(visual_tool_motion_track_shared, move_and_crossing_resize_use_last_release_point) {
	auto frame = Frame();
	RoiController control;
	ASSERT_TRUE(control.Press(frame, {170, 140}).handled);
	auto const move = control.Move(frame, {210, 180});
	Publish(frame, move);
	auto const release = control.Release(frame, {260, 220});
	ASSERT_TRUE(release.edit);
	ExpectRoi(release.edit->roi, 190, 160, 120, 80);
	frame = Frame();
	ASSERT_TRUE(control.Press(frame, {230, 180}).handled);
	Publish(frame, control.Move(frame, {260, 210}));
	auto const crossed = control.Release(frame, {90, 70});
	ASSERT_TRUE(crossed.edit);
	ExpectRoi(crossed.edit->roi, 80, 50, 20, 30);
	EXPECT_FALSE(control.Active());
}

TEST(visual_tool_motion_track_shared, same_point_release_does_not_snap_a_nearby_grip) {
	auto frame = Frame();
	RoiController control;
	ASSERT_TRUE(control.Press(frame, {228, 178}).handled);
	EXPECT_EQ(RoiGrab::Se, control.Grab());
	auto const release = control.Release(frame, {228, 178});
	EXPECT_TRUE(release.handled);
	EXPECT_FALSE(release.edit);
	ExpectRoi(*frame.roi, 100, 80, 120, 80);
	EXPECT_FALSE(control.Active());
}

TEST(visual_tool_motion_track_shared, captured_pose_survives_new_frame_and_tracking_publication) {
	auto frame = RidingFrame(200);
	RoiController control;
	ASSERT_TRUE(control.Press(frame, {270, 140}).handled);
	EXPECT_EQ(RoiGrab::Move, control.Grab());
	auto const captured = control.CapturedPose();
	ASSERT_TRUE(captured);
	frame = RidingFrame(350);
	frame.identity.frame = 11;
	frame.anchor.center_x = 120;
	auto const move = control.Move(frame, {290, 150});
	ASSERT_TRUE(move.edit);
	ExpectRoi(move.edit->roi, 120, 90, 120, 80);
	EXPECT_FALSE(move.edit->reanchor);
	EXPECT_EQ(captured->forward.matrix, control.CapturedPose()->forward.matrix);
}

TEST(visual_tool_motion_track_shared, off_frame_existing_grip_is_editable_but_empty_bar_is_not) {
	auto frame = RidingFrame(0);
	frame.roi = RoiRect{.x = 20, .y = 80, .w = 40, .h = 40};
	RoiController control;
	ASSERT_TRUE(control.Press(frame, {-70, 100}).handled);
	EXPECT_EQ(RoiGrab::Nw, control.Grab());
	auto const resize = control.Release(frame, {-90, 90});
	ASSERT_TRUE(resize.edit);
	ExpectRoi(resize.edit->roi, 0, 70, 60, 50);
	EXPECT_FALSE(resize.edit->reanchor);
	frame = Frame();
	EXPECT_FALSE(control.Press(frame, {5, 200}).handled);
	EXPECT_FALSE(control.Active());
}

TEST(visual_tool_motion_track_shared, clamp_uses_storage_for_identity_and_anchor_space_for_pose) {
	auto frame = Frame();
	RoiController control;
	ASSERT_TRUE(control.Press(frame, {170, 140}).handled);
	auto const clamped = control.Release(frame, {-100, -100});
	ASSERT_TRUE(clamped.edit);
	ExpectRoi(clamped.edit->roi, 0, 0, 120, 80);
	frame = RidingFrame(200);
	ASSERT_TRUE(control.Press(frame, {270, 140}).handled);
	auto const unbounded_origin = control.Release(frame, {-100, -100});
	ASSERT_TRUE(unbounded_origin.edit);
	ExpectRoi(unbounded_origin.edit->roi, -270, -160, 120, 80);
	frame = Frame();
	ASSERT_TRUE(control.Press(frame, {230, 180}).handled);
	auto const capped = control.Release(frame, {1000, 1000});
	ASSERT_TRUE(capped.edit);
	ExpectRoi(capped.edit->roi, 100, 0, 512, 480);
}

TEST(visual_tool_motion_track_shared, capture_loss_preserves_live_roi_and_ignores_later_release) {
	auto frame = Frame();
	RoiController control;
	ASSERT_TRUE(control.Press(frame, {170, 140}).handled);
	Publish(frame, control.Move(frame, {185, 155}));
	control.CaptureLost();
	EXPECT_FALSE(control.Active());
	EXPECT_FALSE(control.Release(frame, {410, 320}).handled);
	EXPECT_FALSE(control.Move(frame, {440, 350}).edit);
	ExpectRoi(*frame.roi, 115, 95, 120, 80);
}

TEST(visual_tool_motion_track_shared, source_replacement_or_invalid_mapping_ends_without_edit) {
	auto frame = Frame();
	RoiController control;
	ASSERT_TRUE(control.Press(frame, {170, 140}).handled);
	frame.identity.generation++;
	auto const replaced = control.Move(frame, {190, 160});
	EXPECT_TRUE(replaced.handled);
	EXPECT_FALSE(replaced.edit);
	EXPECT_FALSE(control.Active());
	frame = Frame();
	ASSERT_TRUE(control.Press(frame, {170, 140}).handled);
	frame.viewport.storage_width = 0;
	auto const invalid = control.Release(frame, {190, 160});
	EXPECT_FALSE(invalid.edit);
	EXPECT_FALSE(control.Active());
}

TEST(visual_tool_motion_track_shared, plan_preview_keeps_covered_knots_and_last_covered_endpoint) {
	MotionTrackApplyPlan plan;
	plan.status = ApplyPlanStatus::Ok;
	plan.lines = {
		{.parts = {{.covered = false, .x0 = -100, .y0 = -100, .x1 = -50, .y1 = -50},
				   {.x0 = 10, .y0 = 20, .x1 = 30, .y1 = 40},
				   {.x0 = 50, .y0 = 60, .x1 = 70, .y1 = 80},
				   {.covered = false, .x0 = 90, .y0 = 90, .x1 = 100, .y1 = 100}}},
		{.parts = {{.covered = false, .x0 = -200, .y0 = -200}}},
		{.parts = {{.x0 = 8, .y0 = 12, .x1 = 16, .y1 = 24}}}};
	auto const preview = BuildPlanPreview(plan, 1280, 240, 640, 480);
	ASSERT_TRUE(preview);
	ASSERT_EQ(2u, preview->paths.size());
	ASSERT_EQ(3u, preview->paths[0].size());
	EXPECT_FLOAT_EQ(20, preview->paths[0][0].x);
	EXPECT_FLOAT_EQ(10, preview->paths[0][0].y);
	EXPECT_FLOAT_EQ(100, preview->paths[0][1].x);
	EXPECT_FLOAT_EQ(30, preview->paths[0][1].y);
	EXPECT_FLOAT_EQ(140, preview->paths[0][2].x);
	EXPECT_FLOAT_EQ(40, preview->paths[0][2].y);
	ASSERT_EQ(2u, preview->paths[1].size());
	EXPECT_FLOAT_EQ(16, preview->paths[1][0].x);
	EXPECT_FLOAT_EQ(6, preview->paths[1][0].y);
	EXPECT_FLOAT_EQ(32, preview->paths[1][1].x);
	EXPECT_FLOAT_EQ(12, preview->paths[1][1].y);
	EXPECT_DOUBLE_EQ(70, plan.lines[0].parts[2].x1);
}

TEST(visual_tool_motion_track_shared, plan_preview_rejects_invalid_dimensions_or_non_mutating_plan) {
	MotionTrackApplyPlan plan;
	plan.status = ApplyPlanStatus::NeedsConfirmation;
	plan.lines = {{.parts = {{.x0 = 10, .y0 = 20, .x1 = 30, .y1 = 40}}}};
	EXPECT_TRUE(BuildPlanPreview(plan, 640, 480, 640, 480));
	EXPECT_FALSE(BuildPlanPreview(plan, 0, 480, 640, 480));
	EXPECT_FALSE(BuildPlanPreview(plan, 640, -1, 640, 480));
	EXPECT_FALSE(BuildPlanPreview(plan, 640, 480, 0, 480));
	EXPECT_FALSE(BuildPlanPreview(plan, 640, 480, 640, -1));
	plan.status = ApplyPlanStatus::IncompleteCoverage;
	EXPECT_FALSE(BuildPlanPreview(plan, 640, 480, 640, 480));
}
