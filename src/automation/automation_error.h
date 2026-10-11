#pragma once

#include <libaegisub/exception.h>

namespace Automation4 {
DEFINE_EXCEPTION(AutomationError, agi::Exception);
DEFINE_EXCEPTION(ScriptLoadError, AutomationError);
DEFINE_EXCEPTION(MacroRunError, AutomationError);
}
