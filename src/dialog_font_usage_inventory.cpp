#include "dialog_font_usage_inventory.h"

#include "compat.h"
#include "dialog_manager.h"
#include "font_family_catalog_cache.h"
#include "font_usage_inventory.h"
#include "font_usage_inventory_watcher.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "ui_dispatch.h"
#include "utils.h"

#include <libaegisub/exception.h>
#include <libaegisub/fs.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dirdlg.h>
#include <wx/dnd.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/spinbutt.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/tipwin.h>
#include <wx/utils.h>
#include <wx/wrapsizer.h>

namespace {

constexpr char recursive_option[] = "Tool/Font Usage Inventory/Recursive";

constexpr char threshold_option[] = "Tool/Font Usage Inventory/Low Frequency Threshold";

int const copy_names_id = wxWindow::NewControlId();
int const copy_full_id = wxWindow::NewControlId();

std::array const scroll_events{wxEVT_SCROLLWIN_TOP, wxEVT_SCROLLWIN_BOTTOM, wxEVT_SCROLLWIN_LINEUP,
							   wxEVT_SCROLLWIN_LINEDOWN, wxEVT_SCROLLWIN_PAGEUP, wxEVT_SCROLLWIN_PAGEDOWN,
							   wxEVT_SCROLLWIN_THUMBTRACK, wxEVT_SCROLLWIN_THUMBRELEASE};

class InventoryField final : public wxTextCtrl {
	wxString details;

	public:
	InventoryField(wxWindow *parent, wxString const& value, bool summary,
				   std::function<bool()> can_copy_list, std::function<void(bool)> copy_list)
		: wxTextCtrl(parent, wxID_ANY, value, wxDefaultPosition, wxDefaultSize,
					 wxBORDER_NONE | wxTE_READONLY | wxTE_MULTILINE | wxTE_NOHIDESEL | wxTE_NO_VSCROLL | (summary ? wxTE_RIGHT : 0)) {
		SetName(summary ? wxS("Font usage status") : wxS("Font usage name"));
		Bind(wxEVT_CONTEXT_MENU, [this, can_copy_list = std::move(can_copy_list)](wxContextMenuEvent&) {
			long start = 0;
			long end = 0;
			GetSelection(&start, &end);
			wxMenu menu;
			menu.Append(wxID_COPY, _("Copy"));
			menu.Append(wxID_SELECTALL, _("Select All"));
			menu.AppendSeparator();
			menu.Append(copy_names_id, _("Copy font names list"));
			menu.Append(copy_full_id, _("Copy full list"));
			menu.Enable(wxID_COPY, start != end);
			menu.Enable(wxID_SELECTALL, !GetValue().empty());
			menu.Enable(copy_names_id, can_copy_list());
			menu.Enable(copy_full_id, can_copy_list());
			PopupMenu(&menu);
		});
		Bind(wxEVT_MENU, [this](wxCommandEvent&) { Copy(); }, wxID_COPY);
		Bind(wxEVT_MENU, [this](wxCommandEvent&) { SelectAll(); }, wxID_SELECTALL);
		Bind(wxEVT_MENU, [copy_list](wxCommandEvent&) { copy_list(true); }, copy_names_id);
		Bind(wxEVT_MENU, [copy_list = std::move(copy_list)](wxCommandEvent&) { copy_list(false); }, copy_full_id);
	}

	void SetDetails(wxString const& text) { details = text; }

	[[nodiscard]] wxString GetHelpTextAtPoint(wxPoint const&, wxHelpEvent::Origin) const override { return details; }
};
class InventoryDropTarget final : public wxFileDropTarget {
	std::function<bool(wxArrayString const&)> receive;

	public:
	explicit InventoryDropTarget(std::function<bool(wxArrayString const&)> receive)
		: receive(std::move(receive)) {}

	bool OnDropFiles(wxCoord, wxCoord, wxArrayString const& filenames) override {
		return receive(filenames);
	}
};

int RowStyle(font_usage_inventory::Row const& row) {
	if (row.display_state == "missing_font" || row.display_state == "missing_glyphs") return 1;
	if (row.display_state == "warning") return 2;
	if (row.display_state == "low_usage") return 3;
	return 0;
}

wxString JoinDetails(std::set<std::string> const& values) {
	wxString text;
	for (auto const& value : values) {
		if (!text.empty()) text += _("; ");
		text += to_wx(value);
	}
	return text;
}

wxString UsageCount(uint64_t count) {
	return wxString::Format(wxPLURAL("%llu use", "%llu uses", count), static_cast<unsigned long long>(count));
}

wxString ReasonText(font_usage_inventory::Reason const& reason, font_usage_inventory::Row const& row) {
	if (reason.code == "missing_font") return _("Not found");
	if (reason.code == "missing_glyphs")
		return wxString::Format(_("Missing glyphs: %s"), to_wx(font_usage_inventory::FormatMissingCodepoints(row.missing_codepoints.value())));
	if (reason.code == "fake_bold") return _("Warning: synthetic bold");
	if (reason.code == "fake_italic") return _("Warning: synthetic italic");
	if (reason.code == "ambiguous_font_match") return _("Warning: ambiguous font match");
	if (reason.code == "low_usage") return _("Low usage");
	return wxString::Format(_("Warning: %s"), to_wx(reason.code));
}

wxString RowSuffix(font_usage_inventory::Row const& row) {
	auto count = UsageCount(row.usage_count);
	if (row.display_state == "missing_font")
		return wxString::Format(_(L"%s · Not found"), count);
	if (row.display_state == "missing_glyphs")
		return wxString::Format(_(L"%s · Missing glyphs"), count);
	if (row.display_state == "warning")
		return wxString::Format(_(L"%s · Warning"), count);
	if (row.display_state == "low_usage")
		return wxString::Format(_(L"%s · Low usage"), count);
	return count;
}

wxString RowDetails(font_usage_inventory::Row const& row, std::string const& provider) {
	std::set<std::string> reasons;
	std::set<std::string> requested_names(row.ass_names.begin(), row.ass_names.end());
	std::set<std::string> matched_names;
	std::set<int> matched_weights;
	std::set<std::string> matched_paths;
	std::map<std::string, std::set<int>> sources;
	for (auto const& reason : row.reasons) reasons.insert(from_wx(ReasonText(reason, row)));
	for (auto const& source : row.sources) sources.try_emplace(source.input);
	for (auto const& request : row.requests) {
		requested_names.insert(request.ass_name);
		matched_names.insert(request.matched_name);
		matched_names.insert(request.matched_full_name);
		if (request.matched_weight > 0) matched_weights.insert(request.matched_weight);
		matched_paths.insert(request.paths.begin(), request.paths.end());
		auto& lines = sources[request.input];
		lines.insert(request.lines.begin(), request.lines.end());
	}
	reasons.erase("");
	requested_names.erase("");
	matched_names.erase("");
	matched_paths.erase("");
	sources.erase("");
	auto text = to_wx(row.display_name) + wxS("\n") + UsageCount(row.usage_count);
	if (!reasons.empty()) text += wxS("\n") + wxString::Format(_("Reasons: %s"), JoinDetails(reasons));
	if (!requested_names.empty()) text += wxS("\n") + wxString::Format(_("ASS requested names: %s"), JoinDetails(requested_names));
	if (!matched_names.empty()) {
		text += wxS("\n") + wxString::Format(provider == "gdi-dwrite" ? _("GDI returned/matched names: %s") : _("Matched names: %s"), JoinDetails(matched_names));
	}
	if (!matched_weights.empty()) {
		wxString weights;
		for (int weight : matched_weights) {
			if (!weights.empty()) weights += _("; ");
			weights += to_wx(std::to_string(weight));
		}
		text += wxS("\n") + wxString::Format(_("Matched font weights: %s"), weights);
	}
	if (!sources.empty()) {
		text += wxS("\n") + _("Sources:");
		for (auto const& [input, lines] : sources) {
			text += wxS("\n") + to_wx(input);
			if (lines.empty()) continue;
			wxString line_numbers;
			for (auto next = lines.begin(); next != lines.end();) {
				int first = *next;
				int last = first;
				++next;
				while (next != lines.end() && static_cast<int64_t>(*next) == static_cast<int64_t>(last) + 1)
					last = *next++;
				if (!line_numbers.empty()) line_numbers += _(", ");
				line_numbers += first == last ? to_wx(std::to_string(first)) : wxString::Format(_(L"%d–%d"), first, last);
			}
			text += wxString::Format(wxPLURAL(L" · Line %s", L" · Lines %s", lines.size()), line_numbers);
		}
	}
	if (!matched_paths.empty()) text += wxS("\n") + wxString::Format(_("Matched files: %s"), JoinDetails(matched_paths));
	return text;
}

void SetClientFont(wxWindow *window, wxFont const& font) {
	window->SetFont(font);
	for (auto *child : window->GetChildren()) SetClientFont(child, font);
}

}

struct DialogFontUsageInventory::Impl {
	struct VisibleRow {
		std::string key;
		bool reliable;
		wxPanel *panel;
		InventoryField *name;
		InventoryField *summary;
	};
	struct FieldSelection {
		std::string key;
		bool summary = false;
		wxString value;
		long start = 0;
		long end = 0;
		bool focused = false;
	};
	struct ScrollPosition {
		std::string key;
		int offset = 0;
		int absolute = 0;
	};
	struct InputRequest {
		std::vector<agi::fs::path> paths;
		uint64_t generation = 0;
	};
	struct InputValidation {
		std::vector<agi::fs::path> accepted;
		bool has_directory = false;
		size_t ignored = 0;
	};
	struct Completion {
		std::optional<font_usage_inventory::Result> result;
		std::optional<InputValidation> validation;
		std::string error;
		uint64_t input_generation = 0;
		uint64_t font_generation = 0;
		uint64_t validation_generation = 0;
		bool is_validation = false;
	};
	struct Receiver {
		std::atomic<bool> stopped = false;
		std::function<void(Completion)> publish;
	};

	DialogFontUsageInventory *window;
	wxStaticText *source;
	wxButton *choose;
	wxCheckBox *recursive;
	wxTextCtrl *threshold;
	wxSpinButton *threshold_spin;
	wxScrolledWindow *records;
	wxBoxSizer *record_rows;
	wxStaticText *result_status = nullptr;
	FontUsageInventoryWatcher watcher;
	agi::ui::UiActivationScope activation;
	std::shared_ptr<Receiver> receiver = std::make_shared<Receiver>();
	std::vector<agi::fs::path> inputs;
	std::optional<InputRequest> pending_inputs;
	std::optional<font_usage_inventory::Result> result;
	std::vector<VisibleRow> rows;
	std::optional<FieldSelection> retained_focus;
	wxString source_label;
	wxString scan_error;
	wxString watcher_error;
	wxString input_notice;
	uint64_t generation = 0;
	uint64_t validation_generation = 0;
	int valid_threshold = 5;
	bool has_directory = false;
	bool active = false;
	bool validating = false;
	bool dirty = false;
	bool stale = false;
	bool pending_retry = false;
	bool stopped = false;
	bool rendering = false;
	wxColour row_colours[4];
	wxTimer retry;
	wxTimer tip_delay;
	std::map<wxWindow *, wxString> tip_texts;
	wxWindow *tip_target = nullptr;
	wxTipWindow *borrowed_tip = nullptr;

	explicit Impl(DialogFontUsageInventory *owner)
		: window(owner), watcher([this] { RequestScan(); }, [this](std::string const& error) {
		watcher_error = to_wx(error);
		Render(); }), retry(window), tip_delay(window) {
		source_label = _("No source selected");
		source = new wxStaticText(window, wxID_ANY, source_label);
		source->SetMinSize(wxSize(1, -1));
		choose = new wxButton(window, wxID_ANY, _("Choose directory..."));
		recursive = new wxCheckBox(window, wxID_ANY, _("Recursive subdirectories"));
		recursive->SetValue(OPT_GET(recursive_option)->GetBool());
		recursive->Disable();
		auto configured_threshold = OPT_GET(threshold_option)->GetInt();
		if (configured_threshold >= 0 && configured_threshold <= std::numeric_limits<int>::max())
			valid_threshold = static_cast<int>(configured_threshold);
		threshold = new wxTextCtrl(window, wxID_ANY, wxString::Format(wxS("%d"), valid_threshold),
								   wxDefaultPosition, window->FromDIP(wxSize(105, -1)), wxTE_PROCESS_ENTER);
		threshold->SetName(wxS("Low-frequency threshold"));
		threshold_spin = new wxSpinButton(window, wxID_ANY, wxDefaultPosition,
										  wxDefaultSize, wxSP_VERTICAL);
		threshold_spin->SetRange(0, std::numeric_limits<int>::max());
		threshold_spin->SetValue(valid_threshold);
		SetTipText(source, {});
		SetTipText(threshold, {});
		records = new wxScrolledWindow(window, wxID_ANY);
		records->SetName(wxS("Font usage records"));
		records->SetScrollRate(0, window->FromDIP(10));
		record_rows = new wxBoxSizer(wxVERTICAL);
		records->SetSizer(record_rows);

		auto *source_row = new wxBoxSizer(wxHORIZONTAL);
		source_row->Add(source, wxSizerFlags(1).Align(wxALIGN_CENTER_VERTICAL).Border(wxRIGHT));
		source_row->Add(choose, wxSizerFlags().Align(wxALIGN_CENTER_VERTICAL));
		auto *threshold_row = new wxBoxSizer(wxHORIZONTAL);
		threshold_row->Add(new wxStaticText(window, wxID_ANY, _("Low-frequency threshold")),
						   wxSizerFlags().Align(wxALIGN_CENTER_VERTICAL).Border(wxRIGHT, 4));
		threshold_row->Add(threshold, wxSizerFlags().Align(wxALIGN_CENTER_VERTICAL));
		threshold_row->Add(threshold_spin, wxSizerFlags().Align(wxALIGN_CENTER_VERTICAL));
		auto *settings_row = new wxWrapSizer(wxHORIZONTAL);
		settings_row->Add(recursive, wxSizerFlags().Align(wxALIGN_CENTER_VERTICAL).Border(wxRIGHT));
		settings_row->Add(threshold_row, wxSizerFlags().Align(wxALIGN_CENTER_VERTICAL));
		auto *layout = new wxBoxSizer(wxVERTICAL);
		layout->Add(source_row, wxSizerFlags().Expand().Border());
		layout->Add(settings_row, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		layout->Add(records, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		window->SetSizer(layout);

		choose->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
			wxDirDialog dialog(window, _("Choose directory"));
			if (dialog.ShowModal() == wxID_OK) ReplaceInputs({dialog.GetPath()}, true);
		});
		recursive->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
			OPT_SET(recursive_option)->SetBool(recursive->GetValue());
			RestartSources();
		});
		threshold->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { CommitThreshold(); });
		threshold->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent& event) {
			CommitThreshold();
			event.Skip();
		});
		threshold_spin->Bind(wxEVT_SPIN, [this](wxSpinEvent& event) {
			if (!CommitThreshold()) {
				event.Veto();
				return;
			}
			threshold->ChangeValue(wxString::Format(wxS("%d"), event.GetPosition()));
			CommitThreshold();
		});
		window->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
			if (event.GetKeyCode() == WXK_F5) {
				watcher_error.clear();
				watcher.SetSources(inputs, Recursive());
				RequestScan();
			}
			else
				event.Skip();
		});
		window->Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
			pending_retry = false;
			RequestScan(false); }, retry.GetId());
		window->Bind(wxEVT_TIMER, &Impl::OnTipTimer, this, tip_delay.GetId());
		window->Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
			Stop();
			event.Skip();
		});
		window->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
			CloseTip();
			LayoutSource();
			Relayout();
			event.Skip();
		});
		records->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
			Relayout();
			event.Skip();
		});
		window->Bind(wxEVT_SHOW, &Impl::OnTipCancel, this);
		for (auto type : scroll_events) records->Bind(type, &Impl::OnTipCancel, this);
		records->Bind(wxEVT_MOUSEWHEEL, &Impl::OnTipCancel, this);
		records->Bind(wxEVT_CONTEXT_MENU, [this](wxContextMenuEvent&) {
			CloseTip();
			wxMenu menu;
			menu.Append(copy_names_id, _("Copy font names list"));
			menu.Append(copy_full_id, _("Copy full list"));
			menu.Enable(copy_names_id, CanCopyList());
			menu.Enable(copy_full_id, CanCopyList());
			records->PopupMenu(&menu);
		});
		records->Bind(wxEVT_MENU, [this](wxCommandEvent&) { CopyList(true); }, copy_names_id);
		records->Bind(wxEVT_MENU, [this](wxCommandEvent&) { CopyList(false); }, copy_full_id);
		AcceptDrops(window);
		AcceptDrops(records);
		activation.AddConnections(
			OPT_SUB("Subtitle/Edit Box/Font Face", &Impl::ApplyFont, this),
			OPT_SUB("Subtitle/Edit Box/Font Size", &Impl::ApplyFont, this),
			OPT_SUB("Colour/Subtitle/Background", &Impl::ApplyFont, this),
			OPT_SUB("Colour/Subtitle/Syntax/Normal", &Impl::ApplyFont, this));
		receiver->publish = [this](Completion completion) { Complete(std::move(completion)); };
		ApplyFont();
	}

	~Impl() {
		Stop();
	}

	void Stop() {
		if (stopped) return;
		stopped = true;
		CloseTip();
		window->Unbind(wxEVT_TIMER, &Impl::OnTipTimer, this, tip_delay.GetId());
		window->Unbind(wxEVT_SHOW, &Impl::OnTipCancel, this);
		for (auto type : scroll_events) records->Unbind(type, &Impl::OnTipCancel, this);
		records->Unbind(wxEVT_MOUSEWHEEL, &Impl::OnTipCancel, this);
		for (auto const& [target, text] : tip_texts) UnbindTip(target);
		tip_texts.clear();
		pending_inputs.reset();
		retry.Stop();
		watcher.Stop();
		receiver->stopped = true;
		receiver->publish = {};
		activation.Deactivate();
	}

	void OnTipTimer(wxTimerEvent&) {
		if (stopped || !tip_target) return;
		auto bounds = tip_target->GetScreenRect();
		if (!tip_target->IsShownOnScreen() || !bounds.Contains(wxGetMousePosition())) {
			CloseTip();
			return;
		}
		borrowed_tip = new wxTipWindow(tip_target, tip_texts.at(tip_target), window->FromDIP(600), &borrowed_tip, &bounds);
	}

	void CloseTip() {
		tip_delay.Stop();
		tip_target = nullptr;
		if (auto *shown = std::exchange(borrowed_tip, nullptr)) {
			shown->SetTipWindowPtr(nullptr);
			shown->Close();
		}
	}

	void OnTipEnter(wxMouseEvent& event) {
		CloseTip();
		auto *target = static_cast<wxWindow *>(event.GetEventObject());
		if (!stopped && !tip_texts.at(target).empty()) {
			tip_target = target;
			tip_delay.StartOnce(500);
		}
		event.Skip();
	}

	void OnTipLeave(wxMouseEvent& event) {
		if (event.GetEventObject() == tip_target && !tip_target->GetScreenRect().Contains(wxGetMousePosition())) CloseTip();
		event.Skip();
	}

	void OnTipCancel(wxEvent& event) {
		CloseTip();
		event.Skip();
	}

	void UnbindTip(wxWindow *target) {
		target->Unbind(wxEVT_ENTER_WINDOW, &Impl::OnTipEnter, this);
		target->Unbind(wxEVT_LEAVE_WINDOW, &Impl::OnTipLeave, this);
		target->Unbind(wxEVT_LEFT_DOWN, &Impl::OnTipCancel, this);
		target->Unbind(wxEVT_RIGHT_DOWN, &Impl::OnTipCancel, this);
	}

	void SetTipText(wxWindow *target, wxString text) {
		if (tip_target == target) CloseTip();
		target->SetHelpText(text);
		bool inserted = tip_texts.insert_or_assign(target, std::move(text)).second;
		if (!inserted) return;
		target->Bind(wxEVT_ENTER_WINDOW, &Impl::OnTipEnter, this);
		target->Bind(wxEVT_LEAVE_WINDOW, &Impl::OnTipLeave, this);
		target->Bind(wxEVT_LEFT_DOWN, &Impl::OnTipCancel, this);
		target->Bind(wxEVT_RIGHT_DOWN, &Impl::OnTipCancel, this);
	}

	[[nodiscard]] bool Recursive() const { return has_directory && recursive->GetValue(); }

	void AcceptDrops(wxWindow *target) {
		target->SetDropTarget(new InventoryDropTarget([this](wxArrayString const& names) { return ReplaceInputs(names); }));
#ifdef __WXMSW__
		target->DragAcceptFiles(true);
		target->Bind(wxEVT_DROP_FILES, [this](wxDropFilesEvent& event) {
			wxArrayString names;
			for (int i = 0; i < event.GetNumberOfFiles(); ++i) names.Add(event.GetFiles()[i]);
			ReplaceInputs(names);
		});
#endif
	}

	void LayoutSource() {
		auto *row = source->GetContainingSizer();
		int border = row->GetItem(source)->GetBorder();
		int margins = window->GetSizer()->GetItem(row)->GetBorder() * 2;
		int width = std::max(1, window->GetClientSize().x - choose->GetBestSize().x - margins - border);
		source->SetLabel(source_label);
		source->Wrap(width);
		window->Layout();
	}

	void ApplyFont() {
		auto font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
		auto face = FontFace("Subtitle/Edit Box");
		if (!face.empty()) font.SetFaceName(face);
		font.SetPointSize(static_cast<int>(OPT_GET("Subtitle/Edit Box/Font Size")->GetInt()));
		SetClientFont(window, font);
		auto background = to_wx(OPT_GET("Colour/Subtitle/Background")->GetColor());
		auto foreground = to_wx(OPT_GET("Colour/Subtitle/Syntax/Normal")->GetColor());
		bool dark = background.Red() * 299 + background.Green() * 587 + background.Blue() * 114 < 128000;
		row_colours[0] = foreground;
		row_colours[1] = dark ? wxColour(255, 115, 115) : wxColour(185, 0, 0);
		row_colours[2] = dark ? wxColour(245, 201, 80) : wxColour(136, 100, 0);
		row_colours[3] = dark ? wxColour(120, 180, 255) : wxColour(0, 75, 185);
		records->SetBackgroundColour(background);
		records->SetForegroundColour(foreground);
		threshold->SetMinSize(wxSize(threshold->GetTextExtent(wxS("2147483647")).x + window->FromDIP(20), -1));
		choose->InvalidateBestSize();
		recursive->InvalidateBestSize();
		LayoutSource();
		Render();
	}

	bool CommitThreshold() {
		auto value = from_wx(threshold->GetValue());
		int parsed = 0;
		auto conversion = std::from_chars(value.data(), value.data() + value.size(), parsed);
		bool digits = !value.empty() && std::ranges::all_of(value, [](char c) { return c >= '0' && c <= '9'; });
		if (!digits || conversion.ec != std::errc{} || conversion.ptr != value.data() + value.size() || parsed < 0) {
			threshold->ChangeValue(wxString::Format(wxS("%d"), valid_threshold));
			threshold_spin->SetValue(valid_threshold);
			threshold->SetBackgroundColour(wxColour(255, 220, 220));
			SetTipText(threshold, _("Enter a whole number from 0 to 2147483647. The last valid value was restored."));
			threshold->Refresh();
			return false;
		}
		threshold->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
		SetTipText(threshold, {});
		threshold->Refresh();
		threshold_spin->SetValue(parsed);
		if (valid_threshold == parsed) return true;
		valid_threshold = parsed;
		OPT_SET(threshold_option)->SetInt(parsed);
		if (result) font_usage_inventory::RecomputeThreshold(*result, parsed);
		Render();
		return true;
	}

	bool ReplaceInputs(wxArrayString const& names, bool known_directory = false) {
		if (stopped || names.empty()) return false;
		InputRequest request;
		request.generation = ++validation_generation;
		for (auto const& name : names) request.paths.emplace_back(name.wc_str());
		input_notice.clear();
		if (known_directory) {
			pending_inputs.reset();
			ApplyInputs({.accepted = std::move(request.paths), .has_directory = true});
		}
		else {
			pending_inputs = std::move(request);
			StartWork();
			Render();
		}
		return true;
	}

	void ApplyInputs(InputValidation validation) {
		inputs = std::move(validation.accepted);
		has_directory = validation.has_directory;
		recursive->Enable(has_directory);
		input_notice = validation.ignored ? wxString::Format(_("Ignored unsupported items: %zu"), validation.ignored) : wxString{};
		if (inputs.size() == 1)
			source_label = wxString(inputs.front().filename().wstring());
		else
			source_label = wxString::Format(_("Multiple sources (%zu items)"), inputs.size());
		if (source_label.empty() && inputs.size() == 1) source_label = wxString(inputs.front().wstring());
		LayoutSource();
		wxString tooltip;
		for (auto const& path : inputs) {
			if (!tooltip.empty()) tooltip += '\n';
			tooltip += wxString(path.wstring());
		}
		SetTipText(source, std::move(tooltip));
		RestartSources();
	}

	void StartValidation() {
		active = true;
		validating = true;
		auto request = std::move(*pending_inputs);
		pending_inputs.reset();
		auto state = receiver;
		auto lifetime = activation.GetLifetime();
		agi::dispatch::Background().Async([request = std::move(request), state, lifetime] {
			if (state->stopped) return;
			Completion completion;
			completion.is_validation = true;
			completion.validation_generation = request.generation;
			try {
				InputValidation validation;
				for (auto const& path : request.paths) {
					if (state->stopped) return;
					std::error_code error;
					bool directory = std::filesystem::is_directory(path, error);
					auto extension = path.extension().native();
					bool ass = extension.size() == 4 && extension[0] == '.' &&
							   (extension[1] == 'a' || extension[1] == 'A') &&
							   (extension[2] == 's' || extension[2] == 'S') &&
							   (extension[3] == 's' || extension[3] == 'S');
					if (directory || ass) {
						validation.accepted.push_back(path);
						validation.has_directory |= directory;
					}
					else
						++validation.ignored;
				}
				completion.validation = std::move(validation);
			}
			catch (std::exception const& error) {
				completion.error = error.what();
			}
			catch (...) {
				completion.error = "Unknown source inspection failure";
			}
			if (state->stopped) return;
			agi::ui::MainAsyncIfAlive(lifetime, [state, completion = std::move(completion)]() mutable {
				if (state->publish) state->publish(std::move(completion));
			});
		});
	}

	void StartWork() {
		if (stopped || active) return;
		if (pending_inputs)
			StartValidation();
		else if (dirty && !inputs.empty())
			StartScan();
	}

	void RestartSources() {
		++generation;
		retry.Stop();
		pending_retry = false;
		result.reset();
		scan_error.clear();
		watcher_error.clear();
		stale = false;
		watcher.SetSources(inputs, Recursive());
		RequestScan();
	}

	void RequestScan(bool allow_retry = true) {
		if (stopped || inputs.empty()) return;
		if (allow_retry) pending_retry = true;
		dirty = true;
		StartWork();
		Render();
	}

	void StartScan() {
		active = true;
		dirty = false;
		scan_error.clear();
		font_usage_inventory::Options options;
		options.inputs = inputs;
		options.recursive = Recursive();
		options.low_frequency_threshold = valid_threshold;
		auto input_generation = generation;
		auto font_generation = font_family_catalog_cache::GetGeneration();
		auto state = receiver;
		auto lifetime = activation.GetLifetime();
		agi::dispatch::Background().Async([options = std::move(options), state, lifetime, input_generation, font_generation] {
			if (state->stopped) return;
			Completion completion;
			completion.input_generation = input_generation;
			completion.font_generation = font_generation;
			try {
				completion.result = font_usage_inventory::Scan(options);
			}
			catch (agi::Exception const& error) {
				completion.error = error.GetMessage();
			}
			catch (std::exception const& error) {
				completion.error = error.what();
			}
			catch (...) {
				completion.error = "Unknown font analysis failure";
			}
			if (state->stopped) return;
			agi::ui::MainAsyncIfAlive(lifetime, [state, completion = std::move(completion)]() mutable {
				if (state->publish) state->publish(std::move(completion));
			});
		});
	}

	void Complete(Completion completion) {
		active = false;
		validating = false;
		if (completion.is_validation) {
			if (completion.validation_generation == validation_generation) {
				if (completion.validation && !completion.validation->accepted.empty())
					ApplyInputs(std::move(*completion.validation));
				else if (completion.validation)
					input_notice = _("No supported sources were dropped. Choose a directory or an ASS file.");
				else
					input_notice = _("Could not inspect dropped sources.") + wxS(" ") + to_wx(completion.error);
			}
			StartWork();
			Render();
			return;
		}
		bool current = completion.input_generation == generation &&
					   completion.font_generation == font_family_catalog_cache::GetGeneration();
		if (completion.result) current &= completion.result->catalog_generation == completion.font_generation;
		if (current) {
			if (completion.result) {
				result = std::move(completion.result);
				font_usage_inventory::RecomputeThreshold(*result, valid_threshold);
				stale = result->result != 0 && std::none_of(result->files.begin(), result->files.end(),
															[](auto const& file) { return file.status == "ok"; });
				if (watcher.UpdateDirectories(result->watch_directories)) dirty = true;
				if (watcher.IsAvailable()) watcher_error.clear();
				if (!result->complete && pending_retry && !dirty) {
					pending_retry = false;
					retry.StartOnce(600);
				}
			}
			else {
				scan_error = to_wx(completion.error);
				stale = true;
			}
		}
		else
			dirty = true;
		StartWork();
		Render();
	}

	[[nodiscard]] bool CanCopyList() const {
		return !stopped && !active && !stale && !validating && !pending_inputs && result && !result->rows.empty() &&
			   result->catalog_generation == font_family_catalog_cache::GetGeneration();
	}

	void CopyList(bool names_only) {
		if (!CanCopyList()) return;
#ifdef __WXMSW__
		auto newline = wxS("\r\n");
#else
		auto newline = wxS("\n");
#endif
		wxString text;
		for (auto const& row : result->rows) {
			if (!text.empty()) text += newline;
			text += to_wx(row.display_name);
			if (names_only) continue;
			text += wxS("\t") + UsageCount(row.usage_count);
			bool first = true;
			for (auto const& reason : row.reasons) {
				text += first ? wxString(wxS("\t")) : _("; ");
				text += ReasonText(reason, row);
				first = false;
			}
		}
		if (text.empty()) return;
		if (wxTheClipboard->Open()) {
			wxTheClipboard->SetData(new wxTextDataObject(text));
			wxTheClipboard->Close();
		}
	}

	[[nodiscard]] wxString Status() const {
		wxString status;
		if (validating || pending_inputs)
			status = _("Inspecting dropped sources...");
		else if (!scan_error.empty())
			status = _("Scan failed; any displayed results are outdated.") + wxS(" ") + scan_error;
		else if (active)
			status = result ? _("Updating...") : _("Scanning...");
		else if (result) {
			if (!result->complete)
				status = stale ? _("All sources failed; results are unavailable.") : _("Results are incomplete.");
			else if (result->files.empty())
				status = _("No ASS files found.");
			else if (result->rows.empty())
				status = _("No fonts are actually used.");
		}
		if (result && !result->complete) {
			for (auto const& diagnostic : result->diagnostics) {
				if (!status.empty()) status += '\n';
				if (!diagnostic.input.empty()) status += to_wx(diagnostic.input) + wxS(": ");
				status += to_wx(diagnostic.message);
			}
		}
		if (!watcher.IsAvailable() || !watcher_error.empty()) {
			if (!status.empty()) status += '\n';
			status += _("Automatic updates are unavailable. Press F5 to retry.");
			if (!watcher_error.empty()) status += wxS(" ") + watcher_error;
		}
		if (!input_notice.empty()) {
			if (!status.empty()) status += '\n';
			status += input_notice;
		}
		return status;
	}

	[[nodiscard]] std::vector<FieldSelection> CaptureSelections() const {
		std::vector<FieldSelection> selections;
		for (auto const& row : rows) {
			if (!row.reliable) continue;
			for (bool summary : {false, true}) {
				auto *field = summary ? row.summary : row.name;
				long start = 0;
				long end = 0;
				field->GetSelection(&start, &end);
				if (start != end || field->HasFocus())
					selections.push_back({.key = row.key, .summary = summary, .value = field->GetValue(), .start = start, .end = end, .focused = field->HasFocus()});
			}
		}
		return selections;
	}

	void RestoreSelections(std::vector<FieldSelection> const& selections) {
		for (auto const& selection : selections) {
			auto found = std::ranges::find(rows, selection.key, &VisibleRow::key);
			if (found == rows.end() || !found->reliable) continue;
			auto *field = selection.summary ? found->summary : found->name;
			if (field->GetValue() == selection.value) field->SetSelection(selection.start, selection.end);
		}
		if (!retained_focus || !records->HasFocus()) return;
		auto found = std::ranges::find(rows, retained_focus->key, &VisibleRow::key);
		if (found != rows.end() && found->reliable) {
			auto *field = retained_focus->summary ? found->summary : found->name;
			if (field->GetValue() != retained_focus->value)
				retained_focus.reset();
			else if (field->IsEnabled()) {
				field->SetFocus();
				field->SetSelection(retained_focus->start, retained_focus->end);
				retained_focus.reset();
			}
		}
		else
			retained_focus.reset();
	}

	[[nodiscard]] ScrollPosition CaptureScroll() const {
		int x = 0;
		int y = 0;
		records->CalcUnscrolledPosition(0, 0, &x, &y);
		ScrollPosition position{.absolute = y};
		for (auto const& row : rows) {
			int top = row.panel->GetPosition().y;
			if (top + row.panel->GetSize().y <= 0) continue;
			if (row.reliable) {
				position.key = row.key;
				position.offset = std::max(0, -top);
			}
			break;
		}
		return position;
	}

	void RestoreScroll(ScrollPosition const& position) {
		int y = position.absolute;
		auto found = std::ranges::find(rows, position.key, &VisibleRow::key);
		if (!position.key.empty() && found != rows.end() && found->reliable) {
			int x = 0;
			records->CalcUnscrolledPosition(0, found->panel->GetPosition().y, &x, &y);
			y += std::min(position.offset, found->panel->GetSize().y - 1);
		}
		int unit_x = 0;
		int unit_y = 0;
		records->GetScrollPixelsPerUnit(&unit_x, &unit_y);
		records->Scroll(0, y / std::max(1, unit_y));
	}

	void LayoutRows() {
		int summary_width = 1;
		for (auto const& row : rows)
			summary_width = std::max(summary_width, row.summary->GetSizeFromTextSize(row.summary->GetTextExtent(row.summary->GetValue())).x + window->FromDIP(6));
		int gap = window->FromDIP(8);
		int name_minimum = std::max(records->GetTextExtent(wxS("M")).x, records->GetTextExtent(wxString::FromUTF8("字")).x) + window->FromDIP(6);
		for (int pass = 0; pass < 2; ++pass) {
			int available = std::max(gap + 2, records->GetClientSize().x);
			int right = std::min(summary_width, std::max(1, available - gap - name_minimum));
			int left = std::max(1, available - gap - right);
			for (auto const& row : rows) {
				row.name->SetSize(0, 0, left, std::max(1, row.name->GetSize().y));
				row.summary->SetSize(left + gap, 0, right, std::max(1, row.summary->GetSize().y));
				int height = 1;
				for (auto *field : {row.name, row.summary}) {
					int text_height = std::max(1, field->GetNumberOfLines()) * field->GetCharHeight();
					height = std::max(height, field->GetSizeFromTextSize(0, text_height).y);
				}
				row.name->SetSize(left, height);
				row.summary->SetSize(right, height);
				row.panel->SetMinSize(wxSize(1, height));
			}
			if (result_status) {
				result_status->SetLabel(Status());
				result_status->Wrap(available);
			}
			records->Layout();
			records->FitInside();
			if (records->GetClientSize().x == available) break;
		}
	}

	void Relayout() {
		if (stopped || rendering) return;
		CloseTip();
		rendering = true;
		auto scroll = CaptureScroll();
		LayoutRows();
		RestoreScroll(scroll);
		rendering = false;
	}

	void Render() {
		if (stopped || rendering) return;
		CloseTip();
		rendering = true;
		auto selections = CaptureSelections();
		auto focused = std::ranges::find(selections, true, &FieldSelection::focused);
		if (focused != selections.end()) {
			retained_focus = *focused;
			records->SetFocusIgnoringChildren();
		}
		else if (!records->HasFocus())
			retained_focus.reset();
		if (!result) {
			selections.clear();
			retained_focus.reset();
		}
		auto scroll = CaptureScroll();
		records->Freeze();
		record_rows->Clear(false);
		for (auto const& row : rows) {
			for (auto *field : {row.name, row.summary}) {
				UnbindTip(field);
				tip_texts.erase(field);
			}
			row.panel->Hide();
			row.panel->CallAfter([panel = row.panel] { panel->Destroy(); });
		}
		rows.clear();
		if (auto *status = std::exchange(result_status, nullptr)) {
			status->Hide();
			status->CallAfter([status] { status->Destroy(); });
		}
		if (result) {
			for (auto const& font : result->rows) {
				auto *panel = new wxPanel(records, wxID_ANY);
				panel->SetName(wxS("Font usage row"));
				panel->SetBackgroundColour(records->GetBackgroundColour());
				auto make_field = [this, panel](wxString const& value, bool summary) {
					auto *field = new InventoryField(panel, value, summary, [this] { return CanCopyList(); }, [this](bool names) { CopyList(names); });
					field->SetFont(records->GetFont());
					field->SetBackgroundColour(records->GetBackgroundColour());
					field->Enable(CanCopyList());
					field->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
						auto scroll = event;
						scroll.SetEventObject(records);
						scroll.SetId(records->GetId());
						records->GetEventHandler()->ProcessEvent(scroll);
					});
					AcceptDrops(field);
					return field;
				};
				auto *name = make_field(to_wx(font.display_name), false);
				auto *summary = make_field(RowSuffix(font), true);
				auto tooltip = RowDetails(font, result->provider);
				for (auto *field : {name, summary}) {
					field->SetForegroundColour(row_colours[RowStyle(font)]);
					field->SetDetails(tooltip);
					SetTipText(field, tooltip);
				}
				AcceptDrops(panel);
				rows.push_back({.key = font.key, .reliable = font.selection_reliable, .panel = panel, .name = name, .summary = summary});
				record_rows->Add(panel, wxSizerFlags().Expand().Border(wxBOTTOM, window->FromDIP(3)));
			}
		}
		auto status = Status();
		if (!status.empty()) {
			result_status = new wxStaticText(records, wxID_ANY, status);
			result_status->SetName(wxS("Font usage status message"));
			result_status->SetFont(records->GetFont());
			result_status->SetForegroundColour(row_colours[0]);
			result_status->SetMinSize(wxSize(1, -1));
			record_rows->Add(result_status, wxSizerFlags().Expand());
			AcceptDrops(result_status);
		}
		LayoutRows();
		RestoreSelections(selections);
		RestoreScroll(scroll);
		records->Thaw();
		rendering = false;
	}
	void SystemFontsChanged() {
		if (inputs.empty()) return;
		++generation;
		stale = true;
		RequestScan();
	}
};

DialogFontUsageInventory::DialogFontUsageInventory(agi::Context *context)
	: wxDialog(context->GetUI().parent, wxID_ANY, _("Font Usage List"),
			   wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
	  impl(std::make_unique<Impl>(this)) {
	SetIcon(GETICON(font_collector_button_16));
	SetMinSize(FromDIP(wxSize(360, 240)));
	SetSize(FromDIP(wxSize(600, 560)));
	CenterOnParent();
}

DialogFontUsageInventory::~DialogFontUsageInventory() = default;

void DialogFontUsageInventory::NotifySystemFontsChanged() {
	impl->SystemFontsChanged();
}

void ShowFontUsageInventoryDialog(agi::Context *context) {
	context->GetUI().dialog->ShowOnce<DialogFontUsageInventory>(context);
}

void NotifyFontUsageInventorySystemFontsChanged(agi::Context *context) {
	if (auto *dialog = context->GetUI().dialog->Get<DialogFontUsageInventory>())
		dialog->NotifySystemFontsChanged();
}
