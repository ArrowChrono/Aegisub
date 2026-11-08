#include <main.h>

#include "../../src/video_overlay_draw_context.h"
#include "../../src/visual_guide_interaction.h"

#include <limits>
#include <string>

namespace {
VisualGuideViewport MakeViewport() {
	return {
		100.0, 50.0, 800.0, 400.0,
		400.0, 200.0,
	};
}

VisualGuide MakeGuide(
	std::string id,
	VisualGuidePoint first,
	VisualGuidePoint second = {}) {
	VisualGuide guide;
	guide.id = std::move(id);
	guide.first = first;
	guide.second = second;
	return guide;
}

VisualGuideSnapshotView View(VisualGuideSnapshot const& snapshot) {
	return {
		snapshot.generation,
		snapshot.guides,
		snapshot.selected_id,
	};
}

// Minimal context whose MeasureText is a stable function of the text length so
// the label rectangle is deterministic in tests. Draw primitives are no-ops.
class StubOverlayContext final : public VideoOverlayDrawContext {
	public:
	void SetLineColour(agi::Color const&, float, int) override {}
	void SetFillColour(agi::Color const&, float) override {}
	void SetInvert() override {}
	void ClearInvert() override {}
	void DrawLine(Vector2D, Vector2D) override {}
	void DrawLines(size_t, float const *, size_t) override {}
	void DrawLineStrip(Vector2D const *, size_t) override {}
	void DrawRectangle(Vector2D, Vector2D) override {}
	void DrawPolygon(Vector2D const *, size_t) override {}
	void DrawMultiPolygon(std::vector<float> const&, std::vector<int> const&,
						  std::vector<int> const&, Vector2D, Vector2D, bool) override {}
	void DrawCircle(Vector2D, float) override {}
	void DrawTriangle(Vector2D, Vector2D, Vector2D) override {}
	VideoOverlayTextExtent MeasureText(std::string const& text, VideoOverlayTextStyle const&) override {
		return VideoOverlayTextExtent(static_cast<int>(text.size()) * 7, 12);
	}
	void DrawText(std::string const&, int, int, VideoOverlayTextStyle const&) override { }
};
}

TEST(visual_guide_interaction, shift_snap_uses_horizontal_vertical_and_diagonal_sectors) {
	VisualGuidePoint const fixed { 10.0, 20.0 };
	EXPECT_EQ((VisualGuidePoint{ 40.0, 20.0 }),
		SnapVisualGuideMeasurementPoint(fixed, { 40.0, 25.0 }, true));
	EXPECT_EQ((VisualGuidePoint{ 10.0, 60.0 }),
		SnapVisualGuideMeasurementPoint(fixed, { 14.0, 60.0 }, true));
	EXPECT_EQ((VisualGuidePoint{ 40.0, 50.0 }),
		SnapVisualGuideMeasurementPoint(fixed, { 35.0, 50.0 }, true));
	EXPECT_EQ((VisualGuidePoint{ -20.0, -10.0 }),
		SnapVisualGuideMeasurementPoint(fixed, { -20.0, -5.0 }, true));
	EXPECT_EQ((VisualGuidePoint{ 35.0, 50.0 }),
		SnapVisualGuideMeasurementPoint(fixed, { 35.0, 50.0 }, false));
}

TEST(visual_guide_interaction, hit_testing_prioritizes_selected_endpoints_then_reverse_draw_order) {
	VisualGuideSnapshot snapshot;
	snapshot.guides = {
		MakeGuide("selected", { 100.0, 50.0 }, { 200.0, 50.0 }),
		MakeGuide("newest", { 100.0, 50.0 }, { 200.0, 50.0 }),
	};
	snapshot.selected_id = "selected";

	StubOverlayContext context;
	VisualGuideOverlayStyle style;
	auto hit = HitTestVisualGuides(
		Vector2D(300.0f, 150.0f), View(snapshot), MakeViewport(), style, context, 8.0);
	ASSERT_TRUE(hit);
	EXPECT_EQ("selected", hit.id);
	EXPECT_EQ(VisualGuideHitPart::FirstEndpoint, hit.part);

	snapshot.selected_id.reset();
	hit = HitTestVisualGuides(
		Vector2D(400.0f, 150.0f), View(snapshot), MakeViewport(), style, context, 8.0);
	ASSERT_TRUE(hit);
	EXPECT_EQ("newest", hit.id);
	EXPECT_EQ(VisualGuideHitPart::Line, hit.part);
}

TEST(visual_guide_interaction, hit_testing_skips_unmappable_viewports) {
	VisualGuideSnapshot snapshot;
	snapshot.guides = {
		MakeGuide("segment", { 100.0, 50.0 }, { 200.0, 50.0 }),
	};

	StubOverlayContext context;
	VisualGuideOverlayStyle style;
	auto hit = HitTestVisualGuides(
		Vector2D(300.0f, 150.0f), View(snapshot), MakeViewport(), style, context, 8.0);
	ASSERT_TRUE(hit);
	EXPECT_EQ("segment", hit.id);

	auto viewport = MakeViewport();
	viewport.script_width = std::numeric_limits<double>::quiet_NaN();
	EXPECT_FALSE(HitTestVisualGuides(
		Vector2D(300.0f, 150.0f), View(snapshot), viewport, style, context, 8.0));
}

TEST(visual_guide_interaction, info_box_is_selectable_and_lower_priority_than_line) {
	VisualGuideSnapshot snapshot;
	snapshot.guides = {
		MakeGuide("guide", { 100.0, 50.0 }, { 200.0, 50.0 }),
	};

	StubOverlayContext context;
	VisualGuideOverlayStyle style;
	auto const viewport = MakeViewport();

	// A point sitting exactly on the line body must report Line, not Label.
	auto on_line = HitTestVisualGuides(
		Vector2D(400.0f, 150.0f), View(snapshot), viewport, style, context, 8.0);
	ASSERT_TRUE(on_line);
	EXPECT_EQ(VisualGuideHitPart::Line, on_line.part);

	// Probe the rendered label rectangle to find a point inside it that is not
	// on the line, then assert it resolves to Label.
	auto const geometry = ComputeMeasurementLabelGeometry(
		context, snapshot.guides.front(), viewport,
		VisualGuideToCanvas(snapshot.guides.front().first, viewport),
		VisualGuideToCanvas(snapshot.guides.front().second, viewport),
		style);
	Vector2D const inside(geometry.origin.X() + geometry.size.X() * 0.5f,
		geometry.origin.Y() + geometry.size.Y() * 0.5f);
	auto on_label = HitTestVisualGuides(
		inside, View(snapshot), viewport, style, context, 8.0);
	ASSERT_TRUE(on_label);
	EXPECT_EQ("guide", on_label.id);
	EXPECT_EQ(VisualGuideHitPart::Label, on_label.part);
}

TEST(visual_guide_interaction, drag_updates_measurement_endpoints_and_line_from_original_value) {
	auto measurement = MakeGuide(
		"measurement", { 10.0, 20.0 }, { 30.0, 40.0 });
	auto const viewport = MakeViewport();

	auto moved = ApplyVisualGuideDrag(
		measurement, VisualGuideDragAction::MoveGuide,
		{ 5.0, 5.0 }, { 12.0, 2.0 }, false, viewport);
	EXPECT_EQ((VisualGuidePoint{ 17.0, 17.0 }), moved.first);
	EXPECT_EQ((VisualGuidePoint{ 37.0, 37.0 }), moved.second);

	auto endpoint = ApplyVisualGuideDrag(
		measurement, VisualGuideDragAction::MoveSecondEndpoint,
		{}, { 35.0, 61.0 }, true, viewport);
	EXPECT_EQ(measurement.first, endpoint.first);
	EXPECT_EQ((VisualGuidePoint{ 51.0, 61.0 }), endpoint.second);
	EXPECT_EQ(measurement, ApplyVisualGuideDrag(
		measurement, VisualGuideDragAction::MoveGuide,
		{ 0.0, 0.0 },
		{ std::numeric_limits<double>::infinity(), 0.0 }, false, viewport));
}

TEST(visual_guide_interaction, drag_clamps_endpoints_to_script_rectangle) {
	auto measurement = MakeGuide(
		"measurement", { 10.0, 20.0 }, { 30.0, 40.0 });
	auto const viewport = MakeViewport();

	auto moved = ApplyVisualGuideDrag(
		measurement, VisualGuideDragAction::MoveSecondEndpoint,
		{}, { 900.0, -50.0 }, false, viewport);
	EXPECT_DOUBLE_EQ(400.0, moved.second.x);
	EXPECT_DOUBLE_EQ(0.0, moved.second.y);

	auto whole = ApplyVisualGuideDrag(
		measurement, VisualGuideDragAction::MoveGuide,
		{ 10.0, 20.0 }, { -100.0, -100.0 }, false, viewport);
	EXPECT_DOUBLE_EQ(0.0, whole.first.x);
	EXPECT_DOUBLE_EQ(0.0, whole.first.y);
	// Second is translated by the same delta then clamped.
	EXPECT_DOUBLE_EQ(0.0, whole.second.x);
	EXPECT_DOUBLE_EQ(0.0, whole.second.y);
}

TEST(visual_guide_interaction, clamp_helper_preserves_nonfinite_points) {
	auto const viewport = MakeViewport();
	VisualGuidePoint const infinite {
		std::numeric_limits<double>::infinity(), 1.0 };
	EXPECT_EQ(infinite, ClampVisualGuidePointToScript(infinite, viewport));
}
