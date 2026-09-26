#include "lua_workspace_frame.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "automation/automation_invocation_observer.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "selection_controller.h"
#include "subs_controller.h"
#include "ui_dispatch.h"

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
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stc/stc.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <functional>
#include <mutex>
#include <string_view>
#include <utility>

using namespace Automation4;

namespace {
constexpr int diagnostic_indicator = 8;
constexpr std::size_t runtime_text_limit = 2048;
constexpr std::string_view truncated_marker = "[truncated]";

std::string bounded(std::string const& value) {
	if (value.size() <= runtime_text_limit)
		return value;
	std::size_t end = runtime_text_limit - truncated_marker.size();
	while (end && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80)
		--end;
	return value.substr(0, end) + std::string(truncated_marker);
}

std::optional<std::string> bounded(std::optional<std::string> const& value) {
	return value ? std::optional<std::string>(bounded(*value)) : std::nullopt;
}

struct RuntimeTemplateView {
	std::optional<std::string> kind;
	std::optional<std::string> phase;
	std::optional<std::string> scope;
	std::optional<int> source_line;
	std::optional<std::string> source_style;
	std::optional<std::string> source_text;
	std::optional<int> target_line;
	std::optional<std::string> target_style;
	std::optional<std::string> target_text;
	std::optional<int> syllable_index;
	std::optional<std::string> syllable_text;
	std::optional<int> loop_index;
	std::optional<int> loop_count;
	std::optional<int> highlight_index;
	std::optional<int> char_index;
	std::optional<std::string> char_text;
	std::optional<std::string> parse_error;
	std::optional<std::string> runtime_error;
};

struct RuntimeGeneratedView {
	std::optional<int> count;
	std::optional<int> index;
	std::optional<std::string> text;
	std::optional<std::string> style;
	std::optional<int> start_time;
	std::optional<int> end_time;
	std::optional<int> source_line;
};

struct RuntimeView {
	std::string feature_name;
	std::optional<int> active_row;
	std::optional<int> selected_count;
	std::optional<std::string> subtitle_file;
	std::optional<int> play_res_x;
	std::optional<int> play_res_y;
	std::optional<AutomationInvocationOutcome> outcome;
	std::optional<RuntimeTemplateView> template_view;
	std::optional<RuntimeGeneratedView> generated_view;
};

RuntimeTemplateView project_template(AutomationTemplateDebugState const& source) {
	RuntimeTemplateView result;
	result.kind = bounded(source.kind);
	result.phase = bounded(source.phase);
	result.scope = bounded(source.scope_kind);
	result.loop_index = source.loop_index;
	result.loop_count = source.loop_count;
	result.highlight_index = source.highlight_index;
	result.char_index = source.char_index;
	result.char_text = bounded(source.char_text);
	result.parse_error = bounded(source.parse_error);
	result.runtime_error = bounded(source.runtime_error);
	if (source.identity)
		result.source_line = source.identity->source_line_index;
	if (source.source) {
		result.source_style = bounded(source.source->style);
		result.source_text = bounded(source.source->text);
	}
	if (source.target) {
		auto const& target = *source.target;
		if (target.line) {
			result.target_line = target.line->index;
			result.target_style = bounded(target.line->style);
			result.target_text = bounded(target.line->text);
		}
		if (target.syllable) {
			result.syllable_index = target.syllable->index;
			result.syllable_text = bounded(target.syllable->text);
		}
	}
	return result;
}

bool has_template_detail(RuntimeTemplateView const& value) {
	return value.kind || value.phase || value.scope || value.source_line || value.source_style || value.source_text || value.target_line || value.target_style || value.target_text || value.syllable_index || value.syllable_text || value.loop_index || value.loop_count || value.highlight_index || value.char_index || value.char_text || value.parse_error || value.runtime_error;
}

RuntimeGeneratedView project_generated(AutomationGeneratedLinesSnapshot const& source) {
	RuntimeGeneratedView result;
	result.count = source.count;
	if (source.last_line) {
		result.index = source.last_line->generated_index;
		result.text = bounded(source.last_line->text);
		result.style = bounded(source.last_line->style);
		result.start_time = source.last_line->start_time;
		result.end_time = source.last_line->end_time;
		result.source_line = source.last_line->source_line_index;
	}
	return result;
}

void add_field(wxString& output, wxString const& label, std::optional<std::string> const& value) {
	if (value)
		output += label + wxS(": ") + to_wx(*value) + wxS("\n");
}

void add_field(wxString& output, wxString const& label, std::optional<int> const& value) {
	if (value)
		output += label + wxS(": ") + wxString::Format(wxS("%d"), *value) + wxS("\n");
}

wxString run_status(std::optional<AutomationInvocationOutcome> outcome) {
	if (!outcome)
		return wxS("running");
	switch (*outcome) {
		case AutomationInvocationOutcome::Completed: return wxS("completed");
		case AutomationInvocationOutcome::Cancelled: return wxS("cancelled");
		case AutomationInvocationOutcome::Failed: return wxS("failed");
	}
	return wxS("unknown");
}
}

struct LuaWorkspaceRuntimeObservation {
	std::mutex mutex;
	RuntimeView view;
	bool queued = false;
	LuaWorkspaceDocument const *document = nullptr;
	std::uint64_t revision = 0;
};

namespace {
class WorkspaceInvocationObserver final : public AutomationInvocationObserver {
	std::shared_ptr<LuaWorkspaceRuntimeObservation> observation;
	std::function<void()> request_render;

	void QueueRender(bool needed) {
		if (needed)
			request_render();
	}

	public:
	WorkspaceInvocationObserver(std::shared_ptr<LuaWorkspaceRuntimeObservation> value, std::function<void()> render)
		: observation(std::move(value)), request_render(std::move(render)) {}

	void OnRuntimeStateSnapshot(AutomationRuntimeStateSnapshot const& snapshot) override {
		if (snapshot.invocation.kind != AutomationInvocationKind::MacroRun)
			return;
		std::optional<RuntimeTemplateView> template_view;
		std::optional<RuntimeGeneratedView> generated_view;
		if (snapshot.template_debug) {
			template_view = project_template(*snapshot.template_debug);
			if (snapshot.template_debug->generated)
				generated_view = project_generated(*snapshot.template_debug->generated);
		}
		bool queue = false;
		{
			std::scoped_lock lock(observation->mutex);
			if (observation->view.outcome)
				return;
			observation->view.active_row = snapshot.context_snapshot.selection.active_row;
			observation->view.selected_count = static_cast<int>(snapshot.context_snapshot.selection.selected_rows.size());
			if (snapshot.context_snapshot.has_project_context) {
				auto const& project = snapshot.context_snapshot.project;
				observation->view.subtitle_file = project.subtitle_file.empty() ? std::nullopt : std::optional<std::string>(bounded(project.subtitle_file));
				observation->view.play_res_x = project.play_res_x > 0 ? std::optional<int>(project.play_res_x) : std::nullopt;
				observation->view.play_res_y = project.play_res_y > 0 ? std::optional<int>(project.play_res_y) : std::nullopt;
			}
			else {
				observation->view.subtitle_file.reset();
				observation->view.play_res_x.reset();
				observation->view.play_res_y.reset();
			}
			if (template_view && has_template_detail(*template_view))
				observation->view.template_view = std::move(template_view);
			if (generated_view)
				observation->view.generated_view = std::move(generated_view);
			if (!observation->queued) {
				observation->queued = true;
				queue = true;
			}
		}
		QueueRender(queue);
	}

	void OnInvocationFinished(AutomationInvocationOutcome outcome) override {
		bool queue = false;
		{
			std::scoped_lock lock(observation->mutex);
			observation->view.outcome = outcome;
			if (!observation->queued) {
				observation->queued = true;
				queue = true;
			}
		}
		QueueRender(queue);
	}
};
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
	auto body = new wxBoxSizer(wxHORIZONTAL);
	body->Add(editor, 3, wxEXPAND | wxLEFT | wxRIGHT, 8);
	auto runtime_tabs = new wxNotebook(panel, wxID_ANY);
	runtime_tabs->SetMinSize(wxSize(280, -1));
	runtime_context = new wxTextCtrl(runtime_tabs, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
	generated_output = new wxTextCtrl(runtime_tabs, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
	runtime_context->SetName(wxS("Lua runtime context"));
	generated_output->SetName(wxS("Lua generated output"));
	runtime_tabs->AddPage(runtime_context, _("Context"));
	runtime_tabs->AddPage(generated_output, _("Generated"));
	body->Add(runtime_tabs, 1, wxEXPAND | wxRIGHT, 8);
	layout->Add(body, 1, wxEXPAND);
	diagnostics = new wxStaticText(panel, wxID_ANY, _("Open a karaoke code line or a Lua source file."), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
	diagnostics->SetName(wxS("Lua diagnostics"));
	layout->Add(diagnostics, 0, wxEXPAND | wxALL, 8);
	panel->SetSizer(layout);
	ClearRuntimeObservation();
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
		ClearRuntimeObservation();
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
		file_connection = context->subsController->AddFileOpenListener([this](agi::fs::path const&, bool) {
			ClearRuntimeObservation();
			RefreshDocument();
		});
	}
	RefreshDocument(false);
}

LuaWorkspaceFrame::~LuaWorkspaceFrame() { DetachContext(); }

void LuaWorkspaceFrame::DetachContext() {
	FinishPendingDiscard(false);
	ClearRuntimeObservation();
	observation_lifetime.reset();
	commit_connection.Disconnect();
	file_connection.Disconnect();
	context = nullptr;
	if (document)
		document->DetachContext();
}

void LuaWorkspaceFrame::ClearRuntimeObservation() {
	agi::ui::VerifyAccess();
	++invocation_sequence;
	active_observation.reset();
	if (runtime_context)
		runtime_context->ChangeValue(_("No observed macro run. Runtime context comes from saved ASS/template execution, not unapplied Workspace edits."));
	if (generated_output)
		generated_output->ChangeValue(_("No generated output observed."));
}

void LuaWorkspaceFrame::RenderRuntimeObservation(std::uint64_t sequence, std::weak_ptr<LuaWorkspaceRuntimeObservation> const& observation) {
	agi::ui::VerifyAccess();
	auto state = observation.lock();
	if (!state || !context || sequence != invocation_sequence || active_observation != state)
		return;
	if (document.get() != state->document || (document && document->GetRevision() != state->revision)) {
		ClearRuntimeObservation();
		return;
	}
	RuntimeView view;
	{
		std::scoped_lock lock(state->mutex);
		view = state->view;
		state->queued = false;
	}
	wxString status = run_status(view.outcome);
	wxString context_text = wxS("Invocation: macro_run\nStatus: ") + status + wxS("\n");
	context_text += wxS("Feature: ") + to_wx(view.feature_name) + wxS("\n\n");
	context_text += _("Runtime observes saved ASS/template execution. Unapplied Workspace edits are not executed.");
	context_text += wxS("\n");
	context_text += _("Text previews are limited to 2048 UTF-8 bytes; longer values are marked [truncated].");
	context_text += wxS("\n\n");
	add_field(context_text, wxS("Active row"), view.active_row);
	add_field(context_text, wxS("Selected rows"), view.selected_count);
	add_field(context_text, wxS("Subtitle file"), view.subtitle_file);
	add_field(context_text, wxS("PlayRes X"), view.play_res_x);
	add_field(context_text, wxS("PlayRes Y"), view.play_res_y);
	context_text += wxS("\n");
	if (view.template_view) {
		auto const& value = *view.template_view;
		add_field(context_text, wxS("Template kind"), value.kind);
		add_field(context_text, wxS("Phase"), value.phase);
		add_field(context_text, wxS("Scope"), value.scope);
		add_field(context_text, wxS("Source line"), value.source_line);
		add_field(context_text, wxS("Source style"), value.source_style);
		add_field(context_text, wxS("Source text"), value.source_text);
		add_field(context_text, wxS("Target line"), value.target_line);
		add_field(context_text, wxS("Target style"), value.target_style);
		add_field(context_text, wxS("Target text"), value.target_text);
		add_field(context_text, wxS("Syllable index"), value.syllable_index);
		add_field(context_text, wxS("Syllable text"), value.syllable_text);
		add_field(context_text, wxS("Loop index"), value.loop_index);
		add_field(context_text, wxS("Loop count"), value.loop_count);
		add_field(context_text, wxS("Highlight index"), value.highlight_index);
		add_field(context_text, wxS("Character index"), value.char_index);
		add_field(context_text, wxS("Character text"), value.char_text);
		add_field(context_text, wxS("Parse error"), value.parse_error);
		add_field(context_text, wxS("Runtime error"), value.runtime_error);
	}
	else
		context_text += _("No template context reported for this run.");
	runtime_context->ChangeValue(context_text);
	wxString generated_text = wxS("Status: ") + status + wxS("\n\n");
	generated_text += _("Text previews are limited to 2048 UTF-8 bytes; longer values are marked [truncated].");
	generated_text += wxS("\n\n");
	if (view.generated_view) {
		auto const& value = *view.generated_view;
		add_field(generated_text, wxS("Reported generated count"), value.count);
		add_field(generated_text, wxS("Last generated index"), value.index);
		add_field(generated_text, wxS("Last text"), value.text);
		add_field(generated_text, wxS("Last style"), value.style);
		add_field(generated_text, wxS("Last start (ms)"), value.start_time);
		add_field(generated_text, wxS("Last end (ms)"), value.end_time);
		add_field(generated_text, wxS("Last source line"), value.source_line);
		if (!value.index && !value.text)
			generated_text += _("No generated line in the latest reported template context.");
	}
	else
		generated_text += view.outcome ? _("No generated line reported for this run.") : _("No generated line reported yet.");
	generated_output->ChangeValue(generated_text);
}

std::shared_ptr<AutomationInvocationObserver> LuaWorkspaceFrame::BeginInvocationObservation(AutomationInvocation const& invocation) {
	agi::ui::VerifyAccess();
	if (!context || !IsShown() || invocation.kind != AutomationInvocationKind::MacroRun || !observation_lifetime)
		return {};
	ClearRuntimeObservation();
	auto state = std::make_shared<LuaWorkspaceRuntimeObservation>();
	state->document = document.get();
	state->revision = document ? document->GetRevision() : 0;
	state->view.feature_name = bounded(invocation.feature_name);
	active_observation = state;
	std::weak_ptr<LuaWorkspaceRuntimeObservation> weak_state = state;
	std::weak_ptr<void> lifetime = observation_lifetime;
	auto sequence = invocation_sequence;
	RenderRuntimeObservation(sequence, weak_state);
	return std::make_shared<WorkspaceInvocationObserver>(std::move(state), [this, weak_state, lifetime, sequence] {
		agi::ui::MainAsyncIfAlive(lifetime, [this, weak_state, sequence] { RenderRuntimeObservation(sequence, weak_state); });
	});
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
	ClearRuntimeObservation();
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
