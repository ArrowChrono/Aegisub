#pragma once

class wxWindow;

namespace Automation4 {
class AutomationDialog;

wxWindow *CreateAutomationDialogWindow(AutomationDialog& model, wxWindow *parent);
}
