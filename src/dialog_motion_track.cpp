// Interaction contract:
//  - Analyze runs on a DialogProgress task thread with a Project raw-video
//    lease held for the whole run; user cancellation surfaces as
//    agi::UserCancelException thrown by DialogProgress::Run on this thread
//    AFTER the task has committed its prefix — caught here.
//  - Apply runs the pure planner and executes the returned parts in a single
//    AssFile::Commit.
//  - Close destroys the session along with the window.

#include "dialog_motion_track.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info_service.h"
#include "auto4_base.h"
#include "async_video_provider.h"
#include "compat.h"
#include "dialog_manager.h"
#include "dialog_progress.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "motion_track/dialog_option_events.h"
#include "motion_track/raw_batch_motion_frame_reader.h"
#include "motion_track/similarity_backend.h"
#include "options.h"
#include <libaegisub/make_unique.h>
#include "project.h"
#include "visual_tool_cross.h"
#include "visual_tool_motion_track.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_display.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/vfr.h>

#include <wx/button.h>
#include <wx/combobox.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>

using namespace aegisub::motion_track;

namespace {

/// Reader used when hooks.run_batch performs all fetching; never queried.
class NullMotionFrameReader final : public MotionFrameReader {
	FrameReadResult FetchGray(int, RoiRect, GrayPatch&) override {
		return {FrameReadStatus::Error, "fetching requires run_batch"};
	}
};

/// Maps a batch-scoped FrameReadResult onto the RawVideoBatchStatus surface
/// RunRawVideoBatch reports. Non-Ok results never map to Completed so the
/// session's stop-reason mapping stays faithful.
RawVideoBatchStatus ToBatchStatus(FrameReadResult const& read) {
	switch (read.status) {
		case FrameReadStatus::Ok:
			return RawVideoBatchStatus::Completed;
		case FrameReadStatus::ProviderChanged:
			return RawVideoBatchStatus::ProviderChanged;
		case FrameReadStatus::FrameUnavailable:
			return RawVideoBatchStatus::FrameUnavailable;
		default:
			return RawVideoBatchStatus::DecodeError;
	}
}

/// Analyze and Apply share this set: selected non-comment lines in grid
/// order, or the active line when the selection is empty.
std::vector<AssDialogue *> CollectApplyTargets(agi::Context *c) {
	auto const& core = c->GetCore();
	std::vector<AssDialogue *> out;
	for (auto *line : core.selectionController->GetSortedSelection()) {
		if (line && !line->Comment)
			out.push_back(line);
	}
	if (out.empty()) {
		if (auto *active = core.selectionController->GetActiveLine();
			active && !active->Comment)
			out.push_back(active);
	}
	return out;
}

} // namespace

DialogMotionTrack::DialogMotionTrack(agi::Context *c)
	: wxDialog(c->GetUI().parent, -1, _("Motion Track")), context(c), video_open(c->GetCore().project->AddVideoProviderListener(
																		  &DialogMotionTrack::OnVideoOpen, this)),
	  timecodes_loaded(c->GetCore().project->AddTimecodesListener(
		  &DialogMotionTrack::OnTimecodesChanged, this)) {
	auto *root = new wxBoxSizer(wxVERTICAL);

	auto *roi_grid = new wxFlexGridSizer(4, 4, 5, 5);
	auto add_spin = [&](wxString const& label, int value, int minv, int maxv)
		-> wxSpinCtrl * {
		roi_grid->Add(new wxStaticText(this, -1, label),
					  0, wxALIGN_CENTRE_VERTICAL);
		auto *spin = new wxSpinCtrl(this);
		spin->SetRange(minv, maxv);
		spin->SetValue(value);
		roi_grid->Add(spin, 1, wxEXPAND);
		return spin;
	};

	roi_x = add_spin(_("ROI X"), 40, 0, kMaxRoiOrigin);
	roi_y = add_spin(_("ROI Y"), 30, 0, kMaxRoiOrigin);
	roi_w = add_spin(_("ROI W"), 24, kMinRoiSide, kMaxRoiSide);
	roi_h = add_spin(_("ROI H"), 16, kMinRoiSide, kMaxRoiSide);
	root->Add(roi_grid, 0, wxALL | wxEXPAND, 5);

	// Options are sectioned: everything that steers the tracker lives in
	// "Tracking", everything that shapes the written output in "Apply", so
	// per-mode availability (growth needs Similarity + Exact) reads as part
	// of the layout instead of a surprise.
	auto *track_box = new wxStaticBox(this, -1, _("Tracking"));
	auto *track_grid = new wxFlexGridSizer(2, 2, 5, 5);
	track_grid->Add(new wxStaticText(track_box, -1, _("Direction")),
					0, wxALIGN_CENTRE_VERTICAL);
	direction = new wxComboBox(track_box, -1, _("Both directions"),
							   wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
	direction->Append(_("Forward"));
	direction->Append(_("Backward"));
	direction->Append(_("Both directions"));
	direction->SetSelection(std::clamp(
		int(OPT_GET("Tool/Motion Track/Direction")->GetInt()), 0, 2));
	track_grid->Add(direction, 1, wxEXPAND);

	track_grid->Add(new wxStaticText(track_box, -1, _("Model")),
					0, wxALIGN_CENTRE_VERTICAL);
	model = new wxComboBox(track_box, -1, _("Translation"),
						   wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
	model->Append(_("Translation"));
	model->Append(_("Similarity (rotation+scale)"));
	model->Append(_("Affine (shear+non-uniform scale)"));
	model->Append(_("Perspective (four corners)"));
	model->SetSelection(std::clamp(
		static_cast<int>(OPT_GET("Tool/Motion Track/Model")->GetInt()), 0, 3));
	model->SetToolTip(_("Track translation, rotation and scale, affine deformation, "
						"or a perspective plane. Compact fits the written ASS geometry in video pixels."));
	track_grid->Add(model, 1, wxEXPAND);

	track_grid->Add(new wxStaticText(track_box, -1, _("Template refresh")),
					0, wxALIGN_CENTRE_VERTICAL);
	template_refresh = new wxCheckBox(track_box, -1, wxString());
	template_refresh->SetValue(
		OPT_GET("Tool/Motion Track/Template Refresh")->GetBool());
	track_grid->Add(template_refresh, 1, wxEXPAND);

	auto *track_sizer = new wxStaticBoxSizer(track_box, wxVERTICAL);
	track_sizer->Add(track_grid, 1, wxEXPAND | wxALL, 5);
	root->Add(track_sizer, 0, wxALL | wxEXPAND, 5);

	auto *apply_box = new wxStaticBox(this, -1, _("Apply"));
	auto *apply_grid = new wxFlexGridSizer(2, 2, 5, 5);
	apply_grid->Add(new wxStaticText(apply_box, -1, _("Apply mode")),
					0, wxALIGN_CENTRE_VERTICAL);
	apply_mode = new wxComboBox(apply_box, -1, _("Compact"),
								wxDefaultPosition, wxDefaultSize, 0, nullptr, wxCB_READONLY);
	apply_mode->Append(_("Compact"));
	apply_mode->Append(_("Exact"));
	apply_mode->SetSelection(std::clamp(
		int(OPT_GET("Tool/Motion Track/Apply Mode")->GetInt()), 0, 1));
	apply_grid->Add(apply_mode, 1, wxEXPAND);

	apply_grid->Add(new wxStaticText(apply_box, -1, _("Compact error (1/100 video px)")),
					0, wxALIGN_CENTRE_VERTICAL);
	epsilon = new wxSpinCtrl(apply_box);
	epsilon->SetRange(1, 1000);
	// The option is in storage (video) pixels (default 1.0); the control is in
	// 1/100 px, so the threshold means the same on-screen error at every
	// script resolution.
	epsilon->SetValue(std::clamp(
		int(std::lround(
			OPT_GET("Tool/Motion Track/Compact Error")->GetDouble() * 100.0)),
		1, 1000));
	apply_grid->Add(epsilon, 1, wxEXPAND);

	apply_grid->Add(new wxStaticText(apply_box, -1, _("Position decimals")),
					0, wxALIGN_CENTRE_VERTICAL);
	decimals = new wxSpinCtrl(apply_box);
	decimals->SetRange(0, 6);
	decimals->SetValue(std::clamp(
		int(OPT_GET("Tool/Motion Track/Position Decimals")->GetInt()), 0, 6));
	apply_grid->Add(decimals, 1, wxEXPAND);

	apply_grid->Add(new wxStaticText(apply_box, -1, _("Smooth (frames)")),
					0, wxALIGN_CENTRE_VERTICAL);
	smooth = new wxSpinCtrl(apply_box);
	// Local-linear smoothing half-window: removes per-frame tracker jitter
	// before any simplification, without lagging real constant-velocity
	// motion. 0 disables.
	smooth->SetRange(0, 15);
	smooth->SetValue(std::clamp(
		int(OPT_GET("Tool/Motion Track/Smooth Frames")->GetInt()), 0, 15));
	apply_grid->Add(smooth, 1, wxEXPAND);

	apply_grid->Add(new wxStaticText(apply_box, -1, _("Stabilize trajectory")),
					0, wxALIGN_CENTRE_VERTICAL);
	stabilize = new wxCheckBox(apply_box, -1, wxString());
	stabilize->SetToolTip(_(
		"Remove tracker jitter and flatten noise-only holds before writing "
		"positions; exact for linear motion"));
	stabilize->SetValue(
		OPT_GET("Tool/Motion Track/Stabilize")->GetBool());
	apply_grid->Add(stabilize, 1, wxEXPAND);

	apply_grid->Add(new wxStaticText(apply_box, -1, _("Scale border/shadow/blur")),
					0, wxALIGN_CENTRE_VERTICAL);
	growth = new wxCheckBox(apply_box, -1, wxString());
	growth->SetToolTip(_(
		"Similarity Exact applies only: scale \\bord, \\shad and \\blur with "
		"the tracked zoom so outline, shadow and blur follow the object"));
	growth->SetValue(
		OPT_GET("Tool/Motion Track/Scale Border Shadow Blur")->GetBool());
	apply_grid->Add(growth, 1, wxEXPAND);

	apply_grid->Add(new wxStaticText(apply_box, -1, _("Write \\fad from fade")),
					0, wxALIGN_CENTRE_VERTICAL);
	apply_fad = new wxCheckBox(apply_box, -1, wxString());
	apply_fad->SetToolTip(_(
		"When Analyze detected a fade, write its timing as \\fad on applied "
		"lines; without a detected fade this does nothing"));
	apply_fad->SetValue(
		OPT_GET("Tool/Motion Track/Apply Fad")->GetBool());
	apply_grid->Add(apply_fad, 1, wxEXPAND);

	apply_grid->Add(new wxStaticText(apply_box, -1, _("Plan preview")),
					0, wxALIGN_CENTRE_VERTICAL);
	preview = new wxCheckBox(apply_box, -1, wxString());
	preview->SetToolTip(_("Show the positions Apply would write as a ghost path on the video"));
	apply_grid->Add(preview, 1, wxEXPAND);

	auto *apply_sizer = new wxStaticBoxSizer(apply_box, wxVERTICAL);
	apply_sizer->Add(apply_grid, 1, wxEXPAND | wxALL, 5);
	root->Add(apply_sizer, 0, wxALL | wxEXPAND, 5);

	stats = new wxTextCtrl(this, -1, wxEmptyString, wxDefaultPosition,
						   wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
	stats->SetMinSize(wxSize(380, 110));
	root->Add(stats, 1, wxALL | wxEXPAND, 5);

	auto *buttons = new wxBoxSizer(wxHORIZONTAL);
	analyze_btn = new wxButton(this, -1, _("Analyze"));
	apply_btn = new wxButton(this, -1, _("Apply"));
	close_btn = new wxButton(this, wxID_CANCEL, _("Close"));
	buttons->Add(analyze_btn, 0, wxRIGHT, 5);
	buttons->Add(apply_btn, 0, wxRIGHT, 5);
	buttons->AddStretchSpacer();
	buttons->Add(close_btn, 0);
	root->Add(buttons, 0, wxALL | wxEXPAND, 5);

	analyze_btn->Bind(wxEVT_BUTTON, &DialogMotionTrack::OnAnalyze, this);
	apply_btn->Bind(wxEVT_BUTTON, &DialogMotionTrack::OnApply, this);
	preview->Bind(wxEVT_CHECKBOX, &DialogMotionTrack::OnPreviewToggle, this);
	for (wxComboBox *choice : {model, direction})
		BindDialogOptionChanges(*choice, DialogOptionControl::Choice,
								[this] { OnTrackingSettingsChanged(); });
	BindDialogOptionChanges(*apply_mode, DialogOptionControl::Choice,
							[this] { OnApplyOptionsChanged(); });
	for (wxSpinCtrl *spin : {epsilon, decimals, smooth})
		BindDialogOptionChanges(*spin, DialogOptionControl::Spin,
								[this] { OnApplyOptionsChanged(); });
	for (wxCheckBox *check : {stabilize, growth, apply_fad})
		BindDialogOptionChanges(*check, DialogOptionControl::Check,
								[this] { OnApplyOptionsChanged(); });
	// Both events, because they cover different halves of the same edit: the
	// arrows and the wrapped-up commit send wxEVT_SPINCTRL, while typing digits
	// into the text field only sends wxEVT_TEXT until focus leaves. The handler
	// is idempotent, so the pair that arrive together are harmless.
	for (wxSpinCtrl *spin : {roi_x, roi_y, roi_w, roi_h}) {
		spin->Bind(wxEVT_SPINCTRL, &DialogMotionTrack::OnRoiSpin, this);
		spin->Bind(wxEVT_TEXT, &DialogMotionTrack::OnRoiSpin, this);
	}
	UpdateApplyOptionAvailability();
	RefreshButtons(); // no trajectory yet, so Apply starts disabled
	// No handler on close_btn: DialogManager::ShowOnce binds wxEVT_BUTTON for
	// wxID_CANCEL on the dialog itself and that is what unregisters us from
	// created_dialogs. Handling the click here without Skip() would leave a
	// dangling entry behind, and a later ShowOnce would resurrect a destroyed
	// window.

	SetSizerAndFit(root);
	CentreOnParent();
	SetIcon(GETICON(motion_track_button_16));

	SyncRoiSpinRanges();
	// Attach the overlay visual tool to the current display so users can
	// aim/drag the ROI box directly on the video.
	EnsureOverlayOnCurrentDisplay();
}

void DialogMotionTrack::ClearSessionState() {
	session.reset();
	apply_source_.Clear();
	roi_anchor_ = {};
	plan_preview_.reset();
	if (preview)
		preview->SetValue(false);
	RefreshButtons();
	RefreshReadonlyStats();
	RefreshVideoDisplay();
}

void DialogMotionTrack::RequestClearSession() {
	// Analyze holds `session` on the progress-task thread. CloseVideo
	// announces nullptr on the UI thread before retiring the lease, so
	// resetting here would free that object concurrently with RunAnalyze.
	if (analyze_running_) {
		pending_session_clear_ = true;
		return;
	}
	ClearSessionState();
}

void DialogMotionTrack::FinishAnalyze() {
	analyze_running_ = false;
	if (pending_session_clear_) {
		pending_session_clear_ = false;
		ClearSessionState();
		return;
	}
	RefreshButtons();
	RefreshReadonlyStats();
	RefreshVideoDisplay();
}

void DialogMotionTrack::SyncRoiSpinRanges() {
	auto *provider = context->GetCore().project->VideoProvider();
	int const sw = provider ? std::max(0, provider->GetWidth()) : kMaxRoiOrigin;
	int const sh = provider ? std::max(0, provider->GetHeight()) : kMaxRoiOrigin;
	int const max_x = std::max(0, sw - kMinRoiSide);
	int const max_y = std::max(0, sh - kMinRoiSide);
	int const max_w = sw > 0 ? std::clamp(sw, kMinRoiSide, kMaxRoiSide) : kMaxRoiSide;
	int const max_h = sh > 0 ? std::clamp(sh, kMinRoiSide, kMaxRoiSide) : kMaxRoiSide;
	if (!roi_x)
		return;
	roi_x->SetRange(0, provider ? max_x : kMaxRoiOrigin);
	roi_y->SetRange(0, provider ? max_y : kMaxRoiOrigin);
	roi_w->SetRange(kMinRoiSide, max_w);
	roi_h->SetRange(kMinRoiSide, max_h);
}

void DialogMotionTrack::OnVideoOpen(AsyncVideoProvider *new_provider) {
	// CloseVideo announces nullptr while the old provider is still live, so
	// the argument is the source of truth — reading VideoProvider() here
	// would keep a closed-video session around.
	if (!new_provider) {
		RequestClearSession();
		return;
	}
	RawVideoIdentity const identity = new_provider->GetRawVideoIdentity();
	if (session && !identity.Matches(last_identity))
		RequestClearSession();
	last_identity = identity;
	SyncRoiSpinRanges();
	// Opening a video recreates the display (or resets its tool); bring the
	// ROI overlay back so the session stays editable without reopening.
	EnsureOverlayOnCurrentDisplay();
}

void DialogMotionTrack::OnTimecodesChanged(agi::vfr::Framerate const& fps) {
	if (!session)
		return;
	// DoLoadVideo always announces timecodes after the provider. Identical
	// mappings must not drop a still-valid trajectory.
	if (MotionTrackTimecodesMatch(apply_source_.Snapshot(), fps))
		return;
	RequestClearSession();
}

void DialogMotionTrack::EnsureOverlayOnCurrentDisplay() {
	auto *display = context->GetUI().videoDisplay;
	if (!display || display->ToolIsType(typeid(VisualToolMotionTrack)))
		return;
	display->SetTool(agi::make_unique<VisualToolMotionTrack>(display, context));
}

DialogMotionTrack::~DialogMotionTrack() {
	OPT_SET("Tool/Motion Track/Direction")
		->SetInt(std::max(0, direction->GetSelection()));
	OPT_SET("Tool/Motion Track/Model")
		->SetInt(std::max(0, model->GetSelection()));
	OPT_SET("Tool/Motion Track/Apply Mode")
		->SetInt(std::max(0, apply_mode->GetSelection()));
	OPT_SET("Tool/Motion Track/Compact Error")
		->SetDouble(epsilon->GetValue() / 100.0);
	OPT_SET("Tool/Motion Track/Position Decimals")
		->SetInt(decimals->GetValue());
	OPT_SET("Tool/Motion Track/Smooth Frames")
		->SetInt(smooth->GetValue());
	OPT_SET("Tool/Motion Track/Template Refresh")
		->SetBool(template_refresh->GetValue());
	OPT_SET("Tool/Motion Track/Stabilize")
		->SetBool(stabilize->GetValue());
	OPT_SET("Tool/Motion Track/Scale Border Shadow Blur")
		->SetBool(growth->GetValue());
	OPT_SET("Tool/Motion Track/Apply Fad")
		->SetBool(apply_fad->GetValue());
	ResetOverlayTool();
}

void DialogMotionTrack::ResetOverlayTool() {
	auto *display = context->GetUI().videoDisplay;
	if (!display || !display->ToolIsType(typeid(VisualToolMotionTrack)))
		return;
	display->SetTool(agi::make_unique<VisualToolCross>(display, context));
}

std::unique_ptr<MotionTrackSession> DialogMotionTrack::ContinueOrRebuild(
	SessionDomains const& domains, RoiRect roi, TrackDirection dir,
	TrackModel track_model, std::vector<AssDialogue *> const& targets) {
	auto *provider = context->GetCore().project->VideoProvider();
	RawVideoIdentity const identity =
		provider ? provider->GetRawVideoIdentity() : RawVideoIdentity{};

	bool const same_targets =
		apply_source_.ContinueTargetsMatch(targets);

	bool const can_continue = session && identity.Matches(last_identity) && MotionTrackSettingsMatch(*session->Capture(), dir, track_model) && same_targets;

	last_identity = identity;

	if (can_continue) {
		// Continue: origin seed and committed samples survive; SetBackendSeed
		// moves only the backend seed onto the new ROI/frame.
		return std::move(session);
	}
	return std::make_unique<MotionTrackSession>(domains, roi, dir,
												track_model);
}

bool DialogMotionTrack::PrepareAnalyzeRequest(AnalyzeRequest& request) {
	auto core = context->GetCore();
	auto *provider = core.project->VideoProvider();
	if (!provider) {
		wxMessageBox(_("No video is open."), _("Motion Track"),
					 wxOK | wxICON_ERROR, this);
		return false;
	}

	RoiRect roi{roi_x->GetValue(), roi_y->GetValue(),
				roi_w->GetValue(), roi_h->GetValue()};

	auto const& fps = core.project->Timecodes();
	int const frame_count = provider->GetFrameCount();
	if (frame_count <= 0) {
		wxMessageBox(_("The video has no frames."), _("Motion Track"),
					 wxOK | wxICON_ERROR, this);
		return false;
	}
	if (provider->GetWidth() < kMinRoiSide || provider->GetHeight() < kMinRoiSide) {
		wxMessageBox(_("The video is too small to track."), _("Motion Track"),
					 wxOK | wxICON_ERROR, this);
		return false;
	}

	auto targets = CollectApplyTargets(context);
	if (targets.empty()) {
		wxMessageBox(_("No subtitle line selected."), _("Motion Track"),
					 wxOK | wxICON_ERROR, this);
		return false;
	}

	int first = std::numeric_limits<int>::max();
	int last = std::numeric_limits<int>::min();
	for (auto *target : targets) {
		int const line_first = std::clamp(
			fps.FrameAtTime(int(target->Start), agi::vfr::Time::START),
			0, frame_count - 1);
		int const line_last = std::clamp(
			fps.FrameAtTime(int(target->End), agi::vfr::Time::END),
			0, frame_count - 1);
		if (line_last < line_first)
			continue;
		first = std::min(first, line_first);
		last = std::max(last, line_last);
	}
	if (last < first) {
		wxMessageBox(_("Selected line has an empty time range."),
					 _("Motion Track"), wxOK | wxICON_ERROR, this);
		return false;
	}

	SessionDomains domains;
	// Decode the continuous hull of the selected lines' frame intervals.
	// The 1500/10000-frame limits therefore apply to the requested span.
	domains.decode_interval = FrameInterval{first, last};
	domains.direction_domain = FrameInterval{first, last};
	domains.video_frame_count = frame_count;
	// Storage dims travel into the published snapshot; the overlay needs them
	// to map storage coordinates onto the display, so leaving them at 0
	// silently disables all overlay drawing.
	domains.storage_width = provider->GetWidth();
	domains.storage_height = provider->GetHeight();
	// Hidden advanced knob: 0 keeps the ROI-derived default radius.
	domains.search_radius_override =
		std::max(0, int(OPT_GET("Tool/Motion Track/Search Radius")->GetInt()));

	TrackDirection const dir = SelectedDirection();
	TrackModel const track_model = SelectedModel();

	// The spin ROI lives in the space of the frame it was last edited at, so
	// ride it onto the new seed frame before handing it to the backend -- the
	// seed template must cover what the box displays on screen, not wherever
	// the trajectory started. Needs the pre-Analyze snapshot, so this runs
	// before ContinueOrRebuild can replace the session.
	int const seed_frame =
		std::clamp(core.videoController->GetPresentedFrameN(), first, last);
	std::shared_ptr<const MotionTrackSnapshot> snap;
	if (session)
		snap = session->Capture();
	if (snap)
		roi = MapRoiToFrame(*snap, seed_frame, roi, roi_anchor_);
	// The spins accept anchor-space values outside the frame (posed overlay
	// drags push them through SetOverlayRoi), so the seeded rectangle is
	// always clamped into storage here instead of relying on spin range
	// coercion -- the same clamp discipline as the overlay tool's
	// storage-space PushRoi path. Writing the clamped values back is
	// deferred until Commit so a cancelled confirmation cannot change the
	// overlay.
	int const sw = std::max(1, provider->GetWidth());
	int const sh = std::max(1, provider->GetHeight());
	roi.w = std::clamp(roi.w, kMinRoiSide, std::min(kMaxRoiSide, sw));
	roi.h = std::clamp(roi.h, kMinRoiSide, std::min(kMaxRoiSide, sh));
	roi.x = std::clamp(roi.x, 0, sw - roi.w);
	roi.y = std::clamp(roi.y, 0, sh - roi.h);

	request.domains = domains;
	request.roi = roi;
	request.dir = dir;
	request.track_model = track_model;
	request.targets = std::move(targets);
	request.seed_frame = seed_frame;
	// The origin resolver interpolates an existing \move at seed_time_ms, so
	// this has to be the seed frame's own time — using the line start would
	// read the wrong \move position whenever the seed is not the first frame.
	// Commit keeps the first origin's time on Continue; overwriting it here
	// would combine a new \move sample with the old-origin trajectory.
	request.seed_time_ms = fps.TimeAtFrame(seed_frame, agi::vfr::Time::START);
	request.range = CheckDecodeInterval(domains.decode_interval);
	return true;
}

void DialogMotionTrack::CommitAnalyzeRequest(AnalyzeRequest const& request) {
	auto core = context->GetCore();
	auto const& fps = core.project->Timecodes();
	int const frame_count = request.domains.video_frame_count;

	session = ContinueOrRebuild(request.domains, request.roi, request.dir,
								request.track_model, request.targets);
	apply_source_.Capture(*core.ass, request.targets, fps, frame_count);

	// Continue keeps origin_seed_frame; a rebuild starts at -1. \move is
	// interpolated at the origin seed time, so only a fresh origin may
	// replace seed_time_ms -- otherwise Apply would add move(t1)-move(t0).
	bool const keep_origin_seed_time = [&] {
		auto snap = session ? session->Capture() : nullptr;
		return snap && snap->origin_seed_frame >= 0;
	}();

	double const cx = request.roi.x + (request.roi.w - 1) / 2.0;
	double const cy = request.roi.y + (request.roi.h - 1) / 2.0;
	// The chosen session owns the anchor: rebuilds start at identity, while
	// Continue carries the previous shape into both seed and overlay.
	roi_anchor_ = session->SetBackendSeed(request.seed_frame, request.roi, cx, cy);
	if (!keep_origin_seed_time)
		seed_time_ms = request.seed_time_ms;

	SetOverlayRoi(request.roi, false);
}

void DialogMotionTrack::OnAnalyze(wxCommandEvent&) {
	// A stale preview must not survive a new trajectory.
	InvalidatePlanPreview();
	AnalyzeRequest request;
	if (!PrepareAnalyzeRequest(request))
		return;

	auto core = context->GetCore();
	core.videoController->Stop();

	// Runtime knobs for the tracking backend, re-read per run so toggling the
	// checkbox takes effect on the next Analyze without reopening the dialog.
	TrackerBackend *backends[]{&translation_backend, &similarity_backend,
							   &affine_backend, &homography_backend};
	TrackerBackend *backend = backends[std::clamp(model->GetSelection(), 0, 3)];
	if (model->GetSelection() == 0) {
		aegisub::motion_track::TranslationTrackerConfig backend_config;
		backend_config.template_refresh = template_refresh->GetValue();
		translation_backend.SetConfig(backend_config);
	}

	analyze_btn->Disable();
	apply_btn->Disable();

	// Range caps are decided from the request so a No / TooLong / lease
	// failure cannot SetBackendSeed over a completed session.
	if (request.range == RangeCheck::TooLong) {
		wxMessageBox(_("The tracking range exceeds 10000 frames and cannot be analyzed."),
					 _("Motion Track"), wxOK | wxICON_ERROR, this);
		RefreshButtons();
		return;
	}
	if (request.range == RangeCheck::NeedsConfirmation) {
		auto answer = wxMessageBox(
			_("The tracking range exceeds 1500 frames and may take a while.\n"
			  "Continue?"),
			_("Motion Track"), wxYES_NO | wxICON_QUESTION, this);
		if (answer != wxYES) {
			RefreshButtons();
			return;
		}
	}

	auto lease_handle = std::make_shared<VideoProviderLeaseState::Handle>(
		core.project->AcquireVideoProviderLease());
	if (!*lease_handle) {
		wxMessageBox(_("Video is being replaced; try again."), _("Motion Track"),
					 wxOK | wxICON_WARNING, this);
		RefreshButtons();
		return;
	}

	CommitAnalyzeRequest(request);

	auto const identity = core.project->VideoProvider()->GetRawVideoIdentity();
	last_identity = identity;

	NullMotionFrameReader null_reader;

	DialogProgress progress(this, _("Motion Track"), _("Tracking..."), true);
	analyze_running_ = true;
	try {
		progress.Run([&, identity](agi::ProgressSink *sink) {
			AnalyzeRunHooks hooks;
			hooks.user_cancelled = [sink] { return sink->IsCancelled(); };
			hooks.progress = [&](int done, int total) {
				if (total > 0)
					sink->SetProgress(done, total);
			};
			hooks.run_batch = [&, identity](
								  std::function<FrameReadResult(
									  MotionFrameReader&)> const& fn) {
				auto thunk = [&](RawFrameAccess& access) {
					RawBatchMotionFrameReader batched(access);
					return ToBatchStatus(fn(batched));
				};
				return core.project->VideoProvider()
					->RunRawVideoBatch(identity, thunk)
					.status;
			};
			session->RunAnalyze(null_reader, *backend, *lease_handle, hooks);
		});
	}
	catch (agi::UserCancelException const&) {
		// Expected on cancel; the committed prefix is already published.
	}
	catch (agi::Exception const& e) {
		wxMessageBox(to_wx(e.GetMessage()), _("Motion Track"),
					 wxOK | wxICON_ERROR, this);
	}
	catch (...) {
		FinishAnalyze();
		throw;
	}
	FinishAnalyze();
}
bool DialogMotionTrack::BuildApplyInput(ApplyPlanInput& input,
										std::vector<AssDialogue *>& targets) {
	auto snap = session ? session->Capture() : nullptr;
	if (!snap || snap->samples.empty()) {
		wxMessageBox(_("Nothing to apply — run Analyze first."),
					 _("Motion Track"), wxOK | wxICON_INFORMATION, this);
		return false;
	}
	if (!MotionTrackSettingsMatch(*snap, SelectedDirection(), SelectedModel())) {
		wxMessageBox(_("The model or direction changed since Analyze; re-run Analyze first."),
					 _("Motion Track"), wxOK | wxICON_INFORMATION, this);
		return false;
	}
	auto core = context->GetCore();
	auto *provider = core.project->VideoProvider();
	if (!provider || !provider->GetRawVideoIdentity().Matches(last_identity)) {
		wxMessageBox(_("The video changed since Analyze; re-run Analyze first."),
					 _("Motion Track"), wxOK | wxICON_ERROR, this);
		return false;
	}
	auto& ass = *core.ass;
	if (!MotionTrackSourceIsCurrent(ass, apply_source_.Snapshot(),
									core.project->Timecodes())) {
		wxMessageBox(_("The subtitles or timecodes changed since Analyze; re-run Analyze first."),
					 _("Motion Track"), wxOK | wxICON_ERROR, this);
		return false;
	}
	targets = apply_source_.Targets();

	FillApplyInputFromSnapshot(input, *snap);
	input.storage_width = last_identity.width;
	input.storage_height = last_identity.height;
	// \pos/\move are PlayRes coordinates. LayoutRes only scales blur,
	// \frx/\fry, and SBAS=no borders — never the position tags.
	FillApplyInputScriptResolution(input, ass);
	input.timecodes = core.project->Timecodes();
	input.video_frame_count = last_identity.frame_count;
	// \move is interpolated at the origin seed, which Continue never moves.
	// Prefer the snapshot's origin frame so a stale dialog seed_time_ms
	// cannot combine a new \move sample with the old-origin trajectory.
	if (snap->origin_seed_frame >= 0)
		input.seed_time_ms = input.timecodes.TimeAtFrame(
			snap->origin_seed_frame, agi::vfr::Time::START);
	else
		input.seed_time_ms = seed_time_ms;
	input.options.mode =
		apply_mode->GetSelection() == 1 ? ApplyMode::Exact : ApplyMode::Compact;
	input.options.compact_epsilon = epsilon->GetValue() / 100.0;
	input.options.position_decimals = decimals->GetValue();
	input.options.smooth_frames = smooth->IsEnabled() ? smooth->GetValue() : 0;
	input.options.stabilization.enable = stabilize->IsEnabled() && stabilize->GetValue();
	bool const growth_on = growth->IsEnabled() && growth->GetValue();
	input.options.scale_border = growth_on;
	input.options.scale_shadow = growth_on;
	input.options.scale_blur = growth_on;
	input.options.apply_fad = apply_fad->IsEnabled() && apply_fad->GetValue();
	input.text_extents = &Automation4::CalculateTextExtents;
	return true;
}

void DialogMotionTrack::OnApply(wxCommandEvent&) {
	ApplyPlanInput input;
	std::vector<AssDialogue *> apply_targets;
	if (!BuildApplyInput(input, apply_targets))
		return;
	auto core = context->GetCore();
	auto& ass = *core.ass;
	// A confirmation pumps events and may clear the captured source when
	// video/timecodes change. Consume its owned baseline before any modal;
	// Apply validates the live source again before accepting this plan.
	auto plan = BuildApplyPlan(ass, apply_targets, input);

	if (session->LastStopReason() != AnalyzeStopReason::Completed) {
		auto answer = wxMessageBox(
			_("The trajectory is incomplete (tracking was interrupted).\n"
			  "Applying will fail unless every needed frame is covered.\n\n"
			  "Try to apply anyway?"),
			_("Motion Track"), wxYES_NO | wxICON_WARNING, this);
		if (answer != wxYES)
			return;
	}

	if (plan.status == ApplyPlanStatus::IncompleteCoverage) {
		std::string msg = "Trajectory does not cover:\n";
		for (auto const& l : plan.uncovered)
			for (auto const& r : l.ranges)
				msg += "  frames " + std::to_string(r.first) + ".." +
					   std::to_string(r.last) + "\n";
		wxMessageBox(to_wx(msg), _("Motion Track"), wxOK | wxICON_WARNING, this);
		return;
	}
	if (!plan.has_mutations()) {
		wxMessageBox(to_wx(plan.message), _("Motion Track"),
					 wxOK | wxICON_ERROR, this);
		return;
	}
	if (plan.needs_confirmation()) {
		auto answer = wxMessageBox(
			to_wx("This will create " + std::to_string(plan.event_count) +
				  " events.\nContinue?"),
			_("Motion Track"), wxYES_NO | wxICON_QUESTION, this);
		if (answer != wxYES)
			return;
	}
	if (!plan.needs_manual_review.empty()) {
		auto answer = wxMessageBox(
			to_wx(std::to_string(plan.needs_manual_review.size()) +
				  " of the applied lines carry \\org or \\clip tags. A "
				  "rotation origin or clip rectangle does not follow the "
				  "tracked position: text swung around a stale \\org "
				  "wobbles, and text pushed out of a stale \\clip "
				  "disappears. These lines need a manual check after "
				  "applying.\n\nApply anyway?"),
			_("Motion Track"), wxYES_NO | wxICON_WARNING, this);
		if (answer != wxYES)
			return;
	}

	auto commit = [&](std::vector<AssDialogue *> const& lines) {
		ass.Commit(from_wx(_("Apply motion track")),
				   AssFile::COMMIT_DIAG_TEXT | AssFile::COMMIT_DIAG_ADDREM |
					   AssFile::COMMIT_DIAG_TIME);
		Selection selected(lines.begin(), lines.end());
		core.selectionController->SetSelectionAndActive(std::move(selected), lines.front());
	};
	std::string message;
	bool const applied = apply_source_.Apply(ass, plan, core.project->Timecodes(), commit, message);
	if (!applied) {
		wxMessageBox(to_wx(message), _("Motion Track"), wxOK | wxICON_ERROR, this);
		return;
	}
	plan_preview_.reset();
	preview->SetValue(false);
	RefreshReadonlyStats();
	RefreshVideoDisplay();
}

void DialogMotionTrack::OnPreviewToggle(wxCommandEvent&) {
	plan_preview_.reset();
	if (preview->GetValue()) {
		ApplyPlanInput input;
		std::vector<AssDialogue *> targets;
		if (BuildApplyInput(input, targets)) {
			auto plan = BuildApplyPlan(*context->GetCore().ass, targets, input);
			if (plan.has_mutations()) {
				auto data = std::make_shared<PlanPreviewData>();
				double const scale_x =
					double(last_identity.width) / std::max(1, input.script_width);
				double const scale_y =
					double(last_identity.height) / std::max(1, input.script_height);
				for (auto const& pl : plan.lines) {
					std::vector<PlanPreviewPoint> path;
					for (auto const& part : pl.parts) {
						if (!part.covered)
							continue;
						path.push_back(PlanPreviewPoint{
							float(part.x0 * scale_x), float(part.y0 * scale_y)});
					}
					// Trailing knot of the last covered part closes the path.
					for (auto it = pl.parts.rbegin(); it != pl.parts.rend(); ++it)
						if (it->covered) {
							path.push_back(PlanPreviewPoint{
								float(it->x1 * scale_x), float(it->y1 * scale_y)});
							break;
						}
					if (path.size() >= 2)
						data->paths.push_back(std::move(path));
				}
				plan_preview_ = std::move(data);
			}
			else {
				// Surface the reason in the stats box instead of popping a
				// modal from a checkbox toggle.
				preview->SetValue(false);
				stats->SetValue(to_wx(plan.message.empty()
										  ? std::string("preview unavailable")
										  : plan.message));
			}
		}
		else {
			preview->SetValue(false);
		}
	}
	RefreshVideoDisplay();
}

void DialogMotionTrack::OnRoiSpin(wxCommandEvent&) {
	// SetOverlayRoi is writing the controls: not a user edit, and re-anchoring
	// here would clobber the anchor that call is installing.
	if (updating_roi_spins_)
		return;
	// A typed number means what it says on the frame the user is looking at, so
	// that frame becomes the values' space. Without this, roi_anchor_ still
	// names the frame of the last overlay drag and CommitAnalyzeRequest rides
	// the typed rectangle forward from there onto some other part of the image
	// -- silently, since the numbers on screen never change to show it. A frame
	// with no Ok sample yields an invalid anchor, which MapRoiToFrame passes
	// through untouched, so the values are then plain storage pixels.
	//
	// One anchor covers all four spins, so editing one re-reads the other three
	// at the presented frame and the box can jump once. That is deliberate: the
	// alternative is to bake the ride into the values first, which would
	// overwrite the digit just typed.
	ReanchorRoiToPresentedFrame();
	RefreshVideoDisplay();
}

TrackDirection DialogMotionTrack::SelectedDirection() const {
	switch (direction->GetSelection()) {
		case 0: return TrackDirection::Forward;
		case 1: return TrackDirection::Backward;
		default: return TrackDirection::Bidirectional;
	}
}

TrackModel DialogMotionTrack::SelectedModel() const {
	constexpr TrackModel models[]{TrackModel::Translation, TrackModel::Similarity,
								  TrackModel::Affine, TrackModel::Homography};
	return models[std::clamp(model->GetSelection(), 0, 3)];
}

void DialogMotionTrack::InvalidatePlanPreview() {
	bool const visible = plan_preview_ || preview->GetValue();
	plan_preview_.reset();
	preview->SetValue(false);
	if (visible)
		RefreshVideoDisplay();
}

void DialogMotionTrack::OnTrackingSettingsChanged() {
	UpdateApplyOptionAvailability();
	InvalidatePlanPreview();
	RefreshButtons();
	RefreshReadonlyStats();
}

void DialogMotionTrack::OnApplyOptionsChanged() {
	UpdateApplyOptionAvailability();
	InvalidatePlanPreview();
	RefreshButtons();
}

void DialogMotionTrack::RefreshVideoDisplay() {
	if (auto *display = context->GetUI().videoDisplay)
		display->Render();
}

static std::string StopReasonText(AnalyzeStopReason reason) {
	switch (reason) {
		case AnalyzeStopReason::None: return "none";
		case AnalyzeStopReason::Completed: return "completed";
		case AnalyzeStopReason::UserCanceled: return "canceled by user";
		case AnalyzeStopReason::ConsecutiveFailures:
			return "stopped after repeated failures";
		case AnalyzeStopReason::ProviderChanged: return "video changed";
		case AnalyzeStopReason::FrameUnavailable: return "frame unavailable";
		case AnalyzeStopReason::DecodeError: return "decode error";
		case AnalyzeStopReason::Error: return "error";
	}
	return "unknown";
}

void DialogMotionTrack::RefreshButtons() {
	analyze_btn->Enable(!analyze_running_);
	auto snap = session ? session->Capture() : nullptr;
	// A trajectory of nothing but Failed/Missing samples cannot produce a
	// single \pos, and OnApply would only pop an error box.
	bool const can_apply = !analyze_running_ && snap && snap->success_count > 0 &&
						   MotionTrackSettingsMatch(*snap, SelectedDirection(), SelectedModel());
	apply_btn->Enable(can_apply);
	preview->Enable(can_apply);
}

void DialogMotionTrack::UpdateApplyOptionAvailability() {
	// Growth compensation is currently emitted for static similarity poses;
	// Compact's animated transform does not rewrite these style dimensions.
	bool const similarity = model->GetSelection() == 1;
	bool const exact = apply_mode->GetSelection() == 1;
	growth->Enable(similarity && exact);
	template_refresh->Enable(model->GetSelection() == 0);
	smooth->Enable(model->GetSelection() == 0);
	stabilize->Enable(model->GetSelection() <= 1);
	apply_fad->Enable(model->GetSelection() == 0);
}

void DialogMotionTrack::RefreshReadonlyStats() {
	auto snap = session ? session->Capture() : nullptr;
	if (!snap) {
		stats->SetValue(wxEmptyString);
		return;
	}
	std::string text = agi::format(
		"ok/failed: %d/%d\nseed frame: %d\nstop reason: %s",
		snap->success_count, snap->failure_count, snap->backend_seed_frame,
		StopReasonText(snap->stop_reason));
	if (!MotionTrackSettingsMatch(*snap, SelectedDirection(), SelectedModel()))
		text = from_wx(_("The model or direction changed; run Analyze before Apply or preview.")) +
			   "\n" + text;
	if (!snap->diagnostic_message.empty())
		text += "\ndiagnostic: " + snap->diagnostic_message;
	if (snap->stop_reason == AnalyzeStopReason::UserCanceled || snap->stop_reason == AnalyzeStopReason::ConsecutiveFailures)
		text += "\nhint: drag the ROI back onto the object and press Analyze "
				"again — tracking continues from there (origin kept)";
	stats->SetValue(to_wx(text));
}

std::shared_ptr<const MotionTrackSnapshot>
DialogMotionTrack::CaptureForOverlay() const {
	return session ? session->Capture() : nullptr;
}

RoiRect DialogMotionTrack::OverlayRoi() const {
	return RoiRect{roi_x->GetValue(), roi_y->GetValue(),
				   roi_w->GetValue(), roi_h->GetValue()};
}

void DialogMotionTrack::SetOverlayRoi(RoiRect roi, bool reanchor) {
	// wxSpinCtrl silently coerces values into the current range, so an
	// anchor-space rectangle that the pose carried outside the frame would
	// snap back to the frame edge and freeze the drag it came from. Widen the
	// range to cover the value before storing it; SyncRoiSpinRanges restores
	// the storage bounds whenever the video changes, and the Analyze path
	// (CommitAnalyzeRequest) clamps the seeded rectangle back into storage
	// space regardless of what the spins hold.
	auto const store = [](wxSpinCtrl *spin, int value) {
		int const lo = std::min(spin->GetMin(), value);
		int const hi = std::max(spin->GetMax(), value);
		if (lo != spin->GetMin() || hi != spin->GetMax())
			spin->SetRange(lo, hi);
		spin->SetValue(value);
	};
	// Guard the programmatic writes. SetRange can coerce the value and emit
	// wxEVT_TEXT on MSW, and OnRoiSpin must not read that as a user edit: it
	// would re-anchor to the presented frame and so overwrite the anchor this
	// call is establishing -- mid-drag, the one the pose is riding.
	updating_roi_spins_ = true;
	store(roi_x, roi.x);
	store(roi_y, roi.y);
	store(roi_w, roi.w);
	store(roi_h, roi.h);
	updating_roi_spins_ = false;
	if (reanchor)
		ReanchorRoiToPresentedFrame();
}

void DialogMotionTrack::ReanchorRoiToPresentedFrame() {
	auto snap = session ? session->Capture() : nullptr;
	roi_anchor_ =
		snap ? RideAnchorAtFrame(
				   *snap, context->GetCore().videoController->GetPresentedFrameN())
			 : RoiRideAnchor{};
}

void ShowMotionTrackDialog(agi::Context *c) {
	// Show delegates to the single-instance path: a second invocation focuses
	// the existing window instead of stacking a duplicate.
	c->GetUI().dialog->Show<DialogMotionTrack>(c);
	// The overlay tool is destroyed whenever another visual tool takes over;
	// re-attach it so one click on the command restores ROI interaction
	// without closing and reopening the dialog.
	if (auto *dialog = c->GetUI().dialog->Get<DialogMotionTrack>())
		dialog->EnsureOverlayOnCurrentDisplay();
}
