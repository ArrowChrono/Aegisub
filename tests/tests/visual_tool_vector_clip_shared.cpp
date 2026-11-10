#include <main.h>

#include "../../src/video_overlay_draw_context.h"
#include "../../src/visual_tool_vector_clip_shared.h"

#include <array>
#include <cmath>
#include <limits>

namespace {
using namespace aegisub::visual_vector_clip;

Spline Path(std::string const& drawing) {
	Spline path;
	path.DecodeFromAss(drawing);
	return path;
}

class VectorOverlayRecorder final : public VideoOverlayDrawContext {
	public:
	struct Paint {
		agi::Color colour;
		float alpha = 0;
		int width = 0;
	};
	struct Polygon {
		std::vector<float> points;
		std::vector<int> starts;
		std::vector<int> counts;
		Vector2D origin;
		Vector2D size;
		bool inverted;
		Paint line;
		Paint fill;
	};
	struct Shape {
		Vector2D first;
		Vector2D second;
		float radius;
		Paint line;
		Paint fill;
	};
	Paint line;
	Paint fill;
	std::vector<Polygon> polygons;
	std::vector<Shape> circles;
	std::vector<Shape> rectangles;
	std::vector<std::vector<Vector2D>> strips;
	std::vector<Paint> strip_paints;
	std::vector<std::pair<Vector2D, Vector2D>> dashes;
	void SetLineColour(agi::Color const& colour, float alpha, int width) override { line = {.colour = colour, .alpha = alpha, .width = width}; }
	void SetFillColour(agi::Color const& colour, float alpha) override { fill = {.colour = colour, .alpha = alpha, .width = 0}; }
	void SetInvert() override { ADD_FAILURE() << "Unexpected invert state"; }
	void ClearInvert() override { ADD_FAILURE() << "Unexpected invert state"; }
	void DrawLine(Vector2D first, Vector2D second) override { dashes.emplace_back(first, second); }
	void DrawLines(size_t dimension, float const *points, size_t count) override {
		ASSERT_EQ(2u, dimension);
		ASSERT_EQ(0u, count % 2);
		for (size_t index = 0; index < count; index += 2)
			dashes.emplace_back(Vector2D(points[index * 2], points[index * 2 + 1]),
								Vector2D(points[index * 2 + 2], points[index * 2 + 3]));
	}
	void DrawLineStrip(Vector2D const *points, size_t count) override {
		strips.emplace_back(points, points + count);
		strip_paints.push_back(line);
	}
	void DrawRectangle(Vector2D first, Vector2D second) override { rectangles.push_back({.first = first, .second = second, .radius = 0, .line = line, .fill = fill}); }
	void DrawPolygon(Vector2D const *, size_t) override { ADD_FAILURE() << "Expected one multi-polygon batch"; }
	void DrawMultiPolygon(std::vector<float> const& points, std::vector<int> const& starts,
						  std::vector<int> const& counts, Vector2D origin, Vector2D size, bool inverted) override {
		polygons.push_back({.points = points, .starts = starts, .counts = counts, .origin = origin, .size = size, .inverted = inverted, .line = line, .fill = fill});
	}
	void DrawCircle(Vector2D center, float radius) override { circles.push_back({.first = center, .second = {}, .radius = radius, .line = line, .fill = fill}); }
	void DrawTriangle(Vector2D, Vector2D, Vector2D) override { ADD_FAILURE() << "Unexpected triangle"; }
	VideoOverlayTextExtent MeasureText(std::string const&, VideoOverlayTextStyle const&) override {
		ADD_FAILURE() << "Vector Clip has no text";
		return {};
	}
	void DrawText(std::string const&, int, int, VideoOverlayTextStyle const&) override { ADD_FAILURE() << "Vector Clip has no text"; }
};
}

TEST(visual_tool_vector_clip_shared, codec_preserves_scale_subpaths_and_truncates_towards_zero) {
	Spline path({.video_position = {40, 30}, .video_size = {1280, 240}, .script_size = {640, 480}});
	path.SetScale(3);
	path.DecodeFromAss("m -8 16 l 32 48 64 16 m 80 -8 b 96 -16 112 0 128 16");
	ASSERT_EQ(5u, path.size());
	EXPECT_EQ(Vector2D(36, 32), path[0].p1);
	EXPECT_EQ(Vector2D(56, 36), path[1].p2);
	EXPECT_EQ(Vector2D(80, 29), path[3].p1);
	EXPECT_EQ(Vector2D(88, 28), path[4].p2);
	EXPECT_EQ(Vector2D(96, 30), path[4].p3);
	EXPECT_EQ(Vector2D(104, 32), path[4].p4);
	EXPECT_EQ("(3,m -8 16 l 32 48 64 16 m 80 -8 b 96 -16 112 0 128 16 )", BuildClipValue(path));
	path.MovePoint(path.begin(), 0, {39.25f, 29.8125f});
	EXPECT_EQ("(3,m -1 -1 l 32 48 64 16 m 80 -8 b 96 -16 112 0 128 16 )", BuildClipValue(path));
	auto const original = path;
	path.SetCoordinateSystem({.video_position = {0, 0}, .video_size = {1, 1}, .script_size = {1, 1}});
	path.DecodeFromAss("m 4 8 l 12 16");
	EXPECT_EQ("(3,m 4 8 l 12 16 )", BuildClipValue(path));
	EXPECT_EQ("(3,m -1 -1 l 32 48 64 16 m 80 -8 b 96 -16 112 0 128 16 )", BuildClipValue(original));
	EXPECT_EQ(1, Spline().GetScale());
	EXPECT_EQ("()", BuildClipValue(Spline()));
}

TEST(visual_tool_vector_clip_shared, codec_admission_requires_complete_supported_finite_groups) {
	std::array<char const *, 6> const accepted{
		"", "m 0 0", "m -1.5 2.25 l 30 40 50 60",
		"m 1 2 3 4 l 5 6",
		"m 0 0 b 10 0 20 10 30 10 40 10 50 0 60 0", "m 0 0 l 10 10 m 50 60 l 70 80"};
	for (size_t index = 0; index < accepted.size(); ++index) {
		SCOPED_TRACE(index);
		EXPECT_TRUE(IsEditableDrawing(accepted[index], 1));
	}
	std::array<char const *, 19> const rejected{
		"m", "m 0", "m 0 0 l", "m 0 0 l 2", "m 0 0 b 1 2 3 4 5",
		"m 0 0 b 1 2 3 4 5 6 7", "m 0 0 n 10 10", "m 0 0 s 10 10 20 20 30 30",
		"m 0 0 p 10 10", "m 0 0 c", "m 0 0 q 1 2", "m 0 0 garbage",
		"m NaN 0", "m 0 inf", "m 1e100 0", "m 0 0 l 2147483648 0", "m 0 0 b 1 2 m 3 4",
		"l 20 30", "b 0 0 10 10 20 20"};
	for (size_t index = 0; index < rejected.size(); ++index) {
		SCOPED_TRACE(index);
		EXPECT_FALSE(IsEditableDrawing(rejected[index], 1));
	}
}

TEST(visual_tool_vector_clip_shared, encodability_checks_only_used_points_and_scaled_signed_integer_bounds) {
	Spline path;
	path.emplace_back(Vector2D(-2147483648.f, 2147483520.f));
	ASSERT_TRUE(IsEncodable(path));
	path[0].p2 = {std::numeric_limits<float>::quiet_NaN(), 0.f};
	EXPECT_TRUE(IsEncodable(path));
	path[0].p1 = {2147483648.f, 0.f};
	EXPECT_FALSE(IsEncodable(path));
	path[0].p1 = {std::nextafter(-2147483648.f, -std::numeric_limits<float>::infinity()), 0.f};
	EXPECT_FALSE(IsEncodable(path));
	path[0].p1 = {0.f, std::numeric_limits<float>::infinity()};
	EXPECT_FALSE(IsEncodable(path));
	path[0].p1 = {std::numeric_limits<float>::quiet_NaN(), 0.f};
	EXPECT_FALSE(IsEncodable(path));
	path[0].p1 = {536870912.f, 0.f};
	path.SetScale(3);
	EXPECT_FALSE(IsEncodable(path));
	path[0].p1 = {536870880.f, 0.f};
	EXPECT_TRUE(IsEncodable(path));
	path.emplace_back(Vector2D(0, 0), Vector2D(1, 1), Vector2D(2, 2), Vector2D(3, 3));
	path.back().p3 = {std::numeric_limits<float>::infinity(), 0.f};
	EXPECT_FALSE(IsEncodable(path));
}

TEST(visual_tool_vector_clip_shared, features_identify_move_line_and_bicubic_handles_with_strict_hit_boundaries) {
	auto path = Path("m 10 20 l 30 40 b 50 60 70 80 90 100");
	auto features = MakeFeatures(path);
	ASSERT_EQ(5u, features.size());
	std::array<Vector2D, 5> const points{Vector2D(10, 20), Vector2D(30, 40), Vector2D(50, 60), Vector2D(70, 80), Vector2D(90, 100)};
	std::array<size_t, 5> const curves{0, 1, 2, 2, 2};
	std::array<int, 5> const parts{0, 1, 1, 2, 3};
	for (size_t index = 0; index < features.size(); ++index) {
		EXPECT_EQ(points[index], features[index].position);
		EXPECT_EQ(curves[index], features[index].curve_index);
		EXPECT_EQ(parts[index], features[index].point);
		EXPECT_EQ(index == 2 || index == 3, features[index].square);
		EXPECT_FALSE(features[index].selected);
		EXPECT_FALSE(features[index].active);
	}
	EXPECT_EQ(std::optional<size_t>(2), HitTest(features, {52.9f, 62.9f}));
	EXPECT_FALSE(HitTest(features, {53, 60}));
	EXPECT_EQ(std::optional<size_t>(0), HitTest(features, {12, 22}));
	EXPECT_FALSE(HitTest(features, {13, 20}));
	EXPECT_FALSE(HitTest(features, {12.5f, 22.5f}));
	EXPECT_FALSE(HitTest(features, {std::numeric_limits<float>::quiet_NaN(), 20.f}));
	features[4].position = features[0].position;
	EXPECT_EQ(std::optional<size_t>(4), HitTest(features, {10, 20}));
	std::vector<Feature> appended;
	AppendFeatures(path, 2, appended);
	ASSERT_EQ(3u, appended.size());
	EXPECT_EQ(Vector2D(50, 60), appended[0].position);
	EXPECT_EQ(Vector2D(90, 100), appended[2].position);
}

TEST(visual_tool_vector_clip_shared, box_bounds_are_inclusive_on_all_edges) {
	EXPECT_TRUE(InBox({10, 20}, {30, 40}, {10, 20}));
	EXPECT_TRUE(InBox({10, 20}, {30, 40}, {30, 40}));
	EXPECT_TRUE(InBox({10, 20}, {30, 40}, {20, 30}));
	EXPECT_FALSE(InBox({10, 20}, {30, 40}, {9.99f, 30.f}));
	EXPECT_FALSE(InBox({10, 20}, {30, 40}, {30.01f, 30.f}));
	EXPECT_FALSE(InBox({10, 20}, {30, 40}, {20.f, 19.99f}));
	EXPECT_FALSE(InBox({10, 20}, {30, 40}, {20.f, 40.01f}));
}

TEST(visual_tool_vector_clip_shared, moving_endpoints_reconnects_adjacent_segments_without_bridging_subpaths) {
	auto path = Path("m 10 20 l 30 40 b 40 50 50 60 60 70 m 100 110 l 120 130");
	path.MovePoint(path.begin(), 0, {11, 22});
	EXPECT_EQ(Vector2D(11, 22), path[1].p1);
	path.MovePoint(path.begin() + 1, 1, {33, 44});
	EXPECT_EQ(Vector2D(33, 44), path[2].p1);
	path.MovePoint(path.begin() + 2, 1, {45, 55});
	path.MovePoint(path.begin() + 2, 2, {65, 75});
	path.MovePoint(path.begin() + 2, 3, {80, 90});
	EXPECT_EQ(Vector2D(100, 110), path[3].p1);
	EXPECT_EQ(Vector2D(100, 110), path[4].p1);
	EXPECT_EQ("m 11 22 l 33 44 b 45 55 65 75 80 90 m 100 110 l 120 130 ", path.EncodeToAss());
	path.MovePoint(path.begin() + 3, 0, {101, 112});
	EXPECT_EQ(Vector2D(80, 90), path[2].p4);
	EXPECT_EQ(Vector2D(101, 112), path[4].p1);
}

TEST(visual_tool_vector_clip_shared, append_line_move_and_bicubic_use_previous_segment_tangent) {
	Spline path;
	Append(path, Mode::Line, {10, 20});
	UpdateAppend(path, Mode::Line, {20, 30});
	ASSERT_EQ(1u, path.size());
	EXPECT_EQ(SplineCurve::POINT, path[0].type);
	EXPECT_EQ(Vector2D(20, 30), path[0].p1);
	Append(path, Mode::Line, {100, 30});
	UpdateAppend(path, Mode::Line, {100, 30});
	Append(path, Mode::Bicubic, {100, 110});
	UpdateAppend(path, Mode::Bicubic, {100, 110});
	ASSERT_EQ(3u, path.size());
	EXPECT_EQ(Vector2D(100, 30), path[2].p1);
	EXPECT_EQ(Vector2D(120, 30), path[2].p2);
	EXPECT_EQ(Vector2D(100, 90), path[2].p3);
	EXPECT_EQ(Vector2D(100, 110), path[2].p4);
	UpdateAppend(path, Mode::Bicubic, {100, 70});
	EXPECT_EQ(Vector2D(110, 30), path[2].p2);
	EXPECT_EQ(Vector2D(100, 60), path[2].p3);
	Append(path, Mode::Move, {200, 150});
	EXPECT_EQ(SplineCurve::POINT, path.back().type);
	EXPECT_EQ(Vector2D(200, 150), path.back().p1);
	EXPECT_EQ("m 20 30 l 100 30 b 110 30 100 60 100 70 m 200 150 ", path.EncodeToAss());
}

TEST(visual_tool_vector_clip_shared, convert_and_split_use_literal_bezier_geometry_and_preserve_point_split) {
	auto path = Path("m 0 0 l 80 0");
	EXPECT_FALSE(Convert(path, {.curve_index = 0, .parameter = 0, .position = {0, 0}}));
	auto point_split = path;
	EXPECT_TRUE(Insert(point_split, {.curve_index = 0, .parameter = 0, .position = {0, 0}}));
	EXPECT_EQ("m 0 0 0 0 l 80 0 ", point_split.EncodeToAss());
	ASSERT_TRUE(Convert(path, {.curve_index = 1, .parameter = .5f, .position = {40, 0}}));
	EXPECT_EQ("m 0 0 b 20 0 60 0 80 0 ", path.EncodeToAss());
	ASSERT_TRUE(Convert(path, {.curve_index = 1, .parameter = .5f, .position = {40, 0}}));
	EXPECT_EQ("m 0 0 l 80 0 ", path.EncodeToAss());
	ASSERT_TRUE(Insert(path, {.curve_index = 1, .parameter = .25f, .position = {20, 0}}));
	EXPECT_EQ("m 0 0 l 20 0 80 0 ", path.EncodeToAss());

	path = Path("m 0 0 b 0 80 80 80 80 0");
	ASSERT_TRUE(Insert(path, {.curve_index = 1, .parameter = .5f, .position = {40, 60}}));
	ASSERT_EQ(3u, path.size());
	EXPECT_EQ("m 0 0 b 0 40 20 60 40 60 60 60 80 40 80 0 ", path.EncodeToAss());
	EXPECT_EQ(Vector2D(40, 60), path[2].p1);
}

TEST(visual_tool_vector_clip_shared, admission_scale_accepts_only_positive_signed_power_of_two_range) {
	for (int scale : {1, 2, 3, 30, 31}) {
		SCOPED_TRACE(scale);
		EXPECT_TRUE(IsEditableDrawing("m 0 0 l 16 32", scale));
		EXPECT_TRUE(IsEditableDrawing("", scale));
	}
	for (int scale : {0, -1, 32, 33, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
		SCOPED_TRACE(scale);
		EXPECT_FALSE(IsEditableDrawing("m 0 0 l 16 32", scale));
		EXPECT_FALSE(IsEditableDrawing("", scale));
	}
}

TEST(visual_tool_vector_clip_shared, each_subpath_closing_hit_inserts_before_its_next_move_and_never_global_diagonal) {
	auto path = Path("m 10 10 l 90 10 90 90 m 210 210 l 290 210 290 290");
	auto first = FindClosest(path, {50, 50});
	ASSERT_TRUE(first);
	ASSERT_TRUE(first->closing);
	EXPECT_EQ(0u, first->closing->first);
	EXPECT_EQ(2u, first->closing->last);
	EXPECT_EQ(Vector2D(50, 50), first->position);
	EXPECT_FLOAT_EQ(.5f, first->parameter);
	ASSERT_TRUE(Insert(path, *first));
	EXPECT_EQ("m 10 10 l 90 10 90 90 50 50 m 210 210 l 290 210 290 290 ", path.EncodeToAss());
	auto second = FindClosest(path, {250, 250});
	ASSERT_TRUE(second);
	ASSERT_TRUE(second->closing);
	EXPECT_EQ(4u, second->closing->first);
	EXPECT_EQ(6u, second->closing->last);
	EXPECT_EQ(Vector2D(250, 250), second->position);
	EXPECT_FLOAT_EQ(.5f, second->parameter);
	ASSERT_TRUE(Insert(path, *second));
	EXPECT_EQ("m 10 10 l 90 10 90 90 50 50 m 210 210 l 290 210 290 290 250 250 ", path.EncodeToAss());
	auto gap = FindClosest(path, {150, 150});
	ASSERT_TRUE(gap);
	EXPECT_NE(Vector2D(150, 150), gap->position);
	EXPECT_EQ(Vector2D(90, 90), gap->position);
	EXPECT_FALSE(gap->closing);
}

TEST(visual_tool_vector_clip_shared, closest_and_insert_use_implicit_closing_edge_without_converting_it) {
	auto path = Path("m 10 10 l 90 10 90 90");
	auto nearest = FindClosest(path, {50, 50});
	ASSERT_TRUE(nearest);
	EXPECT_EQ(Vector2D(50, 50), nearest->position);
	EXPECT_FLOAT_EQ(.5f, nearest->parameter);
	auto const original = path.EncodeToAss();
	EXPECT_FALSE(Convert(path, *nearest));
	EXPECT_EQ(original, path.EncodeToAss());
	ASSERT_TRUE(Insert(path, *nearest));
	EXPECT_EQ("m 10 10 l 90 10 90 90 50 50 ", path.EncodeToAss());
	EXPECT_FALSE(FindClosest(Spline(), {10, 10}));
}

TEST(visual_tool_vector_clip_shared, remove_controls_endpoints_and_moves_reconnects_only_owned_segments) {
	auto controls = Path("m 0 0 b 20 30 40 30 60 0 l 90 0");
	for (int point : {1, 2}) {
		SCOPED_TRACE(point);
		auto path = controls;
		ASSERT_TRUE(RemovePoint(path, 1, point));
		EXPECT_EQ("m 0 0 l 60 0 90 0 ", path.EncodeToAss());
	}
	ASSERT_TRUE(RemovePoint(controls, 1, 3));
	EXPECT_EQ("m 0 0 l 90 0 ", controls.EncodeToAss());
	EXPECT_EQ(Vector2D(0, 0), controls[1].p1);
	auto path = Path("m 10 10 l 30 30 50 50 m 100 100 l 120 120");
	ASSERT_TRUE(RemovePoint(path, 0, 0));
	EXPECT_EQ("m 30 30 l 50 50 m 100 100 l 120 120 ", path.EncodeToAss());
	ASSERT_TRUE(RemovePoint(path, 1, 1));
	EXPECT_EQ("m 30 30 100 100 l 120 120 ", path.EncodeToAss());
	EXPECT_EQ(Vector2D(100, 100), path[1].p1);
	auto const unchanged = path.EncodeToAss();
	EXPECT_FALSE(RemovePoint(path, 20, 0));
	EXPECT_FALSE(RemovePoint(path, 0, 3));
	EXPECT_EQ(unchanged, path.EncodeToAss());
	auto singleton = Path("m 8 9");
	ASSERT_TRUE(RemovePoint(singleton, 0, 0));
	EXPECT_TRUE(singleton.empty());
	EXPECT_EQ("()", BuildClipValue(singleton));
	EXPECT_FALSE(RemovePoint(singleton, 0, 0));
}

TEST(visual_tool_vector_clip_shared, freehand_uses_screen_distance_boundary_and_keeps_final_residual) {
	auto path = Path("m 100 100 l 200 100");
	BeginFreehand(path, {0, 0});
	EXPECT_EQ("m 0 0 ", path.EncodeToAss());
	EXPECT_FALSE(UpdateFreehand(path, Mode::Freehand, {29.99f, 0.f}, false));
	EXPECT_EQ(1u, path.size());
	EXPECT_TRUE(UpdateFreehand(path, Mode::Freehand, {18, 24}, false));
	EXPECT_EQ("m 0 0 l 18 24 ", path.EncodeToAss());
	EXPECT_FALSE(UpdateFreehand(path, Mode::Freehand, {18, 24}, true));
	EXPECT_EQ(2u, path.size());
	EXPECT_TRUE(UpdateFreehand(path, Mode::Freehand, {20, 25}, true));
	EXPECT_EQ("m 0 0 l 18 24 20 25 ", path.EncodeToAss());
}

TEST(visual_tool_vector_clip_shared, smooth_freehand_uses_sixty_pixel_spacing_and_literal_curve_controls) {
	Spline path;
	BeginFreehand(path, {0, 0});
	EXPECT_FALSE(UpdateFreehand(path, Mode::FreehandSmooth, {59.99f, 0.f}, false));
	EXPECT_TRUE(UpdateFreehand(path, Mode::FreehandSmooth, {60, 0}, false));
	EXPECT_EQ("m 0 0 l 60 0 ", path.EncodeToAss());
	BeginFreehand(path, {0, 0});
	ASSERT_TRUE(UpdateFreehand(path, Mode::FreehandSmooth, {120, 0}, false));
	ASSERT_TRUE(UpdateFreehand(path, Mode::FreehandSmooth, {120, 120}, false));
	ASSERT_TRUE(UpdateFreehand(path, Mode::FreehandSmooth, {0, 120}, false));
	ASSERT_TRUE(UpdateFreehand(path, Mode::FreehandSmooth, {0, 0}, true));
	ASSERT_EQ(5u, path.size());
	EXPECT_EQ(SplineCurve::POINT, path[0].type);
	EXPECT_EQ(SplineCurve::BICUBIC, path[1].type);
	EXPECT_EQ(Vector2D(60, 0), path[1].p2);
	EXPECT_EQ(Vector2D(90, -30), path[1].p3);
	EXPECT_EQ(Vector2D(120, 0), path[1].p4);
	EXPECT_EQ(Vector2D(-30, 90), path[4].p2);
	EXPECT_EQ(Vector2D(0, 60), path[4].p3);
	EXPECT_EQ(Vector2D(0, 0), path[4].p4);
}

TEST(visual_tool_vector_clip_shared, overlay_batches_subpaths_inverse_shading_and_literal_feature_colours) {
	auto path = Path("m 10 20 l 30 20 30 40 m 100 120 l 130 120");
	auto features = MakeFeatures(path);
	features[0].selected = true;
	features[1].selected = true;
	features[1].active = true;
	for (bool inverse : {false, true}) {
		VectorOverlayRecorder draw;
		DrawOverlay(draw, {.spline = path, .features = features, .inverse = inverse, .video_position = {8, 9}, .video_size = {640, 360}, .line_colour = agi::Color(10, 20, 30), .highlight_primary = agi::Color(40, 50, 60), .highlight_secondary = agi::Color(70, 80, 90), .shaded_alpha = .375f});
		ASSERT_EQ(1u, draw.polygons.size());
		auto const& polygon = draw.polygons[0];
		EXPECT_EQ((std::vector<float>{10, 20, 30, 20, 30, 40, 100, 120, 130, 120}), polygon.points);
		EXPECT_EQ((std::vector<int>{0, 3}), polygon.starts);
		EXPECT_EQ((std::vector<int>{3, 2}), polygon.counts);
		EXPECT_EQ(Vector2D(8, 9), polygon.origin);
		EXPECT_EQ(Vector2D(640, 360), polygon.size);
		EXPECT_EQ(!inverse, polygon.inverted);
		EXPECT_EQ(agi::Color(10, 20, 30), polygon.line.colour);
		EXPECT_FLOAT_EQ(.5f, polygon.line.alpha);
		EXPECT_EQ(2, polygon.line.width);
		EXPECT_EQ(agi::Color(0, 0, 0), polygon.fill.colour);
		EXPECT_FLOAT_EQ(.375f, polygon.fill.alpha);
		ASSERT_EQ(5u, draw.circles.size());
		EXPECT_EQ(agi::Color(70, 80, 90), draw.circles[0].fill.colour);
		EXPECT_EQ(agi::Color(40, 50, 60), draw.circles[1].fill.colour);
		EXPECT_EQ(agi::Color(10, 20, 30), draw.circles[2].fill.colour);
		EXPECT_EQ(Vector2D(10, 20), draw.circles[0].first);
		EXPECT_FLOAT_EQ(3, draw.circles[0].radius);
		EXPECT_FLOAT_EQ(.6f, draw.circles[0].fill.alpha);
		EXPECT_TRUE(draw.rectangles.empty());
		EXPECT_TRUE(draw.dashes.empty());
	}
}

TEST(visual_tool_vector_clip_shared, overlay_highlights_selected_segment_and_draws_insert_marker) {
	auto path = Path("m 10 10 l 90 10 90 90");
	auto features = MakeFeatures(path);
	VectorOverlayRecorder draw;
	DrawOverlay(draw, {.spline = path, .features = features, .mode = Mode::Insert, .video_size = {640, 480}, .pointer = {50, 12}, .pointer_valid = true, .line_colour = agi::Color(10, 20, 30), .highlight_primary = agi::Color(40, 50, 60), .highlight_secondary = agi::Color(70, 80, 90)});
	ASSERT_EQ(1u, draw.strips.size());
	EXPECT_EQ((std::vector<Vector2D>{{10, 10}, {90, 10}}), draw.strips[0]);
	EXPECT_EQ(agi::Color(70, 80, 90), draw.strip_paints[0].colour);
	EXPECT_EQ(2, draw.strip_paints[0].width);
	ASSERT_EQ(4u, draw.circles.size());
	EXPECT_EQ(Vector2D(50, 10), draw.circles.back().first);
	EXPECT_FLOAT_EQ(4, draw.circles.back().radius);
	EXPECT_FLOAT_EQ(0, draw.circles.back().fill.alpha);
}

TEST(visual_tool_vector_clip_shared, overlay_bicubic_controls_box_and_append_preview_keep_literal_dash_endpoints) {
	auto path = Path("m 10 10 b 22 10 34 22 34 34");
	auto features = MakeFeatures(path);
	VectorOverlayRecorder draw;
	DrawOverlay(draw, {.spline = path, .features = features, .video_size = {640, 480}, .line_colour = agi::Color(10, 20, 30)});
	ASSERT_EQ(2u, draw.rectangles.size());
	EXPECT_EQ(Vector2D(19, 7), draw.rectangles[0].first);
	EXPECT_EQ(Vector2D(25, 13), draw.rectangles[0].second);
	EXPECT_EQ(Vector2D(31, 19), draw.rectangles[1].first);
	EXPECT_EQ(Vector2D(37, 25), draw.rectangles[1].second);
	EXPECT_EQ((std::vector<std::pair<Vector2D, Vector2D>>{
				  {{10, 10}, {16, 10}}, {{34, 22}, {34, 28}}}),
			  draw.dashes);

	auto line_path = Path("m 10 10 l 22 10");
	auto line_features = MakeFeatures(line_path);
	VectorOverlayRecorder box;
	DrawOverlay(box, {.spline = line_path, .features = line_features, .mode = Mode::Drag, .video_size = {640, 480}, .pointer = {42, 42}, .pointer_valid = true, .holding = true, .drag_start = {30, 30}});
	EXPECT_EQ((std::vector<std::pair<Vector2D, Vector2D>>{
				  {{30, 30}, {30, 36}}, {{30, 42}, {36, 42}}, {{42, 42}, {42, 36}}, {{42, 30}, {36, 30}}}),
			  box.dashes);
	VectorOverlayRecorder append;
	DrawOverlay(append, {.spline = line_path, .features = line_features, .mode = Mode::Line, .video_size = {640, 480}, .pointer = {34, 10}, .pointer_valid = true});
	EXPECT_EQ((std::vector<std::pair<Vector2D, Vector2D>>{
				  {{34, 10}, {28, 10}}, {{22, 10}, {16, 10}}, {{34, 10}, {28, 10}}}),
			  append.dashes);
}
