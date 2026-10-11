#pragma once

#include "automation_host.h"
#include "automation_invocation_observer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>

class AssDialogue;
class AssFile;

namespace Automation4 {
class AutomationDialog;
class AutomationRunner;
struct LuaWorkspaceRunRequest;

class AutomationSelection {
	public:
	virtual ~AutomationSelection() = default;
	virtual std::set<AssDialogue *> const& GetSelectedSet() const = 0;
	virtual AssDialogue *GetActiveLine() const = 0;
	virtual void SetSelectionAndActive(std::set<AssDialogue *> selection, AssDialogue *active) = 0;
};

struct AutomationMacroContext {
	AssFile *ass = nullptr;
	AutomationSelection *selection = nullptr;
	std::shared_ptr<AutomationHost> host;
	uint64_t document_generation = 0;
	bool workspace_invocation_active = false;
	std::function<bool()> is_current;
};

enum AutomationMacroFlags {
	MACRO_NORMAL = 0,
	MACRO_TOGGLE = 1,
	MACRO_RADIO = 2,
	MACRO_VALIDATE = 4,
	MACRO_DYNAMIC_NAME = 8,
	MACRO_DYNAMIC_HELP = 16,
	MACRO_DYNAMIC_ICON = 32
};

class AutomationMacro {
	public:
	virtual ~AutomationMacro() = default;
	virtual std::string const& Name() const = 0;
	virtual std::string const& Display() const = 0;
	virtual std::string const& Help() const = 0;
	virtual int Type() const = 0;
	virtual std::string_view ExecutionId() const = 0;
	virtual bool Validate(AutomationMacroContext const& context) = 0;
	virtual bool ValidateForWorkspace(AutomationMacroContext const& context,
									  std::shared_ptr<LuaWorkspaceRunRequest const> const& request, AutomationRunner& runner) = 0;
	virtual bool IsActive(AutomationMacroContext const& context) = 0;
	virtual AutomationInvocationOutcome Run(AutomationMacroContext const& context) = 0;
};

class AutomationExportFilter {
	public:
	virtual ~AutomationExportFilter() = default;
	virtual std::string const& Name() const = 0;
	virtual void SetName(std::string name) = 0;
	virtual std::string const& Description() const = 0;
	virtual int Priority() const = 0;
	virtual std::shared_ptr<AutomationDialog> GenerateConfigDialog(AutomationMacroContext const& context) = 0;
	virtual void ProcessSubs(AssFile *subs, std::shared_ptr<AutomationHost> host, AutomationUiAnchor anchor = {}) = 0;
	std::string SettingsIdentifier() const;
	void LoadSettings(AssFile& file, AutomationDialog& dialog) const;
	void SaveSettings(AssFile& file, AutomationDialog& dialog) const;
};

class AutomationFeatureRegistry {
	public:
	virtual ~AutomationFeatureRegistry() = default;
	virtual bool ActivateMacro(AutomationMacro& macro) = 0;
	virtual void DeactivateMacro(AutomationMacro& macro) = 0;
	virtual void ActivateFilter(AutomationExportFilter& filter) = 0;
	virtual void DeactivateFilter(AutomationExportFilter& filter) = 0;
};
}
