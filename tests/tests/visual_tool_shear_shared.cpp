#include <main.h>

#include "../../src/video_overlay_draw_context.h"
#include "../../src/visual_tool_shear_shared.h"

#include <array>
#include <limits>
#include <utility>

namespace {
using namespace aegisub::visual_shear;

class ShearOverlayRecorder final : public VideoOverlayDrawContext {
	public:
	struct Paint {
		agi::Color colour;
		float alpha;
		int width;
	};
	struct Segment {
		Vector2D first;
		Vector2D second;
		Paint paint;
	};
	struct Shape {
		Vector2D first;
		Vector2D second;
		float radius;
		Paint line;
		Paint fill;
	};
	struct Text {
		std::string text;
		int x;
		int y;
		VideoOverlayTextStyle style;
	};
	Paint line{};
	Paint fill{};
	std::vector<Segment> segments;
	std::vector<Shape> rectangles;
	std::vector<Shape> circles;
	std::vector<Text> texts;
	void SetLineColour(agi::Color const& colour, float alpha, int width) override { line = {.colour = colour, .alpha = alpha, .width = width}; }
	void SetFillColour(agi::Color const& colour, float alpha) override { fill = {.colour = colour, .alpha = alpha, .width = 0}; }
	void SetInvert() override { ADD_FAILURE() << "Shear must not enable inversion"; }
	void ClearInvert() override { ADD_FAILURE() << "Shear must not clear inversion"; }
	void DrawLine(Vector2D first, Vector2D second) override { segments.push_back({.first = first, .second = second, .paint = line}); }
	void DrawLines(size_t, float const *, size_t) override { ADD_FAILURE() << "Unexpected batch"; }
	void DrawLineStrip(Vector2D const *, size_t) override { ADD_FAILURE() << "Unexpected strip"; }
	void DrawRectangle(Vector2D first, Vector2D second) override { rectangles.push_back({.first = first, .second = second, .radius = 0, .line = line, .fill = fill}); }
	void DrawPolygon(Vector2D const *, size_t) override { ADD_FAILURE() << "Unexpected polygon"; }
	void DrawMultiPolygon(std::vector<float> const&, std::vector<int> const&, std::vector<int> const&, Vector2D, Vector2D, bool) override { ADD_FAILURE() << "Unexpected multipolygon"; }
	void DrawCircle(Vector2D center, float radius) override { circles.push_back({.first = center, .second = {}, .radius = radius, .line = line, .fill = fill}); }
	void DrawTriangle(Vector2D, Vector2D, Vector2D) override { ADD_FAILURE() << "Unexpected triangle"; }
	VideoOverlayTextExtent MeasureText(std::string const&, VideoOverlayTextStyle const&) override { return {}; }
	void DrawText(std::string const& text, int x, int y, VideoOverlayTextStyle const& style) override { texts.push_back({.text = text, .x = x, .y = y, .style = style}); }
};
}

TEST(visual_tool_shear_shared, geometry_preserves_center_margin_handle_clamp_and_small_extent_order) {
	auto interior = MakeGeometry({240, 220}, {40, 30}, {800, 600}, {.25f, -.5f});
	ASSERT_TRUE(interior);
	EXPECT_EQ(Vector2D(240, 220), interior->center);
	EXPECT_EQ(Vector2D(260, 120), interior->handles[0]);
	EXPECT_EQ(Vector2D(340, 180), interior->handles[1]);
	auto edge = MakeGeometry({-500, 900}, {40, 30}, {800, 600}, {20, -20});
	ASSERT_TRUE(edge);
	EXPECT_EQ(Vector2D(200, 470), edge->center);
	EXPECT_EQ(Vector2D(830, 370), edge->handles[0]);
	EXPECT_EQ(Vector2D(300, 40), edge->handles[1]);
	auto small = MakeGeometry({500, -500}, {40, 30}, {15, 12}, {0, 0});
	ASSERT_TRUE(small);
	EXPECT_EQ(Vector2D(47.5f, 36.f), small->center);
	EXPECT_EQ(Vector2D(45, 32), small->handles[0]);
	EXPECT_EQ(Vector2D(45, 32), small->handles[1]);
}

TEST(visual_tool_shear_shared, geometry_rejects_invalid_input_and_keeps_finite_overflow_clamp) {
	float const nan = std::numeric_limits<float>::quiet_NaN();
	float const inf = std::numeric_limits<float>::infinity();
	EXPECT_FALSE(MakeGeometry({nan, 220.f}, {40, 30}, {800, 600}, {0, 0}));
	EXPECT_FALSE(MakeGeometry({240, 220}, {40.f, inf}, {800, 600}, {0, 0}));
	EXPECT_FALSE(MakeGeometry({240, 220}, {40, 30}, {0, 600}, {0, 0}));
	EXPECT_FALSE(MakeGeometry({240, 220}, {40, 30}, {800, -1}, {0, 0}));
	EXPECT_FALSE(MakeGeometry({240, 220}, {40, 30}, {inf, 600.f}, {0, 0}));
	EXPECT_FALSE(MakeGeometry({240, 220}, {40, 30}, {800, 600}, {nan, 0.f}));
	EXPECT_FALSE(MakeGeometry({240, 220}, {40, 30}, {800, 600}, {0.f, inf}));
	auto clamped = MakeGeometry({240, 220}, {40, 30}, {800, 600},
								{std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()});
	ASSERT_TRUE(clamped);
	EXPECT_EQ(Vector2D(830, 120), clamped->handles[0]);
	EXPECT_EQ(Vector2D(340, 40), clamped->handles[1]);
}

TEST(visual_tool_shear_shared, hit_test_preserves_strict_square_circle_boundaries_and_later_handle_tie) {
	Geometry geometry{.center = {240, 220}, .handles = {Vector2D(260, 120), Vector2D(340, 180)}};
	EXPECT_EQ(Axis::X, HitTest(geometry, {265.9f, 125.9f}));
	EXPECT_FALSE(HitTest(geometry, {266, 120}));
	EXPECT_EQ(Axis::Y, HitTest(geometry, {343, 184}));
	EXPECT_FALSE(HitTest(geometry, {346, 180}));
	EXPECT_FALSE(HitTest(geometry, {345, 185}));
	EXPECT_FALSE(HitTest(geometry, {std::numeric_limits<float>::quiet_NaN(), 180.f}));
	geometry.handles = {Vector2D(300, 200), Vector2D(300, 200)};
	EXPECT_EQ(Axis::Y, HitTest(geometry, {300, 200}));
	EXPECT_EQ(Axis::X, HitTest(geometry, {305, 205}));
}

TEST(visual_tool_shear_shared, arithmetic_preserves_axis_precedence_screen_lever_and_decimal_format) {
	EXPECT_STREQ("\\fax", Tag(Axis::X));
	EXPECT_STREQ("\\fay", Tag(Axis::Y));
	EXPECT_EQ(Axis::X, NudgeAxis({1, -2}));
	EXPECT_EQ(Axis::Y, NudgeAxis({0, -2}));
	EXPECT_FLOAT_EQ(.25f, DragDelta(Axis::X, {20, 80}));
	EXPECT_FLOAT_EQ(-.25f, DragDelta(Axis::Y, {80, -20}));
	EXPECT_FLOAT_EQ(-.2f, NudgeDelta(Axis::X, {-2, 3}, .1f));
	EXPECT_FLOAT_EQ(.3f, NudgeDelta(Axis::Y, {-2, 3}, .1f));
	EXPECT_EQ("0.375", FormatAdjustedValue(.25f, .125f));
	EXPECT_EQ("-0.625", FormatAdjustedValue(-.75f, .125f));
	EXPECT_EQ("0", FormatAdjustedValue(.25f, -.25f));
	EXPECT_EQ("1.235", FormatValue(1.23456f));
	EXPECT_EQ("-1.235", FormatValue(-1.23456f));
	EXPECT_FALSE(FormatAdjustedValue(std::numeric_limits<float>::max(), std::numeric_limits<float>::max()));
	EXPECT_FALSE(FormatAdjustedValue(std::numeric_limits<float>::quiet_NaN(), .25f));
	EXPECT_FALSE(FormatAdjustedValue(.25f, std::numeric_limits<float>::infinity()));
}

TEST(visual_tool_shear_shared, overlay_preserves_literal_rails_features_labels_and_host_supplied_style) {
	OverlayState state;
	state.geometry = {.center = {240, 220}, .handles = {Vector2D(260, 120), Vector2D(340, 180)}};
	state.coefficients = {.25f, -.5f};
	state.video_position = {40.75f, 30.75f};
	state.video_size = {800, 600};
	state.primary_colour = agi::Color(10, 20, 30);
	state.secondary_colour = agi::Color(40, 50, 60);
	state.handle_fill_colours = {agi::Color(70, 80, 90), agi::Color(100, 110, 120)};
	state.font_size = 21;
	state.hint = "localized shear hint";
	ShearOverlayRecorder draw;
	DrawOverlay(draw, state);
	std::array<std::pair<Vector2D, Vector2D>, 8> const expected{
		std::pair{Vector2D(120, 120), Vector2D(360, 120)},
		std::pair{Vector2D(340, 100), Vector2D(340, 340)},
		std::pair{Vector2D(240, 114), Vector2D(240, 126)},
		std::pair{Vector2D(334, 220), Vector2D(346, 220)},
		std::pair{Vector2D(260, 108), Vector2D(260, 132)},
		std::pair{Vector2D(248, 120), Vector2D(272, 120)},
		std::pair{Vector2D(340, 168), Vector2D(340, 192)},
		std::pair{Vector2D(328, 180), Vector2D(352, 180)}};
	ASSERT_EQ(expected.size(), draw.segments.size());
	for (std::size_t i = 0; i < expected.size(); ++i) {
		EXPECT_EQ(expected[i].first, draw.segments[i].first) << i;
		EXPECT_EQ(expected[i].second, draw.segments[i].second) << i;
		EXPECT_EQ(state.secondary_colour, draw.segments[i].paint.colour) << i;
		EXPECT_FLOAT_EQ(i < 4 ? .7f : 1.f, draw.segments[i].paint.alpha) << i;
		EXPECT_EQ(1, draw.segments[i].paint.width) << i;
	}
	ASSERT_EQ(1u, draw.rectangles.size());
	EXPECT_EQ(Vector2D(254, 114), draw.rectangles[0].first);
	EXPECT_EQ(Vector2D(266, 126), draw.rectangles[0].second);
	EXPECT_EQ(state.handle_fill_colours[0], draw.rectangles[0].fill.colour);
	EXPECT_FLOAT_EQ(.3f, draw.rectangles[0].fill.alpha);
	ASSERT_EQ(1u, draw.circles.size());
	EXPECT_EQ(Vector2D(340, 180), draw.circles[0].first);
	EXPECT_EQ(6, draw.circles[0].radius);
	EXPECT_EQ(state.handle_fill_colours[1], draw.circles[0].fill.colour);
	EXPECT_FLOAT_EQ(.3f, draw.circles[0].fill.alpha);
	ASSERT_EQ(3u, draw.texts.size());
	EXPECT_EQ("fax 0.25", draw.texts[0].text);
	EXPECT_EQ(120, draw.texts[0].x);
	EXPECT_EQ(94, draw.texts[0].y);
	EXPECT_EQ("fay -0.5", draw.texts[1].text);
	EXPECT_EQ(350, draw.texts[1].x);
	EXPECT_EQ(244, draw.texts[1].y);
	EXPECT_EQ(state.hint, draw.texts[2].text);
	EXPECT_EQ(48, draw.texts[2].x);
	EXPECT_EQ(606, draw.texts[2].y);
	for (auto const& text : draw.texts) {
		EXPECT_TRUE(text.style.face.empty());
		EXPECT_FALSE(text.style.bold);
		EXPECT_FALSE(text.style.italic);
		EXPECT_TRUE(text.style.outline);
		EXPECT_EQ(21, text.style.size);
		EXPECT_EQ(state.primary_colour, text.style.colour);
	}
}

TEST(visual_tool_shear_shared, diagnostic_replaces_handles_rails_and_hint_at_video_origin) {
	OverlayState state;
	state.video_position = {40.75f, 30.75f};
	state.video_size = {800, 600};
	state.primary_colour = agi::Color(10, 20, 30);
	state.font_size = 19;
	state.hint = "localized hint";
	state.diagnostic = "localized unsupported selection";
	ShearOverlayRecorder draw;
	DrawOverlay(draw, state);
	EXPECT_TRUE(draw.segments.empty());
	EXPECT_TRUE(draw.rectangles.empty());
	EXPECT_TRUE(draw.circles.empty());
	ASSERT_EQ(1u, draw.texts.size());
	EXPECT_EQ(state.diagnostic, draw.texts[0].text);
	EXPECT_EQ(48, draw.texts[0].x);
	EXPECT_EQ(38, draw.texts[0].y);
	EXPECT_EQ(19, draw.texts[0].style.size);
	EXPECT_EQ(state.primary_colour, draw.texts[0].style.colour);
}
