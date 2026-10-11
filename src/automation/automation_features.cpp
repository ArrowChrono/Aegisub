#include "automation_features.h"

#include "automation_ui.h"
#include "../ass_file.h"
#include "../string_codec.h"

namespace Automation4 {
std::string AutomationExportFilter::SettingsIdentifier() const {
	return inline_string_encode(Name());
}

void AutomationExportFilter::LoadSettings(AssFile& file, AutomationDialog& dialog) const {
	auto const& value = file.Properties.automation_settings[SettingsIdentifier()];
	if (!value.empty())
		dialog.Unserialise(value);
}

void AutomationExportFilter::SaveSettings(AssFile& file, AutomationDialog& dialog) const {
	file.Properties.automation_settings[SettingsIdentifier()] = dialog.Serialise();
}
}
