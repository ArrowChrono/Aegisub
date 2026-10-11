// Copyright (c) 2006, 2007, Niels Martin Hansen
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

#include "wx_automation_dialog.h"

#include "automation/automation_ui.h"
#include "colour_button.h"
#include "compat.h"
#include "perf_trace.h"
#include "validators.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <type_traits>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/gbsizer.h>
#include <wx/odcombo.h>
#include <wx/panel.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/valgen.h>

namespace {
using namespace Automation4;

struct ControlCreateSummary {
	int instance_count = 0;
	int item_count_total = 0;
	int item_count_max = 0;
	double duration_ms = 0.0;
};

struct ControlCreateTrace {
	double native_construct_ms = 0.0;
	double validator_bind_ms = 0.0;
	double initial_value_set_ms = 0.0;
	double floatedit_spin_construct_with_value_ms = 0.0;
	double floatedit_spin_validator_bind_ms = 0.0;
	double floatedit_text_construct_with_validator_ms = 0.0;
};

double DurationMs(std::chrono::steady_clock::duration duration) {
	return std::chrono::duration<double, std::milli>(duration).count();
}

int ButtonId(AutomationDialogButtonRole role) {
	switch (role) {
		case AutomationDialogButtonRole::Ok: return wxID_OK;
		case AutomationDialogButtonRole::Yes: return wxID_YES;
		case AutomationDialogButtonRole::Save: return wxID_SAVE;
		case AutomationDialogButtonRole::Apply: return wxID_APPLY;
		case AutomationDialogButtonRole::Close: return wxID_CLOSE;
		case AutomationDialogButtonRole::No: return wxID_NO;
		case AutomationDialogButtonRole::Cancel: return wxID_CANCEL;
		case AutomationDialogButtonRole::Help: return wxID_HELP;
		case AutomationDialogButtonRole::ContextHelp: return wxID_CONTEXT_HELP;
		case AutomationDialogButtonRole::Custom: return wxID_ANY;
	}
	return wxID_ANY;
}

char const *TraceType(AutomationDialogValue const& value) {
	return std::visit([](auto const& data) -> char const * {
		using T = std::decay_t<decltype(data)>;
		if constexpr (std::is_same_v<T, AutomationDialogLabel>)
			return "label";
		else if constexpr (std::is_same_v<T, AutomationDialogEdit>)
			return data.multiline ? "textbox" : "edit";
		else if constexpr (std::is_same_v<T, AutomationDialogColor>)
			return data.alpha ? "coloralpha" : "color";
		else if constexpr (std::is_same_v<T, AutomationDialogIntEdit>)
			return "intedit";
		else if constexpr (std::is_same_v<T, AutomationDialogFloatEdit>)
			return "floatedit";
		else if constexpr (std::is_same_v<T, AutomationDialogDropdown>)
			return "dropdown_odcombo";
		else
			return "checkbox";
	},
					  value);
}

wxControl *CreateControl(AutomationDialogControl& control, wxWindow *parent, ControlCreateTrace& trace) {
	auto *created = std::visit([&](auto& data) -> wxControl * {
		using T = std::decay_t<decltype(data)>;
		if constexpr (std::is_same_v<T, AutomationDialogLabel>) {
			return new wxStaticText(parent, -1, to_wx(data.label));
		}
		else if constexpr (std::is_same_v<T, AutomationDialogEdit>) {
			if (data.multiline) {
				auto *text = new wxTextCtrl(parent, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE, StringBinder(&data.value));
				text->SetMinSize(wxSize(0, parent->FromDIP(30)));
				return text;
			}
			auto *text = new wxTextCtrl(parent, -1, to_wx(data.value));
			text->SetMaxLength(0);
			text->SetValidator(StringBinder(&data.value));
			return text;
		}
		else if constexpr (std::is_same_v<T, AutomationDialogColor>) {
			return new ColourButton(parent, wxSize(50 * control.width, 10 * control.height), data.alpha, data.value, ColorValidator(&data.value));
		}
		else if constexpr (std::is_same_v<T, AutomationDialogIntEdit>) {
			auto *spin = new wxSpinCtrl(parent, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, data.min, data.max, data.value);
			spin->SetValidator(wxGenericValidator(&data.value));
			return spin;
		}
		else if constexpr (std::is_same_v<T, AutomationDialogFloatEdit>) {
			if (data.step > 0) {
				auto const construct_started = std::chrono::steady_clock::now();
				auto *spin = new wxSpinCtrlDouble(parent, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, data.min, data.max, data.value, data.step);
				trace.floatedit_spin_construct_with_value_ms = DurationMs(std::chrono::steady_clock::now() - construct_started);
				auto const validator_started = std::chrono::steady_clock::now();
				spin->SetValidator(DoubleSpinValidator(&data.value));
				trace.floatedit_spin_validator_bind_ms = DurationMs(std::chrono::steady_clock::now() - validator_started);
				return spin;
			}
			DoubleValidator validator(&data.value, data.min, data.max);
			auto const construct_started = std::chrono::steady_clock::now();
			auto *text = new wxTextCtrl(parent, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, validator);
			trace.floatedit_text_construct_with_validator_ms = DurationMs(std::chrono::steady_clock::now() - construct_started);
			return text;
		}
		else if constexpr (std::is_same_v<T, AutomationDialogDropdown>) {
			auto const items = to_wx(data.items);
			auto const value = to_wx(data.value);
			auto const construct_started = std::chrono::steady_clock::now();
			auto *combo = new wxOwnerDrawnComboBox(parent, -1, wxEmptyString, wxDefaultPosition, wxDefaultSize, items, wxCB_READONLY | wxODCB_STD_CONTROL_PAINT);
			trace.native_construct_ms = DurationMs(std::chrono::steady_clock::now() - construct_started);
			auto const validator_started = std::chrono::steady_clock::now();
			combo->SetValidator(StringBinder(&data.value));
			trace.validator_bind_ms = DurationMs(std::chrono::steady_clock::now() - validator_started);
			auto const initial_value_started = std::chrono::steady_clock::now();
			combo->SetStringSelection(value);
			trace.initial_value_set_ms = DurationMs(std::chrono::steady_clock::now() - initial_value_started);
			return combo;
		}
		else {
			auto *checkbox = new wxCheckBox(parent, -1, to_wx(data.label));
			checkbox->SetValidator(wxGenericValidator(&data.value));
			checkbox->SetValue(data.value);
			return checkbox;
		}
	},
							   control.value);
	if (!control.hint.empty() && !std::holds_alternative<AutomationDialogLabel>(control.value))
		created->SetToolTip(to_wx(control.hint));
	return created;
}

wxWindow *CreateWindow(AutomationDialog& model, wxWindow *parent) {
	auto const create_window_started = std::chrono::steady_clock::now();
	auto *window = new wxPanel(parent);
	auto *sizer = new wxGridBagSizer(4, 4);
	auto const control_count = static_cast<int>(model.controls.size());
	auto const button_count = static_cast<int>(model.buttons.size());
	auto const create_controls_started = std::chrono::steady_clock::now();
	std::map<std::string, ControlCreateSummary> control_summaries;
	std::map<std::string, std::map<std::string, ControlCreateSummary>> control_step_summaries;
	for (auto& control : model.controls) {
		auto const control_started = std::chrono::steady_clock::now();
		ControlCreateTrace trace;
		auto *created_control = CreateControl(control, window, trace);
		auto const control_duration_ms = DurationMs(std::chrono::steady_clock::now() - control_started);
		auto const *type = TraceType(control.value);
		auto const *dropdown = std::get_if<AutomationDialogDropdown>(&control.value);
		auto const item_count = dropdown ? static_cast<int>(dropdown->items.size()) : 0;
		auto& summary = control_summaries[type];
		++summary.instance_count;
		summary.duration_ms += control_duration_ms;
		summary.item_count_total += item_count;
		summary.item_count_max = std::max(summary.item_count_max, item_count);
		auto accumulate_step = [&](char const *step, double duration_ms) {
			if (duration_ms <= 0.0) return;
			auto& step_summary = control_step_summaries[type][step];
			++step_summary.instance_count;
			step_summary.duration_ms += duration_ms;
			step_summary.item_count_total += item_count;
			step_summary.item_count_max = std::max(step_summary.item_count_max, item_count);
		};
		accumulate_step("native_construct", trace.native_construct_ms);
		accumulate_step("validator_bind", trace.validator_bind_ms);
		accumulate_step("initial_value_set", trace.initial_value_set_ms);
		accumulate_step("floatedit_spin_construct_with_value", trace.floatedit_spin_construct_with_value_ms);
		accumulate_step("floatedit_spin_validator_bind", trace.floatedit_spin_validator_bind_ms);
		accumulate_step("floatedit_text_construct_with_validator", trace.floatedit_text_construct_with_validator_ms);
		auto const flags = std::holds_alternative<AutomationDialogLabel>(control.value) ? wxALIGN_CENTRE_VERTICAL | wxALIGN_LEFT : wxEXPAND;
		sizer->Add(created_control, wxGBPosition(control.y, control.x), wxGBSpan(control.height, control.width), flags);
	}
	for (auto const& [type, summary] : control_summaries) {
		perf_trace::ObserveLuaDialogControlTypeSummary(type.c_str(), control_count, button_count,
													   summary.instance_count, summary.item_count_total, summary.item_count_max, summary.duration_ms);
	}
	for (auto const& [type, step_summaries] : control_step_summaries) {
		for (auto const& [step, summary] : step_summaries) {
			perf_trace::ObserveLuaDialogControlStepSummary(type.c_str(), step.c_str(), control_count, button_count,
														   summary.instance_count, summary.item_count_total, summary.item_count_max, summary.duration_ms);
		}
	}
	perf_trace::ObserveLuaDialogPhase("create_controls", control_count, button_count,
									  DurationMs(std::chrono::steady_clock::now() - create_controls_started));

	if (!model.use_buttons) {
		auto const layout_started = std::chrono::steady_clock::now();
		window->SetSizerAndFit(sizer);
		perf_trace::ObserveLuaDialogPhase("layout_window", control_count, 0, DurationMs(std::chrono::steady_clock::now() - layout_started));
		perf_trace::ObserveLuaDialogPhase("create_window_total", control_count, 0, DurationMs(std::chrono::steady_clock::now() - create_window_started));
		perf_trace::TraceLuaDialogOpenEnd(control_count, 0, -1.0, true);
		return window;
	}

	auto const create_buttons_started = std::chrono::steady_clock::now();
	auto *dialog = static_cast<wxDialog *>(parent);
	auto *button_sizer = new wxStdDialogButtonSizer;
	auto make_button = [&](size_t index) {
		auto const& button_model = model.buttons[index];
		auto const id = ButtonId(button_model.role);
		auto *button = new wxButton(window, id, to_wx(button_model.label));
		button->Bind(wxEVT_BUTTON, [&model, dialog, index](wxCommandEvent&) {
			model.button_pushed = static_cast<int>(index);
			dialog->TransferDataFromWindow();
			dialog->EndModal(0);
		});
		if (id == wxID_OK || id == wxID_YES || id == wxID_SAVE) {
			button->SetDefault();
			dialog->SetAffirmativeId(id);
		}
		if (id == wxID_CLOSE || id == wxID_NO) dialog->SetEscapeId(id);
		return button;
	};
	if (std::ranges::none_of(model.buttons, [](AutomationDialogButton const& button) {
			return button.role == AutomationDialogButtonRole::Custom;
		})) {
		for (size_t i = 0; i < model.buttons.size(); ++i) button_sizer->AddButton(make_button(i));
		button_sizer->Realize();
	}
	else {
		for (size_t i = 0; i < model.buttons.size(); ++i) button_sizer->Add(make_button(i));
	}
	perf_trace::ObserveLuaDialogPhase("create_buttons", control_count, button_count,
									  DurationMs(std::chrono::steady_clock::now() - create_buttons_started));

	auto const layout_started = std::chrono::steady_clock::now();
	auto *main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(sizer, 0, wxBOTTOM, 5);
	main_sizer->Add(button_sizer);
	window->SetSizerAndFit(main_sizer);
	perf_trace::ObserveLuaDialogPhase("layout_window", control_count, button_count, DurationMs(std::chrono::steady_clock::now() - layout_started));
	perf_trace::ObserveLuaDialogPhase("create_window_total", control_count, button_count, DurationMs(std::chrono::steady_clock::now() - create_window_started));
	perf_trace::TraceLuaDialogOpenEnd(control_count, button_count, -1.0, true);
	return window;
}
}

namespace Automation4 {
wxWindow *CreateAutomationDialogWindow(AutomationDialog& model, wxWindow *parent) {
	perf_trace::TraceLuaDialogOpenBegin();
	perf_trace::ObserveLuaDialogPhase("build_model", static_cast<int>(model.controls.size()),
									  static_cast<int>(model.buttons.size()), model.model_build_duration_ms);
	try {
		return CreateWindow(model, parent);
	}
	catch (...) {
		perf_trace::TraceLuaDialogOpenEnd(-1, -1, -1.0, false);
		throw;
	}
}
}