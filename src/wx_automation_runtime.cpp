#include "wx_automation_runtime.h"

#include "auto4_base.h"
#include "auto4_lua_factory.h"
#include "ass_file.h"
#include "ass_style.h"
#include "automation/automation_features.h"
#include "automation/automation_live_host.h"
#include "automation/automation_runtime_services.h"
#include "automation/lua_workspace_run.h"
#include "command/command.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "selection_controller.h"
#include "subs_controller.h"
#include "utils.h"
#include "wx_automation_dialog.h"

#ifdef WITH_PLUGIN_BRIDGE
#include "coreclr/plugin_lua_api.h"
#endif

#include <libaegisub/dispatch.h>
#include <libaegisub/format.h>
#include <libaegisub/fs.h>

#include <map>
#include <wx/clipbrd.h>
#include <wx/log.h>
#include <wx/translation.h>

namespace Automation4 {
namespace {
class WxRuntimeServices final : public AutomationRuntimeServices {
	public:
	[[nodiscard]] std::string Translate(std::string const& text) const override {
		return wxTranslations::Get() ? from_wx(wxGetTranslation(to_wx(text))) : text;
	}
	[[nodiscard]] std::string GetClipboard() const override {
		std::string text;
		agi::dispatch::Main().Sync([&] { text = ::GetClipboard(); });
		return text;
	}
	[[nodiscard]] bool SetClipboard(std::string const& text) const override {
		bool succeeded = false;
		agi::dispatch::Main().Sync([&] {
			auto& clipboard = *wxTheClipboard;
			if (clipboard.Open()) {
				succeeded = clipboard.SetData(new wxTextDataObject(to_wx(text)));
				clipboard.Close();
				clipboard.Flush();
			}
		});
		return succeeded;
	}
	bool CalculateTextExtents(AssStyle const& style, std::string const& text,
							  double& width, double& height, double& descent, double& external_leading) const override {
		auto copy = style;
		return Automation4::CalculateTextExtents(&copy, text, width, height, descent, external_leading);
	}
	void InstallLuaModules(lua_State *state) const override {
#ifdef WITH_PLUGIN_BRIDGE
		RegisterPluginLuaApi(state);
#else
		(void)state;
#endif
	}
	void LogWarning(std::string const& message) const override { wxLogWarning(wxS("%s"), to_wx(message)); }
	void LogError(std::string const& message) const override { wxLogError(wxS("%s"), to_wx(message)); }
};

class WxSelection final : public AutomationSelection {
	SelectionController *selection;

	public:
	explicit WxSelection(SelectionController *selection) : selection(selection) {}
	[[nodiscard]] std::set<AssDialogue *> const& GetSelectedSet() const override { return selection->GetSelectedSet(); }
	[[nodiscard]] AssDialogue *GetActiveLine() const override { return selection->GetActiveLine(); }
	void SetSelectionAndActive(std::set<AssDialogue *> lines, AssDialogue *active) override {
		selection->SetSelectionAndActive(std::move(lines), active);
	}
};

std::shared_ptr<AutomationHost> ContextHost(agi::Context const *context, std::shared_ptr<AutomationHost> const& bound) {
	return bound && bound->ProjectContextIdentity() == context ? bound : CreateAutomationLiveHost(context);
}

AutomationMacroContext MakeContext(agi::Context const *context, WxSelection& selection,
								   std::shared_ptr<AutomationHost> const& bound = {}) {
	auto const generation = context->subsController->GetDocumentGeneration();
	auto const revision = context->subsController->GetDocumentRevision();
	auto const document = context->GetCore().ass.get();
	return {.ass = document,
			.selection = &selection,
			.host = ContextHost(context, bound),
			.document_generation = generation,
			.workspace_invocation_active = context->lua_workspace_invocation_active,
			.is_current = [context, generation, revision, document] {
				return context->GetCore().ass.get() == document &&
					   context->subsController->GetDocumentGeneration() == generation &&
					   context->subsController->GetDocumentRevision() == revision;
			}};
}

class WxMacro final : public cmd::Command {
	AutomationMacro& macro;
	std::shared_ptr<AutomationHost> const& bound_host;

	public:
	WxMacro(AutomationMacro& macro, std::shared_ptr<AutomationHost> const& host) : macro(macro), bound_host(host) {}
	[[nodiscard]] AutomationMacro& RuntimeMacro() const { return macro; }
	[[nodiscard]] std::shared_ptr<AutomationHost> const& BoundHost() const { return bound_host; }
	[[nodiscard]] const char *name() const override { return macro.Name().c_str(); }
	wxString StrMenu(agi::Context const *) const override { return to_wx(macro.Display()); }
	wxString StrDisplay(agi::Context const *) const override { return to_wx(macro.Display()); }
	[[nodiscard]] wxString StrHelp() const override { return to_wx(macro.Help()); }
	[[nodiscard]] int Type() const override { return macro.Type(); }
	bool Validate(agi::Context const *context) override {
		WxSelection selection(context->GetCore().selectionController.get());
		return macro.Validate(MakeContext(context, selection, bound_host));
	}
	bool IsActive(agi::Context const *context) override {
		WxSelection selection(context->GetCore().selectionController.get());
		return macro.IsActive(MakeContext(context, selection, bound_host));
	}
	void operator()(agi::Context *context) override {
		WxSelection selection(context->GetCore().selectionController.get());
		macro.Run(MakeContext(context, selection, bound_host));
	}
};

class WxExportFilter final : public ExportFilter {
	AutomationExportFilter& filter;
	std::shared_ptr<AutomationHost> const& bound_host;
	std::shared_ptr<AutomationHost> automation_host;
	std::shared_ptr<AutomationDialog> dialog;

	public:
	WxExportFilter(AutomationExportFilter& filter, std::shared_ptr<AutomationHost> const& host)
		: ExportFilter(filter.Name(), filter.Description(), filter.Priority()), filter(filter), bound_host(host) {}
	void ClearInvocationHost() { automation_host.reset(); }
	wxWindow *GetConfigDialogWindow(wxWindow *parent, agi::Context *context) override {
		WxSelection selection(context->GetCore().selectionController.get());
		auto invocation_context = MakeContext(context, selection, bound_host);
		automation_host = invocation_context.host;
		dialog = filter.GenerateConfigDialog(invocation_context);
		if (!dialog) return nullptr;
		filter.LoadSettings(*invocation_context.ass, *dialog);
		return CreateAutomationDialogWindow(*dialog, parent);
	}
	void LoadSettings(bool, agi::Context *context) override {
		automation_host = ContextHost(context, bound_host);
		if (dialog) filter.SaveSettings(*context->GetCore().ass, *dialog);
	}
	void ProcessSubs(AssFile *subs, wxWindow *parent) override {
		filter.ProcessSubs(subs, automation_host ? automation_host : bound_host, AutomationUiAnchor{parent});
	}
};
class WxFeatureRegistry final : public AutomationFeatureRegistry {
	std::shared_ptr<AutomationHost> bound_host;
	struct MacroAdapter {
		std::unique_ptr<WxMacro> pending;
		WxMacro *published = nullptr;
	};
	std::map<AutomationMacro *, MacroAdapter> macros;
	std::map<AutomationExportFilter *, std::unique_ptr<WxExportFilter>> filters;

	public:
	void SetAutomationHost(std::shared_ptr<AutomationHost> host) {
		bound_host = std::move(host);
		for (auto& [filter, adapter] : filters) adapter->ClearInvocationHost();
	}
	cmd::Command *GetMacro(AutomationMacro& macro) {
		auto [entry, inserted] = macros.try_emplace(&macro);
		if (inserted) entry->second.pending = std::make_unique<WxMacro>(macro, bound_host);
		return entry->second.published ? entry->second.published : entry->second.pending.get();
	}
	ExportFilter *GetFilter(AutomationExportFilter& filter) {
		auto [entry, inserted] = filters.try_emplace(&filter);
		if (inserted) entry->second = std::make_unique<WxExportFilter>(filter, bound_host);
		return entry->second.get();
	}
	bool ActivateMacro(AutomationMacro& macro) override {
		if (cmd::get_if(macro.Name())) {
			macros.erase(&macro);
			return false;
		}
		GetMacro(macro);
		auto& entry = macros.at(&macro);
		entry.published = entry.pending.get();
		cmd::reg(std::move(entry.pending));
		return true;
	}
	void DeactivateMacro(AutomationMacro& macro) override {
		auto entry = macros.find(&macro);
		if (entry == macros.end()) return;
		if (entry->second.published && cmd::get_if(macro.Name()) == entry->second.published)
			cmd::unreg(macro.Name());
		macros.erase(entry);
	}
	void ActivateFilter(AutomationExportFilter& filter) override {
		auto adapter = GetFilter(filter);
		AssExportFilterChain::Register(adapter);
		filter.SetName(adapter->GetName());
	}
	void DeactivateFilter(AutomationExportFilter& filter) override { filters.erase(&filter); }
	void Clear() {
		while (!macros.empty()) DeactivateMacro(*macros.begin()->first);
		filters.clear();
	}
};

class WxScript final : public Script {
	std::shared_ptr<WxFeatureRegistry> registry = std::make_shared<WxFeatureRegistry>();
	std::unique_ptr<AutomationScriptInstance> runtime;

	public:
	explicit WxScript(std::unique_ptr<AutomationScriptInstance> runtime)
		: Script(runtime->GetFilename()), runtime(std::move(runtime)) { this->runtime->SetFeatureRegistry(registry); }
	void Reload() override {
		registry->Clear();
		runtime->Reload();
	}
	[[nodiscard]] std::string GetName() const override { return runtime->GetName(); }
	[[nodiscard]] std::string GetDescription() const override { return runtime->GetDescription(); }
	[[nodiscard]] std::string GetAuthor() const override { return runtime->GetAuthor(); }
	[[nodiscard]] std::string GetVersion() const override { return runtime->GetVersion(); }
	[[nodiscard]] bool GetLoadedState() const override { return runtime->GetLoadedState(); }
	[[nodiscard]] std::string GetEngineName() const override { return runtime->GetEngineName(); }
	[[nodiscard]] std::vector<cmd::Command *> GetMacros() const override {
		std::vector<cmd::Command *> result;
		for (auto macro : runtime->GetRuntimeMacros()) result.push_back(registry->GetMacro(*macro));
		return result;
	}
	[[nodiscard]] std::vector<ExportFilter *> GetFilters() const override {
		std::vector<ExportFilter *> result;
		for (auto filter : runtime->GetRuntimeFilters()) result.push_back(registry->GetFilter(*filter));
		return result;
	}
	void CommitPendingFeatures() override { runtime->CommitPendingFeatures(); }
	void ValidateApplicationActivation() override { runtime->ValidateApplicationActivation(); }
	[[nodiscard]] std::optional<AutomationRuntimeStateSnapshot> TryGetRuntimeStateSnapshot() const override { return runtime->TryGetRuntimeStateSnapshot(); }
	void SetRuntimeTraceSink(AutomationRuntimeTraceSink *sink) override { runtime->SetRuntimeTraceSink(sink); }
	void SetAutomationHost(std::shared_ptr<AutomationHost> host) override {
		registry->SetAutomationHost(host);
		runtime->SetAutomationHost(std::move(host));
	}
	void SetDebugSession(AutomationDebugSession *session) override { runtime->SetDebugSession(session); }
};
}

std::shared_ptr<AutomationRuntimeServices> CreateWxAutomationRuntimeServices() { return std::make_shared<WxRuntimeServices>(); }

std::unique_ptr<Script> CreateWxAutomationScript(std::unique_ptr<AutomationScriptInstance> runtime) {
	return runtime ? std::make_unique<WxScript>(std::move(runtime)) : nullptr;
}

std::string_view GetLuaMacroExecutionId(cmd::Command const *command) {
	auto adapter = dynamic_cast<WxMacro const *>(command);
	return adapter ? adapter->RuntimeMacro().ExecutionId() : std::string_view{};
}

bool ValidateLuaMacroForWorkspace(cmd::Command *command, agi::Context const *context,
								  std::shared_ptr<LuaWorkspaceRunRequest const> const& request, AutomationRunner& runner) {
	auto adapter = dynamic_cast<WxMacro *>(command);
	if (!adapter) throw AutomationError("The selected Workspace macro is not backed by Lua");
	WxSelection selection(context->GetCore().selectionController.get());
	return adapter->RuntimeMacro().ValidateForWorkspace(MakeContext(context, selection, adapter->BoundHost()), request, runner);
}

std::unique_ptr<Script> ScriptFactory::CreateFromFileForWorkspace(agi::fs::path const& filename,
																  std::shared_ptr<LuaWorkspaceRunRequest const> request, AutomationRunner& runner, bool *recognised_out) {
	bool recognised = agi::fs::HasExtension(filename, "lua") || agi::fs::HasExtension(filename, "moon");
	if (recognised_out) *recognised_out = recognised;
	if (!recognised) return {};
	return CreateWxAutomationScript(CreateLuaAutomationScriptInstanceForWorkspace(
		filename, CreateWxAutomationRuntimeServices(), std::move(request), runner));
}
}
