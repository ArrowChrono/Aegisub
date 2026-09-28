#include "lua_workspace_frame.h"
#include "lua_workspace_language.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "automation/automation_invocation_observer.h"
#include "automation/automation_breakpoint_store.h"
#include "automation/automation_debug_service.h"
#include "automation/automation_live_host.h"
#include "automation/karaoke_line_classifier.h"
#include "auto4_base.h"
#include "auto4_lua.h"
#include "command/command.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "selection_controller.h"
#include "subs_controller.h"
#include "ui_dispatch.h"

#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/scope_exit.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/bmpbndl.h>
#include <wx/choicdlg.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/filedlg.h>
#include <wx/font.h>
#include <wx/intl.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/stattext.h>
#include <wx/stc/stc.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/treectrl.h>
#include <wx/utils.h>
#include <wx/wrapsizer.h>

#include <algorithm>
#include <functional>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>

using namespace Automation4;

namespace {
constexpr int diagnostic_indicator = 8;
constexpr std::size_t runtime_text_limit = 2048;
constexpr std::size_t runtime_fragment_limit = 16;
constexpr std::string_view truncated_marker = "[truncated]";
constexpr std::string_view karaoke_templater_execution_id = "aegisub.kara-templater.apply";
constexpr char source_width_option[] = "Automation/Lua Workspace/Layout/Source Width Percent";
constexpr char output_height_option[] = "Automation/Lua Workspace/Layout/Output Height Percent";

class DebugVariableItemData final : public wxTreeItemData {
	public:
	wxString detail;
	explicit DebugVariableItemData(wxString value) : detail(std::move(value)) {}
};

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

struct RuntimeSourceFragmentView {
	std::optional<int> line;
	std::optional<std::string> kind;
	std::optional<std::string> text;
};

struct RuntimeTemplateView {
	std::optional<std::string> event_type;
	std::optional<std::string> template_type;
	std::optional<int> template_id;
	std::optional<std::string> template_name;
	std::optional<std::string> owner_script;
	std::optional<std::string> fragment_kind;
	std::vector<RuntimeSourceFragmentView> source_fragments;
	std::optional<std::size_t> source_fragment_count;
	std::optional<std::string> phase;
	std::optional<std::string> scope;
	std::optional<int> source_line;
	std::optional<std::string> source_style;
	std::optional<std::string> source_text;
	std::optional<int> target_line;
	std::optional<std::string> target_style;
	std::optional<std::string> target_text;
	std::optional<AutomationTemplateLineSnapshot> original_line;
	std::optional<AutomationTemplateLineSnapshot> current_line;
	std::optional<AutomationTemplateSyllableSnapshot> syllable;
	std::optional<AutomationTemplateSyllableSnapshot> base_syllable;
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
	result.event_type = bounded(source.kind);
	result.phase = bounded(source.phase);
	result.scope = bounded(source.scope_kind);
	result.loop_index = source.loop_index;
	result.loop_count = source.loop_count;
	result.highlight_index = source.highlight_index;
	result.char_index = source.char_index;
	result.char_text = bounded(source.char_text);
	result.parse_error = bounded(source.parse_error);
	result.runtime_error = bounded(source.runtime_error);
	if (source.identity) {
		result.template_type = bounded(source.identity->template_kind);
		result.template_id = source.identity->template_debug_id;
		result.template_name = bounded(source.identity->template_id);
		result.owner_script = bounded(source.identity->owner_script);
		result.fragment_kind = bounded(source.identity->fragment_kind);
		result.source_line = source.identity->source_line_index;
	}
	if (source.source) {
		result.source_style = bounded(source.source->style);
		result.source_text = bounded(source.source->text);
		result.source_fragment_count = source.source->fragments.size();
		for (std::size_t index = 0; index < std::min(*result.source_fragment_count, runtime_fragment_limit); ++index) {
			auto const& fragment = source.source->fragments[index];
			result.source_fragments.push_back({.line = fragment.source_line_index, .kind = bounded(fragment.fragment_kind), .text = bounded(fragment.text)});
		}
	}
	if (source.target) {
		auto const& target = *source.target;
		result.original_line = target.original_line;
		result.current_line = target.line;
		result.syllable = target.syllable;
		result.base_syllable = target.base_syllable;
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
	auto bound_line = [](std::optional<AutomationTemplateLineSnapshot>& line) {
		if (!line)
			return;
		line->line_class = bounded(line->line_class);
		line->style = bounded(line->style);
		line->actor = bounded(line->actor);
		line->effect = bounded(line->effect);
		line->text = bounded(line->text);
	};
	auto bound_syllable = [](std::optional<AutomationTemplateSyllableSnapshot>& syllable) {
		if (!syllable)
			return;
		syllable->text = bounded(syllable->text);
		syllable->text_stripped = bounded(syllable->text_stripped);
		syllable->inline_fx = bounded(syllable->inline_fx);
	};
	bound_line(result.original_line);
	bound_line(result.current_line);
	bound_syllable(result.syllable);
	bound_syllable(result.base_syllable);
	return result;
}

bool has_template_detail(RuntimeTemplateView const& value) {
	return value.event_type || value.phase || value.scope || value.source_line || value.source_style || value.source_text || value.target_line || value.target_style || value.target_text || value.syllable_index || value.syllable_text || value.loop_index || value.loop_count || value.highlight_index || value.char_index || value.char_text || value.parse_error || value.runtime_error;
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

void add_snapshot_field(wxString& output, wxString const& label, std::optional<std::string> const& value) {
	output += label + wxS(": ");
	output += value ? to_wx(*value) : wxString(wxS("[not captured]"));
	output += wxS("\n");
}

void add_snapshot_field(wxString& output, wxString const& label, std::optional<int> const& value) {
	output += label + wxS(": ");
	output += value ? wxString::Format(wxS("%d"), *value) : wxString(wxS("[not captured]"));
	output += wxS("\n");
}

void add_snapshot_field(wxString& output, wxString const& label, std::optional<double> const& value) {
	output += label + wxS(": ");
	output += value ? wxString::Format(wxS("%.2f"), *value) : wxString(wxS("[not captured]"));
	output += wxS("\n");
}

void add_line_snapshot(wxString& output, wxString const& prefix, std::optional<AutomationTemplateLineSnapshot> const& line, bool applicable) {
	if (!line) {
		output += prefix + (applicable ? wxS(": [not captured]\n") : wxS(": [not applicable]\n"));
		return;
	}
	add_snapshot_field(output, prefix + wxS(" index"), line->index);
	add_snapshot_field(output, prefix + wxS(" layer"), line->layer);
	add_snapshot_field(output, prefix + wxS(" style"), line->style);
	add_snapshot_field(output, prefix + wxS(" text"), line->text);
	add_snapshot_field(output, prefix + wxS(" effect"), line->effect);
	add_snapshot_field(output, prefix + wxS(" start (ms)"), line->start_time);
	add_snapshot_field(output, prefix + wxS(" end (ms)"), line->end_time);
}

void add_syllable_snapshot(wxString& output, wxString const& prefix, std::optional<AutomationTemplateSyllableSnapshot> const& syllable, bool applicable) {
	if (!syllable) {
		output += prefix + (applicable ? wxS(": [not captured]\n") : wxS(": [not applicable]\n"));
		return;
	}
	add_snapshot_field(output, prefix + wxS(" index"), syllable->index);
	add_snapshot_field(output, prefix + wxS(" text"), syllable->text);
	add_snapshot_field(output, prefix + wxS(" start relative to line (ms)"), syllable->start_time);
	add_snapshot_field(output, prefix + wxS(" end relative to line (ms)"), syllable->end_time);
	add_snapshot_field(output, prefix + wxS(" duration (ms)"), syllable->duration);
	add_snapshot_field(output, prefix + wxS(" inline_fx"), syllable->inline_fx);
	add_snapshot_field(output, prefix + wxS(" left"), syllable->left);
	add_snapshot_field(output, prefix + wxS(" center"), syllable->center);
	add_snapshot_field(output, prefix + wxS(" right"), syllable->right);
	add_snapshot_field(output, prefix + wxS(" width"), syllable->width);
	add_snapshot_field(output, prefix + wxS(" height"), syllable->height);
}

template <typename T>
void add_change_field(wxString& output, wxString const& label, std::optional<T> const& original, std::optional<T> const& current) {
	output += label + wxS(": ") + (original && current ? (*original == *current ? wxS("unchanged") : wxS("changed")) : wxS("[not captured]")) + wxS("\n");
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
	bool workspace_run = false;
	LuaWorkspaceDocument const *document = nullptr;
	std::uint64_t generation = 0;
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
	SetIcon(GETICON(automation_toolbutton_16));
	auto panel = new wxPanel(this);
	auto layout = new wxBoxSizer(wxVERTICAL);
	auto actions = new wxWrapSizer(wxHORIZONTAL, wxREMOVE_LEADING_SPACES);
	int const button_height = panel->FromDIP(30);
	auto group = [panel, actions] {
		auto row = new wxBoxSizer(wxHORIZONTAL);
		actions->Add(row, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT | wxBOTTOM, panel->FromDIP(8));
		return row;
	};
	auto icon_button = [panel, button_height](wxBoxSizer *row, int id, wxString const& name, wxBitmapBundle const& icon) {
		auto button = new wxButton(panel, id, {}, wxDefaultPosition, wxSize(panel->FromDIP(32), button_height), wxBU_EXACTFIT | wxBU_NOTEXT);
		button->SetBitmap(icon);
		button->SetLabel(name);
		button->SetName(name);
		button->SetToolTip(name);
		row->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, panel->FromDIP(2));
		return button;
	};
	auto text_button = [panel, button_height](wxBoxSizer *row, wxString const& name) {
		auto button = new wxButton(panel, wxID_ANY, name, wxDefaultPosition, wxSize(-1, button_height), wxBU_EXACTFIT);
		button->SetName(name);
		row->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, panel->FromDIP(2));
		return button;
	};
	auto file_actions = group();
	auto open = icon_button(file_actions, wxID_OPEN, _("Open File"), CMD_ICON_BUNDLE_GET(open_toolbutton, wxLayout_Default));
	open_button = open;
	apply = icon_button(file_actions, wxID_SAVE, _("Apply"), CMD_ICON_BUNDLE_GET(save_toolbutton, wxLayout_Default));
	format = icon_button(file_actions, wxID_ANY, _("Format"), CMD_ICON_BUNDLE_GET(format_toolbutton, wxLayout_Default));
	reload = icon_button(file_actions, wxID_ANY, _("Reload"), CMD_ICON_BUNDLE_GET(reload_toolbutton, wxLayout_Default));
	auto copy = icon_button(file_actions, wxID_COPY, _("Copy source"), CMD_ICON_BUNDLE_GET(copy_button, wxLayout_Default));
	auto run_actions = group();
	run_button = icon_button(run_actions, wxID_ANY, _("Run"), CMD_ICON_BUNDLE_GET(button_play, wxLayout_Default));
	debug_button = text_button(run_actions, _("Debug"));
	pause_on_entry = new wxCheckBox(panel, wxID_ANY, _("Pause on entry"));
	pause_on_entry->SetName(wxS("Pause on entry"));
	pause_on_entry->SetToolTip(_("By default, Debug runs to the next breakpoint or completes if none is set."));
	run_actions->Add(pause_on_entry, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, panel->FromDIP(5));
	continue_button = text_button(run_actions, _("Continue"));
	pause_button = icon_button(run_actions, wxID_ANY, _("Pause"), CMD_ICON_BUNDLE_GET(button_pause, wxLayout_Default));
	stop_button = icon_button(run_actions, wxID_ANY, _("Stop"), CMD_ICON_BUNDLE_GET(button_stop, wxLayout_Default));
	detach_button = text_button(run_actions, _("Detach"));
	auto step_actions = group();
	step_in_button = text_button(step_actions, _("Step In"));
	step_over_button = text_button(step_actions, _("Step Over"));
	step_out_button = text_button(step_actions, _("Step Out"));
	auto breakpoint_actions = group();
	auto toggle_breakpoint = text_button(breakpoint_actions, _("Toggle Breakpoint"));
	auto clear_breakpoints = text_button(breakpoint_actions, _("Clear Breakpoints"));
	auto toolbar_buttons = {open, apply, format, reload, copy, run_button, debug_button, continue_button, pause_button, stop_button,
							detach_button, step_in_button, step_over_button, step_out_button, toggle_breakpoint, clear_breakpoints};
	int uniform_height = button_height;
	for (auto button : toolbar_buttons)
		uniform_height = std::max(uniform_height, button->GetBestSize().y);
	for (auto button : toolbar_buttons)
		button->SetMinSize(wxSize(button->GetMinSize().x, uniform_height));
	pause_on_entry->SetMinSize(wxSize(-1, uniform_height));
	layout->Add(actions, 0, wxEXPAND | wxALL, panel->FromDIP(6));
	run_status = new wxStaticText(panel, wxID_ANY, _("No Workspace invocation."), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
	run_status->SetName(wxS("Lua run status"));
	layout->Add(run_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
	output_splitter = new wxSplitterWindow(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_NOBORDER | wxSP_LIVE_UPDATE);
	source_splitter = new wxSplitterWindow(output_splitter, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_NOBORDER | wxSP_LIVE_UPDATE);
	output_splitter->SetName(wxS("Lua output splitter"));
	source_splitter->SetName(wxS("Lua source splitter"));
	editor = new wxStyledTextCtrl(source_splitter, wxID_ANY);
	editor->SetName(wxS("Lua source"));
	editor->SetCodePage(wxSTC_CP_UTF8);
	editor->SetLexer(wxSTC_LEX_LUA);
	editor->SetKeyWords(0, wxS("and break do else elseif end false for function goto if in local nil not or repeat return then true until while"));
	editor->SetTabWidth(4);
	editor->SetIndent(4);
	editor->SetUseTabs(true);
	editor->SetTabIndents(true);
	editor->SetBackSpaceUnIndents(true);
	editor->SetEOLMode(wxSTC_EOL_LF);
	editor->SetMarginType(0, wxSTC_MARGIN_SYMBOL);
	editor->SetMarginMask(0, 1 << 1);
	editor->SetMarginWidth(0, FromDIP(16));
	editor->SetMarginSensitive(0, true);
	editor->SetMarginType(1, wxSTC_MARGIN_SYMBOL);
	editor->SetMarginMask(1, 1 << 2);
	editor->SetMarginWidth(1, FromDIP(16));
	editor->SetMarginType(2, wxSTC_MARGIN_NUMBER);
	editor->SetMarginMask(2, 0);
	editor->SetMarginWidth(2, FromDIP(24));
	editor->MarkerDefine(1, wxSTC_MARK_CIRCLE, wxColour(180, 30, 30), wxColour(180, 30, 30));
	editor->MarkerDefine(2, wxSTC_MARK_ARROW, wxColour(35, 70, 180), wxColour(35, 70, 180));
	editor->IndicatorSetStyle(diagnostic_indicator, wxSTC_INDIC_SQUIGGLE);
	editor->IndicatorSetForeground(diagnostic_indicator, wxColour(200, 40, 40));
	editor->IndicatorSetUnder(diagnostic_indicator, true);
	editor->SetReadOnly(true);
	runtime_tabs = new wxNotebook(source_splitter, wxID_ANY);
	runtime_tabs->SetMinSize(wxSize(280, -1));
	runtime_context = new wxTextCtrl(runtime_tabs, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
	generated_output = new wxTextCtrl(runtime_tabs, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
	runtime_context->SetName(wxS("Lua runtime context"));
	generated_output->SetName(wxS("Lua generated output"));
	runtime_tabs->AddPage(runtime_context, _("Context"));
	runtime_tabs->AddPage(generated_output, _("Generated"));
	auto execution_panel = new wxPanel(runtime_tabs);
	auto execution_layout = new wxBoxSizer(wxVERTICAL);
	execution_identity = new wxStaticText(execution_panel, wxID_ANY, _("No paused source."), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
	execution_identity->SetName(wxS("Lua execution identity"));
	execution_source = new wxStyledTextCtrl(execution_panel, wxID_ANY);
	execution_source->SetName(wxS("Lua execution source"));
	execution_source->SetCodePage(wxSTC_CP_UTF8);
	execution_source->SetLexer(wxSTC_LEX_LUA);
	execution_source->SetKeyWords(0, wxS("and break do else elseif end false for function goto if in local nil not or repeat return then true until while"));
	execution_source->SetMarginType(0, wxSTC_MARGIN_SYMBOL);
	execution_source->SetMarginMask(0, 1 << 2);
	execution_source->SetMarginWidth(0, FromDIP(16));
	execution_source->SetMarginType(1, wxSTC_MARGIN_NUMBER);
	execution_source->SetMarginMask(1, 0);
	execution_source->SetMarginWidth(1, FromDIP(24));
	execution_source->MarkerDefine(2, wxSTC_MARK_ARROW, wxColour(35, 70, 180), wxColour(35, 70, 180));
	execution_source->SetReadOnly(true);
	execution_layout->Add(execution_identity, 0, wxEXPAND | wxALL, 4);
	execution_layout->Add(execution_source, 1, wxEXPAND | wxALL, 4);
	execution_panel->SetSizer(execution_layout);
	runtime_tabs->AddPage(execution_panel, _("Execution Source"));
	execution_tab_index = static_cast<int>(runtime_tabs->GetPageCount()) - 1;
	auto stack_panel = new wxPanel(runtime_tabs);
	auto stack_layout = new wxBoxSizer(wxVERTICAL);
	debug_location = new wxStaticText(stack_panel, wxID_ANY, _("No active debug pause."), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
	debug_location->SetName(wxS("Lua debug location"));
	stack_frames = new wxListBox(stack_panel, wxID_ANY);
	stack_frames->SetName(wxS("Lua stack frames"));
	debug_variables = new wxTreeCtrl(stack_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_HIDE_ROOT | wxTR_SINGLE);
	debug_variables->SetName(wxS("Lua variables tree"));
	variable_details = new wxTextCtrl(stack_panel, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
	variable_details->SetName(wxS("Lua variable details"));
	variable_details->SetMinSize(wxSize(-1, 90));
	stack_layout->Add(debug_location, 0, wxEXPAND | wxALL, 4);
	stack_layout->Add(stack_frames, 1, wxEXPAND | wxALL, 4);
	stack_layout->Add(debug_variables, 2, wxEXPAND | wxALL, 4);
	stack_layout->Add(variable_details, 1, wxEXPAND | wxALL, 4);
	stack_panel->SetSizer(stack_layout);
	runtime_tabs->AddPage(stack_panel, _("Stack and Variables"));
	stack_tab_index = static_cast<int>(runtime_tabs->GetPageCount()) - 1;
	language = std::make_unique<LuaWorkspaceLanguage>(editor, runtime_tabs);
	source_splitter->SetMinimumPaneSize(FromDIP(180));
	source_splitter->SplitVertically(editor, runtime_tabs);
	auto output_panel = new wxPanel(output_splitter);
	auto output_layout = new wxBoxSizer(wxVERTICAL);
	diagnostics = new wxStaticText(output_panel, wxID_ANY, _("Open a karaoke code line or a Lua source file."), wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
	diagnostics->SetName(wxS("Lua diagnostics"));
	output_layout->Add(diagnostics, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 4);
	run_log = new wxTextCtrl(output_panel, wxID_ANY, {}, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
	run_log->SetName(wxS("Lua run log"));
	output_layout->Add(run_log, 1, wxEXPAND | wxALL, 4);
	output_panel->SetSizer(output_layout);
	output_splitter->SetMinimumPaneSize(FromDIP(56));
	output_splitter->SplitHorizontally(source_splitter, output_panel);
	layout->Add(output_splitter, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
	panel->SetSizer(layout);
	ApplyEditorPreferences();
	editor_font_face_connection = OPT_SUB("Automation/Lua Workspace/Editor/Font Face", [this] { ApplyEditorPreferences(); });
	editor_font_size_connection = OPT_SUB("Automation/Lua Workspace/Editor/Font Size", [this] { ApplyEditorPreferences(); });
	editor_wrap_connection = OPT_SUB("Automation/Lua Workspace/Editor/Wrap", [this] { ApplyEditorPreferences(); });
	ClearRuntimeObservation();
	auto frame_layout = new wxBoxSizer(wxVERTICAL);
	frame_layout->Add(panel, 1, wxEXPAND);
	SetSizer(frame_layout);
	SetMinSize(wxSize(600, 400));
	auto source_percent = std::clamp(static_cast<int>(OPT_GET(source_width_option)->GetInt()), 10, 90);
	auto output_percent = std::clamp(static_cast<int>(OPT_GET(output_height_option)->GetInt()), 8, 65);
	source_splitter->SetSashGravity(source_percent / 100.0);
	output_splitter->SetSashGravity(1.0 - output_percent / 100.0);
	source_splitter->Bind(wxEVT_SPLITTER_SASH_POS_CHANGING, [this](wxSplitterEvent& event) {
		source_drag_pending = true;
		event.Skip();
	});
	source_splitter->Bind(wxEVT_SPLITTER_SASH_POS_RESIZE, [this](wxSplitterEvent& event) {
		source_drag_pending = false;
		event.SetSashPosition(event.GetNewSize() * std::clamp(static_cast<int>(OPT_GET(source_width_option)->GetInt()), 10, 90) / 100);
	});
	source_splitter->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED, [this](wxSplitterEvent& event) {
		if (!restoring_splitters && source_drag_pending && source_splitter->GetClientSize().x > 0) {
			int percent = 100 * event.GetSashPosition() / source_splitter->GetClientSize().x;
			OPT_SET(source_width_option)->SetInt(percent);
			source_splitter->SetSashGravity(percent / 100.0);
		}
		source_drag_pending = false;
		event.Skip();
	});
	output_splitter->Bind(wxEVT_SPLITTER_SASH_POS_CHANGING, [this](wxSplitterEvent& event) {
		output_drag_pending = true;
		event.Skip();
	});
	output_splitter->Bind(wxEVT_SPLITTER_SASH_POS_RESIZE, [this](wxSplitterEvent& event) {
		output_drag_pending = false;
		event.SetSashPosition(event.GetNewSize() * (100 - std::clamp(static_cast<int>(OPT_GET(output_height_option)->GetInt()), 8, 65)) / 100);
	});
	output_splitter->Bind(wxEVT_SPLITTER_SASH_POS_CHANGED, [this](wxSplitterEvent& event) {
		if (!restoring_splitters && output_drag_pending && output_splitter->GetClientSize().y > 0) {
			int percent = 100 * (output_splitter->GetClientSize().y - event.GetSashPosition()) / output_splitter->GetClientSize().y;
			OPT_SET(output_height_option)->SetInt(percent);
			output_splitter->SetSashGravity(1.0 - percent / 100.0);
		}
		output_drag_pending = false;
		event.Skip();
	});
	CallAfter([this, source_percent, output_percent] {
		restoring_splitters = true;
		source_splitter->SetSashPosition(source_splitter->GetClientSize().x * source_percent / 100);
		output_splitter->SetSashPosition(output_splitter->GetClientSize().y * (100 - output_percent) / 100);
		restoring_splitters = false;
	});

	open->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
		wxFileDialog dialog(this, _("Open Lua source"), {}, {}, _("Lua source (*.lua)|*.lua|All files|*.*"), wxFD_OPEN | wxFD_FILE_MUST_EXIST);
		if (dialog.ShowModal() == wxID_OK)
			OpenFile(agi::fs::PathFromString(from_wx(dialog.GetPath())));
	});
	apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SaveDocument(); });
	format->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { FormatDocument(); });
	reload->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ReloadDocument(); });
	copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { CopySource(); });
	run_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartRun(false); });
	debug_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartRun(true); });
	continue_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (active_session) active_session->Resume(AutomationDebugResumeAction::Continue); });
	pause_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (active_session) active_session->RequestPause(); });
	step_in_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (active_session) active_session->Resume(AutomationDebugResumeAction::StepIn); });
	step_over_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (active_session) active_session->Resume(AutomationDebugResumeAction::Next); });
	step_out_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (active_session) active_session->Resume(AutomationDebugResumeAction::StepOut); });
	stop_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
		if (active_run_request) {
			active_run_request->stop_requested->store(true);
			if (active_session)
				active_session->Detach();
			run_status->SetLabel(_("Stopping at the next safe Lua checkpoint..."));
		}
	});
	detach_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
		if (active_session)
			active_session->Detach();
		run_status->SetLabel(_("Detached; the invocation is still running and may commit subtitles."));
		UpdateRunControls();
	});
	toggle_breakpoint->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ToggleBreakpoint(editor->GetCurrentLine() + 1); });
	clear_breakpoints->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { editor->MarkerDeleteAll(1); });
	editor->Bind(wxEVT_STC_MARGINCLICK, [this](wxStyledTextEvent& event) {
		if (event.GetMargin() == 0)
			ToggleBreakpoint(editor->LineFromPosition(event.GetPosition()) + 1);
	});
	stack_frames->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { ShowSelectedFrame(); });
	debug_variables->Bind(wxEVT_TREE_SEL_CHANGED, [this](wxTreeEvent&) {
		if (!rebuilding_debug_tree)
			RefreshVariableDetails(active_session && active_session->GetStateSnapshot().current_pause.has_value());
	});
	debug_timer = new wxTimer(this);
	Bind(wxEVT_TIMER, [this](wxTimerEvent&) { PollDebugState(); }, debug_timer->GetId());
	editor->Bind(wxEVT_STC_CHANGE, [this](wxStyledTextEvent&) {
		if (loading || !document)
			return;
		UpdateLineNumberMargins();
		FinishPendingDiscard(false);
		source_diagnostic.reset();
		action_message.clear();
		document->SetSource(from_wx(editor->GetText()));
		if (active_observation) {
			if (active_observation->workspace_run)
				RenderRuntimeObservation(invocation_sequence, active_observation);
			else
				ClearRuntimeObservation();
		}
		UpdateRunControls();
		RefreshDocument(false);
	});
	editor->Bind(wxEVT_STC_CHARADDED, [this](wxStyledTextEvent& event) {
		event.Skip();
		if (event.GetKey() != '\n')
			return;
		int line = editor->GetCurrentLine();
		if (line > 0) {
			editor->SetLineIndentation(line, editor->GetLineIndentation(line - 1));
			editor->GotoPos(editor->GetLineIndentPosition(line));
		}
	});
	editor->Bind(wxEVT_STC_UPDATEUI, [this](wxStyledTextEvent& event) {
		event.Skip();
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
			auto *focused = wxWindow::FindFocus();
			if (focused == editor || focused == execution_source) {
				auto *source = static_cast<wxStyledTextCtrl *>(focused);
				if (key == 'A') {
					source->SelectAll();
					return;
				}
				if (key == 'C') {
					source->Copy();
					return;
				}
				if (key == 'X' || key == 'V') {
					if (source == editor) {
						if (key == 'X')
							source->Cut();
						else
							source->Paste();
					}
					return;
				}
			}
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
	Bind(wxEVT_SHOW, [this](wxShowEvent& event) {
		language->SetActive(event.IsShown());
		event.Skip();
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
			if (!IsInvocationRunning())
				ClearInvocationPresentation();
			RefreshDocument();
		});
	}
	RefreshDocument(false);
	UpdateRunControls();
}

LuaWorkspaceFrame::~LuaWorkspaceFrame() {
	language.reset();
	DetachContext();
	debug_timer->Stop();
	delete debug_timer;
}

void LuaWorkspaceFrame::DetachContext() {
	if (IsInvocationRunning())
		return;
	FinishPendingDiscard(false);
	ClearRuntimeObservation();
	observation_lifetime.reset();
	commit_connection.Disconnect();
	file_connection.Disconnect();
	context = nullptr;
	if (document)
		document->DetachContext();
	if (language)
		language->SetActive(false);
}

std::shared_ptr<LuaWorkspaceRunRequest const> LuaWorkspaceFrame::GetWorkspaceRunRequest(AutomationInvocation const& invocation) const {
	agi::ui::VerifyAccess();
	if (!context || !active_run_request || invocation.kind != AutomationInvocationKind::MacroRun || invocation.feature_name != active_run_request->macro_command)
		return {};
	return active_run_request;
}

std::shared_ptr<LuaWorkspaceRunRequest const> LuaWorkspaceFrame::GetActiveRunRequest() const {
	agi::ui::VerifyAccess();
	return active_run_request;
}

bool LuaWorkspaceFrame::IsInvocationRunning() const { return static_cast<bool>(active_run_request); }

void LuaWorkspaceFrame::ReportRunProgress(std::uint64_t invocation_id, std::string const& text) {
	agi::ui::VerifyAccess();
	if (active_run_request && active_run_request->invocation_id == invocation_id)
		run_log->ChangeValue(to_wx(text));
}

void LuaWorkspaceFrame::ToggleBreakpoint(int line) {
	if (line < 1 || line > editor->GetLineCount())
		return;
	int index = line - 1;
	if (editor->MarkerGet(index) & (1 << 1))
		editor->MarkerDelete(index, 1);
	else
		editor->MarkerAdd(index, 1);
}

std::vector<AutomationDebugBreakpoint> LuaWorkspaceFrame::CaptureBreakpoints(std::string const& source_uri) const {
	std::vector<AutomationDebugBreakpoint> values;
	for (int line = 0; line < editor->GetLineCount(); ++line) {
		if (editor->MarkerGet(line) & (1 << 1))
			values.push_back({.source_path = source_uri, .line = line + 1, .enabled = true});
	}
	return values;
}

void LuaWorkspaceFrame::UpdateRunControls() {
	if (!run_button)
		return;
	bool busy = IsInvocationRunning();
	bool available = context && document && target_state.state == LuaWorkspaceDocumentState::Ready && no_macro_revision != document->GetRevision();
	run_button->Enable(!busy && available);
	debug_button->Enable(!busy && available);
	pause_on_entry->Enable(!busy);
	open_button->Enable(!busy);
	bool paused = false;
	bool attached = false;
	if (busy && active_session) {
		auto state = active_session->GetStateSnapshot();
		paused = state.state == AutomationDebugSessionState::Paused;
		attached = state.attached;
	}
	continue_button->Enable(busy && paused && attached);
	step_in_button->Enable(busy && paused && attached);
	step_over_button->Enable(busy && paused && attached);
	step_out_button->Enable(busy && paused && attached);
	pause_button->Enable(busy && !active_run_request->macro_command.empty() && active_run_request->debug_session && attached && !paused);
	stop_button->Enable(busy && !active_run_request->stop_requested->load());
	detach_button->Enable(busy && !active_run_request->macro_command.empty() && attached);
	if (apply)
		apply->Enable(!busy && document && target_state.state != LuaWorkspaceDocumentState::Invalidated);
	if (reload)
		reload->Enable(!busy && document && target_state.state != LuaWorkspaceDocumentState::Invalidated);
}

void LuaWorkspaceFrame::PollDebugState() {
	if (!active_session)
		return;
	if (active_run_request && (active_run_request->macro_command.empty() || !active_run_request->debug_session)) {
		UpdateRunControls();
		return;
	}
	auto state = active_session->GetStateSnapshot();
	if (state.version == last_debug_version)
		return;
	last_debug_version = state.version;
	if (active_observation && active_observation->workspace_run)
		RenderRuntimeObservation(invocation_sequence, active_observation);
	if (state.current_pause) {
		bool first_pause = !last_debug_snapshot;
		last_debug_snapshot = state;
		stack_frames->Clear();
		for (auto const& frame : state.current_pause->frames) {
			wxString label = to_wx(frame.function_name.empty() ? frame.kind : frame.function_name);
			label += wxS(" - ") + to_wx(frame.location.display_name);
			label += wxString::Format(wxS(":%d"), frame.location.line);
			stack_frames->Append(label);
		}
		if (!state.current_pause->frames.empty()) {
			stack_frames->SetSelection(0);
			ShowSelectedFrame();
		}
		bool current_editor_source = UpdatePausedEditorMarker(state);
		auto const& pause = *state.current_pause;
		debug_location->SetLabel(_("Paused at ") + to_wx(pause.location.display_name) + wxString::Format(wxS(":%d"), pause.location.line) + wxS(" [") + to_wx(ToString(pause.reason)) + wxS("]") + (current_editor_source ? _(" [editable source]") : _(" [captured source]")));
		debug_location->SetToolTip(debug_location->GetLabel());
		if (first_pause)
			runtime_tabs->SetSelection(current_editor_source ? stack_tab_index : execution_tab_index);
	}
	else {
		UpdatePausedEditorMarker({});
		if (last_debug_snapshot && !stack_frames->IsEmpty()) {
			execution_source->MarkerDeleteAll(2);
			RefreshExecutionIdentity();
			RefreshVariableDetails(false);
			debug_location->SetLabel(_("Last pause [last pause, not current] - invocation is running or has ended."));
		}
		else
			debug_location->SetLabel(_("Debug running; waiting for the next breakpoint or manual pause."));
		debug_location->SetToolTip(debug_location->GetLabel());
	}
	if (active_run_request && active_run_request->stop_requested->load())
		run_status->SetLabel(_("Stopping at the next safe Lua checkpoint; subtitle commit is still locked."));
	else if (!state.attached && IsInvocationRunning())
		run_status->SetLabel(_("Detached; the invocation is still running and may commit subtitles."));
	else if (state.current_pause) {
		auto const& pause = *state.current_pause;
		run_status->SetLabel(wxS("Debug: paused [") + to_wx(ToString(pause.reason)) + wxString::Format(wxS(" #%llu] "), static_cast<unsigned long long>(pause.sequence)) + to_wx(pause.location.source_path) + wxString::Format(wxS(":%d"), pause.location.line));
	}
	else
		run_status->SetLabel(wxS("Debug: ") + to_wx(ToString(state.state)) + wxS(" - ") + to_wx(state.message));
	run_status->SetToolTip(run_status->GetLabel());
	UpdateRunControls();
}

void LuaWorkspaceFrame::ShowSelectedFrame() {
	auto current = active_session ? active_session->GetStateSnapshot() : AutomationDebugStateSnapshot{};
	bool is_current_pause = current.current_pause.has_value();
	auto state = is_current_pause ? current : last_debug_snapshot.value_or(AutomationDebugStateSnapshot{});
	if (!state.current_pause)
		return;
	int selected = stack_frames->GetSelection();
	if (selected == wxNOT_FOUND || static_cast<std::size_t>(selected) >= state.current_pause->frames.size())
		return;
	auto const& frame = state.current_pause->frames[selected];
	auto source = last_sources ? last_sources->Find(frame.location.source_path) : nullptr;
	execution_source->SetReadOnly(false);
	execution_source->MarkerDeleteAll(2);
	execution_source->SetText(source ? to_wx(source->text) : wxString{});
	UpdateLineNumberMargins();
	execution_source->SetReadOnly(true);
	RefreshExecutionIdentity();
	if (source && is_current_pause && frame.location.line > 0 && frame.location.line <= execution_source->GetLineCount()) {
		execution_source->MarkerAdd(frame.location.line - 1, 2);
		execution_source->ScrollToLine(frame.location.line - 1);
	}
	rebuilding_debug_tree = true;
	auto resume_tree_events = agi::make_scope_exit([this] { rebuilding_debug_tree = false; });
	debug_variables->DeleteAllItems();
	auto root = debug_variables->AddRoot(wxS("Variables"));
	std::function<wxTreeItemId(wxTreeItemId const&, AutomationDebugVariable const&)> append_variable;
	append_variable = [&](wxTreeItemId const& parent, AutomationDebugVariable const& variable) {
		wxString name = to_wx(variable.name), value = to_wx(variable.value);
		wxString summary = value.size() > 80 ? value.Left(77) + wxS("...") : value;
		auto detail = wxS("Captured value: ") + value + wxS("\nName: ") + name + wxS("\nType: ") + to_wx(variable.value_type);
		auto item = debug_variables->AppendItem(parent, name + wxS(" = ") + summary, -1, -1, new DebugVariableItemData(std::move(detail)));
		for (auto const& child : variable.children)
			append_variable(item, child);
		return item;
	};
	wxTreeItemId first_variable;
	auto append_group = [&](wxString const& title, auto const& entries, bool expand) {
		auto group = debug_variables->AppendItem(root, title + wxString::Format(wxS(" (%zu)"), entries.size()));
		for (auto const& variable : entries) {
			auto item = append_variable(group, variable);
			if (expand && !first_variable.IsOk())
				first_variable = item;
		}
		if (expand)
			debug_variables->Expand(group);
	};
	append_group(_("Locals"), frame.locals, true);
	append_group(_("Upvalues"), frame.upvalues, true);
	for (auto const& scope : state.current_pause->scopes)
		append_group(to_wx(scope.name), scope.variables, false);
	if (first_variable.IsOk())
		debug_variables->SelectItem(first_variable);
	RefreshVariableDetails(is_current_pause);
}

void LuaWorkspaceFrame::RefreshExecutionIdentity() {
	auto current = active_session ? active_session->GetStateSnapshot() : AutomationDebugStateSnapshot{};
	auto state = current.current_pause ? current : last_debug_snapshot.value_or(AutomationDebugStateSnapshot{});
	if (!state.current_pause)
		return;
	int selected = stack_frames->GetSelection();
	if (selected == wxNOT_FOUND || static_cast<std::size_t>(selected) >= state.current_pause->frames.size())
		return;
	auto source = last_sources ? last_sources->Find(state.current_pause->frames[selected].location.source_path) : nullptr;
	if (!source) {
		execution_identity->SetLabel(_("Source is not in the immutable invocation registry."));
		return;
	}
	bool stale = document && (document->GetSourceIdentity() != source->source_identity || document->GetRevision() != source->revision);
	auto identity = to_wx(source->source_identity) + wxString::Format(wxS(" / revision %llu"), static_cast<unsigned long long>(source->revision));
	if (stale)
		identity += wxS(" [stale editor revision]");
	if (!current.current_pause)
		identity += wxS(" [last pause, not current]");
	execution_identity->SetLabel(identity);
	execution_identity->SetToolTip(identity);
}

void LuaWorkspaceFrame::RefreshVariableDetails(bool live) {
	wxString prefix = live ? _("Paused value\n") : _("Last pause [not live]\n");
	auto selection = debug_variables->GetSelection();
	auto *data = selection.IsOk() ? static_cast<DebugVariableItemData *>(debug_variables->GetItemData(selection)) : nullptr;
	variable_details->ChangeValue(prefix + (data ? data->detail : _("Select a variable to inspect its captured value.")));
}

bool LuaWorkspaceFrame::UpdatePausedEditorMarker(AutomationDebugStateSnapshot const& state) {
	int line = -1;
	if (state.current_pause && !state.current_pause->frames.empty() && document) {
		auto const& location = state.current_pause->frames.front().location;
		auto source = last_sources ? last_sources->Find(location.source_path) : nullptr;
		if (source && source->source_identity == document->GetSourceIdentity() && source->revision == document->GetRevision() && location.line > 0 && location.line <= editor->GetLineCount())
			line = location.line - 1;
	}
	if (editor->MarkerNext(0, 1 << 2) == line)
		return line >= 0;
	bool was_loading = loading;
	loading = true;
	auto restore_loading = agi::make_scope_exit([&] { loading = was_loading; });
	editor->MarkerDeleteAll(2);
	if (line >= 0) {
		editor->MarkerAdd(line, 2);
		editor->ScrollToLine(line);
	}
	return line >= 0;
}

void LuaWorkspaceFrame::StartRun(bool debug) {
	agi::ui::VerifyAccess();
	auto reject = [this](std::string message) {
		run_status->SetLabel(to_wx(message));
		ShowResult({.state = LuaWorkspaceDocumentState::Error, .message = std::move(message)});
	};
	if (!context || !document || IsInvocationRunning()) {
		reject("A Workspace invocation is already active or no source is open");
		return;
	}
	if (no_macro_revision == document->GetRevision()) {
		reject("This Lua source has no registered macro; edit or reload it before Run/Debug");
		return;
	}
	auto check = document->Check();
	if (!check.Succeeded()) {
		run_status->SetLabel(to_wx(check.message));
		ShowResult(check);
		return;
	}
	if (!config::automation_debug_service) {
		reject("The local Automation debug service is unavailable");
		return;
	}
	{
		std::optional<wxWindowDisabler> disabled_windows;
		auto *service = config::automation_debug_service;
		AutomationDebugLaunchRequest launch;
		launch.enabled = debug;
		launch.stop_on_entry = debug && pause_on_entry->GetValue();
		launch.nonblocking = false;
		std::shared_ptr<AutomationDebugSession> lease;
		try {
			lease = service->PrepareLocalSession({.engine_name = "Lua", .script_file = document->GetKind() == LuaWorkspaceDocumentKind::LuaFile ? document->GetFilename() : agi::fs::path{}, .feature_name = {}}, std::move(launch));
		}
		catch (std::exception const& error) {
			reject(error.what());
			return;
		}
		if (!lease) {
			reject("A local or remote Automation controller already owns this invocation");
			return;
		}
		auto release_session = [service, lease] {
			service->ClearSession(lease);
			try {
				if (config::global_scripts)
					config::global_scripts->ProcessPendingReload();
			}
			catch (agi::Exception const& error) {
				LOG_E("automation/workspace") << "Could not schedule deferred script reload: " << error.GetMessage();
			}
			catch (std::exception const& error) {
				LOG_E("automation/workspace") << "Could not schedule deferred script reload: " << error.what();
			}
		};
		auto release_lease = agi::make_scope_exit([&] { release_session(); });
		auto autosave_guard = context->subsController->InhibitAutosave();
		auto const document_generation = context->subsController->GetDocumentGeneration();
		bool const karaoke_templater = document->GetKind() == LuaWorkspaceDocumentKind::KaraokeCode;
		if (!karaoke_templater && !SaveDocument()) {
			reject("Save the Lua file successfully before Run/Debug");
			return;
		}

		auto request = std::make_shared<LuaWorkspaceRunRequest>();
		request->invocation_id = ++next_run_id;
		request->sources = std::make_shared<LuaWorkspaceSourceRegistry>();
		request->stop_requested = std::make_shared<std::atomic<bool>>(false);
		LuaWorkspaceSource source;
		source.source_identity = document->GetSourceIdentity();
		source.revision = document->GetRevision();
		source.display_name = document->GetDisplayName();
		source.text = document->GetSource();
		source.uri = karaoke_templater
						 ? MakeLuaWorkspaceSourceUri(request->invocation_id, source.source_identity, source.revision)
						 : NormalizeAutomationDebugSource(agi::fs::PathToString(document->GetFilename()));
		request->sources->Register(source);
		if (karaoke_templater)
			request->source_override = LuaWorkspaceSourceOverride{.document_generation = document->GetDocumentGeneration(), .dialogue_id = document->GetDialogueId(), .source = source};
		else
			request->file_source = source;
		lease->SetBreakpoints(CaptureBreakpoints(source.uri));
		lease->SetSourceRegistry(request->sources);
		if (debug)
			request->debug_session = lease;

		std::unique_ptr<Script> local_script;
		Script *script = nullptr;
		cmd::Command *command = nullptr;
		std::string failure;
		ClearRuntimeObservation();
		active_run_request = request;
		active_session = lease;
		last_sources = request->sources;
		last_debug_snapshot.reset();
		last_debug_version = 0;
		stack_frames->Clear();
		rebuilding_debug_tree = true;
		debug_variables->DeleteAllItems();
		rebuilding_debug_tree = false;
		variable_details->Clear();
		UpdatePausedEditorMarker({});
		execution_source->SetReadOnly(false);
		execution_source->SetText(wxString{});
		UpdateLineNumberMargins();
		execution_source->SetReadOnly(true);
		execution_identity->SetLabel(_("No paused source."));
		debug_location->SetLabel(_("Preparing Lua Workspace invocation."));
		context->lua_workspace_invocation_active = true;
		run_status->SetLabel(_("Preparing Lua Workspace invocation; Stop cancels loading or validation."));
		run_log->Clear();
		RefreshDocument();
		UpdateRunControls();
		debug_timer->Start(100);
		auto finish = agi::make_scope_exit([this, &script, debug, lease, request, &failure, &release_session, &release_lease] {
			debug_timer->Stop();
			PollDebugState();
			if (auto final_state = lease->GetStateSnapshot(); final_state.current_pause)
				last_debug_snapshot = std::move(final_state);
			std::optional<AutomationInvocationOutcome> outcome;
			if (active_observation) {
				std::scoped_lock lock(active_observation->mutex);
				if (active_observation->workspace_run)
					outcome = active_observation->view.outcome;
			}
			bool const failed = !failure.empty() || outcome == AutomationInvocationOutcome::Failed;
			bool const cancelled = !failed && (outcome == AutomationInvocationOutcome::Cancelled ||
											   (!outcome && request->stop_requested->load()));
			bool const completed = !failed && !cancelled && outcome == AutomationInvocationOutcome::Completed;
			lease->MarkCompleted(completed ? 0 : 1,
								 !failure.empty() ? failure : failed  ? "failed"
														  : cancelled ? "cancelled"
														  : completed ? "completed"
																	  : "unobserved");
			if (debug && script)
				script->SetDebugSession(nullptr);
			release_session();
			release_lease.release();
			context->lua_workspace_invocation_active = false;
			active_run_request.reset();
			active_session.reset();
			if (active_observation && active_observation->workspace_run)
				RenderRuntimeObservation(invocation_sequence, active_observation);
			UpdatePausedEditorMarker({});
			if (last_debug_snapshot && !stack_frames->IsEmpty()) {
				execution_source->MarkerDeleteAll(2);
				RefreshExecutionIdentity();
				RefreshVariableDetails(false);
				debug_location->SetLabel(_("Last pause [last pause, not current] - invocation has ended."));
			}
			else
				debug_location->SetLabel(_("Invocation ended without stopping at a breakpoint."));
			debug_location->SetToolTip(debug_location->GetLabel());
			if (!failure.empty())
				run_status->SetLabel(to_wx("Invocation failed: " + failure));
			else if (failed)
				run_status->SetLabel(_("Invocation failed; inspect Runtime Context and the run log."));
			else if (cancelled)
				run_status->SetLabel(_("Invocation cancelled after reaching a safe terminal state."));
			else if (completed)
				run_status->SetLabel(_("Invocation completed."));
			else
				run_status->SetLabel(_("Invocation ended without an observed terminal result."));
			wxString last_progress = run_log->GetValue();
			run_log->ChangeValue(run_status->GetLabel() + (last_progress.empty() ? wxString{} : _("\nLast reported progress (not live):\n") + last_progress));
			RefreshDocument();
			UpdateRunControls();
			if (close_after_run) {
				close_after_run = false;
				CallAfter([this] { if (PrepareToClose()) Hide(); });
			}
		});
		auto fail = [&](std::string message) {
			failure = message;
			reject(std::move(message));
		};
		disabled_windows.emplace(this);
		std::unique_ptr<BackgroundScriptRunner> preparation_runner;
		if (karaoke_templater) {
			std::vector<std::pair<Script *, cmd::Command *>> matches;
			for (auto *manager : {static_cast<ScriptManager *>(context->local_scripts.get()), static_cast<ScriptManager *>(config::global_scripts)}) {
				if (!manager)
					continue;
				for (auto const& candidate : manager->GetScripts()) {
					if (!candidate || !candidate->GetLoadedState())
						continue;
					for (auto *macro : candidate->GetMacros()) {
						if (macro && candidate->GetEngineName() == "Lua" && GetLuaMacroExecutionId(macro) == karaoke_templater_execution_id)
							matches.emplace_back(candidate.get(), macro);
					}
				}
			}
			if (matches.size() != 1) {
				fail("Exactly one loaded karaoke templater macro is required for Workspace Run/Debug");
				return;
			}
			script = matches.front().first;
			command = matches.front().second;
		}
		else {
			ScriptManager *managed = nullptr;
			Script *managed_script = nullptr;
			for (auto *manager : {static_cast<ScriptManager *>(context->local_scripts.get()), static_cast<ScriptManager *>(config::global_scripts)}) {
				if (!manager)
					continue;
				for (auto const& candidate : manager->GetScripts()) {
					if (candidate && candidate->GetFilename() == document->GetFilename()) {
						if (managed_script) {
							fail("The Lua file has more than one managed script identity");
							return;
						}
						managed = manager;
						managed_script = candidate.get();
					}
				}
			}
			try {
				auto host = CreateAutomationLiveHost(context);
				preparation_runner = host->Ui().CreateWorkspaceBackgroundScriptRunner(request, "Preparing Lua Workspace");
				if (!preparation_runner)
					throw AutomationError("The Lua Workspace preparation runner is unavailable");
				auto candidate = ScriptFactory::CreateFromFileForWorkspace(document->GetFilename(), request, *preparation_runner);
				if (request->stop_requested->load())
					return;
				if (!candidate || !candidate->GetLoadedState() || candidate->GetEngineName() != "Lua") {
					std::string message = "The saved Lua file did not reload as an Automation Lua script";
					if (candidate && !candidate->GetDescription().empty())
						message += ": " + candidate->GetDescription();
					fail(std::move(message));
					return;
				}
				if (managed)
					script = managed->ReplaceForWorkspace(managed_script, std::move(candidate), lease);
				else {
					local_script = std::move(candidate);
					script = local_script.get();
				}
			}
			catch (agi::UserCancelException const&) {
				request->stop_requested->store(true);
				return;
			}
			catch (agi::Exception const& error) {
				fail(error.GetMessage());
				return;
			}
			catch (std::exception const& error) {
				fail(error.what());
				return;
			}
			auto macros = script->GetMacros();
			if (macros.empty()) {
				no_macro_revision = document->GetRevision();
				UpdateRunControls();
				fail("The reloaded Lua file has no registered macro to run");
				return;
			}
			wxArrayString names;
			std::vector<std::string> command_names;
			for (auto *macro : macros) {
				names.Add(macro->StrDisplay(context));
				command_names.emplace_back(macro->name());
			}
			int chosen = 0;
			if (macros.size() > 1) {
				wxSingleChoiceDialog choice(this, _("Select a macro from the saved and reloaded Lua file"), _("Lua Workspace macro"), names);
				if (choice.ShowModal() != wxID_OK) {
					request->stop_requested->store(true);
					return;
				}
				chosen = choice.GetSelection();
			}
			if (chosen < 0 || static_cast<std::size_t>(chosen) >= macros.size()) {
				fail("The selected Lua macro is no longer available");
				return;
			}
			auto const& chosen_name = command_names[chosen];
			if (script && script->GetLoadedState()) {
				for (auto *macro : script->GetMacros()) {
					if (macro && std::string_view(macro->name()) == chosen_name) {
						if (command) {
							fail("The reloaded Lua file has a duplicate macro command identity");
							return;
						}
						command = macro;
					}
				}
			}
		}
		if (request->stop_requested->load())
			return;
		if (context->subsController->GetDocumentGeneration() != document_generation) {
			fail("The subtitle document changed while preparing this Workspace invocation");
			return;
		}
		check = document->Check();
		if (!check.Succeeded()) {
			fail(check.message);
			return;
		}
		bool qualified = !karaoke_templater;
		if (karaoke_templater) {
			for (auto const& line : context->ass->Events) {
				auto const classification = ClassifyKaraokeLine(line.Comment, line.Effect.get());
				if (classification.kind == KaraokeLineKind::Template ||
					(classification.kind == KaraokeLineKind::Code && (classification.scopes & KaraokeOnce))) {
					qualified = true;
					break;
				}
			}
		}
		if (!script || !command || script->GetEngineName() != "Lua" || !qualified) {
			fail("The selected Automation macro is not available for this subtitle document");
			return;
		}
		if (!karaoke_templater) {
			try {
				if (!preparation_runner)
					throw AutomationError("The Lua Workspace preparation runner is unavailable");
				bool const available = ValidateLuaMacroForWorkspace(command, context, request, *preparation_runner);
				if (request->stop_requested->load())
					return;
				if (!available) {
					fail("The selected Automation macro is not available for this subtitle document");
					return;
				}
			}
			catch (agi::UserCancelException const&) {
				request->stop_requested->store(true);
				return;
			}
			catch (agi::Exception const& error) {
				fail(error.GetMessage());
				return;
			}
			catch (std::exception const& error) {
				fail(error.what());
				return;
			}
		}
		if (request->stop_requested->load())
			return;
		if (context->subsController->GetDocumentGeneration() != document_generation) {
			fail("The subtitle document changed while validating this Workspace invocation");
			return;
		}
		request->macro_command = command->name();
		lease->SetTarget({.engine_name = script->GetEngineName(), .script_file = script->GetFilename(), .feature_name = request->macro_command});
		if (debug)
			script->SetDebugSession(lease.get());
		debug_location->SetLabel(_("Debug running; waiting for the next breakpoint or manual pause."));
		run_status->SetLabel(debug ? _("Debug running; source and breakpoints are frozen for this invocation.")
								   : _("Run active; unapplied source may still commit generated subtitles."));
		UpdateRunControls();
		try {
			(*command)(context);
		}
		catch (agi::Exception const& error) {
			failure = error.GetMessage();
		}
		catch (std::exception const& error) {
			failure = error.what();
		}
	}
	std::weak_ptr<void> lifetime = observation_lifetime;
	agi::ui::MainAsyncIfAlive(lifetime, [this] {
		if (context && !IsInvocationRunning()) {
			try {
				context->subsController->ProcessPendingExternalChange();
			}
			catch (agi::Exception const& error) {
				run_status->SetLabel(to_wx(error.GetMessage()));
			}
			catch (std::exception const& error) {
				run_status->SetLabel(to_wx(error.what()));
			}
		}
	});
}

void LuaWorkspaceFrame::ClearRuntimeObservation() {
	agi::ui::VerifyAccess();
	++invocation_sequence;
	active_observation.reset();
	if (runtime_context)
		runtime_context->ChangeValue(_("No observed macro run. Workspace Run/Debug can use a captured editor revision; regular Automation runs use saved source."));
	if (generated_output)
		generated_output->ChangeValue(_("No generated output observed."));
}

void LuaWorkspaceFrame::RenderRuntimeObservation(std::uint64_t sequence, std::weak_ptr<LuaWorkspaceRuntimeObservation> const& observation) {
	agi::ui::VerifyAccess();
	auto state = observation.lock();
	if (!state || !context || sequence != invocation_sequence || active_observation != state)
		return;
	if (document.get() != state->document || context->subsController->GetDocumentGeneration() != state->generation) {
		ClearRuntimeObservation();
		return;
	}
	if (document && !state->workspace_run && document->GetRevision() != state->revision) {
		ClearRuntimeObservation();
		return;
	}
	RuntimeView view;
	{
		std::scoped_lock lock(state->mutex);
		view = state->view;
		state->queued = false;
	}
	wxString status = ::run_status(view.outcome);
	wxString context_text = wxS("Invocation: macro_run\nStatus: ") + status + wxS("\n");
	context_text += wxS("Feature: ") + to_wx(view.feature_name) + wxS("\n");
	if (document)
		context_text += wxString::Format(wxS("Observed revision %llu; editor revision %llu (%s)\n"),
										 static_cast<unsigned long long>(state->revision), static_cast<unsigned long long>(document->GetRevision()),
										 document->GetRevision() == state->revision ? wxS("current") : wxS("stale"));
	if (state->workspace_run) {
		auto debug_state = active_session ? active_session->GetStateSnapshot() : AutomationDebugStateSnapshot{};
		context_text += debug_state.current_pause                                   ? wxS("Debug pause: current\n")
						: last_debug_snapshot && last_debug_snapshot->current_pause ? wxS("Debug pause: last pause, not current\n")
																					: wxS("Debug pause: none\n");
	}
	else
		context_text += wxS("Debug pause: not attached to this Automation-menu invocation\n");
	context_text += wxS("Template event snapshot: latest reported event, not live values from a paused Lua frame.\n\n");
	if (state->workspace_run)
		context_text += _("Workspace Run/Debug used the captured source revision; later editor edits affect only the next invocation.");
	else
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
		add_snapshot_field(context_text, wxS("Event type"), value.event_type);
		add_snapshot_field(context_text, wxS("Template type"), value.template_type);
		add_snapshot_field(context_text, wxS("Phase"), value.phase);
		add_snapshot_field(context_text, wxS("Scope"), value.scope);
		add_snapshot_field(context_text, wxS("Template ID"), value.template_id);
		add_snapshot_field(context_text, wxS("Template name"), value.template_name);
		add_snapshot_field(context_text, wxS("Owner script"), value.owner_script);
		add_snapshot_field(context_text, wxS("Template fragment kind"), value.fragment_kind);
		add_snapshot_field(context_text, wxS("Source line"), value.source_line);
		add_snapshot_field(context_text, wxS("Source style"), value.source_style);
		add_snapshot_field(context_text, wxS("Source text"), value.source_text);
		if (value.source_fragment_count)
			context_text += wxString::Format(wxS("Source fragments: %llu total, %llu shown\n"),
											 static_cast<unsigned long long>(*value.source_fragment_count), static_cast<unsigned long long>(value.source_fragments.size()));
		else
			context_text += wxS("Source fragments: [not captured]\n");
		for (std::size_t index = 0; index < value.source_fragments.size(); ++index) {
			auto const& fragment = value.source_fragments[index];
			context_text += wxString::Format(wxS("Source fragment %llu:\n"), static_cast<unsigned long long>(index + 1));
			add_snapshot_field(context_text, wxS("  line"), fragment.line);
			add_snapshot_field(context_text, wxS("  kind"), fragment.kind);
			add_snapshot_field(context_text, wxS("  text"), fragment.text);
		}
		if (value.source_fragment_count && *value.source_fragment_count > value.source_fragments.size())
			context_text += wxString::Format(wxS("Source fragments omitted: %llu [preview limit]\n"),
											 static_cast<unsigned long long>(*value.source_fragment_count - value.source_fragments.size()));
		add_field(context_text, wxS("Target line"), value.target_line);
		add_field(context_text, wxS("Target style"), value.target_style);
		add_field(context_text, wxS("Target text"), value.target_text);
		add_field(context_text, wxS("Syllable index"), value.syllable_index);
		add_field(context_text, wxS("Syllable text"), value.syllable_text);
		context_text += wxS("\nInput subtitle (orgline) and current template line:\n");
		bool line_scope = value.scope != std::optional<std::string>("once");
		add_line_snapshot(context_text, wxS("Orgline"), value.original_line, line_scope);
		add_line_snapshot(context_text, wxS("Current line"), value.current_line, line_scope);
		if (value.original_line && value.current_line) {
			auto const& original = *value.original_line;
			auto const& current = *value.current_line;
			add_change_field(context_text, wxS("Line text change"), original.text, current.text);
			add_change_field(context_text, wxS("Line layer change"), original.layer, current.layer);
			add_change_field(context_text, wxS("Line style change"), original.style, current.style);
			add_change_field(context_text, wxS("Line effect change"), original.effect, current.effect);
			context_text += wxS("Line timing change: ");
			context_text += original.start_time && original.end_time && current.start_time && current.end_time
								? (*original.start_time == *current.start_time && *original.end_time == *current.end_time ? wxS("unchanged") : wxS("changed"))
								: wxS("[not captured]");
			context_text += wxS("\n");
		}
		context_text += wxS("Syllable timing is relative to the input line; karaskel geometry uses PlayRes/script coordinates, not screen pixels.\n");
		bool syllable_scope = value.scope && *value.scope != "line" && *value.scope != "once";
		add_syllable_snapshot(context_text, wxS("Syllable"), value.syllable, syllable_scope);
		add_syllable_snapshot(context_text, wxS("Base syllable"), value.base_syllable, syllable_scope);
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
	state->generation = context->subsController->GetDocumentGeneration();
	state->workspace_run = active_run_request && invocation.feature_name == active_run_request->macro_command;
	state->revision = state->workspace_run && active_run_request->source_override
						  ? active_run_request->source_override->source.revision
					  : state->workspace_run && active_run_request->file_source
						  ? active_run_request->file_source->revision
					  : document ? document->GetRevision()
								 : 0;
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

void LuaWorkspaceFrame::ApplyEditorPreferences() {
	bool was_loading = loading;
	loading = true;
	auto restore_loading = agi::make_scope_exit([this, was_loading] { loading = was_loading; });
	int size = std::clamp(static_cast<int>(OPT_GET("Automation/Lua Workspace/Editor/Font Size")->GetInt()), 3, 42);
	wxFont font(wxFontInfo(size).Family(wxFONTFAMILY_TELETYPE));
	auto face = OPT_GET("Automation/Lua Workspace/Editor/Font Face")->GetString();
	if (!face.empty())
		font.SetFaceName(to_wx(face));
	int wrap = OPT_GET("Automation/Lua Workspace/Editor/Wrap")->GetBool() ? wxSTC_WRAP_WORD : wxSTC_WRAP_NONE;
	for (auto source : {editor, execution_source}) {
		source->StyleSetFont(wxSTC_STYLE_DEFAULT, font);
		source->StyleClearAll();
		source->StyleSetForeground(wxSTC_LUA_COMMENT, wxColour(80, 125, 80));
		source->StyleSetForeground(wxSTC_LUA_COMMENTLINE, wxColour(80, 125, 80));
		source->StyleSetForeground(wxSTC_LUA_WORD, wxColour(35, 70, 180));
		source->StyleSetForeground(wxSTC_LUA_STRING, wxColour(155, 60, 45));
		source->StyleSetForeground(wxSTC_LUA_CHARACTER, wxColour(155, 60, 45));
		source->StyleSetForeground(wxSTC_LUA_LITERALSTRING, wxColour(155, 60, 45));
		source->StyleSetForeground(wxSTC_LUA_NUMBER, wxColour(125, 55, 135));
		source->SetWrapMode(wrap);
	}
	UpdateLineNumberMargins();
}

void LuaWorkspaceFrame::UpdateLineNumberMargins() {
	for (auto source : {editor, execution_source}) {
		auto const digits = std::to_string(std::max(1, source->GetLineCount())).size();
		int const width = source->TextWidth(wxSTC_STYLE_LINENUMBER, to_wx(std::string(digits, '9'))) + FromDIP(8);
		int const margin = source == editor ? 2 : 1;
		if (source->GetMarginWidth(margin) != width)
			source->SetMarginWidth(margin, width);
	}
}

void LuaWorkspaceFrame::ClearInvocationPresentation() {
	last_sources.reset();
	last_debug_snapshot.reset();
	last_debug_version = 0;
	stack_frames->Clear();
	rebuilding_debug_tree = true;
	auto resume_tree_events = agi::make_scope_exit([this] { rebuilding_debug_tree = false; });
	debug_variables->DeleteAllItems();
	variable_details->Clear();
	execution_source->SetReadOnly(false);
	execution_source->SetText(wxString{});
	UpdateLineNumberMargins();
	execution_source->SetReadOnly(true);
	execution_identity->SetLabel(_("No paused source."));
	execution_identity->SetToolTip(execution_identity->GetLabel());
	debug_location->SetLabel(_("No active debug pause."));
	debug_location->SetToolTip(debug_location->GetLabel());
	run_status->SetLabel(_("No Workspace invocation."));
	run_status->SetToolTip(run_status->GetLabel());
	run_log->Clear();
	runtime_tabs->SetSelection(0);
}

void LuaWorkspaceFrame::SetEditorSource() {
	FinishPendingDiscard(false);
	ClearRuntimeObservation();
	loading = true;
	editor->MarkerDeleteAll(1);
	editor->MarkerDeleteAll(2);
	editor->SetReadOnly(false);
	editor->SetText(document ? to_wx(document->GetSource()) : wxString{});
	UpdateLineNumberMargins();
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
	if (language)
		language->Update(document.get(), document ? document->GetCodeScopes() : 0);
	if (document && check_target)
		target_state = document->Check();
	apply->Enable(!IsInvocationRunning() && document && target_state.state != LuaWorkspaceDocumentState::Invalidated);
	format->Enable(document != nullptr);
	reload->Enable(!IsInvocationRunning() && document && target_state.state != LuaWorkspaceDocumentState::Invalidated);
	auto apply_label = document && document->GetKind() == LuaWorkspaceDocumentKind::LuaFile ? _("Save") : _("Apply");
	apply->SetLabel(apply_label);
	apply->SetName(apply_label);
	apply->SetToolTip(apply_label);
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
	UpdateRunControls();
	if (active_session)
		UpdatePausedEditorMarker(active_session->GetStateSnapshot());
	if (last_debug_snapshot && last_debug_snapshot->current_pause)
		RefreshExecutionIdentity();
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
	ClearInvocationPresentation();
	no_macro_revision.reset();
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
	if (IsInvocationRunning()) {
		run_status->SetLabel(_("Cannot switch the Workspace target during an active invocation."));
		return false;
	}
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
	if (IsInvocationRunning()) {
		run_status->SetLabel(_("Cannot open a different Lua file during an active invocation."));
		return false;
	}
	auto discard = agi::make_scope_exit([this] { FinishPendingDiscard(false); });
	if (!PrepareToClose(true))
		return false;
	LuaWorkspaceDocumentResult result;
	auto candidate = LuaWorkspaceDocument::OpenFile(filename, result);
	return FinishOpen(std::move(candidate), result);
}

bool LuaWorkspaceFrame::PrepareToClose(bool defer_discard) {
	if (IsInvocationRunning()) {
		wxMessageDialog dialog(this, _("A Workspace invocation is still active. Stop and close after it ends, detach while keeping this control window open, or cancel closing."),
							   _("Active Lua Workspace invocation"), wxYES_NO | wxCANCEL | wxICON_WARNING);
		dialog.SetYesNoCancelLabels(_("Stop then close"), _("Detach and keep open"), _("Cancel"));
		int choice = dialog.ShowModal();
		if (choice == wxID_YES) {
			close_after_run = true;
			active_run_request->stop_requested->store(true);
			if (active_session)
				active_session->Detach();
			run_status->SetLabel(_("Stopping; Workspace will close after the invocation ends."));
		}
		else if (choice == wxID_NO) {
			close_after_run = false;
			if (active_session)
				active_session->Detach();
			run_status->SetLabel(active_run_request->stop_requested->load()
									 ? _("Stopping; this window will remain open after cancellation.")
									 : _("Detached; this window remains available to stop the invocation before it commits subtitles."));
			UpdateRunControls();
		}
		else if (choice == wxID_CANCEL && close_after_run) {
			close_after_run = false;
			run_status->SetLabel(_("Stopping; this window will remain open after cancellation."));
		}
		return false;
	}
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
	if (IsInvocationRunning()) {
		run_status->SetLabel(_("Apply/Save is unavailable until the invocation and its ASS commit finish."));
		return false;
	}
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
	if (IsInvocationRunning()) {
		run_status->SetLabel(_("Reload is unavailable during an active invocation."));
		return false;
	}
	if (!document)
		return false;
	if (IsDirty() && wxMessageBox(_("Discard editor changes and reload the current target?"), _("Reload Lua source"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES)
		return false;
	auto result = document->Reload();
	if (result.Succeeded()) {
		ClearInvocationPresentation();
		no_macro_revision.reset();
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
