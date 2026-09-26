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

/// @file auto4_base.h
/// @see auto4_base.cpp
/// @ingroup scripting
///

#pragma once

#include "automation/engine/automation_script_instance.h"
#include "automation/automation_runtime_state_snapshot.h"
#include "automation/automation_runtime_trace_sink.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs_fwd.h>
#include <libaegisub/signal.h>

#include "ass_export_filter.h"

#include <filesystem>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class AssStyle;
class wxWindow;
class wxDialog;

namespace agi { struct Context; }
namespace agi { class FileDialogService; }
namespace cmd { class Command; }

namespace Automation4 {
	class AutomationHost;
	class AutomationDebugSession;

	DEFINE_EXCEPTION(AutomationError, agi::Exception);
	DEFINE_EXCEPTION(ScriptLoadError, AutomationError);
	DEFINE_EXCEPTION(MacroRunError, AutomationError);

	// Calculate the extents of a text string given a style
	bool CalculateTextExtents(AssStyle *style, std::string const& text, double &width, double &height, double &descent, double &extlead);

	class ScriptDialog;

	class ExportFilter : public AssExportFilter {
		std::unique_ptr<ScriptDialog> config_dialog;
		std::shared_ptr<AutomationHost> automation_host;

		/// subclasses should implement this, producing a new ScriptDialog
		virtual std::unique_ptr<ScriptDialog> GenerateConfigDialog(wxWindow *parent, agi::Context *c) = 0;

	protected:
		std::string GetScriptSettingsIdentifier();
		std::shared_ptr<AutomationHost> GetAutomationHost() const { return automation_host; }

	public:
		ExportFilter(std::string const& name, std::string const& description, int priority);

		wxWindow* GetConfigDialogWindow(wxWindow *parent, agi::Context *c) override;
		void LoadSettings(bool is_default, agi::Context *c) override;

		// Subclasses must implement ProcessSubs from AssExportFilter
	};

	/// A "dialog" which actually generates a non-top-level window that is then
	/// either inserted into a dialog or into the export filter configuration
	/// panel
	class ScriptDialog {
	public:
		virtual ~ScriptDialog() = default;

		/// Create a window with the given parent
		virtual wxWindow *CreateWindow(wxWindow *parent) = 0;

		/// Serialize the values of the controls in this dialog to a string
		/// suitable for storage in the subtitle script
		virtual std::string Serialise() { return ""; }

		/// Restore the values of the controls in this dialog from a string
		/// stored in the subtitle script
		virtual void Unserialise(std::string const& serialised) { }
	};

	class ProgressSink;

	struct AutomationOpenFileDialogRequest {
		std::string message;
		std::string dir;
		std::string file;
		std::string wildcard;
		bool multiple = false;
		bool must_exist = true;
	};

	struct AutomationSaveFileDialogRequest {
		std::string message;
		std::string dir;
		std::string file;
		std::string wildcard;
		bool prompt_overwrite = true;
	};

	class BackgroundScriptRunner {
		std::unique_ptr<agi::BackgroundRunner> impl;
		wxWindow *parent = nullptr;
		std::string title;
		std::shared_ptr<agi::FileDialogService> file_dialog_service;

	public:
		wxWindow *GetParentWindow() const;
		std::string GetTitle() const;
		std::vector<agi::fs::path> RequestOpenFiles(AutomationOpenFileDialogRequest const& request) const;
		agi::fs::path RequestSaveFile(AutomationSaveFileDialogRequest const& request) const;

		void Run(std::function<void(ProgressSink*)> task);

		BackgroundScriptRunner(wxWindow *parent, std::string const& title, std::shared_ptr<agi::FileDialogService> file_dialog_service = {});
		BackgroundScriptRunner(
			std::unique_ptr<agi::BackgroundRunner> impl,
			wxWindow *parent,
			std::string title,
			std::shared_ptr<agi::FileDialogService> file_dialog_service = {});
		~BackgroundScriptRunner();
	};

	class AutomationBackgroundScriptRunnerFactory {
	public:
		virtual ~AutomationBackgroundScriptRunnerFactory() = default;
		virtual std::unique_ptr<BackgroundScriptRunner> Create(std::string const& title) = 0;
	};

	class NullAutomationBackgroundScriptRunnerFactory final : public AutomationBackgroundScriptRunnerFactory {
	public:
		std::unique_ptr<BackgroundScriptRunner> Create(std::string const&) override {
			return {};
		}
	};

	/// A wrapper around agi::ProgressSink which adds the ability to open
	/// dialogs on the GUI thread
	class ProgressSink final : public agi::ProgressSink {
		agi::ProgressSink *impl;
		BackgroundScriptRunner *bsr;
		int trace_level;
	public:
		void SetIndeterminate() override { impl->SetIndeterminate(); }
		void SetTitle(std::string const& title) override { impl->SetTitle(title); }
		void SetMessage(std::string const& msg) override { impl->SetMessage(msg); }
		void SetProgress(int64_t cur, int64_t max) override { impl->SetProgress(cur, max); }
		void Log(std::string const& str) override { impl->Log(str); }
		bool IsCancelled() override { return impl->IsCancelled(); }

		/// Show the passed dialog on the GUI thread, blocking the calling
		/// thread until it closes
		void ShowDialog(ScriptDialog *config_dialog);
		int ShowDialog(wxDialog *dialog);
		std::vector<agi::fs::path> RequestOpenFiles(AutomationOpenFileDialogRequest const& request);
		agi::fs::path RequestSaveFile(AutomationSaveFileDialogRequest const& request);
		wxWindow *GetParentWindow() const { return bsr->GetParentWindow(); }

		/// Get the current automation trace level
		int GetTraceLevel() const { return trace_level; }

		ProgressSink(agi::ProgressSink *impl, BackgroundScriptRunner *bsr);
	};

	class Script : public AutomationScriptInstance {
		agi::fs::path filename;

	protected:
		/// The automation include path, consisting of the user-specified paths
		/// along with the script's path
		std::vector<agi::fs::path> include_path;
		Script(agi::fs::path const& filename);

	public:
		virtual ~Script() = default;

		/// Reload this script
		virtual void Reload() = 0;

		/// The script's file name with path
		agi::fs::path GetFilename() const override { return filename; }
		/// The script's file name without path
		agi::fs::path GetPrettyFilename() const { return filename.filename(); }
		/// The script's name. Not required to be unique.
		virtual std::string GetName() const=0;
		/// A short description of the script
		virtual std::string GetDescription() const=0;
		/// The author of the script
		virtual std::string GetAuthor() const=0;
		/// A version string that should not be used for anything but display
		virtual std::string GetVersion() const=0;
		/// Did the script load correctly?
		virtual bool GetLoadedState() const=0;

		/// Get a list of commands provided by this script
		virtual std::vector<cmd::Command*> GetMacros() const=0;
		/// Get a list of export filters provided by this script
		virtual std::vector<ExportFilter*> GetFilters() const=0;
		/// Commit features discovered while loading the script to the global registries.
		virtual void CommitPendingFeatures() { }
		/// Name of the runtime engine backing the script
		virtual std::string GetEngineName() const { return ""; }
		/// Get the current automation runtime snapshot when supported by the engine
		virtual std::optional<AutomationRuntimeStateSnapshot> TryGetRuntimeStateSnapshot() const { return std::nullopt; }
		/// Install an engine-agnostic runtime trace sink when supported by the engine
		virtual void SetRuntimeTraceSink(AutomationRuntimeTraceSink*) { }
		/// Install an engine-agnostic automation host when supported by the engine
		virtual void SetAutomationHost(std::shared_ptr<AutomationHost>) { }
		/// Install an engine-agnostic debug session when supported by the engine
		virtual void SetDebugSession(AutomationDebugSession*) { }
	};

	/// A manager of loaded automation scripts
	class ScriptManager {
	protected:
		std::vector<std::unique_ptr<Script>> scripts;
		std::vector<cmd::Command*> macros;

		agi::signal::Signal<> ScriptsChanged;

	public:
		/// Deletes all scripts managed
		virtual ~ScriptManager() = default;
		/// Add a script to the manager.
		void Add(std::unique_ptr<Script> script);
		/// Remove a script from the manager, and delete the Script object.
		void Remove(Script *script);
		/// Deletes all scripts managed
		void RemoveAll();
		/// Reload all scripts managed
		virtual void Reload() = 0;
		/// Reload a single managed script
		virtual void Reload(Script *script, std::shared_ptr<AutomationDebugSession> const& owner = {});

		/// Get all managed scripts (both loaded and invalid)
		const std::vector<std::unique_ptr<Script>>& GetScripts() const { return scripts; }

		const std::vector<cmd::Command*>& GetMacros();
		// No need to have getters for the other kinds of features, I think.
		// They automatically register themselves in the relevant places.

		DEFINE_SIGNAL_ADDERS(ScriptsChanged, AddScriptChangeListener)
	};

	/// Manager for scripts specified by a subtitle file
	class LocalScriptManager final : public ScriptManager {
		agi::Context *context;
		agi::signal::Connection file_open_connection;

		void SaveLoadedList();
	public:
		LocalScriptManager(agi::Context *context);
		void Reload() override;
	};

	/// Manager for scripts in the autoload directory
	class AutoloadScriptManager final : public ScriptManager {
		std::string path;
		agi::fs::path managed_plugin_root;
		std::shared_ptr<char> reload_lifetime = std::make_shared<char>();
		std::shared_ptr<std::atomic<uint64_t>> reload_generation = std::make_shared<std::atomic<uint64_t>>(0);
		bool pending_reload = false;

		void ApplyReloadedScripts(
			std::vector<std::unique_ptr<Script>> loaded_scripts,
			int error_count,
			std::vector<std::pair<std::string, bool>> diagnostics);
	public:
		AutoloadScriptManager(
			std::string path,
			agi::fs::path managed_plugin_root = {});
		void Reload() override;
		void ReloadAsync();
		void ProcessPendingReload();
	};

	class ScriptFactory {
	public:
		/// Get the full wildcard string for all loaded engines
		static std::string GetWildcardStr();

		/// Load a script from a file
		/// @param filename Script to load
		/// @param create_unknown Create a placeholder rather than returning nullptr if no script engine supports the file
		/// @param recognised_out Optional out flag which receives whether any automation engine recognised the file
		static std::unique_ptr<Script> CreateFromFile(
			agi::fs::path const& filename,
			bool create_unknown = true,
			bool *recognised_out = nullptr);
	};

	/// A script which represents a file not recognized by any registered
	/// automation engines
	class UnknownScript final : public Script {
	public:
		UnknownScript(agi::fs::path const& filename) : Script(filename) { }

		void Reload() override { }

		std::string GetName() const override { return agi::fs::PathToString(GetFilename().stem()); }
		std::string GetDescription() const override;
		std::string GetAuthor() const override { return ""; }
		std::string GetVersion() const override { return ""; }
		bool GetLoadedState() const override { return false; }

		std::vector<cmd::Command*> GetMacros() const override { return {}; }
		std::vector<ExportFilter*> GetFilters() const override { return {}; }
	};
}
