#include <main.h>

#include "../../src/visual_tool_scale_policy.h"

#include <cmath>
#include <limits>

namespace {
using visual_tool_scale_policy::Axis;
using visual_tool_scale_policy::NormalizeSelectionTo100;
using visual_tool_scale_policy::NormalizeTo100;

void ExpectRatio(Vector2D value, Vector2D initial) {
	ASSERT_GT(initial.X(), 0.0f);
	ASSERT_GT(initial.Y(), 0.0f);
	EXPECT_NEAR(value.X() * initial.Y(), value.Y() * initial.X(), 1.0e-3f);
}
}

TEST(visual_tool_scale_policy, normalize_x_to_100_preserves_ratio) {
	auto result = NormalizeTo100(Vector2D(150, 120), Axis::X);
	ASSERT_TRUE(result);
	EXPECT_EQ(result->X(), 100.0f);
	EXPECT_EQ(result->Y(), 80.0f);
	ExpectRatio(*result, Vector2D(150, 120));
}

TEST(visual_tool_scale_policy, normalize_y_to_100_preserves_ratio_and_fraction) {
	auto result = NormalizeTo100(Vector2D(133, 77), Axis::Y);
	ASSERT_TRUE(result);
	EXPECT_EQ(result->Y(), 100.0f);
	EXPECT_NEAR(result->X(), 172.72727f, 1.0e-3f);
	ExpectRatio(*result, Vector2D(133, 77));
}

TEST(visual_tool_scale_policy, normalize_is_idempotent_and_allows_zero_dependent_axis) {
	auto same = NormalizeTo100(Vector2D(100, 80), Axis::X);
	ASSERT_TRUE(same);
	EXPECT_EQ(*same, Vector2D(100, 80));

	auto zero = NormalizeTo100(Vector2D(120, 0), Axis::X);
	ASSERT_TRUE(zero);
	EXPECT_EQ(*zero, Vector2D(100, 0));
}

TEST(visual_tool_scale_policy, normalize_rejects_zero_or_negative_fixed_axis) {
	EXPECT_FALSE(NormalizeTo100(Vector2D(0, 80), Axis::X));
	EXPECT_FALSE(NormalizeTo100(Vector2D(100, 0), Axis::Y));
	EXPECT_FALSE(NormalizeTo100(Vector2D(-100, 80), Axis::X));
	EXPECT_FALSE(NormalizeTo100(Vector2D(100, -80), Axis::X));
}

TEST(visual_tool_scale_policy, normalize_rejects_nonfinite_values) {
	float const nan = std::numeric_limits<float>::quiet_NaN();
	float const inf = std::numeric_limits<float>::infinity();
	EXPECT_FALSE(NormalizeTo100(Vector2D(nan, 80.0f), Axis::X));
	EXPECT_FALSE(NormalizeTo100(Vector2D(100.0f, nan), Axis::X));
	EXPECT_FALSE(NormalizeTo100(Vector2D(inf, 80.0f), Axis::X));
	EXPECT_FALSE(NormalizeTo100(Vector2D(100.0f, inf), Axis::Y));
}

TEST(visual_tool_scale_policy, normalized_axis_is_exact_for_non_binary_input) {
	auto x = NormalizeTo100(Vector2D(2.3f, 4.0f), Axis::X);
	auto y = NormalizeTo100(Vector2D(4.0f, 2.3f), Axis::Y);
	ASSERT_TRUE(x);
	ASSERT_TRUE(y);
	EXPECT_EQ(x->X(), 100.0f);
	EXPECT_EQ(y->Y(), 100.0f);
}

TEST(visual_tool_scale_policy, selection_normalizes_each_line_using_its_own_ratio) {
	auto result = NormalizeSelectionTo100(
		{Vector2D(150, 120), Vector2D(200, 100)}, Axis::X);
	ASSERT_TRUE(result);
	ASSERT_EQ(result->size(), 2u);
	EXPECT_EQ((*result)[0], Vector2D(100, 80));
	EXPECT_EQ((*result)[1], Vector2D(100, 50));
}

TEST(visual_tool_scale_policy, selection_normalization_is_all_or_nothing) {
	EXPECT_FALSE(NormalizeSelectionTo100(
		{Vector2D(150, 120), Vector2D(0, 100)}, Axis::X));
	EXPECT_FALSE(NormalizeSelectionTo100({}, Axis::Y));
}

TEST(visual_tool_scale_policy, drag_rejects_undefined_ratio_without_inventing_scale) {
	using visual_tool_scale_policy::ScaleDrag;
	EXPECT_FALSE(ScaleDrag({0, 80}, {20, 0}, false, true, false));
	EXPECT_FALSE(ScaleDrag({100, 0}, {0, -20}, false, true, false));
	EXPECT_FALSE(ScaleDrag({0, 0}, {0, 0}, false, true, false));
	auto flat = ScaleDrag({100, 0}, {20, 0}, false, true, false);
	ASSERT_TRUE(flat);
	EXPECT_EQ(Vector2D(125, 0), *flat);
	auto unconstrained = ScaleDrag({0, 80}, {20, -20}, false, false, false);
	ASSERT_TRUE(unconstrained);
	EXPECT_EQ(Vector2D(25, 105), *unconstrained);
}

TEST(visual_tool_scale_policy, drag_retains_existing_snap_axis_ratio_and_clamp_semantics) {
	using visual_tool_scale_policy::ScaleDrag;
	auto snapped = ScaleDrag({150, 80}, {40, -20}, false, false, true);
	ASSERT_TRUE(snapped);
	EXPECT_EQ(Vector2D(200, 100), *snapped);
	auto constrained = ScaleDrag({150, 80}, {40, -20}, true, false, false);
	ASSERT_TRUE(constrained);
	EXPECT_EQ(Vector2D(200, 80), *constrained);
	auto ratio = ScaleDrag({150, 80}, {40, -20}, false, true, false);
	ASSERT_TRUE(ratio);
	EXPECT_NEAR(200, ratio->X(), 0.0001f);
	EXPECT_NEAR(106.66667f, ratio->Y(), 0.0001f);
	auto clamped = ScaleDrag({150, 80}, {-200, 200}, false, false, false);
	ASSERT_TRUE(clamped);
	EXPECT_EQ(Vector2D(0, 0), *clamped);
}
