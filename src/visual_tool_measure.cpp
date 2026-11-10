#include "visual_tool_measure.h"
#include "visual_tool_measure_shared.h"

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
constexpr float kHitTolerance = 5.0f;
constexpr auto kPerspectiveFitTextOption = "Tool/Visual/Perspective/Fit Text";
constexpr auto kPerspectiveBestFitOption = "Tool/Visual/Perspective/Best Fit";
constexpr auto kPerspectiveFaxFrzOnlyOption = "Tool/Visual/Perspective/Fax Frz Only";
constexpr auto kPerspectiveDecimalPlacesOption = "Tool/Visual/Perspective/Decimal Places";
constexpr auto kPerspectiveShapeToleranceOption = "Tool/Visual/Perspective/Shape Tolerance";
// Tag-rounding budget in output pixels: it bounds the serialization error the
// written digits may add. How far the reachable shape sits from the drawn one
// is not budgeted at all -- the nearest shape is always returned and its
// shortfall is drawn by the preview.
constexpr double kPerspectiveMaxError = aegisub::visual_measure::PerspectiveMaxError;

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
	style.line_colour = line_colour->GetColor();
	style.highlight_colour = highlight_colour->GetColor();
	// Fixed white label border for contrast; the selected state borrows the
	// highlight colour instead. Not user-configurable to avoid a new option.
	style.label_border_colour = {255, 255, 255};
	style.label_font_size = OPT_GET("Tool/Visual/Coordinate Font Size")->GetInt();
	return style;
}

wxString MaximumGuidesStatusMessage() {
	return fmt_tl(
		"A maximum of %d visual guides is allowed.",
		static_cast<int>(VisualGuideController::MaximumGuideCount));
}

using aegisub::visual_measure::DescribeEdgeAnchor;
using aegisub::visual_measure::DescribePerspectiveFailure;
using aegisub::visual_measure::EdgeAnchorForHandle;
using aegisub::visual_measure::LocalizedAssApplyBlocker;
using aegisub::visual_measure::LocalizedGeometryError;
using aegisub::visual_measure::LocalizedPlanError;
using aegisub::visual_measure::LocalizedQuadEditError;
}

VisualToolMeasure::VisualToolMeasure(VideoDisplay *parent, agi::Context *context)
	: VisualToolBase(parent, context), controller(context->GetUI().visualGuideController), text(parent->CreateTextRenderer()), fit_text_to_target(OPT_GET(kPerspectiveFitTextOption)->GetBool()), best_fit(OPT_GET(kPerspectiveBestFitOption)->GetBool()), fax_frz_only(OPT_GET(kPerspectiveFaxFrzOnlyOption)->GetBool()), perspective_decimal_places(perspective::ClampPerspectiveDecimalPlaces(
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
	subscribe_solver_option(kPerspectiveBestFitOption, best_fit);
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
	return aegisub::visual_measure::UnreachableTargetDiagnostic(perspective_no_feasible_reason,
																perspective_no_feasible_metrics, perspective_decimal_places);
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

	auto const solved = aegisub::visual_measure::SolvePerspectivePreview(*perspective_source, *target,
																		 context->output_mapping, {.fit_text = fit_text_to_target, .best_fit = best_fit, .fax_frz_only = fax_frz_only, .decimal_places = perspective_decimal_places, .shape_tolerance = perspective_shape_tolerance, .edge_anchor = perspective_edge_anchor});
	if (!solved.feasible) {
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

	perspective_preview = solved.reachable;
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
				if (best_fit) {
					c->ShowStatus(from_wx(_("Best Fit (No Reduction) ignores held edges; the previous edge selection is kept.")));
					return;
				}
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
		perspective_shape_tolerance,
		best_fit);
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
	c->ShowStatus(aegisub::visual_measure::DescribePerspectiveApplySuccess(*planned.plan, drawn_target, *current_context));
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
	aegisub::visual_measure::DrawGuideHandles(context, parent->GetVisualGuideViewport(), guide,
											  MakeOverlayStyle(line_color_primary_opt, highlight_color_primary_opt));
}

void VisualToolMeasure::DrawPerspective(VideoOverlayDrawContext& context) {
	auto const preview_validation = perspective_creation
										? std::optional<perspective::GeometryValidation>(perspective::ValidateQuad(perspective_creation->preview))
										: std::nullopt;
	auto const& validation = perspective_state.Validation();
	aegisub::visual_measure::PerspectiveOverlay overlay{
		.current = perspective_state.Current(),
		.target = perspective_creation ? std::optional<perspective::Quad>(perspective_creation->preview) : perspective_state.Target(),
		.reachable = perspective_preview,
		.creating = perspective_creation.has_value(),
		.editable = perspective_state.IsEditable(),
		.off_frame = perspective_state.IsBound() && !active_line,
		.modified = perspective_state.IsModified(),
		.valid = preview_validation ? static_cast<bool>(*preview_validation) : validation && static_cast<bool>(*validation),
		.best_fit = best_fit,
		.active_handle = perspective_state.ActiveHandle(),
		.edge_anchor = perspective_edge_anchor,
		.diagnostic = LocalizedDiagnostic()};
	aegisub::visual_measure::PerspectiveOverlayStyle style{
		.line_primary = line_color_primary_opt->GetColor(), .line_secondary = line_color_secondary_opt->GetColor(), .highlight_primary = highlight_color_primary_opt->GetColor(), .highlight_secondary = highlight_color_secondary_opt->GetColor(), .invalid_line = invalid_line_color_opt->GetColor(), .invalid_handle = invalid_handle_color_opt->GetColor(), .label_font_size = static_cast<int>(OPT_GET("Tool/Visual/Coordinate Font Size")->GetInt()), .ui_scale = UiScale()};
	aegisub::visual_measure::DrawPerspective(context, parent->GetVisualGuideViewport(), canvas_size, overlay, style);
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
	if (event.GetId() == best_fit_button && submode == SubMode::PerspectiveQuad) {
		bool const next = event.IsChecked();
		if (best_fit == next)
			return;
		best_fit = next;
		OPT_SET(kPerspectiveBestFitOption)->SetBool(best_fit);
		RefreshAfterSolverOptionChange();
		return;
	}
	if (event.GetId() == fax_frz_only_button && submode == SubMode::PerspectiveQuad && !best_fit) {
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
	best_fit_button = wxID_NONE;
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
		best_fit_button = toolbar->AddTool(
									 wxID_ANY, _("Best Fit (No Reduction)"),
									 wxBitmapBundle::FromBitmap(CMD_ICON_GET(visual_rotatexy, wxLayout_Default, icon_size)),
									 _("Choose the best geometric fit without reducing tags. Only Fit Text and Decimal Places apply; "
									   "Fax + Frz Only, Shape Tolerance, and held edges are ignored without changing their saved values."),
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
	toolbar_helps_valid = false;
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
										  ? (best_fit
												 ? _("Drag empty video space to create a target; drag corners "
													 "to adjust it. Best Fit (No Reduction) ignores held edges.")
												 : _("Drag empty video space to create a target; drag corners "
													 "to adjust it; Alt+click an edge to hold it in place"))
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
	if (best_fit_button != wxID_NONE) {
		toolbar->ToggleTool(best_fit_button, best_fit);
		toolbar->EnableTool(best_fit_button, submode == SubMode::PerspectiveQuad);
	}
	if (fax_frz_only_button != wxID_NONE) {
		toolbar->ToggleTool(fax_frz_only_button, fax_frz_only);
		toolbar->EnableTool(
			fax_frz_only_button, submode == SubMode::PerspectiveQuad && !best_fit);
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
