#include "lua_workspace_frame.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "selection_controller.h"
#include "subs_controller.h"

#include <libaegisub/fs.h>
#include <libaegisub/scope_exit.h>

#include <wx/button.h>
#include <wx/choicdlg.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/filedlg.h>
#include <wx/font.h>
#include <wx/intl.h>
#include <wx/msgdlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stc/stc.h>

#include <algorithm>
#include <utility>

using namespace Automation4;

namespace {
constexpr int diagnostic_indicator = 8;
}

LuaWorkspaceFrame::LuaWorkspaceFrame(agi::Context *value)
	: wxFrame(value ? value->GetUI().parent : nullptr, wxID_ANY, wxS("Lua Workspace"), wxDefaultPosition, wxSize(1000, 720)), context(value) {
	SetName(wxS("Lua Workspace"));
	auto panel = new wxPanel(this);
	auto layout = new wxBoxSizer(wxVERTICAL);
	auto actions = new wxBoxSizer(wxHORIZONTAL);
	auto open = new wxButton(panel, wxID_OPEN, _("Open File"));
	apply = new wxButton(panel, wxID_SAVE, _("Apply"));
	format = new wxButton(panel, wxID_ANY, _("Format"));
	reload = new wxButton(panel, wxID_ANY, _("Reload"));
	auto copy = new wxButton(panel, wxID_COPY, _("Copy source"));
	open->SetName(wxS("Open File"));
	apply->SetName(wxS("Apply"));
	format->SetName(wxS("Format"));
	reload->SetName(wxS("Reload"));
	copy->SetName(wxS("Copy source"));
	for (auto button : {open, apply, format, reload, copy})
		actions->Add(button, 0, wxRIGHT, 6);
	layout->Add(actions, 0, wxEXPAND | wxALL, 8);
	editor = new wxStyledTextCtrl(panel, wxID_ANY);
	editor->SetName(wxS("Lua source"));
	editor->SetCodePage(wxSTC_CP_UTF8);
	editor->SetLexer(wxSTC_LEX_LUA);
	editor->SetKeyWords(0, wxS("and break do else elseif end false for function goto if in local nil not or repeat return then true until while"));
	editor->StyleSetFont(wxSTC_STYLE_DEFAULT, wxFont(wxFontInfo(11).Family(wxFONTFAMILY_TELETYPE)));
	editor->StyleClearAll();
	editor->StyleSetForeground(wxSTC_LUA_COMMENT, wxColour(80, 125, 80));
	editor->StyleSetForeground(wxSTC_LUA_COMMENTLINE, wxColour(80, 125, 80));
	editor->StyleSetForeground(wxSTC_LUA_WORD, wxColour(35, 70, 180));
	editor->StyleSetForeground(wxSTC_LUA_STRING, wxColour(155, 60, 45));
	editor->StyleSetForeground(wxSTC_LUA_CHARACTER, wxColour(155, 60, 45));
	editor->StyleSetForeground(wxSTC_LUA_LITERALSTRING, wxColour(155, 60, 45));
	editor->StyleSetForeground(wxSTC_LUA_NUMBER, wxColour(125, 55, 135));
	editor->SetTabWidth(4);
	editor->SetIndent(4);
	editor->SetUseTabs(true);
	editor->SetTabIndents(true);
	editor->SetBackSpaceUnIndents(true);
	editor->SetEOLMode(wxSTC_EOL_LF);
	editor->SetMarginType(0, wxSTC_MARGIN_NUMBER);
	editor->SetMarginWidth(0, 52);
	editor->SetMarginWidth(1, 0);
	editor->IndicatorSetStyle(diagnostic_indicator, wxSTC_INDIC_SQUIGGLE);
	editor->IndicatorSetForeground(diagnostic_indicator, wxColour(200, 40, 40));
	editor->IndicatorSetUnder(diagnostic_indicator, true);
	editor->SetReadOnly(true);
	layout->Add(editor, 1, wxEXPAND | wxLEFT | wxRIGHT, 8);
	diagnostics = new wxStaticText(panel, wxID_ANY, _("Open a karaoke code line or a Lua source file."), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
	diagnostics->SetName(wxS("Lua diagnostics"));
	layout->Add(diagnostics, 0, wxEXPAND | wxALL, 8);
	panel->SetSizer(layout);
	auto frame_layout = new wxBoxSizer(wxVERTICAL);
	frame_layout->Add(panel, 1, wxEXPAND);
	SetSizer(frame_layout);
	SetMinSize(wxSize(600, 400));

	open->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
		wxFileDialog dialog(this, _("Open Lua source"), {}, {}, _("Lua source (*.lua)|*.lua|All files|*.*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
		if (dialog.ShowModal() == wxID_OK)
			OpenFile(agi::fs::PathFromString(from_wx(dialog.GetPath())));
	});
	apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SaveDocument(); });
	format->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { FormatDocument(); });
	reload->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ReloadDocument(); });
	copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CopySource(); });
	editor->Bind(wxEVT_STC_CHANGE, [this](wxStyledTextEvent&) {
		if (loading || !document)
			return;
		FinishPendingDiscard(false);
		source_diagnostic.reset();
		action_message.clear();
		document->SetSource(from_wx(editor->GetText()));
		RefreshDocument(false);
	});
	editor->Bind(wxEVT_STC_CHARADDED, [this](wxStyledTextEvent& event) {
		if (event.GetKey() != '\n')
			return;
		int line = editor->GetCurrentLine();
		if (line > 0) {
			editor->SetLineIndentation(line, editor->GetLineIndentation(line - 1));
			editor->GotoPos(editor->GetLineIndentPosition(line));
		}
	});
	editor->Bind(wxEVT_STC_UPDATEUI, [this](wxStyledTextEvent&) {
		int pos = editor->GetCurrentPos() - 1;
		int character = pos >= 0 ? editor->GetCharAt(pos) : 0;
		if (character > 0 && std::string_view("()[]{}").find(static_cast<char>(character)) != std::string_view::npos) {
			int match = editor->BraceMatch(pos);
			if (match >= 0)
				editor->BraceHighlight(pos, match);
			else
				editor->BraceBadLight(pos);
		}
		else
			editor->BraceHighlight(-1, -1);
	});
	Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
		if (event.ControlDown() && !event.AltDown()) {
			int key = event.GetKeyCode();
			if (key == 'S') {
				SaveDocument();
				return;
			}
			if (key == 'Z' && wxWindow::FindFocus() == editor) {
				if (event.ShiftDown())
					editor->Redo();
				else
					editor->Undo();
				return;
			}
			if (key == 'Y' && wxWindow::FindFocus() == editor) {
				editor->Redo();
				return;
			}
		}
		event.Skip();
	});
	Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
		if (!PrepareToClose()) {
			if (event.CanVeto())
				event.Veto();
			return;
		}
		Hide();
	});
	Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& event) {
		if (event.GetActive())
			RefreshDocument();
		event.Skip();
	});
	if (context) {
		commit_connection = context->ass->AddCommitListener([this](int, AssDialogue const *) { RefreshDocument(); });
		file_connection = context->subsController->AddFileOpenListener([this](agi::fs::path const&, bool) { RefreshDocument(); });
	}
	RefreshDocument(false);
}

LuaWorkspaceFrame::~LuaWorkspaceFrame() { DetachContext(); }

void LuaWorkspaceFrame::DetachContext() {
	FinishPendingDiscard(false);
	commit_connection.Disconnect();
	file_connection.Disconnect();
	context = nullptr;
	if (document)
		document->DetachContext();
}

bool LuaWorkspaceFrame::IsDirty() const { return document && document->IsDirty(); }

bool LuaWorkspaceFrame::HasPendingDiscard() const {
	return document && pending_discard_document == document.get() && pending_discard_revision == document->GetRevision();
}

void LuaWorkspaceFrame::FinishPendingDiscard(bool commit) {
	bool discard = commit && HasPendingDiscard();
	pending_discard_document = nullptr;
	pending_discard_revision = 0;
	if (discard) {
		document->DiscardChanges();
		source_diagnostic.reset();
		action_message.clear();
		SetEditorSource();
		RefreshDocument();
	}
}

void LuaWorkspaceFrame::SetEditorSource() {
	FinishPendingDiscard(false);
	loading = true;
	editor->SetReadOnly(false);
	editor->SetText(document ? to_wx(document->GetSource()) : wxString{});
	editor->EmptyUndoBuffer();
	editor->SetSavePoint();
	editor->SetReadOnly(!document);
	loading = false;
}

void LuaWorkspaceFrame::ShowResult(LuaWorkspaceDocumentResult const& result, bool remember) {
	if (remember)
		action_message = result.message;
	if (result.diagnostic)
		source_diagnostic = result.diagnostic;
	wxString message = to_wx(result.message.empty() ? action_message : result.message);
	editor->SetIndicatorCurrent(diagnostic_indicator);
	editor->IndicatorClearRange(0, editor->GetTextLength());
	if (source_diagnostic) {
		if (!message.empty())
			message += wxS(": ");
		message += to_wx(source_diagnostic->message);
		if (source_diagnostic->line > 0) {
			int line = std::min(source_diagnostic->line - 1, editor->GetLineCount() - 1);
			int start = editor->PositionFromLine(line);
			int end = editor->GetLineEndPosition(line);
			if (source_diagnostic->column > 0)
				start = std::min(editor->FindColumn(line, source_diagnostic->column - 1), end);
			if (end > start)
				editor->IndicatorFillRange(start, end - start);
			else if (start < editor->GetTextLength())
				editor->IndicatorFillRange(start, editor->PositionAfter(start) - start);
		}
	}
	if (message.empty())
		message = IsDirty() ? _("Source has unsaved changes.") : _("Source matches the saved baseline.");
	diagnostics->SetLabel(message);
	diagnostics->SetToolTip(message);
}

void LuaWorkspaceFrame::RefreshDocument(bool check_target) {
	if (document && check_target)
		target_state = document->Check();
	apply->Enable(document && target_state.state != LuaWorkspaceDocumentState::Invalidated);
	format->Enable(document != nullptr);
	reload->Enable(document && target_state.state != LuaWorkspaceDocumentState::Invalidated);
	apply->SetLabel(document && document->GetKind() == LuaWorkspaceDocumentKind::LuaFile ? _("Save") : _("Apply"));
	wxString title = wxS("Lua Workspace");
	if (document) {
		title += wxS(" - ");
		title += to_wx(document->GetDisplayName());
		if (IsDirty())
			title += wxS(" *");
		if (target_state.state == LuaWorkspaceDocumentState::Conflict)
			title += _(" [conflict]");
		if (target_state.state == LuaWorkspaceDocumentState::Invalidated)
			title += _(" [target unavailable]");
	}
	SetTitle(title);
	ShowResult(target_state, false);
}

bool LuaWorkspaceFrame::FinishOpen(std::unique_ptr<LuaWorkspaceDocument> candidate, LuaWorkspaceDocumentResult const& result) {
	if (!candidate) {
		RefreshDocument();
		ShowResult(result);
		Show();
		Raise();
		return false;
	}
	document = std::move(candidate);
	source_diagnostic.reset();
	action_message.clear();
	target_state = {};
	SetEditorSource();
	RefreshDocument();
	ShowResult(result);
	Show();
	Raise();
	editor->SetFocus();
	return true;
}

bool LuaWorkspaceFrame::OpenCurrentLine() {
	auto discard = agi::make_scope_exit([this] { FinishPendingDiscard(false); });
	if (!context)
		return false;
	auto line = context->selectionController->GetActiveLine();
	if (!line) {
		ShowResult({.state = LuaWorkspaceDocumentState::Invalidated, .message = from_wx(_("Select a karaoke code line first."))});
		return false;
	}
	int id = line->Id;
	if (!PrepareToClose(true))
		return false;
	LuaWorkspaceDocumentResult result;
	auto candidate = LuaWorkspaceDocument::OpenCode(context, id, result);
	return FinishOpen(std::move(candidate), result);
}

bool LuaWorkspaceFrame::OpenFile(agi::fs::path const& filename) {
	auto discard = agi::make_scope_exit([this] { FinishPendingDiscard(false); });
	if (!PrepareToClose(true))
		return false;
	LuaWorkspaceDocumentResult result;
	auto candidate = LuaWorkspaceDocument::OpenFile(filename, result);
	return FinishOpen(std::move(candidate), result);
}

bool LuaWorkspaceFrame::PrepareToClose(bool defer_discard) {
	if (defer_discard && HasPendingDiscard())
		return true;
	FinishPendingDiscard(false);
	if (!IsDirty())
		return true;
	wxMessageDialog dialog(this, _("Save the changes to the Lua Workspace source before continuing?"), _("Unsaved Lua source"), wxYES_NO | wxCANCEL | wxICON_QUESTION);
	dialog.SetYesNoCancelLabels(document->GetKind() == LuaWorkspaceDocumentKind::LuaFile ? _("Save") : _("Apply"), _("Discard"), _("Cancel"));
	int answer = dialog.ShowModal();
	if (answer == wxID_CANCEL)
		return false;
	if (answer == wxID_YES)
		return SaveDocument();
	pending_discard_document = document.get();
	pending_discard_revision = document->GetRevision();
	if (!defer_discard)
		FinishPendingDiscard(true);
	return true;
}

bool LuaWorkspaceFrame::SaveDocument() {
	if (!document)
		return false;
	FinishPendingDiscard(false);
	auto result = document->Save();
	if (result.state == LuaWorkspaceDocumentState::Conflict && result.observed) {
		wxString message = to_wx(result.message);
		message += _("\n\nCurrent target:\n");
		message += to_wx(result.observed->text).Left(1200);
		if (document->GetKind() == LuaWorkspaceDocumentKind::KaraokeCode) {
			message += _("\nEffect: ");
			message += to_wx(result.observed->effect);
			message += _("\nStyle: ");
			message += to_wx(result.observed->style);
		}
		message += _("\n\nEditor source:\n");
		message += to_wx(document->GetSource()).Left(1200);
		wxArrayString choices;
		choices.Add(_("Reload target and discard editor changes"));
		choices.Add(_("Overwrite this target with editor source"));
		choices.Add(_("Copy editor source and keep editing"));
		choices.Add(_("Keep editing without saving"));
		wxSingleChoiceDialog dialog(this, message, _("Lua source conflict"), choices);
		dialog.SetSelection(3);
		if (dialog.ShowModal() != wxID_OK)
			return false;
		switch (dialog.GetSelection()) {
			case 0: return ReloadDocument();
			case 1: result = document->Save(&*result.observed); break;
			case 2: CopySource(); return false;
			default: return false;
		}
	}
	if (result.Succeeded()) {
		source_diagnostic.reset();
		editor->SetSavePoint();
	}
	else if (result.diagnostic && result.diagnostic->line > 0)
		editor->GotoLine(result.diagnostic->line - 1);
	RefreshDocument();
	ShowResult(result);
	return result.Succeeded();
}

bool LuaWorkspaceFrame::ReloadDocument() {
	if (!document)
		return false;
	if (IsDirty() && wxMessageBox(_("Discard editor changes and reload the current target?"), _("Reload Lua source"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES)
		return false;
	auto result = document->Reload();
	if (result.Succeeded()) {
		source_diagnostic.reset();
		SetEditorSource();
	}
	RefreshDocument();
	ShowResult(result);
	return result.Succeeded();
}

void LuaWorkspaceFrame::FormatDocument() {
	if (!document)
		return;
	auto result = FormatLuaSource(document->GetSource());
	if (!result.Succeeded()) {
		ShowResult({.state = LuaWorkspaceDocumentState::Error, .diagnostic = result.diagnostic});
		if (result.diagnostic->line > 0)
			editor->GotoLine(result.diagnostic->line - 1);
		return;
	}
	source_diagnostic.reset();
	action_message.clear();
	if (result.source != document->GetSource()) {
		editor->BeginUndoAction();
		editor->SetTargetStart(0);
		editor->SetTargetEnd(editor->GetTextLength());
		editor->ReplaceTarget(to_wx(result.source));
		editor->EndUndoAction();
	}
	RefreshDocument();
}

void LuaWorkspaceFrame::CopySource() {
	if (!document)
		return;
	if (!wxTheClipboard->Open()) {
		ShowResult({.state = LuaWorkspaceDocumentState::Error, .message = from_wx(_("Unable to open the clipboard. The editor source has not been changed."))});
		return;
	}
	bool copied = wxTheClipboard->SetData(new wxTextDataObject(to_wx(document->GetSource())));
	wxTheClipboard->Close();
	if (!copied) {
		ShowResult({.state = LuaWorkspaceDocumentState::Error, .message = from_wx(_("Unable to copy the source to the clipboard. The editor source has not been changed."))});
		return;
	}
	if (!wxTheClipboard->Flush()) {
		ShowResult({.state = LuaWorkspaceDocumentState::Error, .message = from_wx(_("Source copied, but the clipboard could not be made persistent. Keep Aegisub open to use the copied source."))});
		return;
	}
	ShowResult({.message = from_wx(_("Source copied to the clipboard and available after Aegisub closes."))});
}
