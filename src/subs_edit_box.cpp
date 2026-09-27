// Copyright (c) 2005, Rodrigo Braz Monteiro
// Copyright (c) 2010, Thomas Goyne <plorkyeran@aegisub.org>
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

/// @file subs_edit_box.cpp
/// @brief Main subtitle editing area, including toolbars around the text control
/// @ingroup main_ui

#include "subs_edit_box.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "automation/karaoke_line_classifier.h"
#include "base_grid.h"
#include "command/command.h"
#include "compat.h"
#include "dialog_style_editor.h"
#include "flyweight_hash.h"
#include "font_family_catalog_ui.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/hotkey.h"
#include "initial_line_state.h"
#include "options.h"
#include "perf_trace.h"
#include "scoped_no_composited.h"
#include "project.h"
#include "placeholder_ctrl.h"
#include "selection_controller.h"
#include "status_sink.h"
#include "subtitle_edit_box_focus.h"
#include "subtitle_edit_box_input.h"
#include "subs_edit_ctrl.h"
#include "subs_controller.h"
#ifdef WITH_WXSTC
#include "subs_edit_ctrl_stc.h"
#endif
#include "text_selection_controller.h"
#include "time_display_mode.h"
#include "timeedit_ctrl.h"
#include "tooltip_manager.h"
#include "utils.h"
#include "video_display.h"

#include <libaegisub/character_count.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <unordered_set>

#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/radiobut.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>

#ifdef __WXMSW__
#include <windows.h>
#endif

namespace {
	constexpr int VISUAL_TOOL_TEXT_SYNC_INTERVAL_MS = 16;

#ifdef __WXMSW__
	std::int64_t ComboPaintTimingNowNs() noexcept {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	double ComboPaintTimingDurationMs(std::int64_t started_ns, std::int64_t finished_ns) noexcept {
		return static_cast<double>(finished_ns - started_ns) / 1'000'000.0;
	}

	class TracedSubsEditStyleComboBox final : public wxOwnerDrawnComboBox {
		struct PendingPaintTiming {
			bool pending = false;
			std::uint64_t update_id = 0;
			int previous_selection = wxNOT_FOUND;
			int selection = wxNOT_FOUND;
			std::int64_t requested_ns = 0;
		};

		PendingPaintTiming pending_paint_timing;
		std::uint64_t next_paint_timing_id = 0;

		WXLRESULT MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) override {
			if (message != WM_PAINT)
				return wxOwnerDrawnComboBox::MSWWindowProc(message, wParam, lParam);

			auto const timing = pending_paint_timing;
			pending_paint_timing.pending = false;
			auto const paint_started_ns = timing.pending ? ComboPaintTimingNowNs() : 0;
			auto const result = wxOwnerDrawnComboBox::MSWWindowProc(message, wParam, lParam);
			if (!timing.pending)
				return result;

			auto const paint_finished_ns = ComboPaintTimingNowNs();
			LOG_I("subtitle/editbox/style_paint_timing")
				<< "update_id=" << timing.update_id
				<< " phase=first_paint_complete"
				<< " previous_selection=" << timing.previous_selection
				<< " selection=" << timing.selection
				<< " paint_queue_wait_ms=" << ComboPaintTimingDurationMs(timing.requested_ns, paint_started_ns)
				<< " paint_ms=" << ComboPaintTimingDurationMs(paint_started_ns, paint_finished_ns);
			return result;
		}

	public:
		using wxOwnerDrawnComboBox::wxOwnerDrawnComboBox;

		void Select(int selection) override {
			auto const previous_selection = GetSelection();
			wxOwnerDrawnComboBox::Select(selection);
			if (perf_trace::IsCategoryEnabled(perf_trace::Category::Log)) {
				pending_paint_timing = {
					true,
					++next_paint_timing_id,
					previous_selection,
					selection,
					ComboPaintTimingNowNs(),
				};
			}
			else {
				pending_paint_timing.pending = false;
			}
		}
	};
#endif

template<typename ComboBox>
wxString new_value(ComboBox *ctrl, wxCommandEvent &evt) {
#ifdef __WXGTK__
	return ctrl->GetValue();
#else
	return evt.GetString();
#endif
}

void time_edit_char_hook(wxKeyEvent &event) {
	// Force a modified event on Enter
	if (event.GetKeyCode() == WXK_RETURN) {
		TimeEdit *edit = static_cast<TimeEdit*>(event.GetEventObject());
		edit->SetValue(edit->GetValue());
	}
	else
		event.Skip();
}

// Passing a pointer-to-member directly to a function sometimes does not work
// in VC++ 2015 Update 2, with it instead passing a null pointer
const auto AssDialogue_Actor = &AssDialogue::Actor;
const auto AssDialogue_Effect = &AssDialogue::Effect;

/// OS double-click interval; colour buttons defer single clicks by this much
/// so the second press can reroute to the pick-from-video command. MSW wraps
/// GetDoubleClickTime and GTK reads the user's gtk-double-click-time. Known
/// limitation: wxSYS_DCLICK_MSEC on Cocoa is a hardcoded 500 ms that ignores
/// the user's Double-Click Speed setting (wxWidgets' own comment there says
/// to rely on the system click count), so users with a slower system double
/// click see the dialog open before their second press arrives; reading the
/// com.apple.mouse.doubleClickInterval default behind NSEvent.doubleClick-
/// Interval would fix it once it can be verified on macOS. The constant only
/// backstops ports without the metric. The deferral runs from the click's
/// release while the OS window runs from its press, so the press hold time
/// absorbs any boundary jitter between the two clocks.
int ColorDoubleClickIntervalMs() {
	int const ms = wxSystemSettings::GetMetric(wxSYS_DCLICK_MSEC);
	return ms > 0 ? ms : 400;
}
}

SubsEditBox::SubsEditBox(wxWindow *parent, agi::Context *context)
	: c(context), command_session(context->GetCore().ass.get()), undo_timer(GetEventHandler()), visual_tool_text_sync_timer(GetEventHandler()), color_click_timer(GetEventHandler())
#ifdef WITH_WXSTC
	  ,
	  use_stc(OPT_GET("Subtitle/Use STC")->GetBool())
#endif
{
	using std::bind;

	{
#ifdef _WIN32
		ScopedWxNoComposited no_composited;
#endif
		Create(parent, -1, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxRAISED_BORDER | wxCLIP_CHILDREN, wxS("SubsEditBox"));
	}
#ifndef _WIN32
	SetDoubleBuffered(true);
#endif

	// Top controls
	top_sizer = new wxBoxSizer(wxHORIZONTAL);

	comment_box = new wxCheckBox(this,-1,_("&Comment"));
	comment_box->SetToolTip(_("Comment this line out. Commented lines don't show up on screen."));
#ifdef __WXGTK__
	// Only supported in wxgtk
	comment_box->SetCanFocus(false);
#endif
	top_sizer->Add(comment_box, wxSizerFlags().Expand().Border(wxRIGHT, 5));

	style_box = MakeStyleComboBox(wxS("Default"), &SubsEditBox::OnStyleChange, _("Style for this line"));

	style_edit_button = new wxButton(this, -1, _("Edit"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
	style_edit_button->Bind(wxEVT_BUTTON, [=](wxCommandEvent&) {
		if (active_style) {
			auto font_model = BuildFontFamilyCatalogUiModel();
			DialogStyleEditor(this, active_style, c, nullptr, std::string(), font_model).ShowModal();
		}
	});
	top_sizer->Add(style_edit_button, wxSizerFlags().Expand().Border(wxRIGHT));

	actor_box = new Placeholder<wxComboBox>(this, _("Actor"), wxDefaultSize, wxCB_DROPDOWN | wxTE_PROCESS_ENTER, _("Actor name for this speech. This is only for reference, and is mainly useless."));
	Bind(wxEVT_TEXT, &SubsEditBox::OnActorChange, this, actor_box->GetId());
	Bind(wxEVT_COMBOBOX, &SubsEditBox::OnActorChange, this, actor_box->GetId());
	top_sizer->Add(actor_box, wxSizerFlags(2).Expand().Border(wxRIGHT));

	effect_box = new Placeholder<wxComboBox>(this, _("Effect"), wxDefaultSize, wxCB_DROPDOWN | wxTE_PROCESS_ENTER, _("Effect for this line. This can be used to store extra information for karaoke scripts, or for the effects supported by the renderer."));
	Bind(wxEVT_TEXT, &SubsEditBox::OnEffectChange, this, effect_box->GetId());
	Bind(wxEVT_COMBOBOX, &SubsEditBox::OnEffectChange, this, effect_box->GetId());
	top_sizer->Add(effect_box, wxSizerFlags(3).Expand());

	char_count = new wxTextCtrl(this, -1, wxS("0"), wxDefaultPosition, wxDefaultSize, wxTE_READONLY | wxTE_CENTER);
#if wxCHECK_VERSION(3, 1, 3)
	char_count->SetInitialSize(char_count->GetSizeFromText(wxS("000")));
#else
	char_count->SetInitialSize(char_count->GetSizeFromTextSize(GetTextExtent(wxS("000"))));
#endif
	char_count->SetToolTip(_("Number of characters in the longest line of this subtitle."));
	top_sizer->Add(char_count, wxSizerFlags().Expand());

	// Middle controls
	middle_left_sizer = new wxBoxSizer(wxHORIZONTAL);

	layer = new wxSpinCtrl(this,-1,wxEmptyString,wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER,0,0x7FFFFFFF,0);
#ifdef __WXGTK3__
	// GTK3 has a bug that we cannot shrink the size of a widget, so do nothing here. See:
	//  http://gtk.10911.n7.nabble.com/gtk-widget-set-size-request-stopped-working-with-GTK3-td26274.html
	//  https://trac.wxwidgets.org/ticket/18568
#elif wxCHECK_VERSION(3, 1, 3)
	layer->SetInitialSize(layer->GetSizeFromText(wxS("00")));
#else
	layer->SetInitialSize(layer->GetSizeFromTextSize(GetTextExtent(wxS("00"))));
#endif
	layer->SetToolTip(_("Layer number"));
	// Same as the margin spin controls: keep Ctrl+C/X/V inside the field.
	layer->Bind(wxEVT_CHAR_HOOK, [](wxKeyEvent& event) {
		TextControlClipboardCharHook(event, true);
	});
	middle_left_sizer->Add(layer, wxSizerFlags().Expand());
	middle_left_sizer->AddSpacer(5);

	start_time = MakeTimeCtrl(_("Start time"), TIME_START);
	end_time   = MakeTimeCtrl(_("End time"), TIME_END);
	middle_left_sizer->AddSpacer(5);
	duration   = MakeTimeCtrl(_("Line duration"), TIME_DURATION);
	middle_left_sizer->AddSpacer(5);

	margin[0] = MakeMarginCtrl(_("Left Margin (0 = default from style)"), 0, _("left margin change"));
	margin[1] = MakeMarginCtrl(_("Right Margin (0 = default from style)"), 1, _("right margin change"));
	margin[2] = MakeMarginCtrl(_("Vertical Margin (0 = default from style)"), 2, _("vertical margin change"));
	middle_left_sizer->AddSpacer(5);

	// Middle-bottom controls
	middle_right_sizer = new wxBoxSizer(wxHORIZONTAL);
	MakeButton("edit/style/bold");
	MakeButton("edit/style/italic");
	MakeButton("edit/style/underline");
	MakeButton("edit/style/strikeout");
	MakeButton("edit/font");
	middle_right_sizer->AddSpacer(5);
	MakeColorButton("edit/color/primary", "edit/color/primary/pick/video");
	MakeColorButton("edit/color/secondary", "edit/color/secondary/pick/video");
	MakeColorButton("edit/color/outline", "edit/color/outline/pick/video");
	MakeColorButton("edit/color/shadow", "edit/color/shadow/pick/video");
	middle_right_sizer->AddSpacer(5);
	MakeButton("grid/line/next/create");
	middle_right_sizer->AddSpacer(10);

	by_ass = MakeRadio(_("A&SS"), true, _("Display ASS storage timestamps (h:mm:ss.cs)"));
	by_exact = MakeRadio(_("E&xact"), false, _("Display exact timestamps (h:mm:ss.mmm)"));
	by_frame = MakeRadio(_("F&rame"), false, _("Time by frame number"));
	by_frame->Enable(false);

	split_box = new wxCheckBox(this,-1,_("Show Original"));
	split_box->SetToolTip(_("Show the contents of the subtitle line when it was first selected above the edit box. This is sometimes useful when editing subtitles or translating subtitles into another language."));
	split_box->Bind(wxEVT_CHECKBOX, &SubsEditBox::OnSplit, this);
	middle_right_sizer->Add(split_box, wxSizerFlags().Expand());
#ifdef WITH_WXSTC
	lua_workspace_button = new wxButton(this, -1, _("Lua Workspace..."));
	lua_workspace_button->SetToolTip(_("Edit this code line as multiline Lua in the Workspace (Shift+Enter)."));
	lua_workspace_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
		CallCommand("automation/lua/open-current-line", false);
	});
	middle_right_sizer->Add(lua_workspace_button, wxSizerFlags().Expand().Border(wxLEFT, 5));
	middle_right_sizer->Hide(lua_workspace_button);
#endif

	// Main sizer
	wxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(top_sizer, wxSizerFlags().Expand().Border(wxALL, 3));
	main_sizer->Add(middle_left_sizer, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 3));
	main_sizer->Add(middle_right_sizer, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 3));

	// Text editor
#ifdef WITH_WXSTC
	if (use_stc) {
		edit_ctrl_stc = new SubsStyledTextEditCtrl(this, wxDefaultSize, wxBORDER_SUNKEN, c);
		edit_ctrl_stc->Bind(wxEVT_CHAR_HOOK, &SubsEditBox::OnKeyDown, this);
		edit_ctrl_stc->Bind(wxEVT_AUX1_DOWN, &SubsEditBox::OnMouse, this);
		edit_ctrl_stc->Bind(wxEVT_AUX2_DOWN, &SubsEditBox::OnMouse, this);
	}
	else {
#endif
		edit_ctrl_tc = new SubsTextEditCtrl(this, wxDefaultSize, wxBORDER_SUNKEN, c);
		edit_ctrl_tc->Bind(wxEVT_CHAR_HOOK, &SubsEditBox::OnKeyDown, this);
		edit_ctrl_tc->Bind(wxEVT_AUX1_DOWN, &SubsEditBox::OnMouse, this);
		edit_ctrl_tc->Bind(wxEVT_AUX2_DOWN, &SubsEditBox::OnMouse, this);
#ifdef WITH_WXSTC
	}
#endif

	secondary_editor = new SubsTextEditCtrl(this, wxDefaultSize, wxBORDER_SUNKEN | wxTE_MULTILINE | wxTE_READONLY, nullptr);
	secondary_editor->Bind(wxEVT_AUX1_DOWN, &SubsEditBox::OnMouse, this);
	secondary_editor->Bind(wxEVT_AUX2_DOWN, &SubsEditBox::OnMouse, this);
#ifdef WITH_WXSTC
	if (use_stc) {
		// Here we use the height of secondary_editor as the initial size of edit_ctrl_stc,
		// which is more reasonable than the default given by wxWidgets.
		// See: https://trac.wxwidgets.org/ticket/18471#ticket
		//      https://github.com/wangqr/Aegisub/issues/4
		edit_ctrl_stc->SetInitialSize(secondary_editor->GetSize());
	}
#endif

	main_sizer->Add(secondary_editor, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 3));
#ifdef WITH_WXSTC
	if (use_stc) {
		main_sizer->Add(edit_ctrl_stc, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 3));
	}
	else {
#endif
		main_sizer->Add(edit_ctrl_tc, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 3));
#ifdef WITH_WXSTC
	}
#endif
	main_sizer->Hide(secondary_editor);

	bottom_sizer = new wxBoxSizer(wxHORIZONTAL);
	bottom_sizer->Add(MakeBottomButton("edit/revert"), wxSizerFlags().Border(wxRIGHT));
	bottom_sizer->Add(MakeBottomButton("edit/clear"), wxSizerFlags().Border(wxRIGHT));
	bottom_sizer->Add(MakeBottomButton("edit/clear/text"), wxSizerFlags().Border(wxRIGHT));
	bottom_sizer->Add(MakeBottomButton("edit/insert_original"));
	main_sizer->Add(bottom_sizer);
	main_sizer->Hide(bottom_sizer);

	SetSizerAndFit(main_sizer);

#ifdef WITH_WXSTC
	if (use_stc) {
		edit_ctrl_stc->Bind(wxEVT_STC_MODIFIED, &SubsEditBox::OnChangeStc, this);
		edit_ctrl_stc->SetModEventMask(wxSTC_MOD_INSERTTEXT | wxSTC_MOD_DELETETEXT | wxSTC_STARTACTION);
	}
	else {
#endif
		edit_ctrl_tc->Bind(wxEVT_TEXT, &SubsEditBox::OnChangeTc, this);
#ifdef WITH_WXSTC
	}
#endif

	Bind(wxEVT_TEXT, &SubsEditBox::OnLayerEnter, this, layer->GetId());
	Bind(wxEVT_SPINCTRL, &SubsEditBox::OnLayerEnter, this, layer->GetId());
	Bind(wxEVT_CHECKBOX, &SubsEditBox::OnCommentChange, this, comment_box->GetId());

	Bind(wxEVT_CHAR_HOOK, &SubsEditBox::OnKeyDown, this);
	Bind(wxEVT_SIZE, &SubsEditBox::OnSize, this);
	Bind(wxEVT_TIMER,
		[this](wxTimerEvent&) { command_session.ResetCommitId(); },
		undo_timer.GetId());
	Bind(wxEVT_TIMER, [this](wxTimerEvent&) { ConfirmPendingColorClick(); }, color_click_timer.GetId());
	Bind(wxEVT_TIMER,
		&SubsEditBox::OnVisualToolTextSyncTimer,
		this,
		visual_tool_text_sync_timer.GetId());

	wxSizeEvent evt;
	OnSize(evt);

	auto core = context->GetCore();

	file_changed_slot = core.ass->AddCommitListener(&SubsEditBox::OnCommit, this);
	connections = agi::signal::make_vector({
		core.project->AddTimecodesListener(&SubsEditBox::UpdateFrameTiming, this),
		core.selectionController->AddActiveLineListener(&SubsEditBox::OnActiveLineChanged, this),
		core.selectionController->AddSelectionListener(&SubsEditBox::OnSelectedSetChanged, this),
		core.initialLineState->AddChangeListener(&SubsEditBox::OnLineInitialTextChanged, this),
		core.subsController->AddFileOpenListener([this](agi::fs::path const&, bool) { UpdateTimeDisplayModeFromFile(true); }),
		core.subsController->AddFileSaveListener([this] { UpdateTimeDisplayModeFromFile(false); }),
		OPT_SUB("Subtitle/Edit Box/Margin Spin Step", [this](agi::OptionValue const& option) {
			// Keep integer range fixed; only the arrow/key step size changes.
			int const step = static_cast<int>(std::clamp<int64_t>(
				option.GetInt(), 1, AssStyle::MaxMargin));
			for (auto *ctrl : margin)
				ctrl->SetIncrement(step);
		}),
	 });

#ifdef WITH_WXSTC
	if (use_stc) {
		core.textSelectionController->SetControl(edit_ctrl_stc);
		edit_ctrl_stc->SetFocus();
	}
	else {
#endif
		core.textSelectionController->SetControl(edit_ctrl_tc);
		edit_ctrl_tc->SetFocus();
#ifdef WITH_WXSTC
	}
#endif

	bool show_original = OPT_GET("Subtitle/Show Original")->GetBool();
	if (show_original) {
		split_box->SetValue(true);
		DoOnSplit(true);
	}

	UpdateFrameTiming(core.project->Timecodes());
	UpdateTimeDisplayModeFromFile(true);
}

SubsEditBox::~SubsEditBox() {
	CancelVisualToolTextSync();
	c->GetCore().textSelectionController->SetControl((wxTextCtrl*)nullptr);
}

bool SubsEditBox::CanFocusEditControl() const {
#ifdef WITH_WXSTC
	if (use_stc)
		return aegisub::subtitle_edit_box_focus::CanFocusEditControl(
			line != nullptr,
			edit_ctrl_stc && edit_ctrl_stc->IsEnabled(),
			edit_ctrl_stc && edit_ctrl_stc->AcceptsFocusFromKeyboard());
#endif
	return aegisub::subtitle_edit_box_focus::CanFocusEditControl(
		line != nullptr,
		edit_ctrl_tc && edit_ctrl_tc->IsEnabled(),
		edit_ctrl_tc && edit_ctrl_tc->AcceptsFocusFromKeyboard());
}

void SubsEditBox::FocusEditControl() {
#ifdef WITH_WXSTC
	if (use_stc) {
		edit_ctrl_stc->SetFocus();
		return;
	}
#endif
	edit_ctrl_tc->SetFocus();
}

std::string SubsEditBox::GetEditControlSelectedText() const {
#ifdef WITH_WXSTC
	if (use_stc) {
		if (!edit_ctrl_stc)
			return {};

		int sel_start = edit_ctrl_stc->GetSelectionStart();
		int sel_end = edit_ctrl_stc->GetSelectionEnd();
		if (sel_start == sel_end)
			return {};
		if (sel_start > sel_end)
			std::swap(sel_start, sel_end);

		auto data = edit_ctrl_stc->GetTextRaw();
		int text_len = static_cast<int>(data.length());
		sel_start = std::max(0, std::min(sel_start, text_len));
		sel_end = std::max(sel_start, std::min(sel_end, text_len));
		return std::string(data.data() + sel_start, sel_end - sel_start);
	}
#endif

	if (!edit_ctrl_tc)
		return {};

	long sel_start = 0;
	long sel_end = 0;
	edit_ctrl_tc->GetSelection(&sel_start, &sel_end);
	if (sel_start == sel_end)
		return {};
	if (sel_start > sel_end)
		std::swap(sel_start, sel_end);

	return from_wx(edit_ctrl_tc->GetRange(sel_start, sel_end));
}

std::optional<std::pair<int, int>> SubsEditBox::GetEditControlCaret() const {
	if (!CanFocusEditControl())
		return std::nullopt;

	std::string text;
	long sel_start = 0, sel_end = 0;
#ifdef WITH_WXSTC
	if (use_stc) {
		if (!edit_ctrl_stc)
			return std::nullopt;

		auto data = edit_ctrl_stc->GetTextRaw();
		text.assign(data.data(), data.length());
		sel_start = edit_ctrl_stc->GetSelectionStart();
		sel_end = edit_ctrl_stc->GetSelectionEnd();
	}
	else {
#endif
		if (!edit_ctrl_tc)
			return std::nullopt;

		text = from_wx(edit_ctrl_tc->GetValue());
		edit_ctrl_tc->GetSelection(&sel_start, &sel_end);
		// wxTextCtrl selection positions are in wxString units, convert to UTF-8 bytes
		sel_start = static_cast<long>(edit_ctrl_tc->GetRange(0, sel_start).utf8_str().length());
		sel_end = static_cast<long>(edit_ctrl_tc->GetRange(0, sel_end).utf8_str().length());
#ifdef WITH_WXSTC
	}
#endif

	// Normalize so sel_start <= sel_end
	if (sel_start > sel_end)
		std::swap(sel_start, sel_end);

	// Clamp to text bounds
	auto text_len = static_cast<long>(text.size());
	sel_start = std::clamp(sel_start, 0L, text_len);
	sel_end = std::clamp(sel_end, 0L, text_len);

	// Convert byte offsets to 0-based character offsets
	auto char_start = static_cast<int>(agi::CharacterCount(text.begin(), text.begin() + sel_start, 0));
	auto char_end = static_cast<int>(agi::CharacterCount(text.begin(), text.begin() + sel_end, 0));

	return std::make_pair(char_start, char_end);
}

void SubsEditBox::SetEditControlCaret(int character_index, bool after) {
	size_t character_offset = 0;
	if (character_index > 0)
		character_offset = static_cast<size_t>(character_index - 1) + (after ? 1 : 0);

	std::string text;
#ifdef WITH_WXSTC
	if (use_stc) {
		auto data = edit_ctrl_stc->GetTextRaw();
		text.assign(data.data(), data.length());
	}
	else {
#endif
		text = from_wx(edit_ctrl_tc->GetValue());
#ifdef WITH_WXSTC
	}
#endif

	auto const caret_position = static_cast<long>(agi::IndexOfCharacter(text, character_offset));
	auto core = c->GetCore();
	core.textSelectionController->SetSelection(caret_position, caret_position);
	core.textSelectionController->SetInsertionPoint(caret_position);
	FocusEditControl();
}

void SubsEditBox::SetEditControlSelection(int start, int stop) {
	if (start > stop)
		std::swap(start, stop);

	std::string text;
#ifdef WITH_WXSTC
	if (use_stc) {
		auto data = edit_ctrl_stc->GetTextRaw();
		text.assign(data.data(), data.length());
	}
	else {
#endif
	text = from_wx(edit_ctrl_tc->GetValue());
#ifdef WITH_WXSTC
	}
#endif

	auto const byte_start = static_cast<long>(agi::IndexOfCharacter(text, std::max(0, start)));
	auto const byte_stop = static_cast<long>(agi::IndexOfCharacter(text, std::max(0, stop)));
	auto core = c->GetCore();
	core.textSelectionController->SetSelection(byte_start, byte_stop);
	core.textSelectionController->SetInsertionPoint(byte_stop);
	FocusEditControl();
}

wxSpinCtrl *SubsEditBox::MakeMarginCtrl(wxString const& tooltip, int margin, wxString const& commit_msg) {
	// Integer-only control: range stays AssStyle ±10000; step is preferences-driven.
	// Manual entry may still set any integer in range (wxSpinCtrl SetIncrement contract).
	int const step = static_cast<int>(std::clamp<int64_t>(
		OPT_GET("Subtitle/Edit Box/Margin Spin Step")->GetInt(), 1, AssStyle::MaxMargin));
	wxSpinCtrl *ctrl = new wxSpinCtrl(this, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize,
		wxSP_ARROW_KEYS | wxTE_PROCESS_ENTER | wxTE_CENTER,
		AssStyle::MinMargin, AssStyle::MaxMargin, 0);
	ctrl->SetIncrement(step);
#ifdef __WXGTK3__
	// GTK3 has a bug that we cannot shrink the size of a widget, so do nothing here.
#elif wxCHECK_VERSION(3, 1, 3)
	ctrl->SetInitialSize(ctrl->GetSizeFromText(wxS("00000")));
#else
	ctrl->SetInitialSize(ctrl->GetSizeFromTextSize(GetTextExtent(wxS("00000"))));
#endif
	ctrl->SetToolTip(tooltip);
	middle_left_sizer->Add(ctrl, wxSizerFlags().Expand());

	auto commit = [=](wxCommandEvent&) {
		auto text = ctrl->GetTextValue();
		text.Trim(true);
		text.Trim(false);
		auto const input = aegisub::subtitle_edit_box_input::ResolveMarginInput(
			text.empty(), [=] { return ctrl->GetValue(); });
		if (input.normalize_control)
			ctrl->SetValue(input.value);

		SetSelectedRows([&](AssDialogue *d) { d->Margin[margin] = input.value; },
			commit_msg, AssFile::COMMIT_DIAG_META);
	};
	Bind(wxEVT_TEXT, commit, ctrl->GetId());
	Bind(wxEVT_SPINCTRL, commit, ctrl->GetId());
	// wxSpinCtrl is not a wxTextEntry, so the Default-context edit/line/*
	// hotkeys would cut/paste subtitle lines instead of the field text.
	ctrl->Bind(wxEVT_CHAR_HOOK, [](wxKeyEvent& event) {
		TextControlClipboardCharHook(event, true);
	});

	return ctrl;
}

TimeEdit *SubsEditBox::MakeTimeCtrl(wxString const& tooltip, TimeField field) {
	TimeEdit *ctrl = new TimeEdit(this, -1, c, std::string(), wxDefaultSize, field == TIME_END);
#if wxCHECK_VERSION(3, 1, 3)
	ctrl->SetInitialSize(ctrl->GetSizeFromText(wxS("0:00:00.000")));
#else
	ctrl->SetInitialSize(ctrl->GetSizeFromTextSize(GetTextExtent(wxS("0:00:00.000"))));
#endif
	ctrl->SetToolTip(tooltip);
	ctrl->SetDisplayMode(time_display_mode);
	Bind(wxEVT_TEXT, [=](wxCommandEvent&) {
		if (ctrl->ConsumeInputChanged())
			CommitTimes(field);
	}, ctrl->GetId());
	ctrl->Bind(wxEVT_CHAR_HOOK, time_edit_char_hook);
	middle_left_sizer->Add(ctrl, wxSizerFlags().Expand());
	return ctrl;
}

void SubsEditBox::MakeButton(const char *cmd_name) {
	cmd::Command *command = cmd::get(cmd_name);
#ifdef __WXMSW__
	wxBitmapButton* btn = new wxBitmapButton(this, -1, wxNullBitmap);
	btn->SetBitmap(command->IconBundle(GetLayoutDirection()));
#else
	wxBitmapButton *btn = new wxBitmapButton(this, -1, command->Icon(OPT_GET("App/Toolbar Icon Size")->GetInt()));
#endif
	btn->SetLabel(command->StrDisplay(c));
	btn->SetName(command->StrDisplay(c));
	ToolTipManager::Bind(btn, command->StrHelp(), "Subtitle Edit Box", cmd_name);

	middle_right_sizer->Add(btn, wxSizerFlags().Expand());
	btn->Bind(wxEVT_BUTTON, [this, cmd_name](wxCommandEvent&) { CallCommand(cmd_name); });
}

void SubsEditBox::MakeColorButton(const char *open_cmd_name, const char *pick_cmd_name) {
	cmd::Command *command = cmd::get(open_cmd_name);
#ifdef __WXMSW__
	wxBitmapButton *btn = new wxBitmapButton(this, -1, wxNullBitmap);
	btn->SetBitmap(command->IconBundle(GetLayoutDirection()));
#else
	wxBitmapButton *btn = new wxBitmapButton(this, -1, command->Icon(OPT_GET("App/Toolbar Icon Size")->GetInt()));
#endif
	btn->SetLabel(command->StrDisplay(c));
	btn->SetName(command->StrDisplay(c));
	ToolTipManager::Bind(btn, command->StrHelp(), "Subtitle Edit Box", open_cmd_name);
	btn->SetToolTip(btn->GetToolTipText() + to_wx("\n") + _("Double-click to pick this color straight from the video."));

	middle_right_sizer->Add(btn, wxSizerFlags().Expand());

	// The native button still reports every completed press/release as a
	// click; single clicks stay deferred until the double-click window closes
	// (see ConfirmPendingColorClick), and the port-paired second press
	// reroutes to the pick-from-video command instead. Gesture resolution
	// lives in subtitle_edit_box_color_click.h: only wxEVT_LEFT_DCLICK ever
	// reroutes. wxMSW delivers one physical double-click press to both
	// handlers — wxAnyButton::MSWWindowProc synthesizes a WM_LBUTTONDOWN for
	// the WM_LBUTTONDBLCLK before processing the real double click — so the
	// plain-down binding must stay pass-through; rerouting there would also
	// hijack two ordinary clicks whose second press landed past the system
	// double-click rectangle (ports report that as a plain down).
	btn->Bind(wxEVT_BUTTON, [this, open_cmd_name, pick_cmd_name](wxCommandEvent&) {
		if (color_click_sequencer.OnButtonClick(pick_cmd_name) !=
			aegisub::subtitle_edit_box_color_click::Action::DeferOpen)
			return;
		pending_color_open_command = open_cmd_name;
		color_click_timer.Start(ColorDoubleClickIntervalMs(), wxTIMER_ONE_SHOT);
	});
	btn->Bind(wxEVT_LEFT_DCLICK, [this, pick_cmd_name](wxMouseEvent& event) {
		if (color_click_sequencer.OnDoublePress(pick_cmd_name) ==
			aegisub::subtitle_edit_box_color_click::Action::RunPick) {
			// Swallow the whole press: passing it on would run the native
			// default handling (the BUTTON class treats the double click as
			// another press), which pushes the button again and pulls focus
			// back from the video display — the only control that answers the
			// Escape-cancel hint of the pick session.
			RunPendingColorQuickPick(pick_cmd_name);
			return;
		}
		// Not our double click (e.g. the third press of a triple click); on
		// Windows the press already ran the plain-down handler, which does
		// the stale-swallow cleanup for it.
		event.Skip();
	});
	btn->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
		color_click_sequencer.OnPlainPress();
		event.Skip();
	});
}

void SubsEditBox::RunPendingColorQuickPick(const char *pick_cmd_name) {
	pending_color_open_command = nullptr;
	color_click_timer.Stop();
	// Ports that hand the taken-over press back to the native button still
	// report one more click on release; the sequencer's swallow debt covers
	// it, and a debt left by a press released outside its button is retired
	// by the next plain press.
	// cmd::call drops commands failing Validate before their operator could
	// explain, but a double-click with no video open owes the user the hint,
	// so invoke the operator directly, the way hotkey dispatch does.
	cmd::Command *command = cmd::get(pick_cmd_name);
	auto sink = c->GetStatusSink();
	if (sink)
		sink->SetLastCommand(from_wx(command->StrDisplay(c)));
	(*command)(c);
}

void SubsEditBox::ConfirmPendingColorClick() {
	if (color_click_sequencer.OnWindowExpired() !=
		aegisub::subtitle_edit_box_color_click::Action::OpenDialog)
		return;
	auto const open_cmd = pending_color_open_command;
	pending_color_open_command = nullptr;
	if (open_cmd)
		CallCommand(open_cmd);
}

wxButton *SubsEditBox::MakeBottomButton(const char *cmd_name) {
	cmd::Command *command = cmd::get(cmd_name);
	wxButton *btn = new wxButton(this, -1, command->StrDisplay(c));
	ToolTipManager::Bind(btn, command->StrHelp(), "Subtitle Edit Box", cmd_name);

	btn->Bind(wxEVT_BUTTON, [this, cmd_name](wxCommandEvent&) { CallCommand(cmd_name); });
	return btn;
}

SubsEditStyleComboBox *SubsEditBox::MakeStyleComboBox(wxString const& initial_text, void (SubsEditBox::*handler)(wxCommandEvent&), wxString const& tooltip) {
	wxString styles[] = { wxS("Default") };
#ifdef __WXMSW__
	auto *ctrl = new TracedSubsEditStyleComboBox(
		this,
		-1,
		initial_text,
		wxDefaultPosition,
		wxDefaultSize,
		1,
		styles,
		wxCB_READONLY | wxODCB_STD_CONTROL_PAINT);
#else
	auto *ctrl = new wxComboBox(
		this,
		-1,
		initial_text,
		wxDefaultPosition,
		wxDefaultSize,
		1,
		styles,
		wxCB_READONLY | wxTE_PROCESS_ENTER);
#endif
	ctrl->SetToolTip(tooltip);
	top_sizer->Add(ctrl, wxSizerFlags(2).Expand().Border(wxRIGHT));
	Bind(wxEVT_COMBOBOX, handler, this, ctrl->GetId());
	return ctrl;
}

wxRadioButton *SubsEditBox::MakeRadio(wxString const& text, bool start, wxString const& tooltip) {
	wxRadioButton *ctrl = new wxRadioButton(this, -1, text, wxDefaultPosition, wxDefaultSize, start ? wxRB_GROUP : 0);
	ctrl->SetValue(start);
	ctrl->SetToolTip(tooltip);
	Bind(wxEVT_RADIOBUTTON, &SubsEditBox::OnFrameTimeRadio, this, ctrl->GetId());
	middle_right_sizer->Add(ctrl, wxSizerFlags().Expand().Border(wxRIGHT));
	return ctrl;
}

bool SubsEditBox::IsVisualToolInteracting() const {
	auto *display = c->GetUI().videoDisplay;
	return display && display->IsVisualToolInteracting();
}

void SubsEditBox::QueueVisualToolTextSync() {
	visual_tool_text_sync_pending = true;
	auto const text_bytes = line
		? static_cast<int>(std::min(
			line->Text.get().size(),
			static_cast<size_t>(std::numeric_limits<int>::max())))
		: 0;
	perf_trace::ObserveVideoUiDuration(
		"grid_select.editbox.text.deferred",
		0.0,
		text_bytes);
	if (!visual_tool_text_sync_timer.IsRunning())
		visual_tool_text_sync_timer.StartOnce(VISUAL_TOOL_TEXT_SYNC_INTERVAL_MS);
}

void SubsEditBox::CancelVisualToolTextSync() {
	visual_tool_text_sync_timer.Stop();
	visual_tool_text_sync_pending = false;
}

void SubsEditBox::FlushVisualToolTextSync() {
	if (!visual_tool_text_sync_pending)
		return;

	CancelVisualToolTextSync();
	if (!line)
		return;

	auto const text_bytes = static_cast<int>(std::min(
		line->Text.get().size(),
		static_cast<size_t>(std::numeric_limits<int>::max())));
	perf_trace::VideoUiDurationScope trace(
		"grid_select.editbox.text.coalesced_flush",
		text_bytes,
		IsVisualToolInteracting() ? 1 : 0);
	wxEventBlocker blocker(this);
	UpdateFields(AssFile::COMMIT_DIAG_TEXT, false);
}

void SubsEditBox::OnVisualToolTextSyncTimer(wxTimerEvent&) {
	FlushVisualToolTextSync();
}

void SubsEditBox::OnCommit(int type, AssDialogue const* changed) {
	bool const local_commit = command_session.IsLocalCommitInProgress();
	if (local_commit && !command_session.ShouldObserveLocalCommit())
		return;

	auto core = c->GetCore();
	initial_times.clear();
	bool const affects_current_line =
		type == AssFile::COMMIT_NEW
		|| (type & (AssFile::COMMIT_STYLES | AssFile::COMMIT_ORDER | AssFile::COMMIT_DIAG_ADDREM))
		|| (!changed && !!line)
		|| changed == line;
	bool const defer_text_sync =
		affects_current_line
		&& (type & AssFile::COMMIT_DIAG_TEXT)
		&& IsVisualToolInteracting();
	if (visual_tool_text_sync_pending && !defer_text_sync) {
		if (type == AssFile::COMMIT_DIAG_TEXT)
			FlushVisualToolTextSync();
		else
			CancelVisualToolTextSync();
	}

	if (type == AssFile::COMMIT_NEW || type & AssFile::COMMIT_STYLES) {
		wxEventBlocker blocker(this);
		wxString style = style_box->GetValue();
		style_box->Clear();
		style_box->Append(to_wx(core.ass->GetStyles()));
		style_box->Select(style_box->FindString(style));
		active_style = line ? core.ass->GetStyle(line->Style) : nullptr;
	}

	if (type == AssFile::COMMIT_NEW) {
		wxEventBlocker blocker(this);
		PopulateList(effect_box, AssDialogue_Effect);
		PopulateList(actor_box, AssDialogue_Actor);
		return;
	}
	else if ((type & AssFile::COMMIT_STYLES) && line)
	{
		auto const style_index = style_box->FindString(to_wx(line->Style));
		if (style_box->GetSelection() != style_index)
			style_box->Select(style_index);
	}

	if (!(type ^ AssFile::COMMIT_ORDER) || !affects_current_line)
		return;

	int update_type = type;
	if (defer_text_sync) {
		QueueVisualToolTextSync();
		update_type &= ~AssFile::COMMIT_DIAG_TEXT;
		if (!update_type)
			return;
	}

	wxEventBlocker blocker(this);
	SetControlsState(!!line);
	UpdateFields(update_type, true);
}

void SubsEditBox::UpdateFields(int type, bool repopulate_lists) {
	UpdateCodeMode();
	if (!line) return;

	if (type & AssFile::COMMIT_DIAG_TIME) {
		perf_trace::VideoUiDurationScope trace("grid_select.editbox.time");
		// Linked-time ASS projection is no longer needed; keep calls commented
		// as documentation of the historical paired-control relationship.
		// start_time->SetLinkedTime(line->End);
		// end_time->SetLinkedTime(line->Start);
		start_time->SetTime(line->Start);
		end_time->SetTime(line->End);
		SetDurationField();
	}

	if (type & AssFile::COMMIT_DIAG_TEXT) {
		auto const text_bytes = static_cast<int>(std::min(
			line->Text.get().size(),
			static_cast<size_t>(std::numeric_limits<int>::max())));
#ifdef WITH_WXSTC
		int const styled_text_control = use_stc ? 1 : 0;
#else
		int const styled_text_control = 0;
#endif
		perf_trace::VideoUiDurationScope text_trace(
			"grid_select.editbox.text",
			text_bytes,
			styled_text_control);

		{
			perf_trace::VideoUiDurationScope set_trace(
				"grid_select.editbox.text.set",
				text_bytes,
				styled_text_control);
#ifdef WITH_WXSTC
			if (use_stc) {
				edit_ctrl_stc->SetTextTo(line->Text);
			}
			else
#endif
			{
				edit_ctrl_tc->SetValue(to_wx(line->Text));
			}
		}
		{
			perf_trace::VideoUiDurationScope count_trace(
				"grid_select.editbox.text.count",
				text_bytes);
			UpdateCharacterCount(line->Text);
		}
	}

	if (type & AssFile::COMMIT_DIAG_META) {
		perf_trace::VideoUiDurationScope trace("grid_select.editbox.meta");
		auto core = c->GetCore();
		{
			perf_trace::VideoUiDurationScope meta_trace("grid_select.editbox.meta.layer_margin");
			layer->SetValue(line->Layer);
			for (size_t i = 0; i < margin.size(); ++i)
				margin[i]->SetValue(line->Margin[i]);
		}
		{
			perf_trace::VideoUiDurationScope meta_trace("grid_select.editbox.meta.comment");
			comment_box->SetValue(line->Comment);
		}
		{
			perf_trace::VideoUiDurationScope style_trace(
				"grid_select.editbox.meta.style",
				style_box->GetCount());
			int style_index = -1;
			int const current_style_index = style_box->GetSelection();
			{
				perf_trace::VideoUiDurationScope find_trace(
					"grid_select.editbox.meta.style.find",
					style_box->GetCount(),
					current_style_index);
				style_index = style_box->FindString(to_wx(line->Style));
				find_trace.SetDetails(style_box->GetCount(), style_index);
			}
			{
				perf_trace::VideoUiDurationScope select_trace(
					"grid_select.editbox.meta.style.select",
					current_style_index,
					style_index);
				wxString const target_style = style_index == wxNOT_FOUND
					? wxString{}
					: style_box->GetString(style_index);
				if (current_style_index != style_index || style_box->GetValue() != target_style)
					style_box->Select(style_index);
			}
			{
				perf_trace::VideoUiDurationScope resolve_trace(
					"grid_select.editbox.meta.style.resolve");
				active_style = line ? core.ass->GetStyle(line->Style) : nullptr;
			}
			{
				perf_trace::VideoUiDurationScope button_trace(
					"grid_select.editbox.meta.style.button",
					active_style != nullptr ? 1 : 0);
				style_edit_button->Enable(active_style != nullptr);
			}
		}

		if (repopulate_lists) {
			perf_trace::VideoUiDurationScope populate_trace(
				"grid_select.editbox.meta.populate.effect",
				static_cast<int>(core.ass->Events.size()));
			PopulateList(effect_box, AssDialogue_Effect);
		}
		{
			perf_trace::VideoUiDurationScope meta_trace("grid_select.editbox.meta.effect", effect_box->GetCount());
			effect_box->ChangeValue(to_wx(line->Effect));
			effect_box->SetStringSelection(to_wx(line->Effect));
		}

		if (repopulate_lists) {
			perf_trace::VideoUiDurationScope populate_trace(
				"grid_select.editbox.meta.populate.actor",
				static_cast<int>(core.ass->Events.size()));
			PopulateList(actor_box, AssDialogue_Actor);
		}
		{
			perf_trace::VideoUiDurationScope meta_trace("grid_select.editbox.meta.actor", actor_box->GetCount());
			actor_box->ChangeValue(to_wx(line->Actor));
			actor_box->SetStringSelection(to_wx(line->Actor));
		}
	}
}

void SubsEditBox::UpdateCodeMode() {
	bool const enabled = line != nullptr && Automation4::IsKaraokeCodeLine(line->Comment, line->Effect.get());
	if (code_mode == enabled) {
		return;
	}
	code_mode = enabled;
#ifdef WITH_WXSTC
	if (use_stc) {
		edit_ctrl_stc->SetCodeMode(enabled);
	}
	else {
#endif
		static_cast<SubsTextEditCtrl *>(edit_ctrl_tc)->SetCodeMode(enabled);
#ifdef WITH_WXSTC
	}
	middle_right_sizer->Show(lua_workspace_button, enabled);
	wxSizeEvent size_event;
	OnSize(size_event);
	Layout();
#endif
}

void SubsEditBox::PopulateList(wxComboBox *combo, boost::flyweight<std::string> AssDialogue::*field) {
	auto core = c->GetCore();
	wxEventBlocker blocker(this);

	std::unordered_set<boost::flyweight<std::string>> values;
	for (auto const& line : core.ass->Events) {
		auto const& value = line.*field;
		if (!value.get().empty())
			values.insert(value);
	}

	wxArrayString arrstr;
	arrstr.reserve(values.size());
	transform(values.begin(), values.end(), std::back_inserter(arrstr),
		(wxString (*)(std::string const&))to_wx);

	arrstr.Sort();

	combo->Freeze();
	long pos = combo->GetInsertionPoint();
	wxString value = combo->GetValue();

	combo->Set(arrstr);
	combo->ChangeValue(value);
	combo->SetStringSelection(value);
	combo->SetInsertionPoint(pos);
	combo->Thaw();
}

void SubsEditBox::OnActiveLineChanged(AssDialogue *new_line) {
	perf_trace::VideoUiDurationScope trace("grid_select.editbox");
	wxEventBlocker blocker(this);
	CancelVisualToolTextSync();
	line = new_line;
	command_session.ResetCommitId();

	UpdateFields(AssFile::COMMIT_DIAG_FULL, false);
}

void SubsEditBox::OnSelectedSetChanged() {
	initial_times.clear();
}

void SubsEditBox::OnLineInitialTextChanged(std::string const& new_text) {
	if (split_box->IsChecked())
		secondary_editor->SetValue(to_wx(new_text));
}

void SubsEditBox::UpdateFrameTiming(agi::vfr::Framerate const& fps) {
	if (fps.IsLoaded()) {
		by_frame->Enable(true);
	}
	else {
		by_frame->Enable(false);
		if (time_display_mode == SubtitleTimeDisplayMode::Frame)
			ApplyTimeDisplayMode(non_frame_display_mode);
	}
}

void SubsEditBox::ApplyTimeDisplayMode(SubtitleTimeDisplayMode mode, bool update_radio_buttons) {
	if (mode == SubtitleTimeDisplayMode::Frame && !c->GetCore().project->Timecodes().IsLoaded())
		mode = non_frame_display_mode;

	if (mode != SubtitleTimeDisplayMode::Frame)
		non_frame_display_mode = mode;

	time_display_mode = mode;

	if (update_radio_buttons) {
		by_ass->SetValue(mode == SubtitleTimeDisplayMode::Ass);
		by_exact->SetValue(mode == SubtitleTimeDisplayMode::Exact);
		by_frame->SetValue(mode == SubtitleTimeDisplayMode::Frame);
	}

	start_time->SetDisplayMode(mode);
	end_time->SetDisplayMode(mode);
	duration->SetDisplayMode(mode);
	c->GetUI().subsGrid->SetDisplayMode(mode);

	if (line) {
		// Linked-time ASS projection is no longer needed.
		// start_time->SetLinkedTime(line->End);
		// end_time->SetLinkedTime(line->Start);
		start_time->SetTime(line->Start);
		end_time->SetTime(line->End);
		SetDurationField();
	}
	else {
		// start_time->ClearLinkedTime();
		// end_time->ClearLinkedTime();
	}
}

void SubsEditBox::UpdateTimeDisplayModeFromFile(bool force_apply) {
	auto core = c->GetCore();
	auto const filename = core.subsController->HasFile()
		? core.subsController->Filename()
		: agi::fs::path();
	auto const default_mode = DefaultTimeDisplayModeForFile(filename);

	bool const follow_current_default = time_display_mode == file_default_display_mode;
	bool const follow_frame_default = time_display_mode == SubtitleTimeDisplayMode::Frame
		&& non_frame_display_mode == file_default_display_mode;
	file_default_display_mode = default_mode;

	if (time_display_mode == SubtitleTimeDisplayMode::Frame) {
		if (force_apply || follow_frame_default)
			non_frame_display_mode = default_mode;
		return;
	}

	if (force_apply || follow_current_default)
		ApplyTimeDisplayMode(default_mode);
}

void SubsEditBox::OnKeyDown(wxKeyEvent &event) {
#ifdef WITH_WXSTC
	wxWindow const *editor = use_stc ? static_cast<wxWindow *>(edit_ctrl_stc) : edit_ctrl_tc;
#else
	wxWindow const *editor = edit_ctrl_tc;
#endif
	if (code_mode && event.GetEventObject() == editor) {
		if (event.GetKeyCode() == WXK_RETURN && event.GetModifiers() == wxMOD_SHIFT) {
#ifdef WITH_WXSTC
			CallCommand("automation/lua/open-current-line", false);
#else
			c->ShowError("Lua Workspace is unavailable in this build.", "Lua Workspace");
#endif
			return;
		}
#ifdef WITH_WXSTC
		if (use_stc && (event.GetKeyCode() == WXK_TAB || event.GetKeyCode() == WXK_HOME || event.GetKeyCode() == WXK_END || ((event.GetKeyCode() == WXK_LEFT || event.GetKeyCode() == WXK_RIGHT) && event.AltDown()))) {
			event.Skip();
			return;
		}
#endif
	}
	hotkey::check("Subtitle Edit Box", c, event);
}

void SubsEditBox::OnMouse(wxMouseEvent &event) {
	hotkey::check("Subtitle Edit Box", c, event);
}

#ifdef WITH_WXSTC
void SubsEditBox::OnChangeStc(wxStyledTextEvent &event) {
	if (line && edit_ctrl_stc->GetTextRaw().data() != line->Text.get()) {
		if (event.GetModificationType() & wxSTC_STARTACTION)
			command_session.ResetCommitId();
		CommitText(_("modify text"));
		UpdateCharacterCount(line->Text);
	}
}
#endif

void SubsEditBox::OnChangeTc(wxCommandEvent& event) {
	if (line && from_wx(edit_ctrl_tc->GetValue()) != line->Text.get()) {
		CommitText(_("modify text"));
		UpdateCharacterCount(line->Text);
	}
}

template<class setter>
void SubsEditBox::SetSelectedRows(setter set, wxString const& desc, int type, bool amend) {
	auto const& sel = c->GetCore().selectionController->GetSelectedSet();
	int const amend_id = amend && desc == last_commit_type ? command_session.GetCommitId() : -1;
	command_session.Run(from_wx(desc), type, amend_id, sel.size() == 1 ? *sel.begin() : nullptr, [&] {
		for_each(sel.begin(), sel.end(), set);
	});
	last_commit_type = desc;
	last_time_commit_type = -1;
	initial_times.clear();
	undo_timer.Start(30000, wxTIMER_ONE_SHOT);
}

template<class T>
void SubsEditBox::SetSelectedRows(T AssDialogueBase::*field, T value, wxString const& desc, int type, bool amend) {
	SetSelectedRows([&](AssDialogue *d) { d->*field = value; }, desc, type, amend);
}

template<class T>
void SubsEditBox::SetSelectedRows(T AssDialogueBase::*field, wxString const& value, wxString const& desc, int type, bool amend) {
	boost::flyweight<std::string> conv_value(from_wx(value));
	SetSelectedRows([&](AssDialogue *d) { d->*field = conv_value; }, desc, type, amend);
}

void SubsEditBox::CommitText(wxString const& desc) {
#ifdef WITH_WXSTC
	if (use_stc) {
		auto data = edit_ctrl_stc->GetTextRaw();
		SetSelectedRows(&AssDialogue::Text, boost::flyweight<std::string>(data.data(), data.length()), desc, AssFile::COMMIT_DIAG_TEXT, true);
	}
	else {
#endif
		SetSelectedRows(&AssDialogue::Text, boost::flyweight<std::string>(edit_ctrl_tc->GetValue().utf8_str()), desc, AssFile::COMMIT_DIAG_TEXT, true);
#ifdef WITH_WXSTC
	}
#endif
}

void SubsEditBox::CommitTimes(TimeField field) {
	auto core = c->GetCore();
	auto const& sel = core.selectionController->GetSelectedSet();
	if (field != last_time_commit_type)
		command_session.ResetCommitId();

	last_time_commit_type = field;
	command_session.Run(
		from_wx(_("modify times")),
		AssFile::COMMIT_DIAG_TIME,
		command_session.GetCommitId(),
		sel.size() == 1 ? *sel.begin() : nullptr,
		[&] {
			for (AssDialogue *d : sel) {
				if (!initial_times.count(d))
					initial_times[d] = {d->Start, d->End};

				switch (field) {
					case TIME_START:
						initial_times[d].first = d->Start = start_time->GetTime();
						d->End = std::max(d->Start, initial_times[d].second);
						break;

					case TIME_END:
						initial_times[d].second = d->End = end_time->GetTime();
						d->Start = std::min(d->End, initial_times[d].first);
						break;

					case TIME_DURATION:
						if (time_display_mode == SubtitleTimeDisplayMode::Frame) {
							auto const& fps = core.project->Timecodes();
							d->End = fps.TimeAtFrame(fps.FrameAtTime(d->Start, agi::vfr::START) + duration->GetFrame() - 1, agi::vfr::END);
						}
						else {
							d->End = GetEndTimeForDisplayedDuration(
								d->Start,
								d->End,
								duration->GetTime(),
								time_display_mode,
								&core.project->Timecodes());
						}
						initial_times[d].second = d->End;
						break;
				}
			}
		});

	start_time->SetTime(line->Start);
	end_time->SetTime(line->End);
	// Linked-time ASS projection is no longer needed.
	// start_time->SetLinkedTime(line->End);
	// end_time->SetLinkedTime(line->Start);

	if (field != TIME_DURATION)
		SetDurationField();
}

void SubsEditBox::SetDurationField() {
	if (!line)
		return;

	// With VFR, the frame count calculated from the duration in time can be
	// completely wrong (since the duration is calculated as if it were a start
	// time), so we need to explicitly set it with the correct units.
	if (time_display_mode == SubtitleTimeDisplayMode::Frame)
		duration->SetFrame(end_time->GetFrame() - start_time->GetFrame() + 1);
	else
		// Duration is already derived from the whole dialogue interval, so unlike
		// start/end it does not need linked-boundary ASS projection inside TimeEdit.
		duration->SetTime(agi::Time(GetDurationForDisplay(line->Start, line->End, time_display_mode, &c->GetCore().project->Timecodes())));
}

void SubsEditBox::OnSize(wxSizeEvent &evt) {
	perf_trace::VideoUiDurationScope trace("subs_edit_box.resize.layout");
	int availableWidth = GetVirtualSize().GetWidth();
	int midMin = middle_left_sizer->GetMinSize().GetWidth();
	int botMin = middle_right_sizer->GetMinSize().GetWidth();

	if (button_bar_split) {
		if (availableWidth > midMin + botMin) {
			GetSizer()->Detach(middle_right_sizer);
			middle_left_sizer->Add(middle_right_sizer,0,wxALIGN_CENTER_VERTICAL);
			button_bar_split = false;
		}
	}
	else {
		if (availableWidth < midMin) {
			middle_left_sizer->Detach(middle_right_sizer);
			GetSizer()->Insert(2,middle_right_sizer,0,wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM,3);
			button_bar_split = true;
		}
	}

	evt.Skip();
}

#ifdef __WXMSW__
WXLRESULT SubsEditBox::MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) {
	if (message == WM_SIZE) {
		perf_trace::VideoUiDurationScope trace("subs_edit_box.resize.native");
		return wxPanel::MSWWindowProc(message, wParam, lParam);
	}
	return wxPanel::MSWWindowProc(message, wParam, lParam);
}
#endif

void SubsEditBox::OnFrameTimeRadio(wxCommandEvent &event) {
	event.Skip();

	SubtitleTimeDisplayMode mode = SubtitleTimeDisplayMode::Ass;
	if (by_frame->GetValue())
		mode = SubtitleTimeDisplayMode::Frame;
	else if (by_exact->GetValue())
		mode = SubtitleTimeDisplayMode::Exact;
	ApplyTimeDisplayMode(mode, false);
}

void SubsEditBox::SetControlsState(bool state) {
	if (state == controls_enabled) return;
	controls_enabled = state;

	Enable(state);
	if (!state) {
		wxEventBlocker blocker(this);
		// start_time->ClearLinkedTime();
		// end_time->ClearLinkedTime();
#ifdef WITH_WXSTC
		if (use_stc) {
			edit_ctrl_stc->SetTextTo(std::string());
		}
		else {
#endif
			edit_ctrl_tc->Clear();
#ifdef WITH_WXSTC
		}
#endif
	}
}

void SubsEditBox::OnSplit(wxCommandEvent&) {
	bool show_original = split_box->IsChecked();
	DoOnSplit(show_original);
	OPT_SET("Subtitle/Show Original")->SetBool(show_original);
}

void SubsEditBox::DoOnSplit(bool show_original) {
	Freeze();
	GetSizer()->Show(secondary_editor, show_original);
	GetSizer()->Show(bottom_sizer, show_original);
	Fit();
	SetMinSize(GetSize());
	wxSizer* parent_sizer = GetParent()->GetSizer();
	if (parent_sizer) parent_sizer->Layout();
	Thaw();

	if (show_original)
		secondary_editor->SetValue(to_wx(c->GetCore().initialLineState->GetInitialText()));
}

void SubsEditBox::OnStyleChange(wxCommandEvent &evt) {
	SetSelectedRows(&AssDialogue::Style, new_value(style_box, evt), _("style change"), AssFile::COMMIT_DIAG_META);
	active_style = c->GetCore().ass->GetStyle(line->Style);
}

void SubsEditBox::OnActorChange(wxCommandEvent &evt) {
	bool amend = evt.GetEventType() == wxEVT_TEXT;
	SetSelectedRows(AssDialogue_Actor, new_value(actor_box, evt), _("actor change"), AssFile::COMMIT_DIAG_META, amend);
	PopulateList(actor_box, AssDialogue_Actor);
}

void SubsEditBox::OnLayerEnter(wxCommandEvent &evt) {
	SetSelectedRows(&AssDialogue::Layer, evt.GetInt(), _("layer change"), AssFile::COMMIT_DIAG_META);
}

void SubsEditBox::OnEffectChange(wxCommandEvent &evt) {
	bool amend = evt.GetEventType() == wxEVT_TEXT;
	SetSelectedRows(AssDialogue_Effect, new_value(effect_box, evt), _("effect change"), AssFile::COMMIT_DIAG_META, amend);
	UpdateCodeMode();
	PopulateList(effect_box, AssDialogue_Effect);
}

void SubsEditBox::OnCommentChange(wxCommandEvent &evt) {
	SetSelectedRows(&AssDialogue::Comment, !!evt.GetInt(), _("comment change"), AssFile::COMMIT_DIAG_META);
	UpdateCodeMode();
}

void SubsEditBox::CallCommand(const char *cmd_name, bool refocus_edit_control) {
	cmd::call(cmd_name, c);
	if (!refocus_edit_control)
		return;
#ifdef WITH_WXSTC
	if (use_stc) {
		edit_ctrl_stc->SetFocus();
	}
	else {
#endif
		edit_ctrl_tc->SetFocus();
#ifdef WITH_WXSTC
	}
#endif
}

void SubsEditBox::UpdateCharacterCount(std::string const& text) {
	int ignore = agi::IGNORE_BLOCKS;
	if (OPT_GET("Subtitle/Character Counter/Ignore Whitespace")->GetBool())
		ignore |= agi::IGNORE_WHITESPACE;
	if (OPT_GET("Subtitle/Character Counter/Ignore Punctuation")->GetBool())
		ignore |= agi::IGNORE_PUNCTUATION;
	size_t length = agi::MaxLineLength(text, ignore);
	char_count->SetValue(std::to_wstring(length));
	size_t limit = (size_t)OPT_GET("Subtitle/Character Limit")->GetInt();
	if (limit && length > limit)
		char_count->SetBackgroundColour(to_wx(OPT_GET("Colour/Subtitle/Syntax/Background/Error")->GetColor()));
	else
		char_count->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
}
