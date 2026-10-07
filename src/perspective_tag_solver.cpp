// Copyright (c) 2022, arch1t3cht <arch1t3cht@gmail.com>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

// Inverse Perspective decomposition adapted from arch1t3cht/Aegisub commits
// f44883844f3179fb2f52b937854b1e6f2e014bee,
// 438aa6882b41f5488e31f3f4af31b324507e900b, and
// 947af58ad98e36dee47c6fbb88d89ae271cfa517.

#include "perspective_tag_solver.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace perspective {
namespace {

constexpr int kPositionDecimals = 4;
constexpr int kScaleDecimals = 4;
constexpr int kShearDecimals = 6;
constexpr int kRotationDecimals = 5;

constexpr double Pi = std::numbers::pi;
constexpr double RadiansToDegrees = 180.0 / Pi;
constexpr double DegreesToRadians = Pi / 180.0;
// [0] pos.x, [1] pos.y, [2] ln(scale_x/100), [3] ln(scale_y/100),
// [4] shear_x, [5] shear_y, [6] rotation_z (radians),
// [7] rotation_x (radians), [8] rotation_y (radians).
// Every optimizer family freezes one or more of these slots and leaves the
// rest free, so a family is described by which degrees of freedom it locks,
// not by a different parameterization. CornerResidualCount stays 8: the quad
// still constrains the fit with the same eight samples.
constexpr std::size_t ParameterCount = 9;
constexpr std::size_t CornerResidualCount = 8;

struct AffineMap {
	double a00 = 1.0;
	double a01 = 0.0;
	double a10 = 0.0;
	double a11 = 1.0;
	double b0 = 0.0;
	double b1 = 0.0;
};

// Which slots an implicit-origin fit may move. Each lock freezes exactly one
// slot at the seed's value, so a family keeps the shape of "one shear axis" or
// "locked rotation" without hard-coding zero: a seed that already carries the
// line's \fay or its locked \frz makes the frozen slot preserve that value
// through the fit.
struct ImplicitModel {
	bool preserve_scale = false;
	bool preserve_scale_x = false;
	bool preserve_scale_y = false;
	// Freezes frx/fry at the seed's values, reducing the fit to the in-plane
	// parameters. With a zeroed seed this is an affine fit driven by the same
	// optimizer as the projective families, which is how a scale-pinned affine
	// re-fit is obtained without a second closed-form derivation.
	bool preserve_plane = false;
	// Freezes shear_x (slot 4) at the seed's value. Used by the fay-flavoured
	// families, whose seeds carry shear_x = 0.
	bool preserve_shear_x = false;
	// Freezes shear_y (slot 5) at the seed's value. The fax families seed
	// shear_y = 0; the fay-preserving refits seed the source's own shear_y so
	// an existing \fay survives the fit untouched.
	bool preserve_shear_y = false;
	// Freezes rotation_z (slot 6) at the seed's value, or at the semantic
	// locked_rotation_z when one is set. Used by the double-shear family and
	// by plane refits generated under a rotation lock.
	bool preserve_rotation_z = false;
	// Keeps the source's explicit \org instead of folding it away. Paired with
	// preserve_plane this re-fits inside the plane the line already declares.
	bool preserve_origin = false;
	std::optional<double> locked_rotation_z;
};

struct Vec3 {
	double x = 0.0;
	double y = 0.0;
	double z = 0.0;

	Vec3 operator+(Vec3 other) const { return {x + other.x, y + other.y, z + other.z}; }
	Vec3 operator-(Vec3 other) const { return {x - other.x, y - other.y, z - other.z}; }
	Vec3 operator*(double factor) const { return {x * factor, y * factor, z * factor}; }
	double Length() const { return std::sqrt(x * x + y * y + z * z); }
	Vec3 Cross(Vec3 other) const {
		return {y * other.z - z * other.y,
			z * other.x - x * other.z,
			x * other.y - y * other.x};
	}
};

Vec3 RotateX(Vec3 value, double radians) {
	double const cosine = std::cos(radians);
	double const sine = std::sin(radians);
	return {value.x, cosine * value.y - sine * value.z,
		sine * value.y + cosine * value.z};
}

Vec3 RotateY(Vec3 value, double radians) {
	double const cosine = std::cos(radians);
	double const sine = std::sin(radians);
	return {cosine * value.x - sine * value.z, value.y,
		sine * value.x + cosine * value.z};
}

Vec3 RotateZ(Vec3 value, double radians) {
	double const cosine = std::cos(radians);
	double const sine = std::sin(radians);
	return {cosine * value.x - sine * value.y,
		sine * value.x + cosine * value.y, value.z};
}

Vec2 RotateZ(Vec2 value, double radians) {
	double const cosine = std::cos(radians);
	double const sine = std::sin(radians);
	return {cosine * value.x - sine * value.y,
		sine * value.x + cosine * value.y};
}

bool IsFinite(Vec3 value) {
	return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool NearlyEqual(double left, double right) {
	double const scale = std::max({1.0, std::abs(left), std::abs(right)});
	return std::abs(left - right) <= 1.0e-9 * scale;
}

bool NearlyEqual(Vec2 left, Vec2 right) {
	return NearlyEqual(left.x, right.x) && NearlyEqual(left.y, right.y);
}

bool EquivalentRotation(double left, double right) {
	return std::abs(std::remainder(left - right, 360.0)) <= 1.0e-9;
}

int FieldDecimals(int requested, int field_max, bool best_fit = false) {
	return best_fit ? ClampPerspectiveDecimalPlaces(requested)
					: std::min(ClampPerspectiveDecimalPlaces(requested), field_max);
}

double Quantize(double value, int decimals) {
	static constexpr std::array<double, 10> factors {
		1.0, 10.0, 100.0, 1000.0, 10000.0,
		100000.0, 1000000.0, 10000000.0, 100000000.0, 1000000000.0,
	};
	decimals = std::clamp(decimals, 0, static_cast<int>(factors.size() - 1));
	double const result = std::round(value * factors[decimals]) / factors[decimals];
	return result == 0.0 ? 0.0 : result;
}

void AppendRectSamples(std::vector<Vec2>& samples, Rect bounds, bool include_center) {
	double const center_x = (bounds.left + bounds.right) / 2.0;
	double const center_y = (bounds.top + bounds.bottom) / 2.0;
	std::array<Vec2, 8> const perimeter {{
		{bounds.left, bounds.top},
		{bounds.right, bounds.top},
		{bounds.right, bounds.bottom},
		{bounds.left, bounds.bottom},
		{center_x, bounds.top},
		{bounds.right, center_y},
		{center_x, bounds.bottom},
		{bounds.left, center_y},
	}};
	samples.insert(samples.end(), perimeter.begin(), perimeter.end());
	if (include_center)
		samples.push_back({center_x, center_y});
}

std::vector<Vec2> ResidualSamples(
	BaseBounds const& bounds,
	EvaluatedTransformState const& source) {
	std::vector<Vec2> samples;
	samples.reserve(25 + bounds.residual_samples.size());
	AppendRectSamples(samples, bounds.rectangle, true);
	double const outline_x = std::abs(source.outline_x);
	double const outline_y = std::abs(source.outline_y);
	Rect const outlined {
		bounds.rectangle.left - outline_x,
		bounds.rectangle.top - outline_y,
		bounds.rectangle.right + outline_x,
		bounds.rectangle.bottom + outline_y,
	};
	if (outline_x > 0.0 || outline_y > 0.0)
		AppendRectSamples(samples, outlined, false);
	if (source.shadow_x != 0.0 || source.shadow_y != 0.0) {
		Rect const shadowed {
			outlined.left + source.shadow_x,
			outlined.top + source.shadow_y,
			outlined.right + source.shadow_x,
			outlined.bottom + source.shadow_y,
		};
		AppendRectSamples(samples, shadowed, false);
	}
	samples.insert(
		samples.end(), bounds.residual_samples.begin(), bounds.residual_samples.end());
	return samples;
}

class ResidualReference {
	std::span<Vec2 const> samples;
	OutputCoordinateMapping mapping;
	std::vector<Vec2> projected;
	std::optional<Matrix3> matrix;

	public:
	ResidualReference(std::span<Vec2 const> samples, OutputCoordinateMapping mapping)
		: samples(samples), mapping(mapping), projected(samples.size()) {
	}

	bool Reset(Homography const& target) {
		if (matrix && matrix->Values() == target.Matrix().Values())
			return true;
		matrix.reset();
		double minimum = std::numeric_limits<double>::infinity();
		double maximum = -std::numeric_limits<double>::infinity();
		for (std::size_t index = 0; index < samples.size(); ++index) {
			double const denominator = target.Denominator(samples[index]);
			minimum = std::min(minimum, denominator);
			maximum = std::max(maximum, denominator);
			auto const point = target.Map(samples[index]);
			if (!point)
				return false;
			projected[index] = *point;
		}
		if (!ValidDenominators(minimum, maximum))
			return false;
		matrix = target.Matrix();
		return true;
	}

	// Over-budget trials still validate every projection, but need no further
	// distances. Final errors keep the full scan and subnormal-scale precision.
	[[nodiscard]] std::optional<double> Measure(
		ForwardResult const& candidate,
		double stop_after = std::numeric_limits<double>::infinity()) const {
		double maximum_squared = 0.0;
		double subnormal_maximum = 0.0;
		bool exceeds_budget = false;
		double minimum = std::numeric_limits<double>::infinity();
		double maximum = -std::numeric_limits<double>::infinity();
		Vec2 worst;
		double const stop_squared = stop_after * stop_after;
		for (std::size_t index = 0; index < samples.size(); ++index) {
			double const denominator = candidate.transform.Denominator(samples[index]);
			minimum = std::min(minimum, denominator);
			maximum = std::max(maximum, denominator);
			auto const actual = candidate.transform.Map(samples[index]);
			if (!actual)
				return std::nullopt;
			if (exceeds_budget)
				continue;
			double const dx = (actual->x - projected[index].x) * mapping.scale_x;
			double const dy = (actual->y - projected[index].y) * mapping.scale_y;
			double const squared = dx * dx + dy * dy;
			if (!std::isfinite(squared))
				return std::nullopt;
			if (squared < std::numeric_limits<double>::min() && (dx != 0.0 || dy != 0.0))
				subnormal_maximum = std::max(subnormal_maximum, std::hypot(dx, dy));
			if (squared > maximum_squared) {
				maximum_squared = squared;
				worst = {.x = dx, .y = dy};
			}
			exceeds_budget = maximum_squared > stop_squared
				&& std::hypot(worst.x, worst.y) > stop_after;
		}
		if (!ValidDenominators(minimum, maximum))
			return std::nullopt;
		return std::max(subnormal_maximum, std::hypot(worst.x, worst.y));
	}

	// Centre the error range over the same domain used for final acceptance.
	// This is a minimax step for each coordinate; callers retain it only when
	// it also reduces the full Euclidean maximum.
	[[nodiscard]] std::optional<Vec2> CenteringOffset(ForwardResult const& candidate) const {
		Vec2 minimum{.x = std::numeric_limits<double>::infinity(), .y = std::numeric_limits<double>::infinity()};
		Vec2 maximum{.x = -std::numeric_limits<double>::infinity(), .y = -std::numeric_limits<double>::infinity()};
		for (std::size_t index = 0; index < samples.size(); ++index) {
			auto const actual = candidate.transform.Map(samples[index]);
			if (!actual)
				return std::nullopt;
			Vec2 const error = *actual - projected[index];
			minimum.x = std::min(minimum.x, error.x);
			minimum.y = std::min(minimum.y, error.y);
			maximum.x = std::max(maximum.x, error.x);
			maximum.y = std::max(maximum.y, error.y);
		}
		return (minimum + maximum) * -0.5;
	}

	private:
	static bool ValidDenominators(double minimum, double maximum) {
		// The denominator is linear. A consistent sign at every sample proves
		// that their convex hull (including border/shadow/control geometry)
		// cannot cross the projection horizon between finite sample points.
		double const epsilon = 128.0 * std::numeric_limits<double>::epsilon() * std::max(std::abs(minimum), std::abs(maximum));
		return std::isfinite(minimum) && std::isfinite(maximum) && (minimum > epsilon || maximum < -epsilon);
	}
};

bool IsValidOutputMapping(OutputCoordinateMapping mapping) {
	return std::isfinite(mapping.scale_x)
		&& std::isfinite(mapping.scale_y)
		&& mapping.scale_x > 0.0 && mapping.scale_y > 0.0
		&& mapping.scale_x <= MaxAbsCoordinate
		&& mapping.scale_y <= MaxAbsCoordinate;
}

bool HasValidResidualSamples(BaseBounds const& bounds) {
	return std::all_of(
		bounds.residual_samples.begin(), bounds.residual_samples.end(),
		[](Vec2 point) {
			return std::isfinite(point.x) && std::isfinite(point.y)
				&& std::abs(point.x) <= MaxAbsCoordinate
				&& std::abs(point.y) <= MaxAbsCoordinate;
		});
}

EvaluatedTransformState CanonicalizeCurrentGeometry(EvaluatedTransformState state) {
	state.position = {};
	state.origin.reset();
	state.scale_x = 100.0;
	state.scale_y = 100.0;
	state.shear_x = 0.0;
	state.shear_y = 0.0;
	state.rotation_x = 0.0;
	state.rotation_y = 0.0;
	state.rotation_z = 0.0;
	return state;
}

std::optional<double> StateResidual(
	SolverInput const& input,
	ResidualReference const& reference,
	EvaluatedTransformState const& state,
	double stop_after = std::numeric_limits<double>::infinity()) {
	auto const forward = ForwardQuad(input.source, state);
	if (!forward)
		return std::nullopt;
	return reference.Measure(forward, stop_after);
}

SerializedTransformState Serialize(
	EvaluatedTransformState const& state,
	int maximum_decimals, bool best_fit = false) {
	SerializedTransformState serialized;
	serialized.position = FormatAssPoint(
		state.position, FieldDecimals(maximum_decimals, kPositionDecimals, best_fit));
	if (state.origin)
		serialized.origin = FormatAssPoint(
			*state.origin, FieldDecimals(maximum_decimals, kPositionDecimals, best_fit));
	serialized.scale_x = FormatAssNumber(
		state.scale_x, FieldDecimals(maximum_decimals, kScaleDecimals, best_fit));
	serialized.scale_y = FormatAssNumber(
		state.scale_y, FieldDecimals(maximum_decimals, kScaleDecimals, best_fit));
	serialized.shear_x = FormatAssNumber(
		state.shear_x, FieldDecimals(maximum_decimals, kShearDecimals, best_fit));
	serialized.shear_y = FormatAssNumber(
		state.shear_y, FieldDecimals(maximum_decimals, kShearDecimals, best_fit));
	serialized.rotation_x = FormatAssNumber(
		state.rotation_x, FieldDecimals(maximum_decimals, kRotationDecimals, best_fit));
	serialized.rotation_y = FormatAssNumber(
		state.rotation_y, FieldDecimals(maximum_decimals, kRotationDecimals, best_fit));
	serialized.rotation_z = FormatAssNumber(
		state.rotation_z, FieldDecimals(maximum_decimals, kRotationDecimals, best_fit));
	return serialized;
}

std::size_t TokenSize(std::string_view tag, std::string const& value) {
	return tag.size() + value.size();
}

CandidateScore ScoreCandidate(
	EvaluatedTransformState const& source,
	EvaluatedTransformState const& candidate,
	SerializedTransformState const& serialized,
	CandidateFamily family,
	double residual) {
	CandidateScore score;
	// PreserveFitState is the same decomposition the Fit option just produced,
	// with only the scale fields pinned. Its value is filled in below from the
	// shape-only residual; a negative value lets an otherwise homothetic result
	// win over a different tag decomposition without hiding a real shape error.
	score.fit_consistency_penalty = family == CandidateFamily::PreserveFitState ? -1 : 0;
	score.family_rank = static_cast<int>(family);
	score.residual = residual;
	if (!NearlyEqual(source.position, candidate.position)) {
		++score.changed_tag_count;
		score.token_count += TokenSize("\\pos", serialized.position);
	}
	bool const origin_changed = source.origin.has_value() != candidate.origin.has_value()
		|| (source.origin && candidate.origin && !NearlyEqual(*source.origin, *candidate.origin));
	if (origin_changed) {
		++score.changed_tag_count;
		if (serialized.origin)
			score.token_count += TokenSize("\\org", *serialized.origin);
	}
	if (candidate.origin)
		++score.explicit_origin_penalty;

	auto const scalar = [&](double old_value, double new_value, std::string_view tag,
		std::string const& text) {
		if (!NearlyEqual(old_value, new_value)) {
			++score.changed_tag_count;
			score.token_count += TokenSize(tag, text);
		}
	};
	scalar(source.scale_x, candidate.scale_x, "\\fscx", serialized.scale_x);
	scalar(source.scale_y, candidate.scale_y, "\\fscy", serialized.scale_y);
	scalar(source.shear_x, candidate.shear_x, "\\fax", serialized.shear_x);
	scalar(source.shear_y, candidate.shear_y, "\\fay", serialized.shear_y);
	scalar(source.rotation_x, candidate.rotation_x, "\\frx", serialized.rotation_x);
	scalar(source.rotation_y, candidate.rotation_y, "\\fry", serialized.rotation_y);
	scalar(source.rotation_z, candidate.rotation_z, "\\frz", serialized.rotation_z);
	score.perspective_penalty = (std::abs(candidate.rotation_x) > 1.0e-9 ? 1 : 0)
		+ (std::abs(candidate.rotation_y) > 1.0e-9 ? 1 : 0);
	score.non_fax_shear_penalty = std::abs(candidate.shear_y) > 1.0e-9 ? 1 : 0;
	score.condition_penalty =
		std::abs(std::log(candidate.scale_x / 100.0))
		+ std::abs(std::log(candidate.scale_y / 100.0))
		+ std::abs(candidate.shear_x) + std::abs(candidate.shear_y)
		+ (std::abs(candidate.rotation_x) + std::abs(candidate.rotation_y)
			+ std::abs(candidate.rotation_z)) / 180.0;
	score.emitted_tag_count = score.changed_tag_count;
	return score;
}

bool BetterScore(CandidateScore const& left, CandidateScore const& right) {
	int const left_no_op_penalty = left.changed_tag_count == 0 ? 0 : 1;
	int const right_no_op_penalty = right.changed_tag_count == 0 ? 0 : 1;
	// Fit continuity is compared first because a homothetic Preserve result is
	// the same plane with only its scale fields pinned. Its shape-only bucket
	// is positive when that continuity is not geometrically credible. Snapped
	// and snap_bucket then outrank the no-op preference on purpose; without
	// that, a drag the restricted subset cannot express would let the unchanged
	// source win on tag economy and the tool would look broken.
	return std::tie(
			   left.fit_consistency_penalty,
			   left.snapped,
			   left.snap_bucket,
			   left_no_op_penalty,
			   left.explicit_origin_penalty,
			   left.perspective_penalty,
			   left.non_fax_shear_penalty,
			   left.emitted_tag_count,
			   left.token_count,
			   left.condition_penalty,
			   left.family_rank,
			   left.residual) < std::tie(right.fit_consistency_penalty,
										 right.snapped,
										 right.snap_bucket,
										 right_no_op_penalty,
										 right.explicit_origin_penalty,
										 right.perspective_penalty,
										 right.non_fax_shear_penalty,
										 right.emitted_tag_count,
										 right.token_count,
										 right.condition_penalty,
										 right.family_rank,
										 right.residual);
}

std::pair<std::size_t, std::size_t> EdgeAnchorEndpoints(PerspectiveEdgeAnchor anchor);
double HeldEdgeError(Quad const& landed, Quad const& drawn,
					 std::pair<std::size_t, std::size_t> held, OutputCoordinateMapping mapping);

bool HonorsEdgeAnchor(SolverInput const& input, ForwardResult const& landed) {
	return input.edge_anchor == PerspectiveEdgeAnchor::None || HeldEdgeError(landed.quad, input.target,
																			 EdgeAnchorEndpoints(input.edge_anchor), input.output_mapping) <= input.max_error;
}

struct QuantizationField {
	double EvaluatedTransformState::*scalar = nullptr;
	int component = 0;
	int decimals = 0;

	double& Value(EvaluatedTransformState& state) const {
		if (scalar)
			return state.*scalar;
		switch (component) {
			case 0: return state.position.x;
			case 1: return state.position.y;
			case 2: return state.origin->x;
			default: return state.origin->y;
		}
	}
	[[nodiscard]] double Value(EvaluatedTransformState const& state) const {
		if (scalar)
			return state.*scalar;
		switch (component) {
			case 0: return state.position.x;
			case 1: return state.position.y;
			case 2: return state.origin->x;
			default: return state.origin->y;
		}
	}
};

std::vector<QuantizationField> QuantizationFields(
	SolverInput const& input, EvaluatedTransformState const& raw) {
	int const decimals = ClampPerspectiveDecimalPlaces(input.maximum_decimals);
	int const position = FieldDecimals(decimals, kPositionDecimals, input.best_fit);
	bool const restricted = input.representation_policy == PerspectiveRepresentationPolicy::FaxFrzOnly;
	bool const multiline = input.source.bounds.multiline_text;
	std::vector<QuantizationField> fields;
	fields.reserve(11);
	fields.push_back({.scalar = nullptr, .component = 0, .decimals = position});
	fields.push_back({.scalar = nullptr, .component = 1, .decimals = position});
	if (raw.origin && !restricted) {
		fields.push_back({.scalar = nullptr, .component = 2, .decimals = position});
		fields.push_back({.scalar = nullptr, .component = 3, .decimals = position});
	}
	auto const add = [&](double EvaluatedTransformState::*field, int cap) {
		// Inherited style values need no digits in the output. Rounding them
		// would add a tag and discard the constrained refit's intended saving.
		if (!input.best_fit && input.measure_tag_cost && input.event_style && NearlyEqual(raw.*field, input.event_style.value().*field))
			return;
		fields.push_back({.scalar = field, .component = 0, .decimals = FieldDecimals(decimals, cap, input.best_fit)});
	};
	if (input.scale_policy == PerspectiveScalePolicy::Fit) {
		add(&EvaluatedTransformState::scale_x, kScaleDecimals);
		add(&EvaluatedTransformState::scale_y, kScaleDecimals);
	}
	if (!multiline)
		add(&EvaluatedTransformState::shear_x, kShearDecimals);
	if (!restricted && !multiline)
		add(&EvaluatedTransformState::shear_y, kShearDecimals);
	if (!restricted) {
		add(&EvaluatedTransformState::rotation_x, kRotationDecimals);
		add(&EvaluatedTransformState::rotation_y, kRotationDecimals);
	}
	if (!input.locked_rotation_z)
		add(&EvaluatedTransformState::rotation_z, kRotationDecimals);
	return fields;
}

// Search one-step single and paired moves around maximum-precision rounding.
// Position and angle/scale errors can cancel at an adjacent decimal grid point.
void RepairQuantization(
	SolverInput const& input, ResidualReference const& reference,
	std::span<QuantizationField const> fields, EvaluatedTransformState const& raw,
	EvaluatedTransformState& state,
	std::optional<double>& residual) {
	auto const initial = state;
	std::optional<PerspectiveTagCost> best_cost;
	auto const consider = [&](EvaluatedTransformState const& trial) {
		auto const landed = ForwardQuad(input.source, trial);
		if (!landed)
			return false;
		auto const error = reference.Measure(landed);
		if (input.best_fit) {
			if (error && (!residual || *error < *residual)) {
				state = trial;
				residual = error;
			}
			return false;
		}
		bool const feasible = error && *error <= input.max_error && HonorsEdgeAnchor(input, landed);
		if (feasible && !input.measure_tag_cost) {
			// The initial rounding may have less model error but miss the held
			// edge. Any fully feasible neighbor must be allowed to replace it.
			state = trial;
			residual = error;
			return true;
		}
		if (feasible && input.measure_tag_cost) {
			auto const cost = input.measure_tag_cost(trial,
													 Serialize(trial, input.maximum_decimals), input.scale_policy);
			if (cost && (!best_cost || std::tie(cost->tag_count, cost->byte_count) < std::tie(best_cost->tag_count, best_cost->byte_count) || (std::tie(cost->tag_count, cost->byte_count) == std::tie(best_cost->tag_count, best_cost->byte_count) && *error < *residual))) {
				state = trial;
				residual = error;
				best_cost = cost;
			}
			return false;
		}
		if (best_cost)
			return false;
		if (error && (!residual || *error < *residual)) {
			state = trial;
			residual = error;
		}
		return false;
	};
	for (std::size_t first = 0; first < fields.size(); ++first) {
		if (fields[first].Value(initial) == fields[first].Value(raw))
			continue;
		double const step = std::pow(10.0, -fields[first].decimals);
		for (int direction : {-1, 1}) {
			auto trial = initial;
			fields[first].Value(trial) += direction * step;
			if (consider(trial))
				return;
			for (std::size_t second = first + 1; second < fields.size(); ++second) {
				if (fields[second].Value(initial) == fields[second].Value(raw))
					continue;
				double const other_step = std::pow(10.0, -fields[second].decimals);
				for (int other_direction : {-1, 1}) {
					auto pair = trial;
					fields[second].Value(pair) += other_direction * other_step;
					if (consider(pair))
						return;
				}
			}
		}
	}
}

void CompactQuantization(
	SolverInput const& input, ResidualReference const& reference,
	ResidualReference const& target, std::span<QuantizationField const> fields,
	EvaluatedTransformState const& raw, EvaluatedTransformState& state) {
	struct Proposal {
		std::size_t field;
		double value;
		std::size_t saved;
		double error;
	};
	std::vector<Proposal> proposals;
	proposals.reserve(fields.size() * kMaxPerspectiveDecimalPlaces);
	for (std::size_t index = 0; index < fields.size(); ++index) {
		auto const& field = fields[index];
		auto const original_size = FormatAssNumber(field.Value(state), field.decimals).size();
		std::optional<double> previous_value;
		for (int decimals = 0; decimals < field.decimals; ++decimals) {
			double const value = Quantize(field.Value(raw), decimals);
			// Successive decimal caps can produce the same value and spelling.
			// Retain one trial so duplicates do not consume the pair shortlist.
			if (previous_value && *previous_value == value)
				continue;
			previous_value = value;
			auto const size = FormatAssNumber(value, decimals).size();
			if (size >= original_size)
				continue;
			auto trial = state;
			field.Value(trial) = value;
			auto const error = StateResidual(input, reference, trial);
			if (error)
				proposals.push_back({.field = index, .value = value, .saved = original_size - size, .error = *error});
		}
	}
	if (proposals.empty())
		return;
	std::optional<PerspectiveTagCost> current_cost;
	if (input.measure_tag_cost) {
		current_cost = input.measure_tag_cost(state,
											  Serialize(state, input.maximum_decimals), input.scale_policy);
		if (!current_cost)
			return;
	}
	std::ranges::stable_sort(proposals, [](auto const& left, auto const& right) {
		return left.saved != right.saved ? left.saved > right.saved : left.error < right.error;
	});
	auto const initial_total = StateResidual(input, target, state);
	bool const in_band = initial_total && *initial_total <= input.shape_tolerance;
	auto const acceptable = [&](EvaluatedTransformState const& trial) {
		auto const landed = ForwardQuad(input.source, trial);
		if (!landed || !HonorsEdgeAnchor(input, landed))
			return false;
		auto const error = reference.Measure(landed, input.max_error);
		if (!error || *error > input.max_error)
			return false;
		if (in_band) {
			auto const total = target.Measure(landed, input.shape_tolerance);
			if (!total || *total > input.shape_tolerance)
				return false;
		}
		if (current_cost) {
			auto const cost = input.measure_tag_cost(trial,
													 Serialize(trial, input.maximum_decimals), input.scale_policy);
			if (!cost || std::tie(cost->tag_count, cost->byte_count) > std::tie(current_cost->tag_count, current_cost->byte_count))
				return false;
			current_cost = cost;
		}
		return true;
	};
	auto const shorter = [&](Proposal const& proposal) {
		auto const& field = fields[proposal.field];
		return FormatAssNumber(proposal.value, field.decimals).size() < FormatAssNumber(field.Value(state), field.decimals).size();
	};
	std::vector<Proposal> rejected;
	for (auto const& proposal : proposals) {
		if (!shorter(proposal))
			continue;
		auto trial = state;
		fields[proposal.field].Value(trial) = proposal.value;
		if (acceptable(trial))
			state = trial;
		else if (rejected.size() < 12)
			rejected.push_back(proposal);
	}
	// A bounded shortlist allows two fields to become shorter together even
	// when neither independent rounding meets the geometric budget.
	for (std::size_t first = 0; first < rejected.size(); ++first) {
		for (std::size_t second = first + 1; second < rejected.size(); ++second) {
			auto const& left = rejected[first];
			auto const& right = rejected[second];
			if (left.field == right.field || !shorter(left) || !shorter(right))
				continue;
			auto trial = state;
			fields[left.field].Value(trial) = left.value;
			fields[right.field].Value(trial) = right.value;
			if (acceptable(trial))
				state = trial;
		}
	}
}

int SnapBucket(double snap_error, double unit) {
	if (!(unit > 0.0) || !std::isfinite(snap_error) || snap_error <= 0.0)
		return 0;
	double const bucket = std::ceil(snap_error / unit);
	if (!std::isfinite(bucket))
		return std::numeric_limits<int>::max();
	return static_cast<int>(
		std::min<double>(bucket, std::numeric_limits<int>::max()));
}

// A pinned scale is allowed to change the overall size, but it should not
// change the plane selected by Fit. Measure the landed quad after removing the
// best uniform scale about the drawn quad's centre; translation remains part
// of the error because position is one of the tags that should be preserved.
double HomotheticShapeError(
	Quad const& target,
	Quad const& landed,
	OutputCoordinateMapping mapping) {
	Vec2 target_center;
	for (auto const& point : target)
		target_center = target_center + point / static_cast<double>(target.size());
	double numerator = 0.0;
	double denominator = 0.0;
	for (std::size_t index = 0; index < target.size(); ++index) {
		Vec2 const target_vector = target[index] - target_center;
		Vec2 const landed_vector = landed[index] - target_center;
		numerator += target_vector.Dot(landed_vector);
		denominator += target_vector.SquareLength();
	}
	if (!(denominator > 0.0) || !std::isfinite(numerator)
		|| !std::isfinite(denominator))
		return std::numeric_limits<double>::infinity();
	double const factor = numerator / denominator;
	if (!(factor > 0.0) || !std::isfinite(factor))
		return std::numeric_limits<double>::infinity();
	double maximum = 0.0;
	for (std::size_t index = 0; index < target.size(); ++index) {
		Vec2 const expected = target_center
			+ (target[index] - target_center) * factor;
		Vec2 const delta = landed[index] - expected;
		double const error = std::hypot(
			delta.x * mapping.scale_x, delta.y * mapping.scale_y);
		if (!std::isfinite(error))
			return std::numeric_limits<double>::infinity();
		maximum = std::max(maximum, error);
	}
	return maximum;
}

// Where a quantization attempt stopped. The distinction is what the UI
// reports: ModelRejected means the candidate's model could not be projected
// or measured at all. QuantizationRejected means the model is fine but the
// emitted digits blew the rounding budget, so decimal places is the lever.
enum class QuantizeStage {
	Succeeded,
	ModelRejected,
	QuantizationRejected,
};

struct QuantizeOutcome {
	std::optional<SolverCandidate> candidate;
	QuantizeStage stage = QuantizeStage::ModelRejected;
	// Rounding error the quantization-stage rejection left behind, in output
	// pixels, plus the serialization budget it was judged against, so a total
	// failure can quote both instead of refusing generically. Unmeasurable
	// rejections (projection-domain deaths) stay numberless.
	std::optional<double> rejected_error;
	double rejected_budget = 0.0;
};

QuantizeOutcome StageRejection(QuantizeStage stage) {
	QuantizeOutcome outcome;
	outcome.stage = stage;
	return outcome;
}

// Records what a digits-stage rejection left behind: how far the written
// digits land from the model they must reproduce, and the rounding budget
// they blew -- the number raising the Decimal Places option shrinks.
QuantizeOutcome RejectedQuantization(double rounding_error, double budget) {
	QuantizeOutcome outcome;
	outcome.stage = QuantizeStage::QuantizationRejected;
	outcome.rejected_error = rounding_error;
	outcome.rejected_budget = budget;
	return outcome;
}

QuantizeOutcome QuantizeCandidate(
	SolverInput const& input,
	ResidualReference const& target,
	CandidateFamily family,
	EvaluatedTransformState const& raw,
	ResidualReference& model_reference) {
	auto const forward = ForwardQuad(input.source, raw);
	if (!forward)
		return StageRejection(QuantizeStage::ModelRejected);
	auto const snap_error = target.Measure(forward);
	if (!snap_error)
		return StageRejection(QuantizeStage::ModelRejected);
	// Doing nothing is only an answer when nothing was needed. NoOp skips
	// digit rounding entirely, so without this gate it outlives every real
	// family whenever coarse decimals kill them, and "wins" with the line
	// left exactly where it was, however far that is from the target.
	if (family == CandidateFamily::NoOp && *snap_error > input.max_error)
		return StageRejection(QuantizeStage::ModelRejected);
	// Numeric precision always measures against the unrounded model. Model
	// fitting and serialization must not silently change reference frames.
	if (!model_reference.Reset(forward.transform))
		return StageRejection(QuantizeStage::ModelRejected);
	auto const& reference = model_reference;
	auto state = raw;
	int const maximum_decimals = ClampPerspectiveDecimalPlaces(input.maximum_decimals);
	auto const fields = QuantizationFields(input, raw);
	if (family != CandidateFamily::NoOp) {
		for (auto const& field : fields)
			field.Value(state) = Quantize(field.Value(raw), field.decimals);
		if (input.scale_policy == PerspectiveScalePolicy::Preserve) {
			state.scale_x = input.source.state.scale_x;
			state.scale_y = input.source.state.scale_y;
		}
	}
	auto const rounded = ForwardQuad(input.source, state);
	auto const& rounding_reference = input.best_fit ? target : reference;
	auto residual = rounded ? rounding_reference.Measure(rounded) : std::nullopt;
	if (family != CandidateFamily::NoOp && (input.best_fit || !residual || *residual > input.max_error || !HonorsEdgeAnchor(input, rounded)))
		RepairQuantization(input, rounding_reference, fields, raw, state, residual);
	if (!residual)
		return StageRejection(QuantizeStage::QuantizationRejected);
	if (!input.best_fit && *residual > input.max_error)
		return RejectedQuantization(*residual, input.max_error);

	if (!input.best_fit && family != CandidateFamily::NoOp && family != CandidateFamily::PreserveShapeAffine) {
		CompactQuantization(input, reference, target, fields, raw, state);
	}
	if (family != CandidateFamily::NoOp) {
		// Adjacent-grid additions may carry binary rounding beyond the final
		// decimal spelling. Measure the exact values Serialize will write.
		for (auto const& field : fields)
			field.Value(state) = Quantize(field.Value(state), field.decimals);
	}

	auto const final_forward = ForwardQuad(input.source, state);
	if (!final_forward)
		return StageRejection(QuantizeStage::QuantizationRejected);
	auto const quantization_error = reference.Measure(final_forward);
	if (!quantization_error)
		return StageRejection(QuantizeStage::QuantizationRejected);
	if (!input.best_fit && *quantization_error > input.max_error)
		return RejectedQuantization(*quantization_error, input.max_error);
	// Total is measured against the drawn quad, so it stays comparable with the
	// value the staged verification recomputes after the tags are written.
	auto const total_error = target.Measure(final_forward);
	if (!total_error)
		return StageRejection(QuantizeStage::QuantizationRejected);
	auto serialized = Serialize(state, maximum_decimals, input.best_fit);
	auto score = ScoreCandidate(
		input.source.state, state, serialized, family, *total_error);
	if (input.measure_tag_cost) {
		auto const cost = input.measure_tag_cost(state, serialized, input.scale_policy);
		if (!cost)
			return StageRejection(QuantizeStage::ModelRejected);
		score.emitted_tag_count = cost->tag_count;
		score.token_count = cost->byte_count;
	}
	bool const snapped = *total_error > input.shape_tolerance;
	score.snapped = snapped ? 1 : 0;
	score.snap_bucket = snapped
							? SnapBucket(*snap_error, input.shape_tolerance)
							: 0;
	if (family == CandidateFamily::PreserveFitState) {
		double const shape_error = HomotheticShapeError(
			input.target, forward.quad, input.output_mapping);
		score.fit_consistency_penalty = shape_error <= input.shape_tolerance
											? -1
											: SnapBucket(shape_error, input.shape_tolerance);
	}
	SolverCandidate candidate{
		.family = family,
		.state = state,
		.serialized = serialized,
		.score = score,
		.max_error = *total_error,
		.snap_error = *snap_error,
		.quantization_error = *quantization_error,
	};
	return {std::move(candidate), QuantizeStage::Succeeded};
}

bool Solve2x2(
	double a11, double a12, double a21, double a22,
	double b1, double b2, double& x1, double& x2) {
	double const determinant = a11 * a22 - a12 * a21;
	double const scale = std::abs(a11 * a22) + std::abs(a12 * a21);
	if (!std::isfinite(determinant) || scale == 0.0
		|| std::abs(determinant) <= 64.0 * std::numeric_limits<double>::epsilon() * scale)
		return false;
	x1 = (b1 * a22 - a12 * b2) / determinant;
	x2 = (a11 * b2 - b1 * a21) / determinant;
	return std::isfinite(x1) && std::isfinite(x2);
}

std::optional<AffineMap> LocalAffine(Homography const& homography, Vec2 center) {
	auto const& matrix = homography.Matrix();
	double const denominator = homography.Denominator(center);
	if (!std::isfinite(denominator) || denominator == 0.0)
		return std::nullopt;
	double const numerator_x = matrix(0, 0) * center.x + matrix(0, 1) * center.y + matrix(0, 2);
	double const numerator_y = matrix(1, 0) * center.x + matrix(1, 1) * center.y + matrix(1, 2);
	double const denominator_squared = denominator * denominator;
	AffineMap result;
	result.a00 = (matrix(0, 0) * denominator - numerator_x * matrix(2, 0)) / denominator_squared;
	result.a01 = (matrix(0, 1) * denominator - numerator_x * matrix(2, 1)) / denominator_squared;
	result.a10 = (matrix(1, 0) * denominator - numerator_y * matrix(2, 0)) / denominator_squared;
	result.a11 = (matrix(1, 1) * denominator - numerator_y * matrix(2, 1)) / denominator_squared;
	auto const mapped = homography.Map(center);
	if (!mapped)
		return std::nullopt;
	result.b0 = mapped->x - result.a00 * center.x - result.a01 * center.y;
	result.b1 = mapped->y - result.a10 * center.x - result.a11 * center.y;
	for (double const value : {result.a00, result.a01, result.a10, result.a11, result.b0, result.b1}) {
		if (!std::isfinite(value))
			return std::nullopt;
	}
	return result;
}

std::optional<AffineMap> FitAffine(
	Quad const& target, Rect bounds, PerspectiveEdgeAnchor anchor) {
	double const width = bounds.Width();
	double const height = bounds.Height();
	if (!std::isfinite(width) || !std::isfinite(height)
		|| width <= 0.0 || height <= 0.0)
		return std::nullopt;

	// Orthogonal least-squares projection of the four target corners onto the
	// six-degree-of-freedom affine subspace. QuantizeCandidate later decides
	// whether this simpler representation fits the output error budget.
	Vec2 horizontal =
		(target[1] + target[2] - target[0] - target[3]) / (2.0 * width);
	Vec2 vertical =
		(target[3] + target[2] - target[0] - target[1]) / (2.0 * height);
	Vec2 const source_center {
		(bounds.left + bounds.right) / 2.0,
		(bounds.top + bounds.bottom) / 2.0,
	};
	Vec2 target_center =
		(target[0] + target[1] + target[2] + target[3]) / 4.0;

	// An anchored edge is not one of two equal measurements to be averaged: it
	// is the measurement. It fixes its own basis vector outright, and the
	// opposite edge then contributes only its midpoint, which is where averaging
	// genuinely belongs -- neither of that edge's corners was trusted.
	if (anchor != PerspectiveEdgeAnchor::None) {
		bool const horizontal_anchor = anchor == PerspectiveEdgeAnchor::Top
			|| anchor == PerspectiveEdgeAnchor::Bottom;
		// Endpoints of the anchored edge, then of the edge opposite it.
		std::size_t held_from = 0;
		std::size_t held_to = 1;
		std::size_t free_from = 3;
		std::size_t free_to = 2;
		switch (anchor) {
			case PerspectiveEdgeAnchor::Top: break;
			case PerspectiveEdgeAnchor::Bottom:
				held_from = 3; held_to = 2; free_from = 0; free_to = 1;
				break;
			case PerspectiveEdgeAnchor::Left:
				held_from = 0; held_to = 3; free_from = 1; free_to = 2;
				break;
			case PerspectiveEdgeAnchor::Right:
				held_from = 1; held_to = 2; free_from = 0; free_to = 3;
				break;
			case PerspectiveEdgeAnchor::None: break;
		}
		double const held_span = horizontal_anchor ? width : height;
		Vec2 const held = (target[held_to] - target[held_from]) / held_span;
		Vec2 const held_mid = (target[held_from] + target[held_to]) / 2.0;
		Vec2 const free_mid = (target[free_from] + target[free_to]) / 2.0;
		// Signed so that Bottom and Right anchors push the offset the other way.
		double const cross_span = horizontal_anchor ? height : width;
		bool const held_is_first = anchor == PerspectiveEdgeAnchor::Top
			|| anchor == PerspectiveEdgeAnchor::Left;
		Vec2 const cross = (held_is_first
			? free_mid - held_mid
			: held_mid - free_mid) / cross_span;
		if (horizontal_anchor) {
			horizontal = held;
			vertical = cross;
		}
		else {
			vertical = held;
			horizontal = cross;
		}
		// Place the mapped rectangle so the anchored edge lands on the drawn
		// edge exactly. Its midpoint is enough: the basis vector above already
		// fixed the edge's direction and length.
		Vec2 const anchored_source_mid = horizontal_anchor
			? Vec2 {source_center.x,
				held_is_first ? bounds.top : bounds.bottom}
			: Vec2 {held_is_first ? bounds.left : bounds.right,
				source_center.y};
		target_center = held_mid
			+ horizontal * (source_center.x - anchored_source_mid.x)
			+ vertical * (source_center.y - anchored_source_mid.y);
	}

	AffineMap const result {
		horizontal.x,
		vertical.x,
		horizontal.y,
		vertical.y,
		target_center.x - horizontal.x * source_center.x
			- vertical.x * source_center.y,
		target_center.y - horizontal.y * source_center.x
			- vertical.y * source_center.y,
	};
	for (double const value : {
		result.a00, result.a01, result.a10,
		result.a11, result.b0, result.b1}) {
		if (!std::isfinite(value))
			return std::nullopt;
	}
	if (std::abs(result.a00 * result.a11 - result.a01 * result.a10)
		<= 1.0e-14)
		return std::nullopt;
	return result;
}

// Closed-form decompositions use Euclidean lengths and angles. A script X
// coordinate is not that metric when LayoutRes and PlayRes have
// different aspect ratios: solve D*A*D^-1 in layout units, then restore only
// position/origin to script units. Scale, shear and rotation remain ASS tags.
ForwardInput InLayoutMetric(ForwardInput source, double aspect) {
	source.play_resolution.width *= aspect;
	source.bounds.rectangle.left *= aspect;
	source.bounds.rectangle.right *= aspect;
	if (source.bounds.alignment_extent)
		source.bounds.alignment_extent->width *= aspect;
	source.bounds.alignment_offset.x *= aspect;
	for (auto& point : source.bounds.residual_samples)
		point.x *= aspect;
	source.state.position.x *= aspect;
	if (source.state.origin)
		source.state.origin->x *= aspect;
	return source;
}

Quad ScaleQuadX(Quad quad, double factor) {
	for (auto& point : quad)
		point.x *= factor;
	return quad;
}

AffineMap InLayoutMetric(AffineMap map, double aspect) {
	map.a01 *= aspect;
	map.a10 /= aspect;
	map.b0 *= aspect;
	return map;
}

std::optional<EvaluatedTransformState> FromLayoutMetric(
	std::optional<EvaluatedTransformState> state, double aspect) {
	if (state) {
		state->position.x /= aspect;
		if (state->origin)
			state->origin->x /= aspect;
	}
	return state;
}

std::optional<EvaluatedTransformState> DecomposeAffine(
	AffineMap const& map,
	ForwardInput const& source,
	bool use_fay,
	double layout_aspect = 1.0) {
	if (layout_aspect != 1.0)
		return FromLayoutMetric(DecomposeAffine(
									InLayoutMetric(map, layout_aspect),
									InLayoutMetric(source, layout_aspect), use_fay),
								layout_aspect);
	auto state = source.state;
	state.origin.reset();
	state.rotation_x = 0.0;
	state.rotation_y = 0.0;
	state.shear_x = 0.0;
	state.shear_y = 0.0;
	double theta = 0.0;
	double scale_x = 0.0;
	double scale_y = 0.0;
	if (!use_fay) {
		scale_x = std::hypot(map.a00, map.a10);
		if (scale_x == 0.0)
			return std::nullopt;
		double const cosine = map.a00 / scale_x;
		double const sine = map.a10 / scale_x;
		theta = std::atan2(sine, cosine);
		scale_y = -sine * map.a01 + cosine * map.a11;
		if (scale_y <= 0.0)
			return std::nullopt;
		state.shear_x = (cosine * map.a01 + sine * map.a11) / scale_x;
	} else {
		scale_y = std::hypot(map.a01, map.a11);
		if (scale_y == 0.0)
			return std::nullopt;
		double const sine = -map.a01 / scale_y;
		double const cosine = map.a11 / scale_y;
		theta = std::atan2(sine, cosine);
		scale_x = cosine * map.a00 + sine * map.a10;
		if (scale_x <= 0.0)
			return std::nullopt;
		state.shear_y = (-sine * map.a00 + cosine * map.a10) / scale_y;
	}

	state.scale_x = scale_x * 100.0;
	state.scale_y = scale_y * 100.0;
	state.rotation_z = -theta * RadiansToDegrees;
	double const cosine = std::cos(theta);
	double const sine = std::sin(theta);
	Vec2 const shift = ResolveBoundsAlignmentShift(source.bounds, state.alignment);
	Vec2 const rotated_scaled_shift {
		cosine * scale_x * shift.x - sine * scale_y * shift.y,
		sine * scale_x * shift.x + cosine * scale_y * shift.y,
	};
	state.position = {map.b0 - rotated_scaled_shift.x, map.b1 - rotated_scaled_shift.y};
	return state;
}

std::optional<EvaluatedTransformState> DecomposeLockedAffine(
	AffineMap const& map,
	ForwardInput const& source,
	double locked_rotation_z,
	double layout_aspect = 1.0) {
	if (layout_aspect != 1.0)
		return FromLayoutMetric(DecomposeLockedAffine(
									InLayoutMetric(map, layout_aspect),
									InLayoutMetric(source, layout_aspect), locked_rotation_z),
								layout_aspect);
	// Repository-local fixed-angle decomposition. Holding frz constant frees
	// its optimizer slot for the second shear without over-parameterizing the
	// eight degrees of freedom of a quad homography.
	if (!std::isfinite(locked_rotation_z))
		return std::nullopt;
	double const rotation = locked_rotation_z * DegreesToRadians;
	Vec2 const basis_x = RotateZ(Vec2 {map.a00, map.a10}, rotation);
	Vec2 const basis_y = RotateZ(Vec2 {map.a01, map.a11}, rotation);
	if (!std::isfinite(basis_x.x) || !std::isfinite(basis_x.y)
		|| !std::isfinite(basis_y.x) || !std::isfinite(basis_y.y)
		|| basis_x.x <= 1.0e-12 || basis_y.y <= 1.0e-12)
		return std::nullopt;

	auto state = source.state;
	state.origin.reset();
	state.scale_x = basis_x.x * 100.0;
	state.scale_y = basis_y.y * 100.0;
	state.shear_x = basis_y.x / basis_x.x;
	state.shear_y = basis_x.y / basis_y.y;
	state.rotation_x = 0.0;
	state.rotation_y = 0.0;
	state.rotation_z = locked_rotation_z;
	double const shear_determinant = 1.0 - state.shear_x * state.shear_y;
	if (!std::isfinite(state.scale_x) || !std::isfinite(state.scale_y)
		|| !std::isfinite(state.shear_x) || !std::isfinite(state.shear_y)
		|| !std::isfinite(shear_determinant) || shear_determinant <= 0.0)
		return std::nullopt;

	Vec2 const shift = ResolveBoundsAlignmentShift(source.bounds, state.alignment);
	Vec2 const rotated_shift = RotateZ(
		Vec2 {shift.x * basis_x.x, shift.y * basis_y.y}, -rotation);
	state.position = {map.b0 - rotated_shift.x, map.b1 - rotated_shift.y};
	if (!std::isfinite(state.position.x) || !std::isfinite(state.position.y))
		return std::nullopt;
	return state;
}

std::optional<EvaluatedTransformState> TranslationCandidate(
	AffineMap const& map,
	ForwardInput const& source,
	double layout_aspect = 1.0) {
	if (layout_aspect != 1.0)
		return FromLayoutMetric(TranslationCandidate(
									InLayoutMetric(map, layout_aspect),
									InLayoutMetric(source, layout_aspect)),
								layout_aspect);
	double const tolerance = 1.0e-10;
	if (std::abs(map.a00 - 1.0) > tolerance || std::abs(map.a11 - 1.0) > tolerance
		|| std::abs(map.a01) > tolerance || std::abs(map.a10) > tolerance)
		return std::nullopt;
	auto state = source.state;
	state.origin.reset();
	state.scale_x = 100.0;
	state.scale_y = 100.0;
	state.shear_x = 0.0;
	state.shear_y = 0.0;
	state.rotation_x = 0.0;
	state.rotation_y = 0.0;
	state.rotation_z = 0.0;
	Vec2 const shift = ResolveBoundsAlignmentShift(source.bounds, state.alignment);
	state.position = {map.b0 - shift.x, map.b1 - shift.y};
	return state;
}

std::optional<EvaluatedTransformState> SimilarityCandidate(
	AffineMap const& map,
	ForwardInput const& source,
	double layout_aspect = 1.0) {
	if (layout_aspect != 1.0)
		return FromLayoutMetric(SimilarityCandidate(
									InLayoutMetric(map, layout_aspect),
									InLayoutMetric(source, layout_aspect)),
								layout_aspect);
	auto candidate = DecomposeAffine(map, source, false);
	if (!candidate)
		return std::nullopt;
	double const scale = (candidate->scale_x + candidate->scale_y) / 2.0;
	double const tolerance = 1.0e-9 * std::max(100.0, std::abs(scale));
	if (std::abs(candidate->scale_x - candidate->scale_y) > tolerance
		|| std::abs(candidate->shear_x) > 1.0e-9)
		return std::nullopt;
	candidate->scale_x = scale;
	candidate->scale_y = scale;
	candidate->shear_x = 0.0;
	double const theta = -candidate->rotation_z * DegreesToRadians;
	double const factor = scale / 100.0;
	Vec2 const shift = ResolveBoundsAlignmentShift(source.bounds, candidate->alignment);
	Vec2 const rotated_shift {
		factor * (std::cos(theta) * shift.x - std::sin(theta) * shift.y),
		factor * (std::sin(theta) * shift.x + std::cos(theta) * shift.y),
	};
	candidate->position = {map.b0 - rotated_shift.x, map.b1 - rotated_shift.y};
	return candidate;
}

bool IsParallelogram(Quad const& target) {
	Vec2 const diagonal_error = target[0] + target[2] - target[1] - target[3];
	double const scale = std::max({1.0,
								   (target[1] - target[0]).SquareLength(),
								   (target[2] - target[1]).SquareLength(),
								   (target[3] - target[2]).SquareLength(),
								   (target[0] - target[3]).SquareLength()});
	return diagonal_error.SquareLength() <= 1.0e-12 * scale;
}

bool IsRectangle(Quad const& target) {
	if (!IsParallelogram(target))
		return false;
	Vec2 const horizontal = target[1] - target[0];
	Vec2 const vertical = target[3] - target[0];
	double const scale = std::max(1.0,
								  horizontal.SquareLength() * vertical.SquareLength());
	return std::abs(horizontal.Dot(vertical)) <= 1.0e-12 * scale;
}

Quad ScaleQuadAboutCenter(Quad const& quad, double factor);

struct PreserveShapeAffineResult {
	EvaluatedTransformState state;
	Quad effective_target;
	// Exact affine homotheties can safely suppress the generic projective
	// families. A rectangle with a different intrinsic aspect ratio cannot be
	// represented at a pinned scale; its fallback remains a scored approximation.
	bool exact = true;
};

std::optional<PreserveShapeAffineResult> PreserveShapeAffineCandidate(
	SolverInput const& input,
	Quad const& target,
	std::optional<double> locked_rotation_z,
	double layout_aspect = 1.0) {
	if (layout_aspect != 1.0) {
		auto normalized = input;
		normalized.source = InLayoutMetric(input.source, layout_aspect);
		normalized.output_mapping.scale_x /= layout_aspect;
		auto result = PreserveShapeAffineCandidate(normalized,
												   ScaleQuadX(target, layout_aspect), locked_rotation_z);
		if (result) {
			result->state = *FromLayoutMetric(result->state, layout_aspect);
			result->effective_target = ScaleQuadX(result->effective_target, 1.0 / layout_aspect);
		}
		return result;
	}
	if (!IsParallelogram(target))
		return std::nullopt;
	double const width = input.source.bounds.rectangle.Width();
	double const height = input.source.bounds.rectangle.Height();
	double const scale_x = input.source.state.scale_x / 100.0;
	double const scale_y = input.source.state.scale_y / 100.0;
	if (!(width > 0.0) || !(height > 0.0) || !(scale_x > 0.0) || !(scale_y > 0.0) || !std::isfinite(width) || !std::isfinite(height) || !std::isfinite(scale_x) || !std::isfinite(scale_y))
		return std::nullopt;

	Vec2 const target_u = (target[1] - target[0]) / width;
	Vec2 const target_v = (target[3] - target[0]) / height;
	// Undoing the renderer's clockwise Z rotation gives
	//
	//   R(r) * [target_u target_v] * factor
	//       = [[scale_x, scale_x * fax],
	//          [scale_y * fay, scale_y]].
	//
	// The two fixed diagonal entries determine r.  Solving that pair directly
	// matters for rectangles whose aspect ratio differs from the source: both
	// shear axes are then needed, and forcing them into an opposite-shear pair
	// silently changes the shape.
	double cosine = 0.0;
	double sine = 0.0;
	if (locked_rotation_z) {
		if (!std::isfinite(*locked_rotation_z))
			return std::nullopt;
		double const rotation = *locked_rotation_z * DegreesToRadians;
		cosine = std::cos(rotation);
		sine = std::sin(rotation);
	}
	else {
		double const sine_coefficient =
			scale_x * target_v.x + scale_y * target_u.y;
		double const cosine_coefficient =
			scale_x * target_v.y - scale_y * target_u.x;
		double const length =
			std::hypot(sine_coefficient, cosine_coefficient);
		if (!(length > 1.0e-12) || !std::isfinite(length))
			return std::nullopt;
		cosine = sine_coefficient / length;
		sine = -cosine_coefficient / length;
	}

	double const diagonal_x = cosine * target_u.x - sine * target_u.y;
	double const diagonal_y = sine * target_v.x + cosine * target_v.y;
	if (!(diagonal_x > 1.0e-12) || !(diagonal_y > 1.0e-12))
		return std::nullopt;
	double const factor_x = scale_x / diagonal_x;
	double const factor_y = scale_y / diagonal_y;
	if (!std::isfinite(factor_x) || !std::isfinite(factor_y) || factor_x <= 0.0 || factor_y <= 0.0 || std::abs(factor_x - factor_y) > 1.0e-8 * std::max({1.0, factor_x, factor_y}))
		return std::nullopt;
	double const factor = (factor_x + factor_y) / 2.0;
	double const shear_x = factor * (cosine * target_v.x - sine * target_v.y) / scale_x;
	double const shear_y = factor * (sine * target_u.x + cosine * target_u.y) / scale_y;
	if (!std::isfinite(factor) || !std::isfinite(shear_x) || !std::isfinite(shear_y) || 1.0 - shear_x * shear_y <= 1.0e-12)
		return std::nullopt;

	Quad const effective_target = ScaleQuadAboutCenter(target, factor);
	auto state = input.source.state;
	state.origin.reset();
	state.scale_x = input.source.state.scale_x;
	state.scale_y = input.source.state.scale_y;
	state.shear_x = shear_x;
	state.shear_y = shear_y;
	state.rotation_x = 0.0;
	state.rotation_y = 0.0;
	state.rotation_z = std::atan2(sine, cosine) * RadiansToDegrees;
	Vec2 const shift = ResolveBoundsAlignmentShift(
		input.source.bounds, state.alignment);
	Vec2 const first{
		(input.source.bounds.rectangle.left + input.source.bounds.rectangle.top * shear_x + shift.x) * scale_x,
		(input.source.bounds.rectangle.left * shear_y + input.source.bounds.rectangle.top + shift.y) * scale_y};
	state.position = effective_target[0] - RotateZ(
											   first, -state.rotation_z * DegreesToRadians);
	if (!std::isfinite(state.position.x) || !std::isfinite(state.position.y))
		return std::nullopt;
	return PreserveShapeAffineResult{state, effective_target, true};
}

std::optional<PreserveShapeAffineResult> PreserveRectangleFallback(
	SolverInput const& input,
	Quad const& target,
	std::optional<double> locked_rotation_z,
	double layout_aspect = 1.0) {
	if (layout_aspect != 1.0) {
		auto normalized = input;
		normalized.source = InLayoutMetric(input.source, layout_aspect);
		normalized.output_mapping.scale_x /= layout_aspect;
		auto result = PreserveRectangleFallback(normalized,
												ScaleQuadX(target, layout_aspect), locked_rotation_z);
		if (result) {
			result->state = *FromLayoutMetric(result->state, layout_aspect);
			result->effective_target = ScaleQuadX(result->effective_target, 1.0 / layout_aspect);
		}
		return result;
	}
	if (!IsRectangle(target))
		return std::nullopt;
	double const width = input.source.bounds.rectangle.Width();
	double const height = input.source.bounds.rectangle.Height();
	double const scale_x = input.source.state.scale_x / 100.0;
	double const scale_y = input.source.state.scale_y / 100.0;
	if (!(width > 0.0) || !(height > 0.0) || !(scale_x > 0.0) || !(scale_y > 0.0))
		return std::nullopt;

	Vec2 const edge_x = target[1] - target[0];
	Vec2 const edge_y = target[3] - target[0];
	double const target_width = std::hypot(edge_x.x, edge_x.y);
	double const target_height = std::hypot(edge_y.x, edge_y.y);
	double const base_width = width * scale_x;
	double const base_height = height * scale_y;
	if (!(target_width > 0.0) || !(target_height > 0.0) || !(base_width > 0.0) || !(base_height > 0.0))
		return std::nullopt;

	// With fixed scales, every affine rectangle has the source aspect ratio.
	// Opposite shears can increase both sides by the same factor while keeping
	// them orthogonal; use that extra degree of freedom for the closest size.
	double const scale_ratio = scale_x / scale_y;
	double const target_angle = std::atan2(edge_x.y, edge_x.x);
	double factor = 1.0;
	double shear_x = 0.0;
	double shear_y = 0.0;
	double rotation = 0.0;
	if (locked_rotation_z) {
		if (!std::isfinite(*locked_rotation_z))
			return std::nullopt;
		// With a fixed Z angle, choose the opposite-shear rectangle whose local
		// x basis points at the requested edge. A positive x diagonal is required
		// by the renderer, so angles beyond +/-90 degrees have no valid fallback.
		double const local_angle =
			target_angle + *locked_rotation_z * DegreesToRadians;
		if (std::cos(local_angle) <= 1.0e-12)
			return std::nullopt;
		double const shear_magnitude = -std::tan(local_angle);
		factor = std::sqrt(1.0 + shear_magnitude * shear_magnitude);
		shear_x = shear_magnitude / scale_ratio;
		shear_y = -scale_ratio * shear_magnitude;
		rotation = *locked_rotation_z * DegreesToRadians;
	}
	else {
		factor = std::max(1.0,
						  (target_width * base_width + target_height * base_height) / (base_width * base_width + base_height * base_height));
		double const shear_magnitude =
			std::sqrt(std::max(0.0, factor * factor - 1.0));
		shear_x = shear_magnitude / scale_ratio;
		shear_y = -scale_ratio * shear_magnitude;
		// The local x basis is tilted by the opposite shear before the renderer's
		// clockwise Z rotation. Align that basis with the target's top edge.
		double const local_angle = std::atan2(scale_y * shear_y, scale_x);
		rotation = local_angle - target_angle;
	}
	Vec2 const center = (target[0] + target[2]) / 2.0;
	Vec2 const local_width{
		base_width * factor * std::cos(target_angle),
		base_width * factor * std::sin(target_angle)};
	Vec2 const local_height{
		-base_height * factor * std::sin(target_angle),
		base_height * factor * std::cos(target_angle)};
	Quad landed_target{
		center - (local_width + local_height) / 2.0,
		center + (local_width - local_height) / 2.0,
		center + (local_width + local_height) / 2.0,
		center + (local_width * -1.0 + local_height) / 2.0};

	auto state = input.source.state;
	state.origin.reset();
	state.scale_x = input.source.state.scale_x;
	state.scale_y = input.source.state.scale_y;
	state.shear_x = shear_x;
	state.shear_y = shear_y;
	state.rotation_x = 0.0;
	state.rotation_y = 0.0;
	state.rotation_z = locked_rotation_z
						   ? *locked_rotation_z
						   : rotation * RadiansToDegrees;
	Vec2 const shift = ResolveBoundsAlignmentShift(
		input.source.bounds, state.alignment);
	Vec2 const first{
		(input.source.bounds.rectangle.left + input.source.bounds.rectangle.top * shear_x + shift.x) * scale_x,
		(input.source.bounds.rectangle.left * shear_y + input.source.bounds.rectangle.top + shift.y) * scale_y};
	double const theta = locked_rotation_z
							 ? -*locked_rotation_z * DegreesToRadians
							 : -rotation;
	state.position = landed_target[0] - RotateZ(first, theta);
	if (!std::isfinite(state.position.x) || !std::isfinite(state.position.y))
		return std::nullopt;
	// Keep judging against the homothetic target. The landed rectangle is only
	// the closest fixed-aspect affine state; its residual must remain visible to
	// the normal candidate ranking and preview.
	return PreserveShapeAffineResult{state, target, false};
}

std::optional<EvaluatedTransformState> ExplicitOriginCandidate(
	SolverInput const& input,
	Quad const& target,
	ForwardResult const& projection_context,
	double layout_aspect = 1.0) {
	if (layout_aspect != 1.0) {
		auto normalized = input;
		normalized.source = InLayoutMetric(input.source, layout_aspect);
		return FromLayoutMetric(ExplicitOriginCandidate(normalized,
														ScaleQuadX(target, layout_aspect), projection_context),
								layout_aspect);
	}
	auto const center = QuadCenter(target);
	if (!center)
		return std::nullopt;
	auto q0 = target[0] - *center;
	auto q1 = target[1] - *center;
	auto q2 = target[2] - *center;
	auto q3 = target[3] - *center;
	Vec2 const diagonal = target[2] - target[0];
	Vec2 const side2 = target[1] - target[2];
	Vec2 const side3 = target[3] - target[2];
	double z1 = 0.0;
	double z3 = 0.0;
	if (!Solve2x2(side2.x, side3.x, side2.y, side3.y,
		-diagonal.x, -diagonal.y, z1, z3))
		return std::nullopt;

	double const distance = projection_context.camera_distance;
	std::array<Vec3, 4> points {{
		{q0.x, q0.y, distance},
		{q1.x * z1, q1.y * z1, distance * z1},
		{q2.x * (z1 + z3 - 1.0), q2.y * (z1 + z3 - 1.0), distance * (z1 + z3 - 1.0)},
		{q3.x * z3, q3.y * z3, distance * z3},
	}};
	Vec3 const side0 = points[1] - points[0];
	Vec3 const side1 = points[3] - points[0];
	double lambda0 = 0.0;
	double lambda1 = 0.0;
	if (!Solve2x2(side0.x, side1.x, side0.y, side1.y,
		-points[0].x, -points[0].y, lambda0, lambda1))
		return std::nullopt;
	double const origin_z = (points[0] + side0 * lambda0 + side1 * lambda1).z;
	if (!std::isfinite(origin_z) || std::abs(origin_z) <= 1.0e-12)
		return std::nullopt;
	for (auto& point : points) {
		point = point * (distance / origin_z) - Vec3 {0.0, 0.0, distance};
		if (!IsFinite(point))
			return std::nullopt;
	}

	Vec3 normal = (points[1] - points[0]).Cross(points[3] - points[0]);
	if (normal.Length() <= 1.0e-12)
		return std::nullopt;
	double const rotate_y = std::atan2(normal.x, normal.z);
	normal = RotateY(normal, rotate_y);
	double const rotate_x = std::atan2(normal.y, normal.z);
	for (auto& point : points)
		point = RotateX(RotateY(point, rotate_y), rotate_x);
	double const width = input.source.bounds.rectangle.Width();
	double const height = input.source.bounds.rectangle.Height();
	auto state = input.source.state;
	state.origin = *center;
	state.rotation_x = rotate_x * RadiansToDegrees;
	state.rotation_y = -rotate_y * RadiansToDegrees;
	Vec2 const first = {input.source.bounds.rectangle.left, input.source.bounds.rectangle.top};
	Vec2 const shift = ResolveBoundsAlignmentShift(input.source.bounds, state.alignment);
	if (input.locked_rotation_z) {
		// Repository-local decomposition: keep the renderer-facing Z angle and
		// recover the remaining 2D basis with both ASS shear axes.
		double const rotation = *input.locked_rotation_z * DegreesToRadians;
		Vec2 const top {
			(points[1].x - points[0].x) / width,
			(points[1].y - points[0].y) / width};
		Vec2 const left {
			(points[3].x - points[0].x) / height,
			(points[3].y - points[0].y) / height};
		Vec2 const basis_x = RotateZ(top, rotation);
		Vec2 const basis_y = RotateZ(left, rotation);
		if (!std::isfinite(basis_x.x) || !std::isfinite(basis_x.y)
			|| !std::isfinite(basis_y.x) || !std::isfinite(basis_y.y)
			|| basis_x.x <= 1.0e-12 || basis_y.y <= 1.0e-12)
			return std::nullopt;
		state.scale_x = basis_x.x * 100.0;
		state.scale_y = basis_y.y * 100.0;
		state.shear_x = basis_y.x / basis_x.x;
		state.shear_y = basis_x.y / basis_y.y;
		state.rotation_z = *input.locked_rotation_z;
		Vec2 const local {
			(first.x + first.y * state.shear_x + shift.x) * basis_x.x,
			(first.x * state.shear_y + first.y + shift.y) * basis_y.y,
		};
		Vec2 const rotated_first = RotateZ(
			Vec2 {points[0].x, points[0].y}, rotation);
		state.position = *center + rotated_first - local;
		return state;
	}

	Vec3 top = points[1] - points[0];
	double const rotate_z = std::atan2(top.y, top.x);
	for (auto& point : points)
		point = RotateZ(point, -rotate_z);

	top = points[1] - points[0];
	Vec3 const left = points[3] - points[0];
	if (std::abs(left.y) <= 1.0e-12)
		return std::nullopt;
	double const scale_x = top.Length() / width;
	double const scale_y = std::abs(left.y) / height;
	if (!std::isfinite(scale_x) || !std::isfinite(scale_y)
		|| scale_x <= 0.0 || scale_y <= 0.0)
		return std::nullopt;

	state.scale_x = scale_x * 100.0;
	state.scale_y = scale_y * 100.0;
	state.shear_x = (left.x / left.y) * scale_y / scale_x;
	state.shear_y = 0.0;
	state.rotation_z = -rotate_z * RadiansToDegrees;
	Vec2 const local {
		(first.x + first.y * state.shear_x + shift.x) * scale_x,
		(first.x * state.shear_y + first.y + shift.y) * scale_y,
	};
	state.position = *center + Vec2 {points[0].x - local.x, points[0].y - local.y};
	return state;
}

using Parameters = std::array<double, ParameterCount>;
using ResidualVector = std::array<double, CornerResidualCount>;
using LinearMatrix = std::array<std::array<double, ParameterCount>, ParameterCount>;

Parameters ParametersFromState(EvaluatedTransformState const& state) {
	return {
		state.position.x,
		state.position.y,
		std::log(state.scale_x / 100.0),
		std::log(state.scale_y / 100.0),
		state.shear_x,
		state.shear_y,
		state.rotation_z * DegreesToRadians,
		state.rotation_x * DegreesToRadians,
		state.rotation_y * DegreesToRadians,
	};
}

std::optional<EvaluatedTransformState> StateFromParameters(
	EvaluatedTransformState const& source,
	Parameters const& parameters,
	ImplicitModel model) {
	for (double const value : parameters) {
		if (!std::isfinite(value))
			return std::nullopt;
	}
	auto state = source;
	if (!model.preserve_origin)
		state.origin.reset();
	state.position = {parameters[0], parameters[1]};
	if (model.preserve_scale) {
		state.scale_x = source.scale_x;
		state.scale_y = source.scale_y;
	}
	else {
		state.scale_x = 100.0 * std::exp(parameters[2]);
		state.scale_y = 100.0 * std::exp(parameters[3]);
	}
	// The locked slots read the parameter vector itself: IsFixedImplicitParameter
	// excludes them from every trial update, so a frozen slot keeps the value
	// the optimizer was seeded with for the whole fit. That is what makes a lock
	// preserve the seed's shear or rotation instead of forcing some constant.
	state.shear_x = parameters[4];
	state.shear_y = parameters[5];
	if (model.preserve_rotation_z)
		state.rotation_z =
			model.locked_rotation_z.value_or(parameters[6] * RadiansToDegrees);
	else
		state.rotation_z = parameters[6] * RadiansToDegrees;
	state.rotation_x = parameters[7] * RadiansToDegrees;
	state.rotation_y = parameters[8] * RadiansToDegrees;
	if (!std::isfinite(state.scale_x) || !std::isfinite(state.scale_y))
		return std::nullopt;
	return state;
}

bool IsFixedImplicitParameter(std::size_t index, ImplicitModel model) {
	if (model.preserve_scale && (index == 2 || index == 3))
		return true;
	if ((model.preserve_scale_x && index == 2) || (model.preserve_scale_y && index == 3))
		return true;
	if (model.preserve_shear_x && index == 4)
		return true;
	if (model.preserve_shear_y && index == 5)
		return true;
	if (model.preserve_rotation_z && index == 6)
		return true;
	return model.preserve_plane && (index == 7 || index == 8);
}

bool EvaluateParameters(
	SolverInput const& input,
	Quad const& target,
	Parameters const& parameters,
	ImplicitModel model,
	ResidualVector& residual,
	double& cost) {
	auto const state = StateFromParameters(input.source.state, parameters, model);
	if (!state)
		return false;
	auto const forward = ForwardQuad(input.source, *state);
	if (!forward)
		return false;
	cost = 0.0;
	for (std::size_t index = 0; index < target.size(); ++index) {
		residual[index * 2] =
			(forward.quad[index].x - target[index].x) * input.output_mapping.scale_x;
		residual[index * 2 + 1] =
			(forward.quad[index].y - target[index].y) * input.output_mapping.scale_y;
		cost += residual[index * 2] * residual[index * 2]
			+ residual[index * 2 + 1] * residual[index * 2 + 1];
	}
	return std::isfinite(cost);
}

bool SolveLinear(LinearMatrix matrix, Parameters right, Parameters& solution) {
	// Singularity is relative to the system's own scale: angle terms and
	// pixel-coordinate terms mix in one normal matrix, so an absolute pivot
	// floor either accepts ill-conditioned systems at pixel scale or
	// rejects healthy ones at normalized scale. 1e-12 of the largest entry
	// is the structural-singularity band for double precision; the solver's
	// residual self-check catches anything borderline the band lets through.
	double scale = 0.0;
	for (auto const& row : matrix)
		for (double entry : row)
			scale = std::max(scale, std::abs(entry));
	for (std::size_t column = 0; column < ParameterCount; ++column) {
		std::size_t pivot = column;
		for (std::size_t row = column + 1; row < ParameterCount; ++row) {
			if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column]))
				pivot = row;
		}
		if (!(std::abs(matrix[pivot][column]) > scale * 1.0e-12))
			return false;
		if (pivot != column) {
			std::swap(matrix[pivot], matrix[column]);
			std::swap(right[pivot], right[column]);
		}
		double const divisor = matrix[column][column];
		for (std::size_t entry = column; entry < ParameterCount; ++entry)
			matrix[column][entry] /= divisor;
		right[column] /= divisor;
		for (std::size_t row = 0; row < ParameterCount; ++row) {
			if (row == column)
				continue;
			double const factor = matrix[row][column];
			for (std::size_t entry = column; entry < ParameterCount; ++entry)
				matrix[row][entry] -= factor * matrix[column][entry];
			right[row] -= factor * right[column];
		}
	}
	solution = right;
	return true;
}

std::optional<EvaluatedTransformState> OptimizeImplicitOrigin(
	SolverInput const& input,
	Quad const& target,
	EvaluatedTransformState const& initial,
	ImplicitModel model,
	int maximum_iterations = 80) {
	Parameters parameters = ParametersFromState(initial);
	// Multiline shear belongs to the layout, regardless of the family's seed.
	if (input.source.bounds.multiline_text) {
		parameters[4] = input.source.state.shear_x;
		parameters[5] = input.source.state.shear_y;
		model.preserve_shear_x = true;
		model.preserve_shear_y = true;
	}
	ResidualVector residual {};
	double cost = 0.0;
	if (!EvaluateParameters(input, target, parameters, model, residual, cost))
		return std::nullopt;
	double damping = 1.0e-3;
	for (int iteration = 0; iteration < maximum_iterations; ++iteration) {
		std::array<ResidualVector, ParameterCount> derivatives {};
		for (std::size_t column = 0; column < ParameterCount; ++column) {
			if (IsFixedImplicitParameter(column, model))
				continue;
			double const step = (column < 2 ? 1.0e-4 : 1.0e-6)
				* std::max(1.0, std::abs(parameters[column]));
			auto plus = parameters;
			auto minus = parameters;
			plus[column] += step;
			minus[column] -= step;
			ResidualVector plus_residual {};
			ResidualVector minus_residual {};
			double plus_cost = 0.0;
			double minus_cost = 0.0;
			bool const has_plus = EvaluateParameters(input, target, plus, model, plus_residual, plus_cost);
			bool const has_minus = EvaluateParameters(input, target, minus, model, minus_residual, minus_cost);
			if (!has_plus && !has_minus)
				return std::nullopt;
			for (std::size_t row = 0; row < CornerResidualCount; ++row) {
				if (has_plus && has_minus)
					derivatives[column][row] = (plus_residual[row] - minus_residual[row]) / (2.0 * step);
				else if (has_plus)
					derivatives[column][row] = (plus_residual[row] - residual[row]) / step;
				else
					derivatives[column][row] = (residual[row] - minus_residual[row]) / step;
			}
		}

		LinearMatrix normal {};
		Parameters gradient {};
		for (std::size_t left = 0; left < ParameterCount; ++left) {
			if (IsFixedImplicitParameter(left, model)) {
				normal[left][left] = 1.0;
				continue;
			}
			for (std::size_t row = 0; row < CornerResidualCount; ++row)
				gradient[left] += derivatives[left][row] * residual[row];
			for (std::size_t right = 0; right < ParameterCount; ++right) {
				if (IsFixedImplicitParameter(right, model))
					continue;
				for (std::size_t row = 0; row < CornerResidualCount; ++row)
					normal[left][right] += derivatives[left][row] * derivatives[right][row];
			}
			normal[left][left] += damping * (normal[left][left] + 1.0e-9);
			gradient[left] = -gradient[left];
		}
		Parameters delta {};
		if (!SolveLinear(normal, gradient, delta)) {
			damping *= 10.0;
			continue;
		}
		auto trial = parameters;
		double delta_norm = 0.0;
		for (std::size_t index = 0; index < ParameterCount; ++index) {
			if (IsFixedImplicitParameter(index, model))
				continue;
			trial[index] += delta[index];
			delta_norm = std::max(delta_norm, std::abs(delta[index]));
		}
		// Wrapping and clamping are only for slots the optimizer actually
		// moved. A frozen slot carries the seed's value verbatim for the whole
		// fit -- remainder() on the frozen rotation would rewrite 270 degrees
		// as -90 and change the \frz tag the line already carries, and a clamp
		// could do the same to a locked shear.
		if (!IsFixedImplicitParameter(2, model))
			trial[2] = std::clamp(trial[2], -12.0, 12.0);
		if (!IsFixedImplicitParameter(3, model))
			trial[3] = std::clamp(trial[3], -12.0, 12.0);
		if (!IsFixedImplicitParameter(4, model))
			trial[4] = std::clamp(trial[4], -1000.0, 1000.0);
		if (!IsFixedImplicitParameter(5, model))
			trial[5] = std::clamp(trial[5], -1000.0, 1000.0);
		if (!IsFixedImplicitParameter(6, model))
			trial[6] = std::remainder(trial[6], 2.0 * Pi);
		if (!IsFixedImplicitParameter(7, model))
			trial[7] = std::remainder(trial[7], 2.0 * Pi);
		if (!IsFixedImplicitParameter(8, model))
			trial[8] = std::remainder(trial[8], 2.0 * Pi);
		ResidualVector trial_residual {};
		double trial_cost = 0.0;
		if (EvaluateParameters(input, target, trial, model, trial_residual, trial_cost) && trial_cost < cost) {
			parameters = trial;
			residual = trial_residual;
			cost = trial_cost;
			damping = std::max(1.0e-12, damping / 3.0);
			if (delta_norm <= 1.0e-10 || cost <= 1.0e-18)
				break;
		}
		else {
			damping = std::min(1.0e12, damping * 10.0);
		}
	}
	return StateFromParameters(input.source.state, parameters, model);
}

// A projective quad has eight independent coordinates. With a fixed source
// scale, an explicit origin supplies the two degrees of freedom that the
// implicit-origin families otherwise spend on scale. Keeping one ASS shear
// axis canonical leaves exactly eight fitting parameters and avoids the
// singular normal matrix produced by optimizing both shear axes together.
constexpr std::size_t ExplicitOriginParameterCount = 8;
using ExplicitOriginParameters =
	std::array<double, ExplicitOriginParameterCount>;
using ExplicitOriginMatrix =
	std::array<std::array<double, ExplicitOriginParameterCount>, ExplicitOriginParameterCount>;
using ExplicitOriginResidual = std::array<double, CornerResidualCount>;

ExplicitOriginParameters ExplicitOriginParametersFromState(
	EvaluatedTransformState const& state, bool use_fay) {
	Vec2 const origin = state.origin.value_or(state.position);
	return {
		state.position.x,
		state.position.y,
		origin.x,
		origin.y,
		(use_fay ? state.shear_y : state.shear_x),
		state.rotation_z * DegreesToRadians,
		state.rotation_x * DegreesToRadians,
		state.rotation_y * DegreesToRadians,
	};
}

std::optional<EvaluatedTransformState> StateFromExplicitOriginParameters(
	EvaluatedTransformState const& source,
	ExplicitOriginParameters const& parameters,
	bool use_fay) {
	for (double const value : parameters) {
		if (!std::isfinite(value))
			return std::nullopt;
	}
	auto state = source;
	state.position = {parameters[0], parameters[1]};
	state.origin = Vec2{parameters[2], parameters[3]};
	state.shear_x = use_fay ? 0.0 : parameters[4];
	state.shear_y = use_fay ? parameters[4] : 0.0;
	state.rotation_z = parameters[5] * RadiansToDegrees;
	state.rotation_x = parameters[6] * RadiansToDegrees;
	state.rotation_y = parameters[7] * RadiansToDegrees;
	return state;
}

bool EvaluateExplicitOriginParameters(
	SolverInput const& input,
	Quad const& target,
	ExplicitOriginParameters const& parameters,
	bool use_fay,
	ExplicitOriginResidual& residual,
	double& cost) {
	auto const state = StateFromExplicitOriginParameters(
		input.source.state, parameters, use_fay);
	if (!state)
		return false;
	auto const forward = ForwardQuad(input.source, *state);
	if (!forward)
		return false;
	cost = 0.0;
	for (std::size_t index = 0; index < target.size(); ++index) {
		residual[index * 2] =
			(forward.quad[index].x - target[index].x) * input.output_mapping.scale_x;
		residual[index * 2 + 1] =
			(forward.quad[index].y - target[index].y) * input.output_mapping.scale_y;
		cost += residual[index * 2] * residual[index * 2] + residual[index * 2 + 1] * residual[index * 2 + 1];
	}
	return std::isfinite(cost);
}

bool SolveExplicitOriginLinear(
	ExplicitOriginMatrix matrix,
	ExplicitOriginParameters right,
	ExplicitOriginParameters& solution) {
	double scale = 0.0;
	for (auto const& row : matrix)
		for (double const entry : row)
			scale = std::max(scale, std::abs(entry));
	for (std::size_t column = 0; column < ExplicitOriginParameterCount; ++column) {
		std::size_t pivot = column;
		for (std::size_t row = column + 1;
			 row < ExplicitOriginParameterCount; ++row) {
			if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column]))
				pivot = row;
		}
		if (!(std::abs(matrix[pivot][column]) > scale * 1.0e-12))
			return false;
		if (pivot != column) {
			std::swap(matrix[pivot], matrix[column]);
			std::swap(right[pivot], right[column]);
		}
		double const divisor = matrix[column][column];
		for (std::size_t entry = column; entry < ExplicitOriginParameterCount; ++entry)
			matrix[column][entry] /= divisor;
		right[column] /= divisor;
		for (std::size_t row = 0; row < ExplicitOriginParameterCount; ++row) {
			if (row == column)
				continue;
			double const factor = matrix[row][column];
			for (std::size_t entry = column; entry < ExplicitOriginParameterCount; ++entry)
				matrix[row][entry] -= factor * matrix[column][entry];
			right[row] -= factor * right[column];
		}
	}
	solution = right;
	return true;
}

std::optional<EvaluatedTransformState> OptimizeFixedScaleExplicitOrigin(
	SolverInput const& input,
	Quad const& target,
	EvaluatedTransformState initial,
	bool use_fay) {
	initial.scale_x = input.source.state.scale_x;
	initial.scale_y = input.source.state.scale_y;
	initial.origin = initial.origin.value_or(initial.position);
	ExplicitOriginParameters parameters =
		ExplicitOriginParametersFromState(initial, use_fay);
	ExplicitOriginResidual residual{};
	double cost = 0.0;
	if (!EvaluateExplicitOriginParameters(
			input, target, parameters, use_fay, residual, cost))
		return std::nullopt;
	double damping = 1.0e-3;
	for (int iteration = 0; iteration < 100; ++iteration) {
		std::array<ExplicitOriginResidual, ExplicitOriginParameterCount> derivatives{};
		for (std::size_t column = 0; column < ExplicitOriginParameterCount; ++column) {
			double const step = (column < 4 ? 1.0e-4 : 1.0e-6) * std::max(1.0, std::abs(parameters[column]));
			auto plus = parameters;
			auto minus = parameters;
			plus[column] += step;
			minus[column] -= step;
			ExplicitOriginResidual plus_residual{};
			ExplicitOriginResidual minus_residual{};
			double plus_cost = 0.0;
			double minus_cost = 0.0;
			bool const has_plus = EvaluateExplicitOriginParameters(
				input, target, plus, use_fay, plus_residual, plus_cost);
			bool const has_minus = EvaluateExplicitOriginParameters(
				input, target, minus, use_fay, minus_residual, minus_cost);
			if (!has_plus && !has_minus)
				break;
			for (std::size_t row = 0; row < CornerResidualCount; ++row) {
				if (has_plus && has_minus)
					derivatives[column][row] =
						(plus_residual[row] - minus_residual[row]) / (2.0 * step);
				else if (has_plus)
					derivatives[column][row] =
						(plus_residual[row] - residual[row]) / step;
				else
					derivatives[column][row] =
						(residual[row] - minus_residual[row]) / step;
			}
		}

		ExplicitOriginMatrix normal{};
		ExplicitOriginParameters gradient{};
		for (std::size_t left = 0; left < ExplicitOriginParameterCount; ++left) {
			for (std::size_t row = 0; row < CornerResidualCount; ++row)
				gradient[left] += derivatives[left][row] * residual[row];
			for (std::size_t right = 0;
				 right < ExplicitOriginParameterCount; ++right) {
				for (std::size_t row = 0; row < CornerResidualCount; ++row)
					normal[left][right] +=
						derivatives[left][row] * derivatives[right][row];
			}
			normal[left][left] += damping * (normal[left][left] + 1.0e-9);
			gradient[left] = -gradient[left];
		}
		ExplicitOriginParameters delta{};
		if (!SolveExplicitOriginLinear(normal, gradient, delta)) {
			damping *= 10.0;
			continue;
		}
		auto trial = parameters;
		double delta_norm = 0.0;
		for (std::size_t index = 0; index < ExplicitOriginParameterCount; ++index) {
			trial[index] += delta[index];
			delta_norm = std::max(delta_norm, std::abs(delta[index]));
		}
		trial[4] = std::clamp(trial[4], -1000.0, 1000.0);
		trial[5] = std::remainder(trial[5], 2.0 * Pi);
		trial[6] = std::clamp(trial[6], -12.0, 12.0);
		trial[7] = std::clamp(trial[7], -12.0, 12.0);
		ExplicitOriginResidual trial_residual{};
		double trial_cost = 0.0;
		if (EvaluateExplicitOriginParameters(
				input, target, trial, use_fay, trial_residual, trial_cost) &&
			trial_cost < cost) {
			parameters = trial;
			residual = trial_residual;
			cost = trial_cost;
			damping = std::max(1.0e-12, damping / 3.0);
			if (delta_norm <= 1.0e-10 || cost <= 1.0e-18)
				break;
		}
		else {
			damping = std::min(1.0e12, damping * 10.0);
		}
	}
	if (auto result = StateFromExplicitOriginParameters(
			input.source.state, parameters, use_fay))
		return result;
	return initial;
}

// Rescales the drawn quad about its corner mean to the area the model set can
// actually produce, and reports the factor it used.
//
// This is what makes a scale lock size-blind. The area-pinned affine families
// have no size freedom at all -- their output area is exactly the frozen
// \fscx/\fscy times the base bounds area -- so they aim at the rescaled quad,
// the one size they can hit. The projective families keep trying the drawn
// quad verbatim first (foreshortening trades apparent area even at pinned
// scale, so quads of almost any area may still be exactly reachable) and only
// fall back to the rescaled one; see SolvePerspectiveTags. Either way, no
// candidate is ever refused merely because the user drew at a different size.
//
// Without this, a scale lock counts pure size drift as model error. A 4% drift
// on a 200x80 box is 8 output pixels, more than any sane tolerance, so the drag
// is refused over a mismatch the user was never asked to avoid.
std::optional<double> PreserveAreaFactor(Quad const& target, Quad const& reachable) {
	double const target_area = std::abs(SignedArea(target));
	double const reachable_area = std::abs(SignedArea(reachable));
	if (!std::isfinite(target_area) || !std::isfinite(reachable_area))
		return std::nullopt;
	if (target_area <= 0.0 || reachable_area <= 0.0)
		return std::nullopt;
	double const factor = std::sqrt(reachable_area / target_area);
	if (!std::isfinite(factor) || factor <= 0.0)
		return std::nullopt;
	return factor;
}

Quad ScaleQuadAboutCenter(Quad const& quad, double factor) {
	Vec2 center;
	for (auto const& point : quad)
		center = center + point / static_cast<double>(quad.size());
	Quad scaled;
	for (std::size_t index = 0; index < quad.size(); ++index)
		scaled[index] = center + (quad[index] - center) * factor;
	return scaled;
}

// The two corners an anchored edge must keep where they were drawn. Corner
// order is TL, TR, BR, BL: Top is p0->p1, Right p1->p2, Bottom p3->p2, Left
// p0->p3.
std::pair<std::size_t, std::size_t> EdgeAnchorEndpoints(PerspectiveEdgeAnchor anchor) {
	switch (anchor) {
		case PerspectiveEdgeAnchor::Top: return {0, 1};
		case PerspectiveEdgeAnchor::Right: return {1, 2};
		case PerspectiveEdgeAnchor::Bottom: return {3, 2};
		case PerspectiveEdgeAnchor::Left: return {0, 3};
		case PerspectiveEdgeAnchor::None: break;
	}
	return {0, 0};
}

// How far a landed quad's anchored corners sit from the drawn ones, in output
// pixels. The anchor is a promise about the drawn quad itself, so both the
// classification pre-check and the final gate measure against request.target,
// never against an area-normalized effective target that Preserve may have
// moved the very edge the user held down.
double HeldEdgeError(
	Quad const& landed,
	Quad const& drawn,
	std::pair<std::size_t, std::size_t> held,
	OutputCoordinateMapping mapping) {
	return std::max(
		std::hypot(
			(landed[held.first].x - drawn[held.first].x) * mapping.scale_x,
			(landed[held.first].y - drawn[held.first].y) * mapping.scale_y),
		std::hypot(
			(landed[held.second].x - drawn[held.second].x) * mapping.scale_x,
			(landed[held.second].y - drawn[held.second].y) * mapping.scale_y));
}

// Explains a NoFeasibleCandidate result by the stage every candidate died at,
// ordered by which lever actually unblocks a solve. Quantization outranks
// everything: a candidate whose model landed inside the snap budget is one
// Decimal Places bump away from feasible, no matter what else died.
// EdgeAnchor comes next: the anchor is a promise about the drawn edge, and a
// conflict there is resolved by releasing the anchor, which unblocks every
// candidate the current settings already allow. RepresentationPolicy follows
// and fires when the Fax + Frz Only filter killed at least one family
// outright: the families it kills are exactly the ones the unrestricted
// policy would have run, and those reach a hand-drawn quad exactly, so the
// switch itself is what to relax. Only when nothing died at any of those
// stages -- nothing generated could be projected or measured at all -- is
// ModelResidual the verdict. Rotation-lock rejections are model-stage deaths
// by this logic: the lock constrains the model a family fits, not the
// representation.
NoFeasibleReason ClassifyNoFeasibleReason(
	std::size_t policy_stage_deaths,
	std::size_t model_stage_deaths,
	std::size_t quantization_stage_deaths,
	std::size_t edge_anchor_stage_deaths) {
	if (quantization_stage_deaths > 0)
		return NoFeasibleReason::Quantization;
	if (edge_anchor_stage_deaths > 0)
		return NoFeasibleReason::EdgeAnchor;
	if (policy_stage_deaths > 0)
		return NoFeasibleReason::RepresentationPolicy;
	return NoFeasibleReason::ModelResidual;
}

}

char const* DescribeSolverError(SolverError error) {
	switch (error) {
		case SolverError::None: return "Perspective solver succeeded";
		case SolverError::InvalidSource: return "source Perspective state is invalid";
		case SolverError::InvalidTarget: return "target Perspective quad is invalid";
		case SolverError::InvalidOutputMapping: return "output coordinate mapping is invalid";
		case SolverError::InvalidErrorBudget: return "Perspective error budget is invalid";
		case SolverError::NoFeasibleCandidate: return "no quantized Perspective tag candidate meets the error budget";
	}
	return "unknown Perspective solver error";
}

int ClampPerspectiveDecimalPlaces(int value) {
	return std::clamp(value, kMinPerspectiveDecimalPlaces, kMaxPerspectiveDecimalPlaces);
}

char const* DescribeResidualError(ResidualError error) {
	switch (error) {
		case ResidualError::None: return "Perspective residual measurement succeeded";
		case ResidualError::InvalidCandidate: return "candidate Perspective state is invalid";
		case ResidualError::InvalidTarget: return "target Perspective quad is invalid";
		case ResidualError::InvalidOutputMapping: return "output coordinate mapping is invalid";
		case ResidualError::ProjectionDomain: return "Perspective residual sample is outside the projection domain";
	}
	return "unknown Perspective residual error";
}

std::string FormatAssNumber(double value, int maximum_decimals) {
	if (!std::isfinite(value))
		return {};
	maximum_decimals = std::clamp(maximum_decimals, 0, 9);
	value = Quantize(value, maximum_decimals);
	std::array<char, 128> buffer {};
	auto const conversion = std::to_chars(
		buffer.data(), buffer.data() + buffer.size(), value,
		std::chars_format::fixed, maximum_decimals);
	if (conversion.ec != std::errc())
		return {};
	std::string result(buffer.data(), conversion.ptr);
	if (auto const decimal = result.find('.'); decimal != std::string::npos) {
		while (!result.empty() && result.back() == '0')
			result.pop_back();
		if (!result.empty() && result.back() == '.')
			result.pop_back();
	}
	if (result == "-0" || result.empty())
		return "0";
	return result;
}

std::string FormatAssPoint(Vec2 point, int maximum_decimals) {
	auto const x = FormatAssNumber(point.x, maximum_decimals);
	auto const y = FormatAssNumber(point.y, maximum_decimals);
	if (x.empty() || y.empty())
		return {};
	return "(" + x + "," + y + ")";
}

bool MatchesPerspectiveRepresentationPolicy(
	EvaluatedTransformState const& state,
	PerspectiveRepresentationPolicy policy) {
	if (policy == PerspectiveRepresentationPolicy::Automatic)
		return true;
	return !state.origin
		&& state.shear_y == 0.0
		&& state.rotation_x == 0.0
		&& state.rotation_y == 0.0;
}

bool MatchesPerspectiveRepresentationPolicy(
	EvaluatedTransformState const& source,
	EvaluatedTransformState const& state,
	PerspectiveRepresentationPolicy policy) {
	if (policy == PerspectiveRepresentationPolicy::Automatic)
		return true;
	if (MatchesPerspectiveRepresentationPolicy(state, policy))
		return true;
	// Nothing outside the subset may be introduced or retuned, but what the
	// line already carried may stay. Judging the result shape absolutely would
	// reject even an unchanged line the moment it has an frx, which is exactly
	// when a typesetter reaches for the switch.
	bool const same_origin = source.origin.has_value() == state.origin.has_value()
		&& (!source.origin || NearlyEqual(*source.origin, *state.origin));
	return same_origin
		&& NearlyEqual(source.shear_y, state.shear_y)
		&& NearlyEqual(source.rotation_x, state.rotation_x)
		&& NearlyEqual(source.rotation_y, state.rotation_y);
}

ResidualResult MeasurePerspectiveResidual(
	ForwardInput const& candidate,
	Quad const& target,
	OutputCoordinateMapping output_mapping) {
	if (!IsValidOutputMapping(output_mapping))
		return {ResidualError::InvalidOutputMapping};
	if (!HasValidResidualSamples(candidate.bounds))
		return {ResidualError::InvalidCandidate, GeometryError::None,
			ForwardError::InvalidBounds};
	auto const forward = ForwardQuad(candidate);
	if (!forward)
		return {ResidualError::InvalidCandidate, GeometryError::None, forward.error};
	auto const target_validation = ValidateQuad(target);
	if (!target_validation)
		return {ResidualError::InvalidTarget, target_validation.error};
	auto const target_transform = MakeHomography(candidate.bounds.rectangle, target);
	if (!target_transform)
		return {ResidualError::InvalidTarget, target_transform.error};
	auto const samples = ResidualSamples(candidate.bounds, candidate.state);
	ResidualReference reference(samples, output_mapping);
	if (!reference.Reset(target_transform.value))
		return {.error = ResidualError::ProjectionDomain};
	auto const residual = reference.Measure(forward);
	if (!residual)
		return {ResidualError::ProjectionDomain};
	return {ResidualError::None, GeometryError::None, ForwardError::None, *residual};
}

SolverResult SolvePerspectiveTags(SolverInput const& input) {
	auto request = input;
	if (request.best_fit) {
		request.representation_policy = PerspectiveRepresentationPolicy::Automatic;
		request.edge_anchor = PerspectiveEdgeAnchor::None;
		request.shape_tolerance = request.max_error;
	}
	if (!HasValidResidualSamples(request.source.bounds))
		return {SolverError::InvalidSource, GeometryError::None,
			ForwardError::InvalidBounds};
	if (request.locked_rotation_z) {
		if (!std::isfinite(*request.locked_rotation_z))
			return {SolverError::InvalidSource, GeometryError::None,
				ForwardError::NonFiniteState};
		if (std::abs(*request.locked_rotation_z) > MaxTransformParameter)
			return {SolverError::InvalidSource, GeometryError::None,
				ForwardError::TransformParameterOutOfRange};
	}
	auto const current_forward = ForwardQuad(request.source);
	auto projection_context = current_forward;
	if (!projection_context) {
		projection_context = ForwardQuad(
			request.source, CanonicalizeCurrentGeometry(request.source.state));
		if (!projection_context)
			return {SolverError::InvalidSource, GeometryError::None,
				projection_context.error};
	}
	double const layout_aspect = *ResolvePerspectiveLayoutAspect(request.source);
	auto const target_validation = ValidateQuad(request.target);
	if (!target_validation)
		return {SolverError::InvalidTarget, target_validation.error};
	if (!IsValidOutputMapping(request.output_mapping))
		return {SolverError::InvalidOutputMapping};
	if (!std::isfinite(request.max_error) || request.max_error <= 0.0 || !std::isfinite(request.shape_tolerance) || request.shape_tolerance <= 0.0)
		return {.error = SolverError::InvalidErrorBudget};
	// Residual samples are immutable for one solve. Build them once instead of
	// reallocating custom drawing samples for every candidate trial.
	auto const residual_samples = ResidualSamples(
		request.source.bounds, request.source.state);
	ResidualReference target_reference(residual_samples, request.output_mapping);
	ResidualReference model_reference(residual_samples, request.output_mapping);

	// Under Preserve the drawn size carries no information -- turning off Fit
	// Text says "do not change my font size", not "my hand-drawn box is already
	// the exact right size". Every family therefore aims at one homothetic copy
	// of the drawn quad whose area matches the currently rendered text. This
	// keeps the inner preview's direction and vanishing points tied to the outer
	// frame while dropping only the size change. Under Fit the target is used
	// unchanged.
	Quad const drawn_target = request.target;
	Quad affine_target = drawn_target;
	std::optional<Homography> affine_target_transform;
	if (request.scale_policy == PerspectiveScalePolicy::Preserve) {
		if (auto const factor = PreserveAreaFactor(
				drawn_target, projection_context.quad)) {
			auto const rescaled = ScaleQuadAboutCenter(drawn_target, *factor);
			// A rescale can only fail validation in extreme cases, and a drawn
			// quad the user can still see is worth more than a normalized one
			// the solver would reject outright.
			if (ValidateQuad(rescaled)) {
				auto const normalized = MakeHomography(
					request.source.bounds.rectangle, rescaled);
				if (normalized) {
					affine_target = rescaled;
					affine_target_transform = normalized.value;
				}
			}
		}
	}

	auto const drawn_target_transform =
		MakeHomography(request.source.bounds.rectangle, drawn_target);
	if (!drawn_target_transform)
		return {SolverError::InvalidTarget, drawn_target_transform.error};
	if (!affine_target_transform)
		affine_target_transform = drawn_target_transform.value;
	std::optional<EvaluatedTransformState> fit_state;
	if (request.scale_policy == PerspectiveScalePolicy::Preserve) {
		// Preserve is the same operation as Fit with the size tags pinned. Keep
		// the Fit winner as a candidate seed so toggling the option does not
		// replace its plane and orientation with a different decomposition.
		SolverInput fit_request = request;
		fit_request.scale_policy = PerspectiveScalePolicy::Fit;
		if (auto const fitted = SolvePerspectiveTags(fit_request);
			fitted && fitted.candidate)
			fit_state = fitted.candidate->state;
	}

	struct RawCandidate {
		CandidateFamily family;
		EvaluatedTransformState state;
		Quad effective_target;
	};
	std::vector<RawCandidate> raw_candidates;
	if (current_forward) {
		if (!request.best_fit)
			raw_candidates.push_back({.family = CandidateFamily::NoOp,
									  .state = request.source.state,
									  .effective_target = affine_target});

		Quad const& translation_target =
			affine_target;
		Vec2 translation;
		for (std::size_t index = 0; index < translation_target.size(); ++index)
			translation = translation + (translation_target[index] - current_forward.quad[index]) / 4.0;
		auto position_only = request.source.state;
		position_only.position = position_only.position + translation;
		raw_candidates.push_back({CandidateFamily::CurrentRepresentation,
								  position_only, affine_target});
		if (request.source.state.origin) {
			auto translated_current = position_only;
			translated_current.origin = *translated_current.origin + translation;
			raw_candidates.push_back({CandidateFamily::CurrentRepresentation,
									  translated_current,
									  affine_target});
		}
	}

	bool const preserve_scale =
		request.scale_policy == PerspectiveScalePolicy::Preserve;
	bool const preserve_parallelogram =
		preserve_scale && IsParallelogram(drawn_target);
	// Closed forms cannot preserve multiline shear; use constrained refits.
	bool const multiline = request.source.bounds.multiline_text;
	std::optional<PreserveShapeAffineResult> preserve_shape;
	if (preserve_parallelogram && request.representation_policy == PerspectiveRepresentationPolicy::Automatic && !multiline) {
		preserve_shape = PreserveShapeAffineCandidate(
			request, affine_target, request.locked_rotation_z, layout_aspect);
		if (!preserve_shape && IsRectangle(ScaleQuadX(affine_target, layout_aspect)))
			preserve_shape = PreserveRectangleFallback(
				request, affine_target, request.locked_rotation_z, layout_aspect);
		if (preserve_shape)
			raw_candidates.push_back({.family = CandidateFamily::PreserveShapeAffine,
									  .state = preserve_shape->state,
									  .effective_target = preserve_shape->effective_target});
	}
	// A regular rectangle is a visual invariant under Preserve even when the
	// pinned scales cannot reproduce its aspect ratio exactly. In that case the
	// rectangle fallback is the deliberate closest state; allowing a projective
	// candidate to compete would trade a modest size shortfall for a visibly
	// skewed inner frame.
	bool const preserve_shape_available =
		!request.best_fit && preserve_shape && (preserve_shape->exact || IsRectangle(ScaleQuadX(affine_target, layout_aspect)));
	// The Fit decomposition is the useful continuity anchor only for an
	// unrestricted projective solve. Restricted Fax + Frz solves and the
	// dedicated affine shape candidates have stronger representation contracts
	// that must remain ahead of this preference.
	if (fit_state
		&& request.representation_policy == PerspectiveRepresentationPolicy::Automatic
		&& !multiline
		&& !preserve_shape_available) {
		auto state = *fit_state;
		state.scale_x = request.source.state.scale_x;
		state.scale_y = request.source.state.scale_y;
		raw_candidates.push_back({CandidateFamily::PreserveFitState,
								  state, affine_target});
	}
	auto const add_refit = [&](
							   CandidateFamily family,
							   EvaluatedTransformState const& seed,
							   ImplicitModel model,
							   Quad const& target) {
		// The refit optimizer must aim at the same quad the finished candidate
		// is judged against, or the fit and the acceptance check disagree.
		if (auto const candidate = OptimizeImplicitOrigin(request, target, seed, model))
			raw_candidates.push_back({.family = family, .state = *candidate, .effective_target = target});
	};

	if (auto const affine = FitAffine(
			affine_target,
			request.source.bounds.rectangle, request.edge_anchor)) {
		if (preserve_scale || multiline) {
			// Refit the free parameters at the pinned scale/shear. Similarity
			// already covers translation without resetting those fields.
			if (auto const seed = SimilarityCandidate(*affine, request.source, layout_aspect))
				add_refit(CandidateFamily::Similarity, *seed,
						  {.preserve_scale = preserve_scale, .preserve_plane = true, .preserve_shear_x = true, .preserve_shear_y = true}, affine_target);
			if (auto const seed = DecomposeAffine(*affine, request.source, false, layout_aspect))
				add_refit(CandidateFamily::AffineFax, *seed,
						  {.preserve_scale = preserve_scale, .preserve_plane = true, .preserve_shear_y = true}, affine_target);
			if (request.representation_policy == PerspectiveRepresentationPolicy::Automatic) {
				if (auto const seed = DecomposeAffine(*affine, request.source, true, layout_aspect))
					add_refit(CandidateFamily::AffineFay, *seed,
							  {.preserve_scale = preserve_scale, .preserve_plane = true, .preserve_shear_x = true}, affine_target);
			}
		}
		else {
			if (auto const candidate = TranslationCandidate(*affine, request.source, layout_aspect))
				raw_candidates.push_back({CandidateFamily::Translation, *candidate,
										  affine_target});
			if (auto const candidate = SimilarityCandidate(*affine, request.source, layout_aspect))
				raw_candidates.push_back({CandidateFamily::Similarity, *candidate,
										  affine_target});
			if (auto const candidate = DecomposeAffine(*affine, request.source, false, layout_aspect))
				raw_candidates.push_back({CandidateFamily::AffineFax, *candidate,
										  affine_target});
			if (request.representation_policy == PerspectiveRepresentationPolicy::Automatic) {
				if (auto const candidate = DecomposeAffine(*affine, request.source, true, layout_aspect))
					raw_candidates.push_back({CandidateFamily::AffineFay, *candidate,
											  affine_target});
			}
		}
	}

	// A line that already carries \fay has an affine shape the fax families
	// above can only fit by zeroing that tag: legal wherever dropping a
	// restricted tag is allowed, but it gives up a degree of freedom the quad
	// may need, and it rewrites the fay wherever the state is not clean without
	// it. Seed from the line itself and let the optimizer move pos/scale/fax/frz
	// with the fay held exactly as written. Automatic gets the family too: it is
	// a plain affine candidate, and the existing scoring already knows how to
	// rank it against the zeroing variants.
	if (std::abs(request.source.state.shear_y) > 1.0e-9 && !request.source.state.origin && std::abs(request.source.state.rotation_x) <= 1.0e-9 && std::abs(request.source.state.rotation_y) <= 1.0e-9) {
		add_refit(CandidateFamily::AffineFay, request.source.state,
				  {.preserve_scale = preserve_scale, .preserve_plane = true, .preserve_shear_y = true}, affine_target);
	}

	// A line that already declares a plane has a fit nobody generated before:
	// hold that plane exactly as written and re-solve only the in-plane tags.
	// Under a restricted policy this is the whole difference between "adjust
	// inside the perspective you set up" and "flatten it". The fax-flavoured
	// variant freezes shear_y at the line's own value, so a line carrying \fay
	// keeps it instead of having it zeroed; for a line without one the frozen
	// value is zero, which is exactly the fit the old hard-zeroed mode
	// produced. A rotation lock no longer skips the block: its refit freezes
	// frz at the lock instead of freeing it, so the lock filter below passes by
	// construction and a vertical line with an existing plane can finally refit
	// in plane at all.
	if ((request.source.state.origin || std::abs(request.source.state.rotation_x) > 1.0e-9 || std::abs(request.source.state.rotation_y) > 1.0e-9) && !preserve_shape_available) {
		// Automatic can change the projective axes and therefore follows the
		// homothetic Preserve target. Fax + Frz Only deliberately keeps the
		// declared plane; when that plane cannot realize the size-normalized
		// target, its residual is reported as the policy's honest shortfall.
		ImplicitModel const plane_model{
			.preserve_scale = preserve_scale,
			.preserve_plane = true,
			.preserve_shear_y = true,
			.preserve_rotation_z = request.locked_rotation_z.has_value(),
			.preserve_origin = true,
			.locked_rotation_z = request.locked_rotation_z};
		bool const preserve_target = preserve_scale && request.representation_policy == PerspectiveRepresentationPolicy::Automatic;
		Quad const& plane_target = preserve_target ? affine_target : drawn_target;
		add_refit(CandidateFamily::CurrentPlaneRefit, request.source.state,
				  plane_model, plane_target);
		// The zero-fax economy variant is meaningless under multi-line bounds:
		// the lock would just restore the source's fax and duplicate the refit
		// above, so it is only generated where the fax may actually move.
		if (request.representation_policy == PerspectiveRepresentationPolicy::Automatic && !multiline) {
			// Tag-economy variant: zero the fax in the seed and freeze it, so
			// the fit goes out to a fay-only representation when the quad
			// allows one. Seeding the zero rather than hard-coding it keeps
			// this a pure lock on slot 4.
			auto seed = request.source.state;
			seed.shear_x = 0.0;
			ImplicitModel const fay_only_model{
				.preserve_scale = preserve_scale,
				.preserve_plane = true,
				.preserve_shear_x = true,
				.preserve_rotation_z = request.locked_rotation_z.has_value(),
				.preserve_origin = true,
				.locked_rotation_z = request.locked_rotation_z};
			add_refit(CandidateFamily::CurrentPlaneRefit, seed,
					  fay_only_model, plane_target);
		}
	}
	if (request.representation_policy == PerspectiveRepresentationPolicy::Automatic && !preserve_shape_available) {
		Vec2 const source_center{
			(request.source.bounds.rectangle.left + request.source.bounds.rectangle.right) / 2.0,
			(request.source.bounds.rectangle.top + request.source.bounds.rectangle.bottom) / 2.0,
		};
		// Preserve has already selected the single homothetic target above.
		// Each projective family is fitted once against that target; retrying
		// with an area derived from the fitted state would change the requested
		// size and could no longer guarantee a proportional inner frame.
		auto const generate_projective = [&](Quad const& target,
											 Homography const& target_transform) {
			auto const run_family = [&](CandidateFamily family, auto&& fit) {
				if (auto const state = fit(target))
					raw_candidates.push_back({.family = family,
											  .state = *state,
											  .effective_target = target});
			};
			if (auto const local_affine = LocalAffine(target_transform, source_center)) {
				if (request.locked_rotation_z) {
					if (auto const initial = DecomposeLockedAffine(
							*local_affine, request.source, *request.locked_rotation_z, layout_aspect)) {
						ImplicitModel const model{
							.preserve_scale = preserve_scale,
							.preserve_rotation_z = true,
							.locked_rotation_z = request.locked_rotation_z};
						run_family(CandidateFamily::ProjectiveImplicitLockedDoubleShear,
								   [&](Quad const& aim) {
									   return OptimizeImplicitOrigin(
										   request, aim, *initial, model);
								   });
					}
				}
				else {
					for (bool const use_fay : {false, true}) {
						if (auto const initial = DecomposeAffine(
								*local_affine, request.source, use_fay, layout_aspect)) {
							ImplicitModel model;
							model.preserve_scale = preserve_scale;
							// One shear axis per family, frozen at the decomposed
							// seed's zero on the other axis.
							if (use_fay)
								model.preserve_shear_x = true;
							else
								model.preserve_shear_y = true;
							run_family(use_fay ? CandidateFamily::ProjectiveImplicitFay
											   : CandidateFamily::ProjectiveImplicitFax,
									   [&](Quad const& aim) {
										   return OptimizeImplicitOrigin(
											   request, aim, *initial, model);
									   });
						}
					}
				}
			}
			// The closed-form origin decomposition derives both shear axes
			// from the target geometry and cannot carry the multi-line lock.
			// Preserve needs a fixed-scale variant: resetting the scales of the
			// closed-form result after fitting changes the quad shape. Re-solving
			// the same eight projective degrees of freedom with an explicit origin
			// keeps the target shape while honoring the source scale.
			if (!multiline) {
				if (preserve_scale && !request.locked_rotation_z) {
					for (bool const use_fay : {false, true}) {
						run_family(CandidateFamily::ProjectiveExplicitOrigin,
								   [&](Quad const& aim) {
									   auto const initial = ExplicitOriginCandidate(
										   request, aim, projection_context, layout_aspect);
									   if (!initial)
										   return std::optional<EvaluatedTransformState>{};
									   return OptimizeFixedScaleExplicitOrigin(
										   request, aim, *initial, use_fay);
								   });
					}
				}
				else {
					run_family(CandidateFamily::ProjectiveExplicitOrigin,
							   [&](Quad const& aim) {
								   return ExplicitOriginCandidate(
									   request, aim, projection_context, layout_aspect);
							   });
				}
			}
		};
		// Preserve means that the preview is a uniformly scaled copy of the
		// drawn quad. Fit uses the original target.
		if (preserve_scale)
			generate_projective(
				affine_target, affine_target_transform.value());
		else
			generate_projective(drawn_target, drawn_target_transform.value);
	}

	// Refit a small set of useful style constraints around the best ordinary
	// implicit representation. Each frozen style value can remove a tag; the
	// remaining parameters must earn that saving by fitting the target again.
	if (!request.best_fit && request.event_style && !preserve_scale && !preserve_shape_available) {
		std::optional<std::size_t> seed_index;
		std::optional<CandidateScore> seed_score;
		for (std::size_t index = 0; index < raw_candidates.size(); ++index) {
			auto const& raw = raw_candidates[index];
			if (raw.state.origin || raw.family == CandidateFamily::NoOp)
				continue;
			auto const transform = MakeHomography(request.source.bounds.rectangle, raw.effective_target);
			if (!transform || !target_reference.Reset(transform.value))
				continue;
			auto const error = StateResidual(request, target_reference, raw.state);
			if (!error || *error > request.shape_tolerance)
				continue;
			auto const score = ScoreCandidate(request.source.state, raw.state,
											  Serialize(raw.state, request.maximum_decimals), raw.family, *error);
			if (!seed_score || BetterScore(score, *seed_score)) {
				seed_score = score;
				seed_index = index;
			}
		}
		if (seed_index) {
			auto const base = raw_candidates[*seed_index];
			for (int constraint = 0; constraint < 3; ++constraint) {
				auto seed = base.state;
				ImplicitModel model;
				model.preserve_plane = std::abs(seed.rotation_x) <= 1.0e-9 && std::abs(seed.rotation_y) <= 1.0e-9;
				model.preserve_shear_y = true;
				model.preserve_rotation_z = request.locked_rotation_z.has_value();
				model.locked_rotation_z = request.locked_rotation_z;
				if (constraint == 0) {
					if (NearlyEqual(seed.scale_x, request.event_style->scale_x))
						continue;
					seed.scale_x = request.event_style->scale_x;
					model.preserve_scale_x = true;
				}
				else if (constraint == 1) {
					if (NearlyEqual(seed.scale_y, request.event_style->scale_y))
						continue;
					seed.scale_y = request.event_style->scale_y;
					model.preserve_scale_y = true;
				}
				else {
					if (request.locked_rotation_z || EquivalentRotation(seed.rotation_z, request.event_style->rotation_z))
						continue;
					seed.rotation_z = request.event_style->rotation_z;
					model.preserve_rotation_z = true;
				}
				if (auto state = OptimizeImplicitOrigin(request, base.effective_target, seed, model, 24)) {
					if (constraint == 0)
						state->scale_x = request.event_style->scale_x;
					else if (constraint == 1)
						state->scale_y = request.event_style->scale_y;
					else
						state->rotation_z = request.event_style->rotation_z;
					raw_candidates.push_back({.family = CandidateFamily::StyleConstrainedRefit, .state = *state, .effective_target = base.effective_target});
				}
			}
		}
	}

	// Corner least squares is only an initial fit. Rebalance the position of
	// approximate implicit candidates over borders, shadows and drawing
	// controls too, retaining the original whenever Euclidean error worsens.
	// Preserve and anchored edges have stronger location contracts.
	if (!preserve_scale && request.edge_anchor == PerspectiveEdgeAnchor::None) {
		std::size_t const count = raw_candidates.size();
		for (std::size_t index = 0; index < count; ++index) {
			auto const raw = raw_candidates[index];
			if (raw.state.origin || raw.family == CandidateFamily::NoOp)
				continue;
			auto const target = MakeHomography(request.source.bounds.rectangle, raw.effective_target);
			if (!target || !target_reference.Reset(target.value))
				continue;
			auto const landed = ForwardQuad(request.source, raw.state);
			if (!landed)
				continue;
			auto const original_error = target_reference.Measure(landed);
			if (!original_error || *original_error <= (request.best_fit ? 0.0 : request.shape_tolerance))
				continue;
			auto const offset = target_reference.CenteringOffset(landed);
			if (!offset)
				continue;
			auto state = raw.state;
			state.position = state.position + *offset;
			auto const error = StateResidual(request, target_reference, state);
			if (error && *error < *original_error)
				raw_candidates.push_back({.family = raw.family, .state = state, .effective_target = raw.effective_target});
		}
	}

	std::optional<SolverCandidate> best;
	Quad best_target {};
	// Stage-at-death counters over the raw candidates, so a total failure can
	// be attributed to the stage that owns the fix. Lock-filter deaths are
	// model-stage deaths: the lock constrains the model the family fits, not
	// the representation policy.
	std::size_t policy_stage_deaths = 0;
	std::size_t model_stage_deaths = 0;
	std::size_t quantization_stage_deaths = 0;
	std::size_t edge_anchor_stage_deaths = 0;
	// Closest any candidate's digits got, so a total failure can quote the
	// rounding error and its budget instead of only naming the option.
	std::optional<NoFeasibleMetrics> quantization_metrics;
	auto const consider = [](std::optional<NoFeasibleMetrics>& best,
							 QuantizeOutcome const& outcome) {
		if (!outcome.rejected_error)
			return;
		if (!best || *outcome.rejected_error < best->best_error) {
			best = NoFeasibleMetrics{
				.best_error = *outcome.rejected_error,
				.budget = outcome.rejected_budget,
			};
		}
	};
	for (auto const& raw : raw_candidates) {
		auto state = raw.state;
		if (preserve_scale) {
			// Pin rather than reject. Every family generated under this policy
			// was already fitted at the source's scale, so this only clears
			// optimizer drift in the last bits; rejecting on inequality is what
			// used to reduce a scale lock to a translation-only solver.
			state.scale_x = request.source.state.scale_x;
			state.scale_y = request.source.state.scale_y;
		}
		if (request.source.bounds.multiline_text
			&& (!NearlyEqual(state.shear_x, request.source.state.shear_x)
				|| !NearlyEqual(state.shear_y, request.source.state.shear_y))) {
			// The multi-line shear lock is part of the generation contract
			// above; this filter is the backstop for the few families whose
			// closed forms cannot carry it. Rejecting rather than restoring
			// keeps the fitted geometry of the remaining parameters honest --
			// overwriting a fitted shear after the fact would invalidate the
			// pos/scale/rotation the fit chose for it.
			++model_stage_deaths;
			continue;
		}
		if (request.locked_rotation_z) {
			if (!EquivalentRotation(state.rotation_z, *request.locked_rotation_z)) {
				++model_stage_deaths;
				continue;
			}
			state.rotation_z = *request.locked_rotation_z;
		}
		if (!MatchesPerspectiveRepresentationPolicy(
				request.source.state, state, request.representation_policy)) {
			++policy_stage_deaths;
			continue;
		}
		// snap_error and total_error are measured against this candidate's own
		// effective target: a plane refit that hit the drawn quad and an affine
		// fit that hit the area-normalized one compete through the existing
		// BetterScore ordering alone, with no new ranking rule.
		auto const judging_transform =
			MakeHomography(request.source.bounds.rectangle, raw.effective_target);
		if (!judging_transform) {
			++model_stage_deaths;
			continue;
		}
		// The anchored edge is a hard obligation measured against the drawn
		// target, decided before quantization runs. A raw candidate whose
		// model already misses the held edge is doomed for every decimal
		// setting: counting its later digits death as quantization would send
		// the user to raise Decimal Places for a conflict the digits can never
		// resolve, so it dies on the anchor stage here instead and skips the
		// quantization work entirely. The check runs again on the quantized
		// state below, where rounding may still cost the last bit of budget.
		std::pair<std::size_t, std::size_t> held {};
		if (request.edge_anchor != PerspectiveEdgeAnchor::None) {
			held = EdgeAnchorEndpoints(request.edge_anchor);
			auto const raw_landed = ForwardQuad(request.source, state);
			if (raw_landed
				&& HeldEdgeError(
					   raw_landed.quad, request.target, held,
					   request.output_mapping)
					> request.max_error) {
				++edge_anchor_stage_deaths;
				continue;
			}
		}
		if (!target_reference.Reset(judging_transform.value)) {
			++model_stage_deaths;
			continue;
		}
		auto outcome = QuantizeCandidate(
			request, target_reference, raw.family, state, model_reference);
		if (!outcome.candidate) {
			if (outcome.stage == QuantizeStage::QuantizationRejected) {
				++quantization_stage_deaths;
				consider(quantization_metrics, outcome);
			}
			else
				++model_stage_deaths;
			continue;
		}
		if (request.edge_anchor != PerspectiveEdgeAnchor::None) {
			auto const landed = ForwardQuad(request.source, outcome.candidate->state);
			if (!landed) {
				++model_stage_deaths;
				continue;
			}
			if (HeldEdgeError(
					landed.quad, request.target, held, request.output_mapping)
				> request.max_error) {
				++edge_anchor_stage_deaths;
				continue;
			}
		}
		bool const better = best && (request.best_fit
										 ? outcome.candidate->max_error < best->max_error
										 : BetterScore(outcome.candidate->score, best->score));
		if (!best || better) {
			best = std::move(outcome.candidate);
			best_target = raw.effective_target;
		}
	}
	if (!best) {
		auto const reason = ClassifyNoFeasibleReason(
			policy_stage_deaths, model_stage_deaths, quantization_stage_deaths,
			edge_anchor_stage_deaths);
		// Only the digits stage carries a number worth quoting: models are
		// never refused for distance anymore, and representation-policy deaths
		// happen at the family filter before anything ran.
		std::optional<NoFeasibleMetrics> metrics;
		if (reason == NoFeasibleReason::Quantization)
			metrics = quantization_metrics;
		return {SolverError::NoFeasibleCandidate, GeometryError::None,
				ForwardError::None, std::nullopt, raw_candidates.size(), drawn_target,
				reason, metrics};
	}
	return {SolverError::None, GeometryError::None, ForwardError::None,
		std::move(best), raw_candidates.size(), best_target};
}
}
