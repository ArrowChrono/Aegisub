#include <main.h>

#include "../../src/source_frame.h"
#include "../../src/video_display_layout.h"
#include "../../src/video_display_frame_policy.h"
#include "../../src/deadline_pacing_policy.h"
#include "../../src/video_render_geometry.h"
#include "../../src/video_frame.h"

TEST(video_display_layout, fixed_size_uses_exact_video_rect_and_bottom_origin) {
	auto layout = BuildVideoDisplayViewportLayout(1920, 1080, 1280, 720, false);
	EXPECT_EQ(0, layout.viewport_left);
	EXPECT_EQ(1280, layout.viewport_width);
	EXPECT_EQ(360, layout.viewport_bottom);
	EXPECT_EQ(0, layout.viewport_top);
	EXPECT_EQ(720, layout.viewport_height);
}

TEST(video_display_layout, free_size_letterboxes_left_right_for_tall_target_aspect) {
	auto layout = BuildVideoDisplayViewportLayout(200, 200, 200, 200, true, 3.0 / 4.0);
	EXPECT_EQ(25, layout.viewport_left);
	EXPECT_EQ(150, layout.viewport_width);
	EXPECT_EQ(0, layout.viewport_bottom);
	EXPECT_EQ(0, layout.viewport_top);
	EXPECT_EQ(200, layout.viewport_height);
}

TEST(video_display_layout, free_size_letterboxes_top_bottom_for_wide_target_aspect) {
	auto layout = BuildVideoDisplayViewportLayout(200, 200, 200, 200, true, 16.0 / 9.0);
	EXPECT_EQ(0, layout.viewport_left);
	EXPECT_EQ(200, layout.viewport_width);
	EXPECT_EQ(43, layout.viewport_bottom);
	EXPECT_EQ(43, layout.viewport_top);
	EXPECT_EQ(113, layout.viewport_height);
}

TEST(video_display_layout, attached_content_layout_defaults_to_base_viewport_without_transform) {
	VideoDisplayViewportLayout base = { 10, 320, 40, 20, 180 };
	auto content = BuildVideoDisplayContentLayout(base, 240, true);
	EXPECT_EQ(base.viewport_left, content.viewport_left);
	EXPECT_EQ(base.viewport_width, content.viewport_width);
	EXPECT_EQ(base.viewport_bottom, content.viewport_bottom);
	EXPECT_EQ(base.viewport_top, content.viewport_top);
	EXPECT_EQ(base.viewport_height, content.viewport_height);
}

TEST(video_display_layout, attached_content_layout_scales_around_base_viewport_center) {
	VideoDisplayViewportLayout base = { 10, 320, 40, 20, 180 };
	auto content = BuildVideoDisplayContentLayout(base, 240, true, { 1.5, 0.0, 0.0 });
	EXPECT_EQ(-70, content.viewport_left);
	EXPECT_EQ(480, content.viewport_width);
	EXPECT_EQ(-25, content.viewport_top);
	EXPECT_EQ(270, content.viewport_height);
	EXPECT_EQ(-5, content.viewport_bottom);
}

TEST(video_display_layout, attached_content_layout_clamps_pan_using_viewport_height_units) {
	VideoDisplayViewportLayout base = { 10, 320, 40, 20, 180 };
	auto content = BuildVideoDisplayContentLayout(base, 240, true, { 2.0, 5.0, -5.0 });
	EXPECT_EQ(242, content.viewport_left);
	EXPECT_EQ(640, content.viewport_width);
	EXPECT_EQ(-322, content.viewport_top);
	EXPECT_EQ(360, content.viewport_height);
	EXPECT_EQ(202, content.viewport_bottom);
}

TEST(video_display_layout, detached_content_layout_stays_equal_to_base_viewport_when_disabled) {
	VideoDisplayViewportLayout base = { 0, 200, 43, 43, 113 };
	auto content = BuildVideoDisplayContentLayout(base, 199, false, { 3.0, 1.0, -1.0 });
	EXPECT_EQ(base.viewport_left, content.viewport_left);
	EXPECT_EQ(base.viewport_width, content.viewport_width);
	EXPECT_EQ(base.viewport_bottom, content.viewport_bottom);
	EXPECT_EQ(base.viewport_top, content.viewport_top);
	EXPECT_EQ(base.viewport_height, content.viewport_height);
}

TEST(video_display_layout, detached_content_layout_can_pan_without_resizing_when_zoom_is_unity) {
	VideoDisplayViewportLayout base = { 0, 200, 43, 43, 113 };
	auto content = BuildVideoDisplayContentLayout(base, 199, true, { 1.0, 0.5, -0.25 });
	EXPECT_EQ(57, content.viewport_left);
	EXPECT_EQ(200, content.viewport_width);
	EXPECT_EQ(15, content.viewport_top);
	EXPECT_EQ(113, content.viewport_height);
	EXPECT_EQ(71, content.viewport_bottom);
}

TEST(video_display_layout, zoom_anchor_uses_viewport_center_and_current_pan) {
	VideoDisplayViewportLayout base = { 10, 320, 40, 20, 180 };
	VideoDisplayContentTransform transform = { 1.0, 0.0, 0.0 };
	auto anchor = GetVideoDisplayZoomAnchorPoint(base, transform, Vector2D(210, 110));
	EXPECT_FLOAT_EQ(40.0f, anchor.X());
	EXPECT_FLOAT_EQ(0.0f, anchor.Y());
}

TEST(video_display_layout, zoom_and_pan_keeps_anchor_under_cursor) {
	VideoDisplayViewportLayout base = { 10, 320, 40, 20, 180 };
	VideoDisplayContentTransform transform = { 1.0, 0.0, 0.0 };
	auto anchor = GetVideoDisplayZoomAnchorPoint(base, transform, Vector2D(210, 110));
	auto zoomed = ZoomVideoDisplayContent(base, transform, 2.0, anchor, Vector2D(210, 110));
	EXPECT_DOUBLE_EQ(2.0, zoomed.zoom);
	EXPECT_NEAR(-40.0 / 180.0, zoomed.pan_x, 1e-6);
	EXPECT_DOUBLE_EQ(0.0, zoomed.pan_y);
}

TEST(video_display_layout, pan_video_display_content_uses_viewport_height_units) {
	VideoDisplayViewportLayout base = { 10, 320, 40, 20, 180 };
	VideoDisplayContentTransform transform = { 1.0, 0.0, 0.0 };
	auto panned = PanVideoDisplayContent(base, transform, Vector2D(18, -36));
	EXPECT_NEAR(0.1, panned.pan_x, 1e-6);
	EXPECT_NEAR(-0.2, panned.pan_y, 1e-6);
}

TEST(video_display_layout, content_zoom_clamps_to_supported_range) {
	EXPECT_DOUBLE_EQ(0.125, ClampVideoDisplayContentZoom(0.01));
	EXPECT_DOUBLE_EQ(10.0, ClampVideoDisplayContentZoom(25.0));
}

TEST(video_display_layout, pan_video_display_content_clamps_extreme_offsets) {
	VideoDisplayViewportLayout base = { 0, 200, 0, 0, 100 };
	VideoDisplayContentTransform transform = { 1.0, 0.0, 0.0 };
	auto panned = PanVideoDisplayContent(base, transform, Vector2D(10000, -10000));
	EXPECT_NEAR(1.4, panned.pan_x, 1e-6);
	EXPECT_NEAR(-0.9, panned.pan_y, 1e-6);
}

TEST(video_display_layout, zoom_video_display_content_clamps_extreme_requests) {
	VideoDisplayViewportLayout base = { 0, 200, 0, 0, 100 };
	VideoDisplayContentTransform transform = { 1.0, 0.0, 0.0 };
	auto anchor = GetVideoDisplayZoomAnchorPoint(base, transform, Vector2D(150, 50));

	auto zoomed_in = ZoomVideoDisplayContent(base, transform, 100.0, anchor, Vector2D(150, 50));
	EXPECT_DOUBLE_EQ(10.0, zoomed_in.zoom);
	EXPECT_NEAR(-4.5, zoomed_in.pan_x, 1e-6);
	EXPECT_DOUBLE_EQ(0.0, zoomed_in.pan_y);

	auto zoomed_out = ZoomVideoDisplayContent(base, transform, 0.01, anchor, Vector2D(150, 50));
	EXPECT_DOUBLE_EQ(0.125, zoomed_out.zoom);
	EXPECT_NEAR(0.4375, zoomed_out.pan_x, 1e-6);
	EXPECT_DOUBLE_EQ(0.0, zoomed_out.pan_y);
}

TEST(video_display_layout, resolve_scroll_action_prefers_modifier_specific_options) {
	EXPECT_EQ(SCALE_VIDEO, ResolveVideoDisplayScrollAction(false, false, SCALE_VIDEO, ZOOM_VIDEO, PAN_VIDEO));
	EXPECT_EQ(ZOOM_VIDEO, ResolveVideoDisplayScrollAction(true, false, SCALE_VIDEO, ZOOM_VIDEO, PAN_VIDEO));
	EXPECT_EQ(PAN_VIDEO, ResolveVideoDisplayScrollAction(false, true, SCALE_VIDEO, ZOOM_VIDEO, PAN_VIDEO));
	EXPECT_EQ(NOTHING, ResolveVideoDisplayScrollAction(true, true, SCALE_VIDEO, ZOOM_VIDEO, PAN_VIDEO));
}

TEST(video_display_layout, source_storage_visible_display_and_viewport_spaces_form_explicit_chain) {
	VideoFrame frame;
	frame.width = 12;
	frame.height = 8;
	frame.pitch = 48;
	frame.flipped = false;
	frame.data.resize(384);

	auto source = MakeSourceFrameView(frame, "TV.709");
	source.geometry = MakeDefaultSourceFrameGeometry(12, 8);
	source.geometry.visible_rect = { 2, 1, 8, 6 };
	source.geometry.rotation = 90;
	source.geometry.display_vflip = true;

	auto visible = GetSourceFrameVisibleRect(source);
	EXPECT_EQ(2, visible.x);
	EXPECT_EQ(1, visible.y);
	EXPECT_EQ(8, visible.width);
	EXPECT_EQ(6, visible.height);

	auto canvas = BuildVideoRenderCanvasLayout(source);
	EXPECT_EQ(8, canvas.canvas_width);
	EXPECT_EQ(6, canvas.canvas_height);
	EXPECT_EQ(-2, canvas.offset_x);
	EXPECT_EQ(-1, canvas.offset_y);

	auto display = BuildVideoRenderOutputLayout(canvas, source.geometry);
	EXPECT_EQ(6, display.output_width);
	EXPECT_EQ(8, display.output_height);
	EXPECT_EQ(90, display.rotation);
	EXPECT_TRUE(display.display_vflip);

	auto viewport = BuildVideoDisplayViewportLayout(
		200,
		200,
		200,
		200,
		true,
		static_cast<double>(display.output_width) / display.output_height);
	EXPECT_EQ(25, viewport.viewport_left);
	EXPECT_EQ(150, viewport.viewport_width);
	EXPECT_EQ(0, viewport.viewport_top);
	EXPECT_EQ(0, viewport.viewport_bottom);
	EXPECT_EQ(200, viewport.viewport_height);
}

TEST(video_display_layout, display_aspect_ratio_helper_uses_baked_quarter_turn_par) {
	SourceFrameGeometry geometry = MakeDefaultSourceFrameGeometry(12, 8);
	geometry.visible_rect = { 2, 1, 8, 6 };
	geometry.rotation = 90;
	geometry.pixel_aspect_ratio = 1.25;

	auto display = GetSourceFrameDisplayOutputRect(geometry);
	EXPECT_EQ(6, display.width);
	EXPECT_EQ(8, display.height);
	EXPECT_DOUBLE_EQ(0.6, GetSourceFrameDisplayAspectRatio(geometry));
}

TEST(video_display_frame_policy, inactive_attached_display_ignores_frame_ready) {
	EXPECT_TRUE(ShouldIgnoreVideoDisplayFrameReady(false, false));
	EXPECT_FALSE(ShouldIgnoreVideoDisplayFrameReady(false, true));
	EXPECT_FALSE(ShouldIgnoreVideoDisplayFrameReady(true, false));
	EXPECT_FALSE(ShouldIgnoreVideoDisplayFrameReady(true, true));
}

TEST(video_display_frame_policy, unchanged_motion_cannot_take_the_next_ready_picture_slot) {
	using namespace std::chrono_literals;
	auto const t0 = DeadlinePacingPolicy::TimePoint{};
	for (auto offset : {-1us, 0us, 1us, 201us, 8000us, 16999us}) {
		SCOPED_TRACE(offset.count());
		DeadlinePacingPolicy gated(17ms);
		gated.Begin(t0);
		EXPECT_FALSE(gated.Request(t0 + 16ms,
								   ShouldRenderVideoDisplayInteraction(true, true, false, false, false, false)));
		EXPECT_FALSE(gated.HasPending());
		auto const arrival = t0 + 17ms + offset;
		if (offset >= 0us)
			EXPECT_FALSE(gated.OnTimer(t0 + 17ms,
									   ShouldRenderVideoDisplayInteraction(true, true, false, false, false, false)));
		bool const emitted = gated.Request(arrival,
										   ShouldRenderVideoDisplayInteraction(true, true, true, false, false, false));
		EXPECT_EQ(offset >= 0us, emitted);
		if (offset < 0us) {
			ASSERT_TRUE(gated.NextDeadline());
			EXPECT_EQ(t0 + 17ms, *gated.NextDeadline());
			EXPECT_TRUE(gated.OnTimer(t0 + 17ms));
		}
	}

	DeadlinePacingPolicy original(17ms);
	original.Begin(t0);
	ASSERT_FALSE(original.Request(t0 + 16ms));
	ASSERT_TRUE(original.OnTimer(t0 + 17ms));
	EXPECT_FALSE(original.Request(t0 + 17201us));
	ASSERT_TRUE(original.NextDeadline());
	EXPECT_EQ(t0 + 34ms, *original.NextDeadline());
}

TEST(video_display_frame_policy, visibility_feedback_and_live_tools_keep_their_display_budget) {
	using namespace std::chrono_literals;
	auto const t0 = DeadlinePacingPolicy::TimePoint{};
	for (bool paired : {false, true}) {
		SCOPED_TRACE(paired);
		DeadlinePacingPolicy policy(17ms);
		policy.Begin(t0);
		ASSERT_FALSE(policy.Request(t0 + 1ms,
									ShouldRenderVideoDisplayInteraction(paired, true, false, false, paired, false)));
		ASSERT_TRUE(policy.NextDeadline());
		EXPECT_EQ(t0 + 17ms, *policy.NextDeadline());
		EXPECT_TRUE(policy.OnTimer(t0 + 17ms,
								   ShouldRenderVideoDisplayInteraction(paired, true, false, false, paired, false)));
		EXPECT_FALSE(policy.Request(t0 + 18ms));
		ASSERT_TRUE(policy.NextDeadline());
		EXPECT_EQ(t0 + 34ms, *policy.NextDeadline());
	}
}

TEST(video_display_frame_policy, failed_draw_preserves_natural_retry_without_arming_a_loop) {
	using namespace std::chrono_literals;
	auto const t0 = DeadlinePacingPolicy::TimePoint{};
	DeadlinePacingPolicy policy(17ms);
	policy.Begin(t0);
	bool const failed_draw = ShouldRenderVideoDisplayInteraction(true, false, false, false, false, false);
	ASSERT_TRUE(failed_draw);
	EXPECT_FALSE(policy.OnTimer(t0 + 17ms, failed_draw));
	EXPECT_FALSE(policy.HasPending());
	EXPECT_TRUE(policy.Request(t0 + 18ms, failed_draw));
	EXPECT_FALSE(policy.NextDeadline());
	EXPECT_FALSE(policy.OnTimer(t0 + 35ms, failed_draw));
	EXPECT_FALSE(policy.Request(t0 + 36ms,
								ShouldRenderVideoDisplayInteraction(true, true, false, false, false, false)));
	EXPECT_FALSE(policy.NextDeadline());
	EXPECT_TRUE(policy.Request(t0 + 37ms,
							   ShouldRenderVideoDisplayInteraction(true, true, true, false, false, false)));
}
