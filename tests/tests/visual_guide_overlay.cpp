#include <main.h>

#include "../../src/visual_guide_overlay.h"
#include "../../src/video_overlay_draw_context.h"

#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
VisualGuideViewport MakeViewport() {
	return {
		100.0, 50.0, 800.0, 400.0,
		400.0, 200.0,
	};
}

VisualGuideSnapshotView ViewFrom(VisualGuideSnapshot const& snapshot) {
	return { snapshot.generation, snapshot.guides, snapshot.selected_id };
}

class CaptureOverlayContext final : public VideoOverlayDrawContext {
public:
	struct Polygon {
		std::vector<Vector2D> points;
		agi::Color fill_colour;
		float fill_alpha = 0.0f;
	};

	struct Rectangle {
		Vector2D first;
		Vector2D second;
	};

	struct Text {
		std::string value;
		int x = 0;
		int y = 0;
	};

	agi::Color line_colour;
	float line_alpha = 1.0f;
	int line_width = 1;
	agi::Color fill_colour;
	float fill_alpha = 1.0f;
	std::vector<Polygon> polygons;
	std::vector<Rectangle> rectangles;
	std::vector<Text> text;
	int invert_count = 0;

	void SetLineColour(agi::Color const& colour, float alpha = 1.0f, int width = 1) override {
		line_colour = colour;
		line_alpha = alpha;
		line_width = width;
	}
	void SetFillColour(agi::Color const& colour, float alpha = 1.0f) override {
		fill_colour = colour;
		fill_alpha = alpha;
	}
	void SetInvert() override { ++invert_count; }
	void ClearInvert() override { }
	void DrawLine(Vector2D, Vector2D) override { }
	void DrawLines(size_t, float const*, size_t) override { }
	void DrawLineStrip(Vector2D const*, size_t) override { }
	void DrawRectangle(Vector2D first, Vector2D second) override {
		rectangles.push_back({ first, second });
	}
	void DrawPolygon(Vector2D const* points, size_t n) override {
		Polygon polygon;
		polygon.points.assign(points, points + n);
		polygon.fill_colour = fill_colour;
		polygon.fill_alpha = fill_alpha;
		polygons.push_back(std::move(polygon));
	}
	void DrawMultiPolygon(std::vector<float> const&, std::vector<int> const&, std::vector<int> const&, Vector2D, Vector2D, bool) override { }
	void DrawCircle(Vector2D, float) override { }
	void DrawTriangle(Vector2D, Vector2D, Vector2D) override { }

	VideoOverlayTextExtent MeasureText(std::string const& value, VideoOverlayTextStyle const&) override {
		return VideoOverlayTextExtent(static_cast<int>(value.size()) * 7, 12);
	}
	void DrawText(std::string const& value, int x, int y, VideoOverlayTextStyle const&) override {
		text.push_back({ value, x, y });
	}
};
}

TEST(visual_guide_overlay, maps_script_points_through_video_viewport) {
	auto const viewport = MakeViewport();
	auto const script = VisualGuideToCanvas({ 100.0, 50.0 }, viewport);
	EXPECT_FLOAT_EQ(300.0f, script.X());
	EXPECT_FLOAT_EQ(150.0f, script.Y());

	auto const round_trip = CanvasToVisualGuide(Vector2D(300.0f, 150.0f), viewport);
	ASSERT_TRUE(round_trip);
	EXPECT_DOUBLE_EQ(100.0, round_trip->x);
	EXPECT_DOUBLE_EQ(50.0, round_trip->y);
}

TEST(visual_guide_overlay, refuses_invalid_dimensions_without_dividing_by_zero) {
	auto viewport = MakeViewport();
	viewport.script_height = 0.0;
	EXPECT_FALSE(CanvasToVisualGuide(Vector2D(100.0f, 50.0f), viewport));

	viewport = MakeViewport();
	viewport.canvas_width = 0.0;
	EXPECT_FALSE(CanvasToVisualGuide(Vector2D(100.0f, 50.0f), viewport));

	viewport = MakeViewport();
	viewport.canvas_x = std::numeric_limits<double>::quiet_NaN();
	EXPECT_FALSE(IsVisualGuideViewportMappable(viewport));
	EXPECT_FALSE(CanvasToVisualGuide(Vector2D(100.0f, 50.0f), viewport));

	VisualGuide guide;
	guide.id = "guide-1";
	guide.first = { 1.0, 2.0 };
	guide.second = { 3.0, 4.0 };
	VisualGuideSnapshot snapshot;
	snapshot.guides.push_back(guide);
	CaptureOverlayContext context;
	VisualGuideOverlay().Draw(context, viewport, ViewFrom(snapshot), {});
	EXPECT_TRUE(context.polygons.empty());
	EXPECT_TRUE(context.rectangles.empty());
	EXPECT_TRUE(context.text.empty());
}

TEST(visual_guide_overlay, formats_measurement_values_without_negative_zero) {
	EXPECT_EQ("+400", FormatVisualGuideLabelNumber(400.0, true));
	EXPECT_EQ("-220.25", FormatVisualGuideLabelNumber(-220.25, true));
	EXPECT_EQ("0", FormatVisualGuideLabelNumber(-0.0001, true));
	EXPECT_EQ("456.51", FormatVisualGuideLabelNumber(456.508488, false));
	EXPECT_EQ("0", FormatVisualGuideLabelNumber(std::numeric_limits<double>::infinity(), false));
}

TEST(visual_guide_overlay, callable_measurement_freezes_exact_rendered_label_geometry) {
	VisualGuide const guide{.id = "guide-1", .first = {.x = 100, .y = 50}, .second = {.x = 200, .y = 50}};
	VisualGuideOverlayStyle style;
	style.label_font_size = 100;
	CaptureOverlayContext context;
	std::vector<std::string> measured_rows;
	auto const measure = [&](std::string const& text, VideoOverlayTextStyle const& text_style) {
		measured_rows.push_back(text);
		EXPECT_EQ("Verdana", text_style.face);
		EXPECT_EQ(72, text_style.size);
		EXPECT_TRUE(text_style.bold);
		EXPECT_EQ(agi::Color(255, 255, 255), text_style.colour);
		return context.MeasureText(text, text_style);
	};
	auto const geometry = ComputeMeasurementLabelGeometry(
		measure, guide, MakeViewport(), {300, 150}, {500, 150}, style);
	EXPECT_EQ((std::vector<std::string>{
				  "dX +100", "dY 0", "L 100", "sx -", "sy 0", "angH 0 deg", "angV 90 deg"}),
			  measured_rows);
	EXPECT_EQ(Vector2D(500, 162), geometry.origin);
	EXPECT_EQ(Vector2D(85, 92), geometry.size);

	auto const direct = ComputeMeasurementLabelGeometry(
		context, guide, MakeViewport(), {300, 150}, {500, 150}, style);
	EXPECT_EQ(geometry.origin, direct.origin);
	EXPECT_EQ(geometry.size, direct.size);

	auto const different_metrics = ComputeMeasurementLabelGeometry(
		[](std::string const&, VideoOverlayTextStyle const&) {
			return VideoOverlayTextExtent(73, 9);
		},
		guide, MakeViewport(), {300, 150}, {500, 150}, style);
	EXPECT_EQ(Vector2D(500, 162), different_metrics.origin);
	EXPECT_EQ(Vector2D(81, 71), different_metrics.size);
}

TEST(visual_guide_overlay, draws_selected_measurement_with_filled_quads_and_label) {
	VisualGuide guide;
	guide.id = "guide-1";
	guide.first = { 100.0, 50.0 };
	guide.second = { 200.0, 100.0 };

	VisualGuideSnapshot snapshot;
	snapshot.guides.push_back(guide);
	snapshot.selected_id = guide.id;

	CaptureOverlayContext context;
	VisualGuideOverlayStyle style;
	style.line_colour = agi::Color(10, 20, 30);
	style.highlight_colour = agi::Color(40, 50, 60);
	VisualGuideOverlay().Draw(context, MakeViewport(), ViewFrom(snapshot), style);

	// Two line-stroke quads (outline + core) then two arrowhead triangles
	// (outline + core) at the second endpoint.
	ASSERT_EQ(4u, context.polygons.size());
	EXPECT_EQ(4u, context.polygons[0].points.size());
	EXPECT_EQ(4u, context.polygons[1].points.size());
	EXPECT_EQ(3u, context.polygons[2].points.size());
	EXPECT_EQ(3u, context.polygons[3].points.size());
	EXPECT_EQ(40, context.polygons[1].fill_colour.r);
	EXPECT_EQ(50, context.polygons[1].fill_colour.g);
	EXPECT_EQ(60, context.polygons[1].fill_colour.b);
	EXPECT_FLOAT_EQ(1.0f, context.polygons[1].fill_alpha);
	// Arrowhead core uses the highlight colour for a selected guide.
	EXPECT_EQ(40, context.polygons[3].fill_colour.r);
	EXPECT_EQ(0, context.invert_count);

	// Label background rectangle plus the seven metric rows.
	ASSERT_EQ(1u, context.rectangles.size());
	EXPECT_GE(context.rectangles[0].first.X(), 100.0f);
	EXPECT_GE(context.rectangles[0].first.Y(), 50.0f);
	EXPECT_LE(context.rectangles[0].second.X(), 900.0f);
	EXPECT_LE(context.rectangles[0].second.Y(), 450.0f);
	ASSERT_EQ(7u, context.text.size());
	EXPECT_EQ("dX +100", context.text[0].value);
	EXPECT_EQ("dY +50", context.text[1].value);
	EXPECT_EQ("L 111.8", context.text[2].value);
	// shear_x = dx/dy = 2, shear_y = dy/dx = 0.5 (no leading + for shears).
	EXPECT_EQ("sx 2", context.text[3].value);
	EXPECT_EQ("sy 0.5", context.text[4].value);
	EXPECT_EQ("angH 26.57 deg", context.text[5].value);
	EXPECT_EQ("angV 63.43 deg", context.text[6].value);
}

TEST(visual_guide_overlay, label_is_anchored_at_second_endpoint_and_renders_axis_dash) {
	VisualGuide guide;
	guide.id = "guide-1";
	// Pure horizontal segment: shear_x denominator (dy) is 0, so sx shows a dash.
	guide.first = { 100.0, 50.0 };
	guide.second = { 200.0, 50.0 };

	VisualGuideSnapshot snapshot;
	snapshot.guides.push_back(guide);

	CaptureOverlayContext context;
	VisualGuideOverlay().Draw(context, MakeViewport(), ViewFrom(snapshot), {});

	ASSERT_EQ(7u, context.text.size());
	EXPECT_EQ("sx -", context.text[3].value); // dash when dy == 0
	// shear_y = dy/dx = 0 -> formatted as 0.
	EXPECT_EQ("sy 0", context.text[4].value);

	// The label is anchored beside the second endpoint (x=200 script ->
	// canvas 500, y=50 script -> canvas 150) with its corner at the endpoint
	// plus a normal-side gap, so it must NOT straddle the endpoint on the
	// vertical axis (it sits entirely above or below the line).
	ASSERT_EQ(1u, context.rectangles.size());
	auto const& rect = context.rectangles[0];
	auto const top = std::min(rect.first.Y(), rect.second.Y());
	auto const bottom = std::max(rect.first.Y(), rect.second.Y());
	bool const straddles_y = top <= 150.0f && bottom >= 150.0f;
	EXPECT_FALSE(straddles_y);
	// The box still overlaps the endpoint horizontally (anchored on its edge).
	EXPECT_LE(rect.first.X(), 500.0f);
	EXPECT_GE(rect.second.X(), 500.0f);
}

TEST(visual_guide_overlay, skips_guides_when_viewport_is_unmappable) {
	auto viewport = MakeViewport();
	viewport.script_width = 0.0;

	VisualGuide guide;
	guide.id = "guide-1";
	guide.first = { 1.0, 2.0 };
	guide.second = { 3.0, 4.0 };

	VisualGuideSnapshot snapshot;
	snapshot.guides.push_back(guide);
	CaptureOverlayContext context;
	VisualGuideOverlay().Draw(context, viewport, ViewFrom(snapshot), {});

	EXPECT_TRUE(context.polygons.empty());
	EXPECT_TRUE(context.rectangles.empty());
	EXPECT_TRUE(context.text.empty());
}
