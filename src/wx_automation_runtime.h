#pragma once

#include <memory>

namespace Automation4 {
class AutomationRuntimeServices;
class AutomationScriptInstance;
class Script;

std::shared_ptr<AutomationRuntimeServices> CreateWxAutomationRuntimeServices();
std::unique_ptr<Script> CreateWxAutomationScript(std::unique_ptr<AutomationScriptInstance> instance);
}
