#pragma once

#include "perspective_apply_plan.h"
#include "perspective_quad_edit_state.h"
#include "visual_guide_controller.h"
#include "visual_tool.h"

#include <memory>
#include <optional>
#include <string>

class OpenGLText;
class VideoOverlayDrawContext;
class wxCommandEvent;
class wxKeyEvent;
class wxMouseCaptureLostEvent;
class wxMouseEvent;
class wxToolBar;

/// Interactive editor for temporary visual measurement guides and the active
/// line's Perspective quad. Segment guides never touch subtitle data;
/// Perspective changes are committed only by the explicit Apply command.
class VisualToolMeasure final : public VisualToolBase {
	enum class SubMode {
		Segment,
		PerspectiveQuad,
	};

	enum class Interaction {
		None,
		CreatingMeasurement,
		DraggingFirstEndpoint,
		DraggingSecondEndpoint,
		DraggingLine,
	};
	enum class PerspectiveRebindMode {
		DiscardTarget,
		PreserveTarget,
		AcceptCurrent,
	};

	std::shared_ptr<VisualGuideController> controller;
	std::unique_ptr<OpenGLText> text;
	wxToolBar *toolbar = nullptr;
	SubMode submode = SubMode::Segment;
	Interaction interaction = Interaction::None;
	std::optional<std::string> editing_id;
	std::optional<VisualGuide> original_guide;
	std::optional<VisualGuide> preview_guide;
	VisualGuidePoint drag_start_point;
	perspective::PerspectiveQuadEditState perspective_state;
	std::optional<perspective::PerspectiveSourceSnapshot> perspective_source;
	// The anchor, its canvas-space twin (for click-vs-drag) and the preview
	// quad are one gesture: they always appear and disappear together.
	struct PerspectiveCreationGesture {
		perspective::Vec2 anchor;
		Vector2D canvas_anchor;
		perspective::Quad preview;
	};
	std::optional<PerspectiveCreationGesture> perspective_creation;
	std::string perspective_diagnostic;
	bool perspective_diagnostic_depends_on_solver_options = false;
	bool fit_text_to_target;
	bool best_fit;
	bool fax_frz_only;
	int perspective_decimal_places;
	double perspective_shape_tolerance;
	// Which drawn edge the user has marked as trustworthy, if any. Alt+click on
	// an edge midpoint toggles it. Per-binding rather than persistent: it
	// describes this particular quad against this particular frame.
	perspective::PerspectiveEdgeAnchor perspective_edge_anchor =
		perspective::PerspectiveEdgeAnchor::None;
	// Verdict of the preview solve behind perspective_preview. Gates the Apply
	// button so a target the solver already rejected never offers an Apply that
	// must fail; NotReady also covers early exits where nothing was solved and
	// solver errors that say nothing about reachability. Apply re-solves and
	// stays the final authority either way.
	enum class PerspectiveSolveState {
		NotReady,
		Pending,
		Feasible,
		Infeasible,
	};
	PerspectiveSolveState perspective_solve_state = PerspectiveSolveState::NotReady;
	// Stage the last infeasible solve died at, as classified by the solver
	// itself rather than guessed from the option flags. Recorded regardless of
	// which slot owns the diagnostic, so both the live preview and a failed
	// Apply can phrase the message against the option that would help.
	perspective::NoFeasibleReason perspective_no_feasible_reason =
		perspective::NoFeasibleReason::None;
	// The closest rejected candidate's deviation and the budget it was judged
	// against, when the last infeasible solve could measure them. Feeds real
	// numbers into UnreachableTargetDiagnostic so the user can dial the named
	// option to a value that accepts the shape instead of guessing upward.
	std::optional<perspective::NoFeasibleMetrics> perspective_no_feasible_metrics;
	// The shape the tags can actually reach for the current target. Hand-dragged
	// quads are almost never exactly representable, so the reachable shape is
	// drawn alongside the target instead of only being reported on failure.
	std::optional<perspective::Quad> perspective_preview;
	// Diagnostics already pushed into the toolbar helps; avoids re-setting the
	// Win32 tooltip text on every mouse motion.
	std::string toolbar_diagnostic;
	bool toolbar_helps_valid = false;
	const agi::OptionValue *invalid_line_color_opt;
	const agi::OptionValue *invalid_handle_color_opt;

	// wxID_NONE, not -1: these hold ids handed back by AddTool(wxID_ANY, ...),
	// which wx auto-allocates from [wxID_AUTO_LOWEST, wxID_AUTO_HIGHEST] --
	// negative. A -1 sentinel tested with >= 0 therefore reads every real button
	// as absent and silently skips the whole state update. wxID_NONE is outside
	// the auto range, so `!= wxID_NONE` distinguishes them correctly.
	int segment_button = wxID_NONE;
	int perspective_button = wxID_NONE;
	int clear_button = wxID_NONE;
	int clear_target_button = wxID_NONE;
	int reset_button = wxID_NONE;
	int fit_button = wxID_NONE;
	int best_fit_button = wxID_NONE;
	int fax_frz_only_button = wxID_NONE;
	int apply_button = wxID_NONE;

	[[nodiscard]] std::optional<VisualGuidePoint> CurrentGuidePoint(bool clamp) const;
	[[nodiscard]] bool IsInsideVideoViewport(Vector2D point) const;
	/// Canvas-pixel tolerances and handle radii scale with the display DPI so
	/// handles stay grabbable on high-DPI screens.
	[[nodiscard]] double UiScale() const;
	/// The user-facing message for the toolbar help, status bar and overlay.
	[[nodiscard]] std::string LocalizedDiagnostic() const;

	void BeginInteraction(wxMouseEvent& event);
	void UpdateInteraction();
	void FinishInteraction();
	void CancelInteraction(bool render = true);
	[[nodiscard]] std::optional<perspective::Vec2> CurrentPerspectivePoint() const;
	void BeginPerspectiveInteraction();
	void UpdatePerspectiveInteraction();
	void FinishPerspectiveInteraction();
	void CancelPerspectiveInteraction(bool render = true);
	[[nodiscard]] bool IsPerspectiveInteractionActive() const noexcept;
	[[nodiscard]] std::optional<perspective::PerspectiveApplyContext>
		CurrentPerspectiveApplyContext() const;
	/// Whether the grid's selected line is still the captured source line;
	/// distinguishes "same line, off its frame range" from a real line switch.
	[[nodiscard]] bool SelectedLineMatchesSource() const;
	void ClearPerspectiveBinding();
	void RebindPerspective(
		PerspectiveRebindMode mode = PerspectiveRebindMode::DiscardTarget,
		bool announce_diagnostic = false);
	void PopulateToolbar();
	void UpdateToolbarState();
	/// Re-solves the current target after Fit Text, Fax+Frz Only or decimal
	/// places change. The toolbar click path used to update
	/// the cached flag, persist the option, then only clear the old message --
	/// which both skipped the preview refresh and made the option subscription
	/// a no-op, because the cache already matched.
	void RefreshAfterSolverOptionChange();
	void DrawWithContext(VideoOverlayDrawContext& context);
	void DrawHandles(VideoOverlayDrawContext& context, VisualGuide const& guide);
	void DrawPerspective(VideoOverlayDrawContext& context);
	void DrawPerspectivePreview(
		VideoOverlayDrawContext& context, perspective::Quad const& quad);
	/// Re-solves the current target and caches the reachable shape plus, when the
	/// target is out of reach entirely, the reason. Solver-only on purpose: this
	/// runs before drawing or on explicit actions, without staging an AssFile
	/// or measuring text. Mouse motions share one pending solve per redraw.
	void UpdatePerspectivePreview();
	void ClearPerspectivePreview();
	/// Why the target cannot be reached, phrased from the solver's own
	/// classification of the stage every candidate died at, so the user knows
	/// which option to relax instead of being handed a guess.
	[[nodiscard]] std::string UnreachableTargetDiagnostic() const;
	void OnSubTool(wxCommandEvent& event);
	bool ShouldRefreshOnAnyExternalCommit() const override {
		return submode == SubMode::PerspectiveQuad;
	}
	void OnFileChanged() override;
	void OnFrameChanged() override;
	void OnLineChanged() override;
	void OnCoordinateSystemsChanged() override;
	void OnDisplayAreaChanged() override;
	void OnMouseCaptureLost(wxMouseCaptureLostEvent& event) override;

public:
	VisualToolMeasure(VideoDisplay *parent, agi::Context *context);
	~VisualToolMeasure() override;

	void OnMouseEvent(wxMouseEvent& event) override;
	bool OnKeyDown(wxKeyEvent& event) override;
	void Draw() override;
	bool SupportsOverlayContext() const override { return true; }
	void DrawOverlay(VideoOverlayDrawContext& context) override;
	void SetToolbar(wxToolBar *toolbar) override;
	bool SetSubMode(int mode) override;
	int GetSubMode() const override { return static_cast<int>(submode); }
	bool Nudge(Vector2D direction, VisualNudgeMagnitude magnitude) override;
	bool SupportsNudge() const override;
	std::string GetHotkeyContext() const override;
	/// Whether the Perspective target is ready to be written to the line.
	[[nodiscard]] bool CanApplyPerspective() const;
	/// Write the Perspective target to the active line (no-op when not ready).
	void ApplyPerspective();
	/// Removes the selected measurement guide; false when nothing was removed.
	bool RemoveSelectedGuide();
};
