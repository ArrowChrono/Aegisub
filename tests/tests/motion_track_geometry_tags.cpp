#include <main.h>

#include "../../src/motion_track/geometry_tags.h"

#include <libaegisub/ass/drawing.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

TEST(visual_geometry_tags, affine_preserves_drawing_commands_and_polarity) {
	using namespace aegisub::motion_track;
	std::string const source = R"({\org(10,20)\iclip(2,m 0 0 l 20 0 20 20 n 4 4 b 6 8 12 8 16 4 s 10 10 12 14 14 10 p 18 8 c)\bord3}Text)";
	PreparedAffineGeometry geometry(source);
	auto result = geometry.Transform({{{1, 0, 3, 0, 1, -2, 0, 0, 1}}}, false);
	ASSERT_TRUE(result.success) << result.error;
	EXPECT_NE(std::string::npos, result.text.find(R"(\org(10,20))"));
	EXPECT_NE(std::string::npos, result.text.find(R"(\iclip(6,m 96 -64 l 416 -64 416 256 n 160 0 b 192 64 288 64 352 0 s 256 96 288 160 320 96 p 384 64 c))"));
	EXPECT_NE(std::string::npos, result.text.find(R"(\bord3}Text)"));
	auto again = geometry.Transform({{{1, 0, 3, 0, 1, -2, 0, 0, 1}}}, false);
	EXPECT_EQ(result.text, again.text);
}

TEST(visual_geometry_tags, rejects_animation_and_rectangle_precedence_changes) {
	using namespace aegisub::motion_track;
	PreparedAffineGeometry animated(R"({\clip(1,2,30,40)\t(0,500,\clip(2,m 0 0 l 4 4))}Text)");
	EXPECT_FALSE(animated.Transform({{{1, 0, 2, 0, 1, 3, 0, 0, 1}}}, false).success);
	PreparedAffineGeometry multiple(R"({\clip(1,2,30,40)\iclip(m 0 0 l 4 4)}Text)");
	EXPECT_FALSE(multiple.Transform({{{0, -1, 0, 1, 0, 0, 0, 0, 1}}}, false).success);
	PreparedAffineGeometry malformed(R"({\clip(m 0 0 l bad)}Text)");
	EXPECT_FALSE(malformed.Transform({{{1, 0, 2, 0, 1, 3, 0, 0, 1}}}, false).success);
}

namespace {
using namespace aegisub::motion_track;
namespace drawing = agi::ass::drawing;
using drawing::PathVerb;
using drawing::Point;

TrackTransform Translation(double x, double y) {
	return {{{1, 0, x, 0, 1, y, 0, 0, 1}}};
}

drawing::PathData OutputClip(std::string const& text) {
	auto const clip = text.find("clip(");
	if (clip == std::string::npos)
		return {};
	auto const comma = text.find(',', clip);
	auto const close = text.find(')', clip);
	if (comma == std::string::npos || close == std::string::npos)
		return {};
	int const scale = std::stoi(text.substr(clip + 5, comma - clip - 5));
	auto path = drawing::ParseAss(text.substr(comma + 1, close - comma - 1),
								  drawing::AssDrawingCompatMode::Libass);
	double const factor = std::ldexp(1.0, 1 - scale);
	return drawing::TransformPath(std::move(path), {.m11 = factor, .m12 = 0, .m21 = 0, .m22 = factor, .dx = 0, .dy = 0});
}

void ExpectPoint(Point point, double x, double y, double tolerance = 0.0005) {
	EXPECT_NEAR(x, point.x, tolerance);
	EXPECT_NEAR(y, point.y, tolerance);
}

double Distance(Point p, Point a, Point b) {
	double const dx = b.x - a.x, dy = b.y - a.y;
	double const squared = dx * dx + dy * dy;
	double const t = squared > 0
						 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / squared, 0.0, 1.0)
						 : 0;
	return std::hypot(p.x - a.x - t * dx, p.y - a.y - t * dy);
}

} // namespace

TEST(motion_track_geometry_tags, preserves_identity_and_unrelated_bytes) {
	std::string const text = R"(literal \org(1,2){comment\bord2\ clip(1,2,30,40)\t(0,500,\org(10,20))}Text{\alpha&H80&})";
	auto const identity = TransformGeometryTags(text, {});
	ASSERT_TRUE(identity.success) << identity.error;
	EXPECT_TRUE(identity.has_geometry);
	EXPECT_EQ(text, identity.text);
	std::string const plain = R"(literal \clip(1,2,3,4){\bord2\t(100,200,\blur3)}Text)";
	auto const translated = TransformGeometryTags(plain, Translation(5, -3));
	ASSERT_TRUE(translated.success) << translated.error;
	EXPECT_FALSE(translated.has_geometry);
	EXPECT_EQ(plain, translated.text);
}

TEST(motion_track_geometry_tags, translates_origins_rectangles_and_inverse_clips) {
	auto const result = TransformGeometryTags(
		R"({note\ orgsuffix(+10,20)\clip(1,2,30,40)\iclip(-10,-20,0,0)\bord2}Text)",
		Translation(5, -3));
	ASSERT_TRUE(result.success) << result.error;
	EXPECT_EQ(R"({note\ orgsuffix(15,17)\clip(6,-1,35,37)\iclip(-5,-23,5,-3)\bord2}Text)", result.text);
}

TEST(motion_track_geometry_tags, converts_rotated_rectangle_to_four_corner_clip) {
	TrackTransform transform{{{0, -1, 50, 1, 0, 3, 0, 0, 1}}};
	auto const result = TransformGeometryTags(R"({\clip(10,20,30,40)}Text)", transform);
	ASSERT_TRUE(result.success) << result.error;
	auto const path = OutputClip(result.text);
	ASSERT_EQ(5u, path.commands.size());
	ExpectPoint(path.commands[0].p1, 30, 13);
	ExpectPoint(path.commands[1].p1, 30, 33);
	ExpectPoint(path.commands[2].p1, 10, 33);
	ExpectPoint(path.commands[3].p1, 10, 13);
	EXPECT_EQ(PathVerb::Close, path.commands.back().verb);
}

TEST(motion_track_geometry_tags, preserves_subpixel_clip_translation) {
	auto const result = TransformGeometryTags(R"({\iclip(0,0,10,20)})", Translation(0.25, 0.5));
	ASSERT_TRUE(result.success) << result.error;
	EXPECT_NE(std::string::npos, result.text.find("\\iclip("));
	auto const path = OutputClip(result.text);
	ASSERT_EQ(5u, path.commands.size());
	ExpectPoint(path.commands[0].p1, 0.25, 0.5);
	ExpectPoint(path.commands[2].p1, 10.25, 20.5);
}

TEST(motion_track_geometry_tags, composes_vector_scale_with_affine_shear_and_nonuniform_scale) {
	TrackTransform transform{{{2, 0.5, 5, 0, 3, -3, 0, 0, 1}}};
	auto const result = TransformGeometryTags(R"({\clip(2,m 0 0 l 20 0 20 20)})", transform);
	ASSERT_TRUE(result.success) << result.error;
	auto const path = OutputClip(result.text);
	ASSERT_EQ(4u, path.commands.size());
	ExpectPoint(path.commands[0].p1, 5, -3);
	ExpectPoint(path.commands[1].p1, 25, -3);
	ExpectPoint(path.commands[2].p1, 30, 27);
}

TEST(motion_track_geometry_tags, keeps_affine_beziers_and_multiple_contours) {
	TrackTransform transform{{{2, 0.5, 5, 0, 3, -3, 0, 0, 1}}};
	auto const result = TransformGeometryTags(
		R"({\clip(m 0 0 b 0 10 10 10 10 0 m 20 0 l 30 0 30 10)})", transform);
	ASSERT_TRUE(result.success) << result.error;
	auto const path = OutputClip(result.text);
	ASSERT_EQ(7u, path.commands.size());
	EXPECT_EQ(PathVerb::CubicTo, path.commands[1].verb);
	ExpectPoint(path.commands[1].p1, 10, 27);
	ExpectPoint(path.commands[1].p2, 30, 27);
	ExpectPoint(path.commands[1].p3, 25, -3);
	EXPECT_EQ(PathVerb::Close, path.commands[2].verb);
	EXPECT_EQ(PathVerb::MoveTo, path.commands[3].verb);
	ExpectPoint(path.commands[3].p1, 45, -3);
}

TEST(motion_track_geometry_tags, lowers_closed_and_extended_bspline_to_equivalent_beziers) {
	auto const result = TransformGeometryTags(
		R"({\clip(m 0 0 s 60 0 60 60 0 60 p -60 60 c)})", Translation(5, -3));
	ASSERT_TRUE(result.success) << result.error;
	auto const path = OutputClip(result.text);
	ASSERT_EQ(7u, path.commands.size());
	ExpectPoint(path.commands[0].p1, 55, 7);
	EXPECT_EQ(PathVerb::CubicTo, path.commands[1].verb);
	ExpectPoint(path.commands[1].p1, 65, 17);
	ExpectPoint(path.commands[1].p2, 65, 37);
	ExpectPoint(path.commands[1].p3, 55, 47);
	EXPECT_EQ(PathVerb::CubicTo, path.commands[5].verb);
	ExpectPoint(path.commands[5].p3, 55, 7);
	EXPECT_EQ(PathVerb::Close, path.commands[6].verb);
}

TEST(motion_track_geometry_tags, bounds_projective_bezier_error_after_serialization) {
	TrackTransform transform{{{1, 0.1, 5, 0.05, 1, -3, 0.006, 0.002, 1}}};
	constexpr double tolerance = 0.05;
	auto const result = TransformGeometryTags(
		R"({\clip(m 0 0 b 0 100 100 100 100 0)})", transform, tolerance);
	ASSERT_TRUE(result.success) << result.error;
	auto const path = OutputClip(result.text);
	ASSERT_GT(path.commands.size(), 4u);
	for (size_t i = 1; i + 1 < path.commands.size(); ++i)
		ASSERT_EQ(PathVerb::LineTo, path.commands[i].verb);
	for (int sample = 0; sample <= 1000; ++sample) {
		double const t = sample / 1000.0;
		double const x = 300 * (1 - t) * t * t + 100 * t * t * t;
		double const y = 300 * (1 - t) * (1 - t) * t + 300 * (1 - t) * t * t;
		double const w = 1 + 0.006 * x + 0.002 * y;
		Point const expected{.x = (x + 0.1 * y + 5) / w, .y = (0.05 * x + y - 3) / w};
		double distance = std::numeric_limits<double>::infinity();
		for (size_t i = 1; i + 1 < path.commands.size(); ++i)
			distance = std::min(distance, Distance(expected, path.commands[i - 1].p1, path.commands[i].p1));
		EXPECT_LE(distance, tolerance) << "sample " << sample;
	}
}

TEST(motion_track_geometry_tags, projects_rectangle_and_origin_with_homogeneous_division) {
	TrackTransform transform{{{1, 0, 0, 0, 1, 0, 0.01, 0, 1}}};
	auto const result = TransformGeometryTags(R"({\org(100,20)\clip(0,0,100,100)})", transform);
	ASSERT_TRUE(result.success) << result.error;
	EXPECT_NE(std::string::npos, result.text.find("\\org(50,10)"));
	auto const path = OutputClip(result.text);
	ASSERT_EQ(5u, path.commands.size());
	ExpectPoint(path.commands[0].p1, 0, 0);
	ExpectPoint(path.commands[1].p1, 50, 0);
	ExpectPoint(path.commands[2].p1, 50, 50);
	ExpectPoint(path.commands[3].p1, 0, 100);
}

TEST(motion_track_geometry_tags, preserves_transform_timing_and_nested_origin) {
	auto const result = TransformGeometryTags(
		R"({\clip(0,0,20,20)\t(10,800,2,\clip(10,20,30,40)\blur3)\t(\org(10,20))}Text)",
		Translation(5, -3));
	ASSERT_TRUE(result.success) << result.error;
	EXPECT_TRUE(result.has_animated_clip);
	EXPECT_EQ(R"({\clip(5,-3,25,17)\t(10,800,2,\clip(15,17,35,37)\blur3)\t(\org(15,17))}Text)", result.text);
}

TEST(motion_track_geometry_tags, rejects_unrepresentable_animated_clip_atomically) {
	std::string const text = R"({\org(10,20)\clip(0,0,20,20)\t(0,800,\clip(10,20,30,40))}Text)";
	TrackTransform transform{{{1, 0.5, 5, 0, 1, 3, 0, 0, 1}}};
	auto const result = TransformGeometryTags(text, transform);
	EXPECT_FALSE(result.success);
	EXPECT_TRUE(result.has_animated_clip);
	EXPECT_NE(std::string::npos, result.error.find("Animated rectangular clip"));
	EXPECT_EQ(text, result.text);
}

TEST(motion_track_geometry_tags, rejects_horizon_crossing_and_malformed_drawing_atomically) {
	std::string const text = R"({\org(0,20)\clip(0,0,100,100)}Text)";
	TrackTransform transform{{{1, 0, 3, 0, 1, 5, -0.02, 0, 1}}};
	auto const horizon = TransformGeometryTags(text, transform);
	EXPECT_FALSE(horizon.success);
	EXPECT_NE(std::string::npos, horizon.error.find("horizon"));
	EXPECT_EQ(text, horizon.text);
	std::string const malformed = R"({\org(10,20)\clip(m 0 0 l 20)}Text)";
	auto const invalid = TransformGeometryTags(malformed, Translation(5, -3));
	EXPECT_FALSE(invalid.success);
	EXPECT_NE(std::string::npos, invalid.error.find("Malformed vector clip"));
	EXPECT_EQ(malformed, invalid.text);
}

TEST(motion_track_geometry_tags, preserves_empty_rectangular_clip_under_rotation) {
	TrackTransform transform{{{0, -1, 50, 1, 0, 3, 0, 0, 1}}};
	auto const result = TransformGeometryTags(R"({\clip(20,20,10,10)\iclip(0,0,0,0)})", transform);
	ASSERT_TRUE(result.success) << result.error;
	EXPECT_EQ(R"({\clip(0,0,0,0)\iclip(0,0,0,0)})", result.text);
}

TEST(motion_track_geometry_tags, rejects_invalid_tolerance_and_unrepresentable_output) {
	std::string const text = R"({\org(10,20)})";
	EXPECT_FALSE(TransformGeometryTags(text, Translation(5, 3), 0).success);
	EXPECT_FALSE(TransformGeometryTags(text, Translation(5, 3), std::numeric_limits<double>::quiet_NaN()).success);
	auto const result = TransformGeometryTags(text, Translation(1e10, 0));
	EXPECT_FALSE(result.success);
	EXPECT_EQ(text, result.text);
	EXPECT_FALSE(TransformGeometryTags(R"({\clip(m 0 0 s 1e100 0 10 10 20 20)})", Translation(5, 3)).success);
}

TEST(motion_track_geometry_tags, rejects_rectangle_conversion_that_changes_clip_precedence) {
	std::string const text = R"({\clip(0,0,50,50)\clip(10,10,20,20)})";
	TrackTransform transform{{{0, -1, 50, 1, 0, 3, 0, 0, 1}}};
	auto const result = TransformGeometryTags(text, transform);
	EXPECT_FALSE(result.success);
	EXPECT_NE(std::string::npos, result.error.find("precedence"));
	EXPECT_EQ(text, result.text);
}

TEST(motion_track_geometry_tags, retains_reversed_animated_rectangle_endpoints) {
	auto const result = TransformGeometryTags(
		R"({\clip(0,0,50,50)\t(0,500,\clip(30,20,10,5))})", Translation(5, -3));
	ASSERT_TRUE(result.success) << result.error;
	EXPECT_EQ(R"({\clip(5,-3,55,47)\t(0,500,\clip(35,17,15,2))})", result.text);
}
