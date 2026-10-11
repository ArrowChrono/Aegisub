#include "app_runtime_facilities.h"

#include "app_runtime.h"

#include "auto4_base.h"
#include "auto4_lua_factory.h"
#include "wx_automation_runtime.h"
#include "automation/engine/automation_engine_registry.h"
#include "automation/automation_debug_service.h"
#ifdef WITH_PLUGIN_BRIDGE
#include "coreclr/dotnet_automation_engine.h"
#endif
#include "export_fixstyle.h"
#include "export_framerate.h"
#include "options.h"
#include "subtitles_provider_libass.h"

#include <libaegisub/exception.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>

#include <utility>

void InitializeRuntimeOptionalFacilities(AppRuntimeInitOptions const& options) {
	if (options.register_automation_script_factory) {
		if (!Automation4::AutomationEngineRegistry::FindEngine("Lua"))
			Automation4::AutomationEngineRegistry::Register(agi::make_unique<Automation4::LuaAutomationEngine>(Automation4::CreateWxAutomationRuntimeServices()));
#ifdef WITH_PLUGIN_BRIDGE
		if (!Automation4::AutomationEngineRegistry::FindEngine("Plugin Bridge"))
			Automation4::AutomationEngineRegistry::Register(agi::make_unique<Automation4::DotNetAutomationEngine>());
#endif
	}

	if (options.warm_subtitles_provider_font_cache)
		libass::CacheFonts();

	if (options.create_global_script_manager) {
		auto managed_plugin_root = agi::fs::path();
#ifdef WITH_PLUGIN_BRIDGE
		managed_plugin_root = config::path->Decode("?user/managed-plugins");
#endif
		config::global_scripts = new Automation4::AutoloadScriptManager(
			OPT_GET("Path/Automation/Autoload")->GetString(),
			std::move(managed_plugin_root));
	}

	if (options.shell_mode == RuntimeShellMode::Gui && !config::automation_debug_service)
		config::automation_debug_service = new Automation4::AutomationDebugService();

	if (options.register_export_filters) {
		AssExportFilterChain::Register(agi::make_unique<AssFixStylesFilter>());
		AssExportFilterChain::Register(agi::make_unique<AssTransformFramerateFilter>());
	}

	if (options.optional_facility_host.register_subtitle_format_extensions)
		options.optional_facility_host.register_subtitle_format_extensions();

	if (options.install_png_handler) {
		if (!options.optional_facility_host.install_png_image_handler)
			throw agi::InternalError("AppRuntime requested PNG handler installation without a host hook.");
		options.optional_facility_host.install_png_image_handler();
	}
}

void CleanupRuntimeOptionalFacilities() {
	if (config::global_scripts) {
		delete config::global_scripts;
		config::global_scripts = nullptr;
	}

	if (config::automation_debug_service) {
		delete config::automation_debug_service;
		config::automation_debug_service = nullptr;
	}
	AssExportFilterChain::Clear();
}

void ShutdownRuntimeApplicationServices() {
#ifdef WITH_PLUGIN_BRIDGE
	Automation4::ShutdownManagedPluginRuntime();
#endif
}
