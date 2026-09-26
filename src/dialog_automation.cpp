// Copyright (c) 2006, Niels Martin Hansen
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

#include "auto4_base.h"
#include "automation/automation_debug_ui.h"
#include "automation/engine/automation_engine_registry.h"
#include "automation/automation_debug_service.h"
#include "compat.h"
#include "command/command.h"
#include "dialog_manager.h"
#include "format.h"
#include "help_button.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "ui_services.h"
#ifdef WITH_WXSTC
#include "frame_main.h"
#include "lua_workspace_frame.h"
#include <libaegisub/string_utils.h>
#endif

#include <libaegisub/fs.h>
#include <libaegisub/signal.h>

#include <algorithm>
#include <string>
#include <vector>

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/listctrl.h>
#include <wx/log.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>

namespace {
/// Struct to attach a flag for global/local to scripts
struct ExtraScriptInfo {
	Automation4::Script *script;
	bool is_global;
};

void ShowCopyableInfoDialog(wxWindow *parent, wxString const& title, wxString const& message)
{
	wxDialog dialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

	auto *text = new wxTextCtrl(
		&dialog,
		wxID_ANY,
		message,
		wxDefaultPosition,
		dialog.FromDIP(wxSize(540, 360)),
		wxTE_MULTILINE | wxTE_RICH2 | wxHSCROLL | wxTE_NOHIDESEL);
	text->SetEditable(false);
	text->SetMinSize(dialog.FromDIP(wxSize(500, 320)));
	text->Bind(wxEVT_CHAR_HOOK, [text](wxKeyEvent &event) {
		if (event.GetModifiers() == wxMOD_CONTROL && (event.GetKeyCode() == 'A' || event.GetKeyCode() == 1)) {
			text->SelectAll();
			return;
		}
		event.Skip();
	});

	auto *copy_button = new wxButton(&dialog, wxID_ANY, _("&Copy"));
	auto *close_button = new wxButton(&dialog, wxID_CLOSE, _("&Close"));

	copy_button->Bind(wxEVT_BUTTON, [text](wxCommandEvent &) {
		text->SelectAll();
		text->Copy();
	});
	close_button->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent &) {
		dialog.EndModal(wxID_CLOSE);
	});

	wxSizer *button_box = new wxBoxSizer(wxHORIZONTAL);
	button_box->AddStretchSpacer(1);
	button_box->Add(copy_button, 0);
	button_box->AddSpacer(8);
	button_box->Add(close_button, 0);

	wxSizer *main_box = new wxBoxSizer(wxVERTICAL);
	main_box->Add(text, wxSizerFlags(1).Expand().Border());
	main_box->Add(button_box, wxSizerFlags().Expand().Border(wxALL & ~wxTOP));
	dialog.SetSizer(main_box);
	dialog.SetMinSize(dialog.FromDIP(wxSize(560, 400)));
	dialog.SetClientSize(dialog.FromDIP(wxSize(560, 400)));
	dialog.CenterOnParent();
	dialog.SetEscapeId(wxID_CLOSE);
	dialog.Bind(wxEVT_INIT_DIALOG, [text](wxInitDialogEvent &) {
		text->SetFocus();
		text->SetInsertionPoint(0);
		text->ShowPosition(0);
	});

	dialog.ShowModal();
}

class DialogAutomation final : public wxDialog {
	agi::Context *context;

	/// Currently loaded scripts
	std::vector<ExtraScriptInfo> script_info;

	/// File-local script manager
	Automation4::ScriptManager *local_manager;

	/// Listener for external changes to the local scripts
	agi::signal::Connection local_scripts_changed;

	/// Global script manager
	Automation4::ScriptManager *global_manager;

	/// Listener for external changes to the global scripts
	agi::signal::Connection global_scripts_changed;


	/// List of loaded scripts
	wxListView *list;

	/// Unload a local script
	wxButton *remove_button;

	/// Reload a script
	wxButton *reload_button;
#ifdef WITH_WXSTC
	wxButton *workspace_button;
	void OnOpenWorkspace(wxCommandEvent&);
#endif

	/// Toggle live automation debug mode
	wxButton *debug_mode_button;

	void RebuildList();
	void AddScript(Automation4::Script *script, bool is_global);
	void SetScriptInfo(int i, Automation4::Script *script);
	void UpdateDisplay();
	void UpdateDebugModeButton();

	void OnAdd(wxCommandEvent &);
	void OnRemove(wxCommandEvent &);
	void OnReload(wxCommandEvent &);

	void OnInfo(wxCommandEvent &);
	void OnReloadAutoload(wxCommandEvent &);
	void OnToggleDebugMode(wxCommandEvent &);

public:
	DialogAutomation(agi::Context *context);
};

DialogAutomation::DialogAutomation(agi::Context *c)
: wxDialog(c->GetUI().parent, -1, _("Automation Manager"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
, context(c)
, local_manager(c->GetCore().local_scripts.get())
, local_scripts_changed(local_manager->AddScriptChangeListener(&DialogAutomation::RebuildList, this))
, global_manager(config::global_scripts)
, global_scripts_changed(global_manager->AddScriptChangeListener(&DialogAutomation::RebuildList, this))
{
	SetIcon(GETICON(automation_toolbutton_16));

	// create main controls
	list = new wxListView(this, -1, wxDefaultPosition, FromDIP(wxSize(600, 175)), wxLC_REPORT|wxLC_SINGLE_SEL);
	wxButton *add_button = new wxButton(this, -1, _("&Add"));
	remove_button = new wxButton(this, -1, _("&Remove"));
	reload_button = new wxButton(this, -1, _("Re&load"));
#ifdef WITH_WXSTC
	workspace_button = new wxButton(this, -1, _("Edit in Lua &Workspace"));
	workspace_button->Bind(wxEVT_BUTTON, &DialogAutomation::OnOpenWorkspace, this);
#endif
	debug_mode_button = new wxButton(this, -1, _("Enable Debug Mode"));
	wxButton *info_button = new wxButton(this, -1, _("Show &Info"));
	wxButton *reload_autoload_button = new wxButton(this, -1, _("Re&scan Autoload Dir"));
	wxButton *close_button = new wxButton(this, wxID_CANCEL, _("&Close"));

	list->Bind(wxEVT_LIST_ITEM_SELECTED, std::bind(&DialogAutomation::UpdateDisplay, this));
	list->Bind(wxEVT_LIST_ITEM_DESELECTED, std::bind(&DialogAutomation::UpdateDisplay, this));
	add_button->Bind(wxEVT_BUTTON, &DialogAutomation::OnAdd, this);
	remove_button->Bind(wxEVT_BUTTON, &DialogAutomation::OnRemove, this);
	reload_button->Bind(wxEVT_BUTTON, &DialogAutomation::OnReload, this);
	debug_mode_button->Bind(wxEVT_BUTTON, &DialogAutomation::OnToggleDebugMode, this);
	info_button->Bind(wxEVT_BUTTON, &DialogAutomation::OnInfo, this);
	reload_autoload_button->Bind(wxEVT_BUTTON, &DialogAutomation::OnReloadAutoload, this);

	// add headers to list view
	list->InsertColumn(0, wxEmptyString, wxLIST_FORMAT_CENTER, 20);
	list->InsertColumn(1, _("Name"), wxLIST_FORMAT_LEFT, 140);
	list->InsertColumn(2, _("Filename"), wxLIST_FORMAT_LEFT, 90);
	list->InsertColumn(3, _("Description"), wxLIST_FORMAT_LEFT, 330);

	// button layout
	wxSizer *button_box = new wxBoxSizer(wxHORIZONTAL);
	button_box->AddStretchSpacer(2);
	button_box->Add(add_button, 0);
	button_box->Add(remove_button, 0);
	button_box->AddSpacer(10);
	button_box->Add(reload_button, 0);
#ifdef WITH_WXSTC
	button_box->Add(workspace_button, 0);
#endif
	button_box->Add(debug_mode_button, 0);
	button_box->Add(info_button, 0);
	button_box->AddSpacer(10);
	button_box->Add(reload_autoload_button, 0);
	button_box->AddSpacer(10);
	button_box->Add(new HelpButton(this,"Automation Manager"), 0);
	button_box->Add(close_button, 0);
	button_box->AddStretchSpacer(2);

	// main layout
	wxSizer *main_box = new wxBoxSizer(wxVERTICAL);
	main_box->Add(list, wxSizerFlags(1).Expand().Border());
	main_box->Add(button_box, wxSizerFlags().Expand().Border(wxALL & ~wxTOP));
	SetSizerAndFit(main_box);
	Center();

	// why doesn't this work... the button gets the "default" decoration but doesn't answer to Enter
	// ("esc" does work)
	SetDefaultItem(close_button);
	SetAffirmativeId(wxID_CANCEL);
	close_button->SetDefault();

	RebuildList();
}

void DialogAutomation::RebuildList()
{
	script_info.clear();
	list->DeleteAllItems();

	for (auto& script : local_manager->GetScripts()) AddScript(script.get(), false);
	for (auto& script : global_manager->GetScripts()) AddScript(script.get(), true);

	UpdateDisplay();
}

void DialogAutomation::SetScriptInfo(int i, Automation4::Script *script)
{
	list->SetItem(i, 1, to_wx(script->GetName()));
	list->SetItem(i, 2, script->GetPrettyFilename().wstring());
	list->SetItem(i, 3, to_wx(script->GetDescription()));
	if (!script->GetLoadedState())
		list->SetItemBackgroundColour(i, wxColour(255,128,128));
	else
		list->SetItemBackgroundColour(i, list->GetBackgroundColour());
}

void DialogAutomation::AddScript(Automation4::Script *script, bool is_global)
{
	ExtraScriptInfo ei = { script, is_global };
	script_info.push_back(ei);

	wxListItem itm;
	itm.SetText(is_global ? wxS("G") : wxS("L"));
	itm.SetData((int)script_info.size()-1);
	itm.SetId(list->GetItemCount());
	SetScriptInfo(list->InsertItem(itm), script);
}

void DialogAutomation::UpdateDisplay()
{
	int i = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	bool selected = i >= 0;
	bool local = selected && !script_info[list->GetItemData(i)].is_global;
	remove_button->Enable(local);
	reload_button->Enable(selected);
#ifdef WITH_WXSTC
	workspace_button->Enable(selected && agi::util::strings::iequals(
											 agi::fs::PathToString(script_info[list->GetItemData(i)].script->GetFilename().extension()), ".lua"));
#endif
	UpdateDebugModeButton();
}

#ifdef WITH_WXSTC
void DialogAutomation::OnOpenWorkspace(wxCommandEvent&) {
	int index = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	if (index < 0)
		return;
	auto const filename = script_info[list->GetItemData(index)].script->GetFilename();
	context->GetUI().frame->GetLuaWorkspace()->OpenFile(filename);
}
#endif

void DialogAutomation::UpdateDebugModeButton()
{
	if (!debug_mode_button)
		return;

	bool enabled = config::automation_debug_service && config::automation_debug_service->IsEnabled();
	debug_mode_button->SetLabel(enabled
		? _("Disable Debug Mode")
		: _("Enable Debug Mode"));
}

template<class Container>
static bool has_file(Container const& c, agi::fs::path const& fn)
{
	return any_of(c.begin(), c.end(),
		[&](std::unique_ptr<Automation4::Script> const& s) { return fn == s->GetFilename(); });
}

void DialogAutomation::OnAdd(wxCommandEvent &)
{
	auto fnames = context->RequestOpenFiles({
		from_wx(_("Add Automation script")),
		"Path/Last/Automation",
		"",
		"",
		Automation4::ScriptFactory::GetWildcardStr()
	});
	if (fnames.empty())
		return;

	for (auto const& fnpath : fnames) {
		if (has_file(local_manager->GetScripts(), fnpath) || has_file(global_manager->GetScripts(), fnpath)) {
			wxLogError(wxS("Script '%s' is already loaded"), to_wx(agi::fs::PathToString(fnpath)));
			continue;
		}

		bool recognised = false;
		auto script = Automation4::ScriptFactory::CreateFromFile(fnpath, true, &recognised);
		if (!recognised)
			wxLogError(_("The file was not recognised as an Automation script: %s"), fnpath.wstring());
		else if (script && !script->GetLoadedState())
			wxLogError(_("Failed to load Automation script '%s':\n%s"), fnpath.wstring(), to_wx(script->GetDescription()));
		local_manager->Add(std::move(script));
	}
}

void DialogAutomation::OnRemove(wxCommandEvent &)
{
	int i = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	if (i < 0) return;
	const ExtraScriptInfo &ei = script_info[list->GetItemData(i)];
	if (ei.is_global) return;

	local_manager->Remove(ei.script);
}

void DialogAutomation::OnReload(wxCommandEvent &)
{
	int i = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	if (i < 0) return;

	ExtraScriptInfo const& ei = script_info[list->GetItemData(i)];
	if (ei.is_global)
		global_manager->Reload(ei.script);
	else
		local_manager->Reload(ei.script);
}

void DialogAutomation::OnInfo(wxCommandEvent &)
{
	int i = list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
	ExtraScriptInfo *ei = i >= 0 ? &script_info[list->GetItemData(i)] : nullptr;

	wxArrayString info;
	std::back_insert_iterator<wxArrayString> append_info(info);

	info.push_back(fmt_tl(
		"Total scripts loaded: %d\nGlobal scripts loaded: %d\nLocal scripts loaded: %d\n",
		local_manager->GetScripts().size() + global_manager->GetScripts().size(),
		global_manager->GetScripts().size(),
		local_manager->GetScripts().size()));

	info.push_back(_("Scripting engines installed:"));
	for (auto const& engine : Automation4::AutomationEngineRegistry::GetEngines()) {
		if (!engine)
			continue;
		info.push_back(fmt_wx("- %s (%s)", engine->EngineName(), engine->FilenamePattern()));
	}

	if (ei) {
		info.push_back(fmt_tl("\nScript info:\nName: %s\nDescription: %s\nAuthor: %s\nVersion: %s\nFull path: %s\nState: %s\n\nFeatures provided by script:",
			ei->script->GetName(),
			ei->script->GetDescription(),
			ei->script->GetAuthor(),
			ei->script->GetVersion(),
			ei->script->GetFilename().wstring(),
			ei->script->GetLoadedState() ? _("Correctly loaded") : _("Failed to load")));

		for (auto const* f : ei->script->GetMacros())
			info.push_back(fmt_tl("    Macro: %s (%s)", f->StrDisplay(context), f->name()));
		for (auto const* f : ei->script->GetFilters())
			info.push_back(fmt_tl("    Export filter: %s", f->GetName()));
	}

	bool debug_enabled = config::automation_debug_service && config::automation_debug_service->IsEnabled();
	info.push_back(fmt_tl("\nDebug mode: %s",
		debug_enabled ? _("Enabled") : _("Disabled")));
	if (debug_enabled && config::automation_debug_service) {
		auto debug_state = config::automation_debug_service->GetStateSnapshot();
		info.push_back(fmt_tl("Debugger connected: %s",
			debug_state.client_connected ? _("Yes") : _("No")));
		info.push_back(fmt_tl("Debugger configured: %s",
			debug_state.client_configured ? _("Yes") : _("No")));
		if (debug_state.endpoint.available) {
			info.push_back(fmt_tl("Debug host: %s", debug_state.endpoint.host));
			info.push_back(fmt_tl("Debug port: %d", debug_state.endpoint.port));
			info.push_back(fmt_tl("Debug authentication: %s",
				debug_state.endpoint.token.empty() ? _("Not required") : _("Token")));
			info.push_back(fmt_tl("Debug token: %s",
				debug_state.endpoint.token.empty() ? _("(not required)") : to_wx(debug_state.endpoint.token)));
		}

		auto session = config::automation_debug_service->GetCurrentSession();
		if (session) {
			auto target = session->GetTarget();
			info.push_back(fmt_tl("Current debug target: %s (%s)",
				target.feature_name,
				target.script_file.wstring()));
		}
	}

	ShowCopyableInfoDialog(this, _("Automation Script Info"), wxJoin(info, '\n', 0));
}

void DialogAutomation::OnReloadAutoload(wxCommandEvent &)
{
	global_manager->Reload();
}

void DialogAutomation::OnToggleDebugMode(wxCommandEvent &)
{
	auto result = Automation4::ToggleAutomationDebugService(config::automation_debug_service);
	UpdateDebugModeButton();
	if (result.show_error)
		context->ShowError(result.message);
	else
		context->ShowStatus(result.message);
}
}

void ShowAutomationDialog(agi::Context *c) {
	c->GetUI().dialog->Show<DialogAutomation>(c);
}
