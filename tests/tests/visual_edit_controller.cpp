#include <main.h>

#include "../../src/visual_edit_controller.h"
#include "../../src/video_overlay_draw_context.h"
#include "../../src/visual_tool_drag_snapshot.h"
#include "../../src/visual_tool_transform_policy.h"
#include "../../src/visual_tool_transform_overlay.h"
#include "../../src/visual_tool_scale_policy.h"
#include "../../src/visual_tool_shear_shared.h"
#include "../../src/visual_ass_edit.h"
#include "../../src/visual_geometry_link.h"
#include "../../src/ass_dialogue.h"
#include "../../src/ass_style.h"

#include <libaegisub/vfr.h>

#include <algorithm>
#include <array>
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
	std::vector<std::pair<Paint, Paint>> rectangle_paints;
	struct Text {
		std::string text;
		int x;
		int y;
		VideoOverlayTextStyle style;
	};
	std::vector<Text> texts;
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
	void DrawRectangle(Vector2D, Vector2D) override { rectangle_paints.emplace_back(line_paint, fill_paint); }
	void DrawPolygon(Vector2D const *, size_t) override { polygon_paints.emplace_back(line_paint, fill_paint); }
	void DrawMultiPolygon(std::vector<float> const&, std::vector<int> const&, std::vector<int> const&, Vector2D, Vector2D, bool) override {}
	void DrawCircle(Vector2D, float) override { circle_paints.emplace_back(line_paint, fill_paint); }
	void DrawTriangle(Vector2D, Vector2D, Vector2D) override {}
	VideoOverlayTextExtent MeasureText(std::string const&, VideoOverlayTextStyle const&) override { return {}; }
	void DrawText(std::string const& text, int x, int y, VideoOverlayTextStyle const& style) override {
		texts.push_back({.text = text, .x = x, .y = y, .style = style});
	}
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

TEST(visual_edit_controller, recognized_double_click_sequences_preserve_the_first_click_hidden_selection) {
	for (bool second_press : {false, true}) {
		SCOPED_TRACE(second_press ? "native-second-press" : "direct-double-click");
		auto first = MakeLine(1, 0, R"({\pos(20,30)\org(10,10)}A)", {20, 30});
		first.origin = Vector2D(10, 10);
		auto second = MakeLine(2, 1, R"({\move(100,120,140,160,10,90)\org(90,100)}B)", {100, 120});
		second.move = Move{.start = {100, 120}, .end = {140, 160}, .start_ms = 10, .end_ms = 90};
		second.origin = Vector2D(90, 100);
		second.first_frame = 80;
		Controller controller(Mode::Drag);
		controller.Reset({first, second}, {1, 2}, 1, {.number = 12, .time_ms = 500}, Canvas());
		controller.Pointer(Input(PointerPhase::Press, {50, 70}, true));
		controller.Pointer(Input(PointerPhase::Release, {50, 70}));
		ASSERT_EQ((std::set<int>{1}), controller.SelectedLines());
		if (second_press)
			controller.Pointer(Input(PointerPhase::Press, {50, 70}, true));
		auto const result = controller.Pointer(Input(PointerPhase::DoubleClick, {50, 70}, second_press));
		EXPECT_TRUE(result.selection_changed);
		EXPECT_EQ((std::set<int>{1, 2}), controller.SelectedLines());
		ASSERT_EQ(2u, controller.DraftChanges().size());
		EXPECT_EQ(R"({\pos(50,70)\org(40,50)}A)", controller.DraftChanges()[0].text);
		EXPECT_EQ(R"({\move(130,160,170,200,10,90)\org(120,140)}B)", controller.DraftChanges()[1].text);
		controller.Pointer(Input(PointerPhase::Release, {50, 70}));
		EXPECT_EQ((std::set<int>{1, 2}), controller.SelectedLines());
		EXPECT_EQ(2u, controller.DraftChanges().size());
	}
}

TEST(visual_edit_controller, planned_double_click_second_press_retains_the_whole_captured_batch) {
	auto first = MakeLine(1, 0, R"({\pos(20,30)}A)", {20, 30});
	auto second = MakeLine(2, 1, R"({\move(100,120,140,160,10,90)\org(90,100)}B)", {100, 120});
	second.move = Move{.start = {100, 120}, .end = {140, 160}, .start_ms = 10, .end_ms = 90};
	second.origin = Vector2D(90, 100);
	second.first_frame = 80;
	Controller controller(Mode::Drag);
	controller.Reset({first, second}, {1, 2}, 1, {.number = 12, .time_ms = 500}, Canvas());
	int plans = 0;
	OverridePlanner planner = [&](std::span<OverrideEdit const> edits) -> std::optional<std::vector<VisualTextEdit>> {
		++plans;
		EXPECT_EQ((std::set<int>{1, 2}), controller.SelectedLines());
		EXPECT_EQ(3u, edits.size());
		for (auto const& edit : edits) {
			EXPECT_EQ(OverrideIntent::Translation, edit.intent);
			EXPECT_EQ(Vector2D(30, 40), edit.translation);
		}
		return std::vector<VisualTextEdit>{
			{.id = 1, .row = 0, .original_text = first.text, .text = R"({\pos(50,70)}A)"},
			{.id = 2, .row = 1, .original_text = second.text, .text = R"({\move(130,160,170,200,10,90)\org(120,140)}B)"}};
	};
	controller.Pointer(Input(PointerPhase::Press, {50, 70}, true), nullptr, true, planner);
	controller.BeginPreparedPress(true, planner);
	controller.Pointer(Input(PointerPhase::Release, {50, 70}), nullptr, false, planner);
	controller.Pointer(Input(PointerPhase::Press, {50, 70}, true), nullptr, true, planner);
	controller.BeginPreparedPress(true, planner);
	controller.Pointer(Input(PointerPhase::DoubleClick, {50, 70}, true), nullptr, false, planner);
	controller.Pointer(Input(PointerPhase::Release, {50, 70}), nullptr, false, planner);
	EXPECT_EQ(1, plans);
	EXPECT_EQ((std::set<int>{1, 2}), controller.SelectedLines());
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(first.text, controller.DraftChanges()[0].original_text);
	EXPECT_EQ(second.text, controller.DraftChanges()[1].original_text);
	EXPECT_EQ(R"({\pos(50,70)}A)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\move(130,160,170,200,10,90)\org(120,140)}B)", controller.DraftChanges()[1].text);
}

TEST(visual_edit_controller, ordinary_click_release_replaces_an_older_double_click_target) {
	for (bool second_press : {false, true}) {
		SCOPED_TRACE(second_press ? "native-second-press" : "direct-double-click");
		auto first = MakeLine(1, 0, R"({\pos(20,30)}A)", {20, 30});
		auto second = MakeLine(2, 1, R"({\pos(100,120)}B)", {100, 120});
		Controller controller(Mode::Drag);
		controller.Reset({first, second}, {1, 2}, 1, {}, Canvas());
		controller.Pointer(Input(PointerPhase::Press, {200, 200}, true));
		controller.Pointer(Input(PointerPhase::Release, {200, 200}));
		controller.Pointer(Input(PointerPhase::Press, {50, 70}, true));
		controller.Pointer(Input(PointerPhase::Release, {50, 70}));
		if (second_press)
			controller.Pointer(Input(PointerPhase::Press, {50, 70}, true));
		controller.Pointer(Input(PointerPhase::DoubleClick, {50, 70}, second_press));
		EXPECT_EQ((std::set<int>{1}), controller.SelectedLines());
		ASSERT_EQ(1u, controller.DraftChanges().size());
		EXPECT_EQ(1, controller.DraftChanges()[0].id);
		EXPECT_EQ(R"({\pos(50,70)}A)", controller.DraftChanges()[0].text);
	}
}

TEST(visual_edit_controller, external_owner_changes_retire_saved_double_click_targets) {
	enum class OwnerChange : std::uint8_t { Selection,
											Frame,
											Viewport,
											Lines,
											Reset,
											CaptureLoss };
	for (auto change : {OwnerChange::Selection, OwnerChange::Frame, OwnerChange::Viewport,
						OwnerChange::Lines, OwnerChange::Reset, OwnerChange::CaptureLoss}) {
		for (bool second_press : {false, true}) {
			SCOPED_TRACE(static_cast<int>(change));
			SCOPED_TRACE(second_press ? "native-second-press" : "direct-double-click");
			auto first = MakeLine(1, 0, R"({\pos(20,30)}A)", {20, 30});
			auto second = MakeLine(2, 1, R"({\pos(100,120)}B)", {100, 120});
			Controller controller(Mode::Drag);
			controller.Reset({first, second}, {1, 2}, 1, {}, Canvas());
			controller.Pointer(Input(PointerPhase::Press, {50, 70}, true));
			controller.Pointer(Input(PointerPhase::Release, {50, 70}));
			if (second_press || change == OwnerChange::CaptureLoss)
				controller.Pointer(Input(PointerPhase::Press, {50, 70}, true));
			switch (change) {
				case OwnerChange::Selection: controller.SetSelection({2}, 2); break;
				case OwnerChange::Frame: controller.SetFrame({.number = 1, .time_ms = 40}); break;
				case OwnerChange::Viewport: controller.SetViewport(Canvas()); break;
				case OwnerChange::Lines: controller.UpdateLines(std::array{first}); break;
				case OwnerChange::Reset: controller.Reset({first, second}, {2}, 2, {}, Canvas()); break;
				case OwnerChange::CaptureLoss: controller.FinishInteraction(); break;
			}
			auto const selected = controller.SelectedLines();
			controller.Pointer(Input(PointerPhase::DoubleClick, {50, 70}, second_press));
			EXPECT_EQ(selected, controller.SelectedLines());
			EXPECT_TRUE(controller.DraftChanges().empty());
		}
	}
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

TEST(visual_edit_controller, nudge_rotate_z_uses_active_angle_for_hidden_selection_and_accepts_repeated_commands) {
	auto active = MakeLine(1, 0, R"({\fr359.5\move(20,30,40,50,100,200)\b1}Active)", {20, 30});
	active.rotation_z = 359.5f;
	active.move = Move{.start = {20, 30}, .end = {40, 50}, .start_ms = 100, .end_ms = 200};
	auto hidden = MakeLine(2, 7, R"({\frz45\fax0.2}Hidden{\i1}tail)", {300, 200});
	hidden.rotation_z = 45;
	hidden.first_frame = 101;
	Controller controller(Mode::RotateZ);
	controller.Reset({active, hidden}, {1, 2}, 1, {}, Canvas());

	ASSERT_TRUE(controller.CanNudge());
	EXPECT_TRUE(controller.Nudge({1, 0}, .75f, 10).geometry_changed);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\move(20,30,40,50,100,200)\b1\frz0.25}Active)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\fax0.2\frz0.25}Hidden{\i1}tail)", controller.DraftChanges()[1].text);
	EXPECT_EQ(7, controller.DraftChanges()[1].row);
	EXPECT_EQ(hidden.text, controller.DraftChanges()[1].original_text);
	auto const accepted = controller.DraftChanges()[0].text;
	controller.AcceptChanges();
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_TRUE(controller.IncrementalChanges().empty());

	EXPECT_TRUE(controller.Nudge({-1, 0}, .5f, 10).geometry_changed);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(accepted, controller.DraftChanges()[0].original_text);
	EXPECT_EQ(R"({\move(20,30,40,50,100,200)\b1\frz359.8}Active)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\fax0.2\frz359.8}Hidden{\i1}tail)", controller.DraftChanges()[1].text);
	EXPECT_FALSE(controller.IsInteracting());
	EXPECT_EQ((std::set<int>{1, 2}), controller.SelectedLines());
}

TEST(visual_edit_controller, nudge_rotate_z_origin_uses_each_selected_source_without_editing_unselected_active) {
	auto active = MakeLine(1, 0, R"({\pos(400,300)\frz22}Active)", {400, 300});
	auto origin = MakeLine(2, 3, R"({\org(100,150)\move(20,30,40,50,100,200)\frz40}Origin)", {20, 30});
	origin.origin = Vector2D(100, 150);
	auto fallback = MakeLine(3, 8, R"({\pos(250,200)\fscx125}Fallback)", {250, 200});
	fallback.first_frame = 101;
	Controller controller(Mode::RotateZ);
	controller.Reset({active, origin, fallback}, {2, 3}, 1, {},
					 {.video_position = {10, 20}, .video_size = {1280, 480}, .script_size = {640, 480}});

	EXPECT_TRUE(controller.Nudge({0, -1}, 15, 2.5f).geometry_changed);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(2, controller.DraftChanges()[0].id);
	EXPECT_EQ(R"({\move(20,30,40,50,100,200)\frz40\org(100,147.5)}Origin)", controller.DraftChanges()[0].text);
	EXPECT_EQ(3, controller.DraftChanges()[1].id);
	EXPECT_EQ(R"({\pos(250,200)\fscx125\org(250,197.5)}Fallback)", controller.DraftChanges()[1].text);
	EXPECT_EQ((std::set<int>{2, 3}), controller.SelectedLines());
	EXPECT_EQ(std::optional<int>(1), controller.ActiveLine());
}

TEST(visual_edit_controller, nudge_rotate_xy_uses_active_axis_and_keeps_other_selected_axis) {
	auto active = MakeLine(1, 0, R"({\frx359.5\fry359.25\frz30}Active)", {100, 100});
	active.rotation_x = 359.5f;
	active.rotation_y = 359.25f;
	auto selected = MakeLine(2, 2, R"({\frx70\fry80\fax0.2\fay-0.1\frz45}Selected)", {200, 200});
	selected.rotation_x = 70;
	selected.rotation_y = 80;
	selected.first_frame = 101;
	Controller controller(Mode::RotateXY);
	controller.Reset({active, selected}, {2}, 1, {}, Canvas());

	EXPECT_TRUE(controller.Nudge({1, 0}, 1, 10).geometry_changed);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(2, controller.DraftChanges()[0].id);
	EXPECT_EQ(R"({\frx70\fax0.2\fay-0.1\frz45\fry0.25}Selected)", controller.DraftChanges()[0].text);
	controller.AcceptChanges();
	EXPECT_TRUE(controller.Nudge({0, -1}, 1, 10).geometry_changed);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\fax0.2\fay-0.1\frz45\fry0.25\frx0.5}Selected)", controller.DraftChanges()[0].text);
	EXPECT_EQ((std::set<int>{2}), controller.SelectedLines());
}

TEST(visual_edit_controller, nudge_scale_uses_active_pair_decimal_steps_and_zero_clamp) {
	auto active = MakeLine(1, 0, R"({\fscx0.5\fscy80.25}Active)", {100, 100});
	active.scale = {.5f, 80.25f};
	auto hidden = MakeLine(2, 5, R"({\fscx200\fscy50\move(20,30,40,50,100,200)\b1}Hidden)", {20, 30});
	hidden.scale = {200, 50};
	hidden.first_frame = 101;
	Controller controller(Mode::Scale);
	controller.Reset({active, hidden}, {1, 2}, 1, {}, Canvas());

	EXPECT_TRUE(controller.Nudge({-1, 0}, .75f, 10).geometry_changed);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\fscx0\fscy80.25}Active)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\move(20,30,40,50,100,200)\b1\fscx0\fscy80.25}Hidden)", controller.DraftChanges()[1].text);
	controller.AcceptChanges();
	EXPECT_TRUE(controller.Nudge({0, -1}, .25f, 10).geometry_changed);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\fscx0\fscy80.5}Active)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\move(20,30,40,50,100,200)\b1\fscx0\fscy80.5}Hidden)", controller.DraftChanges()[1].text);
}

TEST(visual_edit_controller, normalize_scale_uses_each_hidden_source_ratio_without_editing_unselected_active) {
	using Axis = visual_tool_scale_policy::Axis;
	auto active = MakeLine(1, 0, R"({\fscx123\fscy456}Active)", {100, 100});
	active.scale = {123, 456};
	auto first = MakeLine(2, 4, R"({\fscx200\fscy50\move(20,30,40,50,100,200)\b1}First)", {20, 30});
	first.scale = {200, 50};
	auto hidden = MakeLine(3, 9, R"({\fscx80\fscy120\fax0.2}Hidden{\i1}tail)", {300, 200});
	hidden.scale = {80, 120};
	hidden.first_frame = 101;
	Controller controller(Mode::Scale);
	controller.Reset({active, first, hidden}, {2, 3}, 1, {}, Canvas());

	ASSERT_TRUE(controller.CanNormalizeScale(Axis::X));
	EXPECT_TRUE(controller.NormalizeScale(Axis::X).geometry_changed);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(2, controller.DraftChanges()[0].id);
	EXPECT_EQ(R"({\move(20,30,40,50,100,200)\b1\fscx100\fscy25}First)", controller.DraftChanges()[0].text);
	EXPECT_EQ(3, controller.DraftChanges()[1].id);
	EXPECT_EQ(R"({\fax0.2\fscx100\fscy150}Hidden{\i1}tail)", controller.DraftChanges()[1].text);
	EXPECT_EQ((std::set<int>{2, 3}), controller.SelectedLines());
	controller.AcceptChanges();

	ASSERT_TRUE(controller.CanNormalizeScale(Axis::Y));
	EXPECT_TRUE(controller.NormalizeScale(Axis::Y).geometry_changed);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\move(20,30,40,50,100,200)\b1\fscx400\fscy100}First)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\fax0.2\fscx66.667\fscy100}Hidden{\i1}tail)", controller.DraftChanges()[1].text);
}

TEST(visual_edit_controller, normalize_scale_rejects_invalid_selected_axis_atomically_and_allows_dependent_zero) {
	using Axis = visual_tool_scale_policy::Axis;
	auto valid = MakeLine(1, 0, R"({\fscx200\fscy50\b1}Valid)", {100, 100});
	valid.scale = {200, 50};
	auto zero = MakeLine(2, 5, R"({\fscx0\fscy50\frz20}Zero)", {300, 200});
	zero.scale = {0, 50};
	zero.first_frame = 101;
	Controller controller(Mode::Scale);
	controller.Reset({valid, zero}, {1, 2}, 1, {}, Canvas());

	EXPECT_FALSE(controller.CanNormalizeScale(Axis::X));
	EXPECT_FALSE(controller.NormalizeScale(Axis::X).geometry_changed);
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_TRUE(controller.IncrementalChanges().empty());
	ASSERT_TRUE(controller.CanNormalizeScale(Axis::Y));
	EXPECT_TRUE(controller.NormalizeScale(Axis::Y).geometry_changed);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(valid.text, controller.DraftChanges()[0].original_text);
	EXPECT_EQ(zero.text, controller.DraftChanges()[1].original_text);
	EXPECT_EQ(R"({\b1\fscx400\fscy100}Valid)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\frz20\fscx0\fscy100}Zero)", controller.DraftChanges()[1].text);
}

TEST(visual_edit_controller, discrete_commands_preserve_exact_noop_without_inserting_inherited_tags) {
	using Axis = visual_tool_scale_policy::Axis;
	auto inherited = MakeLine(1, 0, R"({\pos(100,100)\b1}Text{\i1}tail)", {100, 100});
	Controller controller(Mode::Scale);
	controller.Reset({inherited}, {1}, 1, {}, Canvas());
	ASSERT_TRUE(controller.CanNormalizeScale(Axis::X));
	EXPECT_FALSE(controller.NormalizeScale(Axis::X).geometry_changed);
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_TRUE(controller.IncrementalChanges().empty());

	auto zero = MakeLine(1, 0, R"({\fscx0.000\fscy080.250\b1}Exact{\i1}tail)", {100, 100});
	zero.scale = {0.f, 80.25f};
	controller.Reset({zero}, {1}, 1, {}, Canvas());
	EXPECT_FALSE(controller.Nudge({-1, 0}, 1, 10).geometry_changed);
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_TRUE(controller.IncrementalChanges().empty());
	EXPECT_TRUE(controller.Nudge({0, -1}, .25f, 10).geometry_changed);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(zero.text, controller.DraftChanges()[0].original_text);
}

TEST(visual_edit_controller, discrete_commands_require_supported_mode_visible_active_and_nonempty_selection) {
	using Axis = visual_tool_scale_policy::Axis;
	auto line = MakeLine(1, 0, R"({\fscx200\fscy50}Text)", {100, 100});
	line.scale = {200, 50};
	for (auto mode : {Mode::Drag, Mode::RectClip, Mode::RotateZ, Mode::RotateXY, Mode::Scale}) {
		SCOPED_TRACE(static_cast<int>(mode));
		Controller controller(mode);
		controller.Reset({line}, {1}, 1, {}, Canvas());
		bool const supports_nudge = mode == Mode::RotateZ || mode == Mode::RotateXY || mode == Mode::Scale;
		EXPECT_EQ(supports_nudge, controller.CanNudge());
		EXPECT_EQ(mode == Mode::Scale, controller.CanNormalizeScale(Axis::X));
		if (!supports_nudge) {
			EXPECT_FALSE(controller.Nudge({1, 0}, 1, 10).geometry_changed);
			EXPECT_TRUE(controller.DraftChanges().empty());
		}
		if (mode != Mode::Scale) {
			EXPECT_FALSE(controller.NormalizeScale(Axis::X).geometry_changed);
			EXPECT_TRUE(controller.DraftChanges().empty());
		}
		controller.SetFrame({.number = 101});
		EXPECT_FALSE(controller.CanNudge());
		EXPECT_FALSE(controller.CanNormalizeScale(Axis::X));
		EXPECT_FALSE(controller.Nudge({1, 0}, 1, 10).geometry_changed);
		EXPECT_FALSE(controller.NormalizeScale(Axis::X).geometry_changed);
		EXPECT_TRUE(controller.DraftChanges().empty());
		controller.SetFrame({});
		controller.SetSelection({}, 1);
		EXPECT_FALSE(controller.CanNudge());
		EXPECT_FALSE(controller.CanNormalizeScale(Axis::X));
		EXPECT_FALSE(controller.Nudge({1, 0}, 1, 10).geometry_changed);
		EXPECT_FALSE(controller.NormalizeScale(Axis::X).geometry_changed);
		EXPECT_TRUE(controller.DraftChanges().empty());
		controller.SetSelection({1}, std::nullopt);
		EXPECT_FALSE(controller.CanNudge());
		EXPECT_FALSE(controller.CanNormalizeScale(Axis::X));
		EXPECT_FALSE(controller.Nudge({1, 0}, 1, 10).geometry_changed);
		EXPECT_FALSE(controller.NormalizeScale(Axis::X).geometry_changed);
		EXPECT_TRUE(controller.DraftChanges().empty());
	}
}

TEST(visual_edit_controller, discrete_commands_reject_pointer_interaction_without_losing_its_draft) {
	using Axis = visual_tool_scale_policy::Axis;
	auto line = MakeLine(1, 0, R"({\pos(100,100)\fscx150\fscy80}Text)", {100, 100});
	line.scale = {150, 80};
	Controller controller(Mode::Scale);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {300, 200}, true)).interaction_began);
	controller.Pointer(Input(PointerPhase::Move, {340, 180}, true));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	auto const preview = controller.DraftChanges()[0].text;

	EXPECT_FALSE(controller.CanNudge());
	EXPECT_FALSE(controller.CanNormalizeScale(Axis::X));
	EXPECT_FALSE(controller.Nudge({1, 0}, 1, 10).geometry_changed);
	EXPECT_FALSE(controller.NormalizeScale(Axis::X).geometry_changed);
	EXPECT_TRUE(controller.IsInteracting());
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(preview, controller.DraftChanges()[0].text);
	EXPECT_EQ(line.text, controller.DraftChanges()[0].original_text);
	controller.FinishInteraction();
	controller.AcceptChanges();
	EXPECT_TRUE(controller.CanNudge());
	EXPECT_TRUE(controller.CanNormalizeScale(Axis::X));
}

TEST(visual_edit_controller, nudge_rejects_invalid_input_without_partial_geometry_or_draft_changes) {
	float const infinity = std::numeric_limits<float>::infinity();
	float const nan = std::numeric_limits<float>::quiet_NaN();
	struct InvalidInput {
		Vector2D direction;
		float step;
		float origin_step;
	};
	InvalidInput const invalid[] = {
		{.direction = {nan, 0.f}, .step = 1, .origin_step = 10},
		{.direction = {0.f, infinity}, .step = 1, .origin_step = 10},
		{.direction = {1, 0}, .step = nan, .origin_step = 10},
		{.direction = {1, 0}, .step = infinity, .origin_step = 10},
		{.direction = {0, 1}, .step = 1, .origin_step = nan},
		{.direction = {0, 1}, .step = 1, .origin_step = infinity},
		{.direction = {1, 0}, .step = -1, .origin_step = 10},
		{.direction = {0, 1}, .step = 1, .origin_step = -10},
	};
	for (auto mode : {Mode::RotateZ, Mode::RotateXY, Mode::Scale}) {
		SCOPED_TRACE(static_cast<int>(mode));
		auto line = MakeLine(1, 0, R"({\pos(100,100)\fscx150\fscy80\frz30\frx20\fry10}Text)", {100, 100});
		line.scale = {150, 80};
		line.rotation_z = 30;
		line.rotation_x = 20;
		line.rotation_y = 10;
		for (size_t i = 0; i < std::size(invalid); ++i) {
			SCOPED_TRACE(i);
			Controller controller(mode);
			controller.Reset({line}, {1}, 1, {}, Canvas());
			auto const& input = invalid[i];
			EXPECT_FALSE(controller.Nudge(input.direction, input.step, input.origin_step).geometry_changed);
			EXPECT_TRUE(controller.DraftChanges().empty());
			EXPECT_TRUE(controller.IncrementalChanges().empty());
			EXPECT_FALSE(controller.IsInteracting());
		}
	}
}

TEST(visual_edit_controller, nudge_origin_rejects_late_selected_overflow_before_mutating_earlier_rows) {
	auto first = MakeLine(1, 0, R"({\org(100,150)\frz20}First)", {100, 100});
	first.origin = Vector2D(100, 150);
	auto overflow = MakeLine(2, 8, R"({\b1}Overflow)", {300.f, std::numeric_limits<float>::max()});
	overflow.first_frame = 101;
	Controller controller(Mode::RotateZ);
	controller.Reset({first, overflow}, {1, 2}, 1, {}, Canvas());

	EXPECT_FALSE(controller.Nudge({0, 1}, 1, std::numeric_limits<float>::max()).geometry_changed);
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_TRUE(controller.IncrementalChanges().empty());
	controller.SetSelection({1}, 1);
	EXPECT_TRUE(controller.Nudge({0, -1}, 1, 2.5f).geometry_changed);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(first.text, controller.DraftChanges()[0].original_text);
	EXPECT_EQ(R"({\frz20\org(100,147.5)}First)", controller.DraftChanges()[0].text);
}

TEST(visual_edit_controller, opposite_nudges_restore_exact_burst_baseline) {
	auto line = MakeLine(1, 0, R"({\pos(100,100)\frz020.00\fscx100.00\fscy080.0\b1}Text)", {100, 100});
	line.rotation_z = 20;
	line.scale = {100, 80};
	for (auto mode : {Mode::RotateZ, Mode::RotateXY, Mode::Scale}) {
		Controller controller(mode);
		controller.Reset({line}, {1}, 1, {}, Canvas());
		ASSERT_TRUE(controller.Nudge({1, 0}, 1, 2).geometry_changed);
		ASSERT_TRUE(controller.Nudge({-1, 0}, 1, 2).geometry_changed);
		EXPECT_TRUE(controller.DraftChanges().empty());
		ASSERT_EQ(1u, controller.IncrementalChanges().size());
		EXPECT_EQ(line.text, controller.IncrementalChanges()[0].text);
	}
	Controller origin(Mode::RotateZ);
	origin.Reset({line}, {1}, 1, {}, Canvas());
	ASSERT_TRUE(origin.Nudge({0, 1}, 1, 2).geometry_changed);
	ASSERT_TRUE(origin.Nudge({0, -1}, 1, 2).geometry_changed);
	EXPECT_TRUE(origin.DraftChanges().empty());
	EXPECT_EQ(line.text, origin.IncrementalChanges()[0].text);
}

namespace {
class CrossOverlayRecorder final : public VideoOverlayDrawContext {
	public:
	VideoOverlayTextExtent extent{37, 19};
	std::vector<std::string> operations;
	std::vector<float> vertices;
	size_t dimensions = 0;
	size_t vertex_count = 0;
	agi::Color line_colour;
	float line_alpha = 0;
	int line_width = 0;
	std::string measured_text;
	VideoOverlayTextStyle measured_style;
	std::string drawn_text;
	VideoOverlayTextStyle drawn_style;
	int text_x = 0;
	int text_y = 0;
	void SetLineColour(agi::Color const& colour, float alpha, int width) override {
		line_colour = colour;
		line_alpha = alpha;
		line_width = width;
	}
	void SetFillColour(agi::Color const&, float) override {}
	void SetInvert() override { operations.emplace_back("invert"); }
	void ClearInvert() override { operations.emplace_back("clear"); }
	void DrawLine(Vector2D, Vector2D) override { operations.emplace_back("line"); }
	void DrawLines(size_t dim, float const *lines, size_t n) override {
		operations.emplace_back("lines");
		dimensions = dim;
		vertex_count = n;
		vertices.assign(lines, lines + dim * n);
	}
	void DrawLineStrip(Vector2D const *, size_t) override {}
	void DrawRectangle(Vector2D, Vector2D) override {}
	void DrawPolygon(Vector2D const *, size_t) override {}
	void DrawMultiPolygon(std::vector<float> const&, std::vector<int> const&, std::vector<int> const&, Vector2D, Vector2D, bool) override {}
	void DrawCircle(Vector2D, float) override {}
	void DrawTriangle(Vector2D, Vector2D, Vector2D) override {}
	VideoOverlayTextExtent MeasureText(std::string const& text, VideoOverlayTextStyle const& style) override {
		operations.emplace_back("measure");
		measured_text = text;
		measured_style = style;
		return extent;
	}
	void DrawText(std::string const& text, int x, int y, VideoOverlayTextStyle const& style) override {
		operations.emplace_back("text");
		drawn_text = text;
		drawn_style = style;
		text_x = x;
		text_y = y;
	}
};

Viewport CrossCanvas() {
	return {.video_position = {40, 30}, .video_size = {800, 600}, .script_size = {400, 300}, .canvas_size = {1000, 800}};
}
}

TEST(visual_edit_controller, cross_hover_draws_full_canvas_invert_batch_and_measured_label_without_lines) {
	Controller controller(Mode::Cross);
	controller.Reset({}, {}, {}, {}, CrossCanvas());
	controller.Pointer(Input(PointerPhase::Enter, {140.5f, 90.5f}));
	auto context = std::make_shared<VisualToolRenderContext>();
	auto snapshot = controller.CaptureSnapshot(context, {});
	ASSERT_TRUE(snapshot);
	EXPECT_EQ(context, snapshot->context);
	EXPECT_FALSE(snapshot->HitTest({140.5f, 90.5f}));

	CrossOverlayRecorder draw;
	snapshot->Draw(draw);
	EXPECT_EQ((std::vector<std::string>{"invert", "lines", "clear", "measure", "text"}), draw.operations);
	EXPECT_EQ(2u, draw.dimensions);
	EXPECT_EQ(4u, draw.vertex_count);
	EXPECT_EQ((std::vector<float>{0, 90.5f, 1000, 90.5f, 140.5f, 0, 140.5f, 800}), draw.vertices);
	EXPECT_EQ(agi::Color(255, 255, 255), draw.line_colour);
	EXPECT_EQ(1, draw.line_width);
	EXPECT_FLOAT_EQ(1, draw.line_alpha);
	EXPECT_EQ("50.25,30.25", draw.measured_text);
	EXPECT_EQ(draw.measured_text, draw.drawn_text);
	EXPECT_EQ("Verdana", draw.measured_style.face);
	EXPECT_EQ("Verdana", draw.drawn_style.face);
	EXPECT_EQ(12, draw.drawn_style.size);
	EXPECT_TRUE(draw.drawn_style.bold);
	EXPECT_FALSE(draw.drawn_style.italic);
	EXPECT_TRUE(draw.drawn_style.outline);
	EXPECT_EQ(agi::Color(255, 255, 255), draw.drawn_style.colour);
	EXPECT_EQ(144, draw.text_x);
	EXPECT_EQ(93, draw.text_y);
	EXPECT_TRUE(controller.DraftChanges().empty());
}

TEST(visual_edit_controller, cross_label_uses_independent_extent_quadrants_shift_and_font_size_clamps) {
	struct Case {
		Vector2D pointer;
		bool shift;
		int requested_font;
		int font;
		VideoOverlayTextExtent extent;
		char const *text;
		int x;
		int y;
	};
	Case cases[] = {
		{.pointer = {140.5f, 90.5f}, .shift = false, .requested_font = 5, .font = 6, .extent = {71, 23}, .text = "50.25,30.25", .x = 144, .y = 93},
		{.pointer = {740.5f, 90.5f}, .shift = false, .requested_font = 73, .font = 72, .extent = {71, 23}, .text = "350.25,30.25", .x = 665, .y = 93},
		{.pointer = {140.5f, 690.5f}, .shift = false, .requested_font = 17, .font = 17, .extent = {71, 23}, .text = "50.25,330.25", .x = 144, .y = 664},
		{.pointer = {740.5f, 690.5f}, .shift = true, .requested_font = 6, .font = 6, .extent = {11, 7}, .text = "49.75,-30.25", .x = 725, .y = 680},
		{.pointer = {500, 400}, .shift = false, .requested_font = 72, .font = 72, .extent = {71, 23}, .text = "230,185", .x = 504, .y = 374},
		{.pointer = {20.5f, 10.5f}, .shift = false, .requested_font = 12, .font = 12, .extent = {71, 23}, .text = "-9.75,-9.75", .x = 24, .y = 13}};
	for (size_t i = 0; i < std::size(cases); ++i) {
		SCOPED_TRACE(i);
		auto const& item = cases[i];
		Controller controller(Mode::Cross);
		controller.Reset({}, {}, {}, {}, CrossCanvas());
		controller.Pointer(Input(PointerPhase::Move, item.pointer, false, false, false, item.shift));
		auto snapshot = controller.CaptureSnapshot({}, {.coordinate_font_size = item.requested_font});
		ASSERT_TRUE(snapshot);
		CrossOverlayRecorder draw;
		draw.extent = item.extent;
		snapshot->Draw(draw);
		EXPECT_EQ(item.text, draw.drawn_text);
		EXPECT_EQ(item.text, draw.measured_text);
		EXPECT_EQ(item.x, draw.text_x);
		EXPECT_EQ(item.y, draw.text_y);
		EXPECT_EQ(item.font, draw.drawn_style.size);
		EXPECT_EQ(item.font, draw.measured_style.size);
		EXPECT_EQ(item.pointer.Y(), draw.vertices.at(1));
		EXPECT_EQ(item.pointer.X(), draw.vertices.at(4));
	}
}

TEST(visual_edit_controller, cross_label_precision_changes_only_above_script_width_and_truncates_negative_coordinates) {
	for (float video_width : {399.f, 400.f, 800.f}) {
		SCOPED_TRACE(video_width);
		auto viewport = CrossCanvas();
		viewport.video_size = {video_width, 600.f};
		Controller controller(Mode::Cross);
		controller.Reset({}, {}, {}, {}, viewport);
		controller.Pointer(Input(PointerPhase::Enter, {20.5f, 10.5f}));
		auto snapshot = controller.CaptureSnapshot({}, {});
		ASSERT_TRUE(snapshot);
		CrossOverlayRecorder draw;
		snapshot->Draw(draw);
		EXPECT_EQ(video_width > 400 ? "-9.75,-9.75" : "-19,-9", draw.drawn_text);
	}
}

TEST(visual_edit_controller, cross_retained_snapshot_owns_pointer_viewport_style_and_context_after_reset_and_destroy) {
	std::shared_ptr<VisualToolRenderSnapshot const> retained;
	auto context = std::make_shared<VisualToolRenderContext>();
	{
		Controller controller(Mode::Cross);
		controller.Reset({}, {}, {}, {}, CrossCanvas());
		controller.Pointer(Input(PointerPhase::Enter, {740.5f, 690.5f}));
		retained = controller.CaptureSnapshot(context, {.coordinate_font_size = 17});
		controller.Pointer(Input(PointerPhase::Move, {20, 10}, false, false, false, true));
		controller.Reset({}, {}, {}, {}, {.video_position = {0, 0}, .video_size = {100, 100}, .script_size = {100, 100}, .canvas_size = {200, 200}});
	}
	ASSERT_TRUE(retained);
	EXPECT_EQ(context, retained->context);
	CrossOverlayRecorder draw;
	retained->Draw(draw);
	EXPECT_EQ("350.25,330.25", draw.drawn_text);
	EXPECT_EQ(17, draw.drawn_style.size);
	EXPECT_EQ((std::vector<float>{0, 690.5f, 1000, 690.5f, 740.5f, 0, 740.5f, 800}), draw.vertices);
	EXPECT_EQ(699, draw.text_x);
	EXPECT_EQ(668, draw.text_y);
}

TEST(visual_edit_controller, cross_press_drag_release_never_capture_or_select_and_leave_clears_until_reentry) {
	auto first = MakeLine(1, 0, R"({\pos(50,60)}First)", {50, 60});
	auto selected = MakeLine(2, 1, R"({\pos(100,120)}Selected)", {100, 120});
	Controller controller(Mode::Cross);
	controller.Reset({first, selected}, {2}, 1, {}, CrossCanvas());
	for (auto phase : {PointerPhase::Enter, PointerPhase::Press, PointerPhase::Move, PointerPhase::Release}) {
		SCOPED_TRACE(static_cast<int>(phase));
		auto result = controller.Pointer(Input(phase, {140, 150}, phase != PointerPhase::Release));
		EXPECT_EQ(CaptureAction::None, result.capture);
		EXPECT_FALSE(result.interaction_began);
		EXPECT_FALSE(result.interaction_ended);
		EXPECT_FALSE(result.selection_changed);
		EXPECT_FALSE(result.geometry_changed);
		EXPECT_FALSE(controller.IsInteracting());
		EXPECT_EQ((std::set<int>{2}), controller.SelectedLines());
		EXPECT_EQ(std::optional<int>(1), controller.ActiveLine());
		EXPECT_TRUE(controller.DraftChanges().empty());
		EXPECT_TRUE(controller.IncrementalChanges().empty());
		ASSERT_TRUE(controller.CaptureSnapshot({}, {}));
	}
	controller.Pointer(Input(PointerPhase::Leave, {140, 150}));
	auto absent = controller.CaptureSnapshot({}, {});
	if (absent) {
		CrossOverlayRecorder draw;
		absent->Draw(draw);
		EXPECT_TRUE(draw.operations.empty());
	}
	controller.Pointer(Input(PointerPhase::Enter, {200, 210}));
	auto reentered = controller.CaptureSnapshot({}, {});
	ASSERT_TRUE(reentered);
	CrossOverlayRecorder draw;
	reentered->Draw(draw);
	EXPECT_EQ("80,90", draw.drawn_text);
}

TEST(visual_edit_controller, cross_double_click_without_press_uses_unselected_move_start_and_translates_hidden_selection) {
	auto active = MakeLine(1, 0, R"({\move(20,30,220,230,0,1000)}Active)", {20, 30});
	active.move = Move{.start = {20, 30}, .end = {220, 230}, .start_ms = 0, .end_ms = 1000};
	auto hidden = MakeLine(2, 4, R"({\move(100,120,140,160,0,90)\org(90,100)\iclip(1,2,30,40)}Hidden{\i1}tail)", {100, 120});
	hidden.move = Move{.start = {100, 120}, .end = {140, 160}, .start_ms = 0, .end_ms = 90};
	hidden.origin = Vector2D(90, 100);
	hidden.first_frame = 80;
	auto untimed = MakeLine(3, 5, R"({\move(200,220,240,260,-5,0)\clip(1,2,30,40)}Untimed)", {200, 220});
	untimed.move = Move{.start = {200, 220}, .end = {240, 260}, .start_ms = -5, .end_ms = 0};
	auto positioned = MakeLine(4, 6, R"({\pos(300,320)\b1}Position)", {300, 320});
	auto start_timed = MakeLine(5, 7, R"({\move(400,420,440,460,10,0)}StartTimed)", {400, 420});
	start_timed.move = Move{.start = {400, 420}, .end = {440, 460}, .start_ms = 10, .end_ms = 0};
	Controller controller(Mode::Cross);
	controller.Reset({active, hidden, untimed, positioned, start_timed}, {2, 3, 4, 5}, 1, {.number = 12, .time_ms = 500}, CrossCanvas());
	auto result = controller.Pointer(Input(PointerPhase::DoubleClick, {140, 170}, false, false, false, true));
	EXPECT_TRUE(result.geometry_changed);
	EXPECT_EQ(CaptureAction::None, result.capture);
	EXPECT_FALSE(result.interaction_began);
	EXPECT_FALSE(result.interaction_ended);
	EXPECT_FALSE(result.selection_changed);
	EXPECT_FALSE(controller.IsInteracting());
	EXPECT_EQ((std::set<int>{2, 3, 4, 5}), controller.SelectedLines());
	EXPECT_EQ(std::optional<int>(1), controller.ActiveLine());
	ASSERT_EQ(4u, controller.DraftChanges().size());
	EXPECT_EQ(hidden.text, controller.DraftChanges()[0].original_text);
	EXPECT_EQ(2, controller.DraftChanges()[0].id);
	EXPECT_EQ(R"({\iclip(1,2,30,40)\move(130,160,170,200,0,90)\org(120,140)}Hidden{\i1}tail)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\clip(1,2,30,40)\move(230,260,270,300)}Untimed)", controller.DraftChanges()[1].text);
	EXPECT_EQ(R"({\b1\pos(330,360)}Position)", controller.DraftChanges()[2].text);
	EXPECT_EQ(R"({\move(430,460,470,500,10,0)}StartTimed)", controller.DraftChanges()[3].text);
}

TEST(visual_edit_controller, cross_double_click_serializes_fractional_and_negative_integer_coordinates_using_zoom_policy) {
	for (float scale : {1.f, 2.f}) {
		SCOPED_TRACE(scale);
		auto line = MakeLine(1, 0, R"({\pos(20,30)\org(10,10)\clip(0,0,40,40)}Text)", {20, 30});
		line.origin = Vector2D(10, 10);
		Controller controller(Mode::Cross);
		controller.Reset({line}, {1}, 1, {}, {.video_position = {0, 0}, .video_size = {640 * scale, 480 * scale}, .script_size = {640, 480}, .canvas_size = {1280, 960}});
		auto result = controller.Pointer(Input(PointerPhase::DoubleClick, {-1.5f * scale, -3.5f * scale}));
		EXPECT_TRUE(result.geometry_changed);
		ASSERT_EQ(1u, controller.DraftChanges().size());
		EXPECT_EQ(scale == 1 ? R"({\clip(0,0,40,40)\pos(-1,-3)\org(-11,-23)}Text)" : R"({\clip(0,0,40,40)\pos(-1.5,-3.5)\org(-11.5,-23.5)}Text)", controller.DraftChanges()[0].text);
	}
}

TEST(visual_edit_controller, cross_override_planner_rejects_whole_batch_without_mutating_next_translation_anchor) {
	auto first = MakeLine(1, 0, R"({\pos(20,30)}First)", {20, 30});
	auto hidden = MakeLine(2, 4, R"({\move(100,120,140,160,10,90)\org(90,100)}Hidden)", {100, 120});
	hidden.move = Move{.start = {100, 120}, .end = {140, 160}, .start_ms = 10, .end_ms = 90};
	hidden.origin = Vector2D(90, 100);
	hidden.first_frame = 80;
	Controller controller(Mode::Cross);
	controller.Reset({first, hidden}, {1, 2}, 1, {}, CrossCanvas());
	bool accept = false;
	int calls = 0;
	OverridePlanner planner = [&](std::span<OverrideEdit const> edits) -> std::optional<std::vector<VisualTextEdit>> {
		++calls;
		EXPECT_EQ(3u, edits.size());
		for (auto const& edit : edits) {
			EXPECT_EQ(OverrideIntent::Translation, edit.intent);
			EXPECT_EQ(Vector2D(30, 40), edit.translation);
		}
		if (edits.size() == 3) {
			EXPECT_EQ(1, edits[0].id);
			EXPECT_EQ("\\pos", edits[0].tag);
			EXPECT_EQ("(50,70)", edits[0].value);
			EXPECT_EQ(2, edits[1].id);
			EXPECT_EQ("\\move", edits[1].tag);
			EXPECT_EQ("(130,160,170,200,10,90)", edits[1].value);
			EXPECT_EQ("\\org", edits[2].tag);
			EXPECT_EQ("(120,140)", edits[2].value);
		}
		if (!accept) return std::nullopt;
		return std::vector<VisualTextEdit>{
			{.id = 1, .row = 0, .original_text = first.text, .text = R"({\pos(50,70)}First)"},
			{.id = 2, .row = 4, .original_text = hidden.text, .text = R"({\move(130,160,170,200,10,90)\org(120,140)}Hidden)"}};
	};
	auto rejected = controller.Pointer(Input(PointerPhase::DoubleClick, {140, 170}), nullptr, false, planner);
	EXPECT_FALSE(rejected.geometry_changed);
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_TRUE(controller.IncrementalChanges().empty());
	EXPECT_EQ((std::set<int>{1, 2}), controller.SelectedLines());
	accept = true;
	EXPECT_TRUE(controller.Pointer(Input(PointerPhase::DoubleClick, {140, 170}), nullptr, false, planner).geometry_changed);
	EXPECT_EQ(2, calls);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(first.text, controller.DraftChanges()[0].original_text);
	EXPECT_EQ(R"({\pos(50,70)}First)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\move(130,160,170,200,10,90)\org(120,140)}Hidden)", controller.DraftChanges()[1].text);
}

TEST(visual_edit_controller, cross_invalid_pointer_preserves_last_snapshot_and_existing_draft) {
	auto line = MakeLine(1, 0, R"({\pos(20,30)}Text)", {20, 30});
	Controller controller(Mode::Cross);
	controller.Reset({line}, {1}, 1, {}, CrossCanvas());
	ASSERT_TRUE(controller.Pointer(Input(PointerPhase::DoubleClick, {140, 170})).geometry_changed);
	for (auto point : {Vector2D(std::numeric_limits<float>::quiet_NaN(), 70.f), Vector2D(50.f, std::numeric_limits<float>::infinity()), Vector2D(std::numeric_limits<float>::max(), 70.f)}) {
		for (auto phase : {PointerPhase::Move, PointerPhase::DoubleClick}) {
			SCOPED_TRACE(static_cast<int>(phase));
			Result result;
			EXPECT_NO_THROW(result = controller.Pointer(Input(phase, point)));
			EXPECT_FALSE(result.geometry_changed);
			EXPECT_EQ(CaptureAction::None, result.capture);
			EXPECT_TRUE(controller.IncrementalChanges().empty());
			ASSERT_EQ(1u, controller.DraftChanges().size());
			EXPECT_EQ(R"({\pos(50,70)}Text)", controller.DraftChanges()[0].text);
			auto snapshot = controller.CaptureSnapshot({}, {});
			ASSERT_TRUE(snapshot);
			CrossOverlayRecorder draw;
			snapshot->Draw(draw);
			EXPECT_EQ("50,70", draw.drawn_text);
		}
	}
}

TEST(visual_edit_controller, cross_invalid_viewport_or_late_selected_target_rejects_before_any_source_is_changed) {
	auto first = MakeLine(1, 0, R"({\pos(20,30)}First)", {20, 30});
	auto second = MakeLine(2, 4, R"({\pos(100,120)\org(90,100)}Second)", {100, 120});
	second.origin = Vector2D(90, 100);
	std::vector<Viewport> invalid_viewports;
	for (int i = 0; i < 5; ++i) {
		auto viewport = CrossCanvas();
		if (i == 0) viewport.video_size = {0, 600};
		if (i == 1) viewport.script_size = {400, 0};
		if (i == 2) viewport.video_position = {std::numeric_limits<float>::infinity(), 30.f};
		if (i == 3) viewport.video_size = {800.f, std::numeric_limits<float>::quiet_NaN()};
		if (i == 4) viewport.canvas_size = {0, 800};
		invalid_viewports.push_back(viewport);
	}
	for (auto const& invalid : invalid_viewports) {
		Controller controller(Mode::Cross);
		controller.Reset({first, second}, {1, 2}, 1, {}, invalid);
		EXPECT_FALSE(controller.Pointer(Input(PointerPhase::DoubleClick, {140, 170})).geometry_changed);
		EXPECT_TRUE(controller.DraftChanges().empty());
		EXPECT_TRUE(controller.IncrementalChanges().empty());
		controller.SetViewport(CrossCanvas());
		EXPECT_TRUE(controller.Pointer(Input(PointerPhase::DoubleClick, {140, 170})).geometry_changed);
		ASSERT_EQ(2u, controller.DraftChanges().size());
		EXPECT_EQ(R"({\pos(50,70)}First)", controller.DraftChanges()[0].text);
		EXPECT_EQ(R"({\pos(130,160)\org(120,140)}Second)", controller.DraftChanges()[1].text);
	}
	auto invalid_target = second;
	invalid_target.origin = Vector2D(std::numeric_limits<float>::infinity(), 100.f);
	Controller controller(Mode::Cross);
	controller.Reset({first, invalid_target}, {1, 2}, 1, {}, CrossCanvas());
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::DoubleClick, {140, 170})).geometry_changed);
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_TRUE(controller.IncrementalChanges().empty());
	controller.UpdateLines(std::span<Line const>(&second, 1));
	EXPECT_TRUE(controller.Pointer(Input(PointerPhase::DoubleClick, {140, 170})).geometry_changed);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(first.text, controller.DraftChanges()[0].original_text);
	EXPECT_EQ(R"({\pos(50,70)}First)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\pos(130,160)\org(120,140)}Second)", controller.DraftChanges()[1].text);
}

TEST(visual_edit_controller, cross_exact_noop_has_no_draft_while_implicit_and_noncanonical_position_are_serialized) {
	struct Case {
		char const *source;
		char const *expected;
		bool changed;
	};
	Case cases[] = {
		{.source = R"({\b1\pos(20,30)}Text)", .expected = R"({\b1\pos(20,30)}Text)", .changed = false},
		{.source = R"({\b1}Text)", .expected = R"({\b1\pos(20,30)}Text)", .changed = true},
		{.source = R"({\pos(020.00,030.0)\b1}Text)", .expected = R"({\b1\pos(20,30)}Text)", .changed = true}};
	for (size_t i = 0; i < std::size(cases); ++i) {
		SCOPED_TRACE(i);
		auto const& item = cases[i];
		auto line = MakeLine(1, 0, item.source, {20, 30});
		Controller controller(Mode::Cross);
		controller.Reset({line}, {1}, 1, {}, CrossCanvas());
		auto result = controller.Pointer(Input(PointerPhase::DoubleClick, {80, 90}));
		EXPECT_EQ(item.changed, result.geometry_changed);
		EXPECT_EQ(item.changed ? 1u : 0u, controller.DraftChanges().size());
		if (item.changed) {
			EXPECT_EQ(item.source, controller.DraftChanges()[0].original_text);
			EXPECT_EQ(item.expected, controller.DraftChanges()[0].text);
		}
		else
			EXPECT_TRUE(controller.IncrementalChanges().empty());
	}
}

TEST(visual_edit_controller, cross_no_active_off_frame_and_empty_selection_still_label_without_editing) {
	for (int partition = 0; partition < 3; ++partition) {
		SCOPED_TRACE(partition);
		auto line = MakeLine(1, 0, R"({\pos(20,30)}Text)", {20, 30});
		if (partition == 1) line.first_frame = 80;
		Controller controller(Mode::Cross);
		controller.Reset({line}, partition == 2 ? std::set<int>{} : std::set<int>{1}, partition == 0 ? std::nullopt : std::optional<int>{1}, {}, CrossCanvas());
		auto result = controller.Pointer(Input(PointerPhase::DoubleClick, {140, 170}));
		EXPECT_FALSE(result.geometry_changed);
		EXPECT_FALSE(result.selection_changed);
		EXPECT_EQ(CaptureAction::None, result.capture);
		EXPECT_TRUE(controller.DraftChanges().empty());
		EXPECT_TRUE(controller.IncrementalChanges().empty());
		auto snapshot = controller.CaptureSnapshot({}, {});
		ASSERT_TRUE(snapshot);
		CrossOverlayRecorder draw;
		snapshot->Draw(draw);
		EXPECT_EQ("50,70", draw.drawn_text);
	}
}

namespace {
Line ReadShearLine(int id, int row, std::string text, bool comment = false, int start_ms = 0) {
	AssDialogue dialogue;
	dialogue.Id = id;
	dialogue.Row = row;
	dialogue.Text = std::move(text);
	dialogue.Comment = comment;
	dialogue.Start = start_ms;
	dialogue.End = start_ms + 4000;
	return ReadLine(dialogue, nullptr, {640, 480}, agi::vfr::Framerate(25));
}
}

TEST(visual_edit_controller, shear_read_line_carries_whole_source_admission_and_refreshes_on_rebinding) {
	auto active = ReadShearLine(1, 0, R"({\pos(320,240)\fax0.25\fay-0.5}Active)");
	auto rejected = ReadShearLine(2, 5, R"({\fax0.125\t(0,500,\fay0.5)}Hidden)", true);
	EXPECT_TRUE(active.static_shear);
	EXPECT_FALSE(rejected.static_shear);
	EXPECT_EQ(.25f, active.fax);
	EXPECT_EQ(-.5f, active.fay);
	Controller controller(Mode::Shear);
	controller.Reset({active, rejected}, {1, 2}, 1, {}, Canvas());
	EXPECT_FALSE(controller.CanNudge());
	EXPECT_FALSE(controller.CanResetShear(aegisub::visual_shear::Axis::X));
	EXPECT_FALSE(controller.Nudge({1, 0}, .1f, 1).geometry_changed);
	EXPECT_FALSE(controller.ResetShear(aegisub::visual_shear::Axis::Y).geometry_changed);
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Press, {340, 140}, true)).interaction_began);
	EXPECT_TRUE(controller.DraftChanges().empty());

	Style style;
	style.shear_hint = "localized hint";
	style.shear_diagnostic = "localized unsupported selection";
	auto snapshot = controller.CaptureSnapshot({}, style);
	ASSERT_TRUE(snapshot);
	TransformOverlayRecorder draw;
	snapshot->Draw(draw);
	ASSERT_EQ(1u, draw.texts.size());
	EXPECT_EQ(style.shear_diagnostic, draw.texts.front().text);
	EXPECT_TRUE(draw.lines.empty());
	EXPECT_FALSE(snapshot->HitTest({340, 140}));

	auto replacement = ReadShearLine(2, 5, R"({\fax0.125\fay0.5}Hidden)", true);
	controller.UpdateLines(std::span<Line const>(&replacement, 1));
	EXPECT_TRUE(controller.CanNudge());
	EXPECT_TRUE(controller.CanResetShear(aegisub::visual_shear::Axis::Y));
	EXPECT_TRUE(controller.Nudge({1, 0}, .125f, 1).geometry_changed);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\pos(320,240)\fay-0.5\fax0.375}Active)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\fay0.5\fax0.25}Hidden)", controller.DraftChanges()[1].text);
}

TEST(visual_edit_controller, shear_nudge_uses_each_selected_coefficient_including_hidden_and_comment_rows) {
	auto active = ReadShearLine(1, 0, R"({\pos(320,240)\fax0.25\fay-0.5}Active)");
	auto hidden = ReadShearLine(2, 4, R"({\move(20,30,40,50,100,200)\fax-0.75\fay0.125\org(8,9)\clip(1,2,30,40)}Hidden{\i1}tail)", false, 8000);
	auto comment = ReadShearLine(3, 8, R"({\fax1.25\fay-0.25\fscx150\frz12}Comment)", true);
	Controller controller(Mode::Shear);
	controller.Reset({active, hidden, comment}, {1, 2, 3}, 1, {}, Canvas());
	ASSERT_TRUE(controller.CanNudge());
	EXPECT_TRUE(controller.Nudge({1, 0}, .125f, 1).geometry_changed);
	ASSERT_EQ(3u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\pos(320,240)\fay-0.5\fax0.375}Active)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\move(20,30,40,50,100,200)\fay0.125\org(8,9)\clip(1,2,30,40)\fax-0.625}Hidden{\i1}tail)", controller.DraftChanges()[1].text);
	EXPECT_EQ(R"({\fay-0.25\fscx150\frz12\fax1.375}Comment)", controller.DraftChanges()[2].text);
	EXPECT_EQ(8, controller.DraftChanges()[2].row);
	EXPECT_EQ(comment.text, controller.DraftChanges()[2].original_text);
	auto const accepted = controller.DraftChanges()[1].text;
	controller.AcceptChanges();
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_TRUE(controller.Nudge({0, -1}, .25f, 1).geometry_changed);
	ASSERT_EQ(3u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\pos(320,240)\fax0.375\fay-0.75}Active)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\move(20,30,40,50,100,200)\org(8,9)\clip(1,2,30,40)\fax-0.625\fay-0.125}Hidden{\i1}tail)", controller.DraftChanges()[1].text);
	EXPECT_EQ(accepted, controller.DraftChanges()[1].original_text);
	EXPECT_EQ(R"({\fscx150\frz12\fax1.375\fay-0.5}Comment)", controller.DraftChanges()[2].text);
}

TEST(visual_edit_controller, shear_unselected_active_is_anchor_only_and_does_not_join_admission_or_edits) {
	auto active = ReadShearLine(1, 0, R"({\pos(320,240)\fax0.25\fay-0.5\t(\fax1)}Active)");
	auto selected = ReadShearLine(2, 7, R"({\fax-0.75\fay0.125}Selected)", true);
	ASSERT_FALSE(active.static_shear);
	Controller controller(Mode::Shear);
	controller.Reset({active, selected}, {2}, 1, {}, Canvas());
	ASSERT_TRUE(controller.CanNudge());
	EXPECT_TRUE(controller.Nudge({1, 1}, .125f, 1).geometry_changed);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(2, controller.DraftChanges()[0].id);
	EXPECT_EQ(R"({\fay0.125\fax-0.625}Selected)", controller.DraftChanges()[0].text);
	EXPECT_EQ((std::set<int>{2}), controller.SelectedLines());
	EXPECT_EQ(std::optional<int>(1), controller.ActiveLine());
	controller.AcceptChanges();
	ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {340, 140}, true)).interaction_began);
	controller.Pointer(Input(PointerPhase::Release, {380, 180}));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(2, controller.DraftChanges()[0].id);
	EXPECT_EQ(R"({\fay0.125\fax-0.125}Selected)", controller.DraftChanges()[0].text);
	auto snapshot = controller.CaptureSnapshot({}, {});
	ASSERT_TRUE(snapshot);
	TransformOverlayRecorder draw;
	snapshot->Draw(draw);
	ASSERT_GE(draw.texts.size(), 2u);
	EXPECT_EQ("fax 0.25", draw.texts[0].text);
	EXPECT_EQ("fay -0.5", draw.texts[1].text);
}

TEST(visual_edit_controller, shear_axis_drags_use_screen_displacement_and_ignore_modifiers) {
	for (bool modified : {false, true}) {
		SCOPED_TRACE(modified);
		auto active = ReadShearLine(1, 0, R"({\pos(160,240)\fax0.25\fay-0.5\frz30}Active)");
		auto hidden = ReadShearLine(2, 5, R"({\fax-0.75\fay0.125}Hidden)", true);
		Controller controller(Mode::Shear);
		controller.Reset({active, hidden}, {1, 2}, 1, {},
						 {.video_position = {40, 30}, .video_size = {1280, 480}, .script_size = {640, 480}});
		auto press = controller.Pointer(Input(PointerPhase::Press, {380, 170}, true, modified, modified, modified));
		EXPECT_EQ(CaptureAction::Capture, press.capture);
		ASSERT_TRUE(press.interaction_began);
		EXPECT_FALSE(press.selection_changed);
		controller.Pointer(Input(PointerPhase::Move, {400, 250}, true, modified, modified, modified));
		ASSERT_EQ(2u, controller.DraftChanges().size());
		EXPECT_EQ(R"({\pos(160,240)\fay-0.5\frz30\fax0.5}Active)", controller.DraftChanges()[0].text);
		EXPECT_EQ(R"({\fay0.125\fax-0.5}Hidden)", controller.DraftChanges()[1].text);
		auto release = controller.Pointer(Input(PointerPhase::Release, {400, 250}, false, modified, modified, modified));
		EXPECT_EQ(CaptureAction::Release, release.capture);
		EXPECT_TRUE(release.interaction_ended);
		controller.AcceptChanges();
		ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {460, 230}, true, modified, modified, modified)).interaction_began);
		controller.Pointer(Input(PointerPhase::Release, {540, 210}, false, modified, modified, modified));
		ASSERT_EQ(2u, controller.DraftChanges().size());
		EXPECT_EQ(R"({\pos(160,240)\frz30\fax0.5\fay-0.75}Active)", controller.DraftChanges()[0].text);
		EXPECT_EQ(R"({\fax-0.5\fay-0.125}Hidden)", controller.DraftChanges()[1].text);
	}
}

TEST(visual_edit_controller, shear_background_and_double_click_preserve_selection_and_never_edit) {
	auto active = ReadShearLine(1, 0, R"({\pos(320,240)\fax0.25\fay-0.5}Active)");
	auto selected = ReadShearLine(2, 6, R"({\pos(100,120)\fax-0.75}Selected)");
	Controller controller(Mode::Shear);
	controller.Reset({active, selected}, {1, 2}, 1, {}, Canvas());
	for (auto phase : {PointerPhase::Press, PointerPhase::Move, PointerPhase::Release, PointerPhase::DoubleClick}) {
		auto result = controller.Pointer(Input(phase, {50, 50}, phase == PointerPhase::Press || phase == PointerPhase::Move));
		EXPECT_FALSE(result.interaction_began);
		EXPECT_FALSE(result.selection_changed);
		EXPECT_EQ(CaptureAction::None, result.capture);
		EXPECT_FALSE(controller.IsInteracting());
		EXPECT_EQ((std::set<int>{1, 2}), controller.SelectedLines());
		EXPECT_TRUE(controller.DraftChanges().empty());
	}
	auto double_click = controller.Pointer(Input(PointerPhase::DoubleClick, {340, 140}));
	EXPECT_FALSE(double_click.interaction_began);
	EXPECT_TRUE(controller.DraftChanges().empty());
}

TEST(visual_edit_controller, shear_reset_targets_only_requested_axis_and_preserves_noop_source) {
	using Axis = aegisub::visual_shear::Axis;
	auto active = ReadShearLine(1, 0, R"({\pos(320,240)\fax0.25\fay-0.5}Active)");
	auto first = ReadShearLine(2, 4, R"({\move(20,30,40,50,100,200)\fax-0.75\fay0.125\org(8,9)\iclip(1,2,30,40)}First)", false, 8000);
	auto comment = ReadShearLine(3, 8, R"({\fax1.25\fay-0.25\fscy80\frx14}Comment)", true);
	Controller controller(Mode::Shear);
	controller.Reset({active, first, comment}, {2, 3}, 1, {}, Canvas());
	ASSERT_TRUE(controller.CanResetShear(Axis::X));
	EXPECT_TRUE(controller.ResetShear(Axis::X).geometry_changed);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\move(20,30,40,50,100,200)\fay0.125\org(8,9)\iclip(1,2,30,40)\fax0}First)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\fay-0.25\fscy80\frx14\fax0}Comment)", controller.DraftChanges()[1].text);
	controller.AcceptChanges();
	EXPECT_FALSE(controller.ResetShear(Axis::X).geometry_changed);
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_TRUE(controller.ResetShear(Axis::Y).geometry_changed);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\move(20,30,40,50,100,200)\org(8,9)\iclip(1,2,30,40)\fax0\fay0}First)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\fscy80\frx14\fax0\fay0}Comment)", controller.DraftChanges()[1].text);

	auto implicit = ReadShearLine(4, 1, R"({\pos(320,240)\b1}Implicit{\i1}tail)");
	auto explicit_zero = ReadShearLine(5, 2, R"({\fax0.000\fay-0.000\b1}Exact)");
	controller.Reset({implicit, explicit_zero}, {4, 5}, 4, {}, Canvas());
	EXPECT_FALSE(controller.ResetShear(Axis::X).geometry_changed);
	EXPECT_FALSE(controller.ResetShear(Axis::Y).geometry_changed);
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_TRUE(controller.IncrementalChanges().empty());
}

TEST(visual_edit_controller, shear_return_to_drag_and_nudge_origin_restores_exact_bytes) {
	auto line = ReadShearLine(1, 0, R"({\pos(320,240)\fax00.2500\fay-00.5000\b1}Text{\i1}tail)");
	Controller controller(Mode::Shear);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {340, 140}, true)).interaction_began);
	controller.Pointer(Input(PointerPhase::Move, {360, 170}, true));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	controller.Pointer(Input(PointerPhase::Release, {340, 140}));
	EXPECT_TRUE(controller.DraftChanges().empty());
	ASSERT_EQ(1u, controller.IncrementalChanges().size());
	EXPECT_EQ(line.text, controller.IncrementalChanges()[0].text);
	controller.AcceptChanges();
	ASSERT_TRUE(controller.Nudge({0, -1}, .125f, 1).geometry_changed);
	ASSERT_TRUE(controller.Nudge({0, 1}, .125f, 1).geometry_changed);
	EXPECT_TRUE(controller.DraftChanges().empty());
	ASSERT_EQ(1u, controller.IncrementalChanges().size());
	EXPECT_EQ(line.text, controller.IncrementalChanges()[0].text);
}

TEST(visual_edit_controller, shear_late_overflow_and_invalid_drag_preserve_last_valid_batch) {
	auto active = ReadShearLine(1, 0, R"({\pos(320,240)\fax0.25\fay-0.5}Active)");
	auto hidden = ReadShearLine(2, 5, R"({\fax3e38\fay0.125}Hidden)", true);
	Controller controller(Mode::Shear);
	controller.Reset({active, hidden}, {1, 2}, 1, {}, Canvas());
	EXPECT_FALSE(controller.Nudge({1, 0}, 1e38f, 1).geometry_changed);
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_TRUE(controller.IncrementalChanges().empty());
	EXPECT_TRUE(controller.Nudge({0, 1}, .125f, 1).geometry_changed);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	auto const first_preview = controller.DraftChanges()[0].text;
	auto const second_preview = controller.DraftChanges()[1].text;
	EXPECT_FALSE(controller.Nudge({1, 0}, 1e38f, 1).geometry_changed);
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(first_preview, controller.DraftChanges()[0].text);
	EXPECT_EQ(second_preview, controller.DraftChanges()[1].text);
	controller.AcceptChanges();
	ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {420, 210}, true)).interaction_began);
	controller.Pointer(Input(PointerPhase::Move, {450, 230}, true));
	ASSERT_EQ(2u, controller.DraftChanges().size());
	auto const valid_drag = controller.DraftChanges()[0].text;
	float const nan = std::numeric_limits<float>::quiet_NaN();
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Move, {nan, 240.f}, true)).geometry_changed);
	EXPECT_EQ(valid_drag, controller.DraftChanges()[0].text);
	auto release = controller.Pointer(Input(PointerPhase::Release, {nan, 240.f}));
	EXPECT_TRUE(release.interaction_ended);
	EXPECT_EQ(CaptureAction::Release, release.capture);
	EXPECT_FALSE(controller.IsInteracting());
	EXPECT_EQ(valid_drag, controller.DraftChanges()[0].text);
}

TEST(visual_edit_controller, shear_commands_require_visible_active_selection_and_idle_supported_mode) {
	using Axis = aegisub::visual_shear::Axis;
	auto line = ReadShearLine(1, 0, R"({\pos(320,240)\fax0.25\fay-0.5}Text)");
	for (auto mode : {Mode::Drag, Mode::RectClip, Mode::RotateZ, Mode::RotateXY, Mode::Scale, Mode::Cross}) {
		Controller other(mode);
		other.Reset({line}, {1}, 1, {}, Canvas());
		EXPECT_FALSE(other.CanResetShear(Axis::X));
		EXPECT_FALSE(other.ResetShear(Axis::Y).geometry_changed);
		EXPECT_TRUE(other.DraftChanges().empty());
	}
	Controller controller(Mode::Shear);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	controller.SetFrame({.number = 500});
	EXPECT_FALSE(controller.CanNudge());
	EXPECT_FALSE(controller.CanResetShear(Axis::X));
	EXPECT_FALSE(controller.Nudge({1, 0}, .1f, 1).geometry_changed);
	EXPECT_FALSE(controller.ResetShear(Axis::Y).geometry_changed);
	controller.SetFrame({});
	controller.SetSelection({}, 1);
	EXPECT_FALSE(controller.CanNudge());
	EXPECT_FALSE(controller.CanResetShear(Axis::X));
	controller.SetSelection({1}, std::nullopt);
	EXPECT_FALSE(controller.CanNudge());
	EXPECT_FALSE(controller.CanResetShear(Axis::X));
	controller.SetSelection({1}, 1);
	ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {340, 140}, true)).interaction_began);
	controller.Pointer(Input(PointerPhase::Move, {360, 140}, true));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	auto const preview = controller.DraftChanges()[0].text;
	EXPECT_FALSE(controller.CanNudge());
	EXPECT_FALSE(controller.CanResetShear(Axis::X));
	EXPECT_FALSE(controller.Nudge({1, 0}, .1f, 1).geometry_changed);
	EXPECT_FALSE(controller.ResetShear(Axis::Y).geometry_changed);
	EXPECT_EQ(preview, controller.DraftChanges()[0].text);
	EXPECT_TRUE(controller.IsInteracting());
	controller.FinishInteraction();
	EXPECT_TRUE(controller.CanNudge());
	EXPECT_TRUE(controller.CanResetShear(Axis::Y));
}

TEST(visual_edit_controller, shear_drag_rejects_late_finite_overflow_before_any_selected_write) {
	auto active = ReadShearLine(1, 0, R"({\pos(320,240)\fax0.25\fay-0.5}Active)");
	auto hidden = ReadShearLine(2, 5, R"({\fax3.4e38\fay0.125}Hidden)", true);
	Controller controller(Mode::Shear);
	controller.Reset({active, hidden}, {1, 2}, 1, {}, Canvas());
	ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {340, 140}, true)).interaction_began);
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Move, {std::numeric_limits<float>::max(), 140.f}, true)).geometry_changed);
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_TRUE(controller.IncrementalChanges().empty());
	EXPECT_TRUE(controller.IsInteracting());
	controller.Pointer(Input(PointerPhase::Release, {360, 140}));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(active.text, controller.DraftChanges()[0].original_text);
	EXPECT_EQ(R"({\pos(320,240)\fay-0.5\fax0.5}Active)", controller.DraftChanges()[0].text);
}

TEST(visual_edit_controller, shear_invalid_command_arguments_and_geometry_preserve_source) {
	auto active = ReadShearLine(1, 0, R"({\pos(320,240)\fax0.25\fay-0.5}Active)");
	Controller controller(Mode::Shear);
	controller.Reset({active}, {1}, 1, {}, Canvas());
	float const nan = std::numeric_limits<float>::quiet_NaN();
	float const inf = std::numeric_limits<float>::infinity();
	EXPECT_FALSE(controller.Nudge({nan, 0.f}, .1f, 1).geometry_changed);
	EXPECT_FALSE(controller.Nudge({0.f, inf}, .1f, 1).geometry_changed);
	EXPECT_FALSE(controller.Nudge({1, 0}, nan, 1).geometry_changed);
	EXPECT_FALSE(controller.Nudge({1, 0}, inf, 1).geometry_changed);
	EXPECT_FALSE(controller.Nudge({1, 0}, 0, 1).geometry_changed);
	EXPECT_FALSE(controller.Nudge({1, 0}, -.1f, 1).geometry_changed);
	EXPECT_FALSE(controller.Nudge({0, 0}, .1f, 1).geometry_changed);
	EXPECT_TRUE(controller.DraftChanges().empty());
	auto viewport = Canvas();
	viewport.video_size = {0, 480};
	controller.SetViewport(viewport);
	EXPECT_FALSE(controller.CaptureSnapshot({}, {}));
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Press, {340, 140}, true)).interaction_began);
	EXPECT_TRUE(controller.DraftChanges().empty());
	controller.SetViewport(Canvas());
	EXPECT_TRUE(controller.Nudge({1, 0}, .125f, 1).geometry_changed);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(active.text, controller.DraftChanges()[0].original_text);
	EXPECT_EQ(R"({\pos(320,240)\fay-0.5\fax0.375}Active)", controller.DraftChanges()[0].text);
}

TEST(visual_edit_controller, shear_hover_uses_host_highlight_without_changing_selection_or_retained_style) {
	auto line = ReadShearLine(1, 0, R"({\pos(320,240)\fax0.25\fay-0.5}Text)");
	Controller controller(Mode::Shear);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	Style style;
	style.highlight_primary = 0x00332211;
	style.highlight_secondary = 0x00665544;
	style.line_primary = 0x00998877;
	auto retained = controller.CaptureSnapshot({}, style);
	ASSERT_TRUE(retained);
	controller.Pointer(Input(PointerPhase::Move, {340, 140}));
	auto hovered = controller.CaptureSnapshot({}, style);
	ASSERT_TRUE(hovered);
	TransformOverlayRecorder original;
	TransformOverlayRecorder highlight;
	retained->Draw(original);
	hovered->Draw(highlight);
	ASSERT_EQ(1u, original.rectangle_paints.size());
	ASSERT_EQ(1u, highlight.rectangle_paints.size());
	ASSERT_EQ(1u, highlight.circle_paints.size());
	EXPECT_EQ(agi::Color(0x11, 0x22, 0x33), original.rectangle_paints[0].second.colour);
	EXPECT_EQ(agi::Color(0x44, 0x55, 0x66), highlight.rectangle_paints[0].second.colour);
	EXPECT_EQ(agi::Color(0x11, 0x22, 0x33), highlight.circle_paints[0].second.colour);
	EXPECT_EQ((std::set<int>{1}), controller.SelectedLines());
	EXPECT_TRUE(controller.DraftChanges().empty());
}

TEST(visual_edit_controller, shear_prepared_press_uses_only_presented_handles_and_blocks_commands) {
	using Axis = aegisub::visual_shear::Axis;
	auto line = ReadShearLine(1, 0, R"({\pos(320,240)\fax0.25\fay-0.5}Text)");
	Controller controller(Mode::Shear);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	auto displayed = controller.CaptureSnapshot({}, {});
	ASSERT_TRUE(displayed);
	line.position = {480, 240};
	line.text = R"({\pos(480,240)\fax0.25\fay-0.5}Text)";
	controller.UpdateLines(std::span<Line const>(&line, 1));
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Press, {500, 140}, true), displayed.get()).interaction_began);
	auto prepared = controller.Pointer(Input(PointerPhase::Press, {340, 140}, true), displayed.get(), true);
	EXPECT_EQ(CaptureAction::None, prepared.capture);
	EXPECT_FALSE(controller.IsInteracting());
	EXPECT_FALSE(controller.CanNudge());
	EXPECT_FALSE(controller.CanResetShear(Axis::X));
	auto began = controller.BeginPreparedPress();
	ASSERT_TRUE(began.interaction_began);
	EXPECT_EQ(CaptureAction::Capture, began.capture);
	controller.Pointer(Input(PointerPhase::Release, {360, 140}));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\pos(480,240)\fay-0.5\fax0.5}Text)", controller.DraftChanges()[0].text);
}

TEST(visual_edit_controller, shear_retained_snapshot_keeps_geometry_labels_style_and_context_after_owner_changes) {
	auto context = std::make_shared<VisualToolRenderContext>();
	std::shared_ptr<VisualToolRenderSnapshot const> retained;
	Style style;
	style.line_primary = 0x00332211;
	style.line_secondary = 0x00665544;
	style.highlight_primary = 0x00998877;
	style.highlight_secondary = 0x00ccbbaa;
	style.coordinate_font_size = 19;
	style.shear_hint = "localized shear hint";
	style.shear_diagnostic = "localized shear diagnostic";
	{
		auto line = ReadShearLine(1, 0, R"({\move(240,220,600,400,100,900)\fax0.25\fay-0.5\frz45\fscx150}Text)");
		Controller controller(Mode::Shear);
		controller.Reset({line}, {1}, 1, {.number = 12, .time_ms = 500}, Canvas());
		retained = controller.CaptureSnapshot(context, style);
		ASSERT_TRUE(retained);
		EXPECT_EQ(context, retained->context);
		EXPECT_TRUE(retained->HitTest({260, 120}));
		EXPECT_FALSE(retained->HitTest({600, 400}));
		controller.SetViewport({.video_position = {50, 30}, .video_size = {320, 240}, .script_size = {640, 480}});
		controller.Reset({}, {}, std::nullopt, {}, Canvas());
	}
	style.shear_hint = "changed";
	style.coordinate_font_size = 40;
	TransformOverlayRecorder draw;
	retained->Draw(draw);
	ASSERT_EQ(3u, draw.texts.size());
	EXPECT_EQ("fax 0.25", draw.texts[0].text);
	EXPECT_EQ(120, draw.texts[0].x);
	EXPECT_EQ(94, draw.texts[0].y);
	EXPECT_EQ("fay -0.5", draw.texts[1].text);
	EXPECT_EQ(350, draw.texts[1].x);
	EXPECT_EQ(244, draw.texts[1].y);
	EXPECT_EQ("localized shear hint", draw.texts[2].text);
	EXPECT_EQ(8, draw.texts[2].x);
	EXPECT_EQ(456, draw.texts[2].y);
	EXPECT_EQ(19, draw.texts[0].style.size);
	EXPECT_EQ(agi::Color(0x11, 0x22, 0x33), draw.texts[0].style.colour);
	EXPECT_TRUE(draw.texts[0].style.face.empty());
	EXPECT_FALSE(draw.texts[0].style.bold);
	EXPECT_TRUE(draw.texts[0].style.outline);
	EXPECT_EQ(context, retained->context);
}

namespace {
using VectorMode = aegisub::visual_vector_clip::Mode;

Line VectorLine(int id, int row, std::string text, bool comment = false, int start_ms = 0) {
	return ReadShearLine(id, row, std::move(text), comment, start_ms);
}
}

TEST(visual_edit_controller, vector_mode_selection_rejects_wrong_tool_invalid_values_and_active_or_prepared_gestures) {
	auto line = VectorLine(1, 0, R"({\clip(m 10 10 l 90 10 90 90)}A)");
	Controller controller(Mode::VectorClip);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	EXPECT_EQ(VectorMode::Drag, controller.VectorClipMode());
	for (auto mode : {VectorMode::Line, VectorMode::Bicubic, VectorMode::Convert, VectorMode::Insert,
					  VectorMode::Remove, VectorMode::Freehand, VectorMode::FreehandSmooth, VectorMode::Move, VectorMode::Drag}) {
		EXPECT_TRUE(controller.SetVectorClipMode(mode).toolbar_changed);
		EXPECT_EQ(mode, controller.VectorClipMode());
		EXPECT_FALSE(controller.SetVectorClipMode(mode).toolbar_changed);
		EXPECT_TRUE(controller.DraftChanges().empty());
	}
	EXPECT_FALSE(controller.SetVectorClipMode(static_cast<VectorMode>(9)).toolbar_changed);
	EXPECT_FALSE(controller.SetVectorClipMode(static_cast<VectorMode>(255)).toolbar_changed);
	EXPECT_EQ(VectorMode::Drag, controller.VectorClipMode());
	auto displayed = controller.CaptureSnapshot({}, {});
	ASSERT_TRUE(displayed);
	auto prepared = controller.Pointer(Input(PointerPhase::Press, {90, 10}, true), displayed.get(), true);
	EXPECT_EQ(CaptureAction::None, prepared.capture);
	EXPECT_FALSE(controller.IsInteracting());
	EXPECT_FALSE(controller.SetVectorClipMode(VectorMode::Move).toolbar_changed);
	auto began = controller.BeginPreparedPress();
	ASSERT_TRUE(began.interaction_began);
	EXPECT_EQ(CaptureAction::Capture, began.capture);
	EXPECT_FALSE(controller.SetVectorClipMode(VectorMode::Move).toolbar_changed);
	auto finished = controller.FinishInteraction();
	EXPECT_EQ(CaptureAction::Release, finished.capture);
	EXPECT_TRUE(finished.interaction_ended);
	EXPECT_TRUE(controller.SetVectorClipMode(VectorMode::Move).toolbar_changed);
	Controller other(Mode::RectClip);
	other.Reset({line}, {1}, 1, {}, Canvas());
	EXPECT_FALSE(other.SetVectorClipMode(VectorMode::Line).toolbar_changed);
	auto empty = VectorLine(2, 0, "No clip");
	controller.Reset({empty}, {2}, 2, {}, Canvas());
	EXPECT_EQ(VectorMode::Line, controller.VectorClipMode());
}

TEST(visual_edit_controller, vector_read_line_uses_first_inverse_tag_rectangle_conversion_and_original_source_bytes) {
	auto vector = VectorLine(1, 0, R"({\clip(m 1 2 l 3 4)\b1}A{\iclip(3,m -8 16 l 32 48)\clip(m 8 9)}B)");
	EXPECT_EQ("m -8 16 l 32 48", vector.vector_clip.drawing);
	EXPECT_EQ(3, vector.vector_clip.scale);
	EXPECT_TRUE(vector.vector_clip.inverse);
	EXPECT_TRUE(vector.vector_clip.editable);
	EXPECT_EQ(R"({\clip(m 1 2 l 3 4)\b1}A{\iclip(3,m -8 16 l 32 48)\clip(m 8 9)}B)", vector.text);
	auto rectangle = VectorLine(2, 0, R"({\iclip(-10,20,30,40)}A)");
	EXPECT_EQ("m -10 20 l 30 20 30 40 -10 40", rectangle.vector_clip.drawing);
	EXPECT_EQ(1, rectangle.vector_clip.scale);
	EXPECT_TRUE(rectangle.vector_clip.inverse);
	auto literal = VectorLine(3, 0, R"({\clip(m 10 10 l 20 20)}Literal \iclip(3,m 80 80) text)");
	EXPECT_FALSE(literal.vector_clip.inverse);
	EXPECT_EQ("m 10 10 l 20 20", literal.vector_clip.drawing);
	auto absent = VectorLine(4, 0, R"({\b1}No clip)");
	EXPECT_TRUE(absent.vector_clip.drawing.empty());
	EXPECT_EQ(1, absent.vector_clip.scale);
	EXPECT_FALSE(absent.vector_clip.inverse);
	EXPECT_TRUE(absent.vector_clip.editable);
	auto low = VectorLine(5, 0, R"({\clip(0,m 10 20 l 30 40)}A)");
	EXPECT_EQ(1, low.vector_clip.scale);
	EXPECT_TRUE(low.vector_clip.editable);
}

TEST(visual_edit_controller, vector_invalid_selected_or_active_geometry_blocks_the_entire_batch_and_rebind_recovers) {
	auto active = VectorLine(1, 0, R"({\clip(m 10 10 l 90 10 90 90)}Active)");
	std::array<char const *, 12> const rejected{
		R"({\clip(m 0 0 n 20 30)}Unsupported)",
		R"({\iclip(m 0 0 s 10 10 20 20 30 30)}Spline)",
		R"({\clip(m 0 0 l 20)}Incomplete)",
		R"({\clip(m NaN 0 l 20 30)}NaN)",
		R"({\clip(m 0 0 b 1 2 3 4 5 inf)}Infinity)",
		R"({\clip(32,m 0 0 l 20 30)}Scale)",
		R"({\clip(33,m 0 0 l 20 30)}ScaleWrap)",
		R"({\clip(m 0 0 l 2147483648 0)}Overflow)",
		R"({\clip(l 10 20 30 40)}MissingMove)",
		R"({\clip(m 0 0 l 20 30}Unterminated)",
		R"({\clip(m 0 0 l 20 30)trailing}Garbage)",
		R"({\clip(m 0 0 b 1 2 m 3 4)}PartialCurve)"};
	for (size_t index = 0; index < rejected.size(); ++index) {
		SCOPED_TRACE(index);
		auto invalid = VectorLine(2, 5, rejected[index], true, 8000);
		ASSERT_FALSE(invalid.vector_clip.editable);
		Controller controller(Mode::VectorClip);
		controller.Reset({active, invalid}, {1, 2}, 1, {}, Canvas());
		EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Press, {90, 10}, true)).interaction_began);
		EXPECT_TRUE(controller.DraftChanges().empty());
		EXPECT_TRUE(controller.IncrementalChanges().empty());
		auto replacement = VectorLine(2, 5, R"({\iclip(m 5 5 l 15 15)}Recovered)", true, 8000);
		controller.UpdateLines(std::span<Line const>(&replacement, 1));
		ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {90, 10}, true)).interaction_began);
		controller.Pointer(Input(PointerPhase::Release, {100, 20}));
		ASSERT_EQ(2u, controller.DraftChanges().size());
		EXPECT_EQ(R"({\clip(m 10 10 l 100 20 90 90)}Active)", controller.DraftChanges()[0].text);
		EXPECT_EQ(R"({\iclip(m 10 10 l 100 20 90 90)}Recovered)", controller.DraftChanges()[1].text);
	}
	auto invalid_active = VectorLine(3, 0, R"({\clip(m 0 0 n 20 20)}Anchor)");
	Controller controller(Mode::VectorClip);
	controller.Reset({invalid_active, active}, {1}, 3, {}, Canvas());
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Press, {20, 20}, true)).interaction_began);
	EXPECT_TRUE(controller.DraftChanges().empty());
}

TEST(visual_edit_controller, vector_drag_replaces_selected_hidden_comment_offframe_paths_with_own_inverse_and_active_scale) {
	auto active = VectorLine(1, 0, R"({\clip(3,m 40 40 l 360 40 360 360)\b1}Anchor)");
	auto hidden = VectorLine(2, 4, R"({\iclip(m 100 100 l 200 100)\pos(20,30)}Hidden{\i1}tail)", false, 8000);
	auto comment = VectorLine(3, 8, R"({\clip(1,2,30,40)\fax0.25}Comment)", true);
	auto literal = VectorLine(4, 9, R"({\b1}Literal \iclip text)");
	auto unselected = VectorLine(5, 10, R"({\clip(m 7 8 l 9 10)}Unselected)");
	Controller controller(Mode::VectorClip);
	controller.Reset({active, hidden, comment, literal, unselected}, {2, 3, 4}, 1, {}, Canvas());
	auto press = controller.Pointer(Input(PointerPhase::Press, {90, 10}, true));
	ASSERT_TRUE(press.interaction_began);
	EXPECT_FALSE(press.selection_changed);
	EXPECT_EQ(CaptureAction::Capture, press.capture);
	controller.Pointer(Input(PointerPhase::Release, {100, 20}));
	ASSERT_EQ(3u, controller.DraftChanges().size());
	EXPECT_EQ(2, controller.DraftChanges()[0].id);
	EXPECT_EQ(hidden.text, controller.DraftChanges()[0].original_text);
	EXPECT_EQ(R"({\pos(20,30)\iclip(3,m 40 40 l 400 80 360 360)}Hidden{\i1}tail)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\fax0.25\clip(3,m 40 40 l 400 80 360 360)}Comment)", controller.DraftChanges()[1].text);
	EXPECT_EQ(R"({\b1\clip(3,m 40 40 l 400 80 360 360)}Literal \iclip text)", controller.DraftChanges()[2].text);
	EXPECT_EQ((std::set<int>{2, 3, 4}), controller.SelectedLines());
	EXPECT_EQ(std::optional<int>(1), controller.ActiveLine());
}

TEST(visual_edit_controller, vector_control_toggle_and_shrinking_box_select_handles_without_document_edits) {
	auto active = VectorLine(1, 0, R"({\clip(m 10 10 l 90 10 90 90)}A)");
	auto hidden = VectorLine(2, 5, R"({\iclip(m 200 200 l 220 220)}B)", true);
	Controller controller(Mode::VectorClip);
	controller.Reset({active, hidden}, {1, 2}, 1, {}, Canvas());
	controller.Pointer(Input(PointerPhase::Press, {90, 10}, true, true));
	controller.Pointer(Input(PointerPhase::Release, {90, 10}, false, true));
	controller.Pointer(Input(PointerPhase::Press, {90, 90}, true, true));
	controller.Pointer(Input(PointerPhase::Release, {90, 90}, false, true));
	EXPECT_TRUE(controller.DraftChanges().empty());
	controller.Pointer(Input(PointerPhase::Press, {90, 90}, true, true));
	controller.Pointer(Input(PointerPhase::Release, {90, 90}, false, true));
	controller.Pointer(Input(PointerPhase::Press, {0, 0}, true, true));
	controller.Pointer(Input(PointerPhase::Move, {100, 100}, true, true));
	controller.Pointer(Input(PointerPhase::Move, {20, 20}, true, true));
	controller.Pointer(Input(PointerPhase::Release, {20, 20}, false, true));
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_EQ((std::set<int>{1, 2}), controller.SelectedLines());
	controller.Pointer(Input(PointerPhase::Press, {90, 10}, true));
	controller.Pointer(Input(PointerPhase::Release, {100, 20}));
	ASSERT_EQ(2u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\clip(m 20 20 l 100 20 90 90)}A)", controller.DraftChanges()[0].text);
	EXPECT_EQ(R"({\iclip(m 20 20 l 100 20 90 90)}B)", controller.DraftChanges()[1].text);
}

TEST(visual_edit_controller, vector_shift_drag_uses_logical_screen_axis_and_ignores_script_anisotropy) {
	auto line = VectorLine(1, 0, R"({\clip(3,m 40 80 l 120 160 200 240)}A)");
	Controller controller(Mode::VectorClip);
	controller.Reset({line}, {1}, 1, {}, {.video_position = {40, 30}, .video_size = {1280, 240}, .script_size = {640, 480}});
	ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {100, 50}, true)).interaction_began);
	controller.Pointer(Input(PointerPhase::Move, {110, 65}, true, false, false, true));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\clip(3,m 40 80 l 120 280 200 240)}A)", controller.DraftChanges()[0].text);
	controller.Pointer(Input(PointerPhase::Release, {120, 70}, false, false, false, true));
	EXPECT_EQ(R"({\clip(3,m 40 80 l 160 160 200 240)}A)", controller.DraftChanges()[0].text);
}

TEST(visual_edit_controller, vector_no_motion_and_return_to_origin_preserve_every_selected_original_byte) {
	auto active = VectorLine(1, 0, R"({\clip(1,m 010 010 l 090.0 010.0 090 090)\b1}A{\i1}tail)");
	auto hidden = VectorLine(2, 5, R"({\iclip(3,m 004 008 l 012 016)\fax.2500}B)", true);
	Controller controller(Mode::VectorClip);
	controller.Reset({active, hidden}, {1, 2}, 1, {}, Canvas());
	controller.Pointer(Input(PointerPhase::Press, {90, 10}, true));
	controller.Pointer(Input(PointerPhase::Release, {90, 10}));
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_TRUE(controller.IncrementalChanges().empty());
	controller.Pointer(Input(PointerPhase::Press, {90, 10}, true));
	controller.Pointer(Input(PointerPhase::Move, {100, 20}, true));
	ASSERT_EQ(2u, controller.DraftChanges().size());
	controller.Pointer(Input(PointerPhase::Release, {90, 10}));
	EXPECT_TRUE(controller.DraftChanges().empty());
	ASSERT_EQ(2u, controller.IncrementalChanges().size());
	EXPECT_EQ(active.text, controller.IncrementalChanges()[0].text);
	EXPECT_EQ(hidden.text, controller.IncrementalChanges()[1].text);
}

TEST(visual_edit_controller, vector_deferred_discrete_actions_mutate_once_without_capture_or_document_selection_change) {
	auto line = VectorLine(1, 0, R"({\clip(m 10 10 l 90 10 90 90)}A)");
	auto hidden = VectorLine(2, 5, R"({\iclip(m 200 200 l 220 220)}B)", true);
	struct Case {
		VectorMode mode;
		Vector2D point;
		char const *expected;
	};
	std::array<Case, 4> const cases{{{.mode = VectorMode::Convert, .point = {50, 12}, .expected = R"({\clip(m 10 10 b 30 10 70 10 90 10 l 90 90)}A)"},
									 {.mode = VectorMode::Insert, .point = {50, 12}, .expected = R"({\clip(m 10 10 l 50 10 90 10 90 90)}A)"},
									 {.mode = VectorMode::Remove, .point = {90, 10}, .expected = R"({\clip(m 10 10 l 90 90)}A)"},
									 {.mode = VectorMode::Move, .point = {200, 200}, .expected = R"({\clip(m 10 10 l 90 10 90 90 m 200 200)}A)"}}};
	for (size_t index = 0; index < cases.size(); ++index) {
		SCOPED_TRACE(index);
		auto const& item = cases[index];
		Controller controller(Mode::VectorClip);
		controller.Reset({line, hidden}, {1, 2}, 1, {}, Canvas());
		controller.SetVectorClipMode(item.mode);
		auto displayed = controller.CaptureSnapshot({}, {});
		auto prepared = controller.Pointer(Input(PointerPhase::Press, item.point, true), displayed.get(), true);
		EXPECT_EQ(CaptureAction::None, prepared.capture);
		EXPECT_FALSE(prepared.interaction_began);
		EXPECT_TRUE(controller.DraftChanges().empty());
		auto applied = controller.BeginPreparedPress();
		EXPECT_TRUE(applied.interaction_began);
		EXPECT_TRUE(applied.interaction_ended);
		EXPECT_TRUE(applied.geometry_changed);
		EXPECT_FALSE(applied.selection_changed);
		EXPECT_EQ(CaptureAction::None, applied.capture);
		EXPECT_FALSE(controller.IsInteracting());
		ASSERT_EQ(2u, controller.DraftChanges().size());
		EXPECT_EQ(item.expected, controller.DraftChanges()[0].text);
		EXPECT_EQ((std::set<int>{1, 2}), controller.SelectedLines());
		auto const accepted = controller.DraftChanges()[0].text;
		EXPECT_FALSE(controller.BeginPreparedPress().interaction_began);
		EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Release, item.point)).geometry_changed);
		EXPECT_EQ(accepted, controller.DraftChanges()[0].text);
	}
}

TEST(visual_edit_controller, vector_append_line_and_bicubic_finish_at_release_and_preserve_other_subpaths) {
	auto line = VectorLine(1, 0, R"({\clip(m 10 10 l 90 10 m 200 200 l 280 200)}A)");
	Controller controller(Mode::VectorClip);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	controller.SetVectorClipMode(VectorMode::Line);
	ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {300, 220}, true)).interaction_began);
	controller.Pointer(Input(PointerPhase::Move, {310, 230}, true));
	auto ended = controller.Pointer(Input(PointerPhase::Release, {320, 240}));
	EXPECT_EQ(CaptureAction::Release, ended.capture);
	EXPECT_TRUE(ended.interaction_ended);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\clip(m 10 10 l 90 10 m 200 200 l 280 200 320 240)}A)", controller.DraftChanges()[0].text);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	controller.SetVectorClipMode(VectorMode::Bicubic);
	ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {280, 240}, true)).interaction_began);
	controller.Pointer(Input(PointerPhase::Release, {280, 280}));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\clip(m 10 10 l 90 10 m 200 200 l 280 200 b 300 200 280 260 280 280)}A)", controller.DraftChanges()[0].text);
}

TEST(visual_edit_controller, vector_freehand_variants_replace_path_keep_final_point_and_return_to_drag) {
	auto line = VectorLine(1, 0, R"({\iclip(3,m 40 40 l 360 40)}A)");
	for (auto mode : {VectorMode::Freehand, VectorMode::FreehandSmooth}) {
		SCOPED_TRACE(static_cast<int>(mode));
		Controller controller(Mode::VectorClip);
		controller.Reset({line}, {1}, 1, {}, Canvas());
		controller.SetVectorClipMode(mode);
		ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {200, 200}, true)).interaction_began);
		controller.Pointer(Input(PointerPhase::Move, {210, 200}, true));
		auto released = controller.Pointer(Input(PointerPhase::Release, {220, 210}));
		EXPECT_TRUE(released.interaction_ended);
		EXPECT_TRUE(released.toolbar_changed);
		EXPECT_EQ(CaptureAction::Release, released.capture);
		EXPECT_EQ(VectorMode::Drag, controller.VectorClipMode());
		ASSERT_EQ(1u, controller.DraftChanges().size());
		EXPECT_EQ(R"({\iclip(3,m 800 800 l 880 840)}A)", controller.DraftChanges()[0].text);
		auto displayed = controller.CaptureSnapshot({}, {});
		ASSERT_TRUE(displayed);
		EXPECT_TRUE(displayed->HitTest({220, 210}));
		EXPECT_FALSE(displayed->HitTest({90, 10}));
	}
}

TEST(visual_edit_controller, vector_remove_final_point_serializes_empty_path_and_switches_to_line_on_rebind) {
	auto line = VectorLine(1, 0, R"({\iclip(m 10 20)\b1}A)");
	Controller controller(Mode::VectorClip);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	controller.SetVectorClipMode(VectorMode::Remove);
	auto removed = controller.Pointer(Input(PointerPhase::Press, {10, 20}, true));
	EXPECT_TRUE(removed.interaction_began);
	EXPECT_TRUE(removed.interaction_ended);
	EXPECT_EQ(CaptureAction::None, removed.capture);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\b1\iclip()}A)", controller.DraftChanges()[0].text);
	auto rebound = VectorLine(1, 0, controller.DraftChanges()[0].text);
	controller.Reset({rebound}, {1}, 1, {}, Canvas());
	EXPECT_EQ(VectorMode::Line, controller.VectorClipMode());
}

TEST(visual_edit_controller, vector_presented_path_and_mode_must_match_before_handle_or_segment_edit) {
	auto line = VectorLine(1, 0, R"({\clip(m 10 10 l 90 10 90 90)}A)");
	Controller controller(Mode::VectorClip);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	auto original = controller.CaptureSnapshot({}, {});
	ASSERT_TRUE(original);
	auto changed = VectorLine(1, 0, R"({\clip(m 10 10 l 190 10 190 90)}A)");
	controller.UpdateLines(std::span<Line const>(&changed, 1));
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Press, {90, 10}, true), original.get()).interaction_began);
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Press, {190, 10}, true), original.get()).interaction_began);
	EXPECT_TRUE(controller.DraftChanges().empty());
	controller.SetVectorClipMode(VectorMode::Insert);
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Press, {50, 12}, true), original.get()).interaction_began);
	auto fresh = controller.CaptureSnapshot({}, {});
	ASSERT_TRUE(fresh);
	auto inserted = controller.Pointer(Input(PointerPhase::Press, {100, 12}, true), fresh.get());
	EXPECT_TRUE(inserted.interaction_began);
	EXPECT_TRUE(inserted.interaction_ended);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\clip(m 10 10 l 100 10 190 10 190 90)}A)", controller.DraftChanges()[0].text);
}

TEST(visual_edit_controller, vector_snapshot_keeps_owned_geometry_style_and_hit_identity_after_owner_destruction) {
	std::shared_ptr<VisualToolRenderSnapshot const> retained;
	auto context = std::make_shared<VisualToolRenderContext>();
	Style style;
	style.line_primary = 0x00332211;
	style.highlight_primary = 0x00665544;
	style.highlight_secondary = 0x00998877;
	style.shaded_alpha = .375f;
	{
		auto line = VectorLine(1, 0, R"({\clip(m 10 10 b 30 10 70 10 90 10 l 90 90)}A)");
		Controller controller(Mode::VectorClip);
		controller.Reset({line}, {1}, 1, {}, Canvas());
		retained = controller.CaptureSnapshot(context, style);
		ASSERT_TRUE(retained);
		controller.SetViewport({.video_position = {30, 40}, .video_size = {320, 240}, .script_size = {640, 480}});
		controller.Reset({}, {}, std::nullopt, {}, Canvas());
	}
	EXPECT_EQ(context, retained->context);
	auto circle = retained->HitTest({90, 10});
	auto square = retained->HitTest({30, 10});
	ASSERT_TRUE(circle);
	ASSERT_TRUE(square);
	EXPECT_FALSE(circle->line_id);
	EXPECT_EQ(3u, circle->index);
	EXPECT_EQ(1u, square->index);
	EXPECT_NE(circle->type, square->type);
	EXPECT_FALSE(retained->HitTest({190, 10}));
	TransformOverlayRecorder draw;
	retained->Draw(draw);
	ASSERT_EQ(2u, draw.rectangle_paints.size());
	ASSERT_EQ(3u, draw.circle_paints.size());
	EXPECT_EQ(agi::Color(0x11, 0x22, 0x33), draw.rectangle_paints[0].first.colour);
	EXPECT_FLOAT_EQ(.5f, draw.rectangle_paints[0].first.alpha);
	EXPECT_EQ(agi::Color(0x11, 0x22, 0x33), draw.circle_paints[0].second.colour);
	EXPECT_FLOAT_EQ(.6f, draw.circle_paints[0].second.alpha);
}

TEST(visual_edit_controller, vector_nonfinite_or_unencodable_motion_preserves_last_valid_whole_batch) {
	auto line = VectorLine(1, 0, R"({\clip(m 10 10 l 90 10 90 90)}A)");
	auto hidden = VectorLine(2, 5, R"({\iclip(m 200 200 l 220 220)}B)", true);
	Controller controller(Mode::VectorClip);
	controller.Reset({line, hidden}, {1, 2}, 1, {}, Canvas());
	ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {90, 10}, true)).interaction_began);
	controller.Pointer(Input(PointerPhase::Move, {100, 20}, true));
	ASSERT_EQ(2u, controller.DraftChanges().size());
	auto const first = controller.DraftChanges()[0].text;
	auto const second = controller.DraftChanges()[1].text;
	for (auto point : {Vector2D(std::numeric_limits<float>::quiet_NaN(), 20.f),
					   Vector2D(100.f, std::numeric_limits<float>::infinity()), Vector2D(2147483648.f, 20.f)}) {
		EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Move, point, true)).geometry_changed);
		ASSERT_EQ(2u, controller.DraftChanges().size());
		EXPECT_EQ(first, controller.DraftChanges()[0].text);
		EXPECT_EQ(second, controller.DraftChanges()[1].text);
	}
	auto release = controller.Pointer(Input(PointerPhase::Release, {2147483648.f, 20.f}));
	EXPECT_EQ(CaptureAction::Release, release.capture);
	EXPECT_TRUE(release.interaction_ended);
	EXPECT_FALSE(controller.IsInteracting());
	EXPECT_EQ(first, controller.DraftChanges()[0].text);
	EXPECT_EQ(second, controller.DraftChanges()[1].text);
}

TEST(visual_edit_controller, vector_prepared_gesture_is_retired_by_selection_viewport_frame_and_reset_boundaries) {
	auto line = VectorLine(1, 0, R"({\clip(m 10 10 l 90 10 90 90)}A)");
	for (int boundary = 0; boundary < 4; ++boundary) {
		SCOPED_TRACE(boundary);
		Controller controller(Mode::VectorClip);
		controller.Reset({line}, {1}, 1, {}, Canvas());
		auto snapshot = controller.CaptureSnapshot({}, {});
		controller.Pointer(Input(PointerPhase::Press, {90, 10}, true), snapshot.get(), true);
		switch (boundary) {
			case 0: controller.SetSelection({}, 1); break;
			case 1: controller.SetViewport({.video_position = {20, 30}, .video_size = {640, 480}, .script_size = {640, 480}}); break;
			case 2: controller.SetFrame({.number = 1, .time_ms = 40}); break;
			case 3: controller.Reset({line}, {1}, 1, {}, Canvas()); break;
		}
		EXPECT_FALSE(controller.BeginPreparedPress().interaction_began);
		EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Release, {100, 20})).geometry_changed);
		EXPECT_FALSE(controller.IsInteracting());
		EXPECT_TRUE(controller.DraftChanges().empty());
	}
	Controller controller(Mode::VectorClip);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	controller.Pointer(Input(PointerPhase::Press, {90, 10}, true));
	controller.Pointer(Input(PointerPhase::Move, {100, 20}, true));
	ASSERT_EQ(1u, controller.DraftChanges().size());
	controller.FinishInteraction();
	controller.Reset({line}, {1}, 1, {}, Canvas());
	EXPECT_TRUE(controller.DraftChanges().empty());
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Release, {110, 30})).geometry_changed);
	auto snapshot = controller.CaptureSnapshot({}, {});
	EXPECT_TRUE(snapshot->HitTest({90, 10}));
	EXPECT_FALSE(snapshot->HitTest({100, 20}));
}

TEST(visual_edit_controller, vector_insert_targets_each_presented_subpath_closure_and_preserves_other_contours) {
	auto line = VectorLine(1, 0, R"({\iclip(m 10 10 l 90 10 90 90 m 210 210 l 290 210 290 290)}A)");
	Controller controller(Mode::VectorClip);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	controller.SetVectorClipMode(VectorMode::Insert);
	auto first_snapshot = controller.CaptureSnapshot({}, {});
	auto first = controller.Pointer(Input(PointerPhase::Press, {50, 50}, true), first_snapshot.get());
	EXPECT_TRUE(first.interaction_ended);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\iclip(m 10 10 l 90 10 90 90 50 50 m 210 210 l 290 210 290 290)}A)", controller.DraftChanges()[0].text);
	controller.AcceptChanges();
	auto second_snapshot = controller.CaptureSnapshot({}, {});
	auto second = controller.Pointer(Input(PointerPhase::Press, {250, 250}, true), second_snapshot.get());
	EXPECT_TRUE(second.interaction_ended);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\iclip(m 10 10 l 90 10 90 90 50 50 m 210 210 l 290 210 290 290 250 250)}A)", controller.DraftChanges()[0].text);
}

TEST(visual_edit_controller, vector_finish_freehand_keeps_last_valid_residual_and_late_up_cannot_publish_again) {
	auto line = VectorLine(1, 0, R"({\clip(m 10 10 l 90 10)}A)");
	Controller controller(Mode::VectorClip);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	controller.SetVectorClipMode(VectorMode::Freehand);
	ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {200, 200}, true)).interaction_began);
	controller.Pointer(Input(PointerPhase::Move, {220, 210}, true));
	auto finished = controller.FinishInteraction();
	EXPECT_TRUE(finished.interaction_ended);
	EXPECT_TRUE(finished.toolbar_changed);
	EXPECT_EQ(CaptureAction::Release, finished.capture);
	EXPECT_EQ(VectorMode::Drag, controller.VectorClipMode());
	EXPECT_FALSE(controller.IsInteracting());
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(R"({\clip(m 200 200 l 220 210)}A)", controller.DraftChanges()[0].text);
	EXPECT_FALSE(controller.FinishInteraction().interaction_ended);
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Release, {280, 280})).geometry_changed);
	EXPECT_EQ(R"({\clip(m 200 200 l 220 210)}A)", controller.DraftChanges()[0].text);
}

TEST(visual_edit_controller, vector_finish_smooth_freehand_smooths_once_and_preserves_finalized_history_source) {
	auto line = VectorLine(1, 0, R"({\iclip(m 10 10 l 90 10)\b1}A)");
	Controller controller(Mode::VectorClip);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	controller.SetVectorClipMode(VectorMode::FreehandSmooth);
	ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {200, 200}, true)).interaction_began);
	for (auto point : {Vector2D(320, 200), Vector2D(320, 320), Vector2D(200, 320), Vector2D(200, 200)})
		controller.Pointer(Input(PointerPhase::Move, point, true));
	auto finished = controller.FinishInteraction();
	EXPECT_TRUE(finished.interaction_ended);
	EXPECT_TRUE(finished.toolbar_changed);
	EXPECT_EQ(CaptureAction::Release, finished.capture);
	EXPECT_EQ(VectorMode::Drag, controller.VectorClipMode());
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(line.text, controller.DraftChanges()[0].original_text);
	EXPECT_EQ(R"({\b1\iclip(m 200 200 b 260 200 290 170 320 200 350 230 350 290 320 320 290 350 230 350 200 320 170 290 200 260 200 200)}A)", controller.DraftChanges()[0].text);
	auto const final = controller.DraftChanges()[0].text;
	EXPECT_FALSE(controller.FinishInteraction().interaction_ended);
	EXPECT_FALSE(controller.Pointer(Input(PointerPhase::Release, {230, 240})).geometry_changed);
	EXPECT_EQ(final, controller.DraftChanges()[0].text);
	controller.AcceptChanges();
	EXPECT_TRUE(controller.DraftChanges().empty());
}

TEST(visual_edit_controller, planned_xy_rejection_keeps_selected_draft_and_unselected_active_overlay) {
	auto active = MakeLine(1, 0, R"({\frx23\fry71\frz52}Active)", {200, 200});
	active.rotation_x = 23;
	active.rotation_y = 71;
	active.rotation_z = 52;
	auto selected = MakeLine(2, 9, R"({\frx80\fry12\i1}Selected)", {300, 300});
	Controller controller(Mode::RotateXY);
	controller.Reset({active, selected}, {2}, 1, {}, Canvas());
	auto context = std::make_shared<VisualToolRenderContext>();
	VisualToolDragSnapshot displayed_miss(context);
	bool reject = false;
	int calls = 0;
	OverridePlanner planner = [&](std::span<OverrideEdit const> writes) -> std::optional<std::vector<VisualTextEdit>> {
		++calls;
		EXPECT_EQ(2u, writes.size());
		auto text = selected.text;
		for (auto const& write : writes) {
			EXPECT_EQ(2, write.id);
			EXPECT_EQ(OverrideIntent::Perspective, write.intent);
			text = SetOverride(text, write.tag, write.value);
		}
		if (reject)
			return {};
		return std::vector<VisualTextEdit>{{.id = 2, .row = 9, .original_text = selected.text, .text = text}};
	};
	ASSERT_TRUE(controller.Pointer(Input(PointerPhase::Press, {200, 200}, true), &displayed_miss, false, planner).interaction_began);
	controller.Pointer(Input(PointerPhase::Move, {211, 193}, true), &displayed_miss, false, planner);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	auto const accepted = controller.DraftChanges()[0].text;
	TransformOverlayRecorder before;
	controller.CaptureSnapshot(context, {})->Draw(before);
	ASSERT_EQ(1u, before.perspective_batches.size());
	EXPECT_FLOAT_EQ(37.f, before.perspective_batches.front().transform.angle_x);
	EXPECT_FLOAT_EQ(93.f, before.perspective_batches.front().transform.angle_y);
	reject = true;
	controller.Pointer(Input(PointerPhase::Move, {220, 190}, true), &displayed_miss, false, planner);
	EXPECT_TRUE(controller.IncrementalChanges().empty());
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_EQ(accepted, controller.DraftChanges()[0].text);
	TransformOverlayRecorder after;
	controller.CaptureSnapshot(context, {})->Draw(after);
	EXPECT_TRUE(SamePerspectiveBatches(before, after));
	auto released = controller.Pointer(Input(PointerPhase::Release, {220, 190}), &displayed_miss, false, planner);
	EXPECT_EQ(CaptureAction::Release, released.capture);
	EXPECT_TRUE(released.interaction_ended);
	EXPECT_EQ(accepted, controller.DraftChanges()[0].text);
	EXPECT_EQ(3, calls);
}

TEST(visual_edit_controller, planned_scale_return_to_start_restores_exact_source_without_inherited_tags) {
	auto line = MakeLine(1, 0, R"({\b1}Text)", {200, 200});
	Controller controller(Mode::Scale);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	bool restored = false;
	OverridePlanner planner = [&](std::span<OverrideEdit const> writes) -> std::optional<std::vector<VisualTextEdit>> {
		auto text = line.text;
		for (auto const& write : writes) {
			EXPECT_EQ(OverrideIntent::Affine, write.intent);
			if (write.restore_source) {
				restored = true;
				EXPECT_TRUE(write.tag.empty());
			}
			else
				text = SetOverride(text, write.tag, write.value);
		}
		return std::vector<VisualTextEdit>{{.id = 1, .row = 0, .original_text = line.text, .text = text}};
	};
	controller.Pointer(Input(PointerPhase::Press, {200, 200}, true), nullptr, false, planner);
	controller.Pointer(Input(PointerPhase::Move, {220, 190}, true), nullptr, false, planner);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	controller.Pointer(Input(PointerPhase::Release, {200, 200}), nullptr, false, planner);
	EXPECT_TRUE(restored);
	EXPECT_TRUE(controller.DraftChanges().empty());
	ASSERT_EQ(1u, controller.IncrementalChanges().size());
	EXPECT_EQ(line.text, controller.IncrementalChanges()[0].text);
}

TEST(visual_edit_controller, linked_scale_commands_reuse_burst_source_and_reject_without_partial_drafts) {
	using namespace aegisub::visual_geometry;
	AssStyle style;
	AssDialogue dialogue;
	dialogue.Id = 1;
	dialogue.Row = 0;
	dialogue.Text = R"({\pos(0,0)\clip(0,0,20,20)\b1}Text)";
	auto line = ReadLine(dialogue, &style, {640, 480});
	line.first_frame = 0;
	line.last_frame = 100;
	Snapshot snapshot(dialogue, &style, {640, 480});
	Controller controller(Mode::Scale);
	controller.Reset({line}, {1}, 1, {}, Canvas());
	bool reject = false;
	OverridePlanner planner = [&](std::span<OverrideEdit const> writes) -> std::optional<std::vector<VisualTextEdit>> {
		if (reject)
			return {};
		std::vector<Edit> edits;
		bool restore = false;
		for (auto const& write : writes) {
			EXPECT_EQ(OverrideIntent::Affine, write.intent);
			restore |= write.restore_source;
			if (!write.restore_source)
				edits.push_back({.tag = write.tag, .value = write.value});
		}
		Request request{.source = &snapshot, .edits = edits};
		auto result = Plan(std::span(&request, 1), {640, 480}, restore);
		EXPECT_TRUE(result) << result.error;
		return result ? std::optional(std::move(result.edits)) : std::nullopt;
	};
	EXPECT_TRUE(controller.Nudge({1, -1}, 10, 1, planner).geometry_changed);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_NE(std::string::npos, controller.DraftChanges()[0].text.find(R"(\clip(0,0,22,22))"));
	EXPECT_TRUE(controller.Nudge({1, -1}, 10, 1, planner).geometry_changed);
	ASSERT_EQ(1u, controller.DraftChanges().size());
	EXPECT_NE(std::string::npos, controller.DraftChanges()[0].text.find(R"(\clip(0,0,24,24))"));
	auto const last_valid = controller.DraftChanges()[0].text;
	reject = true;
	EXPECT_FALSE(controller.NormalizeScale(visual_tool_scale_policy::Axis::X, planner).geometry_changed);
	EXPECT_EQ(last_valid, controller.DraftChanges()[0].text);
	EXPECT_TRUE(controller.IncrementalChanges().empty());
	reject = false;
	EXPECT_TRUE(controller.NormalizeScale(visual_tool_scale_policy::Axis::X, planner).geometry_changed);
	EXPECT_TRUE(controller.DraftChanges().empty());
	ASSERT_EQ(1u, controller.IncrementalChanges().size());
	EXPECT_EQ(dialogue.Text.get(), controller.IncrementalChanges()[0].text);
}
