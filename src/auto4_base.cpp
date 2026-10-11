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
#include "wx_automation_dialog.h"
#include "wx_automation_runtime.h"

#include "ass_file.h"
#include "ass_style.h"
#include "automation/engine/automation_engine_registry.h"
#include "automation/automation_live_host.h"
#include "automation/automation_debug_service.h"
#include "automation/local_script_references.h"
#include "compat.h"
#ifdef WITH_PLUGIN_BRIDGE
#include "coreclr/dotnet_automation_engine.h"
#include "coreclr/host.h"
#include "coreclr/managed_plugin_activation.h"
#endif
#include "dialog_progress.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "perf_trace.h"
#include "string_codec.h"
#include "subs_controller.h"
#include "ui_dispatch.h"
#include "ui_services.h"

#include <libaegisub/format.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/string_utils.h>
#include <libaegisub/split.h>

#include <chrono>
#include <exception>
#include <optional>
#include <utility>
#include <vector>

#include <wx/dcmemory.h>
#include <wx/log.h>
#include <wx/sizer.h>

#ifdef WIN32
#include "automation/automation_text_extents.h"
#endif

namespace Automation4 {
	namespace {
		using AutoloadTimingClock = std::chrono::steady_clock;

		double AutoloadDurationMs(
			AutoloadTimingClock::time_point started,
			AutoloadTimingClock::time_point finished = AutoloadTimingClock::now()) noexcept
		{
			return std::chrono::duration<double, std::milli>(finished - started).count();
		}

		std::unique_ptr<Script> WrapScriptInstance(std::unique_ptr<AutomationScriptInstance> instance)
		{
			if (!instance)
				return nullptr;

			if (auto *script = dynamic_cast<Script *>(instance.get())) {
				instance.release();
				return std::unique_ptr<Script>(script);
			}

			return CreateWxAutomationScript(std::move(instance));
		}

		std::vector<std::unique_ptr<Script>> WrapScriptInstances(std::vector<std::unique_ptr<AutomationScriptInstance>> instances) {
			std::vector<std::unique_ptr<Script>> result;
			result.reserve(instances.size());
			for (auto& instance : instances)
				result.emplace_back(WrapScriptInstance(std::move(instance)));
			return result;
		}

		void AddFilterEntry(
			std::vector<std::pair<std::string, std::string>>& entries,
			std::string engine_name,
			std::string filename_pattern)
		{
			if (engine_name.empty() || filename_pattern.empty())
				return;

			auto const entry = std::make_pair(std::move(engine_name), std::move(filename_pattern));
			if (find(entries.begin(), entries.end(), entry) == entries.end())
				entries.emplace_back(entry);
		}

		struct ScriptLoadAttempt {
			std::unique_ptr<Script> script;
			bool recognised = false;
		};

		using AutoloadReloadResult = AutomationScriptLoadResult;

		class FailedScript final : public Script {
			std::string description;

		public:
			FailedScript(agi::fs::path const& filename, std::string description)
			: Script(filename)
			, description(std::move(description))
			{
			}

			void Reload() override { }

			std::string GetName() const override { return agi::fs::PathToString(GetFilename().stem()); }
			std::string GetDescription() const override { return description; }
			std::string GetAuthor() const override { return ""; }
			std::string GetVersion() const override { return ""; }
			bool GetLoadedState() const override { return false; }

			std::vector<cmd::Command*> GetMacros() const override { return {}; }
			std::vector<ExportFilter*> GetFilters() const override { return {}; }
		};

		std::string DescribeCurrentException()
		{
			try {
				throw;
			}
			catch (agi::Exception const& e) {
				return e.GetMessage();
			}
			catch (std::exception const& e) {
				return e.what();
			}
			catch (...) {
				return "Unknown error";
			}
		}

		ScriptLoadAttempt LoadWxAutomationScript(agi::fs::path const& script_filename) {
			auto attempt = LoadAutomationScript(script_filename);
			return {.script = WrapScriptInstance(std::move(attempt.script)), .recognised = attempt.recognised};
		}

		ScriptLoadAttempt LoadManagedApplicationPlugin(
			agi::fs::path const& script_filename) {
			auto attempt = LoadWxAutomationScript(script_filename);
			if (!attempt.script || !attempt.script->GetLoadedState()) return attempt;
			try {
				attempt.script->ValidateApplicationActivation();
			}
			catch (...) {
				attempt.recognised = true;
				attempt.script = agi::make_unique<FailedScript>(
					script_filename, DescribeCurrentException());
			}
			return attempt;
		}

#ifdef WITH_PLUGIN_BRIDGE
		/// Prefer the managed-store filename, then the shorter installed payload name.
		std::optional<agi::fs::path> FindAppLocalPluginManifest(agi::fs::path const& plugin_dir)
		{
			static constexpr char const* names[] = {
				"plugin.aegisub-plugin.json",
				"plugin.json",
			};
			for (auto const* name : names) {
				auto candidate = plugin_dir / name;
				if (agi::fs::FileExists(candidate))
					return candidate;
			}
			return std::nullopt;
		}

		/// Load a discovered Plugin Bridge manifest and eagerly validate the
		/// runtime payload so menus/services only appear when the DLL matches.
		ScriptLoadAttempt LoadValidatedPluginBridgeManifest(
			agi::fs::path const& manifest_path)
		{
			ScriptLoadAttempt attempt;
			try {
				auto instance = LoadPluginBridgeManifest(manifest_path);
				if (!instance) {
					attempt.recognised = false;
					return attempt;
				}
				attempt.recognised = true;
				attempt.script = WrapScriptInstance(std::move(instance));
				if (!attempt.script || !attempt.script->GetLoadedState())
					return attempt;
				attempt.script->ValidateApplicationActivation();
			}
			catch (...) {
				attempt.recognised = true;
				attempt.script = agi::make_unique<FailedScript>(
					manifest_path, DescribeCurrentException());
			}
			return attempt;
		}

		/// Discover host-bundled plugins under <exe>/plugins/<id>/ and load them
		/// through the normal Automation engine path. Macros self-register from
		/// each plugin's manifest, so the host never hardcodes per-plugin menus.
		void LoadAppLocalPlugins(AutoloadReloadResult& result)
		{
			agi::fs::path plugins_root;
			try {
				plugins_root = agi::coreclr::GetCurrentExecutableDirectory() / "plugins";
			}
			catch (...) {
				result.diagnostics.push_back({
					"Could not resolve the app-local plugins directory: " +
						DescribeCurrentException(),
					true});
				++result.error_count;
				return;
			}

			if (!agi::fs::DirectoryExists(plugins_root))
				return;

			try {
				for (auto entry : agi::fs::DirectoryIterator(plugins_root, "*")) {
					auto plugin_dir = plugins_root / agi::fs::PathFromString(entry);
					if (!agi::fs::DirectoryExists(plugin_dir))
						continue;

					auto dirname = agi::fs::PathToString(plugin_dir.filename());
					// Runtime host pack lives next to plugins; it is not a plugin.
					if (dirname == "coreclr")
						continue;

					auto manifest = FindAppLocalPluginManifest(plugin_dir);
					if (!manifest)
						continue;

					// Eager validate (same bar as managed-store plugins) so a
					// broken or mismatched payload does not publish macros/services.
					auto attempt = LoadValidatedPluginBridgeManifest(*manifest);
					if (!attempt.script) {
						if (attempt.recognised) {
							++result.error_count;
							result.diagnostics.push_back({
								"App-local plugin '" + dirname +
									"' was recognised but produced no script instance",
								true});
						}
						continue;
					}

					if (!attempt.script->GetLoadedState())
						++result.error_count;
					result.scripts.emplace_back(std::move(attempt.script));
				}
			}
			catch (...) {
				result.diagnostics.push_back({
					"Could not enumerate app-local plugins: " + DescribeCurrentException(),
					true});
				++result.error_count;
			}
		}
#endif

		AutoloadReloadResult LoadWxAutoloadScripts(
			std::string const& path,
			agi::fs::path const& managed_plugin_root) {
			std::vector<agi::fs::path> directories;
			for (auto tok : agi::Split(path, '|'))
				directories.push_back(config::path->Decode(agi::str(tok)));
			auto result = LoadAutoloadScripts(directories);
			auto const regular_script_count = result.scripts.size();

#ifdef WITH_PLUGIN_BRIDGE
			// Bundled plugins under <exe>/plugins/<id>/ register macros from their
			// manifests. Missing packages simply do not appear (auto4 semantics).
			LoadAppLocalPlugins(result);

			if (!managed_plugin_root.empty()) {
				agi::coreclr::ManagedPluginActivationStore store(managed_plugin_root);
				auto discovery = store.Discover();
				for (auto& diagnostic : discovery.diagnostics) {
					if (diagnostic.error) ++result.error_count;
					result.diagnostics.push_back({
						"Managed plugin" +
							(diagnostic.plugin_id.empty()
								? std::string()
								: " '" + diagnostic.plugin_id + "'") +
							": " + diagnostic.message,
						diagnostic.error});
				}

				for (auto const& candidate : discovery.candidates) {
					auto commit_automatic_rollback = [&](std::string const& failed_version,
						std::string const& fallback_version) {
						try {
							if (store.CommitAutomaticRollback(
								candidate.plugin_id,
								candidate.generation,
								failed_version,
								fallback_version))
								return true;
							result.diagnostics.push_back({
								"Managed plugin '" + candidate.plugin_id +
								"' activation state changed while its fallback was loading; "
								"the stale fallback result was ignored",
								true});
						}
						catch (std::exception const& error) {
							result.diagnostics.push_back({
								"Managed plugin '" + candidate.plugin_id +
								"' loaded its fallback but could not persist rollback: " +
								error.what(),
								true});
						}
						++result.error_count;
						return false;
					};

					auto active_attempt = LoadManagedApplicationPlugin(
						candidate.manifest_path);
					if (active_attempt.script && active_attempt.script->GetLoadedState()) {
						if (candidate.rollback_from_version) {
							if (!commit_automatic_rollback(
									*candidate.rollback_from_version, candidate.version))
								continue;
							result.diagnostics.push_back({
								"Managed plugin '" + candidate.plugin_id +
								"' rolled back from version '" +
								*candidate.rollback_from_version + "' to '" +
								candidate.version + "'", false});
						}
						result.scripts.emplace_back(std::move(active_attempt.script));
						continue;
					}

					if (candidate.fallback) {
						auto fallback_attempt = LoadManagedApplicationPlugin(
							candidate.fallback->manifest_path);
						if (fallback_attempt.script &&
							fallback_attempt.script->GetLoadedState()) {
							if (!commit_automatic_rollback(
									candidate.version, candidate.fallback->version))
								continue;
							result.diagnostics.push_back({
								"Managed plugin '" + candidate.plugin_id +
								"' failed to load version '" + candidate.version +
								"' and rolled back to '" +
								candidate.fallback->version + "'", false});
							result.scripts.emplace_back(std::move(fallback_attempt.script));
							continue;
						}
						result.diagnostics.push_back({
							"Managed plugin '" + candidate.plugin_id +
							"' fallback version '" + candidate.fallback->version +
							"' also failed to load" +
							(fallback_attempt.script
								? ": " + fallback_attempt.script->GetDescription()
								: std::string(": manifest was not recognized")),
							true});
					}

					if (active_attempt.script) {
						++result.error_count;
						result.scripts.emplace_back(std::move(active_attempt.script));
					}
					else {
						++result.error_count;
						result.diagnostics.push_back({
							"Managed plugin '" + candidate.plugin_id +
							"' manifest was not recognized by an Automation engine",
							true});
					}
				}
			}
#else
			(void)managed_plugin_root;
#endif

			for (auto index = regular_script_count; index < result.scripts.size(); ++index) {
				auto const& script = result.scripts[index];
				if (script && !script->GetLoadedState())
					result.diagnostics.push_back({.message = "Failed to load Automation script '" + agi::fs::PathToString(script->GetFilename()) +
															 "':\n" + script->GetDescription(),
												  .error = true});
			}

			return result;
		}
	}

	bool CalculateTextExtents(AssStyle *style, std::string const& text, double &width, double &height, double &descent, double &extlead)
	{
#ifdef WIN32
		return CalculateAutomationTextExtents(*style, text, width, height, descent, extlead);
#else
		width = height = descent = extlead = 0;

		double fontsize = style->fontsize * 64;
		double spacing = style->spacing * 64;

		wxMemoryDC thedc;

		// fix fontsize to be 72 DPI
		//fontsize = -FT_MulDiv((int)(fontsize+0.5), 72, thedc.GetPPI().y);

		// now try to get a font!
		// use the font list to get some caching... (chance is the script will need the same font very often)
		// USING wxTheFontList SEEMS TO CAUSE BAD LEAKS!
		//wxFont *thefont = wxTheFontList->FindOrCreateFont(
		wxFont thefont(
			(int)fontsize,
			wxFONTFAMILY_DEFAULT,
			style->italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL,
			style->bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL,
			style->underline,
			to_wx(style->font),
			wxFONTENCODING_SYSTEM); // FIXME! make sure to get the right encoding here, make some translation table between windows and wx encodings
		thedc.SetFont(thefont);

		wxString wtext(to_wx(text));
		if (spacing) {
			// If there's inter-character spacing, kerning info must not be used, so calculate width per character
			// NOTE: Is kerning actually done either way?!
			for (auto const& wc : wtext) {
				int a, b, c, d;
				thedc.GetTextExtent(wc, &a, &b, &c, &d);
				double scaling = fontsize / (double)(b > 0 ? b : 1); // semi-workaround for missing OS/2 table data for scaling
				width += (a + spacing)*scaling;
				height = b > height ? b*scaling : height;
				descent = c > descent ? c*scaling : descent;
				extlead = d > extlead ? d*scaling : extlead;
			}
		} else {
			// If the inter-character spacing should be zero, kerning info can (and must) be used, so calculate everything in one go
			wxCoord lwidth, lheight, ldescent, lextlead;
			thedc.GetTextExtent(wtext, &lwidth, &lheight, &ldescent, &lextlead);
			double scaling = fontsize / (double)(lheight > 0 ? lheight : 1); // semi-workaround for missing OS/2 table data for scaling
			width = lwidth*scaling; height = lheight*scaling; descent = ldescent*scaling; extlead = lextlead*scaling;
		}

		// Compensate for scaling
		width = style->scalex / 100 * width / 64;
		height = style->scaley / 100 * height / 64;
		descent = style->scaley / 100 * descent / 64;
		extlead = style->scaley / 100 * extlead / 64;

		return true;
#endif
	}

	// ProgressSink
	ProgressSink::ProgressSink(agi::ProgressSink *impl, BackgroundScriptRunner *bsr)
	: impl(impl)
	, bsr(bsr)
	, trace_level(OPT_GET("Automation/Trace Level")->GetInt())
	{
	}

	void ProgressSink::ShowDialog(ScriptDialog *config_dialog)
	{
		agi::ui::MainInvoke([=] {
			auto duration_ms = [](auto const& started) {
				return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
			};

			wxDialog w; // container dialog box
			w.SetExtraStyle(wxWS_EX_VALIDATE_RECURSIVELY);
			w.Create(bsr->GetParentWindow(), -1, to_wx(bsr->GetTitle()));
			auto s = new wxBoxSizer(wxHORIZONTAL); // sizer for putting contents in
			wxWindow *ww = config_dialog->CreateWindow(&w); // generate actual dialog contents
			s->Add(ww, 0, wxALL, 5); // add contents to dialog

			auto const outer_fit_started = std::chrono::steady_clock::now();
			w.SetSizerAndFit(s);
			perf_trace::ObserveLuaDialogPhase("outer_dialog_fit", -1, -1, duration_ms(outer_fit_started));

			auto const center_started = std::chrono::steady_clock::now();
			w.CenterOnParent();
			perf_trace::ObserveLuaDialogPhase("outer_dialog_center", -1, -1, duration_ms(center_started));

			w.ShowModal();
		});
	}

	int ProgressSink::ShowDialog(wxDialog *dialog)
	{
		return agi::ui::MainInvoke([dialog] { return dialog->ShowModal(); });
	}

	void ProgressSink::ShowDialog(AutomationDialog& model) {
		class DialogAdapter final : public ScriptDialog {
			AutomationDialog& model;

			public:
			explicit DialogAdapter(AutomationDialog& model) : model(model) {}
			wxWindow *CreateWindow(wxWindow *parent) override { return CreateAutomationDialogWindow(model, parent); }
		} adapter(model);
		ShowDialog(&adapter);
	}

	std::vector<agi::fs::path> ProgressSink::RequestOpenFiles(AutomationOpenFileDialogRequest const& request)
	{
		return bsr->RequestOpenFiles(request);
	}

	agi::fs::path ProgressSink::RequestSaveFile(AutomationSaveFileDialogRequest const& request)
	{
		return bsr->RequestSaveFile(request);
	}

	BackgroundScriptRunner::BackgroundScriptRunner(wxWindow *parent, std::string const& title, std::shared_ptr<agi::FileDialogService> file_dialog_service)
	: impl(std::make_unique<DialogProgress>(parent, to_wx(title)))
	, parent(parent)
	, title(title)
	, file_dialog_service(std::move(file_dialog_service))
	{
	}

	BackgroundScriptRunner::BackgroundScriptRunner(
		std::unique_ptr<agi::BackgroundRunner> impl,
		wxWindow *parent,
		std::string title,
		std::shared_ptr<agi::FileDialogService> file_dialog_service)
	: impl(std::move(impl))
	, parent(parent)
	, title(std::move(title))
	, file_dialog_service(std::move(file_dialog_service))
	{
	}

	BackgroundScriptRunner::~BackgroundScriptRunner()
	{
	}

	void BackgroundScriptRunner::Run(std::function<void(AutomationProgress *)> task) {
		impl->Run([&](agi::ProgressSink *ps) {
			ProgressSink aps(ps, this);
			task(&aps);
		});
	}

	wxWindow *BackgroundScriptRunner::GetParentWindow() const
	{
		if (auto* window = dynamic_cast<wxWindow *>(impl.get()))
			return window;
		return parent;
	}

	std::string BackgroundScriptRunner::GetTitle() const
	{
		if (auto* dialog = dynamic_cast<wxDialog *>(impl.get()))
			return from_wx(dialog->GetTitle());
		return title;
	}

	std::vector<agi::fs::path> BackgroundScriptRunner::RequestOpenFiles(AutomationOpenFileDialogRequest const& request) const
	{
		if (!file_dialog_service)
			return {};

		if (request.multiple) {
			return file_dialog_service->RequestOpenFiles({
				request.message,
				"",
				request.file,
				"",
				request.wildcard,
				request.dir,
				request.must_exist
			});
		}

		auto path = file_dialog_service->RequestOpenFile({
			request.message,
			"",
			request.file,
			"",
			request.wildcard,
			request.dir,
			request.must_exist
		});
		if (path.empty())
			return {};
		return {std::move(path)};
	}

	agi::fs::path BackgroundScriptRunner::RequestSaveFile(AutomationSaveFileDialogRequest const& request) const
	{
		if (!file_dialog_service)
			return {};

		return file_dialog_service->RequestSaveFile({
			request.message,
			"",
			request.file,
			"",
			request.wildcard,
			request.dir,
			request.prompt_overwrite
		});
	}

	// Script
	Script::Script(agi::fs::path const& filename)
	: filename(filename)
	{
	}

	// ScriptManager
	ScriptManager::ScriptManager()
		: AutomationScriptCollection<Script>(
			  [](AutomationScriptMutation mutation, std::shared_ptr<AutomationDebugSession> const& owner) {
				  if (mutation == AutomationScriptMutation::Replace) {
					  agi::ui::VerifyAccess();
					  if (!config::automation_debug_service || !config::automation_debug_service->OwnsLocalSession(owner))
						  throw AutomationError("Cannot replace a script without owning the active Lua Workspace session");
					  return;
				  }
				  if (!config::automation_debug_service || !config::automation_debug_service->HasLocalSession())
					  return;
				  if (mutation == AutomationScriptMutation::Reload) {
					  if (!config::automation_debug_service->OwnsLocalSession(owner) || owner->GetStateSnapshot().invocation_active)
						  throw AutomationError("Cannot reload scripts while a Lua Workspace invocation is active");
					  return;
				  }
				  throw AutomationError("Cannot change loaded scripts while a Lua Workspace invocation is active");
			  },
			  [this] { ScriptsChanged(); }) {
	}

	void ScriptManager::Add(std::unique_ptr<Script> script) {
		AutomationScriptCollection<Script>::Add(std::move(script));
	}

	void ScriptManager::Remove(Script *script)
	{
		AutomationScriptCollection<Script>::Remove(script);
	}

	void ScriptManager::RemoveAll()
	{
		AutomationScriptCollection<Script>::RemoveAll();
	}

	void ScriptManager::Reload(Script *script, std::shared_ptr<AutomationDebugSession> const& owner) {
		AutomationScriptCollection<Script>::Reload(script, owner);
	}

	Script *ScriptManager::ReplaceForWorkspace(
		Script *current,
		std::unique_ptr<Script> replacement,
		std::shared_ptr<AutomationDebugSession> const& owner) {
		return Replace(current, std::move(replacement), owner);
	}

	const std::vector<cmd::Command*>& ScriptManager::GetMacros()
	{
		macros.clear();
		for (auto& script : scripts) {
			std::vector<cmd::Command*> sfs = script->GetMacros();
			copy(sfs.begin(), sfs.end(), back_inserter(macros));
		}
		return macros;
	}

	// AutoloadScriptManager
	AutoloadScriptManager::AutoloadScriptManager(
		std::string path,
		agi::fs::path managed_plugin_root)
	: path(std::move(path))
	, managed_plugin_root(std::move(managed_plugin_root))
	{
	}

	void AutoloadScriptManager::ApplyReloadedScripts(
		std::vector<std::unique_ptr<AutomationScriptInstance>> loaded_scripts,
		int error_count,
		std::vector<std::pair<std::string, bool>> const& diagnostics) {
		if (config::automation_debug_service && config::automation_debug_service->HasLocalSession()) {
			pending_reload = true;
			LOG_W("automation/reload") << "Autoload refresh deferred until the Lua Workspace invocation finishes";
			return;
		}
		pending_reload = false;
		ReplaceAll(WrapScriptInstances(std::move(loaded_scripts)));

		for (auto const& [message, error] : diagnostics) {
			if (error)
				wxLogError(wxS("%s"), to_wx(message));
			else
				wxLogWarning(wxS("%s"), to_wx(message));
		}

		if (error_count == 1) {
			wxLogWarning(wxS("A script in the Automation autoload directory failed to load.\nPlease review the errors, fix them and use the Rescan Autoload Dir button in Automation Manager to load the scripts again."));
		}
		else if (error_count > 1) {
			wxLogWarning(wxS("Multiple scripts in the Automation autoload directory failed to load.\nPlease review the errors, fix them and use the Rescan Autoload Dir button in Automation Manager to load the scripts again."));
		}
	}

	void AutoloadScriptManager::ProcessPendingReload() {
		if (!pending_reload || (config::automation_debug_service && config::automation_debug_service->HasLocalSession()))
			return;
		ReloadAsync();
		pending_reload = false;
	}

	void AutoloadScriptManager::Reload()
	{
		reload_gate.Begin();
		auto result = LoadWxAutoloadScripts(path, managed_plugin_root);
		std::vector<std::pair<std::string, bool>> diagnostics;
		for (auto& diagnostic : result.diagnostics)
			diagnostics.emplace_back(std::move(diagnostic.message), diagnostic.error);
		ApplyReloadedScripts(
			std::move(result.scripts), result.error_count, std::move(diagnostics));
	}

	void AutoloadScriptManager::ReloadAsync()
	{
		auto ticket = reload_gate.Begin();
		auto path_copy = path;
		auto managed_plugin_root_copy = managed_plugin_root;
		auto const reload_id = ticket.Id();
		auto const scheduled_at = AutoloadTimingClock::now();
		LOG_I_IF(perf_trace::IsCategoryEnabled(perf_trace::Category::Log), "automation/autoload/timing")
			<< "reload_id=" << reload_id << " phase=scheduled";

		agi::dispatch::Background().Async([this, ticket, reload_id,
										   scheduled_at,
										   path_copy = std::move(path_copy),
										   managed_plugin_root_copy = std::move(managed_plugin_root_copy)] {
			auto const background_started = AutoloadTimingClock::now();
			auto const background_dispatch_delay_ms =
				AutoloadDurationMs(scheduled_at, background_started);
			auto result = std::make_shared<AutoloadReloadResult>(
				LoadWxAutoloadScripts(path_copy, managed_plugin_root_copy));
			auto const background_finished = AutoloadTimingClock::now();
			auto const background_load_ms =
				AutoloadDurationMs(background_started, background_finished);
			std::size_t loaded_script_count = 0;
			std::size_t macro_count = 0;
			std::size_t filter_count = 0;
			for (auto const& script : result->scripts) {
				if (!script || !script->GetLoadedState())
					continue;
				++loaded_script_count;
				if (auto *adapter = dynamic_cast<Script *>(script.get())) {
					macro_count += adapter->GetMacros().size();
					filter_count += adapter->GetFilters().size();
				}
				else {
					macro_count += script->GetRuntimeMacros().size();
					filter_count += script->GetRuntimeFilters().size();
				}
			}
			auto const script_count = result->scripts.size();
			auto const error_count = result->error_count;
			auto const diagnostic_count = result->diagnostics.size();
			LOG_I_IF(perf_trace::IsCategoryEnabled(perf_trace::Category::Log), "automation/autoload/timing")
				<< "reload_id=" << reload_id
				<< " phase=background_complete"
				<< " background_dispatch_delay_ms=" << background_dispatch_delay_ms
				<< " background_load_ms=" << background_load_ms
				<< " script_count=" << script_count
				<< " loaded_script_count=" << loaded_script_count
				<< " macro_count=" << macro_count
				<< " filter_count=" << filter_count
				<< " error_count=" << error_count
				<< " diagnostic_count=" << diagnostic_count;

			agi::dispatch::Main().Async([this, ticket, reload_id, result,
										 background_finished, background_dispatch_delay_ms, background_load_ms,
										 script_count, loaded_script_count, macro_count, filter_count,
										 error_count, diagnostic_count] {
				auto const main_started = AutoloadTimingClock::now();
				auto const main_queue_wait_ms =
					AutoloadDurationMs(background_finished, main_started);
				if (!ticket.IsAlive()) {
					LOG_I_IF(perf_trace::IsCategoryEnabled(perf_trace::Category::Log), "automation/autoload/timing")
						<< "reload_id=" << reload_id
						<< " phase=main_skipped reason=lifetime_expired"
						<< " main_queue_wait_ms=" << main_queue_wait_ms;
					return;
				}
				if (!ticket.IsCurrent()) {
					LOG_I_IF(perf_trace::IsCategoryEnabled(perf_trace::Category::Log), "automation/autoload/timing")
						<< "reload_id=" << reload_id
						<< " phase=main_skipped reason=superseded"
						<< " main_queue_wait_ms=" << main_queue_wait_ms;
					return;
				}
				std::vector<std::pair<std::string, bool>> diagnostics;
				for (auto& diagnostic : result->diagnostics)
					diagnostics.emplace_back(
						std::move(diagnostic.message), diagnostic.error);
				auto const apply_started = AutoloadTimingClock::now();
				ApplyReloadedScripts(
					std::move(result->scripts),
					result->error_count,
					std::move(diagnostics));
				auto const apply_ms = AutoloadDurationMs(apply_started);
				LOG_I_IF(perf_trace::IsCategoryEnabled(perf_trace::Category::Log), "automation/autoload/timing")
					<< "reload_id=" << reload_id
					<< " phase=main_complete"
					<< " background_dispatch_delay_ms=" << background_dispatch_delay_ms
					<< " background_load_ms=" << background_load_ms
					<< " main_queue_wait_ms=" << main_queue_wait_ms
					<< " apply_ms=" << apply_ms
					<< " script_count=" << script_count
					<< " loaded_script_count=" << loaded_script_count
					<< " macro_count=" << macro_count
					<< " filter_count=" << filter_count
					<< " error_count=" << error_count
					<< " diagnostic_count=" << diagnostic_count;
			});
		});
	}

	LocalScriptManager::LocalScriptManager(agi::Context *c)
	: context(c)
	, file_open_connection(c->GetCore().subsController->AddFileOpenListener(&LocalScriptManager::Reload, this))
	{
		AddScriptChangeListener(&LocalScriptManager::SaveLoadedList, this);
	}

	void LocalScriptManager::Reload()
	{
		auto core = context->GetCore();
		auto const& references = core.ass->Properties.automation_scripts;
		if (references.empty() && scripts.empty())
			return;
		auto result = LoadLocalScripts(references, core.subsController->Filename().parent_path(),
									   OPT_GET("Path/Automation/Base")->GetString());
		ReplaceAll(WrapScriptInstances(std::move(result.scripts)));
		for (auto const& diagnostic : result.diagnostics) {
			if (diagnostic.error)
				context->ShowError(diagnostic.message, "Automation");
			else
				context->ShowWarning(diagnostic.message, "Automation");
		}
	}

	void LocalScriptManager::SaveLoadedList()
	{
		agi::fs::path autobasefn(OPT_GET("Path/Automation/Base")->GetString());
		auto core = context->GetCore();
		std::vector<agi::fs::path> filenames;
		filenames.reserve(GetScripts().size());
		for (auto const& script : GetScripts())
			filenames.push_back(script->GetFilename());
		core.ass->Properties.automation_scripts = SerializeLocalScriptReferences(filenames, *core.path, autobasefn);
	}

	std::unique_ptr<Script> ScriptFactory::CreateFromFile(
		agi::fs::path const& filename,
		bool create_unknown,
		bool *recognised_out)
	{
		auto attempt = LoadAutomationScript(filename, create_unknown);
		if (recognised_out)
			*recognised_out = attempt.recognised;
		return WrapScriptInstance(std::move(attempt.script));
	}

	std::string ScriptFactory::GetWildcardStr()
	{
		std::string fnfilter, catchall;
		std::vector<std::pair<std::string, std::string>> entries;
		for (auto const& engine : AutomationEngineRegistry::GetEngines()) {
			if (!engine)
				continue;
			AddFilterEntry(entries, engine->EngineName(), engine->FilenamePattern());
		}

		for (auto const& entry : entries) {
			std::string filter(entry.second);
			agi::util::strings::replace_all_inplace(filter, ",", ";");
			fnfilter += agi::format("%s scripts (%s)|%s|", entry.first, entry.second, filter);
			catchall += filter + ";";
		}
		fnfilter += from_wx(_("All Files")) + " (*.*)|*.*";

		if (!catchall.empty())
			catchall.pop_back();

		if (entries.size() > 1)
			fnfilter = from_wx(_("All Supported Formats")) + "|" + catchall + "|" + fnfilter;

		return fnfilter;
	}

	std::string UnknownScript::GetDescription() const {
		return from_wx(_("File was not recognized as a script"));
	}
}
