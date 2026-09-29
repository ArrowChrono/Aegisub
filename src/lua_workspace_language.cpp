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
#include <ranges>
#include <set>
#include <utility>

using namespace Automation4;

namespace {
constexpr int language_indicator = 9;

bool HostNameStart(int character) {
	return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_';
}

bool HostNamePart(int character) {
	return HostNameStart(character) || (character >= '0' && character <= '9');
}

int SkipHostSpace(wxStyledTextCtrl *editor, int end) {
	while (end > 0) {
		auto character = editor->GetCharAt(end - 1);
		if (character != ' ' && character != '\t' && character != '\r' && character != '\n' && character != '\v' && character != '\f')
			break;
		--end;
	}
	return end;
}

bool IsLuaStringStyle(int style) {
	switch (style) {
		case wxSTC_LUA_STRING:
		case wxSTC_LUA_CHARACTER:
		case wxSTC_LUA_LITERALSTRING:
		case wxSTC_LUA_STRINGEOL:
			return true;
		default:
			return false;
	}
}

bool IsLuaTextStyle(int style) {
	return style == wxSTC_LUA_COMMENT || style == wxSTC_LUA_COMMENTLINE || style == wxSTC_LUA_COMMENTDOC || IsLuaStringStyle(style);
}

bool CanPairAt(wxStyledTextCtrl *editor, int position) {
	if (position <= 0)
		return true;
	editor->Colourise(0, position);
	auto style = editor->GetStyleAt(position - 1);
	auto previous = editor->GetCharAt(position - 1);
	if ((previous == '\r' || previous == '\n') && (style == wxSTC_LUA_COMMENTLINE || style == wxSTC_LUA_STRINGEOL))
		return true;
	return !IsLuaTextStyle(style);
}

bool CanSkipClosingAt(wxStyledTextCtrl *editor, int position) {
	editor->Colourise(0, position + 1);
	return !IsLuaTextStyle(editor->GetStyleAt(position));
}
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
	editor->Bind(wxEVT_CHAR, &LuaWorkspaceLanguage::OnChar, this);
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
	editor->Unbind(wxEVT_CHAR, &LuaWorkspaceLanguage::OnChar, this);
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
	completion_refresh_pending = !applying && active && document && document->identity == completion_identity && (completion_request != 0 || completion_refresh_pending);
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
	bool changed = value ? !document || document->identity != value->GetSourceIdentity() || document->revision != value->GetRevision() || document->source != value->GetSource() || document->filename != value->GetFilename() || scopes != code_scopes
						 : document.has_value();
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
			host_hints = std::move(environment.host_hints);
			scopes = code_scopes;
		}
	}
	Synchronize(changed);
}

void LuaWorkspaceLanguage::SetActive(bool value) {
	bool changed = active != value;
	active = value;
	Synchronize(changed);
}

void LuaWorkspaceLanguage::Synchronize(bool document_changed) {
	try {
		bool previous_enabled = enabled;
		auto previous_directory = directory_option;
		enabled = OPT_GET("Automation/Lua Workspace/Enable LuaLS")->GetBool();
		directory_option = OPT_GET("Automation/Lua Workspace/LuaLS Directory")->GetString();
		if (previous_enabled != enabled || previous_directory != directory_option || !active || !document)
			local_available = false;
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
		configuration_error = false;
		if (document_changed || next != generation) {
			generation = next;
			Invalidate();
			status->SetLabel(!enabled ? _("LuaLS: disabled; local host hints (limited)") : !active || !document ? _("LuaLS: inactive")
																												: _("LuaLS: synchronizing..."));
			status->SetToolTip(status->GetLabel());
			if (!enabled && active && document)
				diagnostics->ChangeValue(_("LuaLS diagnostics unavailable; local host hints are limited."));
		}
	}
	catch (std::exception const& error) {
		generation = server.Update({}, std::nullopt);
		Invalidate();
		local_available = true;
		configuration_error = true;
		status->SetLabel(_("LuaLS configuration error: ") + to_wx(error.what()) + _("; local host hints (limited)"));
		status->SetToolTip(status->GetLabel());
		diagnostics->ChangeValue(_("LuaLS diagnostics unavailable; local host hints are limited."));
	}
}

std::string LuaWorkspaceLanguage::HostReceiver(int end) const {
	std::string path;
	int cursor = SkipHostSpace(editor, end) - 1;
	while (cursor >= 0 && editor->GetCharAt(cursor) == '.') {
		int segment_end = SkipHostSpace(editor, cursor);
		cursor = segment_end - 1;
		while (cursor >= 0 && HostNamePart(editor->GetCharAt(cursor)))
			--cursor;
		int start = cursor + 1;
		if (start == segment_end || !HostNameStart(editor->GetCharAt(start)) || IsLuaTextStyle(editor->GetStyleAt(start)))
			return {};
		auto segment = from_wx(editor->GetTextRange(start, segment_end));
		if (path.empty())
			path = std::move(segment);
		else {
			path.insert(0, ".");
			path.insert(0, segment);
		}
		cursor = SkipHostSpace(editor, start) - 1;
	}
	return path;
}

int LuaWorkspaceLanguage::HostCallOpen(int position) const {
	std::string closers;
	for (int cursor = position - 1; cursor >= std::max(0, position - 8192); --cursor) {
		if (IsLuaTextStyle(editor->GetStyleAt(cursor)))
			continue;
		int character = editor->GetCharAt(cursor);
		if (character == ')' || character == ']' || character == '}')
			closers.push_back(static_cast<char>(character));
		else if (character == '(' || character == '[' || character == '{') {
			if (closers.empty())
				return character == '(' ? cursor : -1;
			char expected = character == '(' ? ')' : character == '[' ? ']'
																	  : '}';
			if (closers.back() != expected)
				return -1;
			closers.pop_back();
		}
	}
	return -1;
}

void LuaWorkspaceLanguage::PresentCompletion(LuaLanguageEvent event) {
	if (event.request != completion_request || !std::cmp_equal(event.position, editor->GetCurrentPos()) || selection_start != editor->GetSelectionStart() || selection_end != editor->GetSelectionEnd())
		return;
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

void LuaWorkspaceLanguage::PresentTip(LuaLanguageEvent const& event) {
	if (event.text.empty() || editor->AutoCompActive() || (tip_kind == LuaLanguageRequest::Signature && !std::cmp_equal(event.position, editor->GetCurrentPos())))
		ClearTip();
	else
		editor->CallTipShow(static_cast<int>(event.position), to_wx(event.text));
}

void LuaWorkspaceLanguage::Request(LuaLanguageRequest kind, int position) {
	if (!active || !document || position < 0 || applying)
		return;
	int start = editor->GetSelectionStart(), end = editor->GetSelectionEnd();
	bool had_selection = start != end;
	if (kind != LuaLanguageRequest::Completion)
		start = end = position;
	else if (!had_selection)
		start = editor->WordStartPosition(position, true);
	bool local = !enabled || local_available;
	auto id = local ? ++local_request_id : server.Request(kind, position, start, end);
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
	if (!local)
		return;
	auto reject = [&] {
		if (kind == LuaLanguageRequest::Completion)
			completion_request = 0;
		else
			ClearTip();
	};
	LuaLanguageEvent event{.kind = kind == LuaLanguageRequest::Completion ? LuaLanguageEvent::Kind::Completion : kind == LuaLanguageRequest::Signature ? LuaLanguageEvent::Kind::Signature
																																					   : LuaLanguageEvent::Kind::Hover,
						   .generation = generation,
						   .request = id,
						   .position = static_cast<std::size_t>(position)};
	if (kind == LuaLanguageRequest::Hover) {
		if (position >= editor->GetTextLength()) {
			reject();
			return;
		}
		editor->Colourise(0, position + 1);
		if (IsLuaTextStyle(editor->GetStyleAt(position))) {
			reject();
			return;
		}
	}
	else if (position > 0) {
		editor->Colourise(0, position);
		if (IsLuaTextStyle(editor->GetStyleAt(position - 1))) {
			reject();
			return;
		}
	}
	if (kind == LuaLanguageRequest::Completion) {
		auto receiver = HostReceiver(start);
		if (receiver.empty()) {
			reject();
			return;
		}
		auto prefix = had_selection ? std::string{} : from_wx(editor->GetTextRange(start, position));
		for (auto const& hint : host_hints.Members(receiver)) {
			if (!hint.name.starts_with(prefix))
				continue;
			LuaLanguageCompletion item;
			item.label = hint.name;
			item.detail = hint.type;
			item.caret = static_cast<std::size_t>(start) + hint.name.size();
			item.edits.push_back({.start = static_cast<std::size_t>(start), .end = static_cast<std::size_t>(end), .text = hint.name});
			event.completions.push_back(std::move(item));
		}
		PresentCompletion(std::move(event));
		return;
	}
	int word_end = position;
	if (kind == LuaLanguageRequest::Signature) {
		word_end = HostCallOpen(position);
		if (word_end < 0) {
			reject();
			return;
		}
		word_end = SkipHostSpace(editor, word_end);
	}
	else
		while (word_end < editor->GetTextLength() && HostNamePart(editor->GetCharAt(word_end)))
			++word_end;
	int word_start = word_end;
	while (word_start > 0 && HostNamePart(editor->GetCharAt(word_start - 1)))
		--word_start;
	if (word_start == word_end || !HostNameStart(editor->GetCharAt(word_start))) {
		reject();
		return;
	}
	auto name = from_wx(editor->GetTextRange(word_start, word_end));
	auto receiver = HostReceiver(word_start);
	if (receiver.empty()) {
		if (kind == LuaLanguageRequest::Hover) {
			if (auto root = host_hints.roots.find(name); root != host_hints.roots.end() && host_hints.classes.contains(root->second)) {
				event.text = "Local host hint (limited)\n";
				event.text += name;
				event.text += ": ";
				event.text += root->second;
				PresentTip(event);
				return;
			}
		}
		reject();
		return;
	}
	auto hint = host_hints.Find(receiver, name);
	if (!hint || (kind == LuaLanguageRequest::Signature && !hint->type.starts_with("fun("))) {
		reject();
		return;
	}
	event.text = "Local host hint (limited)\n" + receiver + "." + name + (hint->optional ? "?" : "") + ": " + hint->type;
	PresentTip(event);
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

void LuaWorkspaceLanguage::OnChar(wxKeyEvent& event) {
	int character = event.GetUnicodeKey();
	if (character <= 127)
		character = event.GetKeyCode();
	bool text_modifier = (!event.ControlDown() && !event.AltDown()) || (event.ControlDown() && event.AltDown());
	if (character != WXK_NONE && text_modifier && !event.MetaDown() && !editor->GetReadOnly() && editor->GetSelectionEmpty()) {
		auto position = editor->GetCurrentPos();
		auto quote = character == '\'' || character == '"';
		auto closing_character = character == ')' || character == ']' || character == '}';
		editor->Colourise(0, position);
		if ((closing_character || quote) && editor->GetCharAt(position) == character &&
			((closing_character && CanSkipClosingAt(editor, position)) || (quote && position > 0 && IsLuaStringStyle(editor->GetStyleAt(position - 1))))) {
			editor->GotoPos(position + 1);
			return;
		}
		char closing = character == '(' ? ')' : character == '[' ? ']'
											: character == '{'   ? '}'
											: character == '\''  ? '\''
											: character == '"'   ? '"'
																 : 0;
		if (closing && CanPairAt(editor, position)) {
			char pair[]{static_cast<char>(character), closing, 0};
			editor->BeginUndoAction();
			editor->ReplaceSelection(to_wx(pair));
			editor->GotoPos(position + 1);
			editor->EndUndoAction();
			AfterCharacter(character);
			return;
		}
	}
	event.Skip();
}

void LuaWorkspaceLanguage::AfterCharacter(int character) {
	if (completion_refresh_pending) {
		completion_position = editor->GetCurrentPos();
		selection_start = editor->GetSelectionStart();
		selection_end = editor->GetSelectionEnd();
	}
	if (character == '.' || character == ':')
		Request(LuaLanguageRequest::Completion, editor->GetCurrentPos());
	else if (character == '(' || character == ',')
		Request(LuaLanguageRequest::Signature, editor->GetCurrentPos());
}

void LuaWorkspaceLanguage::OnCharacter(wxStyledTextEvent& event) {
	AfterCharacter(event.GetKey());
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
		if (configuration_error)
			continue;
		if (event.generation != generation || !active || !enabled || !document)
			continue;
		if (event.kind == LuaLanguageEvent::Kind::Status) {
			bool next_local = event.text.starts_with("LuaLS unavailable:");
			bool was_local = local_available;
			if (local_available && !next_local) {
				completion_request = 0;
				completion_refresh_pending = false;
				completion.reset();
				editor->AutoCompCancel();
				ClearTip();
			}
			local_available = next_local;
			auto status_text = to_wx(event.text);
			if (local_available)
				status_text += _("; local host hints (limited)");
			status->SetLabel(status_text);
			status->SetToolTip(status_text);
			if (!event.ready && (!next_local || !was_local)) {
				completion.reset();
				editor->AutoCompCancel();
				editor->CallTipCancel();
				ClearDiagnostics();
				if (next_local)
					diagnostics->ChangeValue(_("LuaLS diagnostics unavailable; local host hints are limited."));
			}
		}
		else if (event.kind == LuaLanguageEvent::Kind::Diagnostics) {
			ClearDiagnostics();
			wxString text = to_wx(event.text);
			for (auto const& diagnostic : event.diagnostics) {
				if (std::cmp_greater(diagnostic.end, editor->GetTextLength()))
					continue;
				if (!text.empty())
					text += wxS("\n");
				text += wxString::Format(wxS("%d: "), editor->LineFromPosition(static_cast<int>(diagnostic.start)) + 1) + to_wx(diagnostic.message);
				editor->SetIndicatorCurrent(language_indicator);
				editor->IndicatorFillRange(static_cast<int>(diagnostic.start), static_cast<int>(diagnostic.end - diagnostic.start));
			}
			diagnostics->ChangeValue(text.empty() ? _("LuaLS reported no diagnostics for this buffer version.") : text);
		}
		else if (event.kind == LuaLanguageEvent::Kind::Completion && !local_available)
			PresentCompletion(std::move(event));
		else if ((event.kind == LuaLanguageEvent::Kind::Hover || event.kind == LuaLanguageEvent::Kind::Signature) && tip_request && event.request == tip_request) {
			if (!local_available)
				PresentTip(event);
		}
	}
}
