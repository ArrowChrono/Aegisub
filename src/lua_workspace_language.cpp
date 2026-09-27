#include "lua_workspace_language.h"

#include "automation/lua_language_environment.h"
#include "automation/lua_workspace_document.h"
#include "compat.h"
#include "options.h"

#include <libaegisub/fs.h>
#include <libaegisub/path.h>
#include <libaegisub/split.h>
#include <libaegisub/string_utils.h>

#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stc/stc.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <limits>
#include <set>
#include <ranges>
#include <utility>

using namespace Automation4;

namespace {
constexpr int language_indicator = 9;
}

LuaWorkspaceLanguage::LuaWorkspaceLanguage(wxStyledTextCtrl *editor, wxNotebook *notebook)
	: editor(editor), timer(this) {
	auto panel = new wxPanel(notebook);
	auto layout = new wxBoxSizer(wxVERTICAL);
	status = new wxStaticText(panel, wxID_ANY, _("LuaLS: inactive"), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
	status->SetName(wxS("Lua language status"));
	auto help = new wxStaticText(panel, wxID_ANY, _("Ctrl+Space: completion\nCtrl+Shift+Space: signature\nHover over a symbol for documentation."));
	diagnostics = new wxTextCtrl(panel, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
	diagnostics->SetName(wxS("Lua language diagnostics"));
	layout->Add(status, 0, wxEXPAND | wxALL, 4);
	layout->Add(help, 0, wxEXPAND | wxALL, 4);
	layout->Add(diagnostics, 1, wxEXPAND | wxALL, 4);
	panel->SetSizer(layout);
	notebook->AddPage(panel, _("Language"));
	editor->IndicatorSetStyle(language_indicator, wxSTC_INDIC_SQUIGGLE);
	editor->IndicatorSetForeground(language_indicator, wxColour(195, 130, 20));
	editor->IndicatorSetUnder(language_indicator, true);
	editor->AutoCompSetSeparator('\n');
	editor->AutoCompSetTypeSeparator('\t');
	editor->AutoCompSetChooseSingle(false);
	editor->AutoCompSetAutoHide(false);
	editor->AutoCompSetMaxHeight(12);
	editor->SetMouseDwellTime(500);
	editor->Bind(wxEVT_CHAR_HOOK, &LuaWorkspaceLanguage::OnCharHook, this);
	editor->Bind(wxEVT_STC_CHARADDED, &LuaWorkspaceLanguage::OnCharacter, this);
	editor->Bind(wxEVT_STC_AUTOCOMP_SELECTION, &LuaWorkspaceLanguage::OnSelection, this);
	editor->Bind(wxEVT_STC_UPDATEUI, &LuaWorkspaceLanguage::OnUpdateUI, this);
	editor->Bind(wxEVT_STC_DWELLSTART, &LuaWorkspaceLanguage::OnDwellStart, this);
	editor->Bind(wxEVT_STC_DWELLEND, &LuaWorkspaceLanguage::OnDwellEnd, this);
	Bind(wxEVT_TIMER, &LuaWorkspaceLanguage::OnTimer, this);
	timer.Start(100);
}

LuaWorkspaceLanguage::~LuaWorkspaceLanguage() {
	timer.Stop();
	editor->Unbind(wxEVT_CHAR_HOOK, &LuaWorkspaceLanguage::OnCharHook, this);
	editor->Unbind(wxEVT_STC_CHARADDED, &LuaWorkspaceLanguage::OnCharacter, this);
	editor->Unbind(wxEVT_STC_AUTOCOMP_SELECTION, &LuaWorkspaceLanguage::OnSelection, this);
	editor->Unbind(wxEVT_STC_UPDATEUI, &LuaWorkspaceLanguage::OnUpdateUI, this);
	editor->Unbind(wxEVT_STC_DWELLSTART, &LuaWorkspaceLanguage::OnDwellStart, this);
	editor->Unbind(wxEVT_STC_DWELLEND, &LuaWorkspaceLanguage::OnDwellEnd, this);
}

void LuaWorkspaceLanguage::ClearDiagnostics() {
	editor->SetIndicatorCurrent(language_indicator);
	editor->IndicatorClearRange(0, editor->GetTextLength());
	diagnostics->ChangeValue(_("No version-confirmed diagnostics for this buffer."));
}

void LuaWorkspaceLanguage::ClearTip() {
	tip_request = 0;
	editor->CallTipCancel();
}

void LuaWorkspaceLanguage::Invalidate() {
	completion_refresh_pending = !applying && active && enabled && document && document->identity == completion_identity && (completion_request != 0 || completion_refresh_pending);
	if (completion_refresh_pending) {
		completion_position = editor->GetCurrentPos();
		selection_start = editor->GetSelectionStart();
		selection_end = editor->GetSelectionEnd();
	}
	completion_request = 0;
	ClearTip();
	completion.reset();
	editor->AutoCompCancel();
	ClearDiagnostics();
}

void LuaWorkspaceLanguage::Update(LuaWorkspaceDocument const *value, unsigned code_scopes) {
	if (!value) {
		document.reset();
		scopes.reset();
	}
	else {
		if (!document)
			document.emplace();
		document->identity = value->GetSourceIdentity();
		document->filename = value->GetFilename();
		document->source = value->GetSource();
		document->revision = value->GetRevision();
		if (scopes != code_scopes) {
			auto environment = BuildLuaLanguageEnvironment(code_scopes);
			document->definitions = std::move(environment.definitions);
			document->disabled_builtins = std::move(environment.disabled_builtins);
			scopes = code_scopes;
		}
	}
	Synchronize();
}

void LuaWorkspaceLanguage::SetActive(bool value) {
	active = value;
	Synchronize();
}

void LuaWorkspaceLanguage::Synchronize() {
	try {
		enabled = OPT_GET("Automation/Lua Workspace/Enable LuaLS")->GetBool();
		directory_option = OPT_GET("Automation/Lua Workspace/LuaLS Directory")->GetString();
		includes_option = OPT_GET("Path/Automation/Include")->GetString();
		configuration.directory = config::path->Decode(directory_option);
		configuration.cache_directory = config::path->Decode("?local/lua-workspace/luals");
		configuration.include_directories.clear();
		if (document && !document->filename.empty()) {
			for (auto token : agi::Split(includes_option, '|')) {
				auto path = config::path->Decode(agi::str(token));
				if (path.is_absolute())
					configuration.include_directories.push_back(std::move(path));
			}
		}
		auto next = server.Update(configuration, active && enabled ? document : std::nullopt);
		if (next != generation) {
			generation = next;
			Invalidate();
			status->SetLabel(!enabled ? _("LuaLS: disabled") : !active || !document ? _("LuaLS: inactive")
																					: _("LuaLS: synchronizing..."));
		}
	}
	catch (std::exception const& error) {
		generation = server.Update({}, std::nullopt);
		Invalidate();
		status->SetLabel(_("LuaLS configuration error: ") + to_wx(error.what()));
	}
}

void LuaWorkspaceLanguage::Request(LuaLanguageRequest kind, int position) {
	if (!active || !enabled || !document || position < 0 || applying)
		return;
	int start = editor->GetSelectionStart(), end = editor->GetSelectionEnd();
	if (kind != LuaLanguageRequest::Completion)
		start = end = position;
	else if (start == end)
		start = editor->WordStartPosition(position, true);
	auto id = server.Request(kind, position, start, end);
	ClearTip();
	if (kind == LuaLanguageRequest::Completion) {
		completion_refresh_pending = false;
		completion_request = id;
		completion_identity = document->identity;
		completion_position = position;
		selection_start = editor->GetSelectionStart();
		selection_end = editor->GetSelectionEnd();
	}
	else {
		tip_request = id;
		tip_kind = kind;
		tip_position = position;
	}
}

void LuaWorkspaceLanguage::OnCharHook(wxKeyEvent& event) {
	if (wxWindow::FindFocus() != editor) {
		event.Skip();
		return;
	}
	if (event.ControlDown() && !event.AltDown() && event.GetKeyCode() == WXK_SPACE) {
		Request(event.ShiftDown() ? LuaLanguageRequest::Signature : LuaLanguageRequest::Completion, editor->GetCurrentPos());
		return;
	}
	if (event.GetKeyCode() == WXK_ESCAPE) {
		completion_refresh_pending = false;
		completion_request = 0;
		ClearTip();
		completion.reset();
		editor->AutoCompCancel();
	}
	event.Skip();
}

void LuaWorkspaceLanguage::OnCharacter(wxStyledTextEvent& event) {
	if (completion_refresh_pending) {
		completion_position = editor->GetCurrentPos();
		selection_start = editor->GetSelectionStart();
		selection_end = editor->GetSelectionEnd();
	}
	if (event.GetKey() == '.' || event.GetKey() == ':')
		Request(LuaLanguageRequest::Completion, editor->GetCurrentPos());
	else if (event.GetKey() == '(' || event.GetKey() == ',')
		Request(LuaLanguageRequest::Signature, editor->GetCurrentPos());
	event.Skip();
}

void LuaWorkspaceLanguage::OnDwellStart(wxStyledTextEvent& event) {
	if (!editor->AutoCompActive() && !(tip_request && tip_kind == LuaLanguageRequest::Signature))
		Request(LuaLanguageRequest::Hover, event.GetPosition());
	event.Skip();
}

void LuaWorkspaceLanguage::OnDwellEnd(wxStyledTextEvent& event) {
	if (tip_request && tip_kind == LuaLanguageRequest::Hover)
		ClearTip();
	event.Skip();
}

void LuaWorkspaceLanguage::OnUpdateUI(wxStyledTextEvent& event) {
	if (tip_request && tip_kind == LuaLanguageRequest::Signature && (tip_position != editor->GetCurrentPos() || !editor->GetSelectionEmpty()))
		ClearTip();
	if ((completion_request || completion_refresh_pending) && (completion_position != editor->GetCurrentPos() || selection_start != editor->GetSelectionStart() || selection_end != editor->GetSelectionEnd())) {
		completion_refresh_pending = false;
		completion.reset();
		completion_request = 0;
		editor->AutoCompCancel();
	}
	event.Skip();
}

void LuaWorkspaceLanguage::OnSelection(wxStyledTextEvent& event) {
	auto result = std::move(completion);
	editor->AutoCompCancel();
	if (!result || result->generation != generation || result->request != completion_request || !std::cmp_equal(result->position, editor->GetCurrentPos()) || selection_start != editor->GetSelectionStart() || selection_end != editor->GetSelectionEnd())
		return;
	auto label = from_wx(event.GetText());
	auto found = std::ranges::find(result->completions, label, &LuaLanguageCompletion::label);
	if (found == result->completions.end())
		return;
	for (auto const& edit : found->edits)
		if (edit.start > edit.end || std::cmp_greater(edit.end, editor->GetTextLength()) || std::cmp_greater(edit.text.size(), std::numeric_limits<int>::max()))
			return;
	applying = true;
	editor->BeginUndoAction();
	for (auto const& edit : found->edits | std::views::reverse) {
		editor->SetTargetStart(static_cast<int>(edit.start));
		editor->SetTargetEnd(static_cast<int>(edit.end));
		editor->ReplaceTarget(to_wx(edit.text));
	}
	editor->EndUndoAction();
	editor->GotoPos(static_cast<int>(std::min(found->caret, static_cast<std::size_t>(editor->GetTextLength()))));
	applying = false;
	completion_request = 0;
	completion_refresh_pending = false;
}

void LuaWorkspaceLanguage::OnTimer(wxTimerEvent&) {
	if (enabled != OPT_GET("Automation/Lua Workspace/Enable LuaLS")->GetBool() || directory_option != OPT_GET("Automation/Lua Workspace/LuaLS Directory")->GetString() || includes_option != OPT_GET("Path/Automation/Include")->GetString())
		Synchronize();
	if (completion_refresh_pending && wxWindow::FindFocus() == editor) {
		auto position = editor->GetCurrentPos();
		auto character = position > 0 ? editor->GetCharAt(position - 1) : 0;
		if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') || character >= 128 || character == '_' || character == '.' || character == ':')
			Request(LuaLanguageRequest::Completion, position);
		else
			completion_refresh_pending = false;
	}
	for (auto& event : server.Drain()) {
		if (event.generation != generation || !active || !enabled || !document)
			continue;
		if (event.kind == LuaLanguageEvent::Kind::Status) {
			status->SetLabel(to_wx(event.text));
			status->SetToolTip(to_wx(event.text));
			if (!event.ready) {
				completion.reset();
				editor->AutoCompCancel();
				editor->CallTipCancel();
				ClearDiagnostics();
			}
		}
		else if (event.kind == LuaLanguageEvent::Kind::Diagnostics) {
			ClearDiagnostics();
			wxString text = to_wx(event.text);
			for (auto const& diagnostic : event.diagnostics) {
				if (std::cmp_greater(diagnostic.end, editor->GetTextLength()))
					continue;
				if (!text.empty())
					text += wxS("\n\n");
				text += wxString::Format(wxS("%d: "), editor->LineFromPosition(static_cast<int>(diagnostic.start)) + 1) + to_wx(diagnostic.message);
				editor->SetIndicatorCurrent(language_indicator);
				editor->IndicatorFillRange(static_cast<int>(diagnostic.start), static_cast<int>(diagnostic.end - diagnostic.start));
			}
			diagnostics->ChangeValue(text.empty() ? _("LuaLS reported no diagnostics for this buffer version.") : text);
		}
		else if (event.kind == LuaLanguageEvent::Kind::Completion) {
			if (event.request != completion_request || !std::cmp_equal(event.position, editor->GetCurrentPos()) || selection_start != editor->GetSelectionStart() || selection_end != editor->GetSelectionEnd())
				continue;
			std::set<std::string> labels;
			std::erase_if(event.completions, [&](auto const& item) { return !labels.insert(item.label).second; });
			wxString list;
			for (auto const& item : event.completions) {
				if (!list.empty())
					list += wxS("\n");
				list += to_wx(item.label);
			}
			if (!list.empty()) {
				completion = std::move(event);
				editor->AutoCompShow(0, list);
			}
			else
				completion_request = 0;
		}
		else if ((event.kind == LuaLanguageEvent::Kind::Hover || event.kind == LuaLanguageEvent::Kind::Signature) && tip_request && event.request == tip_request) {
			if (event.text.empty() || editor->AutoCompActive() || (tip_kind == LuaLanguageRequest::Signature && !std::cmp_equal(event.position, editor->GetCurrentPos())))
				ClearTip();
			else
				editor->CallTipShow(static_cast<int>(event.position), to_wx(event.text));
		}
	}
}
