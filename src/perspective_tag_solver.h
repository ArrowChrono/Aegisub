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

#pragma once

#include "perspective_forward.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace perspective {

struct OutputCoordinateMapping {
	double scale_x = 1.0;
	double scale_y = 1.0;
};

enum class CandidateFamily {
	NoOp,
	CurrentRepresentation,
	Translation,
	Similarity,
	AffineFax,
	AffineFay,
	// Keeps the \org and frx/fry the line already declares and re-solves only
	// the in-plane tags. Reaches shapes no other family can under a restricted
	// policy, where the projective families are not generated at all.
	CurrentPlaneRefit,
	ProjectiveImplicitFax,
	ProjectiveImplicitFay,
	ProjectiveImplicitLockedDoubleShear,
	ProjectiveExplicitOrigin,
	// Preserve-scale fallback for parallelogram targets. Keeps the rendered
	// shape affine when a perspective fit would trade shape for the locked size.
	PreserveShapeAffine,
	// Preserve-scale candidate seeded from the corresponding Fit result so
	// toggling Fit Text changes the scale tags before changing other geometry.
	PreserveFitState,
	StyleConstrainedRefit,
};

struct SerializedTransformState {
	std::string position;
	std::optional<std::string> origin;
	std::string scale_x;
	std::string scale_y;
	std::string shear_x;
	std::string shear_y;
	std::string rotation_x;
	std::string rotation_y;
	std::string rotation_z;
};

struct CandidateScore {
	// Candidates within the shape tolerance share an economy band. Outside
	// that band, distance remains ahead of representation preferences.
	int snapped = 0;
	int snap_bucket = 0;
	// Shape-only continuity preference for PreserveFitState. A negative value
	// means its pinned-scale result is homothetic to the drawn quad; positive
	// values are shape-error buckets, so visibly different planes still lose.
	int fit_consistency_penalty = 0;
	int changed_tag_count = 0;
	std::size_t emitted_tag_count = 0;
	int explicit_origin_penalty = 0;
	int perspective_penalty = 0;
	int non_fax_shear_penalty = 0;
	std::size_t token_count = 0;
	double condition_penalty = 0.0;
	int family_rank = 0;
	double residual = 0.0;
};

struct SolverCandidate {
	CandidateFamily family = CandidateFamily::NoOp;
	EvaluatedTransformState state;
	SerializedTransformState serialized;
	CandidateScore score;
	// Total deviation of the emitted tags from effective_target, in output
	// pixels, measured directly rather than adding the two error maxima.
	double max_error = 0.0;
	// How far the unrounded model itself falls short of effective_target. Zero
	// unless the quad lies outside what this tag subset can express, which is
	// the normal case for a hand-dragged corner under a restricted policy.
	double snap_error = 0.0;
	// Rounding error the tag digits add on top of the model, measured against
	// the model's own transform rather than the target.
	double quantization_error = 0.0;

	[[nodiscard]] bool Snapped() const { return score.snapped != 0; }
};

enum class PerspectiveScalePolicy {
	Preserve,
	Fit,
};

struct PerspectiveTagCost {
	std::size_t tag_count = 0;
	std::size_t byte_count = 0;
};

enum class PerspectiveRepresentationPolicy {
	Automatic,
	FaxFrzOnly,
};

// Which drawn edge must land exactly where it was drawn. The anchored edge's
// two endpoints are a hard obligation on every candidate, not a preference
// folded into the affine fit: a candidate that moves them is rejected no
// matter how well it scores, and a solve where nothing can honor the edge is
// reported as NoFeasibleReason::EdgeAnchor. Corner order is TL, TR, BR, BL:
// Top is p0->p1, Right p1->p2, Bottom p3->p2, Left p0->p3. An anchored edge
// and its opposite are interchangeable as inputs; what differs is which one
// keeps its drawn position.
enum class PerspectiveEdgeAnchor {
	None,
	Top,
	Right,
	Bottom,
	Left,
};

constexpr int kMinPerspectiveDecimalPlaces = 0;
constexpr int kMaxPerspectiveDecimalPlaces = 6;
constexpr int kDefaultPerspectiveDecimalPlaces = 4;

[[nodiscard]] int ClampPerspectiveDecimalPlaces(int value);

struct SolverInput {
	ForwardInput source;
	Quad target;
	OutputCoordinateMapping output_mapping;
	// Rounding budget: how much error the emitted decimal digits may add on
	// top of whatever model was chosen. This is a serialization tolerance, not
	// a statement about which shapes are reachable: every measurable model is
	// a candidate, the best one wins by score, and its shortfall against the
	// drawn quad is reported in snap_error and drawn by the preview rather
	// than used to refuse the drag.
	double max_error = 0.1;
	// Optional renderer-facing orientation lock. Vertical CJK faces use a
	// semantic base rotation which must survive geometry decomposition.
	std::optional<double> locked_rotation_z;
	PerspectiveScalePolicy scale_policy = PerspectiveScalePolicy::Fit;
	PerspectiveRepresentationPolicy representation_policy =
		PerspectiveRepresentationPolicy::Automatic;
	bool best_fit = false;
	// Which drawn edge to hold exactly. A hard constraint on every candidate;
	// see PerspectiveEdgeAnchor.
	PerspectiveEdgeAnchor edge_anchor = PerspectiveEdgeAnchor::None;
	// Upper bound on digits after the decimal point in generated tags.
	// Per-field caps still apply (position/scale 4, rotation 5, shear 6).
	int maximum_decimals = kMaxPerspectiveDecimalPlaces;
	// Permitted model simplification, independently of numeric precision.
	// This selects the economy band; a larger unavoidable error is still
	// returned for the existing preview to display.
	double shape_tolerance = 0.1;
	// The ASS layer measures actual rewritten output after style inheritance
	// and resets. Absence uses changed-field cost for standalone geometry use;
	// a null result rejects a candidate that the writer cannot represent.
	std::function<std::optional<PerspectiveTagCost>(
		EvaluatedTransformState const&, SerializedTransformState const&,
		PerspectiveScalePolicy)>
		measure_tag_cost;
	std::optional<EvaluatedTransformState> event_style;
};

enum class ResidualError {
	None,
	InvalidCandidate,
	InvalidTarget,
	InvalidOutputMapping,
	ProjectionDomain,
};

struct ResidualResult {
	ResidualError error = ResidualError::None;
	GeometryError geometry_error = GeometryError::None;
	ForwardError forward_error = ForwardError::None;
	double max_error = 0.0;

	explicit operator bool() const { return error == ResidualError::None; }
};

enum class SolverError {
	None,
	InvalidSource,
	InvalidTarget,
	InvalidOutputMapping,
	InvalidErrorBudget,
	NoFeasibleCandidate,
};

// Where every rejected candidate died, when the solver reports
// NoFeasibleCandidate. The stages name the option that would actually unblock
// the target: the representation policy is the Fax + Frz Only switch, an edge
// anchor conflict is resolved by releasing the anchored edge, the model stage
// means nothing could be projected or measured at all, and the quantization
// stage is bounded by the decimal places setting.
enum class NoFeasibleReason {
	None,
	RepresentationPolicy,
	ModelResidual,
	Quantization,
	EdgeAnchor,
};

// How close the best rejected candidate's digits got, when the solver reports
// NoFeasibleCandidate: best_error is the rounding error the written digits
// left in output pixels and budget is the serialization bound they blew. The
// gap between the two is what the Decimal Places option has to grow by, which
// is why both travel together.
struct NoFeasibleMetrics {
	double best_error = 0.0;
	double budget = 0.0;
};

struct SolverResult {
	SolverError error = SolverError::None;
	GeometryError geometry_error = GeometryError::None;
	ForwardError forward_error = ForwardError::None;
	std::optional<SolverCandidate> candidate;
	std::size_t considered_candidates = 0;
	// The quad the winning candidate actually aimed at. Under Preserve every
	// family aims at the drawn quad rescaled about its centroid to the area the
	// pinned scale can produce, so the reported quad is where the emitted tags
	// are expected to land rather than where the user drew. Under Fit it is the
	// drawn quad itself. Downstream
	// verification must use this rather than the drawn quad, or it would
	// re-introduce the very size error the policy told the solver to ignore.
	// See PreserveAreaFactor in the solver.
	Quad effective_target {};
	// Only meaningful when error is NoFeasibleCandidate: the stage every
	// candidate died at, which the UI diagnostic turns into the option the
	// user should relax. None on success and for input errors.
	NoFeasibleReason no_feasible_reason = NoFeasibleReason::None;
	// Also only for NoFeasibleCandidate: the closest measurable digits-stage
	// rejection. Absent when nothing measurable died there -- model deaths are
	// projection failures and carry no distance, and representation-policy
	// deaths happen at the family filter before anything ran.
	std::optional<NoFeasibleMetrics> no_feasible_metrics;

	explicit operator bool() const { return error == SolverError::None && candidate.has_value(); }
};

[[nodiscard]] char const* DescribeSolverError(SolverError error);
[[nodiscard]] char const* DescribeResidualError(ResidualError error);
[[nodiscard]] std::string FormatAssNumber(double value, int maximum_decimals);
[[nodiscard]] std::string FormatAssPoint(Vec2 point, int maximum_decimals);
// True when the state itself uses no tag outside the restricted subset.
[[nodiscard]] bool MatchesPerspectiveRepresentationPolicy(
	EvaluatedTransformState const& state,
	PerspectiveRepresentationPolicy policy);
// True when the result is either clean by the rule above, or leaves every
// restricted tag exactly as the source had it. A restricted policy limits what
// the tool may *introduce* -- pos, scale, fax and frz stay available while fay,
// frx, fry and org may not be added or retuned -- so enabling the switch on a
// line that already carries perspective tags is not an instant dead end.
[[nodiscard]] bool MatchesPerspectiveRepresentationPolicy(
	EvaluatedTransformState const& source,
	EvaluatedTransformState const& state,
	PerspectiveRepresentationPolicy policy);
// Measures a re-evaluated candidate against a target quad in final output
// pixels. The candidate's bounds supply all source-space residual samples.
[[nodiscard]] ResidualResult MeasurePerspectiveResidual(
	ForwardInput const& candidate,
	Quad const& target,
	OutputCoordinateMapping output_mapping);
[[nodiscard]] SolverResult SolvePerspectiveTags(SolverInput const& input);

}
