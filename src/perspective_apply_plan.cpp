#include "perspective_apply_plan.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_style.h"
#include "ass_style_resolution.h"

#include <cmath>
#include <utility>

namespace perspective {
namespace {

bool FinitePositive(double value) {
	return std::isfinite(value) && value > 0.0 && value <= MaxAbsCoordinate;
}

bool ValidResolution(Resolution value) {
	return FinitePositive(value.width) && FinitePositive(value.height);
}

bool ValidContext(PerspectiveApplyContext const& context) {
	return context.frame_number >= 0
		&& ValidResolution(context.play_resolution)
		&& (!context.layout_resolution
			|| ValidResolution(*context.layout_resolution))
		&& (!context.video_storage_resolution
			|| ValidResolution(*context.video_storage_resolution))
		&& FinitePositive(context.output_mapping.scale_x)
		&& FinitePositive(context.output_mapping.scale_y);
}

bool SameResolution(Resolution left, Resolution right) {
	return left.width == right.width && left.height == right.height;
}

bool SameOptionalResolution(
	std::optional<Resolution> const& left,
	std::optional<Resolution> const& right) {
	if (left.has_value() != right.has_value())
		return false;
	return !left || SameResolution(*left, *right);
}

bool SameContext(
	PerspectiveApplyContext const& left,
	PerspectiveApplyContext const& right) {
	return left.frame_number == right.frame_number
		&& left.capture_time_ms == right.capture_time_ms
		&& SameResolution(left.play_resolution, right.play_resolution)
		&& SameOptionalResolution(left.layout_resolution, right.layout_resolution)
		&& SameOptionalResolution(
			left.video_storage_resolution, right.video_storage_resolution)
		&& left.output_mapping.scale_x == right.output_mapping.scale_x
		&& left.output_mapping.scale_y == right.output_mapping.scale_y;
}

PerspectiveDialogueFingerprint FingerprintDialogue(AssDialogue const& line) {
	return {
		line.Id,
		line.Row,
		line.Comment,
		line.Layer,
		line.Margin,
		line.Start.GetMillisecond(),
		line.End.GetMillisecond(),
		line.Style.get(),
		line.Actor.get(),
		line.Effect.get(),
		line.ExtradataIds.get(),
		line.Text.get(),
	};
}

PerspectiveStyleFingerprint FingerprintStyle(AssStyle const& style) {
	return {
		style.GetEntryData(),
		style.name,
		style.font,
		style.fontsize,
		style.bold,
		style.italic,
		style.underline,
		style.strikeout,
		style.scalex,
		style.scaley,
		style.spacing,
		style.angle,
		style.borderstyle,
		style.outline_w,
		style.shadow_w,
		style.alignment,
		style.Margin,
		style.encoding,
	};
}

bool SameStyle(
	PerspectiveStyleFingerprint const& left,
	PerspectiveStyleFingerprint const& right) {
	return left.entry_data == right.entry_data
		&& left.name == right.name
		&& left.font == right.font
		&& left.font_size == right.font_size
		&& left.bold == right.bold
		&& left.italic == right.italic
		&& left.underline == right.underline
		&& left.strikeout == right.strikeout
		&& left.scale_x == right.scale_x
		&& left.scale_y == right.scale_y
		&& left.spacing == right.spacing
		&& left.angle == right.angle
		&& left.border_style == right.border_style
		&& left.outline_width == right.outline_width
		&& left.shadow_width == right.shadow_width
		&& left.alignment == right.alignment
		&& left.margins == right.margins
		&& left.encoding == right.encoding;
}

bool SameOptionalStyle(
	std::optional<PerspectiveStyleFingerprint> const& left,
	std::optional<PerspectiveStyleFingerprint> const& right) {
	if (left.has_value() != right.has_value())
		return false;
	return !left || SameStyle(*left, *right);
}

bool SameDialogue(
	PerspectiveDialogueFingerprint const& left,
	PerspectiveDialogueFingerprint const& right) {
	return left.id == right.id
		&& left.row == right.row
		&& left.comment == right.comment
		&& left.layer == right.layer
		&& left.margins == right.margins
		&& left.start_ms == right.start_ms
		&& left.end_ms == right.end_ms
		&& left.style == right.style
		&& left.actor == right.actor
		&& left.effect == right.effect
		&& left.extradata_ids == right.extradata_ids
		&& left.text == right.text;
}

PerspectiveSourceFingerprint FingerprintSource(
	AssFile const& file,
	AssDialogue const& line,
	PerspectiveApplyContext const& context) {
	PerspectiveSourceFingerprint result;
	result.file_identity = reinterpret_cast<std::uintptr_t>(&file);
	result.line_identity = reinterpret_cast<std::uintptr_t>(&line);
	result.context = context;
	result.line = FingerprintDialogue(line);
	result.script_info_entries.reserve(file.Info.size());
	for (auto const& info : file.Info)
		result.script_info_entries.push_back(info.GetEntryData());
	if (auto const* style = aegisub::ass_style_resolution::ResolveEventStyle(
		file, line.Style.get()))
		result.event_style = FingerprintStyle(*style);
	return result;
}

bool SameFingerprint(
	PerspectiveSourceFingerprint const& left,
	PerspectiveSourceFingerprint const& right) {
	return left.file_identity == right.file_identity
		&& left.line_identity == right.line_identity
		&& SameContext(left.context, right.context)
		&& SameDialogue(left.line, right.line)
		&& left.script_info_entries == right.script_info_entries
		&& SameOptionalStyle(left.event_style, right.event_style);
}

// Field-by-field equivalent of SameFingerprint(FingerprintSource(...), expected)
// without building any temporary copies: staleness checks run on every Apply
// and on every rebind, so they must not deep-copy the whole script info table,
// event style and line text each time.
bool FingerprintMatchesInPlace(
	AssFile const& file,
	AssDialogue const& line,
	PerspectiveApplyContext const& context,
	PerspectiveSourceFingerprint const& expected) {
	if (reinterpret_cast<std::uintptr_t>(&file) != expected.file_identity
		|| reinterpret_cast<std::uintptr_t>(&line) != expected.line_identity
		|| !SameContext(context, expected.context)
		|| line.Id != expected.line.id
		|| line.Row != expected.line.row
		|| line.Comment != expected.line.comment
		|| line.Layer != expected.line.layer
		|| line.Margin != expected.line.margins
		|| line.Start.GetMillisecond() != expected.line.start_ms
		|| line.End.GetMillisecond() != expected.line.end_ms
		|| line.Style.get() != expected.line.style
		|| line.Actor.get() != expected.line.actor
		|| line.Effect.get() != expected.line.effect
		|| line.ExtradataIds.get() != expected.line.extradata_ids
		|| line.Text.get() != expected.line.text)
		return false;
	if (file.Info.size() != expected.script_info_entries.size())
		return false;
	for (std::size_t index = 0; index < file.Info.size(); ++index) {
		if (file.Info[index].GetEntryData() != expected.script_info_entries[index])
			return false;
	}
	auto const* style = aegisub::ass_style_resolution::ResolveEventStyle(
		file, line.Style.get());
	if (!style != !expected.event_style)
		return false;
	if (!style)
		return true;
	return style->GetEntryData() == expected.event_style->entry_data
		&& style->name == expected.event_style->name
		&& style->font == expected.event_style->font
		&& style->fontsize == expected.event_style->font_size
		&& style->bold == expected.event_style->bold
		&& style->italic == expected.event_style->italic
		&& style->underline == expected.event_style->underline
		&& style->strikeout == expected.event_style->strikeout
		&& style->scalex == expected.event_style->scale_x
		&& style->scaley == expected.event_style->scale_y
		&& style->spacing == expected.event_style->spacing
		&& style->angle == expected.event_style->angle
		&& style->borderstyle == expected.event_style->border_style
		&& style->outline_w == expected.event_style->outline_width
		&& style->shadow_w == expected.event_style->shadow_width
		&& style->alignment == expected.event_style->alignment
		&& style->Margin == expected.event_style->margins
		&& style->encoding == expected.event_style->encoding;
}

bool SamePoint(Vec2 left, Vec2 right) {
	return left.x == right.x && left.y == right.y;
}

bool SameBounds(BaseBounds const& left, BaseBounds const& right) {
	if (left.rectangle.left != right.rectangle.left
		|| left.rectangle.top != right.rectangle.top
		|| left.rectangle.right != right.rectangle.right
		|| left.rectangle.bottom != right.rectangle.bottom
		|| left.kind != right.kind
		|| left.alignment_extent.has_value() != right.alignment_extent.has_value()
		|| !SamePoint(left.alignment_offset, right.alignment_offset)
		|| left.residual_samples.size() != right.residual_samples.size())
		return false;
	if (left.alignment_extent
		&& !SameResolution(*left.alignment_extent, *right.alignment_extent))
		return false;
	for (std::size_t index = 0; index < left.residual_samples.size(); ++index) {
		if (!SamePoint(left.residual_samples[index], right.residual_samples[index]))
			return false;
	}
	return true;
}

template<typename File>
auto ResolveUniqueLine(File& file, int line_id, PerspectivePlanError& error)
	-> decltype(&file.Events.front()) {
	decltype(&file.Events.front()) found = nullptr;
	for (auto& line : file.Events) {
		if (line.Id != line_id)
			continue;
		if (found) {
			error = PerspectivePlanError::DuplicateSourceId;
			return nullptr;
		}
		found = &line;
	}
	if (!found)
		error = PerspectivePlanError::SourceNotFound;
	return found;
}

bool PointerIsUniqueLiveLine(AssFile const& file, AssDialogue const& expected) {
	AssDialogue const* pointer_match = nullptr;
	int id_matches = 0;
	for (auto const& line : file.Events) {
		if (&line == &expected)
			pointer_match = &line;
		if (line.Id == expected.Id)
			++id_matches;
	}
	return pointer_match == &expected && id_matches == 1;
}

PerspectiveCaptureResult CaptureFailure(PerspectivePlanError error) {
	PerspectiveCaptureResult result;
	result.error = error;
	return result;
}

PerspectivePlanResult PlanFailure(PerspectivePlanError error) {
	PerspectivePlanResult result;
	result.error = error;
	return result;
}

PerspectiveExecutionResult ExecutionFailure(PerspectivePlanError error) {
	PerspectiveExecutionResult result;
	result.error = error;
	return result;
}

}

std::optional<double> PerspectiveRotationLock(EffectiveAssState const& state) {
	if (state.drawing_mode
		|| state.text_style.font_name.empty()
		|| state.text_style.font_name.front() != '@')
		return std::nullopt;
	double rotation = state.transform.rotation_z;
	if (!std::isfinite(rotation))
		return std::nullopt;
	if (std::abs(rotation) > MaxTransformParameter) {
		rotation = std::fmod(rotation, 360.0);
		if (rotation < 0.0)
			rotation += 360.0;
	}
	return rotation;
}

PerspectiveMutationPlan::PerspectiveMutationPlan(
	PerspectiveSourceFingerprint source,
	std::string replacement_text,
	SolverCandidate candidate,
	double max_error,
	Quad result_quad)
: source_(std::move(source))
, replacement_text_(std::move(replacement_text))
, candidate_(std::move(candidate))
, max_error_(max_error)
, result_quad_(result_quad) {
}

std::optional<Vec2> PerspectiveFirstEdgeDirection(
	PerspectiveSourceSnapshot const& source) {
	std::optional<Vec2> direction;
	if (auto const rotation = PerspectiveRotationLock(source.state)) {
		double const radians = -*rotation
			* 3.1415926535897932384626433832795 / 180.0;
		direction = Vec2 {std::cos(radians), std::sin(radians)};
	}
	else if (source.current_quad && ValidateQuad(*source.current_quad)) {
		direction = (*source.current_quad)[1] - (*source.current_quad)[0];
	}
	if (!direction)
		return std::nullopt;
	if (std::abs(direction->x) <= 1.0e-12)
		direction->x = 0.0;
	if (std::abs(direction->y) <= 1.0e-12)
		direction->y = 0.0;
	return Vec2 {
		direction->x * source.fingerprint.context.output_mapping.scale_x,
		direction->y * source.fingerprint.context.output_mapping.scale_y};
}

char const* DescribePerspectivePlanError(PerspectivePlanError error) {
	switch (error) {
		case PerspectivePlanError::None: return "Perspective mutation plan succeeded";
		case PerspectivePlanError::InvalidInput: return "Perspective plan input is incomplete";
		case PerspectivePlanError::InvalidContext: return "Perspective capture context is invalid";
		case PerspectivePlanError::SourceNotInFile: return "Perspective source is not a live event in this file";
		case PerspectivePlanError::SourceNotFound: return "Perspective source event no longer exists";
		case PerspectivePlanError::DuplicateSourceId: return "Perspective source event Id is not unique";
		case PerspectivePlanError::StaleSource: return "Perspective source or capture context changed";
		case PerspectivePlanError::InvalidTarget: return "Perspective target quad is invalid";
		case PerspectivePlanError::InvalidErrorBudget: return "Perspective output error budget is invalid";
		case PerspectivePlanError::StateEvaluationFailed: return "Perspective source state could not be evaluated";
		case PerspectivePlanError::ApplyBlocked: return "Perspective Apply is blocked for this source";
		case PerspectivePlanError::BoundsEvaluationFailed: return "Perspective source bounds could not be evaluated";
		case PerspectivePlanError::ForwardEvaluationFailed: return "Perspective source transform could not be evaluated";
		case PerspectivePlanError::SolverFailed: return "Perspective tags could not represent the target";
		case PerspectivePlanError::RewriteFailed: return "Perspective tags could not be rewritten";
		case PerspectivePlanError::NoChange: return "Perspective target requires no ASS change";
		case PerspectivePlanError::StagedStateEvaluationFailed: return "rewritten Perspective state could not be evaluated";
		case PerspectivePlanError::StagedApplyBlocked: return "rewritten Perspective state is not applyable";
		case PerspectivePlanError::StagedScaleMismatch: return "rewritten Perspective state changed the preserved text scale";
		case PerspectivePlanError::StagedRepresentationMismatch: return "rewritten Perspective state violates the selected representation policy";
		case PerspectivePlanError::StagedBoundsEvaluationFailed: return "rewritten Perspective bounds could not be evaluated";
		case PerspectivePlanError::StagedBoundsMismatch: return "rewritten Perspective source bounds changed";
		case PerspectivePlanError::StagedForwardEvaluationFailed: return "rewritten Perspective transform could not be evaluated";
		case PerspectivePlanError::ResidualFailed: return "rewritten Perspective residual could not be measured";
		case PerspectivePlanError::ResidualExceeded: return "rewritten Perspective output exceeds the error budget";
	}
	return "unknown Perspective mutation plan error";
}

PerspectiveCaptureResult CapturePerspectiveSource(
	AssFile const& file,
	AssDialogue const& line,
	PerspectiveApplyContext const& context,
	AssTextExtentsProvider text_extents) {
	if (!ValidContext(context))
		return CaptureFailure(PerspectivePlanError::InvalidContext);
	if (line.Id <= 0)
		return CaptureFailure(PerspectivePlanError::InvalidInput);
	if (!PointerIsUniqueLiveLine(file, line)) {
		PerspectivePlanError resolve_error = PerspectivePlanError::None;
		auto const* same_id = ResolveUniqueLine(file, line.Id, resolve_error);
		if (resolve_error == PerspectivePlanError::DuplicateSourceId)
			return CaptureFailure(resolve_error);
		(void)same_id;
		return CaptureFailure(PerspectivePlanError::SourceNotInFile);
	}

	auto const state = EvaluateEffectiveAssState({
		&file, &line, context.play_resolution, context.capture_time_ms});
	if (!state) {
		PerspectiveCaptureResult result;
		result.error = PerspectivePlanError::StateEvaluationFailed;
		result.state_error = state.error;
		result.apply_blocker = state.apply_blocker;
		return result;
	}
	ForwardInput input;
	input.play_resolution = context.play_resolution;
	input.layout_resolution = context.layout_resolution;
	input.video_storage_resolution = context.video_storage_resolution;
	input.state = state.value.transform;
	auto const layout_aspect = ResolvePerspectiveLayoutAspect(input);
	if (!layout_aspect)
		return CaptureFailure(PerspectivePlanError::InvalidContext);
	auto bounds = EvaluateAssBaseBounds({.line = &line, .state = &state.value, .text_extents = text_extents, .layout_aspect = *layout_aspect});
	if (!bounds) {
		PerspectiveCaptureResult result;
		result.error = PerspectivePlanError::BoundsEvaluationFailed;
		result.geometry_error = bounds.geometry_error;
		result.bounds_error = bounds.error;
		result.bounds_font = bounds.font_name;
		return result;
	}

	input.bounds = std::move(bounds.value);
	auto const forward = ForwardQuad(input);

	PerspectiveCaptureResult result;
	result.source = PerspectiveSourceSnapshot{
		.fingerprint = FingerprintSource(file, line, context),
		.state = state.value,
		.forward_input = std::move(input),
		.current_quad = forward ? std::optional<Quad>(forward.quad) : std::nullopt,
		.apply_blocker = state.apply_blocker,
	};
	result.apply_blocker = state.apply_blocker;
	return result;
}

bool MatchesPerspectiveSource(
	AssFile const& file,
	PerspectiveApplyContext const& context,
	PerspectiveSourceFingerprint const& expected) {
	if (!ValidContext(context) || !SameContext(context, expected.context))
		return false;
	PerspectivePlanError resolve_error = PerspectivePlanError::None;
	auto const* line = ResolveUniqueLine(file, expected.line.id, resolve_error);
	return line && resolve_error == PerspectivePlanError::None
		&& FingerprintMatchesInPlace(file, *line, context, expected);
}

void PreparePerspectiveSolverInput(
	PerspectiveSourceSnapshot const& source, SolverInput& input) {
	input.source = source.forward_input;
	input.locked_rotation_z = PerspectiveRotationLock(source.state);
	input.event_style = source.state.event_style_transform;
	if (input.best_fit) {
		input.representation_policy = PerspectiveRepresentationPolicy::Automatic;
		input.edge_anchor = PerspectiveEdgeAnchor::None;
	}
	PerspectiveTagRewriter rewriter(source.fingerprint.line.text,
									source.state.transform, source.state.event_style_transform);
	input.measure_tag_cost = [rewriter = std::move(rewriter),
							  policy = input.representation_policy,
							  best_fit = input.best_fit,
							  preserve_rotation_z = input.best_fit && input.locked_rotation_z.has_value()](EvaluatedTransformState const& state,
																										   SerializedTransformState const& serialized, PerspectiveScalePolicy scale_policy)
		-> std::optional<PerspectiveTagCost> {
		auto const result = rewriter.Rewrite(state, serialized, scale_policy, policy, best_fit, preserve_rotation_z);
		if (!result)
			return std::nullopt;
		return result.cost;
	};
}

PerspectivePlanResult BuildPerspectiveMutationPlan(
	AssFile const& file,
	PerspectiveApplyContext const& current_context,
	PerspectiveSourceSnapshot const& expected_source,
	Quad const& target,
	double max_error,
	AssTextExtentsProvider text_extents,
	PerspectiveScalePolicy scale_policy,
	PerspectiveRepresentationPolicy representation_policy,
	int maximum_decimals,
	PerspectiveEdgeAnchor edge_anchor,
	double shape_tolerance,
	bool best_fit) {
	if (!ValidContext(current_context))
		return PlanFailure(PerspectivePlanError::InvalidContext);
	if (!std::isfinite(max_error) || max_error <= 0.0 || (!best_fit && (!std::isfinite(shape_tolerance) || shape_tolerance <= 0.0)))
		return PlanFailure(PerspectivePlanError::InvalidErrorBudget);

	PerspectivePlanError resolve_error = PerspectivePlanError::None;
	auto const* line = ResolveUniqueLine(
		file, expected_source.fingerprint.line.id, resolve_error);
	if (!line)
		return PlanFailure(resolve_error);
	if (!SameContext(current_context, expected_source.fingerprint.context)
		|| !FingerprintMatchesInPlace(
			file, *line, current_context, expected_source.fingerprint))
		return PlanFailure(PerspectivePlanError::StaleSource);

	auto const target_validation = ValidateQuad(target);
	if (!target_validation) {
		auto result = PlanFailure(PerspectivePlanError::InvalidTarget);
		result.geometry_error = target_validation.error;
		return result;
	}

	auto const captured = CapturePerspectiveSource(
		file, *line, current_context, text_extents);
	if (!captured) {
		PerspectivePlanResult result;
		static_cast<PerspectivePlanDiagnostic&>(result) = captured;
		return result;
	}
	if (!SameFingerprint(captured.source->fingerprint, expected_source.fingerprint))
		return PlanFailure(PerspectivePlanError::StaleSource);
	if (captured.source->apply_blocker != AssApplyBlocker::None) {
		auto result = PlanFailure(PerspectivePlanError::ApplyBlocked);
		result.apply_blocker = captured.source->apply_blocker;
		return result;
	}

	SolverInput solver_input;
	solver_input.target = target;
	solver_input.output_mapping = current_context.output_mapping;
	solver_input.max_error = max_error;
	solver_input.shape_tolerance = shape_tolerance;
	solver_input.best_fit = best_fit;
	solver_input.scale_policy = scale_policy;
	solver_input.representation_policy = representation_policy;
	solver_input.edge_anchor = edge_anchor;
	solver_input.maximum_decimals = ClampPerspectiveDecimalPlaces(maximum_decimals);
	PreparePerspectiveSolverInput(*captured.source, solver_input);
	auto solver = SolvePerspectiveTags(solver_input);
	if (!solver) {
		auto result = PlanFailure(PerspectivePlanError::SolverFailed);
		result.geometry_error = solver.geometry_error;
		result.forward_error = solver.forward_error;
		result.solver_error = solver.error;
		if (solver.error == SolverError::NoFeasibleCandidate) {
			result.solver_no_feasible_reason = solver.no_feasible_reason;
			result.solver_no_feasible_metrics = solver.no_feasible_metrics;
		}
		return result;
	}

	auto rewrite = RewritePerspectiveTags(
		captured.source->fingerprint.line.text,
		captured.source->state.transform,
		captured.source->state.event_style_transform,
		*solver.candidate,
		scale_policy,
		solver_input.representation_policy,
		best_fit,
		best_fit && solver_input.locked_rotation_z.has_value());
	if (!rewrite) {
		auto result = PlanFailure(PerspectivePlanError::RewriteFailed);
		result.rewrite_error = rewrite.error;
		return result;
	}
	if (!rewrite.changed || rewrite.text == captured.source->fingerprint.line.text)
		return PlanFailure(PerspectivePlanError::NoChange);

	AssDialogue staged(static_cast<AssDialogueBase const&>(*line));
	staged.Text = rewrite.text;
	auto const staged_state = EvaluateEffectiveAssState({
		&file, &staged, current_context.play_resolution,
		current_context.capture_time_ms});
	if (!staged_state) {
		auto result = PlanFailure(PerspectivePlanError::StagedStateEvaluationFailed);
		result.state_error = staged_state.error;
		result.apply_blocker = staged_state.apply_blocker;
		return result;
	}
	if (!staged_state.CanApply()) {
		auto result = PlanFailure(PerspectivePlanError::StagedApplyBlocked);
		result.apply_blocker = staged_state.apply_blocker;
		return result;
	}
	// Exact on purpose, and it stays exact. Preserve never re-serializes the
	// scale tags: the solver freezes scale, MakeRewriteDelta refuses any
	// deviation, and the rewrite copies the original \fscx/\fscy through
	// verbatim. So the staged value is the source value bit-for-bit, and
	// loosening this to a rounding step would only hide a future regression in
	// that chain rather than admit any legitimate plan.
	if (scale_policy == PerspectiveScalePolicy::Preserve
		&& (staged_state.value.transform.scale_x
				!= captured.source->state.transform.scale_x
			|| staged_state.value.transform.scale_y
				!= captured.source->state.transform.scale_y))
		return PlanFailure(PerspectivePlanError::StagedScaleMismatch);
	if (!MatchesPerspectiveRepresentationPolicy(
			captured.source->state.transform, staged_state.value.transform,
			solver_input.representation_policy))
		return PlanFailure(PerspectivePlanError::StagedRepresentationMismatch);
	auto const layout_aspect = ResolvePerspectiveLayoutAspect(captured.source->forward_input);
	if (!layout_aspect)
		return PlanFailure(PerspectivePlanError::InvalidContext);
	auto const staged_bounds = EvaluateAssBaseBounds({.line = &staged,
													  .state = &staged_state.value,
													  .text_extents = text_extents,
													  .layout_aspect = *layout_aspect});
	if (!staged_bounds) {
		auto result = PlanFailure(PerspectivePlanError::StagedBoundsEvaluationFailed);
		result.geometry_error = staged_bounds.geometry_error;
		result.bounds_error = staged_bounds.error;
		result.bounds_font = staged_bounds.font_name;
		return result;
	}
	if (!SameBounds(staged_bounds.value, captured.source->forward_input.bounds))
		return PlanFailure(PerspectivePlanError::StagedBoundsMismatch);
	ForwardInput staged_input;
	staged_input.play_resolution = current_context.play_resolution;
	staged_input.layout_resolution = current_context.layout_resolution;
	staged_input.video_storage_resolution = current_context.video_storage_resolution;
	staged_input.bounds = staged_bounds.value;
	staged_input.state = staged_state.value.transform;
	auto const staged_forward = ForwardQuad(staged_input);
	if (!staged_forward) {
		auto result = PlanFailure(PerspectivePlanError::StagedForwardEvaluationFailed);
		result.geometry_error = staged_forward.geometry_error;
		result.forward_error = staged_forward.error;
		return result;
	}

	// Against the quad the solver aimed at, not the drawn one. Under a scale
	// lock those differ by the size the policy told the solver to ignore, and
	// measuring the drawn quad here would reject the plan for exactly the
	// mismatch the solver was instructed not to fix. Identical to the drawn quad
	// in every other configuration.
	auto const residual = MeasurePerspectiveResidual(
		staged_input, solver.effective_target, current_context.output_mapping);
	if (!residual) {
		auto result = PlanFailure(PerspectivePlanError::ResidualFailed);
		result.geometry_error = residual.geometry_error;
		result.forward_error = residual.forward_error;
		result.residual_error = residual.error;
		return result;
	}
	// A similar distance to the target is insufficient: the writer must land
	// on the candidate that the preview showed, including off-target fits.
	auto const predicted = ForwardQuad(captured.source->forward_input, solver.candidate->state);
	if (!predicted)
		return PlanFailure(PerspectivePlanError::StagedForwardEvaluationFailed);
	auto const rewrite_residual = MeasurePerspectiveResidual(
		staged_input, predicted.quad, current_context.output_mapping);
	if (!rewrite_residual || rewrite_residual.max_error > max_error)
		return PlanFailure(PerspectivePlanError::ResidualExceeded);
	// The staged re-derivation must agree with what the solver predicted for
	// the candidate it chose -- a snapped model is expected to miss the drawn
	// quad by its own snap_error, and that shortfall is reported and drawn by
	// the caller. One rounding budget of slack absorbs re-derivation noise;
	// anything beyond that means the rewrite changed the geometry the solver
	// modeled, which is exactly what this check exists to catch.
	double const residual_budget = solver.candidate->max_error + max_error;
	if (residual.max_error > residual_budget)
		return PlanFailure(PerspectivePlanError::ResidualExceeded);

	if (!FingerprintMatchesInPlace(
			file, *line, current_context, captured.source->fingerprint))
		return PlanFailure(PerspectivePlanError::StaleSource);

	PerspectivePlanResult result;
	PerspectiveMutationPlan plan(
		captured.source->fingerprint,
		std::move(rewrite.text),
		std::move(*solver.candidate),
		residual.max_error,
		staged_forward.quad);
	result.plan = std::move(plan);
	return result;
}

PerspectiveExecutionResult ExecutePerspectiveMutationPlan(
	AssFile& file,
	PerspectiveApplyContext const& current_context,
	PerspectiveMutationPlan const& plan) {
	if (!ValidContext(current_context))
		return ExecutionFailure(PerspectivePlanError::InvalidContext);
	PerspectivePlanError resolve_error = PerspectivePlanError::None;
	auto* line = ResolveUniqueLine(file, plan.LineId(), resolve_error);
	if (!line)
		return ExecutionFailure(resolve_error);
	if (!SameContext(current_context, plan.Source().context)
		|| !FingerprintMatchesInPlace(
			file, *line, current_context, plan.Source()))
		return ExecutionFailure(PerspectivePlanError::StaleSource);
	if (line->Text.get() == plan.ReplacementText())
		return ExecutionFailure(PerspectivePlanError::NoChange);

	decltype(line->Text) replacement(plan.ReplacementText());
	if (!FingerprintMatchesInPlace(
			file, *line, current_context, plan.Source()))
		return ExecutionFailure(PerspectivePlanError::StaleSource);
	line->Text = std::move(replacement);

	PerspectiveExecutionResult result;
	result.line = line;
	return result;
}

}
