// Copyright (c) 2006, Rodrigo Braz Monteiro
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

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "compat.h"
#include "format.h"
#include "help_button.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "timing_processor_preferences.h"
#include "project.h"
#include "selection_controller.h"
#include "utils.h"

#include <libaegisub/address_of_adaptor.h>
#include <libaegisub/ass/time.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/valnum.h>

namespace {
/// @class DialogTimingProcessor
/// @brief Automatic postprocessor for correcting common timing issues
struct DialogTimingProcessor {
	wxDialog d;
	agi::Context *c; ///< Project context

	int leadIn;      ///< Lead-in to add in milliseconds
	int leadOut;     ///< Lead-out to add in milliseconds
	int beforeStart; ///< Maximum time in milliseconds to move start time of line backwards to land on a keyframe
	int afterStart;  ///< Maximum time in milliseconds to move start time of line forwards to land on a keyframe
	int beforeEnd;   ///< Maximum time in milliseconds to move end time of line backwards to land on a keyframe
	int afterEnd;    ///< Maximum time in milliseconds to move end time of line forwards to land on a keyframe
	int adjGap;      ///< Maximum gap in milliseconds to snap adjacent lines to each other
	int adjOverlap;  ///< Maximum overlap in milliseconds to snap adjacent lines to each other

	wxCheckBox *onlySelection; ///< Only process selected lines of the selected styles
	wxCheckBox *hasLeadIn;     ///< Enable adding lead-in
	wxCheckBox *hasLeadOut;    ///< Enable adding lead-out
	wxCheckBox *keysEnable;    ///< Enable snapping to keyframes
	wxCheckBox *adjsEnable;    ///< Enable snapping adjacent lines to each other
	wxSlider *adjacentBias;    ///< Bias between shifting start and end times when snapping adjacent lines
	wxCheckListBox *StyleList; ///< List of styles to process
	wxButton *ApplyButton;     ///< Button to apply the processing

	void OnApply(wxCommandEvent &event);

	/// Check or uncheck all styles
	void CheckAll(bool check);

	/// Enable and disable text boxes based on which checkboxes are checked
	void UpdateControls();

	/// Process the file
	void Process();

	/// Get a list of dialogue lines in the file sorted by start time
	std::vector<AssDialogue*> SortDialogues();

	DialogTimingProcessor(agi::Context *c);
};

wxTextCtrl *make_ctrl(wxWindow *parent, wxSizer *sizer, wxString const& desc, int *value, wxCheckBox *cb, wxString const& tooltip) {
	wxIntegerValidator<int> validator(value);
	validator.SetMin(0);
	wxTextCtrl *ctrl = new wxTextCtrl(parent, -1, wxEmptyString, wxDefaultPosition, wxSize(60,-1), 0, validator);
	ctrl->SetToolTip(tooltip);
	if (!desc.empty())
		sizer->Add(new wxStaticText(parent, -1, desc), wxSizerFlags().Center().Border(wxRIGHT));
	sizer->Add(ctrl, wxSizerFlags().Expand().Border(wxRIGHT));

	ctrl->Enable(cb->IsChecked());
	cb->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent& evt) {
		ctrl->Enable(cb->IsChecked());
		evt.Skip();
	});

	return ctrl;
}

inline wxTextCtrl *make_ctrl(wxStaticBoxSizer *sizer, wxString const& desc, int *value, wxCheckBox *cb, wxString const& tooltip) {
	return make_ctrl(sizer->GetStaticBox()->GetParent(), sizer, desc, value, cb, tooltip);
}

wxCheckBox *make_check(wxStaticBoxSizer *sizer, wxString const& desc, bool initial, wxString const& tooltip) {
	wxCheckBox *cb = new wxCheckBox(sizer->GetStaticBox()->GetParent(), -1, desc);
	cb->SetToolTip(tooltip);
	cb->SetValue(initial);
	sizer->Add(cb, wxSizerFlags().Border(wxRIGHT).Expand());
	return cb;
}

DialogTimingProcessor::DialogTimingProcessor(agi::Context *c)
: d(c->GetUI().parent, -1, _("Timing Post-Processor"))
, c(c)
{
	using std::bind;
	auto core = c->GetCore();

	d.SetIcon(GETICON(timing_processor_toolbutton_16));

	auto const preferences = aegisub::LoadTimingProcessorPreferences(*config::opt);
	leadIn = preferences.lead_in_ms;
	leadOut = preferences.lead_out_ms;
	beforeStart = preferences.key_start_before_ms;
	afterStart = preferences.key_start_after_ms;
	beforeEnd = preferences.key_end_before_ms;
	afterEnd = preferences.key_end_after_ms;
	adjGap = preferences.adjacent_gap_ms;
	adjOverlap = preferences.adjacent_overlap_ms;

	// Styles box
	auto LeftSizer = new wxStaticBoxSizer(wxVERTICAL,&d,_("Apply to styles"));
	StyleList = new wxCheckListBox(&d, -1, wxDefaultPosition, d.FromDIP(wxSize(150, 150)), to_wx(core.ass->GetStyles()));
	StyleList->SetToolTip(_("Select styles to process. Unchecked ones will be ignored."));

	auto all = new wxButton(&d,-1,_("&All"));
	all->SetToolTip(_("Select all styles"));

	auto none = new wxButton(&d,-1,_("&None"));
	none->SetToolTip(_("Deselect all styles"));

	// Options box
	auto optionsSizer = new wxStaticBoxSizer(wxHORIZONTAL,&d,_("Options"));
	onlySelection = new wxCheckBox(&d,-1,_("Affect &selection only"));
	onlySelection->SetValue(preferences.only_selection);
	optionsSizer->Add(onlySelection, 1, wxALL, 0);

	// Lead-in/out box
	auto LeadSizer = new wxStaticBoxSizer(wxHORIZONTAL, &d, _("Lead-in/Lead-out"));

	hasLeadIn = make_check(LeadSizer, _("Add lead &in:"),
						   preferences.lead_in_enabled,
						   _("Enable adding of lead-ins to lines"));
	make_ctrl(LeadSizer, wxEmptyString, &leadIn, hasLeadIn, _("Lead in to be added, in milliseconds"));

	hasLeadOut = make_check(LeadSizer, _("Add lead &out:"),
							preferences.lead_out_enabled,
							_("Enable adding of lead-outs to lines"));
	make_ctrl(LeadSizer, wxEmptyString, &leadOut, hasLeadOut, _("Lead out to be added, in milliseconds"));

	LeadSizer->AddStretchSpacer(1);

	// Adjacent subs sizer
	auto AdjacentSizer = new wxStaticBoxSizer(wxHORIZONTAL, &d, _("Make adjacent subtitles continuous"));
	adjsEnable = make_check(AdjacentSizer, _("&Enable"),
							preferences.adjacent_enabled,
							_("Enable snapping of subtitles together if they are within a certain distance of each other"));

	auto adjBoxes = new wxBoxSizer(wxHORIZONTAL);
	make_ctrl(&d, adjBoxes, _("Max gap:"), &adjGap, adjsEnable,
		_("Maximum difference between start and end time for two subtitles to be made continuous, in milliseconds"));
	make_ctrl(&d, adjBoxes, _("Max overlap:"), &adjOverlap, adjsEnable,
		_("Maximum overlap between the end and start time for two subtitles to be made continuous, in milliseconds"));

	adjacentBias = new wxSlider(&d, -1, mid<int>(0, preferences.adjacent_bias * 100, 100), 0, 100);
	adjacentBias->SetToolTip(_("Sets how to set the adjoining of lines. If set totally to left, it will extend or shrink start time of the second line; if totally to right, it will extend or shrink the end time of the first line."));

	auto adjSliderSizer = new wxBoxSizer(wxHORIZONTAL);
	adjSliderSizer->Add(new wxStaticText(&d, -1, _("Bias: Start <- ")), wxSizerFlags().Center());
	adjSliderSizer->Add(adjacentBias, wxSizerFlags(1).Center());
	adjSliderSizer->Add(new wxStaticText(&d, -1, _(" -> End")), wxSizerFlags().Center());

	auto adjRightSizer = new wxBoxSizer(wxVERTICAL);
	adjRightSizer->Add(adjBoxes, wxSizerFlags().Expand());
	adjRightSizer->Add(adjSliderSizer, wxSizerFlags().Expand().Border(wxTOP));
	AdjacentSizer->Add(adjRightSizer);

	// Keyframes sizer
	auto KeyframesSizer = new wxStaticBoxSizer(wxHORIZONTAL, &d, _("Keyframe snapping"));
	auto KeyframesFlexSizer = new wxFlexGridSizer(2,5,5,0);

	keysEnable = new wxCheckBox(&d, -1, _("E&nable"));
	keysEnable->SetToolTip(_("Enable snapping of subtitles to nearest keyframe, if distance is within threshold"));
	keysEnable->SetValue(preferences.keyframes_enabled);
	KeyframesFlexSizer->Add(keysEnable, 0, wxRIGHT | wxEXPAND, 10);

	// Keyframes are only available if timecodes are loaded
	bool keysAvailable = !core.project->Keyframes().empty() && core.project->Timecodes().IsLoaded();
	if (!keysAvailable) {
		keysEnable->SetValue(false);
		keysEnable->Enable(false);
	}

	make_ctrl(&d, KeyframesFlexSizer, _("Starts before thres.:"), &beforeStart, keysEnable,
		_("Threshold for 'before start' distance, that is, how many milliseconds a subtitle must start before a keyframe to snap to it"));

	make_ctrl(&d, KeyframesFlexSizer, _("Starts after thres.:"), &afterStart, keysEnable,
		_("Threshold for 'after start' distance, that is, how many milliseconds a subtitle must start after a keyframe to snap to it"));

	KeyframesFlexSizer->AddStretchSpacer(1);

	make_ctrl(&d, KeyframesFlexSizer, _("Ends before thres.:"), &beforeEnd, keysEnable,
		_("Threshold for 'before end' distance, that is, how many milliseconds a subtitle must end before a keyframe to snap to it"));

	make_ctrl(&d, KeyframesFlexSizer, _("Ends after thres.:"), &afterEnd, keysEnable,
		_("Threshold for 'after end' distance, that is, how many milliseconds a subtitle must end after a keyframe to snap to it"));

	KeyframesSizer->Add(KeyframesFlexSizer,0,wxEXPAND);
	KeyframesSizer->AddStretchSpacer(1);

	// Button sizer
	auto ButtonSizer = d.CreateStdDialogButtonSizer(wxOK | wxCANCEL | wxHELP);
	ApplyButton = ButtonSizer->GetAffirmativeButton();
	ButtonSizer->GetHelpButton()->Bind(wxEVT_BUTTON, bind(&HelpButton::OpenPage, "Timing Processor"));

	// Right Sizer
	auto RightSizer = new wxBoxSizer(wxVERTICAL);
	RightSizer->Add(optionsSizer,0,wxBOTTOM|wxEXPAND,5);
	RightSizer->Add(LeadSizer,0,wxBOTTOM|wxEXPAND,5);
	RightSizer->Add(AdjacentSizer,0,wxBOTTOM|wxEXPAND,5);
	RightSizer->Add(KeyframesSizer,0,wxBOTTOM|wxEXPAND,5);
	RightSizer->AddStretchSpacer(1);
	RightSizer->Add(ButtonSizer,0,wxLEFT|wxRIGHT|wxBOTTOM|wxEXPAND,0);

	// Style buttons sizer
	auto StyleButtonsSizer = new wxBoxSizer(wxHORIZONTAL);
	StyleButtonsSizer->Add(all,1,0,0);
	StyleButtonsSizer->Add(none,1,0,0);

	// Left sizer
	LeftSizer->Add(StyleList, wxSizerFlags(1).Border(wxBOTTOM));
	LeftSizer->Add(StyleButtonsSizer, wxSizerFlags().Expand());

	// Top Sizer
	auto TopSizer = new wxBoxSizer(wxHORIZONTAL);
	TopSizer->Add(LeftSizer,0,wxRIGHT|wxEXPAND,5);
	TopSizer->Add(RightSizer,1,wxALL|wxEXPAND,0);

	// Main Sizer
	auto MainSizer = new wxBoxSizer(wxVERTICAL);
	MainSizer->Add(TopSizer,1,wxALL|wxEXPAND,5);
	d.SetSizerAndFit(MainSizer);
	d.CenterOnParent();

	d.Bind(wxEVT_CHECKBOX, bind(&DialogTimingProcessor::UpdateControls, this));
	d.Bind(wxEVT_CHECKLISTBOX, bind(&DialogTimingProcessor::UpdateControls, this));
	d.Bind(wxEVT_BUTTON, &DialogTimingProcessor::OnApply, this, wxID_OK);
	all->Bind(wxEVT_BUTTON, bind(&DialogTimingProcessor::CheckAll, this, true));
	none->Bind(wxEVT_BUTTON, bind(&DialogTimingProcessor::CheckAll, this, false));

	CheckAll(true);
}

void DialogTimingProcessor::CheckAll(bool value) {
	size_t count = StyleList->GetCount();
	for (size_t i = 0; i < count; ++i)
		StyleList->Check(i, value);
	UpdateControls();
}

void DialogTimingProcessor::UpdateControls() {
	// Only enable the OK button if it'll actually do something
	bool any_checked = false;
	size_t len = StyleList->GetCount();
	for (size_t i = 0; !any_checked && i < len; ++i)
		any_checked = StyleList->IsChecked(i);
	ApplyButton->Enable(any_checked && (hasLeadIn->IsChecked() || hasLeadOut->IsChecked() || keysEnable->IsChecked() || adjsEnable->IsChecked()));
}

void DialogTimingProcessor::OnApply(wxCommandEvent &) {
	d.TransferDataFromWindow();
	aegisub::SaveTimingProcessorPreferences(*config::opt,
											{.lead_in_ms = leadIn,
											 .lead_out_ms = leadOut,
											 .key_start_before_ms = beforeStart,
											 .key_start_after_ms = afterStart,
											 .key_end_before_ms = beforeEnd,
											 .key_end_after_ms = afterEnd,
											 .adjacent_gap_ms = adjGap,
											 .adjacent_overlap_ms = adjOverlap,
											 .adjacent_bias = adjacentBias->GetValue() / 100.0,
											 .lead_in_enabled = hasLeadIn->IsChecked(),
											 .lead_out_enabled = hasLeadOut->IsChecked(),
											 .keyframes_enabled = keysEnable->IsChecked(),
											 .adjacent_enabled = adjsEnable->IsChecked(),
											 .only_selection = onlySelection->IsChecked()},
											keysEnable->IsEnabled());

	try {
		Process();
	}
	catch (std::overflow_error const&) {
		wxMessageBox(
			_("Adding lead-in/out would produce a time outside the supported range. No subtitles were changed."),
			_("Invalid timing"), wxOK | wxICON_ERROR | wxCENTER, &d);
		return;
	}
	d.EndModal(0);
}

std::vector<AssDialogue*> DialogTimingProcessor::SortDialogues() {
	std::set<boost::flyweight<std::string>> styles;
	for (size_t i = 0; i < StyleList->GetCount(); ++i) {
		if (StyleList->IsChecked(i))
			styles.insert(boost::flyweight<std::string>(from_wx(StyleList->GetString(i))));
	}

	std::vector<AssDialogue*> sorted;

	auto valid_line = [&](const AssDialogue *d) { return !d->Comment && styles.count(d->Style); };
	if (onlySelection->IsChecked()) {
		auto core = c->GetCore();
		for (auto* dialogue : core.selectionController->GetSelectedSet()) {
			if (valid_line(dialogue))
				sorted.push_back(dialogue);
		}
	}
	else {
		auto core = c->GetCore();
		sorted.reserve(core.ass->Events.size());
		for (auto& dialogue : core.ass->Events) {
			if (valid_line(&dialogue))
				sorted.push_back(&dialogue);
		}
	}

	// Check if rows are valid
	for (auto diag : sorted) {
		if (diag->Start > diag->End) {
			wxMessageBox(
				fmt_tl("One of the lines in the file (%i) has negative duration. Aborting.", diag->Row),
				_("Invalid script"),
				wxOK | wxICON_ERROR | wxCENTER);
			sorted.clear();
			break;
		}
	}

	std::sort(sorted.begin(), sorted.end(), [](const AssDialogue *a, const AssDialogue *b) {
		return a->Start < b->Start;
	});
	return sorted;
}

static int get_closest_kf(std::vector<int> const& kf, int frame) {
	const auto pos = std::upper_bound(kf.begin(), kf.end(), frame);
	// Return last keyframe if this is after the last one
	if (pos == end(kf)) return kf.back();
	// *pos is greater than frame, and *(pos - 1) is less than or equal to frame
	return (pos == begin(kf) || *pos - frame < frame - *(pos - 1)) ? *pos : *(pos - 1);
}

template <class Iter, class Field>
static int safe_time(Iter begin, Iter end, AssDialogue *comp, int64_t initial, Field field, int64_t const& (*cmp)(int64_t const&, int64_t const&)) {
	// Compare to every previous line (yay for O(n^2)!) to see if it's OK to add lead-in
	for (; begin != end; ++begin) {
		// If the line doesn't already collide with this line, extend it only
		// to the edge of the line
		if (!comp->CollidesWith(*begin))
			initial = cmp(initial, static_cast<int>((*begin)->*field));
	}
	if (initial < std::numeric_limits<int>::min() || initial > std::numeric_limits<int>::max())
		throw std::overflow_error("Timing processor lead-in/out exceeds the time range");
	return static_cast<int>(initial);
}

void DialogTimingProcessor::Process() {
	std::vector<AssDialogue *> original = SortDialogues();
	if (original.empty())
		return;

	std::vector<AssDialogue> pending;
	std::vector<AssDialogue *> sorted;
	pending.reserve(original.size());
	sorted.reserve(original.size());
	for (auto line : original) {
		pending.emplace_back(static_cast<AssDialogueBase const&>(*line));
		sorted.push_back(&pending.back());
	}

	// Add lead-in/out
	if (hasLeadIn->IsChecked() && leadIn) {
		for (size_t i = 0; i < sorted.size(); ++i)
			sorted[i]->Start = safe_time(sorted.rend() - i, sorted.rend(),
										 sorted[i], static_cast<int64_t>(static_cast<int>(sorted[i]->Start)) - leadIn,
										 &AssDialogue::End, &std::max<int64_t>);
	}

	if (hasLeadOut->IsChecked() && leadOut) {
		for (size_t i = 0; i < sorted.size(); ++i)
			sorted[i]->End = safe_time(sorted.begin() + i + 1, sorted.end(),
									   sorted[i], static_cast<int64_t>(static_cast<int>(sorted[i]->End)) + leadOut,
									   &AssDialogue::Start, &std::min<int64_t>);
	}

	// Make adjacent
	if (adjsEnable->IsChecked()) {
		double bias = adjacentBias->GetValue() / 100.0;

		for (size_t i = 1; i < sorted.size(); ++i) {
			AssDialogue *prev = sorted[i - 1];
			AssDialogue *cur = sorted[i];

			// Raw millisecond values are used in this step instead of the typical centisecond.
			// In this step, we need to distinguish between gap and overlap, as they have different thresholds.
			// A small gap / overlap less than 1 centisecond may not be distinguishable using rounded centisecond values.
			int dist = cur->Start.GetMillisecond() - prev->End.GetMillisecond();
			if ((dist < 0 && -dist <= adjOverlap) || (dist > 0 && dist <= adjGap)) {
				int setPos = prev->End.GetMillisecond() + int(dist * bias + 0.5);
				cur->Start = setPos;
				prev->End = setPos;
			}
		}
	}

	// Keyframe snapping
	if (keysEnable->IsChecked()) {
		auto core = c->GetCore();
		std::vector<int> kf = core.project->Keyframes();
		auto fps = core.project->Timecodes();
		if (auto provider = core.project->VideoProvider())
			kf.push_back(provider->GetFrameCount() - 1);

		for (AssDialogue *cur : sorted) {
			// Get start/end frames
			int startF = fps.FrameAtTime(cur->Start, agi::vfr::START);
			int endF = fps.FrameAtTime(cur->End, agi::vfr::END);

			// Get closest for start
			int closest = get_closest_kf(kf, startF);
			int time = fps.TimeAtFrame(closest, agi::vfr::START);
			if ((closest > startF && time - cur->Start <= beforeStart) || (closest < startF && cur->Start - time <= afterStart))
				cur->Start = time;

			// Get closest for end
			closest = get_closest_kf(kf, endF) - 1;
			time = fps.TimeAtFrame(closest, agi::vfr::END);
			if ((closest > endF && time - cur->End <= beforeEnd) || (closest < endF && cur->End - time <= afterEnd))
				cur->End = time;
		}
	}

	for (size_t i = 0; i < original.size(); ++i) {
		original[i]->Start = sorted[i]->Start;
		original[i]->End = sorted[i]->End;
	}
	c->GetCore().ass->Commit(from_wx(_("timing processor")), AssFile::COMMIT_DIAG_TIME);
}
}

void ShowTimingProcessorDialog(agi::Context *c) {
	DialogTimingProcessor(c).d.ShowModal();
}
