// Copyright (c) 2005, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "video_box.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/toolbar.h"
#include "options.h"
#include "project.h"
#include "secondary_subtitle_strip.h"
#include "selection_controller.h"
#include "secondary_subtitle_session.h"
#include "utils.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_slider.h"

#include <libaegisub/scope_exit.h>

#include <wx/combobox.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statline.h>
#include <wx/textctrl.h>
#include <wx/toplevel.h>
#include <wx/toolbar.h>
#include <wx/weakref.h>

VideoBox::VideoBox(
	wxWindow *parent,
	bool isDetached,
	agi::Context *context)
: wxPanel(parent, -1)
, context(context)
, ownsSecondarySubtitleSession(!isDetached)
, isDetached(isDetached)
, current_frame(context->GetCore().videoController->GetFrameN())
{
	auto ui = context->GetUI();
	if (!ui.secondarySubtitleSession) {
		ui.secondarySubtitleSession = std::make_shared<SecondarySubtitleSession>(context);
		ownsSecondarySubtitleSession = true;
	}
	secondarySubtitleSession = ui.secondarySubtitleSession;

	auto videoSlider = new VideoSlider(this, context);
	videoSlider->SetToolTip(_("Seek video"));

	auto mainToolbar = toolbar::GetToolbar(this, "video", context, "Video", false);

	VideoPosition = new wxTextCtrl(this, -1, wxEmptyString, wxDefaultPosition, wxSize(110, -1), wxTE_READONLY);
	VideoPosition->SetToolTip(_("Current frame time"));
	// Trump frame hotkeys (edit/line/copy etc.) so Ctrl+C copies the field text.
	VideoPosition->Bind(wxEVT_CHAR_HOOK, [](wxKeyEvent &event) {
		TextControlClipboardCharHook(event, false);
	});

	VideoFrameInput = new wxSpinCtrl(
		this, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize,
		wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER, 0, 0, std::max(current_frame, 0));
#ifdef __WXGTK3__
	// GTK3 does not reliably shrink spin controls to their requested text width.
#elif wxCHECK_VERSION(3, 1, 3)
	VideoFrameInput->SetInitialSize(VideoFrameInput->GetSizeFromText(wxS("000000")));
#else
	VideoFrameInput->SetInitialSize(VideoFrameInput->GetSizeFromTextSize(GetTextExtent(wxS("000000"))));
#endif
	VideoFrameInput->SetToolTip(_("Current frame; enter a frame number to jump"));
	VideoFrameInput->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { JumpToInputFrame(); });
	VideoFrameInput->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent &) { JumpToInputFrame(); });
	VideoFrameInput->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &event) {
		if (current_frame >= 0)
			VideoFrameInput->SetValue(current_frame);
		event.Skip();
	});
	VideoFrameInput->Bind(wxEVT_CHAR_HOOK, [](wxKeyEvent &event) {
		TextControlClipboardCharHook(event, true);
	});

	VideoSubsPos = new wxTextCtrl(this, -1, wxEmptyString, wxDefaultPosition, wxSize(110, -1), wxTE_READONLY);
	VideoSubsPos->SetToolTip(_("Time of this frame relative to start and end of current subs"));
	VideoSubsPos->Bind(wxEVT_CHAR_HOOK, [](wxKeyEvent &event) {
		TextControlClipboardCharHook(event, false);
	});

	wxArrayString choices;
	for (int i = 1; i <= 24; ++i)
		choices.Add(fmt_wx("%g%%", i * 12.5));
	auto zoomBox = new wxComboBox(this, -1, wxS("75%"), wxDefaultPosition, wxDefaultSize, choices, wxCB_DROPDOWN | wxTE_PROCESS_ENTER);

	auto visualToolBar = toolbar::GetToolbar(this, "visual_tools", context, "Video", true);
	auto visualSubToolBar = new wxToolBar(this, -1, wxDefaultPosition, wxDefaultSize, wxTB_VERTICAL | wxTB_BOTTOM | wxTB_NODIVIDER | wxTB_FLAT);

	videoDisplay = new VideoDisplay(visualSubToolBar, isDetached, zoomBox, this, context);
	videoDisplay->MoveBeforeInTabOrder(videoSlider);

	auto toolbarSizer = new wxBoxSizer(wxVERTICAL);
	toolbarSizer->Add(visualToolBar, wxSizerFlags(1));
	toolbarSizer->Add(visualSubToolBar, wxSizerFlags());

	auto topSizer = new wxBoxSizer(wxHORIZONTAL);
	topSizer->Add(toolbarSizer, 0, wxEXPAND);
	topSizer->Add(videoDisplay, isDetached, isDetached ? wxEXPAND : 0);

	auto videoBottomSizer = new wxBoxSizer(wxHORIZONTAL);
	videoBottomSizer->Add(mainToolbar, wxSizerFlags(0).Center());
	videoBottomSizer->Add(VideoFrameInput, wxSizerFlags(0).Center().Border(wxLEFT));
	videoBottomSizer->Add(VideoPosition, wxSizerFlags(1).Center().Border(wxLEFT));
	videoBottomSizer->Add(VideoSubsPos, wxSizerFlags(1).Center().Border(wxLEFT));
	videoBottomSizer->Add(zoomBox, wxSizerFlags(0).Center().Border(wxLEFT | wxRIGHT));

	auto VideoSizer = new wxBoxSizer(wxVERTICAL);
	VideoSizer->Add(topSizer, 1, wxEXPAND, 0);
	VideoSizer->Add(new wxStaticLine(this), 0, wxEXPAND, 0);
	VideoSizer->Add(videoSlider, 0, wxEXPAND, 0);
	VideoSizer->Add(videoBottomSizer, 0, wxEXPAND | wxBOTTOM, 5);
	secondarySubtitleStripSeparator = new wxStaticLine(this);
	secondarySubtitleStrip = new SecondarySubtitleStrip(this, context, secondarySubtitleSession, isDetached);
	VideoSizer->Add(secondarySubtitleStripSeparator, 0, wxEXPAND, 0);
	VideoSizer->Add(secondarySubtitleStrip, 0, wxEXPAND, 0);
	VideoSizer->Show(secondarySubtitleStripSeparator, false);
	VideoSizer->Show(secondarySubtitleStrip, false);
	secondarySubtitleStrip->SetPresentationActive(false);
	SetSizer(VideoSizer);
	Bind(wxEVT_SIZE, &VideoBox::OnSize, this);

	ApplyVideoProvider();
	// Detached VideoBox is inserted into its top-level sizer after construction.
	// Its owner synchronizes strip visibility once that sizer is in place.
	if (!isDetached)
		UpdateSecondarySubtitleStripVisibility();

	auto core = context->GetCore();
	connections = agi::signal::make_vector({
		core.ass->AddCommitListener(&VideoBox::OnSubtitlesCommit, this),
		core.project->AddKeyframesListener(&VideoBox::UpdateTimeBoxes, this),
		core.project->AddTimecodesListener(&VideoBox::UpdateTimeBoxes, this),
		core.project->AddVideoProviderListener(&VideoBox::OnVideoProviderChanged, this),
		core.selectionController->AddSelectionListener(&VideoBox::UpdateTimeBoxes, this),
		core.videoController->AddFramePresentedListener(&VideoBox::OnCurrentFrameChanged, this),
		videoDisplay->AddBaseViewportChangedListener(&VideoBox::UpdateSecondarySubtitleStripGutter, this),
		OPT_SUB("Video/Detached/Enabled", &VideoBox::OnDetachedVideoChanged, this),
		OPT_SUB("Video/Secondary Subtitles/Enabled", &VideoBox::OnSecondarySubtitleStripEnabledChanged, this),
	});
}

void VideoBox::SyncToContextState() {
	if (closing)
		return;

	ApplyVideoProvider();
	if (auto video_display = context->GetUI().videoDisplay)
		video_display->SyncToCurrentVideoProvider();
	UpdateSecondarySubtitleStripVisibility();
}

void VideoBox::SyncSecondarySubtitleStripVisibility() {
	if (closing)
		return;
	UpdateSecondarySubtitleStripVisibility();
}

void VideoBox::ApplyVideoProvider() {
	ApplyVideoProvider(context->GetCore().project->VideoProvider());
}

void VideoBox::ApplyVideoProvider(AsyncVideoProvider *provider) {
	if (closing)
		return;

	auto core = context->GetCore();
	hasVideoProvider = provider != nullptr;
	current_frame = provider ? core.videoController->GetFrameN() : -1;
	if (provider) {
		VideoFrameInput->SetRange(0, provider->GetFrameCount() - 1);
		VideoFrameInput->SetValue(current_frame);
		VideoFrameInput->Enable();
	}
	else {
		VideoFrameInput->SetRange(0, 0);
		VideoFrameInput->SetValue(0);
		VideoFrameInput->Disable();
	}
	UpdateTimeBoxes();
}

void VideoBox::JumpToInputFrame() {
	auto core = context->GetCore();
	auto provider = core.project->VideoProvider();
	if (!provider)
		return;

	int const target_frame = std::clamp(VideoFrameInput->GetValue(), 0, provider->GetFrameCount() - 1);
	VideoFrameInput->SetValue(target_frame);
	core.videoController->Stop();
	core.videoController->JumpToFrame(target_frame);
}

bool VideoBox::OpenSecondarySubtitlesFromPath(agi::fs::path const& path, bool show_errors) {
	return !closing && secondarySubtitleStrip
		&& secondarySubtitleStrip->OpenExternalSubtitlesFromPath(path, show_errors);
}

void VideoBox::OnSubtitlesCommit(int type) {
	if (type != AssFile::COMMIT_DIAG_TEXT) {
		UpdateTimeBoxes();
	}
}

void VideoBox::UpdateTimeBoxes() {
	if (closing || !hasVideoProvider)
		return;

	auto core = context->GetCore();

	int frame = current_frame >= 0 ? current_frame : core.videoController->GetFrameN();
	int time = core.videoController->TimeAtFrame(frame, agi::vfr::EXACT);

	// Set the text box for the current frame time
	VideoPosition->SetValue(to_wx(agi::Time(time).GetAssFormatted(true)));
	if (std::binary_search(core.project->Keyframes().begin(), core.project->Keyframes().end(), frame)) {
		// Set the background color to indicate this is a keyframe
		VideoPosition->SetBackgroundColour(to_wx(OPT_GET("Colour/Subtitle Grid/Background/Selection")->GetColor()));
		VideoPosition->SetForegroundColour(to_wx(OPT_GET("Colour/Subtitle Grid/Selection")->GetColor()));
	}
	else {
		VideoPosition->SetBackgroundColour(wxNullColour);
		VideoPosition->SetForegroundColour(wxNullColour);
	}

	AssDialogue *active_line = core.selectionController->GetActiveLine();
	if (!active_line)
		VideoSubsPos->SetValue(wxString());
	else {
		VideoSubsPos->SetValue(fmt_wx(
			"%+dms; %+dms; %dms",
			time - active_line->Start,
			time - active_line->End, active_line->End - active_line->Start));
	}

	VideoPosition->Refresh(false);
	VideoPosition->Update();
	VideoSubsPos->Refresh(false);
	VideoSubsPos->Update();
}

void VideoBox::OnCurrentFrameChanged(int frame_number) {
	if (closing)
		return;

	// Keep an unmodified frame input synchronized even while it owns focus. A
	// focused spin control can still be merely selected; only preserve its value
	// when it differs from the frame that was previously presented, which means
	// the user is in the middle of editing a jump target.
	bool const input_tracks_current_frame =
		VideoFrameInput->GetValue() == current_frame;
	current_frame = frame_number;
	if (input_tracks_current_frame || !VideoFrameInput->HasFocus())
		VideoFrameInput->SetValue(frame_number);
	UpdateTimeBoxes();
}

void VideoBox::UpdateSecondarySubtitleStripVisibility() {
	if (closing || !secondarySubtitleStrip || !secondarySubtitleStripSeparator || !GetSizer())
		return;

	bool const secondary_enabled = OPT_GET("Video/Secondary Subtitles/Enabled")->GetBool();
	bool const detached_mode = OPT_GET("Video/Detached/Enabled")->GetBool();
	bool const show_strip = ShouldShowSecondarySubtitleStrip(
		hasVideoProvider,
		secondary_enabled,
		isDetached,
		detached_mode);
	if (ownsSecondarySubtitleSession)
		secondarySubtitleSession->SetActive(hasVideoProvider && secondary_enabled);
	bool const visibility_changed = secondarySubtitleStrip->IsShown() != show_strip;
	int const preserved_video_height = videoDisplay ? videoDisplay->GetClientSize().GetHeight() : 0;
	int const previous_min_height = GetSecondarySubtitleLayoutMinHeight();

	GetSizer()->Show(secondarySubtitleStripSeparator, show_strip);
	GetSizer()->Show(secondarySubtitleStrip, show_strip);
	secondarySubtitleStrip->SetPresentationActive(show_strip && presentationAvailable);
	if (visibility_changed) {
		int const new_min_height = GetSecondarySubtitleLayoutMinHeight();
		RelayoutAfterSecondarySubtitleStripChange(
			preserved_video_height,
			new_min_height - previous_min_height);
	}
	else {
		GetSizer()->Layout();
		Layout();
		UpdateSecondarySubtitleStripGutter();
	}
}

void VideoBox::UpdateSecondarySubtitleStripGutter() {
	if (closing || !secondarySubtitleStrip || !videoDisplay)
		return;

	int const display_left = std::max(videoDisplay->GetPosition().x, 0);
	wxRect const viewport = videoDisplay->GetBaseViewportRect();
	secondarySubtitleStrip->SetHorizontalLayout(
		display_left,
		display_left + viewport.x,
		viewport.width);
}

int VideoBox::GetSecondarySubtitleLayoutMinHeight() const {
	auto *sizer = const_cast<VideoBox *>(this)->GetSizer();
	return sizer ? sizer->CalcMin().GetHeight() : 0;
}

void VideoBox::RelayoutAfterSecondarySubtitleStripChange(int preserved_video_height, int preferred_client_height_delta) {
	if (closing || !videoDisplay)
		return;

	videoDisplay->BeginInternalLayoutResize();
	auto end_internal_resize = agi::make_scope_exit([this] {
		if (!closing && videoDisplay)
			videoDisplay->EndInternalLayoutResize();
	});

	for (wxWindow *window = this; window; window = window->GetParent())
		window->InvalidateBestSize();

	auto relayout = [this] {
		if (GetSizer())
			GetSizer()->Layout();
		Layout();
		UpdateSecondarySubtitleStripGutter();
		if (auto *parent = GetParent())
			parent->Layout();
	};

	auto *top = dynamic_cast<wxTopLevelWindow *>(wxGetTopLevelParent(this));
	bool const can_resize_top = top && !top->IsMaximized() && !top->IsFullScreen();
	auto resize_top_client = [top, can_resize_top](int delta_height) {
		if (!can_resize_top || delta_height == 0)
			return;

		wxSize target_client_size = top->GetClientSize() + wxSize(0, delta_height);
		target_client_size.SetHeight(std::max(target_client_size.GetHeight(), 1));
		top->SetClientSize(target_client_size);
		top->SendSizeEvent(0);
	};

	resize_top_client(preferred_client_height_delta);
	relayout();

	if (preserved_video_height > 0) {
		int const video_height_delta = preserved_video_height - videoDisplay->GetClientSize().GetHeight();
		resize_top_client(video_height_delta);
	}
	relayout();

	Refresh();
	Update();
	if (auto *parent = GetParent()) {
		parent->Refresh();
		parent->Update();
	}
}

void VideoBox::OnVideoProviderChanged(AsyncVideoProvider *provider) {
	if (closing)
		return;

	ApplyVideoProvider(provider);
	UpdateSecondarySubtitleStripVisibility();
}

void VideoBox::OnDetachedVideoChanged(agi::OptionValue const&) {
	if (closing)
		return;
	UpdateSecondarySubtitleStripVisibility();
}

void VideoBox::OnSecondarySubtitleStripEnabledChanged(agi::OptionValue const&) {
	if (closing)
		return;
	UpdateSecondarySubtitleStripVisibility();
}

void VideoBox::OnSecondarySubtitleStripHeightChanged(int previous_height, int new_height) {
	if (closing || !secondarySubtitleStrip || !secondarySubtitleStrip->IsShown() || !videoDisplay)
		return;

	if (previous_height == new_height)
		return;

	if (secondarySubtitleStripHeightDragActive) {
		PreviewSecondarySubtitleStripHeightChange();
		return;
	}

	int const preserved_video_height = videoDisplay->GetClientSize().GetHeight();
	RelayoutAfterSecondarySubtitleStripChange(preserved_video_height, new_height - previous_height);
}

void VideoBox::BeginSecondarySubtitleStripHeightDrag() {
	if (closing || secondarySubtitleStripHeightDragActive)
		return;

	secondarySubtitleStripHeightDragActive = true;
	if (videoDisplay)
		videoDisplay->BeginInternalLayoutResize();
	secondarySubtitleStripHeightDragPreservedVideoHeight =
		videoDisplay ? videoDisplay->GetClientSize().GetHeight() : 0;
}

void VideoBox::PreviewSecondarySubtitleStripHeightChange() {
	if (closing)
		return;

	for (wxWindow *window = this; window; window = window->GetParent())
		window->InvalidateBestSize();

	if (GetSizer())
		GetSizer()->Layout();
	Layout();
	UpdateSecondarySubtitleStripGutter();
	if (auto *parent = GetParent()) {
		parent->Layout();
	}
	if (secondarySubtitleStripSeparator && secondarySubtitleStripSeparator->IsShown())
		secondarySubtitleStripSeparator->Refresh(false);
	if (secondarySubtitleStrip) {
		secondarySubtitleStrip->Refresh(false);
		secondarySubtitleStrip->Update();
	}
	if (videoDisplay) {
		videoDisplay->Refresh(false);
		videoDisplay->Update();
	}
}

void VideoBox::CommitSecondarySubtitleStripHeightDrag() {
	if (!secondarySubtitleStripHeightDragActive)
		return;

	secondarySubtitleStripHeightDragActive = false;
	if (closing) {
		if (videoDisplay)
			videoDisplay->EndInternalLayoutResize();
		secondarySubtitleStripHeightDragPreservedVideoHeight = 0;
		return;
	}

	RelayoutAfterSecondarySubtitleStripChange(
		secondarySubtitleStripHeightDragPreservedVideoHeight,
		0);
	if (videoDisplay)
		videoDisplay->EndInternalLayoutResize();
	secondarySubtitleStripHeightDragPreservedVideoHeight = 0;
}

void VideoBox::SetSecondarySubtitlePresentationAvailable(bool available) {
	if (closing || presentationAvailable == available)
		return;

	presentationAvailable = available;
	if (secondarySubtitleStrip)
		secondarySubtitleStrip->SetPresentationActive(
			available && secondarySubtitleStrip->IsShown());
}

void VideoBox::PrepareForDetachedClose() {
	if (closing)
		return;

	closing = true;
	presentationAvailable = false;
	if (secondarySubtitleStrip)
		secondarySubtitleStrip->SetPresentationActive(false);
	if (secondarySubtitleStripHeightDragActive) {
		secondarySubtitleStripHeightDragActive = false;
		if (videoDisplay)
			videoDisplay->EndInternalLayoutResize();
	}
	secondarySubtitleStripHeightDragPreservedVideoHeight = 0;
	connections.clear();
}

void VideoBox::OnSize(wxSizeEvent &event) {
	event.Skip();
	if (closing || !secondarySubtitleStrip)
		return;

	wxWeakRef<VideoBox> weak_this(this);
	CallAfter([weak_this] {
		if (auto *self = weak_this.get(); self && !self->closing
			&& !self->IsBeingDeleted() && self->secondarySubtitleStrip)
			self->UpdateSecondarySubtitleStripGutter();
	});
}
