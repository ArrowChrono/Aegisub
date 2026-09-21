#pragma once

// Owns the MotionTrackSession; drives Analyze through DialogProgress with a
// Project raw-video lease; applies trajectories through the pure planner.

#include "motion_track/apply_plan.h"
#include "motion_track/apply_source.h"
#include "motion_track/session.h"
#include "motion_track/similarity_backend.h"
#include "motion_track/planar_backend.h"
#include "motion_track/types.h"

#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/spinctrl.h>
#include <wx/combobox.h>
#include <wx/textctrl.h>
#include <wx/button.h>

#include <libaegisub/signal.h>

#include <memory>
#include <typeinfo>
#include <vector>

class AsyncVideoProvider;
class AssDialogue;
namespace agi {
struct Context;
}

class DialogMotionTrack final : public wxDialog {
	agi::Context *context;
	agi::signal::Connection video_open;
	agi::signal::Connection timecodes_loaded;
	std::unique_ptr<aegisub::motion_track::MotionTrackSession> session;
	aegisub::motion_track::MotionTrackApplySource apply_source_;
	aegisub::motion_track::TranslationTrackerBackend translation_backend;
	aegisub::motion_track::SimilarityTrackerBackend similarity_backend;
	aegisub::motion_track::PlanarTrackerBackend affine_backend{aegisub::motion_track::TrackModel::Affine};
	aegisub::motion_track::PlanarTrackerBackend homography_backend{aegisub::motion_track::TrackModel::Homography};
	RawVideoIdentity last_identity{};
	int seed_time_ms = 0;
	/// Coordinate space of the ROI spin values: the ride affine at the frame
	/// they were last expressed at. Invalid while no Ok sample exists there,
	/// in which case the spin values are plain storage coordinates.
	aegisub::motion_track::RoiRideAnchor roi_anchor_;
	/// Set while SetOverlayRoi writes the spin controls, so the value-changed
	/// handler can tell a programmatic write from a user edit. SetValue does
	/// not emit wxEVT_SPINCTRL, but SetRange may coerce the value and emit
	/// wxEVT_TEXT on MSW, and re-anchoring there would overwrite the anchor
	/// the overlay drag just established.
	bool updating_roi_spins_ = false;

	/// Ghost positions of the last built apply plan, for the overlay tool:
	/// one polyline per target line, knots in storage pixels. Built by the
	/// "Plan preview" checkbox; cleared on Analyze/Apply.
	struct PlanPreviewPoint {
		float x = 0.f;
		float y = 0.f;
	};
	struct PlanPreviewData {
		std::vector<std::vector<PlanPreviewPoint>> paths;
	};
	std::shared_ptr<const PlanPreviewData> plan_preview_;

	wxSpinCtrl *roi_x = nullptr;
	wxSpinCtrl *roi_y = nullptr;
	wxSpinCtrl *roi_w = nullptr;
	wxSpinCtrl *roi_h = nullptr;
	wxComboBox *direction = nullptr;
	wxComboBox *model = nullptr;
	wxComboBox *apply_mode = nullptr;
	wxSpinCtrl *epsilon = nullptr;
	wxSpinCtrl *decimals = nullptr;
	wxSpinCtrl *smooth = nullptr;
	wxCheckBox *template_refresh = nullptr;
	wxCheckBox *stabilize = nullptr;
	wxCheckBox *growth = nullptr;
	wxCheckBox *apply_fad = nullptr;
	wxCheckBox *preview = nullptr;
	wxTextCtrl *stats = nullptr;
	wxButton *analyze_btn = nullptr;
	wxButton *apply_btn = nullptr;
	wxButton *export_btn = nullptr;
	wxButton *close_btn = nullptr;

	void OnAnalyze(wxCommandEvent&);
	void OnApply(wxCommandEvent&);
	void OnExportDebug(wxCommandEvent&);
	void OnPreviewToggle(wxCommandEvent&);
	void OnTrackingSettingsChanged();
	void OnApplyOptionsChanged();
	void InvalidatePlanPreview();
	aegisub::motion_track::TrackDirection SelectedDirection() const;
	aegisub::motion_track::TrackModel SelectedModel() const;
	/// A user edit of one of the four ROI spin controls. Re-expresses the ROI
	/// in the presented frame's space -- the numbers the user typed are the
	/// numbers they see on that frame, not values riding an older anchor --
	/// and repaints so the overlay follows the box immediately.
	void OnRoiSpin(wxCommandEvent&);
	/// Shared by Apply and Plan preview: validates the session/identity,
	/// re-resolves the lines captured at Analyze, and assembles the planner
	/// input. Shows its own error box and returns false when the run cannot
	/// proceed.
	bool BuildApplyInput(aegisub::motion_track::ApplyPlanInput& input,
						 std::vector<AssDialogue *>& targets);
	/// Repaint the video display so overlay changes become visible at once.
	void RefreshVideoDisplay();
	/// Declare the ROI spin values to be expressed in the presented frame's
	/// space. Yields an invalid anchor when no session covers that frame, which
	/// MapRoiToFrame treats as "already storage coordinates".
	void ReanchorRoiToPresentedFrame();
	/// Validated Analyze inputs. Collected before the long-range
	/// confirmation and lease so a No / lease failure cannot mutate an
	/// already-completed session.
	struct AnalyzeRequest {
		aegisub::motion_track::SessionDomains domains;
		aegisub::motion_track::RoiRect roi;
		aegisub::motion_track::TrackDirection dir =
			aegisub::motion_track::TrackDirection::Bidirectional;
		aegisub::motion_track::TrackModel track_model =
			aegisub::motion_track::TrackModel::Translation;
		std::vector<AssDialogue *> targets;
		int seed_frame = 0;
		int seed_time_ms = 0;
		aegisub::motion_track::RangeCheck range =
			aegisub::motion_track::RangeCheck::Ok;
	};
	bool PrepareAnalyzeRequest(AnalyzeRequest& request);
	void CommitAnalyzeRequest(AnalyzeRequest const& request);
	void RefreshReadonlyStats();

	/// Apply/preview require an Ok sample and controls matching the analyzed
	/// model and direction. Called on every path that leaves
	/// OnAnalyze so the pair never gets stuck disabled.
	void RefreshButtons();

	/// Enable/disable the apply options that only make sense for some
	/// model/mode combinations (growth needs Similarity + Exact); called on
	/// construction and whenever those combos change.
	void UpdateApplyOptionAvailability();

	/// Reuse the existing session (Continue semantics: same video identity,
	/// captured target set and direction) or build a fresh one that discards
	/// the trajectory.
	std::unique_ptr<aegisub::motion_track::MotionTrackSession>
	ContinueOrRebuild(aegisub::motion_track::SessionDomains const& domains,
					  aegisub::motion_track::RoiRect roi,
					  aegisub::motion_track::TrackDirection dir,
					  aegisub::motion_track::TrackModel model,
					  std::vector<AssDialogue *> const& targets);

	void ClearSessionState();
	/// Drops the session immediately, or after the in-flight Analyze returns
	/// if `RunAnalyze` is on the progress-task thread. CloseVideo can fire
	/// from a paint path while DialogProgress::Run is pumping events.
	void RequestClearSession();
	void FinishAnalyze();
	void SyncRoiSpinRanges();

	bool analyze_running_ = false;
	bool pending_session_clear_ = false;

	void OnVideoOpen(AsyncVideoProvider *new_provider);
	void OnTimecodesChanged(agi::vfr::Framerate const& fps);

	/// Swap the motion-track overlay tool back to the cross tool. No-op when
	/// the video display is already gone or some other tool has taken over
	/// since, so it is safe to call from the destructor.
	void ResetOverlayTool();

	public:
	/// Re-attach the ROI overlay tool to the current video display unless it
	/// is already active. The command entry point lands here on every
	/// invocation: switching to another visual tool destroyed the previous
	/// overlay instance, so without this re-attach the ROI box would stay
	/// gone until the dialog is closed and reopened.
	void EnsureOverlayOnCurrentDisplay();

	/// Accepted ROI geometry. The spin controls are built from these, and the
	/// overlay tool clamps storage-space drags to them; an anchor-space drag
	/// (a posed ROI) accepts the full range and Analyze re-clamps the seeded
	/// rectangle into the frame after riding it onto the seed frame, so a box
	/// drawn on the video and one typed in still agree on what is accepted.
	static constexpr int kMinRoiSide = 8;
	static constexpr int kMaxRoiSide = 512;
	static constexpr int kMaxRoiOrigin = 32767;

	DialogMotionTrack(agi::Context *context);
	~DialogMotionTrack();

	// Overlay-tool interface (visual_tool_motion_track reads these).
	std::shared_ptr<const aegisub::motion_track::MotionTrackSnapshot>
	CaptureForOverlay() const;
	std::shared_ptr<const PlanPreviewData> CapturePlanPreview() const {
		return plan_preview_;
	}
	aegisub::motion_track::RoiRect OverlayRoi() const;
	/// The anchor describing which frame's coordinate space OverlayRoi() is
	/// expressed in; see RoiRideAnchor.
	aegisub::motion_track::RoiRideAnchor OverlayRoiAnchor() const {
		return roi_anchor_;
	}
	/// Stores the ROI into the spin controls. `reanchor` true (band drawing,
	/// identity-pose edits): the rectangle is storage coordinates at the
	/// presented frame and the anchor is re-derived from it. False (edits made
	/// through a ride pose): the rectangle is in the existing anchor's space
	/// and the anchor is kept.
	void SetOverlayRoi(aegisub::motion_track::RoiRect roi, bool reanchor = true);
};
