// Copyright (c) 2005, Rodrigo Braz Monteiro, Niels Martin Hansen
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

#include "audio_box.h"

#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/toolbar.h"

#include "audio_controller.h"
#include "audio_display.h"
#include "audio_karaoke.h"
#include "audio_timing.h"
#include "options.h"
#include "project.h"
#include "toggle_bitmap.h"
#include "utils.h"

#ifdef AEGISUB_WITH_SKIA_AUDIO_DISPLAY
#include "skia/audio/skia_audio_display_slot.h"
#endif

#include <cmath>
#include <wx/button.h>
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/slider.h>
#include <wx/scrolbar.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/string.h>
#include <wx/toolbar.h>

enum {
	Audio_Horizontal_Zoom = 1600,
	Audio_Vertical_Zoom,
	Audio_Volume,
	Audio_SpectrumOptions
};

AudioBox::AudioBox(wxWindow *parent, agi::Context *context)
: wxSashWindow(parent, -1, wxDefaultPosition, wxDefaultSize, wxSW_3D | wxCLIP_CHILDREN)
, controller(context->GetCore().audioController.get())
, context(context)
, audio_open_connection(context->GetCore().audioController->AddAudioPlayerOpenListener(&AudioBox::OnAudioOpen, this))
, panel(new wxPanel(this, -1, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_RAISED))
#ifdef AEGISUB_WITH_SKIA_AUDIO_DISPLAY
, audioDisplay(std::make_unique<aegisub::skia::audio::AudioDisplaySlot>(
	panel,
	context->GetCore().audioController.get(),
	context,
	[this](wxWindow *window) { BindAudioDisplayWindow(window); }))
#else
, audioDisplay(new AudioDisplay(panel, context->GetCore().audioController.get(), context))
#endif
, HorizontalZoom(new wxSlider(panel, Audio_Horizontal_Zoom, -OPT_GET("Audio/Zoom/Horizontal")->GetInt(), -50, 30, wxDefaultPosition, wxDefaultSize, wxSL_VERTICAL|wxSL_BOTH))
, VerticalZoom(new wxSlider(panel, Audio_Vertical_Zoom, OPT_GET("Audio/Zoom/Vertical")->GetInt(), 0, 100, wxDefaultPosition, wxDefaultSize, wxSL_VERTICAL|wxSL_BOTH|wxSL_INVERSE))
, VolumeBar(new wxSlider(panel, Audio_Volume, OPT_GET("Audio/Volume")->GetInt(), 0, 100, wxDefaultPosition, wxDefaultSize, wxSL_VERTICAL|wxSL_BOTH|wxSL_INVERSE))
{
	SetSashVisible(wxSASH_BOTTOM, true);
	Bind(wxEVT_SASH_DRAGGED, &AudioBox::OnSashDrag, this);

	HorizontalZoom->SetToolTip(_("Horizontal zoom"));
	VerticalZoom->SetToolTip(_("Vertical zoom"));
	VolumeBar->SetToolTip(_("Audio Volume"));

	bool link = OPT_GET("Audio/Link")->GetBool();
	if (link) {
		VolumeBar->SetValue(VerticalZoom->GetValue());
		VolumeBar->Enable(false);
	}

	// VertVol sider
	wxSizer *VertVol = new wxBoxSizer(wxHORIZONTAL);
	VertVol->Add(VerticalZoom,1,wxEXPAND,0);
	VertVol->Add(VolumeBar,1,wxEXPAND,0);
	wxSizer *VertVolArea = new wxBoxSizer(wxVERTICAL);
	VertVolArea->Add(VertVol,1,wxEXPAND,0);

	auto link_btn = new ToggleBitmap(panel, context, "audio/opt/vertical_link", 16, "Audio", wxSize(20, -1));
	link_btn->SetMaxSize(wxDefaultSize);
	VertVolArea->Add(link_btn, 0, wxRIGHT | wxEXPAND, 0);
	OPT_SUB("Audio/Link", &AudioBox::OnVerticalLink, this);

	spectrum_options_btn = new wxButton(panel, Audio_SpectrumOptions, _("CH"), wxDefaultPosition, wxSize(20, -1), wxBU_EXACTFIT);
	spectrum_options_btn->SetToolTip(_("Spectrum display options"));
	spectrum_options_btn->Enable(OPT_GET("Audio/Spectrum")->GetBool());
	VertVolArea->Add(spectrum_options_btn, 0, wxEXPAND, 0);
	spectrum_mode_connection = OPT_SUB("Audio/Spectrum", &AudioBox::OnSpectrumModeChange, this);

	// Top sizer
	wxSizer *TopSizer = new wxBoxSizer(wxHORIZONTAL);
#ifdef AEGISUB_WITH_SKIA_AUDIO_DISPLAY
	TopSizer->Add(GetAudioDisplayWindow(),1,wxEXPAND,0);
#else
	TopSizer->Add(audioDisplay,1,wxEXPAND,0);
#endif
	TopSizer->Add(HorizontalZoom,0,wxEXPAND,0);
	TopSizer->Add(VertVolArea,0,wxEXPAND,0);

	context->GetUI().karaoke = new AudioKaraoke(panel, context);

	// Main sizer
	auto MainSizer = new wxBoxSizer(wxVERTICAL);
	MainSizer->Add(TopSizer,1,wxEXPAND|wxALL,3);
	MainSizer->Add(toolbar::GetToolbar(panel, "audio", context, "Audio"),0,wxEXPAND|wxLEFT|wxRIGHT,3);
	MainSizer->Add(context->GetUI().karaoke,0,wxEXPAND|wxALL,3);
	MainSizer->Show(context->GetUI().karaoke, false);
	panel->SetSizer(MainSizer);

	wxSizer *audioSashSizer = new wxBoxSizer(wxHORIZONTAL);
	audioSashSizer->Add(panel, 1, wxEXPAND);
	SetSizerAndFit(audioSashSizer);
	SetMinSize(wxSize(-1, OPT_GET("Audio/Display Height")->GetInt()));
	SetMinimumSizeY(panel->GetSize().GetHeight());

#ifdef AEGISUB_WITH_SKIA_AUDIO_DISPLAY
	BindAudioDisplayWindow(GetAudioDisplayWindow());
#else
	audioDisplay->Bind(wxEVT_MOUSEWHEEL, &AudioBox::OnMouseWheel, this);
#endif

	audioDisplay->SetZoomLevel(-HorizontalZoom->GetValue());
	audioDisplay->SetAmplitudeScale(pow(mid(1, VerticalZoom->GetValue(), 100) / 50.0, 3));
}

#ifdef AEGISUB_WITH_SKIA_AUDIO_DISPLAY
AudioBox::~AudioBox() = default;

wxWindow *AudioBox::GetAudioDisplayWindow() const {
	return audioDisplay->Window();
}

void AudioBox::BindAudioDisplayWindow(wxWindow *window) {
	window->Bind(wxEVT_MOUSEWHEEL, &AudioBox::OnMouseWheel, this);
}
#endif

void AudioBox::SyncToContextState() {
	context->GetUI().karaoke->SyncToContextState();
	audioDisplay->SyncToCurrentAudioProvider();

	if (context->GetCore().project->AudioProvider())
		OnAudioOpen();
}

BEGIN_EVENT_TABLE(AudioBox,wxSashWindow)
	EVT_COMMAND_SCROLL(Audio_Horizontal_Zoom, AudioBox::OnHorizontalZoom)
	EVT_COMMAND_SCROLL(Audio_Vertical_Zoom, AudioBox::OnVerticalZoom)
	EVT_COMMAND_SCROLL(Audio_Volume, AudioBox::OnVolume)
	EVT_BUTTON(Audio_SpectrumOptions, AudioBox::OnSpectrumOptionsBtn)
END_EVENT_TABLE()

void AudioBox::OnMouseWheel(wxMouseEvent &evt) {
#ifdef AEGISUB_WITH_SKIA_AUDIO_DISPLAY
	if (!ForwardMouseWheelEvent(GetAudioDisplayWindow(), evt))
#else
	if (!ForwardMouseWheelEvent(audioDisplay, evt))
#endif
		return;
	bool zoom = evt.CmdDown() != OPT_GET("Audio/Wheel Default to Zoom")->GetBool() || evt.ShiftDown();
	if (!zoom) {
		int amount = -evt.GetWheelRotation();
		// If the user did a horizontal scroll the amount should be inverted
		// for it to be natural.
		if (evt.GetWheelAxis() == 1) amount = -amount;

		// Reset any accumulated zoom
		mouse_zoom_accum = 0;

		audioDisplay->ScrollBy(amount, evt.GetPosition().x);
	}
	else if (evt.GetWheelAxis() == 0) {
		mouse_zoom_accum += evt.GetWheelRotation();
		int zoom_delta = mouse_zoom_accum / evt.GetWheelDelta();
		mouse_zoom_accum %= evt.GetWheelDelta();
		if (evt.ShiftDown()) {
			SetVerticalZoom(OPT_GET("Audio/Zoom/Vertical")->GetInt() + 3 * zoom_delta);
		} else {
			SetHorizontalZoom(audioDisplay->GetZoomLevel() + zoom_delta);
		}
	}
}

void AudioBox::OnSashDrag(wxSashEvent &event) {
	if (event.GetDragStatus() == wxSASH_STATUS_OUT_OF_RANGE)
		return;

	int new_height = std::min(event.GetDragRect().GetHeight(), GetParent()->GetSize().GetHeight() - 1);

	SetMinSize(wxSize(-1, new_height));
	GetParent()->Layout();

	// Karaoke mode is always disabled when the audio box is first opened, so
	// the initial height shouldn't include it
	auto karaoke = context->GetUI().karaoke;
	if (karaoke->IsEnabled())
		new_height -= karaoke->GetSize().GetHeight() + 6;

	OPT_SET("Audio/Display Height")->SetInt(new_height);
}

void AudioBox::OnHorizontalZoom(wxScrollEvent &event) {
	// Negate the value since we want zoom out to be on bottom and zoom in on top,
	// but the control doesn't want negative on bottom and positive on top.
	SetHorizontalZoom(-event.GetPosition());
}

void AudioBox::SetHorizontalZoom(int new_zoom) {
	audioDisplay->SetZoomLevel(new_zoom);
	HorizontalZoom->SetValue(-new_zoom);
	OPT_SET("Audio/Zoom/Horizontal")->SetInt(new_zoom);
}

void AudioBox::OnVerticalZoom(wxScrollEvent &event) {
	SetVerticalZoom(event.GetPosition());
}

void AudioBox::SetVerticalZoom(int new_zoom) {
	int pos = mid(1, new_zoom, 100);
	OPT_SET("Audio/Zoom/Vertical")->SetInt(pos);
	double value = pow(pos / 50.0, 3);
	audioDisplay->SetAmplitudeScale(value);
	VerticalZoom->SetValue(pos);
	if (!VolumeBar->IsEnabled()) {
		VolumeBar->SetValue(pos);
		controller->SetVolume(value);
	}
}

void AudioBox::OnVolume(wxScrollEvent &event) {
	int pos = mid(1, event.GetPosition(), 100);
	OPT_SET("Audio/Volume")->SetInt(pos);
	controller->SetVolume(pow(pos / 50.0, 3));
}

void AudioBox::OnVerticalLink(agi::OptionValue const& opt) {
	if (opt.GetBool()) {
		int pos = mid(1, VerticalZoom->GetValue(), 100);
		double value = pow(pos / 50.0, 3);
		controller->SetVolume(value);
		VolumeBar->SetValue(pos);
	}
	VolumeBar->Enable(!opt.GetBool());
}

void AudioBox::OnAudioOpen() {
	controller->SetVolume(pow(mid(1, VolumeBar->GetValue(), 100) / 50.0, 3));
	if (spectrum_options_btn)
		spectrum_options_btn->Enable(OPT_GET("Audio/Spectrum")->GetBool());
}

void AudioBox::OnSpectrumModeChange(agi::OptionValue const& opt) {
	if (spectrum_options_btn)
		spectrum_options_btn->Enable(opt.GetBool());
}

void AudioBox::OnSpectrumOptionsBtn(wxCommandEvent &) {
	enum {
		ID_MONO_AVG = wxID_HIGHEST + 2000,
		ID_MONO_BIN_MAX,
		ID_MONO_BIN_AVG,
		ID_SAMPLE_DEV_S16,
		ID_SAMPLE_FLOAT32,
		ID_COMP_LEGACY,
		ID_COMP_CURVE,
		ID_CURVE_LINEAR,
		ID_CURVE_EXTENDED,
		ID_CURVE_MEDIUM,
		ID_CURVE_COMPRESSED,
		ID_CURVE_LOG,
	};

	const int current_mono_mode = mid<int>(0, OPT_GET("Audio/Renderer/Spectrum/Mono Mix Mode")->GetInt(), 2);
	const int current_input_format = mid<int>(0, OPT_GET("Audio/Renderer/Spectrum/Input Format")->GetInt(), 1);
	const int current_computation_mode = mid<int>(0, OPT_GET("Audio/Renderer/Spectrum/Computation Mode")->GetInt(), 1);
	const int current_freq_curve = mid<int>(0, OPT_GET("Audio/Renderer/Spectrum/FreqCurve")->GetInt(), 4);

	wxMenu menu;

	wxMenu *sample_menu = new wxMenu();
	sample_menu->AppendRadioItem(ID_SAMPLE_DEV_S16, _("s16 mono"))->Check(current_input_format == 0);
	sample_menu->AppendRadioItem(ID_SAMPLE_FLOAT32, _("float32"))->Check(current_input_format == 1);
	menu.AppendSubMenu(sample_menu, _("Input format"));

	wxMenu *mono_menu = new wxMenu();
	mono_menu->AppendRadioItem(ID_MONO_AVG, _("Time-domain downmix"))->Check(current_input_format == 0 || current_mono_mode == 0);
	auto *mono_bin_max = mono_menu->AppendRadioItem(ID_MONO_BIN_MAX, _("Strongest channel per frequency bin"));
	mono_bin_max->Check(current_input_format == 1 && current_mono_mode == 1);
	mono_bin_max->Enable(current_input_format == 1);
	auto *mono_bin_avg = mono_menu->AppendRadioItem(ID_MONO_BIN_AVG, _("Average channel energy per frequency bin"));
	mono_bin_avg->Check(current_input_format == 1 && current_mono_mode == 2);
	mono_bin_avg->Enable(current_input_format == 1);
	menu.AppendSubMenu(mono_menu, _("Mono mix method"));

	wxMenu *computation_menu = new wxMenu();
	computation_menu->AppendRadioItem(ID_COMP_LEGACY, _("Legacy linear"))->Check(current_computation_mode == 0);
	computation_menu->AppendRadioItem(ID_COMP_CURVE, _("Frequency curve"))->Check(current_computation_mode == 1);
	menu.AppendSubMenu(computation_menu, _("Spectrum computation mode"));

	wxMenu *curve_menu = new wxMenu();
	curve_menu->AppendRadioItem(ID_CURVE_LINEAR, _("Linear"))->Check(current_freq_curve == 0);
	curve_menu->AppendRadioItem(ID_CURVE_EXTENDED, _("Extended"))->Check(current_freq_curve == 1);
	curve_menu->AppendRadioItem(ID_CURVE_MEDIUM, _("Medium"))->Check(current_freq_curve == 2);
	curve_menu->AppendRadioItem(ID_CURVE_COMPRESSED, _("Compressed"))->Check(current_freq_curve == 3);
	curve_menu->AppendRadioItem(ID_CURVE_LOG, _("Logarithmic"))->Check(current_freq_curve == 4);
	auto *curve_menu_item = menu.AppendSubMenu(curve_menu, _("Spectrum frequency mapping"));
	curve_menu_item->Enable(current_computation_mode == 1);

	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		CallAfter([] { OPT_SET("Audio/Renderer/Spectrum/Mono Mix Mode")->SetInt(0); });
	}, ID_MONO_AVG);
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		CallAfter([] { OPT_SET("Audio/Renderer/Spectrum/Mono Mix Mode")->SetInt(1); });
	}, ID_MONO_BIN_MAX);
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		CallAfter([] { OPT_SET("Audio/Renderer/Spectrum/Mono Mix Mode")->SetInt(2); });
	}, ID_MONO_BIN_AVG);
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		CallAfter([] { OPT_SET("Audio/Renderer/Spectrum/Input Format")->SetInt(0); });
	}, ID_SAMPLE_DEV_S16);
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		CallAfter([] { OPT_SET("Audio/Renderer/Spectrum/Input Format")->SetInt(1); });
	}, ID_SAMPLE_FLOAT32);
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		CallAfter([] { OPT_SET("Audio/Renderer/Spectrum/Computation Mode")->SetInt(0); });
	}, ID_COMP_LEGACY);
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		CallAfter([] { OPT_SET("Audio/Renderer/Spectrum/Computation Mode")->SetInt(1); });
	}, ID_COMP_CURVE);
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		CallAfter([] { OPT_SET("Audio/Renderer/Spectrum/FreqCurve")->SetInt(0); });
	}, ID_CURVE_LINEAR);
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		CallAfter([] { OPT_SET("Audio/Renderer/Spectrum/FreqCurve")->SetInt(1); });
	}, ID_CURVE_EXTENDED);
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		CallAfter([] { OPT_SET("Audio/Renderer/Spectrum/FreqCurve")->SetInt(2); });
	}, ID_CURVE_MEDIUM);
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		CallAfter([] { OPT_SET("Audio/Renderer/Spectrum/FreqCurve")->SetInt(3); });
	}, ID_CURVE_COMPRESSED);
	menu.Bind(wxEVT_MENU, [this](wxCommandEvent &) {
		CallAfter([] { OPT_SET("Audio/Renderer/Spectrum/FreqCurve")->SetInt(4); });
	}, ID_CURVE_LOG);

	PopupMenu(&menu);
}

void AudioBox::ShowKaraokeBar(bool show) {
	wxSizer *panel_sizer = panel->GetSizer();
	auto karaoke = context->GetUI().karaoke;
	if (panel_sizer->IsShown(karaoke) == show) return;

	int new_height = GetSize().GetHeight();
	int kara_height = karaoke->GetSize().GetHeight() + 6;

	if (show)
		new_height += kara_height;
	else
		new_height -= kara_height;

	panel_sizer->Show(karaoke, show);
	SetMinSize(wxSize(-1, new_height));
	GetParent()->Layout();
}

void AudioBox::ScrollAudioBy(int pixel_amount) {
	audioDisplay->ScrollBy(pixel_amount);
}

void AudioBox::ScrollToTime(int time_ms) {
	audioDisplay->ScrollToTime(time_ms);
}

void AudioBox::ScrollToActiveLine() {
	if (controller->GetTimingController())
		audioDisplay->ScrollTimeRangeInView(controller->GetTimingController()->GetIdealVisibleTimeRange());
}
