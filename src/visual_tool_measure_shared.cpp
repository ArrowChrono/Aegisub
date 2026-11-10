#include "visual_tool_measure_shared.h"

#include "translation_service.h"
#include "video_overlay_draw_context.h"

#include <libaegisub/format.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace aegisub::visual_measure {

std::string LocalizedGeometryError(perspective::GeometryError error) {
	using perspective::GeometryError;
	switch (error) {
		case GeometryError::None: return {};
		case GeometryError::NonFinite:
			return _("coordinate is not finite");
		case GeometryError::CoordinateOutOfRange:
			return _("coordinate exceeds the supported range");
		case GeometryError::DuplicatePoint:
			return _("quad contains duplicate points");
		case GeometryError::EdgeTooShort:
			return _("quad edge is too short");
		case GeometryError::AreaTooSmall:
			return _("quad area is too small");
		case GeometryError::SelfIntersecting:
			return _("quad is self-intersecting");
		case GeometryError::WrongWinding:
			return _("quad winding must be TL, TR, BR, BL");
		case GeometryError::NonConvex:
			return _("quad must be strictly convex");
		case GeometryError::InvalidDomain:
			return _("geometry domain is invalid");
		case GeometryError::SingularHomography:
			return _("homography is singular");
		case GeometryError::ProjectionDomain:
			return _("projection denominator crosses or approaches zero");
	}
	return _("unknown geometry error");
}

std::string LocalizedAssStateError(perspective::AssStateError error) {
	using perspective::AssStateError;
	switch (error) {
		case AssStateError::None: return {};
		case AssStateError::InvalidInput:
			return _("Perspective ASS state input is incomplete");
		case AssStateError::InvalidPlayResolution:
			return _("Perspective PlayRes is invalid");
		case AssStateError::MissingEventStyle:
			return _("the dialogue event style cannot be resolved");
		case AssStateError::MissingResetStyle:
			return _("a Perspective-relevant reset style cannot be resolved");
		case AssStateError::InvalidCaptureTime:
			return _("the capture time is outside the dialogue event");
		case AssStateError::InvalidGeometryParameter:
			return _("a Perspective geometry override is invalid");
		case AssStateError::InvalidAlignment:
			return _("the effective ASS alignment is invalid");
		case AssStateError::NonFiniteStyle:
			return _("the effective ASS style contains invalid geometry");
		case AssStateError::MixedGeometryRuns:
			return _("the dialogue has mixed Perspective geometry runs");
	}
	return _("unknown Perspective ASS state error");
}

std::string LocalizedAssApplyBlocker(perspective::AssApplyBlocker blocker) {
	using perspective::AssApplyBlocker;
	switch (blocker) {
		case AssApplyBlocker::None: return {};
		case AssApplyBlocker::UnsupportedMove:
			return _("Perspective Apply does not support \\move");
		case AssApplyBlocker::UnsupportedGeometryAnimation:
			return _("Perspective Apply does not support animated geometry");
		case AssApplyBlocker::UnsupportedNamedReset:
			return _("Perspective Apply does not support named style resets");
	}
	return _("unknown Perspective Apply blocker");
}

std::string LocalizedAssBoundsError(
	perspective::AssBoundsError error,
	std::string const& font = {}) {
	using perspective::AssBoundsError;
	switch (error) {
		case AssBoundsError::None: return {};
		case AssBoundsError::InvalidInput:
			return _("Perspective ASS bounds input is incomplete");
		case AssBoundsError::EmptyGeometry:
			return _("the dialogue has no visible geometry");
		case AssBoundsError::FontUnavailable:
			return font.empty()
					   ? _("renderer-compatible text font bounds are unavailable")
					   : agi::format(_("renderer-compatible bounds are unavailable for the font %s").c_str(), font);
		case AssBoundsError::UnsupportedAutomaticWrap:
			return _("automatic text wrapping does not have stable Perspective base bounds; use explicit line breaks or \\q2");
		case AssBoundsError::MixedGeometryRuns:
			return _("mixed text and drawing runs do not have stable base bounds");
		case AssBoundsError::UnsupportedDrawingLayout:
			return _("multiple ASS drawing runs require unsupported glyph layout");
		case AssBoundsError::UnsupportedDrawingCommand:
			return _("ASS B-spline drawing metrics are not supported");
		case AssBoundsError::DrawingStateMismatch:
			return _("the parsed drawing runs do not match the evaluated ASS state");
		case AssBoundsError::InvalidDrawingSyntax:
			return _("the ASS drawing contains invalid syntax");
		case AssBoundsError::InvalidDrawingGeometry:
			return _("the ASS drawing does not produce measurable geometry");
		case AssBoundsError::InvalidDrawingBounds:
			return _("the ASS drawing bounds are degenerate or outside the supported range");
	}
	return _("unknown Perspective ASS bounds error");
}

std::string LocalizedForwardError(perspective::ForwardError error) {
	using perspective::ForwardError;
	switch (error) {
		case ForwardError::None: return {};
		case ForwardError::UnsupportedMixedBounds:
			return _("mixed text and drawing runs do not have a stable base bounds");
		case ForwardError::UnsupportedDynamicBounds:
			return _("dynamic geometry has no single base bounds");
		case ForwardError::UnsupportedFontBounds:
			return _("font metrics are unavailable for base bounds");
		case ForwardError::InvalidBounds:
			return _("base bounds are invalid");
		case ForwardError::InvalidResolution:
			return _("PlayRes/LayoutRes/video resolution is invalid");
		case ForwardError::InvalidEventTime:
			return _("event-relative evaluation time is invalid");
		case ForwardError::InvalidAlignment:
			return _("alignment must be in the range 1..9");
		case ForwardError::NonFiniteState:
			return _("evaluated ASS state is not finite");
		case ForwardError::TransformParameterOutOfRange:
			return _("transform parameter exceeds the supported range");
		case ForwardError::DegenerateScale:
			return _("scale must not be zero");
		case ForwardError::MirroredTransform:
			return _("mirrored transforms are not supported for ordered quads");
		case ForwardError::SingularTransform:
			return _("shear transform is singular");
		case ForwardError::ProjectionDomain:
			return _("perspective denominator crosses or approaches zero");
		case ForwardError::InvalidQuad:
			return _("forward transform produced an invalid quad");
		case ForwardError::SingularHomography:
			return _("forward homography is singular");
	}
	return _("unknown forward error");
}

std::string LocalizedSolverError(perspective::SolverError error) {
	using perspective::SolverError;
	switch (error) {
		case SolverError::None: return {};
		case SolverError::InvalidSource:
			return _("source Perspective state is invalid");
		case SolverError::InvalidTarget:
			return _("target Perspective quad is invalid");
		case SolverError::InvalidOutputMapping:
			return _("output coordinate mapping is invalid");
		case SolverError::InvalidErrorBudget:
			return _("Perspective error budget is invalid");
		case SolverError::NoFeasibleCandidate:
			return _("no quantized Perspective tag candidate meets the error budget");
	}
	return _("unknown Perspective solver error");
}

std::string LocalizedRewriteError(perspective::RewriteError error) {
	using perspective::RewriteError;
	switch (error) {
		case RewriteError::None: return {};
		case RewriteError::InvalidInput:
			return _("Perspective ASS rewrite input is invalid");
		case RewriteError::InvalidTarget:
			return _("Perspective ASS rewrite target is invalid");
		case RewriteError::UnsupportedMove:
			return _("Perspective ASS rewrite does not support \\move");
		case RewriteError::UnsupportedGeometryAnimation:
			return _("Perspective ASS rewrite does not support animated geometry");
		case RewriteError::UnsupportedNamedReset:
			return _("Perspective ASS rewrite does not support named style resets");
		case RewriteError::InvalidGeometryParameter:
			return _("Perspective ASS rewrite contains invalid geometry");
	}
	return _("unknown Perspective ASS rewrite error");
}

std::string LocalizedResidualError(perspective::ResidualError error) {
	using perspective::ResidualError;
	switch (error) {
		case ResidualError::None: return {};
		case ResidualError::InvalidCandidate:
			return _("candidate Perspective state is invalid");
		case ResidualError::InvalidTarget:
			return _("target Perspective quad is invalid");
		case ResidualError::InvalidOutputMapping:
			return _("output coordinate mapping is invalid");
		case ResidualError::ProjectionDomain:
			return _("Perspective residual sample is outside the projection domain");
	}
	return _("unknown Perspective residual error");
}

std::string LocalizedQuadEditError(perspective::PerspectiveQuadEditError error) {
	using perspective::PerspectiveQuadEditError;
	switch (error) {
		case PerspectiveQuadEditError::None: return {};
		case PerspectiveQuadEditError::NotBound:
			return _("The Perspective quad is not bound to a line.");
		case PerspectiveQuadEditError::NoTarget:
			return _("There is no Perspective target to edit.");
		case PerspectiveQuadEditError::NoCurrentQuad:
			return _("The line's current Perspective quad is unavailable.");
		case PerspectiveQuadEditError::ReadOnly:
			return _("The Perspective target is read-only.");
		case PerspectiveQuadEditError::InvalidSource:
			return _("The Perspective source geometry is invalid.");
		case PerspectiveQuadEditError::InvalidHandle:
			return _("That Perspective handle cannot be dragged.");
		case PerspectiveQuadEditError::GestureAlreadyActive:
			return _("Another Perspective drag is already in progress.");
		case PerspectiveQuadEditError::NoGesture:
			return _("No Perspective drag is in progress.");
		case PerspectiveQuadEditError::InvalidTolerance:
			return _("The Perspective hit tolerance is invalid.");
		case PerspectiveQuadEditError::NonFinitePointer:
			return _("The pointer position is outside the coordinate range.");
		case PerspectiveQuadEditError::CoordinateOutOfRange:
			return _("The Perspective quad would leave the coordinate range.");
	}
	return _("unknown Perspective quad edit error");
}

perspective::PerspectiveEdgeAnchor EdgeAnchorForHandle(
	perspective::PerspectiveQuadHandle handle) {
	using perspective::PerspectiveEdgeAnchor;
	using perspective::PerspectiveQuadHandle;
	switch (handle) {
		case PerspectiveQuadHandle::EdgeTop: return PerspectiveEdgeAnchor::Top;
		case PerspectiveQuadHandle::EdgeRight: return PerspectiveEdgeAnchor::Right;
		case PerspectiveQuadHandle::EdgeBottom: return PerspectiveEdgeAnchor::Bottom;
		case PerspectiveQuadHandle::EdgeLeft: return PerspectiveEdgeAnchor::Left;
		default: return PerspectiveEdgeAnchor::None;
	}
}

std::string DescribeEdgeAnchor(perspective::PerspectiveEdgeAnchor anchor) {
	using perspective::PerspectiveEdgeAnchor;
	switch (anchor) {
		case PerspectiveEdgeAnchor::None:
			return _(
				"No Perspective edge is held; opposite edges are averaged. "
				"Alt+click an edge to hold it.");
		case PerspectiveEdgeAnchor::Top:
			return _("Holding the top edge; the bottom edge absorbs the difference.");
		case PerspectiveEdgeAnchor::Right:
			return _("Holding the right edge; the left edge absorbs the difference.");
		case PerspectiveEdgeAnchor::Bottom:
			return _("Holding the bottom edge; the top edge absorbs the difference.");
		case PerspectiveEdgeAnchor::Left:
			return _("Holding the left edge; the right edge absorbs the difference.");
	}
	return {};
}

std::string LocalizedPlanError(perspective::PerspectivePlanError error) {
	using perspective::PerspectivePlanError;
	switch (error) {
		case PerspectivePlanError::None: return {};
		case PerspectivePlanError::InvalidInput:
			return _("Perspective plan input is incomplete");
		case PerspectivePlanError::InvalidContext:
			return _("Perspective capture context is invalid");
		case PerspectivePlanError::SourceNotInFile:
			return _("Perspective source is not a live event in this file");
		case PerspectivePlanError::SourceNotFound:
			return _("Perspective source event no longer exists");
		case PerspectivePlanError::DuplicateSourceId:
			return _("Perspective source event Id is not unique");
		case PerspectivePlanError::StaleSource:
			return _("Perspective source or capture context changed");
		case PerspectivePlanError::InvalidTarget:
			return _("Perspective target quad is invalid");
		case PerspectivePlanError::InvalidErrorBudget:
			return _("Perspective output error budget is invalid");
		case PerspectivePlanError::StateEvaluationFailed:
			return _("Perspective source state could not be evaluated");
		case PerspectivePlanError::ApplyBlocked:
			return _("Perspective Apply is blocked for this source");
		case PerspectivePlanError::BoundsEvaluationFailed:
			return _("Perspective source bounds could not be evaluated");
		case PerspectivePlanError::ForwardEvaluationFailed:
			return _("Perspective source transform could not be evaluated");
		case PerspectivePlanError::SolverFailed:
			return _("Perspective tags could not represent the target");
		case PerspectivePlanError::RewriteFailed:
			return _("Perspective tags could not be rewritten");
		case PerspectivePlanError::NoChange:
			return _("Perspective target requires no ASS change");
		case PerspectivePlanError::StagedStateEvaluationFailed:
			return _("rewritten Perspective state could not be evaluated");
		case PerspectivePlanError::StagedApplyBlocked:
			return _("rewritten Perspective state is not applyable");
		case PerspectivePlanError::StagedScaleMismatch:
			return _("rewritten Perspective state changed the preserved text scale");
		case PerspectivePlanError::StagedRepresentationMismatch:
			return _("rewritten Perspective state violates the selected representation policy");
		case PerspectivePlanError::StagedBoundsEvaluationFailed:
			return _("rewritten Perspective bounds could not be evaluated");
		case PerspectivePlanError::StagedBoundsMismatch:
			return _("rewritten Perspective source bounds changed");
		case PerspectivePlanError::StagedForwardEvaluationFailed:
			return _("rewritten Perspective transform could not be evaluated");
		case PerspectivePlanError::ResidualFailed:
			return _("rewritten Perspective residual could not be measured");
		case PerspectivePlanError::ResidualExceeded:
			return _("rewritten Perspective output exceeds the error budget");
	}
	return _("unknown Perspective mutation plan error");
}

std::string DescribePerspectiveFailure(
	perspective::PerspectivePlanDiagnostic const& diagnostic) {
	using perspective::PerspectivePlanError;
	switch (diagnostic.error) {
		case PerspectivePlanError::InvalidTarget:
			return LocalizedGeometryError(diagnostic.geometry_error);
		case PerspectivePlanError::StateEvaluationFailed:
		case PerspectivePlanError::StagedStateEvaluationFailed:
			return LocalizedAssStateError(diagnostic.state_error);
		case PerspectivePlanError::ApplyBlocked:
		case PerspectivePlanError::StagedApplyBlocked:
			return LocalizedAssApplyBlocker(diagnostic.apply_blocker);
		case PerspectivePlanError::BoundsEvaluationFailed:
		case PerspectivePlanError::StagedBoundsEvaluationFailed:
			return LocalizedAssBoundsError(
				diagnostic.bounds_error, diagnostic.bounds_font);
		case PerspectivePlanError::ForwardEvaluationFailed:
		case PerspectivePlanError::StagedForwardEvaluationFailed:
			return LocalizedForwardError(diagnostic.forward_error);
		case PerspectivePlanError::SolverFailed:
			return LocalizedSolverError(diagnostic.solver_error);
		case PerspectivePlanError::RewriteFailed:
			return LocalizedRewriteError(diagnostic.rewrite_error);
		case PerspectivePlanError::ResidualFailed:
			return LocalizedResidualError(diagnostic.residual_error);
		default:
			return LocalizedPlanError(diagnostic.error);
	}
}
std::string DescribePerspectiveApplySuccess(
	perspective::PerspectiveMutationPlan const& plan, perspective::Quad const& drawn_target,
	perspective::PerspectiveApplyContext const& context) {
	if (plan.Snapped())
		return agi::format(_("Perspective target applied, snapped %.2f px to the nearest shape "
							 "the tags can represent.")
							   .c_str(),
						   plan.MaxError());
	double drawn_deviation = 0.0;
	auto const& result = plan.ResultQuad();
	for (std::size_t index = 0; index < result.size(); ++index)
		drawn_deviation = std::max(drawn_deviation, std::hypot(
														(result[index].x - drawn_target[index].x) * context.output_mapping.scale_x,
														(result[index].y - drawn_target[index].y) * context.output_mapping.scale_y));
	if (drawn_deviation > PerspectiveMaxError) {
		auto const drawn_area = std::abs(perspective::SignedArea(drawn_target));
		auto const result_area = std::abs(perspective::SignedArea(result));
		if (drawn_area > 0.0 && result_area > 0.0) {
			auto const scale_percent = std::sqrt(result_area / drawn_area) * 100.0;
			if (std::abs(scale_percent - 100.0) > 1.0)
				return agi::format(_("Perspective target applied; the active policies rescale "
									 "it to %s%% of the drawn size, landing up to %s px away.")
									   .c_str(),
								   agi::format("%.2f", scale_percent), agi::format("%.2f", drawn_deviation));
		}
		return agi::format(_("Perspective target applied; the written shape sits up to %s px "
							 "from the drawn target.")
							   .c_str(),
						   agi::format("%.2f", drawn_deviation));
	}
	return _("Perspective target applied. Adjust or redraw the target to apply again.");
}

std::string UnreachableTargetDiagnostic(
	perspective::NoFeasibleReason reason,
	std::optional<perspective::NoFeasibleMetrics> const& metrics, int decimal_places) {
	bool const quotable = metrics && metrics->best_error > metrics->budget;
	switch (reason) {
		case perspective::NoFeasibleReason::RepresentationPolicy:
			return _("The line already uses Perspective tags outside the Fax + Frz "
					 "Only subset, and they cannot be kept while reaching this "
					 "target. Turn the Fax + Frz Only option off to reach it.");
		case perspective::NoFeasibleReason::EdgeAnchor:
			return _("No candidate can hold the anchored edge where it was drawn "
					 "while reaching this target. Alt+click the anchored edge again "
					 "to release it, or adjust the target.");
		case perspective::NoFeasibleReason::ModelResidual:
			return _("No representable Perspective shape could be projected for "
					 "this target.");
		case perspective::NoFeasibleReason::Quantization:
			if (quotable)
				return agi::format(_("The tags can reach this shape, but not with only %d "
									 "decimal places: the written digits land %s px from the "
									 "shape they must reproduce, over a %s px budget. Raise "
									 "the Decimal Places option.")
									   .c_str(),
								   decimal_places, agi::format("%.2f", metrics->best_error),
								   agi::format("%.2f", metrics->budget));
			return _("The tags can reach this shape, but not enough decimal places "
					 "are allowed to write them down accurately. Raise the Decimal "
					 "Places option.");
		case perspective::NoFeasibleReason::None: break;
	}
	return _("No representable Perspective shape could be projected for this target.");
}

PerspectivePreview SolvePerspectivePreview(
	perspective::PerspectiveSourceSnapshot const& source, perspective::Quad const& target,
	perspective::OutputCoordinateMapping mapping, PerspectiveOptions const& options) {
	perspective::SolverInput input;
	input.target = target;
	input.output_mapping = mapping;
	input.max_error = PerspectiveMaxError;
	input.shape_tolerance = options.shape_tolerance;
	input.best_fit = options.best_fit;
	input.scale_policy = options.fit_text ? perspective::PerspectiveScalePolicy::Fit : perspective::PerspectiveScalePolicy::Preserve;
	input.representation_policy = options.fax_frz_only ? perspective::PerspectiveRepresentationPolicy::FaxFrzOnly : perspective::PerspectiveRepresentationPolicy::Automatic;
	input.edge_anchor = options.edge_anchor;
	input.maximum_decimals = options.decimal_places;
	perspective::PreparePerspectiveSolverInput(source, input);
	auto const solved = perspective::SolvePerspectiveTags(input);
	PerspectivePreview result{.feasible = static_cast<bool>(solved), .error = solved.error, .no_feasible_reason = solved.no_feasible_reason, .no_feasible_metrics = solved.no_feasible_metrics};
	if (solved) {
		auto const forward = perspective::ForwardQuad(source.forward_input, solved.candidate->state);
		if (forward)
			result.reachable = forward.quad;
	}
	return result;
}

static VisualGuidePoint ToGuidePoint(perspective::Vec2 point) { return {.x = point.x, .y = point.y}; }

void DrawGuideHandles(VideoOverlayDrawContext& context, VisualGuideViewport const& viewport,
					  VisualGuide const& guide, VisualGuideOverlayStyle const& style) {
	if (!IsVisualGuideViewportMappable(viewport))
		return;

	auto const draw_handle = [&](VisualGuidePoint point) {
		auto const canvas_point = VisualGuideToCanvas(point, viewport);
		context.SetLineColour(style.outline_colour, 0.95f, 2);
		context.SetFillColour(style.outline_colour, 0.85f);
		context.DrawCircle(canvas_point, 3.0f + 1.0f);
		context.SetLineColour(style.highlight_colour, 1.0f, 1);
		context.SetFillColour(style.highlight_colour, 0.85f);
		context.DrawCircle(canvas_point, 3.0f);
	};

	draw_handle(guide.first);
	draw_handle(guide.second);
}

static void DrawPerspectivePreview(VideoOverlayDrawContext& context, VisualGuideViewport const& viewport,
								   Vector2D canvas_size, PerspectiveOverlay const& overlay, PerspectiveOverlayStyle const& style) {
	auto const& quad = *overlay.reachable;
	if (!IsVisualGuideViewportMappable(viewport))
		return;

	std::array<Vector2D, 4> corners;
	for (std::size_t index = 0; index < quad.size(); ++index)
		corners[index] = VisualGuideToCanvas(ToGuidePoint(quad[index]), viewport);

	if (auto const& preview_target = overlay.target) {
		double const hide_threshold = style.ui_scale;
		bool const coincident = std::ranges::all_of(
			corners, [&, index = 0](Vector2D const& corner) mutable {
				auto const drawn = VisualGuideToCanvas(
					ToGuidePoint((*preview_target)[index++]), viewport);
				return (corner - drawn).Len() <= hide_threshold;
			});
		if (coincident)
			return;
	}

	double const dash = 6.0 * style.ui_scale;
	perspective::Rect const visible{
		.left = -dash, .top = -dash, .right = canvas_size.X() + dash, .bottom = canvas_size.Y() + dash};
	auto const stroke = [&](Vector2D from, Vector2D to) {
		auto const range = perspective::ClipSegmentRange(
			{.x = from.X(), .y = from.Y()}, {.x = to.X(), .y = to.Y()}, visible);
		if (!range)
			return;
		Vector2D const delta = to - from;
		double const length = std::hypot(delta.X(), delta.Y());
		if (!(length > 0.0))
			return;
		double const begin = std::floor((*range)[0] * length / (dash * 2.0)) * dash * 2.0;
		double const limit = (*range)[1] * length;
		for (double at = begin; at < limit; at += dash * 2.0) {
			double const end = std::min(at + dash, length);
			context.DrawLine(
				from + delta * (at / length), from + delta * (end / length));
		}
	};

	context.SetLineColour(
		style.highlight_secondary, 0.9f, 2);
	for (std::size_t index = 0; index < corners.size(); ++index)
		stroke(corners[index], corners[(index + 1) % corners.size()]);

	if (auto const& target = overlay.target) {
		context.SetLineColour(
			style.highlight_secondary, 0.45f, 1);
		for (std::size_t index = 0; index < corners.size(); ++index) {
			context.DrawLine(
				VisualGuideToCanvas(ToGuidePoint((*target)[index]), viewport),
				corners[index]);
		}
	}
}

void DrawPerspective(VideoOverlayDrawContext& context, VisualGuideViewport const& viewport,
					 Vector2D canvas_size, PerspectiveOverlay const& overlay, PerspectiveOverlayStyle const& style) {
	auto const *target = overlay.target ? &*overlay.target : nullptr;
	if (!IsVisualGuideViewportMappable(viewport))
		return;
	auto const ui_scale = static_cast<float>(style.ui_scale);

	if (auto const& current = overlay.current;
		current && (overlay.editable || !target)) {
		bool const differs = !target || overlay.modified;
		if (differs) {
			std::array<Vector2D, 5> current_outline;
			for (std::size_t index = 0; index < current->size(); ++index)
				current_outline[index] = VisualGuideToCanvas(
					ToGuidePoint((*current)[index]), viewport);
			current_outline.back() = current_outline.front();
			context.SetLineColour(
				style.line_secondary, 0.4f, 1);
			context.DrawLineStrip(current_outline.data(), current_outline.size());
		}
	}
	auto const& diagnostic = overlay.diagnostic;
	if (!diagnostic.empty()) {
		VideoOverlayTextStyle text_style;
		text_style.size = static_cast<int>(
			style.label_font_size * ui_scale);
		text_style.colour = style.invalid_line;
		text_style.outline = true;
		context.DrawText(
			diagnostic,
			static_cast<int>(viewport.canvas_x) + 8,
			static_cast<int>(viewport.canvas_y) + 8,
			text_style);
	}

	if (!target)
		return;

	std::array<Vector2D, 5> outline;
	for (std::size_t index = 0; index < target->size(); ++index)
		outline[index] = VisualGuideToCanvas(ToGuidePoint((*target)[index]), viewport);
	outline.back() = outline.front();

	bool const valid = overlay.valid;
	bool const draw_editable = overlay.editable && !overlay.off_frame;
	auto const line_colour = !valid ? style.invalid_line : draw_editable ? style.line_primary
																		 : style.line_secondary;
	auto const handle_colour = !valid ? style.invalid_handle : draw_editable ? style.highlight_primary
																			 : style.highlight_secondary;
	agi::Color const handle_outline{20, 20, 20};

	context.SetLineColour(handle_outline, 0.95f, 3);
	context.DrawLineStrip(outline.data(), outline.size());
	context.SetLineColour(line_colour, 1.0f, 1);
	context.DrawLineStrip(outline.data(), outline.size());

	if (!overlay.creating && !overlay.best_fit && overlay.edge_anchor != perspective::PerspectiveEdgeAnchor::None) {
		std::size_t from = 0;
		std::size_t to = 1;
		switch (overlay.edge_anchor) {
			case perspective::PerspectiveEdgeAnchor::Top: break;
			case perspective::PerspectiveEdgeAnchor::Right:
				from = 1;
				to = 2;
				break;
			case perspective::PerspectiveEdgeAnchor::Bottom:
				from = 3;
				to = 2;
				break;
			case perspective::PerspectiveEdgeAnchor::Left:
				from = 0;
				to = 3;
				break;
			case perspective::PerspectiveEdgeAnchor::None: break;
		}
		agi::Color const anchored_colour = valid ? handle_colour : line_colour;
		context.SetLineColour(handle_outline, 0.95f, 5);
		context.DrawLine(outline[from], outline[to]);
		context.SetLineColour(anchored_colour, 1.0f, 3);
		context.DrawLine(outline[from], outline[to]);
	}

	if (!overlay.creating && overlay.reachable)
		DrawPerspectivePreview(context, viewport, canvas_size, overlay, style);

	if (!draw_editable)
		return;

	constexpr std::array<perspective::PerspectiveQuadHandle, 4> corner_handles{
		perspective::PerspectiveQuadHandle::TopLeft,
		perspective::PerspectiveQuadHandle::TopRight,
		perspective::PerspectiveQuadHandle::BottomRight,
		perspective::PerspectiveQuadHandle::BottomLeft,
	};
	auto const active_handle = overlay.active_handle;
	float const handle_radius = 3.0f * ui_scale;
	for (std::size_t index = 0; index < target->size(); ++index) {
		float const radius = corner_handles[index] == active_handle
								 ? handle_radius + 1.0f
								 : handle_radius;
		context.SetLineColour(handle_outline, 0.95f, 2);
		context.SetFillColour(handle_outline, 0.85f);
		context.DrawCircle(outline[index], radius + 1.0f);
		context.SetLineColour(handle_colour, 1.0f, 1);
		context.SetFillColour(handle_colour, 0.9f);
		context.DrawCircle(outline[index], radius);
	}

	for (std::size_t index = 0; index < target->size(); ++index) {
		auto const edge_handle = static_cast<perspective::PerspectiveQuadHandle>(
			static_cast<int>(perspective::PerspectiveQuadHandle::EdgeTop) + index);
		Vector2D const midpoint = (outline[index] + outline[(index + 1) % target->size()]) / 2.0;
		float const radius = edge_handle == active_handle
								 ? handle_radius
								 : handle_radius - 1.0f;
		context.SetLineColour(handle_outline, 0.95f, 2);
		context.SetFillColour(handle_outline, 0.85f);
		context.DrawCircle(midpoint, radius + 1.0f);
		context.SetLineColour(handle_colour, 1.0f, 1);
		context.SetFillColour(handle_colour, 0.75f);
		context.DrawCircle(midpoint, radius);
	}

	if (auto const center = perspective::PerspectiveQuadArithmeticCenter(*target)) {
		auto const canvas_center = VisualGuideToCanvas(ToGuidePoint(*center), viewport);
		float const center_radius = active_handle == perspective::PerspectiveQuadHandle::Center
										? handle_radius + 2.0f
										: handle_radius + 1.0f;
		context.SetLineColour(handle_outline, 0.95f, 2);
		context.SetFillColour(handle_outline, 0.85f);
		context.DrawCircle(canvas_center, center_radius + 1.0f);
		context.SetLineColour(handle_colour, 1.0f, 1);
		context.SetFillColour(handle_colour, 0.9f);
		context.DrawCircle(canvas_center, center_radius);
	}
}

}
