#include "visual_tool_measure.h"

#include "async_video_provider.h"
#include "auto4_base.h"
#include "ass_file.h"
#include "compat.h"
#include "format.h"
#include "gl_text.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "project.h"
#include "selection_controller.h"
#include "utils.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_overlay_draw_context.h"
#include "video_overlay_draw_context_legacy_gl.h"
#include "visual_guide_interaction.h"
#include "visual_guide_overlay.h"

#include <libaegisub/color.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <wx/event.h>
#include <wx/toolbar.h>
#include <wx/translation.h>

namespace {
// Canvas pixels. The perspective hit tests multiply these by UiScale() so
// handles stay grabbable on high-DPI screens.
constexpr float kHandleRadius = 3.0f;
constexpr float kHitTolerance = 5.0f;
constexpr auto kPerspectiveFitTextOption = "Tool/Visual/Perspective/Fit Text";
constexpr auto kPerspectiveFaxFrzOnlyOption = "Tool/Visual/Perspective/Fax Frz Only";
constexpr auto kPerspectiveDecimalPlacesOption = "Tool/Visual/Perspective/Decimal Places";
constexpr auto kPerspectiveShapeToleranceOption = "Tool/Visual/Perspective/Shape Tolerance";
// Tag-rounding budget in output pixels: it bounds the serialization error the
// written digits may add. How far the reachable shape sits from the drawn one
// is not budgeted at all -- the nearest shape is always returned and its
// shortfall is drawn by the preview.
constexpr double kPerspectiveMaxError = 0.1;

VisualGuidePoint ToGuidePoint(perspective::Vec2 point) {
	return {point.x, point.y};
}

perspective::Vec2 ToPerspectivePoint(VisualGuidePoint point) {
	return {point.x, point.y};
}

VisualGuide const* FindGuide(
	VisualGuideSnapshotView const& snapshot,
	std::string const& id) {
	auto const found = std::find_if(
		snapshot.guides.begin(), snapshot.guides.end(), [&id](VisualGuide const& guide) {
			return guide.id == id;
		});
	return found == snapshot.guides.end() ? nullptr : &*found;
}

VisualGuideOverlayStyle MakeOverlayStyle(
	agi::OptionValue const* line_colour,
	agi::OptionValue const* highlight_colour) {
	VisualGuideOverlayStyle style;
	style.line_colour = to_wx(line_colour->GetColor());
	style.highlight_colour = to_wx(highlight_colour->GetColor());
	// Fixed white label border for contrast; the selected state borrows the
	// highlight colour instead. Not user-configurable to avoid a new option.
	style.label_border_colour = wxColour(255, 255, 255);
	style.label_font_size = OPT_GET("Tool/Visual/Coordinate Font Size")->GetInt();
	return style;
}

wxString MaximumGuidesStatusMessage() {
	return fmt_tl(
		"A maximum of %d visual guides is allowed.",
		static_cast<int>(VisualGuideController::MaximumGuideCount));
}

// The pure perspective modules must stay wx-free, so user-facing strings are
// localized here at the UI boundary instead of inside Describe* themselves.
std::string LocalizedGeometryError(perspective::GeometryError error) {
	using perspective::GeometryError;
	switch (error) {
		case GeometryError::None: return {};
		case GeometryError::NonFinite:
			return from_wx(_("coordinate is not finite"));
		case GeometryError::CoordinateOutOfRange:
			return from_wx(_("coordinate exceeds the supported range"));
		case GeometryError::DuplicatePoint:
			return from_wx(_("quad contains duplicate points"));
		case GeometryError::EdgeTooShort:
			return from_wx(_("quad edge is too short"));
		case GeometryError::AreaTooSmall:
			return from_wx(_("quad area is too small"));
		case GeometryError::SelfIntersecting:
			return from_wx(_("quad is self-intersecting"));
		case GeometryError::WrongWinding:
			return from_wx(_("quad winding must be TL, TR, BR, BL"));
		case GeometryError::NonConvex:
			return from_wx(_("quad must be strictly convex"));
		case GeometryError::InvalidDomain:
			return from_wx(_("geometry domain is invalid"));
		case GeometryError::SingularHomography:
			return from_wx(_("homography is singular"));
		case GeometryError::ProjectionDomain:
			return from_wx(_("projection denominator crosses or approaches zero"));
	}
	return from_wx(_("unknown geometry error"));
}

std::string LocalizedAssStateError(perspective::AssStateError error) {
	using perspective::AssStateError;
	switch (error) {
		case AssStateError::None: return {};
		case AssStateError::InvalidInput:
			return from_wx(_("Perspective ASS state input is incomplete"));
		case AssStateError::InvalidPlayResolution:
			return from_wx(_("Perspective PlayRes is invalid"));
		case AssStateError::MissingEventStyle:
			return from_wx(_("the dialogue event style cannot be resolved"));
		case AssStateError::MissingResetStyle:
			return from_wx(_("a Perspective-relevant reset style cannot be resolved"));
		case AssStateError::InvalidCaptureTime:
			return from_wx(_("the capture time is outside the dialogue event"));
		case AssStateError::InvalidGeometryParameter:
			return from_wx(_("a Perspective geometry override is invalid"));
		case AssStateError::InvalidAlignment:
			return from_wx(_("the effective ASS alignment is invalid"));
		case AssStateError::NonFiniteStyle:
			return from_wx(_("the effective ASS style contains invalid geometry"));
		case AssStateError::MixedGeometryRuns:
			return from_wx(_("the dialogue has mixed Perspective geometry runs"));
	}
	return from_wx(_("unknown Perspective ASS state error"));
}

std::string LocalizedAssApplyBlocker(perspective::AssApplyBlocker blocker) {
	using perspective::AssApplyBlocker;
	switch (blocker) {
		case AssApplyBlocker::None: return {};
		case AssApplyBlocker::UnsupportedMove:
			return from_wx(_("Perspective Apply does not support \\move"));
		case AssApplyBlocker::UnsupportedGeometryAnimation:
			return from_wx(_("Perspective Apply does not support animated geometry"));
		case AssApplyBlocker::UnsupportedNamedReset:
			return from_wx(_("Perspective Apply does not support named style resets"));
	}
	return from_wx(_("unknown Perspective Apply blocker"));
}

std::string LocalizedAssBoundsError(
	perspective::AssBoundsError error,
	std::string const& font = {}) {
	using perspective::AssBoundsError;
	switch (error) {
		case AssBoundsError::None: return {};
		case AssBoundsError::InvalidInput:
			return from_wx(_("Perspective ASS bounds input is incomplete"));
		case AssBoundsError::EmptyGeometry:
			return from_wx(_("the dialogue has no visible geometry"));
		case AssBoundsError::FontUnavailable:
			// The face travels with the error so the user learns which font to
			// fix rather than that some font somewhere went unmeasured.
			return font.empty()
					   ? from_wx(_("renderer-compatible text font bounds are unavailable"))
					   : from_wx(fmt_tl(
							 "renderer-compatible bounds are unavailable for the font %s",
							 to_wx(font)));
		case AssBoundsError::UnsupportedAutomaticWrap:
			return from_wx(_("automatic text wrapping does not have stable Perspective base bounds; use explicit line breaks or \\q2"));
		case AssBoundsError::MixedGeometryRuns:
			return from_wx(_("mixed text and drawing runs do not have stable base bounds"));
		case AssBoundsError::UnsupportedDrawingLayout:
			return from_wx(_("multiple ASS drawing runs require unsupported glyph layout"));
		case AssBoundsError::UnsupportedDrawingCommand:
			return from_wx(_("ASS B-spline drawing metrics are not supported"));
		case AssBoundsError::DrawingStateMismatch:
			return from_wx(_("the parsed drawing runs do not match the evaluated ASS state"));
		case AssBoundsError::InvalidDrawingSyntax:
			return from_wx(_("the ASS drawing contains invalid syntax"));
		case AssBoundsError::InvalidDrawingGeometry:
			return from_wx(_("the ASS drawing does not produce measurable geometry"));
		case AssBoundsError::InvalidDrawingBounds:
			return from_wx(_("the ASS drawing bounds are degenerate or outside the supported range"));
	}
	return from_wx(_("unknown Perspective ASS bounds error"));
}

std::string LocalizedForwardError(perspective::ForwardError error) {
	using perspective::ForwardError;
	switch (error) {
		case ForwardError::None: return {};
		case ForwardError::UnsupportedMixedBounds:
			return from_wx(_("mixed text and drawing runs do not have a stable base bounds"));
		case ForwardError::UnsupportedDynamicBounds:
			return from_wx(_("dynamic geometry has no single base bounds"));
		case ForwardError::UnsupportedFontBounds:
			return from_wx(_("font metrics are unavailable for base bounds"));
		case ForwardError::InvalidBounds:
			return from_wx(_("base bounds are invalid"));
		case ForwardError::InvalidResolution:
			return from_wx(_("PlayRes/LayoutRes/video resolution is invalid"));
		case ForwardError::InvalidEventTime:
			return from_wx(_("event-relative evaluation time is invalid"));
		case ForwardError::InvalidAlignment:
			return from_wx(_("alignment must be in the range 1..9"));
		case ForwardError::NonFiniteState:
			return from_wx(_("evaluated ASS state is not finite"));
		case ForwardError::TransformParameterOutOfRange:
			return from_wx(_("transform parameter exceeds the supported range"));
		case ForwardError::DegenerateScale:
			return from_wx(_("scale must not be zero"));
		case ForwardError::MirroredTransform:
			return from_wx(_("mirrored transforms are not supported for ordered quads"));
		case ForwardError::SingularTransform:
			return from_wx(_("shear transform is singular"));
		case ForwardError::ProjectionDomain:
			return from_wx(_("perspective denominator crosses or approaches zero"));
		case ForwardError::InvalidQuad:
			return from_wx(_("forward transform produced an invalid quad"));
		case ForwardError::SingularHomography:
			return from_wx(_("forward homography is singular"));
	}
	return from_wx(_("unknown forward error"));
}

std::string LocalizedSolverError(perspective::SolverError error) {
	using perspective::SolverError;
	switch (error) {
		case SolverError::None: return {};
		case SolverError::InvalidSource:
			return from_wx(_("source Perspective state is invalid"));
		case SolverError::InvalidTarget:
			return from_wx(_("target Perspective quad is invalid"));
		case SolverError::InvalidOutputMapping:
			return from_wx(_("output coordinate mapping is invalid"));
		case SolverError::InvalidErrorBudget:
			return from_wx(_("Perspective error budget is invalid"));
		case SolverError::NoFeasibleCandidate:
			return from_wx(_("no quantized Perspective tag candidate meets the error budget"));
	}
	return from_wx(_("unknown Perspective solver error"));
}

std::string LocalizedRewriteError(perspective::RewriteError error) {
	using perspective::RewriteError;
	switch (error) {
		case RewriteError::None: return {};
		case RewriteError::InvalidInput:
			return from_wx(_("Perspective ASS rewrite input is invalid"));
		case RewriteError::InvalidTarget:
			return from_wx(_("Perspective ASS rewrite target is invalid"));
		case RewriteError::UnsupportedMove:
			return from_wx(_("Perspective ASS rewrite does not support \\move"));
		case RewriteError::UnsupportedGeometryAnimation:
			return from_wx(_("Perspective ASS rewrite does not support animated geometry"));
		case RewriteError::UnsupportedNamedReset:
			return from_wx(_("Perspective ASS rewrite does not support named style resets"));
		case RewriteError::InvalidGeometryParameter:
			return from_wx(_("Perspective ASS rewrite contains invalid geometry"));
	}
	return from_wx(_("unknown Perspective ASS rewrite error"));
}

std::string LocalizedResidualError(perspective::ResidualError error) {
	using perspective::ResidualError;
	switch (error) {
		case ResidualError::None: return {};
		case ResidualError::InvalidCandidate:
			return from_wx(_("candidate Perspective state is invalid"));
		case ResidualError::InvalidTarget:
			return from_wx(_("target Perspective quad is invalid"));
		case ResidualError::InvalidOutputMapping:
			return from_wx(_("output coordinate mapping is invalid"));
		case ResidualError::ProjectionDomain:
			return from_wx(_("Perspective residual sample is outside the projection domain"));
	}
	return from_wx(_("unknown Perspective residual error"));
}

// Gesture rejections used to be discarded at every call site, which is what
// made the quad tool feel like it silently ignored input. These are phrased for
// someone holding the mouse button, not for a log.
std::string LocalizedQuadEditError(perspective::PerspectiveQuadEditError error) {
	using perspective::PerspectiveQuadEditError;
	switch (error) {
		case PerspectiveQuadEditError::None: return {};
		case PerspectiveQuadEditError::NotBound:
			return from_wx(_("The Perspective quad is not bound to a line."));
		case PerspectiveQuadEditError::NoTarget:
			return from_wx(_("There is no Perspective target to edit."));
		case PerspectiveQuadEditError::NoCurrentQuad:
			return from_wx(_("The line's current Perspective quad is unavailable."));
		case PerspectiveQuadEditError::ReadOnly:
			return from_wx(_("The Perspective target is read-only."));
		case PerspectiveQuadEditError::InvalidSource:
			return from_wx(_("The Perspective source geometry is invalid."));
		case PerspectiveQuadEditError::InvalidHandle:
			return from_wx(_("That Perspective handle cannot be dragged."));
		case PerspectiveQuadEditError::GestureAlreadyActive:
			return from_wx(_("Another Perspective drag is already in progress."));
		case PerspectiveQuadEditError::NoGesture:
			return from_wx(_("No Perspective drag is in progress."));
		case PerspectiveQuadEditError::InvalidTolerance:
			return from_wx(_("The Perspective hit tolerance is invalid."));
		case PerspectiveQuadEditError::NonFinitePointer:
			return from_wx(_("The pointer position is outside the coordinate range."));
		case PerspectiveQuadEditError::CoordinateOutOfRange:
			return from_wx(_("The Perspective quad would leave the coordinate range."));
	}
	return from_wx(_("unknown Perspective quad edit error"));
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
			return from_wx(_(
				"No Perspective edge is held; opposite edges are averaged. "
				"Alt+click an edge to hold it."));
		case PerspectiveEdgeAnchor::Top:
			return from_wx(_("Holding the top edge; the bottom edge absorbs the difference."));
		case PerspectiveEdgeAnchor::Right:
			return from_wx(_("Holding the right edge; the left edge absorbs the difference."));
		case PerspectiveEdgeAnchor::Bottom:
			return from_wx(_("Holding the bottom edge; the top edge absorbs the difference."));
		case PerspectiveEdgeAnchor::Left:
			return from_wx(_("Holding the left edge; the right edge absorbs the difference."));
	}
	return {};
}

std::string LocalizedPlanError(perspective::PerspectivePlanError error) {
	using perspective::PerspectivePlanError;
	switch (error) {
		case PerspectivePlanError::None: return {};
		case PerspectivePlanError::InvalidInput:
			return from_wx(_("Perspective plan input is incomplete"));
		case PerspectivePlanError::InvalidContext:
			return from_wx(_("Perspective capture context is invalid"));
		case PerspectivePlanError::SourceNotInFile:
			return from_wx(_("Perspective source is not a live event in this file"));
		case PerspectivePlanError::SourceNotFound:
			return from_wx(_("Perspective source event no longer exists"));
		case PerspectivePlanError::DuplicateSourceId:
			return from_wx(_("Perspective source event Id is not unique"));
		case PerspectivePlanError::StaleSource:
			return from_wx(_("Perspective source or capture context changed"));
		case PerspectivePlanError::InvalidTarget:
			return from_wx(_("Perspective target quad is invalid"));
		case PerspectivePlanError::InvalidErrorBudget:
			return from_wx(_("Perspective output error budget is invalid"));
		case PerspectivePlanError::StateEvaluationFailed:
			return from_wx(_("Perspective source state could not be evaluated"));
		case PerspectivePlanError::ApplyBlocked:
			return from_wx(_("Perspective Apply is blocked for this source"));
		case PerspectivePlanError::BoundsEvaluationFailed:
			return from_wx(_("Perspective source bounds could not be evaluated"));
		case PerspectivePlanError::ForwardEvaluationFailed:
			return from_wx(_("Perspective source transform could not be evaluated"));
		case PerspectivePlanError::SolverFailed:
			return from_wx(_("Perspective tags could not represent the target"));
		case PerspectivePlanError::RewriteFailed:
			return from_wx(_("Perspective tags could not be rewritten"));
		case PerspectivePlanError::NoChange:
			return from_wx(_("Perspective target requires no ASS change"));
		case PerspectivePlanError::StagedStateEvaluationFailed:
			return from_wx(_("rewritten Perspective state could not be evaluated"));
		case PerspectivePlanError::StagedApplyBlocked:
			return from_wx(_("rewritten Perspective state is not applyable"));
		case PerspectivePlanError::StagedScaleMismatch:
			return from_wx(_("rewritten Perspective state changed the preserved text scale"));
		case PerspectivePlanError::StagedRepresentationMismatch:
			return from_wx(_("rewritten Perspective state violates the selected representation policy"));
		case PerspectivePlanError::StagedBoundsEvaluationFailed:
			return from_wx(_("rewritten Perspective bounds could not be evaluated"));
		case PerspectivePlanError::StagedBoundsMismatch:
			return from_wx(_("rewritten Perspective source bounds changed"));
		case PerspectivePlanError::StagedForwardEvaluationFailed:
			return from_wx(_("rewritten Perspective transform could not be evaluated"));
		case PerspectivePlanError::ResidualFailed:
			return from_wx(_("rewritten Perspective residual could not be measured"));
		case PerspectivePlanError::ResidualExceeded:
			return from_wx(_("rewritten Perspective output exceeds the error budget"));
	}
	return from_wx(_("unknown Perspective mutation plan error"));
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

// Two decimals, one px unit: these numbers exist to be dialled against an
// option value, not admired.
wxString FormatPx(double value) {
	return wxString::Format(wxT("%.2f"), value);
}
}

VisualToolMeasure::VisualToolMeasure(VideoDisplay *parent, agi::Context *context)
	: VisualToolBase(parent, context), controller(context->GetUI().visualGuideController), text(parent->CreateTextRenderer()), fit_text_to_target(OPT_GET(kPerspectiveFitTextOption)->GetBool()), fax_frz_only(OPT_GET(kPerspectiveFaxFrzOnlyOption)->GetBool()), perspective_decimal_places(perspective::ClampPerspectiveDecimalPlaces(
																																																																	  OPT_GET(kPerspectiveDecimalPlacesOption)->GetInt())),
	  perspective_shape_tolerance(OPT_GET(kPerspectiveShapeToleranceOption)->GetDouble()),
	  invalid_line_color_opt(OPT_GET("Colour/Visual Tools/Perspective Invalid Line")), invalid_handle_color_opt(OPT_GET("Colour/Visual Tools/Perspective Invalid Handle")) {
	auto core = c->GetCore();
	connections.push_back(core.project->AddVideoProviderListener([this](AsyncVideoProvider*) {
		CancelInteraction(false);
		if (submode == SubMode::PerspectiveQuad)
			RebindPerspective();
		this->parent->Render();
	}));
	connections.push_back(core.project->AddTimecodesListener([this](agi::vfr::Framerate const&) {
		CancelInteraction(false);
		if (submode == SubMode::PerspectiveQuad)
			RebindPerspective();
		this->parent->Render();
	}));
	// Every one of these feeds the same solver pipeline, so a change has to
	// re-solve: the previous answer, and any diagnostic derived from it, was
	// computed under the old settings. Clearing the message instead of redoing
	// the work used to leave the user staring at a target with no explanation.
	auto subscribe_solver_option = [this](char const *name, bool& cached) {
		connections.push_back(OPT_SUB(name,
									  [this, &cached](agi::OptionValue const& value) {
										  bool const next = value.GetBool();
										  if (cached == next)
											  return;
										  cached = next;
										  RefreshAfterSolverOptionChange();
									  }));
	};
	subscribe_solver_option(kPerspectiveFitTextOption, fit_text_to_target);
	subscribe_solver_option(kPerspectiveFaxFrzOnlyOption, fax_frz_only);
	connections.emplace_back(OPT_SUB(kPerspectiveShapeToleranceOption,
									 [this](agi::OptionValue const& value) {
										 double const next = value.GetDouble();
										 if (perspective_shape_tolerance == next)
											 return;
										 perspective_shape_tolerance = next;
										 RefreshAfterSolverOptionChange();
									 }));
	connections.push_back(OPT_SUB(kPerspectiveDecimalPlacesOption,
								  [this](agi::OptionValue const& value) {
									  int const next = perspective::ClampPerspectiveDecimalPlaces(
										  value.GetInt());
									  if (perspective_decimal_places == next)
										  return;
									  perspective_decimal_places = next;
									  RefreshAfterSolverOptionChange();
								  }));
}

double VisualToolMeasure::UiScale() const {
	return std::max(1.0, GetWindowScaleFactor(parent));
}

std::string VisualToolMeasure::LocalizedDiagnostic() const {
	if (auto const& validation = perspective_state.Validation();
		validation && !*validation)
		return LocalizedGeometryError(validation->error);
	return perspective_diagnostic;
}

VisualToolMeasure::~VisualToolMeasure() {
	CancelInteraction(false);
	CancelPerspectiveInteraction(false);
	if (toolbar)
		toolbar->Unbind(wxEVT_TOOL, &VisualToolMeasure::OnSubTool, this);
}

std::optional<VisualGuidePoint> VisualToolMeasure::CurrentGuidePoint(bool clamp) const {
	auto point = CanvasToVisualGuide(mouse_pos, parent->GetVisualGuideViewport());
	if (!point)
		return std::nullopt;
	if (clamp)
		return ClampVisualGuidePointToScript(*point, parent->GetVisualGuideViewport());
	return point;
}

bool VisualToolMeasure::IsInsideVideoViewport(Vector2D point) const {
	auto const& viewport = parent->GetVisualGuideViewport();
	return IsVisualGuideViewportMappable(viewport)
		&& point.X() >= viewport.canvas_x
		&& point.Y() >= viewport.canvas_y
		&& point.X() <= viewport.canvas_x + viewport.canvas_width
		&& point.Y() <= viewport.canvas_y + viewport.canvas_height;
}

void VisualToolMeasure::BeginInteraction(wxMouseEvent&) {
	if (!controller || !IsInsideVideoViewport(mouse_pos))
		return;

	auto const viewport = parent->GetVisualGuideViewport();
	auto const style = MakeOverlayStyle(line_color_primary_opt, highlight_color_primary_opt);
	// Hit-testing needs to measure the info-box text so selection matches the
	// rendered rectangle; reuse the same legacy context the draw pass uses.
	LegacyVideoOverlayDrawContext hit_context(gl, *text);
	if (auto hit = HitTestVisualGuides(
		mouse_pos, controller->CaptureView(), viewport, style, hit_context, kHitTolerance)) {
		std::optional<VisualGuide> guide;
		{
			auto const snapshot = controller->CaptureView();
			if (auto const* found = FindGuide(snapshot, hit.id))
				guide = *found;
		}
		if (!guide)
			return;
		controller->Select(hit.id);

		auto point = CanvasToVisualGuide(mouse_pos, viewport);
		if (!point)
			return;

		original_guide = std::move(*guide);
		editing_id = hit.id;
		drag_start_point = ClampVisualGuidePointToScript(*point, viewport);
		switch (hit.part) {
			case VisualGuideHitPart::FirstEndpoint:
				interaction = Interaction::DraggingFirstEndpoint;
				break;
			case VisualGuideHitPart::SecondEndpoint:
				interaction = Interaction::DraggingSecondEndpoint;
				break;
			case VisualGuideHitPart::Line:
			case VisualGuideHitPart::Label:
				// Dragging the info box translates the whole guide, like the body.
				interaction = Interaction::DraggingLine;
				break;
			case VisualGuideHitPart::None:
				return;
		}
	}
	else {
		auto point = CurrentGuidePoint(true);
		if (!point)
			return;

		controller->Select(std::nullopt);
		VisualGuide preview;
		preview.id = "preview";
		preview.first = *point;
		preview.second = *point;
		preview_guide = preview;
		interaction = Interaction::CreatingMeasurement;
	}

	dragging = true;
	parent->CaptureMouse();
}

void VisualToolMeasure::UpdateInteraction() {
	if (interaction == Interaction::None)
		return;

	auto const viewport = parent->GetVisualGuideViewport();
	if (interaction == Interaction::CreatingMeasurement) {
		auto point = CurrentGuidePoint(true);
		if (point && preview_guide)
			preview_guide->second = SnapVisualGuideMeasurementPoint(
				preview_guide->first, *point, shift_down);
		return;
	}

	if (!editing_id || !original_guide)
		return;

	auto point = CanvasToVisualGuide(mouse_pos, viewport);
	if (!point)
		return;
	point = ClampVisualGuidePointToScript(*point, viewport);

	VisualGuideDragAction action = VisualGuideDragAction::MoveGuide;
	switch (interaction) {
		case Interaction::DraggingLine:
			break;
		case Interaction::DraggingFirstEndpoint:
			action = VisualGuideDragAction::MoveFirstEndpoint;
			break;
		case Interaction::DraggingSecondEndpoint:
			action = VisualGuideDragAction::MoveSecondEndpoint;
			break;
		case Interaction::None:
		case Interaction::CreatingMeasurement:
			return;
	}
	auto updated = ApplyVisualGuideDrag(
		*original_guide, action, drag_start_point, *point, shift_down, viewport);
	controller->Update(*editing_id, std::move(updated));
}

void VisualToolMeasure::FinishInteraction() {
	if (interaction == Interaction::None)
		return;

	if (interaction == Interaction::CreatingMeasurement && preview_guide) {
		auto const viewport = parent->GetVisualGuideViewport();
		auto const first = VisualGuideToCanvas(preview_guide->first, viewport);
		auto const second = VisualGuideToCanvas(preview_guide->second, viewport);
		if ((first - second).SquareLen() > kHitTolerance * kHitTolerance) {
			preview_guide->id.clear();
			if (auto id = controller->Add(*preview_guide))
				controller->Select(*id);
			else
				c->ShowStatus(from_wx(MaximumGuidesStatusMessage()));
		}
	}

	interaction = Interaction::None;
	editing_id.reset();
	original_guide.reset();
	preview_guide.reset();
	dragging = false;
	if (parent->HasCapture())
		parent->ReleaseMouse();
	parent->SetFocus();
}

void VisualToolMeasure::CancelInteraction(bool render) {
	if (interaction == Interaction::None)
		return;

	auto id = editing_id;
	auto original = original_guide;
	interaction = Interaction::None;
	editing_id.reset();
	original_guide.reset();
	preview_guide.reset();
	dragging = false;

	if (id && original)
		controller->Update(*id, std::move(*original));
	if (parent->HasCapture())
		parent->ReleaseMouse();
	if (render)
		parent->Render();
}

std::optional<perspective::Vec2> VisualToolMeasure::CurrentPerspectivePoint() const {
	auto point = CanvasToVisualGuide(mouse_pos, parent->GetVisualGuideViewport());
	if (!point)
		return std::nullopt;
	return ToPerspectivePoint(*point);
}

std::string VisualToolMeasure::UnreachableTargetDiagnostic() const {
	// The solver reports the stage every candidate died at, so the message can
	// name the option that actually owns that stage instead of guessing from
	// the current flags. Models are never refused for distance anymore -- the
	// solver always returns the best candidate and the preview shows where it
	// lands -- so the only reachable-shape refusals left are the representation
	// policy and the digits. When the closest digits rejection was measurable,
	// its numbers ride along, so the option can be dialled instead of nudged.
	auto const& metrics = perspective_no_feasible_metrics;
	bool const quotable = metrics && metrics->best_error > metrics->budget;
	switch (perspective_no_feasible_reason) {
		case perspective::NoFeasibleReason::RepresentationPolicy:
			return from_wx(_(
				"The line already uses Perspective tags outside the Fax + Frz "
				"Only subset, and they cannot be kept while reaching this "
				"target. Turn the Fax + Frz Only option off to reach it."));
		case perspective::NoFeasibleReason::EdgeAnchor:
			return from_wx(_(
				"No candidate can hold the anchored edge where it was drawn "
				"while reaching this target. Alt+click the anchored edge again "
				"to release it, or adjust the target."));
		case perspective::NoFeasibleReason::ModelResidual:
			return from_wx(_(
				"No representable Perspective shape could be projected for "
				"this target."));
		case perspective::NoFeasibleReason::Quantization:
			if (quotable)
				return from_wx(fmt_tl(
					"The tags can reach this shape, but not with only %d "
					"decimal places: the written digits land %s px from the "
					"shape they must reproduce, over a %s px budget. Raise "
					"the Decimal Places option.",
					perspective_decimal_places,
					FormatPx(metrics->best_error), FormatPx(metrics->budget)));
			return from_wx(_(
				"The tags can reach this shape, but not enough decimal places "
				"are allowed to write them down accurately. Raise the Decimal "
				"Places option."));
		case perspective::NoFeasibleReason::None:
			break;
	}
	return from_wx(_(
		"No representable Perspective shape could be projected for this "
		"target."));
}

void VisualToolMeasure::ClearPerspectivePreview() {
	perspective_solve_state = PerspectiveSolveState::NotReady;
	perspective_no_feasible_reason = perspective::NoFeasibleReason::None;
	perspective_no_feasible_metrics.reset();
	perspective_preview.reset();
}

void VisualToolMeasure::UpdatePerspectivePreview() {
	// Reset before any early return: every exit path must leave the Apply gate
	// closed unless the solve below explicitly proves the target reachable.
	ClearPerspectivePreview();
	// A diagnostic from Apply or from rebinding is more specific than anything
	// this can say, so only the solver-owned slot is refreshed here.
	bool const owns_diagnostic = perspective_diagnostic.empty()
		|| perspective_diagnostic_depends_on_solver_options;
	if (owns_diagnostic) {
		perspective_diagnostic.clear();
		perspective_diagnostic_depends_on_solver_options = false;
	}

	if (submode != SubMode::PerspectiveQuad || !perspective_source
		|| !perspective_state.IsBound() || !perspective_state.IsEditable()
		|| !perspective_state.IsModified())
		return;
	auto const& target = perspective_state.Target();
	if (!target)
		return;
	// An invalid quad already has its own live message; solving it would only
	// restate a geometry error the user is mid-gesture on.
	auto const& validation = perspective_state.Validation();
	if (!validation || !*validation)
		return;
	auto const context = CurrentPerspectiveApplyContext();
	if (!context)
		return;

	perspective::SolverInput input;
	input.target = *target;
	input.output_mapping = context->output_mapping;
	input.max_error = kPerspectiveMaxError;
	input.shape_tolerance = perspective_shape_tolerance;
	input.scale_policy = fit_text_to_target
		? perspective::PerspectiveScalePolicy::Fit
		: perspective::PerspectiveScalePolicy::Preserve;
	input.representation_policy = fax_frz_only
		? perspective::PerspectiveRepresentationPolicy::FaxFrzOnly
		: perspective::PerspectiveRepresentationPolicy::Automatic;
	input.edge_anchor = perspective_edge_anchor;
	input.maximum_decimals = perspective_decimal_places;

	perspective::PreparePerspectiveSolverInput(*perspective_source, input);
	auto const solved = perspective::SolvePerspectiveTags(input);
	if (!solved) {
		// Mirror the verdict even when the diagnostic slot is owned elsewhere:
		// the Apply gate only cares whether the target is reachable. Other
		// solver errors describe the input, not reachability, so stay NotReady.
		if (solved.error == perspective::SolverError::NoFeasibleCandidate) {
			perspective_solve_state = PerspectiveSolveState::Infeasible;
			perspective_no_feasible_reason = solved.no_feasible_reason;
			perspective_no_feasible_metrics = solved.no_feasible_metrics;
		}
		if (owns_diagnostic
			&& solved.error == perspective::SolverError::NoFeasibleCandidate) {
			perspective_diagnostic = UnreachableTargetDiagnostic();
			perspective_diagnostic_depends_on_solver_options = true;
		}
		return;
	}
	// A snapped result still applies fine, so feasibility does not depend on it.
	perspective_solve_state = PerspectiveSolveState::Feasible;

	// The solver measures its own residual exactly this way, so this is the
	// shape it believes it reached rather than an independent estimate. Apply
	// re-derives it through a staged file, which can differ once text extents
	// change under Fit; that is a preview, not a promise. Stored whether or
	// not the candidate snapped: under Preserve the candidate's own target is
	// area-normalized, so a "not snapped" result can still sit far from the
	// drawn quad, and the drawn preview is the only honest report of that.
	auto const forward = perspective::ForwardQuad(
		perspective_source->forward_input, solved.candidate->state);
	if (!forward)
		return;
	perspective_preview = forward.quad;
}

void VisualToolMeasure::BeginPerspectiveInteraction() {
	auto const point = CurrentPerspectivePoint();
	auto const& viewport = parent->GetVisualGuideViewport();
	if (!point || !perspective_state.IsEditable()
		|| (perspective_state.IsBound() && !active_line)
		|| !IsVisualGuideViewportMappable(viewport)
		|| !IsInsideVideoViewport(mouse_pos))
		return;

	// A previous Apply failure describes the old target. Once the user starts
	// repairing or replacing it, let live validation own the toolbar message.
	bool const had_diagnostic = !perspective_diagnostic.empty();
	perspective_diagnostic.clear();
	perspective_diagnostic_depends_on_solver_options = false;
	if (had_diagnostic)
		c->ShowStatus({});
	UpdateToolbarState();

	if (auto const& target = perspective_state.Target()) {
		perspective::Quad canvas_target;
		for (std::size_t index = 0; index < target->size(); ++index) {
			auto const canvas_point = VisualGuideToCanvas(
				ToGuidePoint((*target)[index]), viewport);
			canvas_target[index] = {canvas_point.X(), canvas_point.Y()};
		}
		auto const handle = perspective::HitTestPerspectiveQuad(
			canvas_target, {mouse_pos.X(), mouse_pos.Y()},
			kHitTolerance * UiScale());
		// Alt+click on an edge marks that edge as the one to trust, instead of
		// starting a drag. Averaging opposite edges is only right when both were
		// measured equally well; when one is aligned to something visible in the
		// frame, averaging moves it off by half the other edge's error.
		if (alt_down) {
			if (auto const anchor = EdgeAnchorForHandle(handle);
				anchor != perspective::PerspectiveEdgeAnchor::None) {
				perspective_edge_anchor =
					perspective_edge_anchor == anchor
						? perspective::PerspectiveEdgeAnchor::None
						: anchor;
				perspective_diagnostic.clear();
				perspective_diagnostic_depends_on_solver_options = false;
				UpdatePerspectivePreview();
				UpdateToolbarState();
				c->ShowStatus(DescribeEdgeAnchor(perspective_edge_anchor));
				parent->Render();
				return;
			}
		}
		if (handle != perspective::PerspectiveQuadHandle::None) {
			auto const error = perspective_state.BeginGesture(handle, *point);
			if (error == perspective::PerspectiveQuadEditError::None) {
				dragging = true;
				parent->CaptureMouse();
				return;
			}
			// The press landed on a handle, so the user meant to grab the target,
			// not to draw a new one. Falling through to creation here would
			// discard the very quad they were reaching for.
			perspective_diagnostic = LocalizedQuadEditError(error);
			perspective_diagnostic_depends_on_solver_options = false;
			c->ShowStatus(perspective_diagnostic);
			UpdateToolbarState();
			return;
		}
	}

	perspective_creation = PerspectiveCreationGesture {
		*point,
		mouse_pos,
		perspective::PerspectiveQuadFromOppositeCorners(
			*point, *point,
			perspective_source
				? perspective::PerspectiveFirstEdgeDirection(*perspective_source)
				: std::optional<perspective::Vec2> {})};
	dragging = true;
	parent->CaptureMouse();
}

void VisualToolMeasure::UpdatePerspectiveInteraction() {
	if (!IsPerspectiveInteractionActive())
		return;
	if (auto point = CurrentPerspectivePoint()) {
		if (perspective_state.IsGestureActive()) {
			perspective::PerspectiveQuadGestureModifiers modifiers;
			modifiers.axis_locked = shift_down;
			modifiers.symmetric = ctrl_down;
			if (auto const error =
					perspective_state.UpdateGesture(*point, modifiers);
				error != perspective::PerspectiveQuadEditError::None) {
				// Mid-drag rejections are the ones users read as the tool being
				// broken: the handle stops following the cursor with no reason
				// given. Report it without cancelling, so releasing still keeps
				// the last good shape.
				perspective_diagnostic = LocalizedQuadEditError(error);
				perspective_diagnostic_depends_on_solver_options = false;
				c->ShowStatus(perspective_diagnostic);
			}
			else {
				ClearPerspectivePreview();
				perspective_solve_state = PerspectiveSolveState::Pending;
			}
		}
		else if (perspective_creation) {
			perspective_creation->preview =
				perspective::PerspectiveQuadFromOppositeCorners(
					perspective_creation->anchor, *point,
					perspective_source
						? perspective::PerspectiveFirstEdgeDirection(
							*perspective_source)
						: std::optional<perspective::Vec2> {});
		}
	}
	UpdateToolbarState();
}

void VisualToolMeasure::FinishPerspectiveInteraction() {
	if (!IsPerspectiveInteractionActive())
		return;

	if (perspective_state.IsGestureActive()) {
		if (auto const error = perspective_state.FinishGesture();
			error != perspective::PerspectiveQuadEditError::None) {
			perspective_diagnostic = LocalizedQuadEditError(error);
			perspective_diagnostic_depends_on_solver_options = false;
		}
	}
	else if (perspective_creation) {
		perspective::Vec2 const first_canvas {
			perspective_creation->canvas_anchor.X(),
			perspective_creation->canvas_anchor.Y()};
		perspective::Vec2 const second_canvas {mouse_pos.X(), mouse_pos.Y()};
		if (perspective::IsPerspectiveQuadCreationDrag(
				first_canvas, second_canvas, kHitTolerance * UiScale())) {
			// A too-small drag is a click and is meant to do nothing. Anything
			// larger that still fails is a real refusal and gets said out loud.
			if (auto const validation =
					perspective::ValidateQuad(perspective_creation->preview);
				!validation) {
				perspective_diagnostic = LocalizedGeometryError(validation.error);
				perspective_diagnostic_depends_on_solver_options = false;
			}
			else if (auto const error = perspective_state.ReplaceTarget(
					perspective_creation->preview);
				error != perspective::PerspectiveQuadEditError::None) {
				perspective_diagnostic = LocalizedQuadEditError(error);
				perspective_diagnostic_depends_on_solver_options = false;
			}
		}
	}
	perspective_creation.reset();
	dragging = false;
	if (parent->HasCapture())
		parent->ReleaseMouse();
	parent->SetFocus();
	UpdatePerspectivePreview();
	UpdateToolbarState();

	if (auto const& validation = perspective_state.Validation();
		validation && !*validation)
		c->ShowStatus(LocalizedGeometryError(validation->error));
	else if (!perspective_diagnostic.empty())
		c->ShowStatus(perspective_diagnostic);
}

void VisualToolMeasure::CancelPerspectiveInteraction(bool render) {
	if (!IsPerspectiveInteractionActive())
		return;

	// Cancelling restores the pre-gesture target, so a failure here means the
	// state machine is inconsistent rather than the user doing something odd.
	// Still worth surfacing: silently leaving a half-applied drag is worse.
	if (perspective_state.IsGestureActive()) {
		if (auto const error = perspective_state.CancelGesture();
			error != perspective::PerspectiveQuadEditError::None) {
			perspective_diagnostic = LocalizedQuadEditError(error);
			perspective_diagnostic_depends_on_solver_options = false;
			c->ShowStatus(perspective_diagnostic);
		}
	}
	perspective_creation.reset();
	dragging = false;
	if (parent->HasCapture())
		parent->ReleaseMouse();
	UpdatePerspectivePreview();
	UpdateToolbarState();
	if (render)
		parent->Render();
}

bool VisualToolMeasure::IsPerspectiveInteractionActive() const noexcept {
	return perspective_state.IsGestureActive()
		|| perspective_creation.has_value();
}

std::optional<perspective::PerspectiveApplyContext>
VisualToolMeasure::CurrentPerspectiveApplyContext() const {
	auto const viewport = parent->GetVisualGuideViewport();
	if (!IsVisualGuideViewportMappable(viewport))
		return std::nullopt;

	auto core = c->GetCore();
	auto const* provider = core.project->VideoProvider();
	if (!provider || provider->GetWidth() <= 0 || provider->GetHeight() <= 0)
		return std::nullopt;

	perspective::PerspectiveApplyContext context;
	context.frame_number = frame_number;
	context.capture_time_ms = core.videoController->TimeAtFrame(frame_number);
	context.play_resolution = {viewport.script_width, viewport.script_height};
	int layout_width = 0;
	int layout_height = 0;
	core.ass->GetLayoutResolution(layout_width, layout_height);
	if (layout_width > 0 && layout_height > 0) {
		context.layout_resolution = perspective::Resolution {
			static_cast<double>(layout_width), static_cast<double>(layout_height)};
	}
	context.video_storage_resolution = perspective::Resolution {
		static_cast<double>(provider->GetWidth()),
		static_cast<double>(provider->GetHeight())};
	// The solver budget is measured in subtitle render pixels. Canvas zoom,
	// letterboxing, pan and DPI are display-only and must not enter this mapping.
	context.output_mapping = {
		provider->GetWidth() / viewport.script_width,
		provider->GetHeight() / viewport.script_height,
	};
	return context;
}

bool VisualToolMeasure::SelectedLineMatchesSource() const {
	if (!perspective_source)
		return false;
	auto core = c->GetCore();
	auto const* selected = core.selectionController->GetActiveLine();
	if (!selected)
		return false;
	// The same identity triple OnFileChanged uses: a recycled Id must not pass
	// as the captured source, or a deleted-and-retyped line would resurrect a
	// target captured against a different line object.
	auto const& fingerprint = perspective_source->fingerprint;
	return fingerprint.file_identity == reinterpret_cast<std::uintptr_t>(core.ass.get()) && fingerprint.line_identity == reinterpret_cast<std::uintptr_t>(selected) && fingerprint.line.id == selected->Id;
}

bool VisualToolMeasure::CanApplyPerspective() const {
	return submode == SubMode::PerspectiveQuad
		&& active_line != nullptr
		&& perspective_source.has_value()
		&& perspective_state.IsBound()
		&& perspective_state.IsEditable()
		&& perspective_state.HasTarget()
		&& perspective_state.IsModified()
		&& !IsPerspectiveInteractionActive()
		&& perspective_state.Validation().has_value()
		&& static_cast<bool>(*perspective_state.Validation())
		&& perspective_solve_state == PerspectiveSolveState::Feasible;
}

void VisualToolMeasure::ApplyPerspective() {
	command_session.ResetCommitId();
	auto reset_commit_id = agi::make_scope_exit(
		[this] { command_session.ResetCommitId(); });
	if (!CanApplyPerspective())
		return;
	perspective_diagnostic_depends_on_solver_options = false;

	auto const current_context = CurrentPerspectiveApplyContext();
	auto const& target = perspective_state.Target();
	if (!current_context || !target || !perspective_source) {
		perspective_diagnostic = LocalizedPlanError(
			perspective::PerspectivePlanError::InvalidContext);
		UpdateToolbarState();
		c->ShowStatus(perspective_diagnostic);
		return;
	}
	// Copied on purpose: the commit below uses ObserveSelf, whose synchronous
	// OnFileChanged rebinds the state and assigns the just-applied quad into
	// this very target optional. The success report compares the written quad
	// against what the user drew, which only the pre-commit copy still holds.
	perspective::Quad const drawn_target = *target;

	auto core = c->GetCore();
	auto* const resolved = core.selectionController->GetDialogueById(
		perspective_source->fingerprint.line.id);
	if (!resolved || resolved != active_line
		|| resolved != core.selectionController->GetActiveLine()) {
		perspective_diagnostic = LocalizedPlanError(
			perspective::PerspectivePlanError::StaleSource);
		UpdateToolbarState();
		c->ShowStatus(perspective_diagnostic);
		return;
	}

	auto const planned = perspective::BuildPerspectiveMutationPlan(
		*core.ass, *current_context, *perspective_source, drawn_target,
		kPerspectiveMaxError,
		&Automation4::CalculateTextExtents,
		fit_text_to_target
			? perspective::PerspectiveScalePolicy::Fit
			: perspective::PerspectiveScalePolicy::Preserve,
		fax_frz_only
			? perspective::PerspectiveRepresentationPolicy::FaxFrzOnly
			: perspective::PerspectiveRepresentationPolicy::Automatic,
		perspective_decimal_places,
		perspective_edge_anchor,
		perspective_shape_tolerance);
	if (!planned) {
		if (planned.error == perspective::PerspectivePlanError::SolverFailed
			&& planned.solver_error
				== perspective::SolverError::NoFeasibleCandidate) {
			// The plan re-solved with the same inputs as the preview, so its
			// classification is the one the diagnostic should quote.
			perspective_no_feasible_reason =
				planned.solver_no_feasible_reason;
			perspective_no_feasible_metrics = planned.solver_no_feasible_metrics;
			perspective_diagnostic = UnreachableTargetDiagnostic();
			perspective_diagnostic_depends_on_solver_options = true;
		}
		else if (planned.error
			== perspective::PerspectivePlanError::ResidualExceeded) {
			// The old wording sent users to Decimal Places, which is the wrong
			// lever in the common case: the model itself could not reach the
			// shape, and no number of digits changes that.
			perspective_diagnostic = from_wx(_(
				"The rewritten tags land farther from the target than the "
				"solver predicted. Raising the Decimal Places option may "
				"help."));
			perspective_diagnostic_depends_on_solver_options = true;
		}
		else if (planned.error == perspective::PerspectivePlanError::NoChange) {
			// Not a failure: the target already matches what the tags say.
			// Reporting it in the ordinary status channel avoids dressing a
			// no-op up as an error.
			perspective_diagnostic.clear();
			perspective_diagnostic_depends_on_solver_options = false;
			UpdateToolbarState();
			c->ShowStatus(from_wx(_(
				"The Perspective target already matches the line; nothing to "
				"apply.")));
			parent->Render();
			return;
		}
		else
			perspective_diagnostic = DescribePerspectiveFailure(planned);
		UpdateToolbarState();
		c->ShowStatus(perspective_diagnostic);
		parent->Render();
		return;
	}

	if (resolved != active_line
		|| resolved != core.selectionController->GetActiveLine()) {
		perspective_diagnostic = LocalizedPlanError(
			perspective::PerspectivePlanError::StaleSource);
		UpdateToolbarState();
		c->ShowStatus(perspective_diagnostic);
		return;
	}
	auto const executed = perspective::ExecutePerspectiveMutationPlan(
		*core.ass, *current_context, *planned.plan);
	if (!executed) {
		perspective_diagnostic = DescribePerspectiveFailure(executed);
		UpdateToolbarState();
		c->ShowStatus(perspective_diagnostic);
		return;
	}

	AssDialogue const* changed[] = {executed.line};
	command_session.CommitWithFeedback(
		from_wx(_("apply Perspective quad")),
		AssFile::COMMIT_DIAG_TEXT,
		-1,
		executed.line,
		changed,
		aegisub::LocalCommitFeedback::ObserveSelf);
	// The commit rebinds the target to the just-applied geometry, so an
	// immediate second Apply is intentionally a no-op; say so instead of
	// silently greying the button.
	if (planned.plan->Snapped()) {
		// The user asked for a shape the tags cannot express and got the nearest
		// one. Not saying so would leave them thinking the drag was applied
		// exactly, then wondering why the result drifted from their reference.
		c->ShowStatus(from_wx(fmt_tl(
			"Perspective target applied, snapped %.2f px to the nearest shape "
			"the tags can represent.",
			planned.plan->MaxError())));
		return;
	}
	// A plan the solver called exact can still land visibly off the drawn quad:
	// under Preserve the candidates aim at the area-normalized quad, so the
	// applied shape keeps the size the pinned scale produces rather than the
	// size that was drawn. The staged output quad is the authority on what was
	// actually written, so measure that against the drawn corners, in output
	// pixels like every other solver number.
	double const drawn_deviation = [&, mapping = current_context->output_mapping]() {
		double worst = 0.0;
		auto const& result = planned.plan->ResultQuad();
		for (std::size_t index = 0; index < result.size(); ++index) {
			worst = std::max(worst, std::hypot(
				(result[index].x - drawn_target[index].x) * mapping.scale_x,
				(result[index].y - drawn_target[index].y) * mapping.scale_y));
		}
		return worst;
	}();
	if (drawn_deviation > kPerspectiveMaxError) {
		auto const& result = planned.plan->ResultQuad();
		auto const drawn_area = std::abs(perspective::SignedArea(drawn_target));
		auto const result_area = std::abs(perspective::SignedArea(result));
		if (drawn_area > 0.0 && result_area > 0.0) {
			auto const scale_percent = std::sqrt(result_area / drawn_area) * 100.0;
			if (std::abs(scale_percent - 100.0) > 1.0) {
				c->ShowStatus(from_wx(fmt_tl(
					"Perspective target applied; the active policies rescale "
					"it to %s%% of the drawn size, landing up to %s px away.",
					FormatPx(scale_percent), FormatPx(drawn_deviation))));
				return;
			}
		}
		c->ShowStatus(from_wx(fmt_tl(
			"Perspective target applied; the written shape sits up to %s px "
			"from the drawn target.",
			FormatPx(drawn_deviation))));
		return;
	}
	c->ShowStatus(from_wx(_(
		"Perspective target applied. Adjust or redraw the target to apply "
		"again.")));
}

void VisualToolMeasure::ClearPerspectiveBinding() {
	CancelPerspectiveInteraction(false);
	perspective_state.Clear();
	perspective_source.reset();
	perspective_diagnostic.clear();
	perspective_diagnostic_depends_on_solver_options = false;
	ClearPerspectivePreview();
	// The anchor names an edge of a specific quad, so it cannot outlive it.
	perspective_edge_anchor = perspective::PerspectiveEdgeAnchor::None;
	UpdateToolbarState();
}

void VisualToolMeasure::RebindPerspective(
	PerspectiveRebindMode mode,
	bool announce_diagnostic) {
	CancelPerspectiveInteraction(false);
	if (submode != SubMode::PerspectiveQuad) {
		ClearPerspectiveBinding();
		return;
	}
	perspective_diagnostic.clear();
	perspective_diagnostic_depends_on_solver_options = false;

	auto set_diagnostic = [&](std::string message) {
		ClearPerspectiveBinding();
		perspective_diagnostic = std::move(message);
		UpdateToolbarState();
		if (announce_diagnostic)
			c->ShowStatus(perspective_diagnostic);
	};
	// The selected line simply is not displayed on this frame: park the
	// binding and target read-only instead of destroying the editing state,
	// so seeking back into range restores it.
	auto keep_off_frame = [&]() {
		perspective_diagnostic = from_wx(_(
			"The active subtitle line is not displayed on this frame; "
			"the Perspective target is kept."));
		// The captured source belongs to a frame the line is no longer on, so
		// any reachable shape derived from it would be stale.
		ClearPerspectivePreview();
		UpdateToolbarState();
		if (announce_diagnostic)
			c->ShowStatus(perspective_diagnostic);
	};

	auto const current_context = CurrentPerspectiveApplyContext();
	if (!current_context) {
		set_diagnostic(from_wx(_("Perspective Quad requires a valid video coordinate system.")));
		return;
	}
	if (!active_line) {
		if (SelectedLineMatchesSource()) {
			keep_off_frame();
			return;
		}
		set_diagnostic(from_wx(_("Perspective Quad requires an active subtitle line on this frame.")));
		return;
	}

	auto core = c->GetCore();
	auto captured = perspective::CapturePerspectiveSource(
		*core.ass, *active_line, *current_context,
		&Automation4::CalculateTextExtents);
	if (!captured) {
		// OnFrameChanged runs while the previous frame's active_line is still
		// cached; a capture time outside the event means the seek just left
		// the line's display range.
		if (SelectedLineMatchesSource()
			&& captured.error == perspective::PerspectivePlanError::StateEvaluationFailed
			&& captured.state_error == perspective::AssStateError::InvalidCaptureTime) {
			keep_off_frame();
			return;
		}
		set_diagnostic(DescribePerspectiveFailure(captured));
		return;
	}

	bool const editable =
		captured.source->apply_blocker == perspective::AssApplyBlocker::None;
	perspective::PerspectiveQuadEditError bind_error =
		perspective::PerspectiveQuadEditError::InvalidSource;
	switch (mode) {
		case PerspectiveRebindMode::PreserveTarget:
			bind_error = perspective_state.RefreshCurrent(
				captured.source->current_quad, editable);
			break;
		case PerspectiveRebindMode::AcceptCurrent:
			bind_error = perspective_state.Bind(
				captured.source->current_quad,
				editable,
				captured.source->current_quad.has_value());
			break;
		case PerspectiveRebindMode::DiscardTarget:
			bind_error = perspective_state.Bind(
				captured.source->current_quad,
				editable,
				!editable && captured.source->current_quad.has_value());
			break;
	}
	if (bind_error != perspective::PerspectiveQuadEditError::None) {
		set_diagnostic(from_wx(_("Perspective Quad could not bind the evaluated geometry.")));
		return;
	}
	perspective_source = std::move(*captured.source);
	if (perspective_source->apply_blocker != perspective::AssApplyBlocker::None) {
		perspective_diagnostic = LocalizedAssApplyBlocker(
			perspective_source->apply_blocker);
	}
	// A preserved target now sits against freshly captured source geometry, so
	// what the tags can reach may have moved with it.
	UpdatePerspectivePreview();
	UpdateToolbarState();
	if (announce_diagnostic && !perspective_diagnostic.empty())
		c->ShowStatus(perspective_diagnostic);
}

void VisualToolMeasure::OnMouseEvent(wxMouseEvent& event) {
	shift_down = event.ShiftDown();
	ctrl_down = event.CmdDown();
	alt_down = event.AltDown();
	if (event.Leaving() && interaction == Interaction::None
		&& !IsPerspectiveInteractionActive()) {
		mouse_pos = Vector2D();
		parent->RenderToolFeedback();
		return;
	}

	auto const point = event.GetPosition();
	mouse_pos = Vector2D(point.x, point.y);
	if (event.ButtonDown())
		parent->SetFocus();

	if (submode == SubMode::Segment) {
		if (event.LeftDown())
			BeginInteraction(event);
		else if (interaction != Interaction::None) {
			if (event.LeftIsDown())
				UpdateInteraction();
			else {
				// wx can deliver the release position without a preceding motion event.
				// Apply it before committing so guides end exactly where the user let go.
				UpdateInteraction();
				FinishInteraction();
			}
		}
	}
	else {
		if (event.LeftDown())
			BeginPerspectiveInteraction();
		else if (IsPerspectiveInteractionActive()) {
			if (event.LeftIsDown())
				UpdatePerspectiveInteraction();
			else {
				UpdatePerspectiveInteraction();
				FinishPerspectiveInteraction();
			}
		}
	}

	parent->RenderToolFeedback();
}

bool VisualToolMeasure::OnKeyDown(wxKeyEvent& event) {
	if (event.GetKeyCode() == WXK_ESCAPE) {
		if (submode == SubMode::Segment && interaction != Interaction::None) {
			CancelInteraction();
			return true;
		}
		if (submode == SubMode::PerspectiveQuad
			&& IsPerspectiveInteractionActive()) {
			CancelPerspectiveInteraction();
			return true;
		}
	}
	if (submode == SubMode::PerspectiveQuad
		&& (event.GetKeyCode() == WXK_RETURN
			|| event.GetKeyCode() == WXK_NUMPAD_ENTER)
		&& CanApplyPerspective()) {
		ApplyPerspective();
		return true;
	}

	if (submode != SubMode::Segment
		|| event.GetKeyCode() != WXK_DELETE || !controller)
		return false;

	return RemoveSelectedGuide();
}

bool VisualToolMeasure::RemoveSelectedGuide() {
	if (submode != SubMode::Segment || !controller)
		return false;

	auto const view = controller->CaptureView();
	if (!view.selected_id)
		return false;
	auto const selected_id = *view.selected_id;
	auto const* guide = FindGuide(view, selected_id);
	if (!guide) {
		// Stale selection must not swallow Video-context Delete hotkeys.
		controller->Select(std::nullopt);
		return false;
	}
	return controller->Remove(selected_id);
}

bool VisualToolMeasure::SupportsNudge() const {
	return submode == SubMode::PerspectiveQuad;
}

std::string VisualToolMeasure::GetHotkeyContext() const {
	// Only the Perspective submode owns keys; Segment stays on the default
	// Video context so frame stepping keeps working while measuring.
	if (submode == SubMode::PerspectiveQuad)
		return "Visual Measure";
	return {};
}

bool VisualToolMeasure::Nudge(Vector2D direction, VisualNudgeMagnitude magnitude) {
	if (submode != SubMode::PerspectiveQuad || IsPerspectiveInteractionActive())
		return false;
	if (!perspective_state.IsBound() || !perspective_state.IsEditable()
		|| !perspective_state.HasTarget())
		return false;

	double const step = GetNudgeStep(
		"Tool/Visual/Nudge/Perspective Step",
		"Tool/Visual/Nudge/Perspective Step Large",
		magnitude);
	auto const& target = *perspective_state.Target();
	perspective::Quad moved;
	for (std::size_t index = 0; index < moved.size(); ++index) {
		moved[index] = {
			target[index].x + direction.X() * step,
			target[index].y + direction.Y() * step};
	}
	// Nudging into the coordinate limit is the common rejection here, and a
	// keypress that does nothing at all reads as a dead hotkey.
	if (auto const error = perspective_state.ReplaceTarget(moved);
		error != perspective::PerspectiveQuadEditError::None) {
		perspective_diagnostic = LocalizedQuadEditError(error);
		perspective_diagnostic_depends_on_solver_options = false;
		c->ShowStatus(perspective_diagnostic);
		UpdateToolbarState();
		parent->Render();
		return false;
	}
	UpdatePerspectivePreview();
	UpdateToolbarState();
	parent->Render();
	return true;
}

void VisualToolMeasure::DrawHandles(VideoOverlayDrawContext& context, VisualGuide const& guide) {
	auto const viewport = parent->GetVisualGuideViewport();
	if (!IsVisualGuideViewportMappable(viewport))
		return;

	auto const style = MakeOverlayStyle(line_color_primary_opt, highlight_color_primary_opt);
	auto const draw_handle = [&](VisualGuidePoint point) {
		auto const canvas_point = VisualGuideToCanvas(point, viewport);
		context.SetLineColour(style.outline_colour, 0.95f, 2);
		context.SetFillColour(style.outline_colour, 0.85f);
		context.DrawCircle(canvas_point, kHandleRadius + 1.0f);
		context.SetLineColour(style.highlight_colour, 1.0f, 1);
		context.SetFillColour(style.highlight_colour, 0.85f);
		context.DrawCircle(canvas_point, kHandleRadius);
	};

	draw_handle(guide.first);
	draw_handle(guide.second);
}

void VisualToolMeasure::DrawPerspectivePreview(
	VideoOverlayDrawContext& context, perspective::Quad const& quad) {
	auto const& viewport = parent->GetVisualGuideViewport();
	if (!IsVisualGuideViewportMappable(viewport))
		return;

	std::array<Vector2D, 4> corners;
	for (std::size_t index = 0; index < quad.size(); ++index)
		corners[index] = VisualGuideToCanvas(ToGuidePoint(quad[index]), viewport);

	// A result that lands where the target already is would only add dash
	// noise over the solid outline. Comparing in canvas pixels keeps the
	// threshold zoom-independent: rounding residue stays hidden at every
	// zoom, while a Preserve size drift grows with the picture exactly as
	// the user sees it.
	if (auto const& preview_target = perspective_state.Target()) {
		double const hide_threshold = UiScale();
		bool const coincident = std::all_of(
			corners.begin(), corners.end(), [&, index = 0](Vector2D const& corner) mutable {
				auto const drawn = VisualGuideToCanvas(
					ToGuidePoint((*preview_target)[index++]), viewport);
				return (corner - drawn).Len() <= hide_threshold;
			});
		if (coincident)
			return;
	}

	// Dashed, so this reads as a report rather than a second draggable quad.
	// The draw context has no dash primitive, so the segments are stepped by
	// hand in canvas pixels.
	double const dash = 6.0 * UiScale();
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
		// Keep the dash phase anchored at the original endpoint while skipping
		// all offscreen segments, even at extreme zoom or source coordinates.
		double const begin = std::floor((*range)[0] * length / (dash * 2.0)) * dash * 2.0;
		double const limit = (*range)[1] * length;
		for (double at = begin; at < limit; at += dash * 2.0) {
			double const end = std::min(at + dash, length);
			context.DrawLine(
				from + delta * (at / length), from + delta * (end / length));
		}
	};

	// Deliberately not the invalid colour: that one means Apply is impossible,
	// and a snapped result applies fine, it just lands slightly off. Reusing it
	// here would teach the user to ignore it.
	context.SetLineColour(
		to_wx(highlight_color_secondary_opt->GetColor()), 0.9f, 2);
	for (std::size_t index = 0; index < corners.size(); ++index)
		stroke(corners[index], corners[(index + 1) % corners.size()]);

	// Tie each preview corner to the target corner it stands in for, so the
	// direction and size of the shortfall is readable at a glance. That is the
	// whole point when only one or two edges have a reference in the video.
	if (auto const& target = perspective_state.Target()) {
		context.SetLineColour(
			to_wx(highlight_color_secondary_opt->GetColor()), 0.45f, 1);
		for (std::size_t index = 0; index < corners.size(); ++index) {
			context.DrawLine(
				VisualGuideToCanvas(ToGuidePoint((*target)[index]), viewport),
				corners[index]);
		}
	}
}

void VisualToolMeasure::DrawPerspective(VideoOverlayDrawContext& context) {
	auto const* target = perspective_creation
		? &perspective_creation->preview
		: (perspective_state.Target() ? &*perspective_state.Target() : nullptr);
	auto const& viewport = parent->GetVisualGuideViewport();
	if (!IsVisualGuideViewportMappable(viewport))
		return;
	float const ui_scale = static_cast<float>(UiScale());

	// The unmodified subtitle quad is the reference the target is edited
	// against: draw it faintly whenever the target has moved away from it.
	if (auto const& current = perspective_state.Current();
		current
		&& (perspective_state.IsEditable() || !target)) {
		bool const differs = !target || perspective_state.IsModified();
		if (differs) {
			std::array<Vector2D, 5> current_outline;
			for (std::size_t index = 0; index < current->size(); ++index)
				current_outline[index] = VisualGuideToCanvas(
					ToGuidePoint((*current)[index]), viewport);
			current_outline.back() = current_outline.front();
			context.SetLineColour(
				to_wx(line_color_secondary_opt->GetColor()), 0.4f, 1);
			context.DrawLineStrip(current_outline.data(), current_outline.size());
		}
	}
	std::string const diagnostic = LocalizedDiagnostic();
	if (!diagnostic.empty()) {
		VideoOverlayTextStyle style;
		style.size = static_cast<int>(
			OPT_GET("Tool/Visual/Coordinate Font Size")->GetInt() * ui_scale);
		style.colour = to_wx(invalid_line_color_opt->GetColor());
		style.outline = true;
		context.DrawText(
			diagnostic,
			static_cast<int>(viewport.canvas_x) + 8,
			static_cast<int>(viewport.canvas_y) + 8,
			style);
	}

	if (!target)
		return;

	std::array<Vector2D, 5> outline;
	for (std::size_t index = 0; index < target->size(); ++index)
		outline[index] = VisualGuideToCanvas(ToGuidePoint((*target)[index]), viewport);
	outline.back() = outline.front();

	auto const preview_validation = perspective_creation
		? std::optional<perspective::GeometryValidation>(
			perspective::ValidateQuad(perspective_creation->preview))
		: std::nullopt;
	auto const& state_validation = perspective_state.Validation();
	bool const valid = preview_validation
		? static_cast<bool>(*preview_validation)
		: state_validation && static_cast<bool>(*state_validation);
	// A parked off-frame target renders in the read-only colours without
	// handles; it becomes editable again when the line is back on a frame.
	bool const off_frame = perspective_state.IsBound() && !active_line;
	bool const draw_editable = perspective_state.IsEditable() && !off_frame;
	wxColour const line_colour = !valid
		? to_wx(invalid_line_color_opt->GetColor())
		: to_wx((draw_editable
			? line_color_primary_opt : line_color_secondary_opt)->GetColor());
	wxColour const handle_colour = !valid
		? to_wx(invalid_handle_color_opt->GetColor())
		: to_wx((draw_editable
			? highlight_color_primary_opt : highlight_color_secondary_opt)->GetColor());
	wxColour const handle_outline(20, 20, 20);

	context.SetLineColour(handle_outline, 0.95f, 3);
	context.DrawLineStrip(outline.data(), outline.size());
	context.SetLineColour(line_colour, 1.0f, 1);
	context.DrawLineStrip(outline.data(), outline.size());

	// The held edge, drawn heavier than the rest of the outline. Without this
	// the anchor is invisible state that silently changes what Apply produces.
	// Width alone was too easy to miss, so on an otherwise valid quad it also
	// takes the handle colour rather than the line colour; an invalid quad
	// keeps the invalid colour on every edge, because readiness outranks the
	// anchor as a signal.
	if (!perspective_creation
		&& perspective_edge_anchor != perspective::PerspectiveEdgeAnchor::None) {
		std::size_t from = 0;
		std::size_t to = 1;
		switch (perspective_edge_anchor) {
			case perspective::PerspectiveEdgeAnchor::Top: break;
			case perspective::PerspectiveEdgeAnchor::Right: from = 1; to = 2; break;
			case perspective::PerspectiveEdgeAnchor::Bottom: from = 3; to = 2; break;
			case perspective::PerspectiveEdgeAnchor::Left: from = 0; to = 3; break;
			case perspective::PerspectiveEdgeAnchor::None: break;
		}
		wxColour const anchored_colour = valid ? handle_colour : line_colour;
		context.SetLineColour(handle_outline, 0.95f, 5);
		context.DrawLine(outline[from], outline[to]);
		context.SetLineColour(anchored_colour, 1.0f, 3);
		context.DrawLine(outline[from], outline[to]);
	}

	// Only meaningful for the committed target, not for a quad still being
	// rubber-banded into existence.
	if (!perspective_creation && perspective_preview)
		DrawPerspectivePreview(context, *perspective_preview);

	if (!draw_editable)
		return;

	constexpr std::array<perspective::PerspectiveQuadHandle, 4> corner_handles {
		perspective::PerspectiveQuadHandle::TopLeft,
		perspective::PerspectiveQuadHandle::TopRight,
		perspective::PerspectiveQuadHandle::BottomRight,
		perspective::PerspectiveQuadHandle::BottomLeft,
	};
	auto const active_handle = perspective_state.ActiveHandle();
	float const handle_radius = kHandleRadius * ui_scale;
	for (std::size_t index = 0; index < target->size(); ++index) {
		float const radius = corner_handles[index] == active_handle
			? handle_radius + 1.0f : handle_radius;
		context.SetLineColour(handle_outline, 0.95f, 2);
		context.SetFillColour(handle_outline, 0.85f);
		context.DrawCircle(outline[index], radius + 1.0f);
		context.SetLineColour(handle_colour, 1.0f, 1);
		context.SetFillColour(handle_colour, 0.9f);
		context.DrawCircle(outline[index], radius);
	}

	// Edge midpoints give one-directional resize without corner gymnastics.
	for (std::size_t index = 0; index < target->size(); ++index) {
		auto const edge_handle = static_cast<perspective::PerspectiveQuadHandle>(
			static_cast<int>(perspective::PerspectiveQuadHandle::EdgeTop) + index);
		Vector2D const midpoint = (outline[index] + outline[(index + 1) % target->size()]) / 2.0;
		float const radius = edge_handle == active_handle
			? handle_radius : handle_radius - 1.0f;
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
			? handle_radius + 2.0f : handle_radius + 1.0f;
		context.SetLineColour(handle_outline, 0.95f, 2);
		context.SetFillColour(handle_outline, 0.85f);
		context.DrawCircle(canvas_center, center_radius + 1.0f);
		context.SetLineColour(handle_colour, 1.0f, 1);
		context.SetFillColour(handle_colour, 0.9f);
		context.DrawCircle(canvas_center, center_radius);
	}
}

void VisualToolMeasure::DrawWithContext(VideoOverlayDrawContext& context) {
	if (submode == SubMode::PerspectiveQuad) {
		if (perspective_solve_state == PerspectiveSolveState::Pending) {
			UpdatePerspectivePreview();
			UpdateToolbarState();
		}
		DrawPerspective(context);
		return;
	}

	if (preview_guide) {
		VisualGuideOverlay overlay;
		overlay.DrawGuide(
			context,
			parent->GetVisualGuideViewport(),
			*preview_guide,
			true,
			MakeOverlayStyle(line_color_primary_opt, highlight_color_primary_opt));
		DrawHandles(context, *preview_guide);
		return;
	}

	if (!controller)
		return;
	auto const view = controller->CaptureView();
	if (!view.selected_id)
		return;
	if (auto const* guide = FindGuide(view, *view.selected_id))
		DrawHandles(context, *guide);
}

void VisualToolMeasure::Draw() {
	LegacyVideoOverlayDrawContext context(gl, *text);
	DrawWithContext(context);
}

void VisualToolMeasure::DrawOverlay(VideoOverlayDrawContext& context) {
	DrawWithContext(context);
}

void VisualToolMeasure::OnSubTool(wxCommandEvent& event) {
	if (event.GetId() == segment_button) {
		SetSubMode(static_cast<int>(SubMode::Segment));
		return;
	}
	if (event.GetId() == perspective_button) {
		SetSubMode(static_cast<int>(SubMode::PerspectiveQuad));
		return;
	}
	if (event.GetId() == clear_button && submode == SubMode::Segment) {
		CancelInteraction();
		if (controller)
			controller->Clear();
		return;
	}
	if (event.GetId() == clear_target_button
		&& submode == SubMode::PerspectiveQuad) {
		CancelPerspectiveInteraction(false);
		(void)perspective_state.DropTarget();
		bool const had_diagnostic = !perspective_diagnostic.empty();
		perspective_diagnostic.clear();
		perspective_diagnostic_depends_on_solver_options = false;
		if (had_diagnostic)
			c->ShowStatus({});
		UpdateToolbarState();
		parent->Render();
		return;
	}
	if (event.GetId() == apply_button && submode == SubMode::PerspectiveQuad) {
		ApplyPerspective();
		return;
	}
	if (event.GetId() == fit_button && submode == SubMode::PerspectiveQuad) {
		bool const next = event.IsChecked();
		if (fit_text_to_target == next)
			return;
		fit_text_to_target = next;
		OPT_SET(kPerspectiveFitTextOption)->SetBool(fit_text_to_target);
		RefreshAfterSolverOptionChange();
		return;
	}
	if (event.GetId() == fax_frz_only_button && submode == SubMode::PerspectiveQuad) {
		bool const next = event.IsChecked();
		if (fax_frz_only == next)
			return;
		fax_frz_only = next;
		OPT_SET(kPerspectiveFaxFrzOnlyOption)->SetBool(fax_frz_only);
		RefreshAfterSolverOptionChange();
		return;
	}
	if (event.GetId() != reset_button || submode != SubMode::PerspectiveQuad)
		return;

	CancelPerspectiveInteraction(false);
	(void)perspective_state.UseCurrent();
	bool const had_diagnostic = !perspective_diagnostic.empty();
	perspective_diagnostic.clear();
	perspective_diagnostic_depends_on_solver_options = false;
	if (had_diagnostic)
		c->ShowStatus({});
	UpdateToolbarState();
	parent->Render();
}

void VisualToolMeasure::PopulateToolbar() {
	if (!toolbar)
		return;

	toolbar->ClearTools();
	segment_button = wxID_NONE;
	perspective_button = wxID_NONE;
	clear_button = wxID_NONE;
	clear_target_button = wxID_NONE;
	reset_button = wxID_NONE;
	fit_button = wxID_NONE;
	fax_frz_only_button = wxID_NONE;
	apply_button = wxID_NONE;
	toolbar_helps_valid = false;

	int const icon_size = GetVideoUiIconSize(toolbar, OPT_GET("App/Toolbar Icon Size")->GetInt());
	toolbar->SetToolBitmapSize(wxSize(icon_size, icon_size));
	toolbar->AddSeparator();
	// The two submode toggles are always visible; each mode adds only its own
	// action buttons so nothing sits around greyed out.
	segment_button = toolbar->AddTool(
		wxID_ANY, _("Segment"),
		wxBitmapBundle::FromBitmap(CMD_ICON_GET(visual_vector_clip_line, wxLayout_Default, icon_size)),
		_("Measure a line segment"), wxITEM_CHECK)->GetId();
	perspective_button = toolbar->AddTool(
		wxID_ANY, _("Perspective Quad"),
		wxBitmapBundle::FromBitmap(CMD_ICON_GET(visual_vector_clip_drag, wxLayout_Default, icon_size)),
		_("Draw a target quadrilateral for the active subtitle"), wxITEM_CHECK)->GetId();
	toolbar->AddSeparator();
	if (submode == SubMode::Segment) {
		clear_button = toolbar->AddTool(
			wxID_ANY, _("Clear all guides"),
			wxBitmapBundle::FromBitmap(CMD_ICON_GET(delete_button, wxLayout_Default, icon_size)),
			_("Remove all visual guides"))->GetId();
	}
	else {
		reset_button = toolbar->AddTool(
			wxID_ANY, _("Use Current Subtitle Quad"),
			wxBitmapBundle::FromBitmap(CMD_ICON_GET(undo_button, wxLayout_Default, icon_size)),
			_("Initialize the target from the current subtitle geometry"))->GetId();
		fit_button = toolbar->AddTool(
								wxID_ANY, _("Fit Text"),
								wxBitmapBundle::FromBitmap(CMD_ICON_GET(visual_scale, wxLayout_Default, icon_size)),
								_("Scale text to fit the target; off matches the shape at the current font size"),
								wxITEM_CHECK)
						 ->GetId();
		fax_frz_only_button = toolbar->AddTool(
			wxID_ANY, _("Fax + Frz Only"),
			wxBitmapBundle::FromBitmap(CMD_ICON_GET(visual_rotatez, wxLayout_Default, icon_size)),
			_("Allow position and scale changes, but restrict Perspective to fax and frz"),
			wxITEM_CHECK)->GetId();
		apply_button = toolbar->AddTool(
			wxID_ANY, _("Apply Perspective Quad"),
			wxBitmapBundle::FromBitmap(CMD_ICON_GET(button_audio_commit, wxLayout_Default, icon_size)),
			_("Apply Perspective tags"))->GetId();
		clear_target_button = toolbar->AddTool(
			wxID_ANY, _("Clear Target"),
			wxBitmapBundle::FromBitmap(CMD_ICON_GET(delete_button, wxLayout_Default, icon_size)),
			_("Discard the target quadrilateral"))->GetId();
	}
	toolbar->Realize();
	toolbar->Show(true);
	UpdateToolbarState();
}

void VisualToolMeasure::SetToolbar(wxToolBar *new_toolbar) {
	if (toolbar)
		toolbar->Unbind(wxEVT_TOOL, &VisualToolMeasure::OnSubTool, this);
	toolbar = new_toolbar;
	if (!toolbar)
		return;
	toolbar->Bind(wxEVT_TOOL, &VisualToolMeasure::OnSubTool, this);
	PopulateToolbar();
}

bool VisualToolMeasure::SetSubMode(int mode) {
	if (mode < static_cast<int>(SubMode::Segment)
		|| mode > static_cast<int>(SubMode::PerspectiveQuad))
		return false;

	auto const new_mode = static_cast<SubMode>(mode);
	if (new_mode == submode) {
		UpdateToolbarState();
		return true;
	}

	CancelInteraction(false);
	CancelPerspectiveInteraction(false);
	perspective_state.Clear();
	perspective_source.reset();
	perspective_diagnostic.clear();
	perspective_diagnostic_depends_on_solver_options = false;
	ClearPerspectivePreview();
	submode = new_mode;
	if (submode == SubMode::PerspectiveQuad)
		RebindPerspective(PerspectiveRebindMode::DiscardTarget, true);
	else
		UpdateToolbarState();
	PopulateToolbar();
	parent->Render();
	return true;
}

void VisualToolMeasure::RefreshAfterSolverOptionChange() {
	if (perspective_diagnostic_depends_on_solver_options) {
		perspective_diagnostic.clear();
		perspective_diagnostic_depends_on_solver_options = false;
	}
	UpdatePerspectivePreview();
	c->ShowStatus(perspective_diagnostic);
	UpdateToolbarState();
	parent->Render();
}

void VisualToolMeasure::UpdateToolbarState() {
	if (!toolbar)
		return;

	if (segment_button != wxID_NONE)
		toolbar->ToggleTool(segment_button, submode == SubMode::Segment);
	if (perspective_button != wxID_NONE)
		toolbar->ToggleTool(perspective_button, submode == SubMode::PerspectiveQuad);
	// Re-setting the help text recreates the Win32 tooltip, so only push it
	// when the diagnostic actually changed instead of on every mouse motion.
	std::string const diagnostic = LocalizedDiagnostic();
	if (!toolbar_helps_valid || diagnostic != toolbar_diagnostic) {
		toolbar_diagnostic = diagnostic;
		toolbar_helps_valid = true;
		if (perspective_button != wxID_NONE)
			toolbar->SetToolShortHelp(perspective_button,
									  diagnostic.empty()
										  ? _("Drag empty video space to create a target; drag corners "
											  "to adjust it; Alt+click an edge to hold it in place")
										  : to_wx(diagnostic));
		if (apply_button != wxID_NONE)
			toolbar->SetToolShortHelp(apply_button,
				diagnostic.empty() ? _("Apply Perspective tags") : to_wx(diagnostic));
	}
	if (clear_button != wxID_NONE)
		toolbar->EnableTool(clear_button, submode == SubMode::Segment && controller != nullptr);
	if (clear_target_button != wxID_NONE)
		toolbar->EnableTool(clear_target_button,
			submode == SubMode::PerspectiveQuad
				&& perspective_state.IsEditable()
				&& perspective_state.HasTarget()
				&& !IsPerspectiveInteractionActive());
	if (reset_button != wxID_NONE)
		toolbar->EnableTool(reset_button,
			submode == SubMode::PerspectiveQuad
				&& perspective_state.CanInitializeFromCurrent()
				&& !IsPerspectiveInteractionActive());
	if (fit_button != wxID_NONE) {
		toolbar->ToggleTool(fit_button, fit_text_to_target);
		toolbar->EnableTool(fit_button, submode == SubMode::PerspectiveQuad);
	}
	if (fax_frz_only_button != wxID_NONE) {
		toolbar->ToggleTool(fax_frz_only_button, fax_frz_only);
		toolbar->EnableTool(
			fax_frz_only_button, submode == SubMode::PerspectiveQuad);
	}
	if (apply_button != wxID_NONE) {
		toolbar->EnableTool(apply_button, CanApplyPerspective());
	}
}

void VisualToolMeasure::OnFileChanged() {
	CancelInteraction(false);
	CancelPerspectiveInteraction(false);
	if (submode != SubMode::PerspectiveQuad)
		return;

	if (command_session.IsLocalCommitInProgress()) {
		RebindPerspective(PerspectiveRebindMode::AcceptCurrent);
		return;
	}

	bool preserve_target = false;
	if (perspective_source && active_line) {
		// A target may outlive content changes, but never a file or line object
		// replacement: the recaptured fingerprint must still identify this source.
		auto core = c->GetCore();
		auto const& fingerprint = perspective_source->fingerprint;
		preserve_target = fingerprint.file_identity
				== reinterpret_cast<std::uintptr_t>(core.ass.get())
			&& fingerprint.line_identity
				== reinterpret_cast<std::uintptr_t>(active_line)
			&& fingerprint.line.id == active_line->Id;
	}
	RebindPerspective(preserve_target
		? PerspectiveRebindMode::PreserveTarget
		: PerspectiveRebindMode::DiscardTarget);
}

void VisualToolMeasure::OnFrameChanged() {
	CancelInteraction(false);
	CancelPerspectiveInteraction(false);
	// The same line's geometry is frame-invariant unless it is animated, and
	// animated geometry is already read-only, so an edited target must survive
	// seeks instead of being discarded.
	if (submode == SubMode::PerspectiveQuad)
		RebindPerspective(PerspectiveRebindMode::PreserveTarget);
}

void VisualToolMeasure::OnLineChanged() {
	CancelInteraction(false);
	CancelPerspectiveInteraction(false);
	if (submode == SubMode::PerspectiveQuad)
		// The selected line re-entering its display range must restore a
		// parked target; a real line switch must discard it.
		RebindPerspective(SelectedLineMatchesSource()
			? PerspectiveRebindMode::PreserveTarget
			: PerspectiveRebindMode::DiscardTarget);
}

void VisualToolMeasure::OnCoordinateSystemsChanged() {
	CancelInteraction(false);
	CancelPerspectiveInteraction(false);
	if (submode == SubMode::PerspectiveQuad)
		RebindPerspective();
}

void VisualToolMeasure::OnDisplayAreaChanged() {
	CancelInteraction(false);
	CancelPerspectiveInteraction(false);
}

void VisualToolMeasure::OnMouseCaptureLost(wxMouseCaptureLostEvent& event) {
	CancelInteraction(false);
	CancelPerspectiveInteraction(false);
	VisualToolBase::OnMouseCaptureLost(event);
}
