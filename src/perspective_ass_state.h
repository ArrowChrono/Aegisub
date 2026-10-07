#pragma once

#include "ass_file.h"
#include "perspective_tag_solver.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace perspective {

// This evaluator is repository-local ASS host logic. It deliberately does not
// copy the Legacy Perspective implementation or its source notice.

struct AssStateInput {
	AssFile const* file = nullptr;
	AssDialogue const* line = nullptr;
	Resolution play_resolution;
	// Absolute script time in milliseconds. The resulting transform carries
	// the event-relative value expected by renderer-side ASS evaluation.
	std::int64_t capture_time_ms = 0;
};

enum class AssStateError {
	None,
	InvalidInput,
	InvalidPlayResolution,
	MissingEventStyle,
	MissingResetStyle,
	InvalidCaptureTime,
	InvalidGeometryParameter,
	InvalidAlignment,
	NonFiniteStyle,
	MixedGeometryRuns,
};

enum class AssApplyBlocker {
	None,
	UnsupportedMove,
	UnsupportedGeometryAnimation,
	UnsupportedNamedReset,
};

struct EffectiveAssState {
	EvaluatedTransformState transform;
	// Geometry inherited from the event line's resolved Style before applying
	// any override tags. Perspective rewrite uses this one baseline for the
	// initial run and bare \r resets; named resets are evaluated read-only and
	// block Apply.
	EvaluatedTransformState event_style_transform;
	struct TextStyle {
		std::string font_name;
		double font_size = 0.0;
		double spacing = 0.0;
		int font_weight = 400;
		bool italic = false;
		bool underline = false;
		bool strikeout = false;
		int encoding = 1;
		int wrap_style = 0;
		// Horizontal PlayRes space available after resolving event/style margins.
		// Text bounds use this to reject layouts which would require automatic
		// renderer wrapping, since wrapping makes the base rectangle depend on
		// the transform being solved.
		double available_wrap_width = 0.0;
	} text_style;
	bool drawing_mode = false;
	int drawing_scale = 0;
	double drawing_baseline_offset = 0.0;
	std::size_t geometry_run_count = 0;
};

struct AssStateResult {
	AssStateError error = AssStateError::None;
	AssApplyBlocker apply_blocker = AssApplyBlocker::None;
	EffectiveAssState value;

	explicit operator bool() const { return error == AssStateError::None; }
	bool CanApply() const {
		return error == AssStateError::None
			&& apply_blocker == AssApplyBlocker::None;
	}
};

[[nodiscard]] char const* DescribeAssStateError(AssStateError error);
[[nodiscard]] char const* DescribeAssApplyBlocker(AssApplyBlocker blocker);
[[nodiscard]] AssStateResult EvaluateEffectiveAssState(AssStateInput const& input);

// A rewrite is built from one parsed source string and returns a complete new
// string. Unrelated override tags and their original spelling remain byte-for-
// byte unchanged; only the geometry bundle is removed/inserted.
enum class RewriteError {
	None,
	InvalidInput,
	InvalidTarget,
	UnsupportedMove,
	UnsupportedGeometryAnimation,
	UnsupportedNamedReset,
	InvalidGeometryParameter,
};

struct RewriteResult {
	RewriteError error = RewriteError::None;
	std::string text;
	bool changed = false;
	PerspectiveTagCost cost;

	explicit operator bool() const { return error == RewriteError::None; }
};

// Parse once per solve; candidate scoring and the final rewrite share the
// same style-elision and reset rules. Copies retain the immutable source.
class PerspectiveTagRewriter {
	struct Data;
	std::shared_ptr<Data const> data;

	public:
	PerspectiveTagRewriter(std::string_view source_text,
						   EvaluatedTransformState const& source_state,
						   EvaluatedTransformState const& event_style_state);
	[[nodiscard]] RewriteResult Rewrite(
		EvaluatedTransformState const& target,
		SerializedTransformState const& serialized,
		PerspectiveScalePolicy scale_policy,
		PerspectiveRepresentationPolicy representation_policy,
		bool best_fit = false,
		bool preserve_rotation_z = false) const;
};

[[nodiscard]] char const* DescribeRewriteError(RewriteError error);
[[nodiscard]] RewriteResult RewritePerspectiveTags(
	std::string_view source_text,
	EvaluatedTransformState const& source_state,
	EvaluatedTransformState const& event_style_state,
	SolverCandidate const& candidate,
	PerspectiveScalePolicy scale_policy = PerspectiveScalePolicy::Fit,
	// Under a restricted policy the candidate either drops the tags outside
	// the subset or keeps them exactly as the source carries them. The kept
	// form must survive as the original spelling -- the only text that
	// round-trips the value bit for bit -- instead of being re-serialized
	// through the decimal caps.
	PerspectiveRepresentationPolicy representation_policy =
		PerspectiveRepresentationPolicy::Automatic,
	bool best_fit = false,
	bool preserve_rotation_z = false);
}
