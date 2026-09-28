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

#include "ass_file.h"
#include "ass_style.h"
#include "automation/engine/automation_engine_registry.h"
#include "automation/automation_live_host.h"
#include "automation/automation_debug_service.h"
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
#include <future>
#include <optional>
#include <utility>
#include <vector>

#include <wx/dcmemory.h>
#include <wx/log.h>
#include <wx/sizer.h>

#ifdef __WINDOWS__
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <libaegisub/charset_conv_win.h>
#endif

namespace Automation4 {
std::unique_ptr<BackgroundScriptRunner> AutomationUiProxy::CreateWorkspaceBackgroundScriptRunner(
	std::shared_ptr<LuaWorkspaceRunRequest const> const&, std::string const&) const {
	return {};
}

	namespace {
		using AutoloadTimingClock = std::chrono::steady_clock;

		double AutoloadDurationMs(
			AutoloadTimingClock::time_point started,
			AutoloadTimingClock::time_point finished = AutoloadTimingClock::now()) noexcept
		{
			return std::chrono::duration<double, std::milli>(finished - started).count();
		}

		class ScriptCompatibilityWrapper final : public Script {
			std::unique_ptr<AutomationScriptInstance> impl;

		public:
			explicit ScriptCompatibilityWrapper(std::unique_ptr<AutomationScriptInstance> impl)
			: Script(impl ? impl->GetFilename() : agi::fs::path())
			, impl(std::move(impl))
			{
			}

			void Reload() override { impl->Reload(); }
			std::string GetName() const override { return impl->GetName(); }
			std::string GetDescription() const override { return impl->GetDescription(); }
			std::string GetAuthor() const override { return impl->GetAuthor(); }
			std::string GetVersion() const override { return impl->GetVersion(); }
			bool GetLoadedState() const override { return impl->GetLoadedState(); }
			std::vector<cmd::Command*> GetMacros() const override { return impl->GetMacros(); }
			std::vector<ExportFilter*> GetFilters() const override { return impl->GetFilters(); }
			std::string GetEngineName() const override { return impl->GetEngineName(); }
			void ValidateApplicationActivation() override { impl->ValidateApplicationActivation(); }
			std::optional<AutomationRuntimeStateSnapshot> TryGetRuntimeStateSnapshot() const override { return impl->TryGetRuntimeStateSnapshot(); }
			void SetRuntimeTraceSink(AutomationRuntimeTraceSink *sink) override { impl->SetRuntimeTraceSink(sink); }
			void SetAutomationHost(std::shared_ptr<AutomationHost> host) override { impl->SetAutomationHost(std::move(host)); }
			void SetDebugSession(AutomationDebugSession *session) override { impl->SetDebugSession(session); }
		};

		std::unique_ptr<Script> WrapScriptInstance(std::unique_ptr<AutomationScriptInstance> instance)
		{
			if (!instance)
				return nullptr;

			if (auto *script = dynamic_cast<Script *>(instance.get())) {
				instance.release();
				return std::unique_ptr<Script>(script);
			}

			return agi::make_unique<ScriptCompatibilityWrapper>(std::move(instance));
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

		struct AutoloadReloadResult {
			std::vector<std::unique_ptr<Script>> scripts;
			int error_count = 0;
			struct Diagnostic {
				std::string message;
				bool error = false;
			};
			std::vector<Diagnostic> diagnostics;
		};

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

		ScriptLoadAttempt LoadAutomationScript(agi::fs::path const& script_filename)
		{
			ScriptLoadAttempt attempt;
			try {
				attempt.script = ScriptFactory::CreateFromFile(script_filename, false, &attempt.recognised);
			}
			catch (...) {
				attempt.recognised = true;
				attempt.script = agi::make_unique<FailedScript>(script_filename, DescribeCurrentException());
			}
			return attempt;
		}

		ScriptLoadAttempt LoadManagedApplicationPlugin(
			agi::fs::path const& script_filename) {
			auto attempt = LoadAutomationScript(script_filename);
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

		void ReportFailedAutomationScriptLoad(agi::fs::path const& filename, std::string const& description)
		{
			wxLogError(_("Failed to load Automation script '%s':\n%s"), filename.wstring(), to_wx(description));
		}

		void ReportUnrecognisedAutomationScript(agi::fs::path const& filename)
		{
			wxLogError(_("The file was not recognised as an Automation script: %s"), filename.wstring());
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

		AutoloadReloadResult LoadAutoloadScripts(
			std::string const& path,
			agi::fs::path const& managed_plugin_root)
		{
			AutoloadReloadResult result;
			std::vector<agi::fs::path> script_filenames;

			for (auto tok : agi::Split(path, '|')) {
				auto dirname = config::path->Decode(agi::str(tok));
				if (!agi::fs::DirectoryExists(dirname)) continue;

				try {
					dirname = agi::fs::Canonicalize(dirname);
				}
				catch (agi::fs::FileSystemError const&) {
				}

				for (auto filename : agi::fs::DirectoryIterator(dirname, "*.*")) {
					auto script_filename = dirname / agi::fs::PathFromString(filename);
					// Bare plugin.json is only for app-local plugins/<id>/ discovery.
					// Traditional autoload uses *.* and must not pick it up.
					auto leaf = agi::fs::PathToString(script_filename.filename());
					std::transform(leaf.begin(), leaf.end(), leaf.begin(), [](char value) {
						return value >= 'A' && value <= 'Z'
							? static_cast<char>(value - 'A' + 'a')
							: value;
					});
					if (leaf == "plugin.json")
						continue;

					try {
						script_filename = agi::fs::Canonicalize(script_filename);
					}
					catch (agi::fs::FileSystemError const&) {
					}

					if (find(script_filenames.begin(), script_filenames.end(), script_filename) == script_filenames.end())
						script_filenames.emplace_back(std::move(script_filename));
				}
			}

			std::vector<std::future<ScriptLoadAttempt>> script_futures;
			for (auto const& script_filename : script_filenames) {
				try {
					script_futures.emplace_back(std::async(std::launch::async, [=] {
						return LoadAutomationScript(script_filename);
					}));
				}
				catch (...) {
					auto attempt = LoadAutomationScript(script_filename);
					if (attempt.script) {
						if (!attempt.script->GetLoadedState())
							++result.error_count;
						result.scripts.emplace_back(std::move(attempt.script));
					}
				}
			}

			for (auto& future : script_futures) {
				auto attempt = future.get();
				if (!attempt.script)
					continue;

				if (!attempt.script->GetLoadedState())
					++result.error_count;
				result.scripts.emplace_back(std::move(attempt.script));
			}

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

			return result;
		}
	}

	bool CalculateTextExtents(AssStyle *style, std::string const& text, double &width, double &height, double &descent, double &extlead)
	{
		width = height = descent = extlead = 0;

		double fontsize = style->fontsize * 64;
		double spacing = style->spacing * 64;

#ifdef WIN32
		// This is almost copypasta from TextSub
		auto dc = CreateCompatibleDC(nullptr);
		if (!dc) return false;

		SetMapMode(dc, MM_TEXT);

		LOGFONTW lf = {0};
		lf.lfHeight = (LONG)fontsize;
		lf.lfWeight = style->bold ? FW_BOLD : FW_NORMAL;
		lf.lfItalic = style->italic;
		lf.lfUnderline = style->underline;
		lf.lfStrikeOut = style->strikeout;
		lf.lfCharSet = style->encoding;
		lf.lfOutPrecision = OUT_TT_PRECIS;
		lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
		lf.lfQuality = ANTIALIASED_QUALITY;
		lf.lfPitchAndFamily = DEFAULT_PITCH|FF_DONTCARE;
		wcsncpy(lf.lfFaceName, agi::charset::ConvertW(style->font).c_str(), 31);

		auto font = CreateFontIndirect(&lf);
		if (!font) return false;

		auto old_font = SelectObject(dc, font);

		std::wstring wtext(agi::charset::ConvertW(text));
		if (spacing != 0 ) {
			width = 0;
			for (auto c : wtext) {
				SIZE sz;
				GetTextExtentPoint32(dc, &c, 1, &sz);
				width += sz.cx + spacing;
				height = sz.cy;
			}
		}
		else {
			SIZE sz;
			GetTextExtentPoint32(dc, &wtext[0], (int)wtext.size(), &sz);
			width = sz.cx;
			height = sz.cy;
		}

		TEXTMETRIC tm;
		GetTextMetrics(dc, &tm);
		descent = tm.tmDescent;
		extlead = tm.tmExternalLeading;

		SelectObject(dc, old_font);
		DeleteObject(font);
		DeleteObject(dc);

#else // not WIN32
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
#endif

		// Compensate for scaling
		width = style->scalex / 100 * width / 64;
		height = style->scaley / 100 * height / 64;
		descent = style->scaley / 100 * descent / 64;
		extlead = style->scaley / 100 * extlead / 64;

		return true;
	}

	ExportFilter::ExportFilter(std::string const& name, std::string const& description, int priority)
	: AssExportFilter(name, description, priority)
	{
	}

	std::string ExportFilter::GetScriptSettingsIdentifier()
	{
		return inline_string_encode(GetName());
	}

	wxWindow* ExportFilter::GetConfigDialogWindow(wxWindow *parent, agi::Context *c) {
		automation_host = c ? CreateAutomationLiveHost(c) : nullptr;
		config_dialog = GenerateConfigDialog(parent, c);

		if (config_dialog) {
			auto core = c->GetCore();
			std::string const& val = core.ass->Properties.automation_settings[GetScriptSettingsIdentifier()];
			if (!val.empty())
				config_dialog->Unserialise(val);
			return config_dialog->CreateWindow(parent);
		}

		return nullptr;
	}

	void ExportFilter::LoadSettings(bool is_default, agi::Context *c) {
		automation_host = c ? CreateAutomationLiveHost(c) : nullptr;
		if (config_dialog)
			c->GetCore().ass->Properties.automation_settings[GetScriptSettingsIdentifier()] = config_dialog->Serialise();
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

	void BackgroundScriptRunner::Run(std::function<void (ProgressSink*)> task)
	{
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
		include_path.emplace_back(filename.parent_path());

		for (auto probe = filename.parent_path(); !probe.empty();) {
			auto candidate = probe / "include";
			if (agi::fs::DirectoryExists(candidate)) {
				include_path.emplace_back(std::move(candidate));
				break;
			}
			auto parent = probe.parent_path();
			if (parent == probe)
				break;
			probe = std::move(parent);
		}

		std::string include_paths = OPT_GET("Path/Automation/Include")->GetString();
		for (auto tok : agi::Split(include_paths, '|')) {
			auto path = config::path->Decode(agi::str(tok));
			if (path.is_absolute() && agi::fs::DirectoryExists(path))
				include_path.emplace_back(std::move(path));
		}
	}

	// ScriptManager
	void ScriptManager::Add(std::unique_ptr<Script> script)
	{
		if (config::automation_debug_service && config::automation_debug_service->HasLocalSession())
			throw AutomationError("Cannot change loaded scripts while a Lua Workspace invocation is active");
		if (!script)
			return;
		script->CommitPendingFeatures();

		if (find(scripts.begin(), scripts.end(), script) == scripts.end())
			scripts.emplace_back(std::move(script));

		ScriptsChanged();
	}

	void ScriptManager::Remove(Script *script)
	{
		if (config::automation_debug_service && config::automation_debug_service->HasLocalSession())
			throw AutomationError("Cannot change loaded scripts while a Lua Workspace invocation is active");
		auto i = find_if(scripts.begin(), scripts.end(), [&](std::unique_ptr<Script> const& s) { return s.get() == script; });
		if (i != scripts.end())
			scripts.erase(i);

		ScriptsChanged();
	}

	void ScriptManager::RemoveAll()
	{
		if (config::automation_debug_service && config::automation_debug_service->HasLocalSession())
			throw AutomationError("Cannot change loaded scripts while a Lua Workspace invocation is active");
		scripts.clear();
		ScriptsChanged();
	}

	void ScriptManager::Reload(Script *script, std::shared_ptr<AutomationDebugSession> const& owner) {
		if (config::automation_debug_service && config::automation_debug_service->HasLocalSession() && (!config::automation_debug_service->OwnsLocalSession(owner) || owner->GetStateSnapshot().invocation_active))
			throw AutomationError("Cannot reload scripts while a Lua Workspace invocation is active");
		script->Reload();
		script->CommitPendingFeatures();
		ScriptsChanged();
	}

	Script *ScriptManager::ReplaceForWorkspace(
		Script *current,
		std::unique_ptr<Script> replacement,
		std::shared_ptr<AutomationDebugSession> const& owner) {
		agi::ui::VerifyAccess();
		if (!config::automation_debug_service || !config::automation_debug_service->OwnsLocalSession(owner))
			throw AutomationError("Cannot replace a script without owning the active Lua Workspace session");
		if (!current || !replacement || !replacement->GetLoadedState())
			throw AutomationError("Cannot replace a script with an unloaded Workspace candidate");

		auto found = std::ranges::find_if(scripts, [&](std::unique_ptr<Script> const& script) {
			return script.get() == current;
		});
		if (found == scripts.end())
			throw AutomationError("The Workspace script is no longer managed");
		if (replacement->GetFilename() != (*found)->GetFilename())
			throw AutomationError("The Workspace replacement does not match the managed script");

		auto *published = replacement.get();
		std::swap(*found, replacement);
		replacement.reset();
		published->CommitPendingFeatures();
		ScriptsChanged();
		return published;
	}

	void CommitPendingFeatures(std::vector<std::unique_ptr<Script>>& scripts)
	{
		for (auto& script : scripts) {
			if (script && script->GetLoadedState())
				script->CommitPendingFeatures();
		}
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
		std::vector<std::unique_ptr<Script>> loaded_scripts,
		int error_count,
		std::vector<std::pair<std::string, bool>> diagnostics)
	{
		if (config::automation_debug_service && config::automation_debug_service->HasLocalSession()) {
			pending_reload = true;
			LOG_W("automation/reload") << "Autoload refresh deferred until the Lua Workspace invocation finishes";
			return;
		}
		pending_reload = false;
		for (auto& script : loaded_scripts) {
			if (!script->GetLoadedState())
				ReportFailedAutomationScriptLoad(script->GetFilename(), script->GetDescription());
		}

		scripts.clear();
		CommitPendingFeatures(loaded_scripts);
		scripts = std::move(loaded_scripts);

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

		ScriptsChanged();
	}

	void AutoloadScriptManager::ProcessPendingReload() {
		if (!pending_reload || (config::automation_debug_service && config::automation_debug_service->HasLocalSession()))
			return;
		ReloadAsync();
		pending_reload = false;
	}

	void AutoloadScriptManager::Reload()
	{
		reload_generation->fetch_add(1, std::memory_order_relaxed);
		auto result = LoadAutoloadScripts(path, managed_plugin_root);
		std::vector<std::pair<std::string, bool>> diagnostics;
		for (auto& diagnostic : result.diagnostics)
			diagnostics.emplace_back(std::move(diagnostic.message), diagnostic.error);
		ApplyReloadedScripts(
			std::move(result.scripts), result.error_count, std::move(diagnostics));
	}

	void AutoloadScriptManager::ReloadAsync()
	{
		auto lifetime = std::weak_ptr<char>(reload_lifetime);
		auto generation = reload_generation;
		auto path_copy = path;
		auto managed_plugin_root_copy = managed_plugin_root;
		auto const reload_id = generation->fetch_add(1, std::memory_order_relaxed) + 1;
		auto const scheduled_at = AutoloadTimingClock::now();
		LOG_I_IF(perf_trace::IsCategoryEnabled(perf_trace::Category::Log), "automation/autoload/timing")
			<< "reload_id=" << reload_id << " phase=scheduled";

		agi::dispatch::Background().Async([this, lifetime, generation, reload_id,
			scheduled_at,
			path_copy = std::move(path_copy),
			managed_plugin_root_copy = std::move(managed_plugin_root_copy)] {
			auto const background_started = AutoloadTimingClock::now();
			auto const background_dispatch_delay_ms =
				AutoloadDurationMs(scheduled_at, background_started);
			auto result = std::make_shared<AutoloadReloadResult>(
				LoadAutoloadScripts(path_copy, managed_plugin_root_copy));
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
				macro_count += script->GetMacros().size();
				filter_count += script->GetFilters().size();
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

			agi::dispatch::Main().Async([this, lifetime, generation, reload_id, result,
				background_finished, background_dispatch_delay_ms, background_load_ms,
				script_count, loaded_script_count, macro_count, filter_count,
				error_count, diagnostic_count] {
				auto const main_started = AutoloadTimingClock::now();
				auto const main_queue_wait_ms =
					AutoloadDurationMs(background_finished, main_started);
				if (!lifetime.lock()) {
					LOG_I_IF(perf_trace::IsCategoryEnabled(perf_trace::Category::Log), "automation/autoload/timing")
						<< "reload_id=" << reload_id
						<< " phase=main_skipped reason=lifetime_expired"
						<< " main_queue_wait_ms=" << main_queue_wait_ms;
					return;
				}
				if (generation->load(std::memory_order_relaxed) != reload_id) {
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
		bool was_empty = scripts.empty();
		scripts.clear();

		auto core = context->GetCore();
		auto const& local_scripts = core.ass->Properties.automation_scripts;
		if (local_scripts.empty()) {
			if (!was_empty)
				ScriptsChanged();
			return;
		}

		auto autobasefn(OPT_GET("Path/Automation/Base")->GetString());

		for (auto tok : agi::Split(local_scripts, '|')) {
			auto token = agi::util::strings::trim_copy(agi::str(tok));
			if (token.empty()) continue;
			char first_char = token[0];
			std::string trimmed(token.begin() + 1, token.end());

			agi::fs::path basepath;
			if (first_char == '~') {
				basepath = core.subsController->Filename().parent_path();
			} else if (first_char == '$') {
				basepath = autobasefn;
			} else if (first_char == '/') {
			} else {
				context->ShowWarning(
					agi::format(
						"Automation Script referenced with unknown location specifier character.\n"
						"Location specifier found: %c\nFilename specified: %s",
						first_char,
						trimmed),
					"Automation");
				continue;
			}
			auto sfname = basepath / agi::fs::PathFromString(trimmed);
			if (agi::fs::FileExists(sfname)) {
				bool recognised = false;
				auto script = Automation4::ScriptFactory::CreateFromFile(sfname, true, &recognised);
				if (!recognised)
					context->ShowError(
						"The file was not recognised as an Automation script: " +
							agi::fs::PathToString(sfname),
						"Automation");
				else if (script && !script->GetLoadedState())
					context->ShowError(
						"Failed to load Automation script '" + agi::fs::PathToString(sfname) +
							"':\n" + script->GetDescription(),
						"Automation");
				scripts.emplace_back(std::move(script));
			}
			else {
				context->ShowWarning(
					agi::format(
						"Automation Script referenced could not be found.\n"
						"Filename specified: %c%s\nSearched relative to: %s\nResolved filename: %s",
						first_char,
						trimmed,
						agi::fs::PathToString(basepath),
						agi::fs::PathToString(sfname)),
					"Automation");
			}
		}

		CommitPendingFeatures(scripts);
		ScriptsChanged();
	}

	void LocalScriptManager::SaveLoadedList()
	{
		// Store Automation script data
		// Algorithm:
		// 1. If script filename has Automation Base Path as a prefix, the path is relative to that (ie. "$")
		// 2. Otherwise try making it relative to the ass filename
		// 3. If step 2 failed, or absolute path is shorter than path relative to ass, use absolute path ("/")
		// 4. Otherwise, use path relative to ass ("~")
		std::string scripts_string;
		agi::fs::path autobasefn(OPT_GET("Path/Automation/Base")->GetString());
		auto core = context->GetCore();

		for (auto& script : GetScripts()) {
			if (!scripts_string.empty())
				scripts_string += "|";

			auto const scriptfn = script->GetFilename();
			auto const scriptfn_str = agi::fs::PathToString(scriptfn);
			auto const autobase_rel = core.path->MakeRelative(scriptfn, autobasefn);
			auto const assfile_rel = core.path->MakeRelative(scriptfn, "?script");
			auto const autobase_rel_str = agi::fs::PathToGenericString(autobase_rel);
			auto const assfile_rel_str = agi::fs::PathToGenericString(assfile_rel);

			if (autobase_rel_str.size() <= scriptfn_str.size() && autobase_rel_str.size() <= assfile_rel_str.size()) {
				scripts_string += "$" + autobase_rel_str;
			} else if (assfile_rel_str.size() <= scriptfn_str.size() && assfile_rel_str.size() <= autobase_rel_str.size()) {
				scripts_string += "~" + assfile_rel_str;
			} else {
				scripts_string += "/" + agi::fs::PathToGenericString(scriptfn);
			}
		}
		core.ass->Properties.automation_scripts = std::move(scripts_string);
	}

	std::unique_ptr<Script> ScriptFactory::CreateFromFile(
		agi::fs::path const& filename,
		bool create_unknown,
		bool *recognised_out)
	{
		if (recognised_out)
			*recognised_out = false;

		try {
			if (auto script = WrapScriptInstance(AutomationEngineRegistry::CreateFromFile(filename))) {
				if (recognised_out)
					*recognised_out = true;
				return script;
			}
		}
		catch (...) {
			if (recognised_out)
				*recognised_out = true;
			return agi::make_unique<FailedScript>(filename, DescribeCurrentException());
		}

		return create_unknown ? agi::make_unique<UnknownScript>(filename) : nullptr;
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
