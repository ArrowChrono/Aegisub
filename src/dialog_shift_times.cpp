// Copyright (c) 2011, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "dialog_manager.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "help_button.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "shift_times_input.h"
#include "shift_times_history.h"
#include "subs_controller.h"
#include "timeedit_ctrl.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>
#include <libaegisub/signal.h>
#include <libaegisub/vfr.h>

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>

#include <filesystem>
#include <limits>
#include <stdexcept>
#include <wx/dialog.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/radiobox.h>
#include <wx/radiobut.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>

namespace {
class DialogShiftTimes final : public wxDialog {
	agi::Context *context;

	agi::fs::path history_filename;
	aegisub::ShiftHistory history;
	bool history_writable = true;
	agi::vfr::Framerate fps;
	agi::signal::Connection timecodes_loaded_slot;
	agi::signal::Connection selected_set_changed_slot;

	TimeEdit *shift_time;
	wxTextCtrl *shift_frames;
	wxRadioButton *shift_by_time;
	wxRadioButton *shift_by_frames;
	wxRadioButton *shift_forward;
	wxRadioButton *shift_backward;
	wxRadioBox *selection_mode;
	wxRadioBox *time_fields;
	wxListBox *history_box;

	void SaveHistory(std::vector<std::pair<int, int>> shifted_blocks, int amount);
	void LoadHistory();
	void Process(wxCommandEvent&);
	int Shift(int initial_time, int shift, bool by_time, agi::vfr::Time type);

	void OnClear(wxCommandEvent&);
	void OnByTime(wxCommandEvent&);
	void OnByFrames(wxCommandEvent&);
	void RestoreHistory(size_t entry);

	void OnSelectedSetChanged();
	void OnTimecodesLoaded(agi::vfr::Framerate const& new_fps);

public:
DialogShiftTimes(agi::Context *context);
~DialogShiftTimes();
};

static wxString get_history_string(aegisub::ShiftHistoryEntry const& entry) {
	wxString filename = to_wx(entry.filename);
	if (filename.empty())
		filename = _("unsaved");
	wxString amount = to_wx(aegisub::FormatShiftHistoryAmount(entry));
	if (!entry.by_time)
		amount = fmt_tl("%s frames", amount);
	wxString direction = entry.backward ? _("backward") : _("forward");
	wxString fields = entry.fields == 0 ? _("s+e") : entry.fields == 1 ? _("s")
																	   : _("e");
	wxString lines;
	if (entry.mode == 0)
		lines = _("all");
	else if (entry.mode == 2) {
		if (!entry.selected_blocks.empty())
			lines = fmt_tl("from %d onward", entry.selected_blocks.front().first);
	}
	else {
		lines = _("sel ");
		for (auto const& [start, end] : entry.selected_blocks) {
			if (lines != _("sel "))
				lines += wxS(";");
			lines += start == end ? std::to_wstring(start) : fmt_wx("%d-%d", start, end);
		}
	}
	return fmt_wx("%s, %s %s, %s, %s", filename, amount, direction, fields, lines);
}

DialogShiftTimes::DialogShiftTimes(agi::Context *context)
	: wxDialog(context->GetUI().parent, -1, _("Shift Times")), context(context), history_filename(config::path->Decode("?user/shift_history.json")), timecodes_loaded_slot(context->GetCore().project->AddTimecodesListener(&DialogShiftTimes::OnTimecodesLoaded, this)), selected_set_changed_slot(context->GetCore().selectionController->AddSelectionListener(&DialogShiftTimes::OnSelectedSetChanged, this)) {
	SetIcon(GETICON(shift_times_toolbutton_16));

	// Create controls
	shift_by_time = new wxRadioButton(this, -1, _("&Time: "), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
	shift_by_time->SetToolTip(_("Shift by time"));
	shift_by_time->Bind(wxEVT_RADIOBUTTON, &DialogShiftTimes::OnByTime, this);

	shift_by_frames = new wxRadioButton(this, -1 , _("&Frames: "));
	shift_by_frames->SetToolTip(_("Shift by frames"));
	shift_by_frames->Bind(wxEVT_RADIOBUTTON, &DialogShiftTimes::OnByFrames, this);

	shift_time = new TimeEdit(this, -1, context);
	shift_time->SetToolTip(_("Enter time in h:mm:ss.cs notation"));

	shift_frames = new wxTextCtrl(this, -1);
	shift_frames->SetToolTip(_("Enter number of frames to shift by"));

	shift_forward = new wxRadioButton(this, -1, _("For&ward"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
	shift_forward->SetToolTip(_("Shifts subs forward, making them appear later. Use if they are appearing too soon."));

	shift_backward = new wxRadioButton(this, -1, _("&Backward"));
	shift_backward->SetToolTip(_("Shifts subs backward, making them appear earlier. Use if they are appearing too late."));

	wxString selection_mode_vals[] = { _("&All rows"), _("Selected &rows"), _("Selection &onward") };
	selection_mode = new wxRadioBox(this, -1, _("Affect"), wxDefaultPosition, wxDefaultSize, 3, selection_mode_vals, 1);

	wxString time_field_vals[] = { _("Start a&nd End times"), _("&Start times only"), _("&End times only") };
	time_fields = new wxRadioBox(this, -1, _("Times"), wxDefaultPosition, wxDefaultSize, 3, time_field_vals, 1);

	history_box = new wxListBox(this, -1, wxDefaultPosition, FromDIP(wxSize(350, 100)), 0, nullptr, wxLB_HSCROLL);

	wxButton *clear_button = new wxButton(this, -1, _("&Clear"));
	clear_button->Bind(wxEVT_BUTTON, &DialogShiftTimes::OnClear, this);

	// Set initial control states
	OnTimecodesLoaded(context->GetCore().project->Timecodes());
	OnSelectedSetChanged();
	LoadHistory();

	shift_time->SetTime(OPT_GET("Tool/Shift Times/Time")->GetInt());
	*shift_frames << (int)OPT_GET("Tool/Shift Times/Frames")->GetInt();
	shift_by_frames->SetValue(!OPT_GET("Tool/Shift Times/ByTime")->GetBool() && shift_by_frames->IsEnabled());
	time_fields->SetSelection(OPT_GET("Tool/Shift Times/Type")->GetInt());
	selection_mode->SetSelection(OPT_GET("Tool/Shift Times/Affect")->GetInt());
	shift_backward->SetValue(OPT_GET("Tool/Shift Times/Direction")->GetBool());

	if (shift_by_frames->GetValue())
		shift_time->Disable();
	else
		shift_frames->Disable();

	// Position controls
	wxFlexGridSizer* shift_amount_sizer = new wxFlexGridSizer(2, 2, 5, 5);
	shift_amount_sizer->AddGrowableCol(1, 1);
	shift_amount_sizer->Add(shift_by_time, wxSizerFlags(0).Align(wxALIGN_CENTER_VERTICAL));
	shift_amount_sizer->Add(shift_time, wxSizerFlags().Expand());
	shift_amount_sizer->Add(shift_by_frames, wxSizerFlags(0).Align(wxALIGN_CENTER_VERTICAL));
	shift_amount_sizer->Add(shift_frames, wxSizerFlags().Expand());

	wxSizer *shift_direction_sizer = new wxBoxSizer(wxHORIZONTAL);
	shift_direction_sizer->Add(shift_forward, wxSizerFlags(1).Expand());
	shift_direction_sizer->Add(shift_backward, wxSizerFlags(1).Expand().Border(wxLEFT));

	wxSizer *shift_by_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _("Shift by"));
	shift_by_sizer->Add(shift_amount_sizer, wxSizerFlags().Expand());
	shift_by_sizer->Add(shift_direction_sizer, wxSizerFlags().Expand().Border(wxTOP));

	wxSizer *left_sizer = new wxBoxSizer(wxVERTICAL);
	left_sizer->Add(shift_by_sizer, wxSizerFlags().Expand().Border(wxBOTTOM));
	left_sizer->Add(selection_mode, wxSizerFlags().Expand().Border(wxBOTTOM));
	left_sizer->Add(time_fields, wxSizerFlags().Expand());

	wxSizer *history_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _("Load from history"));
	history_sizer->Add(history_box, wxSizerFlags(1).Expand());
	history_sizer->Add(clear_button, wxSizerFlags().Expand().Border(wxTOP));

	wxSizer *top_sizer = new wxBoxSizer(wxHORIZONTAL);
	top_sizer->Add(left_sizer, wxSizerFlags().Border(wxALL & ~wxRIGHT).Expand());
	top_sizer->Add(history_sizer, wxSizerFlags().Border().Expand());

	wxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(top_sizer, wxSizerFlags().Border(wxALL & ~wxBOTTOM));
	main_sizer->Add(CreateButtonSizer(wxOK | wxCANCEL | wxHELP), wxSizerFlags().Right().Border());
	SetSizerAndFit(main_sizer);
	CenterOnParent();

	Bind(wxEVT_BUTTON, &DialogShiftTimes::Process, this, wxID_OK);
	Bind(wxEVT_BUTTON, std::bind(&HelpButton::OpenPage, "Shift Times"), wxID_HELP);
	shift_time->Bind(wxEVT_TEXT_ENTER, &DialogShiftTimes::Process, this);
	history_box->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent& event) { RestoreHistory(event.GetInt()); });

}

DialogShiftTimes::~DialogShiftTimes() {
	OPT_SET("Tool/Shift Times/Time")->SetInt(shift_time->GetTime());
	if (auto shift = dialog_shift_times_detail::ParseFrameInput(shift_frames->GetValue()))
		OPT_SET("Tool/Shift Times/Frames")->SetInt(*shift);
	OPT_SET("Tool/Shift Times/ByTime")->SetBool(shift_by_time->GetValue());
	OPT_SET("Tool/Shift Times/Type")->SetInt(time_fields->GetSelection());
	OPT_SET("Tool/Shift Times/Affect")->SetInt(selection_mode->GetSelection());
	OPT_SET("Tool/Shift Times/Direction")->SetBool(shift_backward->GetValue());
}

void DialogShiftTimes::OnTimecodesLoaded(agi::vfr::Framerate const& new_fps) {
	fps = new_fps;
	if (fps.IsLoaded()) {
		shift_by_frames->Enable();
	}
	else {
		shift_by_time->SetValue(true);
		shift_by_frames->Disable();
		shift_time->Enable();
		shift_frames->Disable();
	}
}

void DialogShiftTimes::OnSelectedSetChanged() {
	auto core = context->GetCore();
	if (core.selectionController->GetSelectedSet().empty()) {
		selection_mode->Enable(1, false);
		selection_mode->Enable(2, false);
		selection_mode->SetSelection(0);
	}
	else {
		selection_mode->Enable(1, true);
		selection_mode->Enable(2, true);
	}
}

void DialogShiftTimes::OnClear(wxCommandEvent&) {
	try {
		agi::fs::Remove(history_filename);
		history_box->Clear();
		history.clear();
		history_writable = true;
	}
	catch (agi::fs::FileSystemError const& e) {
		LOG_E("dialog_shift_times/history") << e.GetMessage();
		wxMessageBox(_("Could not clear shift history."), _("Shift history"), wxOK | wxICON_ERROR, this);
	}
}

void DialogShiftTimes::OnByTime(wxCommandEvent&) {
	shift_time->Enable(true);
	shift_frames->Enable(false);
}

void DialogShiftTimes::OnByFrames(wxCommandEvent&) {
	shift_time->Enable(false);
	shift_frames->Enable(true);
}

void DialogShiftTimes::RestoreHistory(size_t entry) {
	if (entry >= history.size()) return;

	auto const& record = history[entry];
	if (record.by_time) {
		shift_time->SetTime(record.amount);
		shift_by_time->SetValue(true);
		shift_time->Enable(true);
		shift_frames->Enable(false);
	}
	else {
		shift_frames->SetValue(to_wx(std::to_string(record.amount)));
		if (shift_by_frames->IsEnabled()) {
			shift_by_frames->SetValue(true);
			shift_time->Enable(false);
			shift_frames->Enable(true);
		}
	}
	shift_backward->SetValue(record.backward);
	shift_forward->SetValue(!record.backward);
	selection_mode->SetSelection(record.mode);
	time_fields->SetSelection(record.fields);
}

void DialogShiftTimes::SaveHistory(std::vector<std::pair<int, int>> shifted_blocks, int amount) {
	if (!history_writable)
		return;
	auto core = context->GetCore();
	history.insert(history.begin(), {.filename = agi::fs::PathToString(core.subsController->Filename().filename()),
									 .amount = amount,
									 .by_time = shift_by_time->GetValue(),
									 .backward = shift_backward->GetValue(),
									 .fields = time_fields->GetSelection(),
									 .mode = selection_mode->GetSelection(),
									 .selected_blocks = std::move(shifted_blocks)});
	if (history.size() > 50)
		history.resize(50);
	try {
		aegisub::WriteShiftHistory(history_filename, history);
	}
	catch (agi::fs::FileSystemError const& e) {
		LOG_E("dialog_shift_times/save_history") << e.GetMessage();
		wxMessageBox(_("Subtitles were shifted, but shift history could not be saved."), _("Shift history"), wxOK | wxICON_ERROR, this);
	}
}

void DialogShiftTimes::LoadHistory() {
	try {
		json::UnknownElement root;
		json::Reader::Read(root, *agi::io::Open(history_filename));
		auto loaded = aegisub::DecodeShiftHistory(root);
		wxArrayString labels;
		for (auto const& entry : loaded)
			labels.Add(get_history_string(entry));
		history = std::move(loaded);
		history_box->Set(labels);
	}
	catch (agi::fs::FileNotFound const&) {
	}
	catch (agi::fs::FileSystemError const& e) {
		history_writable = false;
		LOG_E("dialog_shift_times/load_history") << e.GetMessage();
	}
	catch (json::Exception const& e) {
		history_writable = false;
		LOG_E("dialog_shift_times/load_history") << e.what();
	}
	if (!history_writable)
		wxMessageBox(_("Shift history could not be loaded. It will not be overwritten. Clear history to start a new history file."), _("Shift history"), wxOK | wxICON_WARNING, this);
}

void DialogShiftTimes::Process(wxCommandEvent&) {
	auto core = context->GetCore();
	int mode = selection_mode->GetSelection();
	int type = time_fields->GetSelection();
	bool reverse = shift_backward->GetValue();
	bool by_time = shift_by_time->GetValue();

	bool start = type != 2;
	bool end = type != 1;

	auto const& sel = core.selectionController->GetSelectedSet();

	int shift = 0;
	if (by_time) {
		shift = shift_time->GetTime();
		if (shift == 0) {
			Close();
			return;
		}
	}
	else {
		auto parsed_shift = dialog_shift_times_detail::ParseFrameInput(shift_frames->GetValue());
		if (!parsed_shift)
			return;
		shift = *parsed_shift;
	}

	int const amount = shift;
	if (auto directed_shift = dialog_shift_times_detail::ApplyDirection(shift, reverse))
		shift = *directed_shift;
	else
		return;

	int block_start = 0;
	std::vector<std::pair<int, int>> shifted_blocks;
	struct PendingShift {
		AssDialogue *line;
		agi::Time start;
		agi::Time end;
	};
	std::vector<PendingShift> pending;
	try {
		for (auto& line : core.ass->Events) {
			if (!sel.count(&line)) {
				if (block_start) {
					shifted_blocks.emplace_back(block_start, line.Row);
					block_start = 0;
				}
				if (mode == 1) continue;
				if (mode == 2 && shifted_blocks.empty()) continue;
			}
			else if (!block_start)
				block_start = line.Row + 1;

			pending.push_back({.line = &line, .start = line.Start, .end = line.End});
			if (start)
				pending.back().start = Shift(line.Start, shift, by_time, agi::vfr::START);
			if (end)
				pending.back().end = Shift(line.End, shift, by_time, agi::vfr::END);
		}
	}
	catch (std::overflow_error const&) {
		wxMessageBox(_("The shifted time is outside the supported range. No subtitles were changed."), _("Shift Times"), wxOK | wxICON_ERROR, this);
		return;
	}

	if (block_start)
		shifted_blocks.emplace_back(block_start, core.ass->Events.back().Row + 1);

	for (auto const& change : pending) {
		change.line->Start = change.start;
		change.line->End = change.end;
	}

	core.ass->Commit(from_wx(_("shifting")), AssFile::COMMIT_DIAG_TIME);

	SaveHistory(std::move(shifted_blocks), amount);
	Close();
}

int DialogShiftTimes::Shift(int initial_time, int shift, bool by_time, agi::vfr::Time type) {
	int64_t const shifted = static_cast<int64_t>(shift) + (by_time ? initial_time : fps.CheckedFrameAtTime(initial_time, type));
	if (shifted < std::numeric_limits<int>::min() || shifted > std::numeric_limits<int>::max())
		throw std::overflow_error("Shift is outside the supported range");
	if (by_time)
		return static_cast<int>(shifted);
	else
		return fps.CheckedTimeAtFrame(shifted, type);
}
}

void ShowShiftTimesDialog(agi::Context *c) {
	c->GetUI().dialog->Show<DialogShiftTimes>(c);
}
