#include <main.h>

#include "../../src/visual_edit_controller.h"
#include "../../src/video_overlay_draw_context.h"
#include "../../src/visual_tool_drag_snapshot.h"
#include "../../src/visual_tool_transform_policy.h"
#include "../../src/visual_tool_transform_overlay.h"
#include "../../src/visual_ass_edit.h"
#include "../../src/ass_dialogue.h"
#include "../../src/ass_style.h"

#include <algorithm>
#include <limits>

using namespace aegisub::visual_edit;

namespace {
class TransformOverlayRecorder final : public VideoOverlayDrawContext {
	public:
	struct Paint {
		agi::Color colour;
		float alpha = 0;
		int width = 0;
	};
	Paint line_paint;
	Paint fill_paint;
	std::vector<std::pair<Vector2D, Vector2D>> lines;
	std::vector<Paint> line_paints;
	std::vector<std::pair<Paint, Paint>> polygon_paints;
	std::vector<std::pair<Paint, Paint>> circle_paints;
	struct PerspectiveBatch {
		std::vector<VideoOverlayPerspectiveVertex> grid, axes, arrows;
		VideoOverlayPerspectiveTransform transform;
		agi::Color primary, secondary;
		int width = 0;
	};
	std::vector<PerspectiveBatch> perspective_batches;
	std::vector<VideoOverlayPreparedPerspectiveBatches const *> prepared_batches;
	void DrawPreparedPerspectiveBatches(VideoOverlayPreparedPerspectiveBatches const& prepared,
										VideoOverlayPerspectiveTransform const& transform, agi::Color const& primary, int width) override {
		prepared_batches.push_back(&prepared);
		VideoOverlayDrawContext::DrawPreparedPerspectiveBatches(prepared, transform, primary, width);
	}
	void DrawPerspectiveBatches(std::span<VideoOverlayPerspectiveVertex const> grid,
								std::span<VideoOverlayPerspectiveVertex const> axes, std::span<VideoOverlayPerspectiveVertex const> arrows,
								VideoOverlayPerspectiveTransform const& transform, agi::Color const& primary, agi::Color const& secondary, int width) override {
		PerspectiveBatch batch;
		batch.grid.assign(grid.begin(), grid.end());
		batch.axes.assign(axes.begin(), axes.end());
		batch.arrows.assign(arrows.begin(), arrows.end());
		batch.transform = transform;
		batch.primary = primary;
		batch.secondary = secondary;
		batch.width = width;
		perspective_batches.push_back(std::move(batch));
	}
	void SetLineColour(agi::Color const& colour, float alpha, int width) override { line_paint = {.colour = colour, .alpha = alpha, .width = width}; }
	void SetFillColour(agi::Color const& colour, float alpha) override { fill_paint = {.colour = colour, .alpha = alpha}; }
	void SetInvert() override {}
	void ClearInvert() override {}
	void DrawLine(Vector2D first, Vector2D second) override {
		lines.emplace_back(first, second);
		line_paints.push_back(line_paint);
	}
	void DrawLines(size_t, float const *, size_t) override {}
	void DrawLineStrip(Vector2D const *, size_t) override {}
	void DrawRectangle(Vector2D, Vector2D) override {}
	void DrawPolygon(Vector2D const *, size_t) override { polygon_paints.emplace_back(line_paint, fill_paint); }
	void DrawMultiPolygon(std::vector<float> const&, std::vector<int> const&, std::vector<int> const&, Vector2D, Vector2D, bool) override {}
	void DrawCircle(Vector2D, float) override { circle_paints.emplace_back(line_paint, fill_paint); }
	void DrawTriangle(Vector2D, Vector2D, Vector2D) override {}
	VideoOverlayTextExtent MeasureText(std::string const&, VideoOverlayTextStyle const&) override { return {}; }
	void DrawText(std::string const&, int, int, VideoOverlayTextStyle const&) override {}
};

bool SamePerspectiveBatches(TransformOverlayRecorder const& a, TransformOverlayRecorder const& b) {
	if (a.perspective_batches.size() != b.perspective_batches.size()) return false;
	auto same_vertices = [](auto const& left, auto const& right) {
		return std::ranges::equal(left, right, [](auto const& p, auto const& q) {
			return p.x == q.x && p.y == q.y && p.z == q.z && p.opacity == q.opacity;
		});
	};
	for (size_t i = 0; i < a.perspective_batches.size(); ++i) {
		auto const& x = a.perspective_batches[i];
		auto const& y = b.perspective_batches[i];
		if (!same_vertices(x.grid, y.grid) || !same_vertices(x.axes, y.axes) || !same_vertices(x.arrows, y.arrows) ||
			x.transform.origin != y.transform.origin || x.transform.angle_x != y.transform.angle_x ||
			x.transform.angle_y != y.transform.angle_y || x.transform.angle_z != y.transform.angle_z ||
			x.transform.fax != y.transform.fax || x.transform.fay != y.transform.fay ||
			x.transform.perspective_z_scale != y.transform.perspective_z_scale ||
			x.primary != y.primary || x.secondary != y.secondary || x.width != y.width) return false;
	}
	return true;
}

Line MakeLine(int id, int row, std::string text, Vector2D position) {
	Line line;
	line.id = id;
	line.row = row;
	line.text = std::move(text);
	line.position = position;
	line.first_frame = 0;
	line.last_frame = 100;
	line.clip = {.first = {0, 0}, .second = {639, 479}, .inverse = false};
	return line;
}
Viewport Canvas() { return {.video_position = {0, 0}, .video_size = {640, 480}, .script_size = {640, 480}}; }
PointerInput Input(PointerPhase phase, Vector2D point, bool down = false, bool control = false, bool alt = false, bool shift = false) {
	return {.phase = phase, .position = point, .left_down = down, .shift = shift, .control = control, .alt = alt};
}
}

TEST(visual_edit_controller, simultaneous_move_endpoints_are_serialized_after_all_positions_and_times_change) {
	auto line = MakeLine(7, 0, "{\\move(20,30,100,120,0,100)}A", {20, 30});
	line.move = Move{.start = {20, 30}, .end = {100, 120}, .start_ms = 0, .end_ms = 100};
	Controller controller(Mode::Drag);
	controller.Reset({line}, {7}, 7, {.number = 12, .time_ms = 500}, Canvas());
	controller.Pointer(Input(PointerPhase::Press, {100, 120}, true, true));
	controller.Pointer(Input(PointerPhase::Release, {100, 120}, false, true));
	controller.Pointer(Input(PointerPhase::Press, {20, 30}, true));
	controller.Pointer(Input(PointerPhase::Move, {30, 50}, true));
	ASSERT_EQ(1u, controller.IncrementalChanges().size());
	EXPECT_EQ("{\\move(30,50,110,140,500,600)}A", controller.IncrementalChanges()[0].text);
	EXPECT_EQ(line.text, controller.DraftChanges()[0].original_text);
	controller.Pointer(Input(PointerPhase::Release, {40, 60}));
	EXPECT_EQ("{\\move(40,60,120,150,500,600)}A", controller.DraftChanges()[0].text);
	EXPECT_FALSE(controller.IsInteracting());
}

TEST(visual_edit_controller, clip_hold_preserves_each_selected_inverse_mode_and_clamps_only_creation) {
	auto first = MakeLine(1, 0, "{\\clip(0,0,639,479)}A", {40, 40});
	auto second = MakeLine(2, 1, "{\\iclip(0,0,639,479)}B", {80, 80});
	second.clip.inverse = true;
	Controller controller(Mode::RectClip);
	controller.Reset({first, second}, {1, 2}, 1, {.number = 12, .time_ms = 500}, Canvas());
	controller.Pointer(Input(PointerPhase::Press, {30, 40}, true, false, true));
	controller.Pointer(Input(PointerPhase::Release, {700, 500}, false, false, true));
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ("{\\clip(30,40,640,480)}A", controller.DraftChanges()[0].text);
	EXPECT_EQ("{\\iclip(30,40,640,480)}B", controller.DraftChanges()[1].text);
	controller.Pointer(Input(PointerPhase::Press, {640, 480}, true));
	controller.Pointer(Input(PointerPhase::Release, {700, 550}));
	EXPECT_EQ("{\\clip(30,40,700,550)}A", controller.DraftChanges()[0].text);
	EXPECT_EQ("{\\iclip(30,40,700,550)}B", controller.DraftChanges()[1].text);
}

TEST(visual_edit_controller, new_clip_without_alt_narrows_selection_but_dragging_corner_does_not) {
	auto first = MakeLine(1, 0, "A", {40, 40});
	auto second = MakeLine(2, 1, "B", {80, 80});
	Controller controller(Mode::RectClip);
	controller.Reset({first, second}, {1, 2}, 2, {.number = 12, .time_ms = 500}, Canvas());
	auto result = controller.Pointer(Input(PointerPhase::Press, {30, 40}, true));
	EXPECT_TRUE(result.selection_changed);
	EXPECT_EQ((std::set<int>{2}), controller.SelectedLines());
	controller.Pointer(Input(PointerPhase::Release, {100, 120}));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(2, controller.DraftChanges()[0].id);
	EXPECT_EQ("{\\clip(30,40,100,120)}B", controller.DraftChanges()[0].text);
}

TEST(visual_edit_controller, clip_hold_uses_parsed_inverse_mode_not_literal_body_text) {
	auto line = MakeLine(1, 0, "{\\clip(0,0,639,479)}literal \\iclip text", {40, 40});
	Controller controller(Mode::RectClip);
	controller.Reset({line}, {1}, 1, {.number = 12, .time_ms = 500}, Canvas());
	controller.Pointer(Input(PointerPhase::Press, {30, 40}, true));
	controller.Pointer(Input(PointerPhase::Release, {100, 120}));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ("{\\clip(30,40,100,120)}literal \\iclip text", controller.DraftChanges()[0].text);
}

TEST(visual_edit_controller, double_click_restores_pre_click_selection_and_translates_invisible_move_and_origin) {
	auto first = MakeLine(1, 0, "{\\pos(20,30)\\org(10,10)}A", {20, 30});
	first.origin = Vector2D(10, 10);
	auto second = MakeLine(2, 1, "{\\move(100,120,140,160)\\org(90,100)}B", {100, 120});
	second.move = Move{.start = {100, 120}, .end = {140, 160}, .start_ms = 0, .end_ms = 0};
	second.origin = Vector2D(90, 100);
	second.first_frame = 80;
	Controller controller(Mode::Drag);
	controller.Reset({first, second}, {1, 2}, 1, {.number = 12, .time_ms = 500}, Canvas());
	controller.Pointer(Input(PointerPhase::Press, {200, 200}, true));
	controller.Pointer(Input(PointerPhase::Release, {200, 200}));
	EXPECT_EQ((std::set<int>{1}), controller.SelectedLines());
	controller.Pointer(Input(PointerPhase::DoubleClick, {50, 70}));
	EXPECT_EQ((std::set<int>{1, 2}), controller.SelectedLines());
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ("{\\pos(50,70)\\org(40,50)}A", controller.DraftChanges()[0].text);
	EXPECT_EQ("{\\move(130,160,170,200)\\org(120,140)}B", controller.DraftChanges()[1].text);
}

TEST(visual_edit_controller, double_click_normalizes_conflicting_position_tags_before_next_drag) {
	auto line = MakeLine(1, 0, R"({\pos(80,60)\move(10,20,30,40)}A)", {80, 60});
	line.move = Move{.start = {10, 20}, .end = {30, 40}, .start_ms = 0, .end_ms = 0};
	Controller controller(Mode::Drag);
	controller.Reset({line}, {1}, 1, {.number = 0, .time_ms = 0}, Canvas());
	controller.Pointer(Input(PointerPhase::Press, {160, 100}, true));
	controller.Pointer(Input(PointerPhase::Release, {160, 100}));
	controller.Pointer(Input(PointerPhase::DoubleClick, {160, 100}));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\move(90,60,110,80)}A)", controller.DraftChanges()[0].text);
	auto snapshot = std::dynamic_pointer_cast<VisualToolDragSnapshot const>(controller.CaptureSnapshot(std::make_shared<VisualToolRenderContext>(), {}));
	ASSERT_TRUE(snapshot);
	ASSERT_EQ(2u, snapshot->features.size());
	EXPECT_EQ(Vector2D(90, 60), snapshot->features[0].pos);
	EXPECT_EQ(Vector2D(110, 80), snapshot->features[1].pos);
	auto hit = snapshot->HitTest({90, 60});
	ASSERT_TRUE(hit);
	EXPECT_EQ(std::optional<int>(1), hit->line_id);
	EXPECT_EQ(DRAG_BIG_SQUARE, hit->type);
	EXPECT_FALSE(snapshot->HitTest({160, 100}));
	controller.AcceptChanges();
	auto begin = controller.Pointer(Input(PointerPhase::Press, {90, 60}, true), snapshot.get());
	EXPECT_TRUE(begin.interaction_began);
	controller.Pointer(Input(PointerPhase::Release, {100, 70}), snapshot.get());
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\move(90,60,110,80)}A)", controller.DraftChanges()[0].original_text);
	EXPECT_EQ(R"({\move(100,70,110,80,0,0)}A)", controller.DraftChanges()[0].text);
}

TEST(visual_edit_controller, capture_loss_keeps_draft_and_shift_constrains_script_delta_through_viewport) {
	auto line = MakeLine(1, 0, "{\\pos(20,30)}A", {20, 30});
	Controller controller(Mode::Drag);
	controller.Reset({line}, {1}, 1, {.number = 12, .time_ms = 500}, {.video_position = {10, 20}, .video_size = {1280, 960}, .script_size = {640, 480}});
	controller.Pointer(Input(PointerPhase::Press, {50, 80}, true));
	controller.Pointer(Input(PointerPhase::Move, {90, 100}, true, false, false, true));
	auto result = controller.FinishInteraction();
	EXPECT_TRUE(result.interaction_ended);
	EXPECT_EQ(CaptureAction::Release, result.capture);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ("{\\pos(40,30)}A", controller.DraftChanges()[0].text);
	EXPECT_FALSE(controller.IsInteracting());
}

TEST(visual_edit_controller, snapshot_miss_never_hits_unpresented_live_geometry) {
	auto line = MakeLine(1, 0, "{\\pos(20,30)}A", {20, 30});
	Controller controller(Mode::Drag);
	controller.Reset({line}, {1}, 1, {.number = 12, .time_ms = 500}, Canvas());
	auto snapshot = controller.CaptureSnapshot(std::make_shared<VisualToolRenderContext>(), {});
	controller.Pointer(Input(PointerPhase::Press, {20, 30}, true));
	controller.Pointer(Input(PointerPhase::Release, {200, 200}));
	controller.Pointer(Input(PointerPhase::Press, {200, 200}, true), snapshot.get());
	EXPECT_FALSE(controller.IsInteracting());
	controller.Pointer(Input(PointerPhase::Move, {250, 250}, true), snapshot.get());
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ("{\\pos(200,200)}A", controller.DraftChanges()[0].text);
}

TEST(visual_edit_controller, toggle_uses_per_line_frame_times_and_keeps_unrelated_override_blocks) {
	auto first = MakeLine(1, 0, R"({\pos(20,30)\b1}A{\pos(8,9)})", {20, 30});
	first.position_to_move_start_ms = -5;
	first.position_to_move_end_ms = 36;
	auto second = MakeLine(2, 1, "{\\move(100,120,140,160,10,90)}B", {100, 120});
	second.move = Move{.start = {100, 120}, .end = {140, 160}, .start_ms = 10, .end_ms = 90};
	Controller controller(Mode::Drag);
	controller.Reset({first, second}, {1, 2}, 1, {.number = 12, .time_ms = 500}, Canvas());
	controller.ToggleMovePosition();
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ("{\\b1\\move(20,30,20,30,-5,36)}A{\\pos(8,9)}", controller.DraftChanges()[0].text);
	EXPECT_EQ("{\\pos(100,120)}B", controller.DraftChanges()[1].text);
}

TEST(visual_edit_controller, toggle_preserves_position_precedence_from_retained_later_override_block) {
	auto line = MakeLine(1, 0, R"({\pos(20,30)\b1}A{\pos(8,9)})", {20, 30});
	line.position_to_move_start_ms = -5;
	line.position_to_move_end_ms = 36;
	Controller controller(Mode::Drag);
	controller.Reset({line}, {1}, 1, {.number = 0, .time_ms = 0}, Canvas());
	controller.ToggleMovePosition();
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\b1\move(20,30,20,30,-5,36)}A{\pos(8,9)})", controller.DraftChanges()[0].text);
	EXPECT_TRUE(controller.ActiveLineHasMove());
	auto snapshot = std::dynamic_pointer_cast<VisualToolDragSnapshot const>(controller.CaptureSnapshot(std::make_shared<VisualToolRenderContext>(), {}));
	ASSERT_TRUE(snapshot);
	ASSERT_EQ(2u, snapshot->features.size());
	EXPECT_EQ(Vector2D(8, 9), snapshot->features[0].pos);
	EXPECT_EQ(Vector2D(20, 30), snapshot->features[1].pos);
	auto hit = snapshot->HitTest({8, 9});
	ASSERT_TRUE(hit);
	EXPECT_EQ(std::optional<int>(1), hit->line_id);
	EXPECT_EQ(DRAG_BIG_SQUARE, hit->type);
}

TEST(visual_edit_controller, prepared_press_allows_selection_autoseek_before_capture_and_move_time_anchor) {
	auto first = MakeLine(1, 0, "{\\pos(20,30)}A", {20, 30});
	auto second = MakeLine(2, 1, "{\\move(100,120,200,220,10,210)}B", {100, 120});
	second.start_ms = 400;
	second.first_frame = 10;
	second.move = Move{.start = {100, 120}, .end = {200, 220}, .start_ms = 10, .end_ms = 210};
	Controller controller(Mode::Drag);
	controller.Reset({first, second}, {1}, 1, {.number = 25, .time_ms = 1000}, Canvas());
	auto select = controller.Pointer(Input(PointerPhase::Press, {100, 120}, true), nullptr, true);
	EXPECT_TRUE(select.selection_changed);
	EXPECT_FALSE(select.interaction_began);
	EXPECT_EQ(CaptureAction::None, select.capture);
	EXPECT_FALSE(controller.IsInteracting());
	EXPECT_EQ(std::optional<int>(2), controller.ActiveLine());
	controller.SetFrame({.number = 10, .time_ms = 400});
	auto begin = controller.BeginPreparedPress();
	EXPECT_TRUE(begin.interaction_began);
	EXPECT_EQ(CaptureAction::Capture, begin.capture);
	EXPECT_TRUE(controller.IsInteracting());
	controller.Pointer(Input(PointerPhase::Move, {110, 140}, true));
	auto end = controller.Pointer(Input(PointerPhase::Release, {120, 150}));
	EXPECT_TRUE(end.interaction_ended);
	EXPECT_EQ(CaptureAction::Release, end.capture);
	EXPECT_FALSE(controller.IsInteracting());
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ("{\\move(120,150,200,220,0,210)}B", controller.DraftChanges()[0].text);
}

TEST(visual_edit_controller, clip_corner_multiselection_survives_seek_within_active_line) {
	auto line = MakeLine(1, 0, "{\\clip(0,0,639,479)}A", {20, 30});
	Controller controller(Mode::RectClip);
	controller.Reset({line}, {1}, 1, {.number = 12, .time_ms = 500}, Canvas());
	controller.Pointer(Input(PointerPhase::Press, {0, 0}, true));
	controller.Pointer(Input(PointerPhase::Release, {0, 0}));
	controller.Pointer(Input(PointerPhase::Press, {639, 479}, true, true));
	controller.Pointer(Input(PointerPhase::Release, {639, 479}, false, true));
	controller.SetFrame({.number = 13, .time_ms = 540});
	controller.Pointer(Input(PointerPhase::Press, {0, 0}, true));
	controller.Pointer(Input(PointerPhase::Release, {10, 20}));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ("{\\clip(10,20,649,499)}A", controller.DraftChanges()[0].text);
}

TEST(visual_edit_controller, incremental_line_update_preserves_other_selection_and_refreshes_geometry_and_timing) {
	auto first = MakeLine(1, 0, "{\\pos(20,30)}A", {20, 30});
	auto second = MakeLine(2, 1, "{\\pos(80,80)}B", {80, 80});
	Controller controller(Mode::Drag);
	controller.Reset({first, second}, {1, 2}, 1, {.number = 12, .time_ms = 500}, Canvas());
	second.text = "{\\pos(90,90)\\b1}B";
	second.position = {90, 90};
	second.first_frame = 30;
	controller.UpdateLines(std::span(&second, 1));
	auto hidden = controller.CaptureSnapshot(std::make_shared<VisualToolRenderContext>(), {});
	EXPECT_FALSE(hidden->HitTest({90, 90}));
	controller.SetFrame({.number = 30, .time_ms = 1200});
	auto visible = controller.CaptureSnapshot(std::make_shared<VisualToolRenderContext>(), {});
	ASSERT_TRUE(visible->HitTest({90, 90}));
	EXPECT_EQ(std::optional<int>(2), visible->HitTest({90, 90})->line_id);
	controller.Pointer(Input(PointerPhase::Press, {20, 30}, true));
	controller.Pointer(Input(PointerPhase::Release, {30, 50}));
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ("{\\pos(30,50)}A", controller.DraftChanges()[0].text);
	EXPECT_EQ(second.text, controller.DraftChanges()[1].original_text);
	EXPECT_EQ("{\\b1\\pos(100,110)}B", controller.DraftChanges()[1].text);
}

TEST(visual_edit_controller, optional_override_planner_rejects_entire_batch_and_restores_last_valid_features) {
	auto first = MakeLine(1, 0, R"({\pos(20,30)\org(10,10)}A)", {20, 30});
	first.origin = Vector2D(10, 10);
	auto second = MakeLine(2, 1, R"({\pos(100,120)}B)", {100, 120});
	Controller controller(Mode::Drag);
	controller.Reset({first, second}, {1, 2}, 1, {.number = 12, .time_ms = 500}, Canvas());
	int calls = 0;
	OverridePlanner planner = [&](std::span<OverrideEdit const> edits) -> std::optional<std::vector<VisualTextEdit>> {
		EXPECT_EQ(2u, edits.size());
		for (auto const& edit : edits) {
			EXPECT_EQ(OverrideIntent::Translation, edit.intent);
			EXPECT_EQ("\\pos", edit.tag);
		}
		if (++calls > 1)
			return std::nullopt;
		EXPECT_EQ(Vector2D(10, 20), edits[0].translation);
		return std::vector<VisualTextEdit>{
			{.id = 1, .row = 0, .original_text = first.text, .text = R"({\pos(30,50)\org(20,30)}A)"},
			{.id = 2, .row = 1, .original_text = second.text, .text = R"({\pos(110,140)}B)"}};
	};
	controller.Pointer(Input(PointerPhase::Press, {20, 30}, true), nullptr, false, planner);
	controller.Pointer(Input(PointerPhase::Move, {30, 50}, true), nullptr, false, planner);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\pos(30,50)\org(20,30)}A)", controller.DraftChanges()[0].text);
	controller.Pointer(Input(PointerPhase::Move, {60, 80}, true), nullptr, false, planner);
	EXPECT_TRUE(controller.IsInteracting());
	EXPECT_TRUE(controller.IncrementalChanges().empty());
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\pos(30,50)\org(20,30)}A)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\pos(110,140)}B)", controller.DraftChanges()[1].text);
	auto snapshot = std::dynamic_pointer_cast<VisualToolDragSnapshot const>(controller.CaptureSnapshot({}, {}));
	ASSERT_TRUE(snapshot);
	ASSERT_EQ(3u, snapshot->features.size());
	EXPECT_EQ(Vector2D(30, 50), snapshot->features[0].pos);
	EXPECT_EQ(Vector2D(20, 30), snapshot->features[1].pos);
	EXPECT_EQ(Vector2D(110, 140), snapshot->features[2].pos);
	controller.Pointer(Input(PointerPhase::Release, {60, 80}), nullptr, false, planner);
	EXPECT_FALSE(controller.IsInteracting());
	EXPECT_TRUE(controller.IncrementalChanges().empty());
	EXPECT_EQ(3, calls);
}

TEST(visual_edit_controller, planned_double_click_includes_invisible_rows_and_keeps_source_on_rejection) {
	auto first = MakeLine(1, 0, R"({\pos(20,30)}A)", {20, 30});
	auto second = MakeLine(2, 1, R"({\move(100,120,140,160,10,90)\org(90,100)}B)", {100, 120});
	second.move = Move{.start = {100, 120}, .end = {140, 160}, .start_ms = 10, .end_ms = 90};
	second.origin = Vector2D(90, 100);
	second.first_frame = 80;
	Controller controller(Mode::Drag);
	controller.Reset({first, second}, {1, 2}, 1, {.number = 12, .time_ms = 500}, Canvas());
	bool accept = false;
	OverridePlanner planner = [&](std::span<OverrideEdit const> edits) -> std::optional<std::vector<VisualTextEdit>> {
		EXPECT_EQ(3u, edits.size());
		for (auto const& edit : edits) {
			EXPECT_EQ(OverrideIntent::Translation, edit.intent);
			EXPECT_EQ(Vector2D(30, 40), edit.translation);
		}
		if (!accept)
			return std::nullopt;
		return std::vector<VisualTextEdit>{
			{.id = 1, .row = 0, .original_text = first.text, .text = R"({\pos(50,70)}A)"},
			{.id = 2, .row = 1, .original_text = second.text, .text = R"({\move(130,160,170,200,10,90)\org(120,140)}B)"}};
	};
	controller.Pointer(Input(PointerPhase::Press, {200, 200}, true));
	controller.Pointer(Input(PointerPhase::Release, {200, 200}));
	controller.Pointer(Input(PointerPhase::DoubleClick, {50, 70}), nullptr, false, planner);
	EXPECT_TRUE(controller.DraftChanges().empty());
	auto rejected = std::dynamic_pointer_cast<VisualToolDragSnapshot const>(controller.CaptureSnapshot({}, {}));
	ASSERT_TRUE(rejected);
	ASSERT_EQ(1u, rejected->features.size());
	EXPECT_EQ(Vector2D(20, 30), rejected->features[0].pos);
	accept = true;
	controller.Pointer(Input(PointerPhase::Press, {200, 200}, true));
	controller.Pointer(Input(PointerPhase::Release, {200, 200}));
	controller.Pointer(Input(PointerPhase::DoubleClick, {50, 70}), nullptr, false, planner);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(first.text, controller.DraftChanges()[0].original_text);
	EXPECT_EQ(R"({\pos(50,70)}A)", controller.DraftChanges()[0].text);
	EXPECT_EQ(second.text, controller.DraftChanges()[1].original_text);
	EXPECT_EQ(R"({\move(130,160,170,200,10,90)\org(120,140)}B)", controller.DraftChanges()[1].text);
}

TEST(visual_edit_controller, prepared_press_can_preserve_times_for_all_selected_move_rows) {
	auto first = MakeLine(1, 0, R"({\move(20,30,60,70,10,100)}A)", {20, 30});
	first.move = Move{.start = {20, 30}, .end = {60, 70}, .start_ms = 10, .end_ms = 100};
	auto second = MakeLine(2, 1, R"({\move(100,120,160,170,40,140)\clip(0,0,20,20)}B)", {100, 120});
	second.move = Move{.start = {100, 120}, .end = {160, 170}, .start_ms = 40, .end_ms = 140};
	Controller controller(Mode::Drag);
	controller.Reset({first, second}, {1, 2}, 1, {.number = 12, .time_ms = 500}, Canvas());
	controller.Pointer(Input(PointerPhase::Press, {20, 30}, true), nullptr, true);
	EXPECT_EQ((std::vector<int>{1, 2}), controller.SelectedMoveLines());
	controller.BeginPreparedPress(true);
	controller.Pointer(Input(PointerPhase::Release, {30, 50}));
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\move(30,50,60,70,10,100)}A)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\clip(0,0,20,20)\move(110,140,160,170,40,140)}B)", controller.DraftChanges()[1].text);
}

TEST(visual_edit_controller, rotate_z_uses_active_angle_for_selection_and_replaces_legacy_alias) {
	auto first = MakeLine(1, 0, R"({\fr30\org(100,100)}日本語)", {140, 140});
	first.origin = Vector2D(100, 100);
	first.rotation_z = 30;
	auto second = MakeLine(2, 1, R"({\frz75\b1}العربية)", {200, 200});
	second.rotation_z = 75;
	Controller controller(Mode::RotateZ);
	controller.Reset({first, second}, {1, 2}, 1, {}, Canvas());
	auto snapshot = controller.CaptureSnapshot(std::make_shared<VisualToolRenderContext>(), {});
	ASSERT_TRUE(snapshot);
	EXPECT_TRUE(controller.Pointer(Input(PointerPhase::Press, {200, 100}, true), snapshot.get()).interaction_began);
	controller.Pointer(Input(PointerPhase::Move, {100, 0}, true), snapshot.get());
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\org(100,100)\frz120}日本語)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\b1\frz120}العربية)", controller.DraftChanges()[1].text);
	EXPECT_EQ(first.text, controller.DraftChanges()[0].original_text);
	auto result = controller.FinishInteraction();
	EXPECT_TRUE(result.interaction_ended);
	EXPECT_EQ(CaptureAction::Release, result.capture);
	controller.AcceptChanges();
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Release, {100, 0})).interaction_ended);
}

TEST(visual_edit_controller, rotate_origin_uses_each_lines_fallback_and_anisotropic_viewport) {
	auto first = MakeLine(1, 0, R"({\org(100,100)\frz20}A)", {150, 150});
	first.origin = Vector2D(100, 100);
	auto second = MakeLine(2, 1, R"({\pos(200,150)}B)", {200, 150});
	Controller controller(Mode::RotateZ);
	controller.Reset({first, second}, {1, 2}, 1, {}, {.video_position = {10, 20}, .video_size = {1280, 480}, .script_size = {640, 480}});
	auto snapshot = controller.CaptureSnapshot(std::make_shared<VisualToolRenderContext>(), {});
	ASSERT_TRUE(snapshot);
	ASSERT_TRUE(snapshot->HitTest({210, 120}));
	controller.Pointer(Input(PointerPhase::Press, {210, 120}, true), snapshot.get());
	controller.Pointer(Input(PointerPhase::Release, {250, 150}), snapshot.get());
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\frz20\org(120,130)}A)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\pos(200,150)\org(220,180)}B)", controller.DraftChanges()[1].text);
	EXPECT_EQ((std::set<int>{1, 2}), controller.SelectedLines());
}

TEST(visual_edit_controller, scale_preserves_selection_and_uses_display_delta_with_snap_and_ratio) {
	auto first = MakeLine(1, 0, R"({\fscx150\fscy80\pos(100,100)}A)", {100, 100});
	first.scale = {150, 80};
	auto second = MakeLine(2, 1, R"({\b1}B)", {300, 300});
	Controller controller(Mode::Scale);
	controller.Reset({first, second}, {1, 2}, 1, {}, {.video_position = {10, 20}, .video_size = {1280, 960}, .script_size = {640, 480}});
	controller.Pointer(Input(PointerPhase::Press, {400, 400}, true));
	controller.Pointer(Input(PointerPhase::Move, {440, 380}, true, true));
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\pos(100,100)\fscx200\fscy100}A)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\b1\fscx200\fscy100}B)", controller.DraftChanges()[1].text);
	controller.Pointer(Input(PointerPhase::Release, {440, 380}, false, false, true));
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\pos(100,100)\fscx200\fscy106}A)", controller.DraftChanges()[0].text);
	EXPECT_EQ((std::set<int>{1, 2}), controller.SelectedLines());
}

TEST(visual_edit_controller, transform_empty_interactions_and_invisible_active_lines_do_not_edit) {
	for (auto mode : {Mode::RotateZ, Mode::Scale}) {
		auto line = MakeLine(1, 0, R"({\pos(100,100)\frz30\fscx150}A)", {100, 100});
		Controller controller(mode);
		controller.Reset({line}, {1}, 1, {}, Canvas());
		controller.Pointer(Input(PointerPhase::Press, {200, 200}, true));
		controller.Pointer(Input(PointerPhase::Release, {200, 200}));
		EXPECT_TRUE(controller.DraftChanges().empty());
		controller.SetFrame({.number = 101});
		EXPECT_FALSE(controller.CaptureSnapshot(std::make_shared<VisualToolRenderContext>(), {}));
		EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Press, {200, 200}, true)).interaction_began);
	}
}

TEST(visual_edit_controller, transform_active_outside_selection_tracks_preview_without_editing_active) {
	for (auto mode : {Mode::RotateZ, Mode::Scale}) {
		auto active = MakeLine(1, 0, R"({\pos(200,200)\frz30\fscx120\fscy80}Active)", {200, 200});
		active.rotation_z = mode == Mode::RotateZ ? 30 : 0;
		active.scale = mode == Mode::Scale ? Vector2D(120, 80) : Vector2D(100, 100);
		auto selected = MakeLine(2, 1, R"({\b1}Selected)", {300, 300});
		Controller controller(mode);
		controller.Reset({active, selected}, {2}, 1, {}, Canvas());
		auto context = std::make_shared<VisualToolRenderContext>();
		auto before = controller.CaptureSnapshot(context, {});
		controller.Pointer(Input(PointerPhase::Press, {300, 200}, true), before.get());
		controller.Pointer(Input(PointerPhase::Move, mode == Mode::RotateZ ? Vector2D(200, 100) : Vector2D(340, 180), true), before.get());
		ASSERT_EQ(1u, controller.DraftChanges().size());
		EXPECT_EQ(2, controller.DraftChanges()[0].id);
		EXPECT_EQ(std::optional<int>(1), controller.ActiveLine());
		EXPECT_EQ((std::set<int>{2}), controller.SelectedLines());
		auto preview = controller.CaptureSnapshot(context, {});
		TransformOverlayRecorder drawn;
		preview->Draw(drawn);
		size_t const axis = mode == Mode::RotateZ ? 3 : 0;
		ASSERT_GT(drawn.lines.size(), axis);
		if (mode == Mode::RotateZ) {
			EXPECT_EQ(R"({\b1\frz120}Selected)", controller.DraftChanges()[0].text);
			EXPECT_NEAR(225, drawn.lines[axis].first.X(), .001f);
			EXPECT_NEAR(243.30127f, drawn.lines[axis].first.Y(), .001f);
		}
		else {
			EXPECT_EQ(R"({\b1\fscx170\fscy105}Selected)", controller.DraftChanges()[0].text);
			EXPECT_EQ(Vector2D(295, 116), drawn.lines[0].first);
			EXPECT_EQ(Vector2D(295, 284), drawn.lines[0].second);
		}
		controller.FinishInteraction();
		TransformOverlayRecorder retired;
		controller.CaptureSnapshot(context, {})->Draw(retired);
		ASSERT_GT(retired.lines.size(), axis);
		EXPECT_NE(drawn.lines[axis], retired.lines[axis]);
		TransformOverlayRecorder retained;
		preview->Draw(retained);
		EXPECT_EQ(drawn.lines, retained.lines);
	}
}

TEST(visual_edit_controller, transform_return_to_press_restores_exact_source_and_erases_drafts) {
	for (auto mode : {Mode::RotateZ, Mode::Scale}) {
		auto line = MakeLine(1, 0, R"({\fr30\fscx150.5\b1}Text{\i1}続き)", {200, 200});
		line.rotation_z = 30;
		line.scale = {150.5f, 80.f};
		Controller controller(mode);
		controller.Reset({line}, {1}, 1, {}, Canvas());
		controller.Pointer(Input(PointerPhase::Press, {300, 200}, true));
		controller.Pointer(Input(PointerPhase::Move, {320, 180}, true));
		ASSERT_EQ(1u, controller.DraftChanges().size());
		controller.Pointer(Input(PointerPhase::Move, {300, 200}, true));
		EXPECT_TRUE(controller.DraftChanges().empty());
		ASSERT_EQ(1u, controller.IncrementalChanges().size());
		EXPECT_EQ(line.text, controller.IncrementalChanges()[0].text);
		controller.Pointer(Input(PointerPhase::Release, {300, 200}));
		EXPECT_TRUE(controller.DraftChanges().empty());
	}
}

TEST(visual_transform_policy, rotate_xy_matches_wx_axis_snap_and_signed_wrap) {
	using aegisub::visual_transform::RotateXY;
	EXPECT_EQ(Vector2D(37, 93), *RotateXY({23, 71}, {11, -7}, false, false));
	EXPECT_EQ(Vector2D(23, 93), *RotateXY({23, 71}, {11, -7}, true, false));
	EXPECT_EQ(Vector2D(30, 90), *RotateXY({23, 71}, {11, -7}, true, true));
	EXPECT_EQ(Vector2D(23, 91), *RotateXY({23, 71}, {10, -10}, true, false));
	EXPECT_EQ(Vector2D(0, 0), *RotateXY({359, 1}, {-.5f, -.5f}, false, false));
	EXPECT_EQ(Vector2D(-80, -80), *RotateXY({0, 0}, {-400, 400}, false, false));
	EXPECT_FALSE(RotateXY({std::numeric_limits<float>::infinity(), 0.f}, {1, 2}, false, false));
	EXPECT_FALSE(RotateXY({0, 0}, {1.f, std::numeric_limits<float>::quiet_NaN()}, false, false));
}

TEST(visual_ass_edit, override_in_place_preserves_first_block_replacement_rules) {
	struct Case {
		char const *name;
		char const *text;
		char const *tag;
		char const *value;
		char const *expected;
	};
	Case const cases[] = {
		{.name = "plain_text", .text = R"(日本語{\i1}続き)", .tag = "\\frz", .value = "25", .expected = R"({\frz25}日本語{\i1}続き)"},
		{.name = "empty_text", .text = "", .tag = "\\fscx", .value = "125", .expected = R"({\fscx125})"},
		{.name = "rotation_alias", .text = R"({\fr10\frz20\b1\frz30}A{\frz40}B)", .tag = "\\frz", .value = "25", .expected = R"({\b1\frz25}A{\frz40}B)"},
		{.name = "colour_alias", .text = R"({\c&H112233&\1c&H445566&\i1}A)", .tag = "\\1c", .value = "&H778899&", .expected = R"({\i1\1c&H778899&}A)"},
		{.name = "position_alias", .text = R"({\move(1,2,3,4,5,6)\pos(7,8)\b1}A)", .tag = "\\pos", .value = "(20,30)", .expected = R"({\b1\pos(20,30)}A)"},
		{.name = "move_alias", .text = R"({\pos(7,8)\move(1,2,3,4,5,6)\b1}A)", .tag = "\\move", .value = "(20,30,40,50)", .expected = R"({\b1\move(20,30,40,50)}A)"},
		{.name = "clip_alias", .text = R"({\iclip(1,2,3,4)\clip(5,6,7,8)\b1}A)", .tag = "\\clip", .value = "(10,20,30,40)", .expected = R"({\b1\clip(10,20,30,40)}A)"},
		{.name = "inverse_clip_alias", .text = R"({\clip(1,2,3,4)\iclip(5,6,7,8)\b1}A)", .tag = "\\iclip", .value = "(10,20,30,40)", .expected = R"({\b1\iclip(10,20,30,40)}A)"},
		{.name = "same_value", .text = R"({\b1\fscx125}A)", .tag = "\\fscx", .value = "125", .expected = R"({\b1\fscx125}A)"},
	};
	for (auto const& entry : cases) {
		SCOPED_TRACE(entry.name);
		AssDialogue dialogue{AssDialogueBase{}};
		dialogue.Text = entry.text;
		SetOverride(dialogue, entry.tag, entry.value);
		EXPECT_EQ(entry.expected, dialogue.Text.get());
		EXPECT_EQ(entry.expected, SetOverride(std::string(entry.text), entry.tag, entry.value));
	}
}

TEST(visual_ass_edit, override_in_place_keeps_dialogue_identity_and_metadata) {
	AssDialogue dialogue{AssDialogueBase{}};
	dialogue.Id = 42;
	dialogue.Row = 7;
	dialogue.Comment = true;
	dialogue.Layer = 3;
	dialogue.Margin = {11, 22, 33};
	dialogue.Start = 1234;
	dialogue.End = 5678;
	dialogue.Style = "Pose";
	dialogue.Actor = "Speaker";
	dialogue.Effect = "Banner";
	dialogue.ExtradataIds = std::vector<uint32_t>{2, 8};
	dialogue.Text = R"({\fr10\b1}Text)";
	SetOverride(dialogue, "\\frz", "25");
	EXPECT_EQ(R"({\b1\frz25}Text)", dialogue.Text.get());
	EXPECT_EQ(42, dialogue.Id);
	EXPECT_EQ(7, dialogue.Row);
	EXPECT_TRUE(dialogue.Comment);
	EXPECT_EQ(3, dialogue.Layer);
	EXPECT_EQ((std::array<int, 3>{11, 22, 33}), dialogue.Margin);
	EXPECT_EQ(1234, dialogue.Start.GetMillisecond());
	EXPECT_EQ(5678, dialogue.End.GetMillisecond());
	EXPECT_EQ("Pose", dialogue.Style.get());
	EXPECT_EQ("Speaker", dialogue.Actor.get());
	EXPECT_EQ("Banner", dialogue.Effect.get());
	EXPECT_EQ((std::vector<uint32_t>{2, 8}), dialogue.ExtradataIds.get());
}

TEST(visual_ass_edit, geometry_read_preserves_style_margins_alignment_and_owned_text) {
	AssStyle style;
	style.alignment = 9;
	style.Margin = {11, 22, 33};
	style.scalex = 140.5;
	style.scaley = 81;
	style.angle = 17;
	AssDialogue dialogue{AssDialogueBase{}};
	dialogue.Margin = {0, 40, 0};
	dialogue.Text = "Text";
	auto line = ReadLine(dialogue, &style, {640, 480});
	EXPECT_EQ(Vector2D(600, 33), line.position);
	EXPECT_FALSE(line.origin);
	EXPECT_EQ(Vector2D(140.5f, 81.f), line.scale);
	EXPECT_EQ(17.f, line.rotation_z);
	EXPECT_EQ(0.f, line.rotation_x);
	EXPECT_EQ(0.f, line.rotation_y);
	EXPECT_EQ(0.f, line.fax);
	EXPECT_EQ(0.f, line.fay);
	dialogue.Text = R"({\a5\fr29}Legacy)";
	EXPECT_EQ("Text", line.text);
	line = ReadLine(dialogue, &style, {640, 480});
	EXPECT_EQ(Vector2D(11, 33), line.position);
	EXPECT_EQ(29.f, line.rotation_z);
	dialogue.Text = R"({\a5\an2}Modern)";
	line = ReadLine(dialogue, &style, {640, 480});
	EXPECT_EQ(Vector2D(305, 447), line.position);
}

TEST(visual_ass_edit, geometry_read_preserves_move_fallback_and_explicit_pose) {
	AssStyle style;
	style.angle = 17;
	style.scalex = 140;
	style.scaley = 80;
	AssDialogue dialogue{AssDialogueBase{}};
	dialogue.Text = R"({\move(20,30,40,50,100,200)\org(60,70)\fr12\frz32\frx23\fry71\fscx125\fscy90\fax0.2\fay-0.15}Text)";
	auto const line = ReadLine(dialogue, &style, {640, 480});
	EXPECT_EQ(Vector2D(20, 30), line.position);
	ASSERT_TRUE(line.move);
	EXPECT_EQ(Vector2D(20, 30), line.move->start);
	EXPECT_EQ(Vector2D(40, 50), line.move->end);
	EXPECT_EQ(100, line.move->start_ms);
	EXPECT_EQ(200, line.move->end_ms);
	EXPECT_EQ(std::optional<Vector2D>(Vector2D(60, 70)), line.origin);
	EXPECT_EQ(23.f, line.rotation_x);
	EXPECT_EQ(71.f, line.rotation_y);
	EXPECT_EQ(32.f, line.rotation_z);
	EXPECT_EQ(Vector2D(125, 90), line.scale);
	EXPECT_EQ(.2f, line.fax);
	EXPECT_EQ(-.15f, line.fay);
	EXPECT_EQ(dialogue.Text.get(), line.text);
}

TEST(visual_edit_controller, rotate_xy_reads_shear_and_preserves_style_and_override_pose) {
	AssStyle style;
	style.scalex = 140.5;
	style.scaley = 81;
	style.angle = 17;
	AssDialogue dialogue{AssDialogueBase{}};
	dialogue.Text = R"({\pos(150,120)\org(80,90)\frx23.5\fry71\frz52\fax0.2\fay-0.15\fscy92.5}日本語)";
	auto line = ReadLine(dialogue, &style, {640, 480});
	EXPECT_EQ(Vector2D(150, 120), line.position);
	EXPECT_EQ(std::optional<Vector2D>(Vector2D(80, 90)), line.origin);
	EXPECT_EQ(23.5f, line.rotation_x);
	EXPECT_EQ(71.f, line.rotation_y);
	EXPECT_EQ(52.f, line.rotation_z);
	EXPECT_EQ(.2f, line.fax);
	EXPECT_EQ(-.15f, line.fay);
	EXPECT_EQ(Vector2D(140.5f, 92.5f), line.scale);
	dialogue.Text = R"({\fr29}العربية)";
	line = ReadLine(dialogue, &style, {640, 480});
	EXPECT_EQ(0.f, line.rotation_x);
	EXPECT_EQ(0.f, line.rotation_y);
	EXPECT_EQ(29.f, line.rotation_z);
	EXPECT_EQ(0.f, line.fax);
	EXPECT_EQ(0.f, line.fay);
	EXPECT_EQ(Vector2D(140.5f, 81.f), line.scale);
}

TEST(visual_edit_controller, rotate_xy_uses_active_display_delta_for_selected_invisible_sources) {
	auto first = MakeLine(1, 0, R"({\frx23\fry71\frz52\fscx140\fscy80\fax0.2\fay-0.15\org(100,100)}日本語)", {150, 150});
	first.origin = Vector2D(100, 100);
	first.rotation_x = 23;
	first.rotation_y = 71;
	first.rotation_z = 52;
	first.scale = {140, 80};
	first.fax = .2f;
	first.fay = -.15f;
	auto second = MakeLine(2, 4, R"({\frx80\fry12\b1}العربية)", {210, 160});
	second.first_frame = 101;
	second.last_frame = 150;
	Controller controller(Mode::RotateXY);
	controller.Reset({first, second}, {1, 2}, 1, {}, {.video_position = {10, 20}, .video_size = {1280, 480}, .script_size = {640, 480}});
	auto snapshot = controller.CaptureSnapshot(std::make_shared<VisualToolRenderContext>(), {});
	ASSERT_TRUE(snapshot);
	EXPECT_TRUE(controller.Pointer(Input(PointerPhase::Press, {400, 200}, true), snapshot.get()).interaction_began);
	controller.Pointer(Input(PointerPhase::Move, {411, 193}, true), snapshot.get());
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\frz52\fscx140\fscy80\fax0.2\fay-0.15\org(100,100)\frx37\fry93}日本語)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\b1\frx37\fry93}العربية)", controller.DraftChanges()[1].text);
	EXPECT_EQ(2, controller.DraftChanges()[1].id);
	EXPECT_EQ(4, controller.DraftChanges()[1].row);
	EXPECT_EQ(second.text, controller.DraftChanges()[1].original_text);
	EXPECT_EQ((std::set<int>{1, 2}), controller.SelectedLines());
	EXPECT_EQ(std::optional<int>(1), controller.ActiveLine());
	EXPECT_TRUE(controller.FinishInteraction().interaction_ended);
	controller.AcceptChanges();
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Release, {411, 193})).interaction_ended);
}

TEST(visual_edit_controller, rotate_xy_origin_uses_per_source_fallback_and_display_axis_constraint) {
	auto active = MakeLine(1, 0, R"({\frx23\fry71\org(100,100)}Active)", {150, 150});
	active.origin = Vector2D(100, 100);
	active.rotation_x = 23;
	active.rotation_y = 71;
	auto first = MakeLine(2, 1, R"({\org(50,70)}Explicit)", {220, 180});
	first.origin = Vector2D(50, 70);
	auto second = MakeLine(3, 2, R"({\pos(300,200)}Fallback)", {300, 200});
	Controller controller(Mode::RotateXY);
	controller.Reset({active, first, second}, {2, 3}, 1, {}, {.video_position = {10, 20}, .video_size = {1280, 480}, .script_size = {640, 480}});
	auto context = std::make_shared<VisualToolRenderContext>();
	auto snapshot = controller.CaptureSnapshot(context, {});
	ASSERT_TRUE(snapshot);
	ASSERT_TRUE(snapshot->HitTest({210, 120}));
	EXPECT_TRUE(controller.Pointer(Input(PointerPhase::Press, {210, 120}, true), snapshot.get()).interaction_began);
	controller.Pointer(Input(PointerPhase::Move, {250, 150}, true, false, false, true), snapshot.get());
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\org(70,70)}Explicit)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\pos(300,200)\org(320,200)}Fallback)", controller.DraftChanges()[1].text);
	auto preview = controller.CaptureSnapshot(context, {});
	ASSERT_TRUE(preview);
	ASSERT_TRUE(preview->HitTest({250, 120}));
	EXPECT_EQ(std::optional<int>(1), preview->HitTest({250, 120})->line_id);
	EXPECT_FALSE(snapshot->HitTest({250, 120}));
	controller.Pointer(Input(PointerPhase::Release, {250, 150}, false, false, false, true));
	EXPECT_EQ((std::set<int>{2, 3}), controller.SelectedLines());
	EXPECT_EQ(std::optional<int>(1), controller.ActiveLine());
	EXPECT_TRUE(controller.CaptureSnapshot(context, {})->HitTest({210, 120}));
}

TEST(visual_edit_controller, rotate_xy_modifiers_return_to_baseline_restores_exact_source) {
	auto line = MakeLine(1, 0, R"({\frx-336.875\fry431.5\fr17\fscx140.5\b1}Text{\i1}続き)", {200, 200});
	line.rotation_x = -336.875f;
	line.rotation_y = 431.5f;
	line.rotation_z = 17;
	line.scale = {140.5f, 80.f};
	Controller controller(Mode::RotateXY);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	controller.Pointer(Input(PointerPhase::Press, {300, 200}, true));
	controller.Pointer(Input(PointerPhase::Move, {311, 193}, true));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\fr17\fscx140.5\b1\frx37.12\fry93.5}Text{\i1}続き)", controller.DraftChanges()[0].text);
	controller.Pointer(Input(PointerPhase::Move, {311, 193}, true, true, false, true));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\fr17\fscx140.5\b1\frx30\fry90}Text{\i1}続き)", controller.DraftChanges()[0].text);
	controller.Pointer(Input(PointerPhase::Move, {300, 200}, true));
	EXPECT_TRUE(controller.DraftChanges().empty());
	ASSERT_EQ(1u, controller.IncrementalChanges().size());
	EXPECT_EQ(line.text, controller.IncrementalChanges()[0].text);
	controller.Pointer(Input(PointerPhase::Release, {300, 200}));
	EXPECT_TRUE(controller.DraftChanges().empty());
}

TEST(visual_edit_controller, rotate_xy_rejects_empty_invisible_and_nonfinite_input_without_losing_last_preview) {
	auto line = MakeLine(1, 0, R"({\frx23\fry71}A)", {200, 200});
	line.rotation_x = 23;
	line.rotation_y = 71;
	Controller controller(Mode::RotateXY);
	controller.Reset({line}, {}, 1, {}, Canvas());
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Press, {300, 200}, true)).interaction_began);
	controller.SetSelection({1}, 1);
	controller.SetFrame({.number = 101});
	EXPECT_FALSE(controller.CaptureSnapshot(std::make_shared<VisualToolRenderContext>(), {}));
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Press, {300, 200}, true)).interaction_began);
	controller.SetFrame({});
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Press, {std::numeric_limits<float>::quiet_NaN(), 200.f}, true)).interaction_began);
	ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {300, 200}, true)).interaction_began);
	controller.Pointer(Input(PointerPhase::Move, {311, 193}, true));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	auto valid = controller.DraftChanges()[0].text;
	controller.Pointer(Input(PointerPhase::Move, {std::numeric_limits<float>::infinity(), 180.f}, true));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(valid, controller.DraftChanges()[0].text);
	EXPECT_TRUE(controller.IsInteracting());
	controller.FinishInteraction();
	controller.AcceptChanges();
	controller.SetViewport({.video_position = {10, 20}, .video_size = {0, 480}, .script_size = {640, 480}});
	EXPECT_FALSE(controller.CaptureSnapshot(std::make_shared<VisualToolRenderContext>(), {}));
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Press, {300, 200}, true)).interaction_began);
}

TEST(visual_edit_controller, rotate_xy_snapshot_is_immutable_constant_size_and_retains_safe_origin_at_edge_on_pose) {
	auto line = MakeLine(1, 0, R"({\org(100,100)\frx30\fry45\frz20\fax0.2\fay-0.1}A)", {140, 150});
	line.origin = Vector2D(100, 100);
	line.rotation_x = 30;
	line.rotation_y = 45;
	line.rotation_z = 20;
	line.fax = .2f;
	line.fay = -.1f;
	Controller controller(Mode::RotateXY);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	auto context = std::make_shared<VisualToolRenderContext>();
	auto snapshot = controller.CaptureSnapshot(context, {});
	ASSERT_TRUE(snapshot);
	TransformOverlayRecorder initial;
	snapshot->Draw(initial);
	ASSERT_EQ(1u, initial.perspective_batches.size());
	line.scale = {250, 0};
	controller.UpdateLines(std::span<Line const>(&line, 1));
	TransformOverlayRecorder scaled;
	controller.CaptureSnapshot(context, {})->Draw(scaled);
	EXPECT_TRUE(SamePerspectiveBatches(initial, scaled));
	line.rotation_x = 0;
	line.rotation_y = 90;
	line.rotation_z = 0;
	line.fax = line.fay = 0;
	controller.UpdateLines(std::span<Line const>(&line, 1));
	auto edge = controller.CaptureSnapshot(context, {});
	ASSERT_TRUE(edge);
	ASSERT_TRUE(edge->HitTest({100, 100}));
	EXPECT_EQ(std::optional<int>(1), edge->HitTest({100, 100})->line_id);
	TransformOverlayRecorder projected;
	edge->Draw(projected);
	ASSERT_EQ(1u, projected.perspective_batches.size());
	auto const& batch = projected.perspective_batches.front();
	EXPECT_EQ(Vector2D(100, 100), batch.transform.origin);
	EXPECT_FLOAT_EQ(90.f, batch.transform.angle_y);
	ASSERT_FALSE(batch.grid.empty());
	for (auto const& vertex : batch.grid)
		EXPECT_TRUE(std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z) && std::isfinite(vertex.opacity));
	TransformOverlayRecorder retained;
	snapshot->Draw(retained);
	EXPECT_TRUE(SamePerspectiveBatches(initial, retained));
}

TEST(visual_edit_controller, rotate_xy_unselected_active_preview_follows_hold_and_displayed_hit_test_only) {
	auto active = MakeLine(1, 0, R"({\frx23\fry71\frz52}Active)", {200, 200});
	active.rotation_x = 23;
	active.rotation_y = 71;
	active.rotation_z = 52;
	auto selected = MakeLine(2, 1, R"({\frx80\fry12\i1}Selected)", {300, 300});
	Controller controller(Mode::RotateXY);
	controller.Reset({active, selected}, {2}, 1, {}, Canvas());
	auto context = std::make_shared<VisualToolRenderContext>();
	auto before = controller.CaptureSnapshot(context, {});
	ASSERT_TRUE(before);
	ASSERT_TRUE(before->HitTest({200, 200}));
	VisualToolDragSnapshot displayed_miss(context);
	EXPECT_TRUE(controller.Pointer(Input(PointerPhase::Press, {200, 200}, true), &displayed_miss).interaction_began);
	controller.Pointer(Input(PointerPhase::Move, {211, 193}, true), &displayed_miss);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(2, controller.DraftChanges()[0].id);
	EXPECT_EQ(R"({\i1\frx37\fry93}Selected)", controller.DraftChanges()[0].text);
	auto preview = controller.CaptureSnapshot(context, {});
	ASSERT_TRUE(preview);
	TransformOverlayRecorder original;
	before->Draw(original);
	TransformOverlayRecorder drawn;
	preview->Draw(drawn);
	ASSERT_EQ(1u, original.perspective_batches.size());
	ASSERT_EQ(1u, drawn.perspective_batches.size());
	EXPECT_FALSE(SamePerspectiveBatches(original, drawn));
	EXPECT_FLOAT_EQ(37.f, drawn.perspective_batches.front().transform.angle_x);
	EXPECT_FLOAT_EQ(93.f, drawn.perspective_batches.front().transform.angle_y);
	controller.FinishInteraction();
	TransformOverlayRecorder retired;
	controller.CaptureSnapshot(context, {})->Draw(retired);
	EXPECT_TRUE(SamePerspectiveBatches(original, retired));
	TransformOverlayRecorder retained;
	preview->Draw(retained);
	EXPECT_TRUE(SamePerspectiveBatches(drawn, retained));
	EXPECT_EQ(std::optional<int>(1), controller.ActiveLine());
	EXPECT_EQ((std::set<int>{2}), controller.SelectedLines());
}

TEST(visual_edit_controller, rotate_xy_invalid_release_retires_capture_and_keeps_last_valid_draft) {
	struct Case {
		Viewport viewport;
		Vector2D release;
	};
	Case const cases[] = {
		{.viewport = Canvas(), .release = {std::numeric_limits<float>::quiet_NaN(), 100.f}},
		{.viewport = Canvas(), .release = {std::numeric_limits<float>::max() * .5f, 100.f}},
		{.viewport = {.video_position = {0, 0}, .video_size = {1, 1}, .script_size = {1, 1}}, .release = {std::numeric_limits<float>::max() * .75f, 100.f}},
	};
	for (auto const& sample : cases) {
		auto line = MakeLine(1, 0, R"({\org(100,100)\frx23\fry71}A)", {140, 140});
		line.origin = Vector2D(100, 100);
		line.rotation_x = 23;
		line.rotation_y = 71;
		Controller controller(Mode::RotateXY);
		controller.Reset({line}, {1}, 1, {}, sample.viewport);
		EXPECT_TRUE(controller.Pointer(Input(PointerPhase::Press, {200, 100}, true)).interaction_began);
		controller.Pointer(Input(PointerPhase::Move, {211, 93}, true));
		ASSERT_EQ(1u, controller.DraftChanges().size());
		auto const text = controller.DraftChanges()[0].text;
		EXPECT_EQ(R"({\org(100,100)\frx37\fry93}A)", text);
		TransformOverlayRecorder before;
		controller.CaptureSnapshot(std::make_shared<VisualToolRenderContext>(), {})->Draw(before);
		auto result = controller.Pointer(Input(PointerPhase::Release, sample.release));
		EXPECT_TRUE(result.interaction_ended);
		EXPECT_EQ(CaptureAction::Release, result.capture);
		EXPECT_FALSE(result.geometry_changed);
		EXPECT_FALSE(controller.IsInteracting());
		ASSERT_EQ(1u, controller.DraftChanges().size());
		EXPECT_EQ(text, controller.DraftChanges()[0].text);
		TransformOverlayRecorder after;
		controller.CaptureSnapshot(std::make_shared<VisualToolRenderContext>(), {})->Draw(after);
		EXPECT_EQ(1u, before.perspective_batches.size());
		EXPECT_TRUE(SamePerspectiveBatches(before, after));
		controller.AcceptChanges();
		EXPECT_TRUE(controller.DraftChanges().empty());
	}
}

TEST(visual_edit_controller, rotate_xy_rejects_origin_projection_overflow_and_atomic_target_addition_overflow) {
	auto first = MakeLine(1, 0, R"({\org(100,100)\frx23\fry71}A)", {140, 140});
	first.origin = Vector2D(100, 100);
	first.rotation_x = 23;
	first.rotation_y = 71;
	auto second = MakeLine(2, 1, R"({\b1}B)", {150, 150});
	second.origin = Vector2D(std::numeric_limits<float>::max() * .75f, 100.f);
	Controller controller(Mode::RotateXY);
	controller.Reset({first, second}, {1, 2}, 1, {}, {.video_position = {0, 0}, .video_size = {1, 1}, .script_size = {1, 1}});
	auto context = std::make_shared<VisualToolRenderContext>();
	auto snapshot = controller.CaptureSnapshot(context, {});
	ASSERT_TRUE(snapshot);
	ASSERT_TRUE(snapshot->HitTest({100, 100}));
	EXPECT_TRUE(controller.Pointer(Input(PointerPhase::Press, {100, 100}, true), snapshot.get()).interaction_began);
	controller.Pointer(Input(PointerPhase::Move, {110, 100}, true));
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\frx23\fry71\org(110,100)}A)", controller.DraftChanges()[0].text);
	auto const first_text = controller.DraftChanges()[0].text;
	auto const second_text = controller.DraftChanges()[1].text;
	auto preview = controller.CaptureSnapshot(context, {});
	ASSERT_TRUE(preview);
	ASSERT_TRUE(preview->HitTest({110, 100}));
	TransformOverlayRecorder before;
	preview->Draw(before);
	Vector2D const invalid_point(std::numeric_limits<float>::max() * .5f, 100.f);
	auto moved = controller.Pointer(Input(PointerPhase::Move, invalid_point, true));
	EXPECT_FALSE(moved.geometry_changed);
	EXPECT_TRUE(controller.IsInteracting());
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(first_text, controller.DraftChanges()[0].text);
	EXPECT_EQ(second_text, controller.DraftChanges()[1].text);
	EXPECT_TRUE(controller.CaptureSnapshot(context, {})->HitTest({110, 100}));
	TransformOverlayRecorder after;
	controller.CaptureSnapshot(context, {})->Draw(after);
	EXPECT_EQ(1u, before.perspective_batches.size());
	EXPECT_TRUE(SamePerspectiveBatches(before, after));
	auto released = controller.Pointer(Input(PointerPhase::Release, invalid_point));
	EXPECT_TRUE(released.interaction_ended);
	EXPECT_EQ(CaptureAction::Release, released.capture);
	EXPECT_FALSE(controller.IsInteracting());
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(first_text, controller.DraftChanges()[0].text);
	EXPECT_EQ(second_text, controller.DraftChanges()[1].text);
	TransformOverlayRecorder retired;
	controller.CaptureSnapshot(context, {})->Draw(retired);
	EXPECT_TRUE(SamePerspectiveBatches(before, retired));
	first.origin = second.origin;
	controller.Reset({first}, {1}, 1, {}, {.video_position = {10, 20}, .video_size = {1280, 480}, .script_size = {640, 480}});
	EXPECT_FALSE(controller.CaptureSnapshot(context, {}));
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Press, {200, 100}, true)).interaction_began);
	EXPECT_TRUE(controller.DraftChanges().empty());
}

TEST(visual_transform_overlay, rotate_xy_preserves_transparency_and_independent_grid_fade) {
	agi::Color const primary{17, 34, 51, 73};
	agi::Color const secondary{68, 85, 102, 149};
	aegisub::visual_transform::RotateXYOverlayState const state{
		.origin = {130, 90},
		.angle_x = 11,
		.angle_y = -13,
		.angle_z = 17,
		.fax = .2f,
		.fay = -.1f,
		.perspective_z_scale = 8,
		.primary_colour = primary,
		.secondary_colour = secondary,
	};
	TransformOverlayRecorder drawn;
	aegisub::visual_transform::DrawRotateXYOverlay(drawn, state);
	ASSERT_EQ(1u, drawn.perspective_batches.size());
	auto const& batch = drawn.perspective_batches.front();
	EXPECT_EQ(primary, batch.primary);
	EXPECT_EQ(secondary, batch.secondary);
	EXPECT_EQ(2, batch.width);
	ASSERT_EQ(248u, batch.grid.size());
	ASSERT_EQ(6u, batch.axes.size());
	EXPECT_EQ(18u, batch.arrows.size());
	auto const ray = [&](VideoOverlayPerspectiveVertex const& from, VideoOverlayPerspectiveVertex const& to) {
		return from.x == 0 && from.y == 0 && from.z == 0 && from.opacity == 1 &&
			   to.x == 0 && to.y == -320 && to.z == 0 && to.opacity == 0;
	};
	int matches = 0;
	for (size_t i = 0; i < batch.grid.size(); i += 2) {
		EXPECT_GE(batch.grid[i].opacity, 0.f);
		EXPECT_LE(batch.grid[i].opacity, 1.f);
		EXPECT_GE(batch.grid[i + 1].opacity, 0.f);
		EXPECT_LE(batch.grid[i + 1].opacity, 1.f);
		matches += ray(batch.grid[i], batch.grid[i + 1]);
	}
	EXPECT_EQ(1, matches);
	for (auto const& vertex : batch.axes) EXPECT_FLOAT_EQ(1.f, vertex.opacity);
	for (auto const& vertex : batch.arrows) EXPECT_FLOAT_EQ(1.f, vertex.opacity);
	constexpr float expected_arrows[][2][3] = {
		{{60.f, 0.f, 0.f}, {50.f, -3.f, -3.f}},
		{{50.f, 3.f, -3.f}, {50.f, 3.f, 3.f}},
		{{50.f, -3.f, 3.f}, {50.f, -3.f, -3.f}},
		{{0.f, 60.f, 0.f}, {-3.f, 50.f, -3.f}},
		{{3.f, 50.f, -3.f}, {3.f, 50.f, 3.f}},
		{{-3.f, 50.f, 3.f}, {-3.f, 50.f, -3.f}},
		{{0.f, 0.f, 60.f}, {-3.f, -3.f, 50.f}},
		{{3.f, -3.f, 50.f}, {3.f, 3.f, 50.f}},
		{{-3.f, 3.f, 50.f}, {-3.f, -3.f, 50.f}},
	};
	ASSERT_GE(batch.arrows.size(), 18u);
	for (size_t i = 0; i < 9; ++i) {
		SCOPED_TRACE(i);
		auto const& first = batch.arrows[2 * i];
		auto const& second = batch.arrows[2 * i + 1];
		EXPECT_FLOAT_EQ(expected_arrows[i][0][0], first.x);
		EXPECT_FLOAT_EQ(expected_arrows[i][0][1], first.y);
		EXPECT_FLOAT_EQ(expected_arrows[i][0][2], first.z);
		EXPECT_FLOAT_EQ(expected_arrows[i][1][0], second.x);
		EXPECT_FLOAT_EQ(expected_arrows[i][1][1], second.y);
		EXPECT_FLOAT_EQ(expected_arrows[i][1][2], second.z);
	}
}

TEST(visual_transform_overlay, rotate_z_preserves_transparency_for_lines_and_highlight_fill) {
	agi::Color const primary{17, 34, 51, 73};
	agi::Color const secondary{68, 85, 102, 149};
	agi::Color const highlight{119, 136, 153, 211};
	aegisub::visual_transform::RotateZOverlayState const state{
		.origin = {100, 100},
		.pos = {150, 130},
		.scale = {140, 85},
		.rotation_x = 11,
		.rotation_y = -13,
		.angle = 37,
		.perspective_z_scale = 8,
		.primary_colour = primary,
		.secondary_colour = secondary,
		.highlight_colour = highlight,
	};
	TransformOverlayRecorder drawn;
	aegisub::visual_transform::DrawRotateZOverlay(drawn, state);
	ASSERT_FALSE(drawn.polygon_paints.empty());
	for (auto const& [stroke, fill] : drawn.polygon_paints) {
		EXPECT_EQ(secondary, stroke.colour);
		EXPECT_EQ(1.f, stroke.alpha);
		EXPECT_EQ(1, stroke.width);
		EXPECT_EQ(highlight, fill.colour);
		EXPECT_EQ(.3f, fill.alpha);
	}
	ASSERT_EQ(3u, drawn.line_paints.size());
	for (auto const& stroke : drawn.line_paints) {
		EXPECT_EQ(primary, stroke.colour);
		EXPECT_EQ(1.f, stroke.alpha);
		EXPECT_EQ(2, stroke.width);
	}
	ASSERT_EQ(2u, drawn.circle_paints.size());
	for (auto const& [stroke, fill] : drawn.circle_paints) {
		EXPECT_EQ(secondary, stroke.colour);
		EXPECT_EQ(1.f, stroke.alpha);
		EXPECT_EQ(1, stroke.width);
		EXPECT_EQ(highlight, fill.colour);
		EXPECT_EQ(.3f, fill.alpha);
	}
}

TEST(visual_transform_overlay, rotate_xy_near_camera_model_preserves_negative_x_front_ray) {
	using namespace aegisub::visual_transform;
	agi::Color const secondary{68, 85, 102, 149};
	RotateXYOverlayState const state{
		.origin = {220, 180},
		.angle_x = 75,
		.angle_y = 80,
		.angle_z = 15,
		.fax = .2f,
		.fay = -.1f,
		.perspective_z_scale = 640,
		.primary_colour = {17, 34, 51, 73},
		.secondary_colour = secondary,
	};
	TransformOverlayRecorder drawn;
	DrawRotateXYOverlay(drawn, state);
	ASSERT_EQ(1u, drawn.perspective_batches.size());
	auto const& batch = drawn.perspective_batches.front();
	EXPECT_EQ(secondary, batch.secondary);
	EXPECT_EQ(2, batch.width);
	EXPECT_EQ(state.origin, batch.transform.origin);
	EXPECT_FLOAT_EQ(75.f, batch.transform.angle_x);
	EXPECT_FLOAT_EQ(80.f, batch.transform.angle_y);
	EXPECT_FLOAT_EQ(15.f, batch.transform.angle_z);
	EXPECT_FLOAT_EQ(.2f, batch.transform.fax);
	EXPECT_FLOAT_EQ(-.1f, batch.transform.fay);
	EXPECT_FLOAT_EQ(640.f, batch.transform.perspective_z_scale);
	int matches = 0;
	for (size_t i = 0; i + 1 < batch.grid.size(); i += 2) {
		auto const& first = batch.grid[i];
		auto const& second = batch.grid[i + 1];
		if (first.x == 0 && first.y == 0 && first.z == 0 && first.opacity == 1 &&
			second.x == -320 && second.y == 0 && second.z == 0 && second.opacity == 0) ++matches;
	}
	EXPECT_EQ(1, matches);
}

TEST(visual_transform_overlay, prepared_geometry_owns_vertices_and_packed_colours) {
	auto const prepared = [] {
		constexpr std::array<VideoOverlayPerspectiveVertex, 2> grid{{
			{.x = -3.f, .y = 4.f, .z = 0.f, .opacity = .25f},
			{.x = 5.f, .y = 0.f, .z = -7.f, .opacity = .75f},
		}};
		constexpr VideoOverlayStaticPerspectiveBatches geometry(grid, grid, grid);
		static_assert(geometry.MaxCoordinate() == 7.f);
		return std::make_unique<VideoOverlayPreparedPerspectiveBatches>(geometry, agi::Color(51, 102, 153, 179));
	}();
	ASSERT_EQ(2u, prepared->Grid().size());
	ASSERT_EQ(2u, prepared->Axes().size());
	ASSERT_EQ(2u, prepared->Arrows().size());
	EXPECT_FLOAT_EQ(-3.f, prepared->Grid()[0].x);
	EXPECT_FLOAT_EQ(-7.f, prepared->Arrows()[1].z);
	EXPECT_FLOAT_EQ(7.f, prepared->MaxCoordinate());
	EXPECT_EQ(agi::Color(51, 102, 153, 179), prepared->SecondaryColour());
	ASSERT_EQ(8u, prepared->GridColours().size());
	for (size_t i = 0; i < 2; ++i) {
		EXPECT_FLOAT_EQ(.2f, prepared->GridColours()[i * 4]);
		EXPECT_FLOAT_EQ(.4f, prepared->GridColours()[i * 4 + 1]);
		EXPECT_FLOAT_EQ(.6f, prepared->GridColours()[i * 4 + 2]);
		EXPECT_FLOAT_EQ(i == 0 ? .25f : .75f, prepared->GridColours()[i * 4 + 3]);
	}
}

TEST(visual_transform_overlay, canonical_geometry_bound_and_cache_preserve_retained_colours) {
	using namespace aegisub::visual_transform;
	auto const& geometry = GetRotateXYOverlayGeometry();
	EXPECT_EQ(248u, geometry.Grid().size());
	EXPECT_EQ(6u, geometry.Axes().size());
	EXPECT_EQ(18u, geometry.Arrows().size());
	EXPECT_FLOAT_EQ(320.f, geometry.MaxCoordinate());
	std::shared_ptr<VideoOverlayPreparedPerspectiveBatches const> retained;
	{
		RotateXYOverlayGeometryCache cache;
		retained = cache.Get(agi::Color(51, 102, 153, 179));
		auto const same = cache.Get(agi::Color(51, 102, 153, 179));
		EXPECT_EQ(retained, same);
		auto const changed = cache.Get(agi::Color(153, 102, 51, 179));
		ASSERT_NE(retained, changed);
		EXPECT_EQ(changed, cache.Get(agi::Color(153, 102, 51, 179)));
		EXPECT_FLOAT_EQ(.2f, retained->GridColours()[0]);
		EXPECT_FLOAT_EQ(.6f, changed->GridColours()[0]);
		for (size_t i = 0; i < geometry.Grid().size(); ++i) {
			EXPECT_FLOAT_EQ(geometry.Grid()[i].opacity, retained->GridColours()[i * 4 + 3]);
			EXPECT_FLOAT_EQ(geometry.Grid()[i].opacity, changed->GridColours()[i * 4 + 3]);
		}
	}
	EXPECT_FLOAT_EQ(.2f, retained->GridColours()[0]);
	EXPECT_FLOAT_EQ(320.f, retained->MaxCoordinate());
}

TEST(visual_edit_controller, rotate_xy_snapshots_reuse_prepared_geometry_without_recolouring_retained_frames) {
	Controller controller(Mode::RotateXY);
	controller.Reset({MakeLine(1, 0, "{\\pos(100,100)}one", {100, 100})}, {1}, 1, {},
					 {.video_size = {640, 480}, .script_size = {640, 480}, .layout_size = {640, 480}});
	auto context = std::make_shared<VisualToolRenderContext>();
	Style const first_style{.line_primary = 0x00332211, .line_secondary = 0x00996633};
	Style const second_style{.line_primary = 0x00332211, .line_secondary = 0x00336699};
	auto first = controller.CaptureSnapshot(context, first_style);
	auto second = controller.CaptureSnapshot(context, first_style);
	auto recoloured = controller.CaptureSnapshot(context, second_style);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	ASSERT_TRUE(recoloured);
	TransformOverlayRecorder a, b, c, retained;
	first->Draw(a);
	second->Draw(b);
	recoloured->Draw(c);
	first->Draw(retained);
	ASSERT_EQ(1u, a.prepared_batches.size());
	ASSERT_EQ(1u, b.prepared_batches.size());
	ASSERT_EQ(1u, c.prepared_batches.size());
	EXPECT_EQ(a.prepared_batches.front(), b.prepared_batches.front());
	EXPECT_NE(a.prepared_batches.front(), c.prepared_batches.front());
	EXPECT_TRUE(SamePerspectiveBatches(a, retained));
	ASSERT_EQ(1u, a.perspective_batches.size());
	ASSERT_EQ(1u, c.perspective_batches.size());
	EXPECT_EQ(agi::Color(51, 102, 153), a.perspective_batches.front().secondary);
	EXPECT_EQ(agi::Color(153, 102, 51), c.perspective_batches.front().secondary);
	controller.Reset({MakeLine(2, 0, "{\\pos(200,150)}two", {200, 150})}, {2}, 2, {},
					 {.video_size = {640, 480}, .script_size = {640, 480}, .layout_size = {640, 480}});
	TransformOverlayRecorder reset;
	controller.CaptureSnapshot(context, second_style)->Draw(reset);
	ASSERT_EQ(1u, reset.prepared_batches.size());
	EXPECT_EQ(c.prepared_batches.front(), reset.prepared_batches.front());
	EXPECT_NE(c.perspective_batches.front().transform.origin, reset.perspective_batches.front().transform.origin);
}
