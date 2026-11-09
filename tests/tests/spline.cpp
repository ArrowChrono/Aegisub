#include <main.h>

#include "../../src/spline.h"

#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace {
void ExpectCurveEqual(SplineCurve const& expected, SplineCurve const& actual) {
	EXPECT_EQ(expected.type, actual.type);
	EXPECT_EQ(expected.p1, actual.p1);
	EXPECT_EQ(expected.p2, actual.p2);
	EXPECT_EQ(expected.p3, actual.p3);
	EXPECT_EQ(expected.p4, actual.p4);
}
}

TEST(spline, closest_parametric_point_uses_closing_segment_without_mutating_curves) {
	std::vector<SplineCurve> curves {
		SplineCurve(Vector2D(0, 0)),
		SplineCurve(Vector2D(0, 0), Vector2D(10, 0)),
		SplineCurve(Vector2D(10, 0), Vector2D(10, 10)),
	};
	auto const original = curves;
	auto const *storage = curves.data();
	auto const capacity = curves.capacity();

	std::size_t curve_index = 0;
	float t = 0.f;
	Vector2D point;
	ASSERT_TRUE(spline_detail::FindClosestParametricPoint(
		curves, Vector2D(5, 5), curve_index, t, point));

	EXPECT_EQ(curves.size(), curve_index);
	EXPECT_FLOAT_EQ(0.5f, t);
	EXPECT_EQ(Vector2D(5, 5), point);
	EXPECT_EQ(storage, curves.data());
	EXPECT_EQ(capacity, curves.capacity());
	ASSERT_EQ(original.size(), curves.size());
	for (std::size_t index = 0; index < curves.size(); ++index)
		ExpectCurveEqual(original[index], curves[index]);
}

TEST(spline, closest_parametric_point_prefers_existing_curve_on_closing_segment_tie) {
	std::vector<SplineCurve> curves {
		SplineCurve(Vector2D(0, 0), Vector2D(10, 0)),
	};

	std::size_t curve_index = 0;
	float t = 0.f;
	Vector2D point;
	ASSERT_TRUE(spline_detail::FindClosestParametricPoint(
		curves, Vector2D(5, 0), curve_index, t, point));

	EXPECT_EQ(0u, curve_index);
	EXPECT_FLOAT_EQ(0.5f, t);
	EXPECT_EQ(Vector2D(5, 0), point);
}

TEST(spline, closest_parametric_point_uses_bicubic_endpoint_with_nonfinite_controls) {
	float const nan = std::numeric_limits<float>::quiet_NaN();
	std::vector<SplineCurve> curves {
		SplineCurve(Vector2D(0, 0)),
		SplineCurve(
			Vector2D(0, 0),
			Vector2D(nan, nan),
			Vector2D(nan, nan),
			Vector2D(10, 10)),
	};

	std::size_t curve_index = 0;
	float t = 0.f;
	Vector2D point;
	ASSERT_TRUE(spline_detail::FindClosestParametricPoint(
		curves, Vector2D(5, 5), curve_index, t, point));

	EXPECT_EQ(curves.size(), curve_index);
	EXPECT_FLOAT_EQ(0.5f, t);
	EXPECT_EQ(Vector2D(5, 5), point);
}

TEST(spline, closest_parametric_point_rejects_empty_curve_set) {
	std::vector<SplineCurve> curves;
	std::size_t curve_index = 42;
	float t = 0.25f;
	Vector2D point(3, 4);

	EXPECT_FALSE(spline_detail::FindClosestParametricPoint(
		curves, Vector2D(5, 5), curve_index, t, point));
	EXPECT_EQ(0u, curve_index);
	EXPECT_FLOAT_EQ(0.25f, t);
	EXPECT_EQ(Vector2D(3, 4), point);
}

TEST(spline, collapsed_bicubic_tessellation_preserves_finite_endpoints) {
	SplineCurve const curve(Vector2D(12, -7), Vector2D(12, -7),
							Vector2D(12, -7), Vector2D(12, -7));
	std::vector<float> points{91, -23};

	EXPECT_EQ(2, curve.GetPoints(points));

	ASSERT_EQ(6u, points.size());
	EXPECT_EQ(91, points[0]);
	EXPECT_EQ(-23, points[1]);
	for (std::size_t index = 2; index < points.size(); ++index)
		EXPECT_TRUE(std::isfinite(points[index]));
	EXPECT_EQ(12, points[2]);
	EXPECT_EQ(-7, points[3]);
	EXPECT_EQ(12, points[4]);
	EXPECT_EQ(-7, points[5]);
}

TEST(spline, short_bicubic_tessellation_preserves_finite_distinct_endpoints) {
	SplineCurve const curve(Vector2D(-2, 3), Vector2D(-1, 3),
							Vector2D(0, 4), Vector2D(1, 4));
	std::vector<float> points;

	EXPECT_EQ(2, curve.GetPoints(points));

	ASSERT_EQ(4u, points.size());
	for (float coordinate : points)
		EXPECT_TRUE(std::isfinite(coordinate));
	EXPECT_EQ(-2, points[0]);
	EXPECT_EQ(3, points[1]);
	EXPECT_EQ(1, points[2]);
	EXPECT_EQ(4, points[3]);
}

TEST(spline, bicubic_tessellation_retains_density_across_eight_pixel_boundary) {
	struct Case {
		char const *name;
		float length;
		int expected_points;
	};
	std::array const cases{
		Case{.name = "below", .length = std::nextafter(8.f, 0.f), .expected_points = 2},
		Case{.name = "at", .length = 8.f, .expected_points = 2},
		Case{.name = "above", .length = std::nextafter(8.f, 16.f), .expected_points = 2},
		Case{.name = "regular", .length = 16.f, .expected_points = 3},
	};
	for (auto const& test : cases) {
		SCOPED_TRACE(test.name);
		SplineCurve const curve(Vector2D(0, 0), Vector2D(0, 0),
								Vector2D(test.length, 0.f), Vector2D(test.length, 0.f));
		std::vector<float> points;

		EXPECT_EQ(test.expected_points, curve.GetPoints(points));

		ASSERT_EQ(static_cast<std::size_t>(test.expected_points * 2), points.size());
		for (float coordinate : points)
			EXPECT_TRUE(std::isfinite(coordinate));
		EXPECT_EQ(0, points[0]);
		EXPECT_EQ(0, points[1]);
		EXPECT_EQ(test.length, points[points.size() - 2]);
		EXPECT_EQ(0, points.back());
		if (test.expected_points == 3) {
			EXPECT_EQ(8, points[2]);
			EXPECT_EQ(0, points[3]);
		}
	}
}

TEST(spline, closest_parametric_point_closes_each_subpath_independently) {
	std::vector<SplineCurve> curves{
		SplineCurve(Vector2D(0, 0)),
		SplineCurve(Vector2D(0, 0), Vector2D(20, 0)),
		SplineCurve(Vector2D(20, 0), Vector2D(20, 20)),
		SplineCurve(Vector2D(100, 100)),
		SplineCurve(Vector2D(100, 100), Vector2D(120, 100)),
		SplineCurve(Vector2D(120, 100), Vector2D(120, 120)),
	};
	auto const original = curves;
	std::array const references{Vector2D(10, 10), Vector2D(110, 110)};
	for (auto const reference : references) {
		SCOPED_TRACE(reference.X());
		std::size_t curve_index = 0;
		float t = 0;
		Vector2D point;

		ASSERT_TRUE(spline_detail::FindClosestParametricPoint(curves,
															  reference, curve_index, t, point));

		EXPECT_EQ(curves.size(), curve_index);
		EXPECT_FLOAT_EQ(0.5f, t);
		EXPECT_EQ(reference, point);
	}
	ASSERT_EQ(original.size(), curves.size());
	for (std::size_t index = 0; index < curves.size(); ++index)
		ExpectCurveEqual(original[index], curves[index]);
}

TEST(spline, closest_parametric_point_never_connects_distinct_subpaths) {
	std::vector<SplineCurve> curves{
		SplineCurve(Vector2D(0, 0)),
		SplineCurve(Vector2D(0, 0), Vector2D(20, 0)),
		SplineCurve(Vector2D(20, 0), Vector2D(20, 20)),
		SplineCurve(Vector2D(100, 100)),
		SplineCurve(Vector2D(100, 100), Vector2D(120, 100)),
		SplineCurve(Vector2D(120, 100), Vector2D(120, 120)),
	};
	std::size_t curve_index = 0;
	float t = 0;
	Vector2D point;

	ASSERT_TRUE(spline_detail::FindClosestParametricPoint(curves,
														  Vector2D(55, 55), curve_index, t, point));

	EXPECT_EQ(2u, curve_index);
	EXPECT_FLOAT_EQ(1.f, t);
	EXPECT_EQ(Vector2D(20, 20), point);
}

TEST(spline, closest_parametric_point_reports_and_clears_closing_identity) {
	std::vector<SplineCurve> curves{
		SplineCurve(Vector2D(0, 0)),
		SplineCurve(Vector2D(0, 0), Vector2D(20, 0)),
		SplineCurve(Vector2D(20, 0), Vector2D(20, 20)),
		SplineCurve(Vector2D(100, 100)),
		SplineCurve(Vector2D(100, 100), Vector2D(120, 100)),
		SplineCurve(Vector2D(120, 100), Vector2D(120, 120)),
	};
	std::size_t curve_index = 0;
	float t = 0;
	Vector2D point;
	std::optional<spline_detail::ClosingSegment> closing;
	ASSERT_TRUE(spline_detail::FindClosestParametricPoint(curves,
														  Vector2D(110, 110), curve_index, t, point, &closing));
	ASSERT_TRUE(closing);
	EXPECT_EQ(3u, closing->first);
	EXPECT_EQ(5u, closing->last);
	EXPECT_EQ(curves.size(), curve_index);
	EXPECT_FLOAT_EQ(0.5f, t);
	EXPECT_EQ(Vector2D(110, 110), point);

	ASSERT_TRUE(spline_detail::FindClosestParametricPoint(curves,
														  Vector2D(10, 10), curve_index, t, point, &closing));
	ASSERT_TRUE(closing);
	EXPECT_EQ(0u, closing->first);
	EXPECT_EQ(2u, closing->last);

	ASSERT_TRUE(spline_detail::FindClosestParametricPoint(curves,
														  Vector2D(10, 0), curve_index, t, point, &closing));
	EXPECT_FALSE(closing);
	EXPECT_EQ(1u, curve_index);
	EXPECT_EQ(Vector2D(10, 0), point);

	closing = spline_detail::ClosingSegment{.first = 3, .last = 5};
	EXPECT_FALSE(spline_detail::FindClosestParametricPoint({},
														   Vector2D(10, 0), curve_index, t, point, &closing));
	EXPECT_FALSE(closing);
}

TEST(spline, scale_factor_has_defined_legacy_32_bit_wrap) {
	struct Case {
		int raw_scale;
		int expected_factor;
	};
	std::array const cases{
		Case{.raw_scale = 1, .expected_factor = 1},
		Case{.raw_scale = 31, .expected_factor = 1073741824},
		Case{.raw_scale = 32, .expected_factor = std::numeric_limits<int>::min()},
		Case{.raw_scale = 33, .expected_factor = 1},
		Case{.raw_scale = 0, .expected_factor = std::numeric_limits<int>::min()},
		Case{.raw_scale = std::numeric_limits<int>::min(), .expected_factor = std::numeric_limits<int>::min()},
		Case{.raw_scale = std::numeric_limits<int>::max(), .expected_factor = 1073741824},
	};
	for (auto const& test : cases) {
		SCOPED_TRACE(test.raw_scale);
		EXPECT_EQ(test.expected_factor, spline_detail::GetScaleFactor(test.raw_scale));
	}
}
