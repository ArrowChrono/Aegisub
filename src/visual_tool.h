// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
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
//
// Aegisub Project http://www.aegisub.org/

/// @file visual_tool.h
/// @see visual_tool.cpp
/// @ingroup visual_ts

#pragma once

#include "gl_wrap.h"
#include "deadline_pacing_policy.h"
#include "vector2d.h"
#include "options.h"
#include "subtitle_command_session.h"
#include "ui_deadline_timer.h"

#include <libaegisub/owning_intrusive_list.h>
#include <libaegisub/signal.h>

#include <set>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <wx/gdicmn.h>
#include <wx/timer.h>

class AssDialogue;
class VideoDisplay;
class VideoOverlayDrawContext;
class VisualToolRenderSnapshot;
struct VisualToolRenderContext;
class wxMouseCaptureLostEvent;
class wxKeyEvent;
class wxMouseEvent;
class wxToolBar;
namespace agi {
	struct Context;
	class OptionValue;
}

/// Magnitude for keyboard nudge of visual typesetting tools.
enum class VisualNudgeMagnitude {
	Normal,
	Large
};

/// @class VisualToolBase
/// @brief Base class for visual tools containing all functionality that doesn't interact with features
///
/// This is required so that visual tools can be used polymorphically, as
/// different VisualTool<T>s are unrelated types otherwise. In addition, as much
/// functionality as possible is implemented here to avoid having four copies
/// of each method for no good reason (and four times as many error messages)
class VisualToolBase {
	void OnCommit(int type, AssDialogue const* changed);
	void OnSeek(int new_frame);
	bool CancelInteraction(bool release_capture);
	void UpdateScriptResolution();
	void UpdateLayoutResolution();

	/// @brief Get the dialogue line currently in the edit box
	/// @return nullptr if the line is not active on the current frame
	AssDialogue *GetActiveDialogueLine();

	// SubtitleSelectionListener implementation
	void OnActiveLineChanged(AssDialogue *new_line);

	// Below here are the virtuals that must be implemented

	/// Called when the script, video or screen resolutions change
	virtual void OnCoordinateSystemsChanged() { DoRefresh(); }
	/// Called when only the canvas-space video rectangle changes. Tools which
	/// store canvas-space state retain the coordinate-system refresh behavior.
	virtual void OnDisplayAreaChanged() { OnCoordinateSystemsChanged(); }

	/// Called when the file changes and the tool needs to resync from script state
	virtual void OnFileChanged() { DoRefresh(); }
	/// Transient tools can request a refresh for external commits which the
	/// generic active/displayed-line filter would otherwise ignore.
	virtual bool ShouldRefreshOnAnyExternalCommit() const { return false; }

	/// Called when the frame number changes
	virtual void OnFrameChanged() { }

	/// Called when the active line changes
	virtual void OnLineChanged() { DoRefresh(); }

	/// Generic refresh to simplify tools which have no interesting state and
	/// can simply do do the same thing for any external change (i.e. most of
	/// them). Called only by the above virtual methods.
	virtual void DoRefresh() { }

protected:
	std::vector<agi::signal::Connection> connections;

	/// Called if a mouse interaction is interrupted by wx losing capture. Tools
	/// which keep their own edit transaction can override this to roll it back.
	virtual void OnMouseCaptureLost(wxMouseCaptureLostEvent &);

	void OnCommitIdResetTimer(wxTimerEvent &);

	OpenGLWrapper gl;

	/// Called when the user double-clicks
	virtual void OnDoubleClick() { }

	agi::Context *c;
	VideoDisplay *parent;

	bool holding = false; ///< Is a hold currently in progress?
	AssDialogue *active_line = nullptr; ///< Active dialogue line; nullptr if it is not visible on the current frame
	bool dragging = false; ///< Is a drag currently in progress?
	bool interaction_trace_active = false;

	int frame_number; ///< Current frame number

	bool shift_down = false; ///< Is shift down?
	bool ctrl_down = false; ///< Is ctrl down?
	bool alt_down = false; ///< Is alt down?

	Vector2D mouse_pos; ///< Last seen mouse position
	Vector2D drag_start; ///< Mouse position at the beginning of the last drag
	Vector2D script_res; ///< Script resolution (PlayRes)
	Vector2D layout_res; ///< Layout resolution (for \frx/\fry perspective preview)
	Vector2D canvas_size; ///< Size of the display canvas
	Vector2D video_pos; ///< Top-left corner of the video in the display area
	Vector2D video_res; ///< Video resolution

	const agi::OptionValue *highlight_color_primary_opt;
	const agi::OptionValue *highlight_color_secondary_opt;
	const agi::OptionValue *line_color_primary_opt;
	const agi::OptionValue *line_color_secondary_opt;
	const agi::OptionValue *shaded_area_alpha_opt;

	aegisub::SubtitleCommandSession command_session;
	std::vector<AssDialogue const *> changed_lines;
	std::unordered_set<AssDialogue const *> changed_line_set;
	AssDialogue *single_changed_line = nullptr;
	agi::signal::Connection file_changed_connection;
	UiDeadlineTimer interaction_render_timer;
	// Keep this aligned with visual_subtitle_update_pacer; Final bypasses both gates.
	DeadlinePacingPolicy interaction_render_pacer{std::chrono::milliseconds(17)};
	int commit_id_reset_timer_id; ///< Distinct from other timers on VideoDisplay
	wxTimer commit_id_reset_timer; ///< Splits keyboard-nudge undo after idle
	void OnInteractionRenderTimer();
	void ArmInteractionRenderTimer();
	void RenderInteractionFrame(int reason);
	void BeginInteractionPacing(int selection_count);
	void EndInteractionPacing(bool render_final, int selection_count = -1);

	/// @brief Identify the line to pass to AssFile::Commit when exactly one line changed
	virtual AssDialogue *GetCommitTargetLine() const;

	/// @brief Commit the current file state
	/// @param message Description of changes for undo
	virtual void Commit(wxString message = wxString());
	void CommitAndRefresh(wxString message = wxString());
	bool IsDisplayed(AssDialogue const* line) const;

	/// Commit a keyboard nudge and schedule undo-coalesce reset after idle.
	void CommitNudge(wxString message = wxString());
	[[nodiscard]] static double GetNudgeStep(
		char const* option,
		char const* large_option,
		VisualNudgeMagnitude magnitude);

	/// Get the line's position if it's set, or it's default based on style if not
	Vector2D GetLinePosition(AssDialogue *diag);
	/// Get the line's origin if it's set, or Vector2D::Bad() if not
	Vector2D GetLineOrigin(AssDialogue *diag);
	bool GetLineMove(AssDialogue *diag, Vector2D &p1, Vector2D &p2, int &t1, int &t2);
	void GetLineRotation(AssDialogue *diag, float &rx, float &ry, float &rz);
	void GetLineShear(AssDialogue *diag, float& fax, float& fay);
	void GetLineScale(AssDialogue *diag, Vector2D &scale);
	void GetLineClip(AssDialogue *diag, Vector2D &p1, Vector2D &p2, bool &inverse);
	std::string GetLineVectorClip(AssDialogue *diag, int &scale, bool &inverse);

	void ClearChangedLines();
	void SetOverride(AssDialogue* line, std::string const& tag, std::string const& value);
	void SetSelectedOverride(std::string const& tag, std::string const& value);

	VisualToolBase(VideoDisplay *parent, agi::Context *context);

public:
	/// Convert a point from video to script coordinates
	Vector2D ToScriptCoords(Vector2D point) const;
	/// Convert a point from script to video coordinates
	Vector2D FromScriptCoords(Vector2D point) const;

	// Stuff called by VideoDisplay
	virtual void OnMouseEvent(wxMouseEvent &event)=0;
	/// Return true when the tool consumed a keyboard event.
	virtual bool OnKeyDown(wxKeyEvent &) { return false; }
	/// Apply a keyboard nudge. Return true when the tool applied the change.
	virtual bool Nudge(Vector2D /*direction*/, VisualNudgeMagnitude /*magnitude*/) { return false; }
	/// Whether this tool implements keyboard nudge (not merely has a hotkey context).
	virtual bool SupportsNudge() const { return false; }
	/// Request a paced redraw after an interaction-related subtitle packet arrives.
	void ScheduleInteractionRender();
	void RenderReadyInteractionFrame();
	/// A holding tool can commit its release state before ending its pacing session.
	[[nodiscard]] bool IsFinishingLocalInteractionCommit() const noexcept {
		return !IsInteracting() && interaction_render_pacer.IsActive() && command_session.IsLocalCommitInProgress();
	}
	/// Select a tool-specific sub-mode. Return true when applied.
	virtual bool SetSubMode(int /*mode*/) { return false; }
	/// Current sub-mode, or -1 when the tool has none.
	virtual int GetSubMode() const { return -1; }
	/// Hotkey context owned by this tool; empty means it has none.
	virtual std::string GetHotkeyContext() const { return {}; }
	/// Cursor this tool wants over the video while it is idle. wxCURSOR_NONE
	/// means the platform default. The display owns the canvas cursor and asks
	/// for this, so a tool must never set the parent's cursor itself: a
	/// host-owned mode (a point-selection session) has to be able to show its
	/// own cursor and hand the tool's back afterwards.
	virtual wxStockCursor GetIdleCursor() const { return wxCURSOR_NONE; }
	/// Whether the tool currently has an active (on-frame) dialogue line.
	bool HasActiveLine() const { return active_line != nullptr; }
	virtual void Draw()=0;
	virtual std::shared_ptr<const VisualToolRenderSnapshot> CaptureRenderSnapshot(
		std::shared_ptr<const VisualToolRenderContext> const&) const { return {}; }
	virtual bool SupportsOverlayContext() const { return false; }
	virtual void DrawOverlay(VideoOverlayDrawContext &) { }
	virtual void SetCanvasSize(int w, int h);
	virtual void SetDisplayArea(int x, int y, int w, int h);
	virtual void SetToolbar(wxToolBar *) { }
	bool IsInteracting() const noexcept { return holding || dragging; }
	virtual ~VisualToolBase();
};

/// Visual tool base class containing all common feature-related functionality
template<class FeatureType>
class VisualTool : public VisualToolBase {
protected:
	typedef FeatureType Feature;
	typedef agi::owning_intrusive_list<FeatureType> feature_list;

private:
	bool sel_changed = false; /// Has the selection already been changed in the current click?
	std::set<AssDialogue *> double_click_selection; ///< Selection targeted by the current click sequence

	/// @brief Called when a hold is begun
	/// @return Should the hold actually happen?
	virtual bool InitializeHold() { return false; }
	/// @brief Called on every mouse event during a hold
	virtual void UpdateHold() { }

	/// @brief Called at the beginning of a drag
	/// @param feature The visual feature clicked on
	/// @return Should the drag happen?
	virtual bool InitializeDrag(FeatureType *feature) { return true; }
	/// @brief Called on every mouse event during a drag
	/// @param feature The current feature to process; not necessarily the one clicked on
	virtual void UpdateDrag(FeatureType *feature) { }

protected:
	std::set<FeatureType *> sel_features; ///< Currently selected visual features

	/// Restore the selection captured before a completed click narrowed it.
	void RestoreDoubleClickSelection();

	/// Topmost feature under the mouse; generally only valid during a drag
	FeatureType *active_feature = nullptr;
	/// List of features which are drawn and can be clicked on
	/// List is used here for the iterator invalidation properties
	feature_list features;

	/// Draw all of the features in the list
	void DrawAllFeatures();
	void DrawAllFeatures(VideoOverlayDrawContext &context);

	/// @brief Remove a feature from the selection
	/// @param i Index in the feature list
	/// Also deselects lines if all features for that line have been deselected
	void RemoveSelection(FeatureType *feat);

	/// @brief Set the selection to a single feature, deselecting everything else
	/// @param i Index in the feature list
	void SetSelection(FeatureType *feat, bool clear);

public:
	/// @brief Handler for all mouse events
	/// @param event Shockingly enough, the mouse event
	void OnMouseEvent(wxMouseEvent &event) override;

	/// @brief Constructor
	/// @param parent The VideoDisplay to use for coordinate conversion
	/// @param video Video and mouse information passing blob
	VisualTool(VideoDisplay *parent, agi::Context *context);
};
