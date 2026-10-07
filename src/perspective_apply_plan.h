#pragma once

#include "perspective_ass_bounds.h"
#include "perspective_ass_state.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class AssDialogue;
class AssFile;

namespace perspective {

// Repository-local host planning. This contains no Legacy Perspective code.

struct PerspectiveApplyContext {
	int frame_number = -1;
	std::int64_t capture_time_ms = 0;
	Resolution play_resolution;
	std::optional<Resolution> layout_resolution;
	std::optional<Resolution> video_storage_resolution;
	OutputCoordinateMapping output_mapping;
};

struct PerspectiveDialogueFingerprint {
	int id = 0;
	int row = -1;
	bool comment = false;
	int layer = 0;
	std::array<int, 3> margins {};
	std::int64_t start_ms = 0;
	std::int64_t end_ms = 0;
	std::string style;
	std::string actor;
	std::string effect;
	std::vector<std::uint32_t> extradata_ids;
	std::string text;
};

struct PerspectiveStyleFingerprint {
	std::string entry_data;
	std::string name;
	std::string font;
	double font_size = 0.0;
	bool bold = false;
	bool italic = false;
	bool underline = false;
	bool strikeout = false;
	double scale_x = 0.0;
	double scale_y = 0.0;
	double spacing = 0.0;
	double angle = 0.0;
	int border_style = 0;
	double outline_width = 0.0;
	double shadow_width = 0.0;
	int alignment = 0;
	std::array<int, 3> margins {};
	int encoding = 0;
};

struct PerspectiveSourceFingerprint {
	// Identity tokens are compared only and are never converted back to or
	// dereferenced as pointers. This catches object replacement without making
	// a mutation plan retain a live AssFile/AssDialogue pointer.
	std::uintptr_t file_identity = 0;
	std::uintptr_t line_identity = 0;
	PerspectiveApplyContext context;
	PerspectiveDialogueFingerprint line;
	// Script Info remains conservative because multiple entries affect the
	// renderer context. Only the event's resolved Style contributes geometry;
	// unrelated styles must not invalidate an in-progress edit.
	std::vector<std::string> script_info_entries;
	std::optional<PerspectiveStyleFingerprint> event_style;
};

struct PerspectiveSourceSnapshot {
	PerspectiveSourceFingerprint fingerprint;
	EffectiveAssState state;
	ForwardInput forward_input;
	// The current subtitle transform is only an optional editing aid. Capture
	// and Apply remain available when it cannot be projected.
	std::optional<Quad> current_quad;
	AssApplyBlocker apply_blocker = AssApplyBlocker::None;
};

// The renderer-facing \frz lock for vertical CJK faces, or nullopt when the
// line's rotation is free. Exposed so a live preview can build the same
// SolverInput that Apply will build; a preview that disagreed with Apply about
// the lock would show a shape Apply then refuses.
[[nodiscard]] std::optional<double> PerspectiveRotationLock(
	EffectiveAssState const& state);

// Resolves the output-pixel direction of the source rectangle's first edge.
// Vertical-font semantics win so strong shears cannot change the new frame's
// corner correspondence; ordinary text follows a valid projected quad.
[[nodiscard]] std::optional<Vec2> PerspectiveFirstEdgeDirection(
	PerspectiveSourceSnapshot const& source);

// Preview and Apply share source semantics and exact rewrite costs. The
// caller supplies the target, output mapping and selected solve policies.
void PreparePerspectiveSolverInput(
	PerspectiveSourceSnapshot const& source, SolverInput& input);

enum class PerspectivePlanError {
	None,
	InvalidInput,
	InvalidContext,
	SourceNotInFile,
	SourceNotFound,
	DuplicateSourceId,
	StaleSource,
	InvalidTarget,
	InvalidErrorBudget,
	StateEvaluationFailed,
	ApplyBlocked,
	BoundsEvaluationFailed,
	ForwardEvaluationFailed,
	SolverFailed,
	RewriteFailed,
	NoChange,
	StagedStateEvaluationFailed,
	StagedApplyBlocked,
	StagedScaleMismatch,
	StagedRepresentationMismatch,
	StagedBoundsEvaluationFailed,
	StagedBoundsMismatch,
	StagedForwardEvaluationFailed,
	ResidualFailed,
	ResidualExceeded,
};

struct PerspectivePlanDiagnostic {
	PerspectivePlanError error = PerspectivePlanError::None;
	GeometryError geometry_error = GeometryError::None;
	AssStateError state_error = AssStateError::None;
	AssApplyBlocker apply_blocker = AssApplyBlocker::None;
	AssBoundsError bounds_error = AssBoundsError::None;
	ForwardError forward_error = ForwardError::None;
	SolverError solver_error = SolverError::None;
	// Stage the solver's candidates died at when solver_error is
	// NoFeasibleCandidate; the UI uses it to name the option to relax.
	NoFeasibleReason solver_no_feasible_reason = NoFeasibleReason::None;
	RewriteError rewrite_error = RewriteError::None;
	ResidualError residual_error = ResidualError::None;
	// Face the bounds evaluation could not measure, when bounds_error is
	// FontUnavailable; names the actual font instead of a generic refusal.
	std::string bounds_font;
	// Distance the closest rejected candidate achieved and the budget it was
	// judged against, when solver_error is NoFeasibleCandidate and the
	// rejection was measurable; the UI turns the gap into the option value
	// that would have accepted the candidate.
	std::optional<NoFeasibleMetrics> solver_no_feasible_metrics;
};

struct PerspectiveCaptureResult : PerspectivePlanDiagnostic {
	std::optional<PerspectiveSourceSnapshot> source;

	explicit operator bool() const {
		return error == PerspectivePlanError::None && source.has_value();
	}
};

struct PerspectivePlanResult;

class PerspectiveMutationPlan {
	PerspectiveSourceFingerprint source_;
	std::string replacement_text_;
	SolverCandidate candidate_;
	double max_error_ = 0.0;
	Quad result_quad_ {};

	PerspectiveMutationPlan(
		PerspectiveSourceFingerprint source,
		std::string replacement_text,
		SolverCandidate candidate,
		double max_error,
		Quad result_quad);

	friend struct PerspectivePlanResult;
	friend PerspectivePlanResult BuildPerspectiveMutationPlan(
		AssFile const&,
		PerspectiveApplyContext const&,
		PerspectiveSourceSnapshot const&,
		Quad const&,
		double,
		AssTextExtentsProvider,
		PerspectiveScalePolicy,
		PerspectiveRepresentationPolicy,
		int,
		PerspectiveEdgeAnchor,
		double,
		bool);

	public:
	PerspectiveMutationPlan(PerspectiveMutationPlan const&) = default;
	PerspectiveMutationPlan(PerspectiveMutationPlan&&) noexcept = default;
	PerspectiveMutationPlan& operator=(PerspectiveMutationPlan const&) = default;
	PerspectiveMutationPlan& operator=(PerspectiveMutationPlan&&) noexcept = default;

	[[nodiscard]] int LineId() const { return source_.line.id; }
	[[nodiscard]] PerspectiveSourceFingerprint const& Source() const { return source_; }
	[[nodiscard]] std::string const& ReplacementText() const { return replacement_text_; }
	[[nodiscard]] SolverCandidate const& Candidate() const { return candidate_; }
	[[nodiscard]] CandidateFamily Family() const { return candidate_.family; }
	[[nodiscard]] double MaxError() const { return max_error_; }
	// Where the emitted tags actually land, re-evaluated from the staged line
	// rather than predicted by the solver. Equals the drawn quad within
	// max_error unless the model had to snap; drawing this is what lets a user
	// see the reachable shape instead of only being told the drag was refused.
	[[nodiscard]] Quad const& ResultQuad() const { return result_quad_; }
	[[nodiscard]] double SnapError() const { return candidate_.snap_error; }
	[[nodiscard]] bool Snapped() const { return candidate_.Snapped(); }
};

struct PerspectivePlanResult : PerspectivePlanDiagnostic {
	std::optional<PerspectiveMutationPlan> plan;

	explicit operator bool() const {
		return error == PerspectivePlanError::None && plan.has_value();
	}
};

struct PerspectiveExecutionResult : PerspectivePlanDiagnostic {
	AssDialogue* line = nullptr;

	explicit operator bool() const {
		return error == PerspectivePlanError::None && line != nullptr;
	}
};

[[nodiscard]] char const* DescribePerspectivePlanError(PerspectivePlanError error);

[[nodiscard]] PerspectiveCaptureResult CapturePerspectiveSource(
	AssFile const& file,
	AssDialogue const& line,
	PerspectiveApplyContext const& context,
	AssTextExtentsProvider text_extents = nullptr);

[[nodiscard]] bool MatchesPerspectiveSource(
	AssFile const& file,
	PerspectiveApplyContext const& context,
	PerspectiveSourceFingerprint const& expected);

// Builds and verifies a complete replacement without mutating the AssFile.
[[nodiscard]] PerspectivePlanResult BuildPerspectiveMutationPlan(
	AssFile const& file,
	PerspectiveApplyContext const& current_context,
	PerspectiveSourceSnapshot const& expected_source,
	Quad const& target,
	double max_error = 0.1,
	AssTextExtentsProvider text_extents = nullptr,
	PerspectiveScalePolicy scale_policy = PerspectiveScalePolicy::Fit,
	PerspectiveRepresentationPolicy representation_policy =
		PerspectiveRepresentationPolicy::Automatic,
	int maximum_decimals = kMaxPerspectiveDecimalPlaces,
	// Which drawn edge to hold exactly when the model has to give something up.
	// See PerspectiveEdgeAnchor.
	PerspectiveEdgeAnchor edge_anchor = PerspectiveEdgeAnchor::None,
	double shape_tolerance = 0.1,
	bool best_fit = false);

// Revalidates immediately before assigning Text. Commit ownership stays with
// the host so it can supply the exact returned line as its changed-lines span.
[[nodiscard]] PerspectiveExecutionResult ExecutePerspectiveMutationPlan(
	AssFile& file,
	PerspectiveApplyContext const& current_context,
	PerspectiveMutationPlan const& plan);

}
