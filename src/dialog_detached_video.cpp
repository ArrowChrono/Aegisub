// Copyright (c) 2007, Rodrigo Braz Monteiro
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

/// @file dialog_detached_video.cpp
/// @brief Detached video window
/// @ingroup main_ui
///

#include "dialog_detached_video.h"

#include "dialog_manager.h"
#include "dialog_motion_track.h"

#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/hotkey.h"
#include "options.h"
#include "persist_location.h"
#include "project.h"
#include "utils.h"
#include "video_box.h"
#include "video_controller.h"
#include "video_display.h"
#include "visual_tool_clip.h"
#include "visual_tool_cross.h"
#include "visual_tool_drag.h"
#include "visual_tool_measure.h"
#include "visual_tool_motion_track.h"
#include "visual_tool_rotatexy.h"
#include "visual_tool_rotatez.h"
#include "visual_tool_scale.h"
#include "visual_tool_shear.h"
#include "visual_tool_vector_clip.h"

#include <libaegisub/format_path.h>
#include <libaegisub/make_unique.h>

#include <filesystem>

#include <wx/sizer.h>
#include <wx/display.h> /// Must be included last.

namespace {
enum class SavedVisualTool {
	None,
	MotionTrack,
	Cross,
	Drag,
	Measure,
	RotateZ,
	RotateXY,
	Scale,
	Shear,
	Clip,
	VectorClip,
};

struct SavedVisualToolState {
	SavedVisualTool tool = SavedVisualTool::None;
	int submode = -1;
};

SavedVisualToolState DetectVisualTool(VideoDisplay *display) {
	if (!display)
		return {};

	auto const state = [display](SavedVisualTool tool) {
		return SavedVisualToolState {tool, display->GetToolSubMode()};
	};
	if (display->ToolIsType(typeid(VisualToolCross)))
		return state(SavedVisualTool::Cross);
	if (display->ToolIsType(typeid(VisualToolDrag)))
		return state(SavedVisualTool::Drag);
	if (display->ToolIsType(typeid(VisualToolMotionTrack)))
		return state(SavedVisualTool::MotionTrack);
	if (display->ToolIsType(typeid(VisualToolMeasure)))
		return state(SavedVisualTool::Measure);
	if (display->ToolIsType(typeid(VisualToolRotateZ)))
		return state(SavedVisualTool::RotateZ);
	if (display->ToolIsType(typeid(VisualToolRotateXY)))
		return state(SavedVisualTool::RotateXY);
	if (display->ToolIsType(typeid(VisualToolScale)))
		return state(SavedVisualTool::Scale);
	if (display->ToolIsType(typeid(VisualToolShear)))
		return state(SavedVisualTool::Shear);
	if (display->ToolIsType(typeid(VisualToolClip)))
		return state(SavedVisualTool::Clip);
	if (display->ToolIsType(typeid(VisualToolVectorClip)))
		return state(SavedVisualTool::VectorClip);
	return {};
}

void RestoreVisualTool(VideoDisplay *display, agi::Context *context, SavedVisualToolState state) {
	if (!display || !context)
		return;

	switch (state.tool) {
		case SavedVisualTool::Cross:
			display->SetTool(agi::make_unique<VisualToolCross>(display, context));
			break;
		case SavedVisualTool::Drag:
			display->SetTool(agi::make_unique<VisualToolDrag>(display, context));
			break;
		case SavedVisualTool::MotionTrack:
			// Only restore when the motion-track dialog is still alive.
			if (context->GetUI().dialog->Get<DialogMotionTrack>())
				display->SetTool(agi::make_unique<VisualToolMotionTrack>(display, context));
			else
				display->SetTool(agi::make_unique<VisualToolCross>(display, context));
			break;
		case SavedVisualTool::Measure:
			display->SetTool(agi::make_unique<VisualToolMeasure>(display, context));
			break;
		case SavedVisualTool::RotateZ:
			display->SetTool(agi::make_unique<VisualToolRotateZ>(display, context));
			break;
		case SavedVisualTool::RotateXY:
			display->SetTool(agi::make_unique<VisualToolRotateXY>(display, context));
			break;
		case SavedVisualTool::Scale:
			display->SetTool(agi::make_unique<VisualToolScale>(display, context));
			break;
		case SavedVisualTool::Shear:
			display->SetTool(agi::make_unique<VisualToolShear>(display, context));
			break;
		case SavedVisualTool::Clip:
			display->SetTool(agi::make_unique<VisualToolClip>(display, context));
			break;
		case SavedVisualTool::VectorClip:
			display->SetTool(agi::make_unique<VisualToolVectorClip>(display, context));
			break;
		case SavedVisualTool::None:
			break;
	}

	if (state.submode >= 0)
		display->SetToolSubMode(state.submode);
}
}

DialogDetachedVideo::DialogDetachedVideo(agi::Context *context)
: wxDialog(context->GetUI().parent, -1, wxS("Detached Video"), wxDefaultPosition, wxSize(400,300), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX | wxMINIMIZE_BOX | wxWANTS_CHARS)
, context(context)
, old_display(context->GetUI().videoDisplay)
, old_slider(context->GetUI().videoSlider)
, video_open(context->GetCore().project->AddVideoProviderListener(&DialogDetachedVideo::OnVideoOpen, this))
{
	auto core = context->GetCore();
	auto ui = context->GetUI();

	SetSize(FromDIP(wxSize(400, 300)));
	// Set obscure stuff
	SetExtraStyle((GetExtraStyle() & ~wxWS_EX_BLOCK_EVENTS) | wxWS_EX_PROCESS_UI_UPDATES);

	SetTitle(fmt_tl("Video: %s", core.project->VideoName().filename()));

	auto const initial_tool = DetectVisualTool(old_display);
	old_display->Unload();

	// Video area;
	video_box = new VideoBox(this, true, context);
	ui.videoDisplay->SetMinClientSize(old_display->GetClientSize());
	video_box->Layout();
	RestoreVisualTool(ui.videoDisplay, context, initial_tool);

	// Set sizer
	wxSizer *mainSizer = new wxBoxSizer(wxVERTICAL);
	mainSizer->Add(video_box,1,wxEXPAND);
	SetSizerAndFit(mainSizer);
	video_box->SyncSecondarySubtitleStripVisibility();

	// Ensure we can grow smaller, without these the window is locked to at least the initial size
	ui.videoDisplay->SetMinSize(wxSize(1,1));
	video_box->SetMinSize(wxSize(1,1));
	SetMinSize(wxSize(1,1));

	persist = agi::make_unique<PersistLocation>(this, "Video/Detached");

	int display_index = wxDisplay::GetFromWindow(this);
	// Ensure that the dialog is no larger than the screen
	if (display_index != wxNOT_FOUND) {
		wxRect bounds_rect = GetRect();
		wxRect disp_rect = wxDisplay(display_index).GetClientArea();
		SetSize(std::min(bounds_rect.width, disp_rect.width), std::min(bounds_rect.height, disp_rect.height));
	}

	OPT_SET("Video/Detached/Enabled")->SetBool(true);

	Bind(wxEVT_CLOSE_WINDOW, &DialogDetachedVideo::OnClose, this);
	Bind(wxEVT_ICONIZE, &DialogDetachedVideo::OnMinimize, this);
	Bind(wxEVT_CHAR_HOOK, &DialogDetachedVideo::OnKeyDown, this);

	AddFullScreenButton(this);
}

DialogDetachedVideo::~DialogDetachedVideo() {
	// The re-dock bookkeeping lives in OnClose, which the teardown path skips
	// (the dialog is deleted directly, without a close event). Put the
	// context's pointers back onto the attached controls before this dialog
	// destroys its child display, so whatever runs later in teardown — e.g.
	// DialogMotionTrack's ResetOverlayTool — never follows the dying detached
	// display. Idempotent after OnClose, which restores the same values.
	auto ui = context->GetUI();
	ui.videoDisplay = old_display;
	ui.videoSlider = old_slider;
}

void DialogDetachedVideo::OnClose(wxCloseEvent &evt) {
	if (close_started) {
		evt.Skip();
		return;
	}
	close_started = true;

	auto core = context->GetCore();
	auto ui = context->GetUI();
	auto const current_tool = DetectVisualTool(ui.videoDisplay);
	auto *detached_display = ui.videoDisplay != old_display ? ui.videoDisplay : nullptr;

	// Stop the presenter before changing the shared option. The option signal
	// is synchronous, and the detached VideoBox is still alive until the dialog
	// is destroyed after this handler returns.
	if (video_box)
		video_box->PrepareForDetachedClose();
	if (detached_display)
		detached_display->Hide();

	ui.videoDisplay = old_display;
	ui.videoSlider = old_slider;

	// Show the attached video box before restoring the visual tool. Attached
	// tools call back into VideoDisplay::UpdateSize(), which is a no-op while
	// the display is hidden.
	OPT_SET("Video/Detached/Enabled")->SetBool(false);
	RestoreVisualTool(ui.videoDisplay, context, current_tool);

	core.videoController->JumpToFrame(core.videoController->GetFrameN());
	ui.videoDisplay->Refresh(false);

	if (detached_display)
		detached_display->Destroy();

	evt.Skip();
}

void DialogDetachedVideo::OnMinimize(wxIconizeEvent &event) {
	if (video_box && !close_started)
		video_box->SetSecondarySubtitlePresentationAvailable(!event.IsIconized());

	if (event.IsIconized()) {
		// Force the video display to repaint as otherwise the last displayed
		// frame stays visible even though the dialog is minimized
		Hide();
		Show();
	}
}

void DialogDetachedVideo::OnKeyDown(wxKeyEvent &evt) {
	hotkey::check("Video Display", context, evt);
}

void DialogDetachedVideo::OnVideoOpen(AsyncVideoProvider *new_provider) {
	if (close_started)
		return;

	auto core = context->GetCore();

	if (new_provider)
		SetTitle(fmt_tl("Video: %s", core.project->VideoName().filename()));
	else {
		Close();
		OPT_SET("Video/Detached/Enabled")->SetBool(true);
	}
}
