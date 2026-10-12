#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_info.h"
#include "../../src/ass_io_core.h"
#include "../../src/ass_style.h"
#include "../../src/auto4_lua_factory.h"
#include "../../src/automation/automation_debug_session.h"
#include "../../src/automation/automation_error.h"
#include "../../src/automation/automation_features.h"
#include "../../src/automation/automation_invocation_observer.h"
#include "../../src/automation/automation_mutation_journal.h"
#include "../../src/automation/automation_runtime_services.h"
#include "../../src/automation/automation_script_collection.h"
#ifdef _WIN32
#include "../../src/automation/automation_text_extents.h"
#endif
#include "../../src/automation/engine/automation_engine_registry.h"
#include "../../src/automation/engine/automation_script_instance.h"
#include "../../src/automation/lua_workspace_run.h"
#include "../../src/options.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>
#include <libaegisub/scope_exit.h>
#include <libaegisub/vfr.h>

#include <lua.hpp>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace Automation4;

void Require(bool condition, std::string const& message) {
	if (!condition) throw std::runtime_error(message);
}

std::vector<std::string> DialogueData(AssFile const& ass) {
	std::vector<std::string> result;
	for (auto const& line : ass.Events) result.push_back(line.GetEntryData());
	return result;
}

AssDialogue& Line(AssFile& ass, int row) {
	Require(row >= 0 && static_cast<size_t>(row) < ass.Events.size(), "Missing expected subtitle row");
	auto found = ass.Events.begin();
	std::advance(found, row);
	Require(found != ass.Events.end(), "Missing expected subtitle row");
	return *found;
}

class Results final {
	json::Object report;
	json::Array cases;
	json::Array checks;
	std::string active_case;
	int passed = 0;

	public:
	void Begin(std::string id) {
		Finish();
		active_case = std::move(id);
	}
	void Check(std::string const& id, bool condition) {
		json::Object check;
		check["id"] = id;
		check["passed"] = condition;
		checks.emplace_back(std::move(check));
		Require(condition, active_case + "/" + id);
		++passed;
	}
	void Finish() {
		if (active_case.empty()) return;
		json::Object value;
		value["id"] = active_case;
		value["checks"] = std::move(checks);
		cases.emplace_back(std::move(value));
		checks.clear();
		active_case.clear();
	}
	void Write(fs::path const& directory, bool success, std::string const& failure,
			   json::Object evidence) {
		Finish();
		report["schema"] = "automation-neutral-runtime-e2e-v1";
		report["success"] = success;
		report["failure"] = failure;
		report["runtime_log"] = "runtime.log";
		report["passed_checks"] = static_cast<json::Integer>(passed);
		report["cases"] = std::move(cases);
		report["evidence"] = std::move(evidence);
		std::ofstream output(directory / "result.json", std::ios::binary | std::ios::trunc);
		Require(output.is_open(), "Cannot open result.json");
		agi::JsonWriter::Write(report, output);
		output << '\n';
		output.close();
		Require(!output.fail(), "Cannot write result.json");
	}
};

class Selection final : public AutomationSelection {
	std::set<AssDialogue *> selected;
	AssDialogue *active = nullptr;

	public:
	[[nodiscard]] std::set<AssDialogue *> const& GetSelectedSet() const override { return selected; }
	[[nodiscard]] AssDialogue *GetActiveLine() const override { return active; }
	void SetSelectionAndActive(std::set<AssDialogue *> value, AssDialogue *line) override {
		selected = std::move(value);
		active = line;
	}
	void Select(AssFile& ass, std::initializer_list<int> rows, int active_row) {
		selected.clear();
		for (int row : rows) selected.insert(&Line(ass, row));
		active = &Line(ass, active_row);
	}
	[[nodiscard]] std::vector<int> Rows() const {
		std::vector<int> rows;
		rows.reserve(selected.size());
		for (auto const *line : selected) rows.push_back(line->Row);
		std::ranges::sort(rows);
		return rows;
	}
};

class RuntimeServices final : public AutomationRuntimeServices {
	std::string translation_prefix;
	std::string module_identity;
	mutable std::string clipboard;

	static int OpenHostModule(lua_State *state) {
		lua_newtable(state);
		lua_pushvalue(state, lua_upvalueindex(1));
		lua_setfield(state, -2, "identity");
#ifdef _WIN32
		lua_pushboolean(state, true);
#else
		lua_pushboolean(state, false);
#endif
		lua_setfield(state, -2, "has_text_extents");
		return 1;
	}

	public:
	mutable std::vector<std::string> warnings;
	mutable std::vector<std::string> errors;
	mutable int text_extent_calls = 0;
	explicit RuntimeServices(std::string translation_prefix = "translated:", std::string module_identity = "neutral-host-module", std::string clipboard = {})
		: translation_prefix(std::move(translation_prefix)), module_identity(std::move(module_identity)), clipboard(std::move(clipboard)) {}
	std::string Translate(std::string const& text) const override { return translation_prefix + text; }
	std::string GetClipboard() const override { return clipboard; }
	bool SetClipboard(std::string const& text) const override {
		clipboard = text;
		return true;
	}
	bool CalculateTextExtents(AssStyle const& style, std::string const& text,
							  double& width, double& height, double& descent, double& external_leading) const override {
#ifdef _WIN32
		++text_extent_calls;
		return CalculateAutomationTextExtents(style, text, width, height, descent, external_leading);
#else
		(void)style;
		(void)text;
		(void)width;
		(void)height;
		(void)descent;
		(void)external_leading;
		return false;
#endif
	}
	void InstallLuaModules(lua_State *state) const override {
		lua_getglobal(state, "package");
		lua_getfield(state, -1, "preload");
		lua_pushlstring(state, module_identity.data(), module_identity.size());
		lua_pushcclosure(state, OpenHostModule, 1);
		lua_setfield(state, -2, "neutral_host");
		lua_pop(state, 2);
	}
	void LogWarning(std::string const& message) const override { warnings.push_back(message); }
	void LogError(std::string const& message) const override { errors.push_back(message); }
};

class FeatureRegistry final : public AutomationFeatureRegistry {
	public:
	std::map<std::string, AutomationMacro *> macros;
	std::map<std::string, AutomationExportFilter *> filters;
	int macro_activations = 0;
	int macro_deactivations = 0;
	int filter_activations = 0;
	int filter_deactivations = 0;
	bool ActivateMacro(AutomationMacro& macro) override {
		if (!macros.emplace(macro.Name(), &macro).second) return false;
		++macro_activations;
		return true;
	}
	void DeactivateMacro(AutomationMacro& macro) override {
		auto it = macros.find(macro.Name());
		Require(it != macros.end() && it->second == &macro, "Deactivated unknown macro");
		macros.erase(it);
		++macro_deactivations;
	}
	void ActivateFilter(AutomationExportFilter& filter) override {
		auto original = filter.Name();
		int suffix = 2;
		while (filters.contains(filter.Name())) filter.SetName(original + " (" + std::to_string(suffix++) + ")");
		filters.emplace(filter.Name(), &filter);
		++filter_activations;
	}
	void DeactivateFilter(AutomationExportFilter& filter) override {
		auto it = filters.find(filter.Name());
		Require(it != filters.end() && it->second == &filter, "Deactivated unknown filter");
		filters.erase(it);
		++filter_deactivations;
	}
	[[nodiscard]] AutomationMacro& Find(std::string_view display) const {
		for (auto const& [name, macro] : macros)
			if (macro->Display() == display) return *macro;
		throw std::runtime_error("Macro unavailable: " + std::string(display));
	}
};

class Observation final : public AutomationInvocationObserver {
	public:
	std::vector<AutomationInvocationOutcome> outcomes;
	std::vector<AutomationRuntimeStateSnapshot> snapshots;
	void OnRuntimeStateSnapshot(AutomationRuntimeStateSnapshot const& value) override { snapshots.push_back(value); }
	void OnInvocationFinished(AutomationInvocationOutcome outcome) override { outcomes.push_back(outcome); }
};

AutomationDialogControl& Control(AutomationDialog& dialog, std::string_view name) {
	for (auto& control : dialog.controls)
		if (control.name == name) return control;
	throw std::runtime_error("Missing dialog control " + std::string(name));
}

void SetDialogValues(AutomationDialog& dialog, bool for_export) {
	std::get<AutomationDialogEdit>(Control(dialog, "prefix").value).value = for_export ? "export:|%" : "accepted:|%";
	std::get<AutomationDialogEdit>(Control(dialog, "notes").value).value = for_export ? "export\nnotes" : "two\nlines";
	std::get<AutomationDialogIntEdit>(Control(dialog, "offset").value).value = for_export ? -125 : 250;
	std::get<AutomationDialogFloatEdit>(Control(dialog, "factor").value).value = 1.5;
	std::get<AutomationDialogDropdown>(Control(dialog, "style").value).value = "Emphasis";
	std::get<AutomationDialogCheckbox>(Control(dialog, "enabled").value).value = true;
	std::get<AutomationDialogColor>(Control(dialog, "rgb").value).value = agi::Color("#336699");
	std::get<AutomationDialogColor>(Control(dialog, "rgba").value).value = agi::Color(0x12, 0x34, 0x56, 0x78);
	std::get<AutomationDialogEdit>(Control(dialog, "alpha").value).value = "&H80&";
}

struct UiState {
	bool cancel_dialog = false;
	int cancel_at_percent = 0;
	int dialog_count = 0;
	int open_count = 0;
	int save_count = 0;
	int progress_updates = 0;
	std::string title;
	std::string message;
	std::string log;
	std::ofstream runtime_log;
	std::function<void()> during_dialog;
	std::shared_ptr<Observation> observation = std::make_shared<Observation>();
	std::shared_ptr<LuaWorkspaceRunRequest const> workspace;
};

class Progress final : public AutomationProgress {
	UiState& state;
	int percent = 0;

	public:
	explicit Progress(UiState& state) : state(state) {}
	void SetIndeterminate() override {}
	void SetTitle(std::string const& value) override { state.title = value; }
	void SetMessage(std::string const& value) override { state.message = value; }
	void SetProgress(int64_t current, int64_t maximum) override {
		Require(maximum > 0, "Invalid progress maximum");
		percent = static_cast<int>(current * 100 / maximum);
		++state.progress_updates;
	}
	void Log(std::string const& value) override {
		state.log += value;
		state.runtime_log << value;
		state.runtime_log.flush();
		std::cerr << value;
	}
	bool IsCancelled() override { return state.cancel_at_percent > 0 && percent >= state.cancel_at_percent; }
	[[nodiscard]] int GetTraceLevel() const override { return 5; }
	void ShowDialog(AutomationDialog& dialog) override {
		++state.dialog_count;
		Require(dialog.controls.size() == 10, "Dialog did not preserve all supported control kinds");
		Require(dialog.buttons.size() == 2 && dialog.buttons[0].role == AutomationDialogButtonRole::Ok &&
					dialog.buttons[1].role == AutomationDialogButtonRole::Cancel,
				"Dialog did not preserve button roles");
		Require(std::get<AutomationDialogLabel>(Control(dialog, "heading").value).label == "Subtitle values", "Label text changed");
		Require(Control(dialog, "prefix").hint == "Prefix" && Control(dialog, "notes").height == 2, "Dialog layout or hint changed");
		Require(std::get<AutomationDialogEdit>(Control(dialog, "notes").value).multiline, "Textbox lost multiline semantics");
		auto const& offset = std::get<AutomationDialogIntEdit>(Control(dialog, "offset").value);
		auto const& factor = std::get<AutomationDialogFloatEdit>(Control(dialog, "factor").value);
		Require(offset.min == -500 && offset.max == 500 && factor.step == 0.25, "Numeric dialog constraints changed");
		Require(std::get<AutomationDialogDropdown>(Control(dialog, "style").value).items == std::vector<std::string>{"Default", "Emphasis"}, "Dropdown choices changed");
		Require(std::get<AutomationDialogColor>(Control(dialog, "rgba").value).alpha, "Coloralpha lost alpha");
		if (state.during_dialog) state.during_dialog();
		SetDialogValues(dialog, false);
		dialog.button_pushed = state.cancel_dialog ? 1 : 0;
	}
	std::vector<agi::fs::path> RequestOpenFiles(AutomationOpenFileDialogRequest const& request) override {
		++state.open_count;
		Require(request.multiple && request.must_exist && request.message == "Choose input", "Open dialog request changed");
		return {"input-one.ass", "input-two.ass"};
	}
	agi::fs::path RequestSaveFile(AutomationSaveFileDialogRequest const& request) override {
		++state.save_count;
		Require(request.message == "Choose output" && request.file == "output.ass", "Save dialog request changed");
		return "chosen-output.ass";
	}
};

class Runner final : public AutomationRunner {
	UiState& state;

	public:
	explicit Runner(UiState& state) : state(state) {}
	void Run(std::function<void(AutomationProgress *)> task) override {
		Progress progress(state);
		task(&progress);
	}
};

class Ui final : public AutomationUiProxy {
	public:
	mutable UiState state;
	std::shared_ptr<AutomationInvocationObserver> BeginInvocationObservation(AutomationInvocation const&) const override { return state.observation; }
	std::shared_ptr<LuaWorkspaceRunRequest const> GetWorkspaceRunRequest(AutomationInvocation const&) const override { return state.workspace; }
	std::unique_ptr<AutomationRunner> CreateWorkspaceBackgroundScriptRunner(std::shared_ptr<LuaWorkspaceRunRequest const> const&, std::string const&) const override {
		return std::make_unique<Runner>(state);
	}
	void ShowStatus(std::string const& value, int) override { state.message = value; }
	bool SupportsInteractiveDialogs() const override { return true; }
	std::unique_ptr<AutomationRunner> CreateBackgroundScriptRunner(std::string const&, AutomationUiAnchor) const override {
		return std::make_unique<Runner>(state);
	}
	void ShowDialog(AutomationProgress& sink, AutomationDialog& dialog) override { sink.ShowDialog(dialog); }
	std::vector<agi::fs::path> RequestOpenFiles(AutomationProgress& sink, AutomationOpenFileDialogRequest const& request) override { return sink.RequestOpenFiles(request); }
	agi::fs::path RequestSaveFile(AutomationProgress& sink, AutomationSaveFileDialogRequest const& request) override { return sink.RequestSaveFile(request); }
	std::shared_ptr<agi::FileDialogService> GetFileDialogService() const override { return {}; }
	bool CanFocusSubtitleEditBox() const override { return false; }
	std::optional<AutomationSubtitleEditBoxCursor> TryGetSubtitleEditBoxCursor() const override { return std::nullopt; }
	std::optional<AutomationVisualGuideSnapshot> TryGetVisualGuides() const override { return std::nullopt; }
	bool ScrollAudioToTime(int) override { return false; }
	bool FocusSubtitleEditBox() override { return false; }
	bool SetSubtitleEditBoxCursor(int, bool) override { return false; }
	bool SetSubtitleEditBoxSelection(int, int) override { return false; }
};

class Media final : public AutomationMediaState {
	int width;
	int height;
	int frame_duration;

	public:
	Media(int width, int height, int frame_duration) : width(width), height(height), frame_duration(frame_duration) {}
	[[nodiscard]] AutomationMediaSnapshot CaptureSnapshot() const override {
		return {.has_video = true, .video_width = width, .video_height = height, .has_timecodes = true};
	}
	[[nodiscard]] std::optional<int> FrameFromMs(int ms) const override { return ms / frame_duration; }
	[[nodiscard]] std::optional<int> MsFromFrame(int frame) const override { return frame * frame_duration; }
	[[nodiscard]] std::optional<AutomationVideoInfo> TryGetVideoInfo() const override { return AutomationVideoInfo{.width = width, .height = height, .aspect_ratio = static_cast<double>(width) / height, .aspect_ratio_type = 0}; }
	[[nodiscard]] std::vector<int> GetKeyframes() const override { return {0, 25, 80}; }
	[[nodiscard]] std::optional<AutomationAudioSelection> TryGetAudioSelection() const override { return std::nullopt; }
};

struct HostValues {
	std::string filename = "session.ass";
	std::string script_directory;
	std::string style_storage;
	int video_width = 1280;
	int video_height = 720;
	int frame_duration = 40;
};

class Host final : public AutomationHost {
	AssFile& ass;
	HostValues const values;
	::Media media;

	public:
	::Ui ui;
	std::shared_ptr<AutomationMutationJournal> journal = std::make_shared<AutomationMutationJournal>();
	explicit Host(AssFile& ass, HostValues values = {})
		: ass(ass), values(std::move(values)), media(this->values.video_width, this->values.video_height, this->values.frame_duration) {}
	bool HasProjectContext() const override { return true; }
	void const *ProjectContextIdentity() const override { return &ass; }
	AutomationProjectSnapshot CaptureProjectSnapshot() const override {
		AutomationProjectSnapshot result;
		result.subtitle_file = values.filename;
		result.script_filename = values.filename;
		result.style_storage = values.style_storage;
		result.info_count = static_cast<int>(ass.Info.size());
		result.style_count = static_cast<int>(ass.Styles.size());
		for (auto const& line : ass.Events) {
			++result.event_count;
			if (line.Comment)
				++result.comment_count;
			else
				++result.dialogue_count;
		}
		ass.GetResolution(result.play_res_x, result.play_res_y);
		return result;
	}
	AutomationMediaState const& Media() const override { return media; }
	AutomationUiProxy& Ui() override { return ui; }
	AutomationUiProxy const& Ui() const override { return ui; }
	std::optional<agi::fs::path> TryGetFileName() const override { return values.filename; }
	agi::fs::path DecodePath(std::string const& path) const override {
		return path == "?script" && !values.script_directory.empty() ? values.script_directory : path;
	}
	std::optional<AutomationProjectPropertiesView> TryGetProjectProperties() const override {
		return AutomationProjectPropertiesView{.style_storage = values.style_storage};
	}
	std::shared_ptr<AutomationMutationJournal> GetMutationJournal() const override { return journal; }
};

class History final {
	AssFile& ass;
	agi::signal::Connection connection;

	public:
	std::vector<std::unique_ptr<AssFile>> states;
	std::vector<std::string> messages;
	explicit History(AssFile& ass) : ass(ass) {
		states.push_back(std::make_unique<AssFile>(ass));
		connection = ass.AddUndoManager([this](AssFileCommit commit) {
			messages.push_back(commit.message);
			states.push_back(std::make_unique<AssFile>(this->ass));
			*commit.commit_id = static_cast<int>(states.size() - 1);
		});
	}
	void Restore(size_t index) { ass = *states.at(index); }
};

bool Rejects(std::function<void()> const& action, std::string_view expected) {
	try {
		action();
	}
	catch (AutomationError const& error) {
		return error.GetMessage().find(expected) != std::string::npos;
	}
	return false;
}

void SaveAndCompare(AssFile& ass, fs::path const& path, Results& results) {
	WriteAssFileForCore(&ass, path, agi::vfr::Framerate(25.0), "utf-8", {.write_extradata = true});
	auto reopened = ReadAssFileForCore(path, "utf-8");
	results.Check("saved_ass_dialogue_roundtrip", DialogueData(reopened) == DialogueData(ass));
	results.Check("saved_ass_styles_roundtrip", reopened.GetStyles() == ass.GetStyles());
	results.Check("saved_ass_resolution_roundtrip", reopened.GetScriptInfo("PlayResX") == "1280" && reopened.GetScriptInfo("PlayResY") == "720");
}

json::Object ExerciseCollections(fs::path const& fixtures, fs::path const& artifacts,
								 std::shared_ptr<RuntimeServices> const& services,
								 std::shared_ptr<Host> const& host, AutomationMacroContext const& context,
								 Results& results) {
	auto source_directory = artifacts / "collection-source";
	fs::create_directories(source_directory);
	fs::copy_file(fixtures / "collection.lua", source_directory / "collection.lua", fs::copy_options::overwrite_existing);
	fs::copy_file(fixtures / "failed-load.lua", source_directory / "failed-load.lua", fs::copy_options::overwrite_existing);
	auto registry = std::make_shared<FeatureRegistry>();
	auto session = std::make_shared<AutomationDebugSession>(AutomationDebugLaunchRequest{.enabled = true, .nonblocking = true});
	int collection_changes = 0;
	AutomationScriptCollection global([&](AutomationScriptMutation, std::shared_ptr<AutomationDebugSession> const& owner) {
		if (session->GetStateSnapshot().invocation_active && owner != session)
			throw AutomationError("Workspace collection guard"); }, [&] { ++collection_changes; });
	AutomationScriptCollection local;
	auto attach = [&](AutomationScriptInstance& script) {
		script.SetAutomationHost(host);
		script.SetFeatureRegistry(registry);
	};
	auto candidate = [&](fs::path const& path) {
		auto attempt = LoadAutomationScript(path, false, services);
		Require(attempt.script != nullptr, "Missing collection candidate");
		attach(*attempt.script);
		return std::move(attempt.script);
	};

	results.Begin("collection_scopes_and_load_failures");
	std::vector<agi::fs::path> directories{source_directory, source_directory / "."};
	auto loaded = LoadAutoloadScripts(directories, services);
	results.Check("same_scope_autoload_alias_is_deduplicated", loaded.scripts.size() == 2 && loaded.error_count == 1 && loaded.diagnostics.size() == 1);
	for (auto const& script : loaded.scripts) attach(*script);
	global.ReplaceAll(std::move(loaded.scripts));
	auto snapshots = global.GetSnapshot(AutomationScriptScope::Global);
	results.Check("failed_script_remains_visible_without_features", snapshots.size() == 2 && std::ranges::count_if(snapshots, [](auto const& value) { return value.loaded; }) == 1 && registry->macros.size() == 1 && registry->filters.size() == 1);
	auto id = AutomationScriptId(source_directory / "collection.lua");
	auto *current = global.FindById(id);
	results.Check("stable_lookup_identifies_loaded_script", current && current->GetVersion() == "1");
	auto const loaded_filename = current->GetFilename();
	results.Check("shared_loaded_lookup_finds_global_script", IsAutomationScriptLoaded(loaded_filename, global.GetScripts(), local.GetScripts()));
	auto failed_entry = std::ranges::find_if(global.GetScripts(), [](auto const& entry) { return !entry->GetLoadedState(); });
	results.Check("shared_loaded_lookup_includes_failed_placeholder", failed_entry != global.GetScripts().end() && IsAutomationScriptLoaded((*failed_entry)->GetFilename(), global.GetScripts(), local.GetScripts()));
	auto local_loaded = LoadLocalScripts("~collection.lua|~missing.lua|?bad.lua", source_directory, "", services);
	results.Check("local_reference_diagnostics_do_not_drop_valid_script", local_loaded.scripts.size() == 1 && local_loaded.diagnostics.size() == 2);
	auto warning_count = services->warnings.size();
	attach(*local_loaded.scripts.front());
	local.Add(std::move(local_loaded.scripts.front()));
	auto local_snapshots = local.GetSnapshot(AutomationScriptScope::Local);
	results.Check("shared_loaded_lookup_finds_local_script_and_rejects_absent_file", IsAutomationScriptLoaded(local.GetScripts().front()->GetFilename(), global.GetScripts(), local.GetScripts()) && !IsAutomationScriptLoaded(source_directory / "missing.lua", global.GetScripts(), local.GetScripts()));
	results.Check("same_file_can_exist_in_two_scopes", local_snapshots.size() == 1 && local_snapshots.front().id == id && local_snapshots.front().scope == AutomationScriptScope::Local && global.GetScripts().size() == 2);
	results.Check("duplicate_macro_does_not_replace_published_owner", registry->macros.size() == 1 && registry->filters.size() == 2 && current->GetRuntimeMacros().size() == 1 && local.GetScripts().front()->GetRuntimeMacros().empty() && services->warnings.size() == warning_count + 1);
	local.RemoveAll();
	results.Check("removing_duplicate_scope_preserves_original_features", registry->macros.size() == 1 && registry->filters.size() == 1 && global.FindById(id) == current);

	results.Begin("collection_workspace_replacement_guard");
	session->BeginInvocation(MakeMacroRunInvocation(registry->Find("Collection/Edit").Name()));
	results.Check("active_workspace_blocks_collection_reload", Rejects([&] { global.Reload(current); }, "Workspace collection guard"));
	results.Check("active_workspace_blocks_collection_remove", Rejects([&] { global.Remove(current); }, "Workspace collection guard"));
	results.Check("active_workspace_blocks_unowned_candidate_replacement", Rejects([&] { global.Replace(current, candidate(current->GetFilename())); }, "Workspace collection guard"));
	results.Check("guard_keeps_published_owner_and_metadata", global.FindById(id) == current && current->GetVersion() == "1" && registry->macros.size() == 1);
	session->EndInvocation();
	fs::copy_file(fixtures / "failed-load.lua", source_directory / "collection.lua", fs::copy_options::overwrite_existing);
	results.Check("failed_replacement_keeps_working_script", Rejects([&] { global.Replace(current, candidate(current->GetFilename())); }, "unloaded Workspace candidate") && global.FindById(id) == current && current->GetVersion() == "1");
	fs::copy_file(fixtures / "collection-v2.lua", source_directory / "collection.lua", fs::copy_options::overwrite_existing);
	session->BeginInvocation(MakeMacroRunInvocation(registry->Find("Collection/Edit").Name()));
	current = global.Replace(current, candidate(current->GetFilename()), session);
	session->EndInvocation();
	results.Check("owning_workspace_atomically_replaces_features", current == global.FindById(id) && current->GetVersion() == "2" && registry->macros.size() == 1 && registry->filters.size() == 1 && services->warnings.size() == warning_count + 1);
	auto outcome = registry->Find("Collection/Edit").Run(context);
	results.Check("published_replacement_executes_new_lua", outcome == AutomationInvocationOutcome::Completed && context.selection->GetActiveLine()->Text.get() == "collection-v2");
	global.Reload(current);
	results.Check("managed_reload_keeps_one_feature_owner", global.FindById(id) == current && current->GetVersion() == "2" && registry->macros.size() == 1 && registry->filters.size() == 1);

	results.Begin("autoload_generation_and_lifetime");
	AutomationScriptReloadGate gate;
	auto earlier = gate.Begin();
	auto earlier_result = LoadAutoloadScripts(directories, services);
	auto latest = gate.Begin();
	auto latest_result = LoadAutoloadScripts(directories, services);
	results.Check("later_generation_invalidates_earlier_loaded_candidates", earlier.IsAlive() && !earlier.IsCurrent() && latest.IsCurrent() && latest.Id() > earlier.Id());
	for (auto const& script : latest_result.scripts) attach(*script);
	if (latest.IsCurrent()) global.ReplaceAll(std::move(latest_result.scripts));
	bool stale_published = false;
	if (earlier.IsCurrent()) {
		stale_published = true;
		global.ReplaceAll(std::move(earlier_result.scripts));
	}
	results.Check("late_result_cannot_replace_published_generation", !stale_published && registry->macros.size() == 1 && registry->filters.size() == 1 && global.FindById(id)->GetVersion() == "2");
	auto lifetime_gate = std::make_unique<AutomationScriptReloadGate>();
	auto expired = lifetime_gate->Begin();
	auto expired_result = LoadAutoloadScripts(directories, services);
	lifetime_gate.reset();
	results.Check("destroyed_host_invalidates_loaded_result_ticket", !expired.IsAlive() && !expired.IsCurrent() && expired_result.scripts.size() == 2);
	global.RemoveAll();
	results.Check("shared_loaded_lookup_clears_removed_scopes", !IsAutomationScriptLoaded(loaded_filename, global.GetScripts(), local.GetScripts()));
	results.Check("collection_cleanup_deactivates_published_features", registry->macros.empty() && registry->filters.empty() && registry->macro_activations == registry->macro_deactivations && registry->filter_activations == registry->filter_deactivations);
	results.Begin("local_reference_parent_traversal_beyond_windows_path_limit");
	auto reference_source = fs::absolute(artifacts / "reference-source");
	fs::create_directories(reference_source);
	auto script_filename = fs::path(u8"local \u03a9.lua");
	fs::copy_file(fixtures / "collection.lua", reference_source / script_filename, fs::copy_options::overwrite_existing);
	auto recovery_directory = reference_source;
	std::string parent_reference;
	while (recovery_directory.native().size() < 238) {
		recovery_directory /= "recovery";
		parent_reference += "../";
	}
	fs::create_directories(recovery_directory);
	auto reference = parent_reference + agi::fs::PathToGenericString(script_filename);
	auto query = recovery_directory / agi::fs::PathFromString(reference);
	results.Check("existing_script_has_over_limit_query_and_short_target", query.native().size() >= 260 && query.lexically_normal().native().size() < 260 && fs::is_regular_file(reference_source / script_filename));
	results.Check("over_limit_query_finds_existing_script", agi::fs::FileExists(query));
	auto recovered = LoadLocalScripts("~" + reference + "|~" + parent_reference + "missing.lua", recovery_directory, "", services);
	std::ofstream reference_log(artifacts / "reference-load.log", std::ios::binary | std::ios::trunc);
	for (auto const& diagnostic : recovered.diagnostics) reference_log << diagnostic.message << '\n';
	reference_log.close();
	results.Check("recovery_loads_existing_lua_and_reports_only_missing_reference", recovered.scripts.size() == 1 && recovered.error_count == 0 && recovered.diagnostics.size() == 1 && recovered.scripts.front()->GetLoadedState());
	attach(*recovered.scripts.front());
	local.Add(std::move(recovered.scripts.front()));
	auto recovered_outcome = registry->Find("Collection/Edit").Run(context);
	results.Check("recovered_lua_macro_changes_subtitle", recovered_outcome == AutomationInvocationOutcome::Completed && context.selection->GetActiveLine()->Text.get() == "collection-v1");
	local.RemoveAll();
	json::Object evidence;
	evidence["reference_query_chars"] = static_cast<json::Integer>(query.native().size());
	evidence["reference_target_chars"] = static_cast<json::Integer>(query.lexically_normal().native().size());
	evidence["change_notifications"] = collection_changes;
	evidence["macro_activations"] = registry->macro_activations;
	evidence["macro_deactivations"] = registry->macro_deactivations;
	evidence["filter_activations"] = registry->filter_activations;
	evidence["filter_deactivations"] = registry->filter_deactivations;
	evidence["expected_duplicate_macro_diagnostics"] = static_cast<json::Integer>(services->warnings.size() - warning_count);
	return evidence;
}

json::Object ExerciseFreshHosts(fs::path const& fixtures, fs::path const& artifacts,
								std::shared_ptr<RuntimeServices> const& services, Results& results) {
	results.Begin("fresh_hosts_with_same_project_identity");
	AssFile ass = ReadAssFileForCore(fixtures / "session.ass", "utf-8");
	ass.Commit("Initialize snapshot host", AssFile::COMMIT_NEW);
	Selection selection;
	selection.Select(ass, {0}, 0);
	auto first = std::make_shared<Host>(ass, HostValues{.filename = "snapshot-first.ass", .script_directory = "snapshot-first", .style_storage = "First storage", .video_width = 1280, .video_height = 720, .frame_duration = 40});
	auto second = std::make_shared<Host>(ass, HostValues{.filename = "snapshot-second.ass", .script_directory = "snapshot-second", .style_storage = "Second storage", .video_width = 640, .video_height = 360, .frame_duration = 50});
	auto third = std::make_shared<Host>(ass, HostValues{.filename = "snapshot-third.ass", .script_directory = "snapshot-third", .style_storage = "Third storage", .video_width = 800, .video_height = 450, .frame_duration = 25});
	auto fourth = std::make_shared<Host>(ass, HostValues{.filename = "snapshot-fourth.ass", .script_directory = "snapshot-fourth", .style_storage = "Fourth storage", .video_width = 1920, .video_height = 1080, .frame_duration = 20});
	for (auto const& host : {first, second, third, fourth}) {
		host->ui.state.runtime_log.open(artifacts / "runtime.log", std::ios::binary | std::ios::app);
		Require(host->ui.state.runtime_log.is_open(), "Cannot append snapshot runtime log");
	}
	results.Check("immutable_host_values_share_document_identity", first->ProjectContextIdentity() == second->ProjectContextIdentity() && second->ProjectContextIdentity() == third->ProjectContextIdentity() && third->ProjectContextIdentity() == fourth->ProjectContextIdentity());
	auto registry = std::make_shared<FeatureRegistry>();
	auto script = AutomationEngineRegistry::CreateFromFile(fixtures / "host-snapshot.lua", services);
	results.Check("snapshot_script_loaded", script && script->GetLoadedState());
	script->SetAutomationHost(first);
	script->SetFeatureRegistry(registry);
	script->CommitPendingFeatures();
	auto& macro = registry->Find("Snapshots/Read host");
	AutomationMacroContext context{.ass = &ass, .selection = &selection, .host = first, .document_generation = 42, .workspace_invocation_active = false};
	results.Check("validation_reads_initial_host_snapshot", macro.Validate(context) && macro.Help() == "snapshot-first.ass|snapshot-first|1280|720|25|400|First storage");
	context.host = second;
	results.Check("validation_adopts_fresh_host_without_identity_change", macro.Validate(context) && macro.Help() == "snapshot-second.ass|snapshot-second|640|360|20|500|Second storage");
	context.host = third;
	auto outcome = macro.Run(context);
	results.Check("macro_run_adopts_fresh_host_without_prior_validation", outcome == AutomationInvocationOutcome::Completed && Line(ass, 0).Text.get() == "snapshot-third.ass|snapshot-third|800|450|40|250|Third storage");
	AssFile exported(ass);
	script->GetRuntimeFilters().front()->ProcessSubs(&exported, fourth);
	results.Check("export_prefers_explicit_fresh_host", Line(exported, 0).Text.get() == "snapshot-fourth.ass|snapshot-fourth|1920|1080|50|200|Fourth storage" && Line(ass, 0).Text.get() == "snapshot-third.ass|snapshot-third|800|450|40|250|Third storage");
	auto snapshot = script->TryGetRuntimeStateSnapshot();
	results.Check("runtime_observation_matches_latest_export_host", snapshot && snapshot->context_snapshot.project.subtitle_file == "snapshot-fourth.ass" && snapshot->context_snapshot.media.video_width == 1920);
	SaveAndCompare(exported, artifacts / "host-snapshot.ass", results);
	json::Object evidence;
	evidence["macro_document_text"] = Line(ass, 0).Text.get();
	evidence["export_document_text"] = Line(exported, 0).Text.get();
	return evidence;
}

json::Object ExerciseServiceScopes(fs::path const& fixtures, fs::path const& artifacts, Results& results) {
	results.Begin("per_window_services_share_one_registry");
	auto filename = fixtures / "service-scope.lua";
	results.Check("unbound_factory_requires_explicit_services", Rejects([&] { AutomationEngineRegistry::CreateFromFile(filename); }, "Lua runtime services are unavailable"));
	AssFile ass_a = ReadAssFileForCore(fixtures / "session.ass", "utf-8");
	AssFile ass_b(ass_a);
	ass_a.Commit("Initialize window A", AssFile::COMMIT_NEW);
	ass_b.Commit("Initialize window B", AssFile::COMMIT_NEW);
	Selection selection_a;
	Selection selection_b;
	selection_a.Select(ass_a, {0}, 0);
	selection_b.Select(ass_b, {0}, 0);
	auto host_a = std::make_shared<Host>(ass_a);
	auto host_b = std::make_shared<Host>(ass_b);
	for (auto const& host : {host_a, host_b}) {
		host->ui.state.runtime_log.open(artifacts / "runtime.log", std::ios::binary | std::ios::app);
		Require(host->ui.state.runtime_log.is_open(), "Cannot append services runtime log");
	}
	auto services_a = std::make_shared<RuntimeServices>("window-a:", "module-a", "seed-a");
	auto services_b = std::make_shared<RuntimeServices>("window-b:", "module-b", "seed-b");
	std::weak_ptr<RuntimeServices> lifetime_a = services_a;
	std::weak_ptr<RuntimeServices> lifetime_b = services_b;
	auto script_a = AutomationEngineRegistry::CreateFromFile(filename, services_a);
	auto script_b = AutomationEngineRegistry::CreateForEngine("Lua", filename, services_b);
	results.Check("both_registry_load_paths_select_window_services", script_a && script_b && script_a->GetLoadedState() && script_b->GetLoadedState() && script_a->GetName() == "window-a:scope" && script_b->GetName() == "window-b:scope");
	auto registry_a = std::make_shared<FeatureRegistry>();
	auto registry_b = std::make_shared<FeatureRegistry>();
	script_a->SetAutomationHost(host_a);
	script_a->SetFeatureRegistry(registry_a);
	script_a->CommitPendingFeatures();
	script_b->SetAutomationHost(host_b);
	script_b->SetFeatureRegistry(registry_b);
	script_b->CommitPendingFeatures();
	AutomationMacroContext context_a{.ass = &ass_a, .selection = &selection_a, .host = host_a, .document_generation = 51, .workspace_invocation_active = false};
	AutomationMacroContext context_b{.ass = &ass_b, .selection = &selection_b, .host = host_b, .document_generation = 52, .workspace_invocation_active = false};
	services_a.reset();
	results.Check("live_script_retains_its_window_services", !lifetime_a.expired());
	auto outcome_a = registry_a->Find("Services/Read own scope").Run(context_a);
	auto outcome_b = registry_b->Find("Services/Read own scope").Run(context_b);
	results.Check("window_a_executes_own_translation_clipboard_and_module", outcome_a == AutomationInvocationOutcome::Completed && Line(ass_a, 0).Text.get() == "window-a:scope|module-a|seed-a|window-a:run|module-a:window-a:run");
	results.Check("window_b_does_not_share_window_a_services", outcome_b == AutomationInvocationOutcome::Completed && Line(ass_b, 0).Text.get() == "window-b:scope|module-b|seed-b|window-b:run|module-b:window-b:run" && services_b->GetClipboard() == "module-b:window-b:run");
	script_a.reset();
	context_a.host.reset();
	host_a.reset();
	results.Check("closing_window_a_releases_services_without_registry_leak", lifetime_a.expired() && registry_a->macros.empty() && AutomationEngineRegistry::FindEngine("Lua") != nullptr);
	services_b.reset();
	outcome_b = registry_b->Find("Services/Read own scope").Run(context_b);
	results.Check("window_b_still_runs_after_window_a_closes", outcome_b == AutomationInvocationOutcome::Completed && Line(ass_b, 0).Text.get() == "window-b:scope|module-b|module-b:window-b:run|window-b:run|module-b:window-b:run" && !lifetime_b.expired());
	script_b.reset();
	results.Check("closing_last_script_releases_last_window_services", lifetime_b.expired() && registry_b->macros.empty());
	SaveAndCompare(ass_b, artifacts / "service-scope.ass", results);
	json::Object evidence;
	evidence["window_a_text"] = Line(ass_a, 0).Text.get();
	evidence["window_b_text_after_window_a_closed"] = Line(ass_b, 0).Text.get();
	evidence["both_service_instances_released"] = lifetime_a.expired() && lifetime_b.expired();
	return evidence;
}

json::Object Run(fs::path const& artifacts, Results& results) {
	fs::path const fixtures = "tests/gui-automation/automation-neutral-runtime/fixtures";
	agi::Options options("", R"({"Path":{"Automation":{"Include":"?data/automation/include"}}})", agi::Options::FLUSH_SKIP);
	agi::Path paths;
	paths.SetToken("?data", fs::current_path());
	config::opt = &options;
	config::path = &paths;
	agi::log::LogSink log;
	agi::log::log = &log;
	agi::dispatch::Init([](agi::dispatch::Thunk const& task) { task(); }, [] { return true; });
	auto cleanup = agi::make_scope_exit([] {
		agi::dispatch::Shutdown();
		agi::log::log = nullptr;
		config::opt = nullptr;
		config::path = nullptr;
	});
	auto services = std::make_shared<RuntimeServices>();
	auto registry = std::make_shared<FeatureRegistry>();
	AutomationEngineRegistry::Register(std::make_unique<LuaAutomationEngine>());
	AssFile ass = ReadAssFileForCore(fixtures / "session.ass", "utf-8");
	ass.Commit("Initialize host", AssFile::COMMIT_NEW);
	Selection selection;
	selection.Select(ass, {0, 1}, 0);
	auto host = std::make_shared<Host>(ass);
	host->ui.state.runtime_log.open(artifacts / "runtime.log", std::ios::binary | std::ios::trunc);
	Require(host->ui.state.runtime_log.is_open(), "Cannot open runtime.log");
	AutomationMacroContext context{.ass = &ass, .selection = &selection, .host = host, .document_generation = 23, .workspace_invocation_active = false};
	History history(ass);

	results.Begin("failed_candidate");
	auto failed = AutomationEngineRegistry::CreateFromFile(fixtures / "failed-load.lua", services);
	results.Check("failure_retains_diagnostic", failed && !failed->GetLoadedState() && failed->GetDescription().find("neutral-fixture-load-failure") != std::string::npos);
	failed->SetFeatureRegistry(registry);
	failed->CommitPendingFeatures();
	results.Check("partially_registered_features_discarded", failed->GetRuntimeMacros().empty() && failed->GetRuntimeFilters().empty() && registry->macros.empty() && registry->filters.empty());
	failed.reset();

	results.Begin("candidate_publication");
	auto script = AutomationEngineRegistry::CreateForEngine("Lua", fixtures / "runtime.lua", services);
	results.Check("real_lua_metadata", script && script->GetLoadedState() && script->GetName() == "Neutral runtime fixture" && script->GetVersion() == "1.0" && script->GetAuthor() == "Aegisub E2E" && script->GetEngineName() == "Lua");
	results.Check("candidate_retains_features_before_publish", script->GetRuntimeMacros().size() == 4 && script->GetRuntimeFilters().size() == 1 && registry->macros.empty());
	script->SetAutomationHost(host);
	script->SetFeatureRegistry(registry);
	results.Check("attaching_registry_does_not_publish", registry->macros.empty() && registry->filters.empty());
	script->CommitPendingFeatures();
	script->CommitPendingFeatures();
	results.Check("publish_exactly_once", registry->macros.size() == 4 && registry->filters.size() == 1 && registry->macro_activations == 4 && registry->filter_activations == 1);

	results.Begin("validation_and_toggle");
	auto& edit = registry->Find("Neutral/Edit");
	results.Check("valid_selected_pair", edit.Validate(context));
	results.Check("dynamic_help_and_flags", edit.Help() == "Selected 2: First line" && (edit.Type() & MACRO_DYNAMIC_HELP) && (edit.Type() & MACRO_TOGGLE));
	results.Check("initial_toggle_false", !edit.IsActive(context));
	selection.Select(ass, {0}, 0);
	results.Check("selection_change_invalidates", !edit.Validate(context) && edit.Help() == "Selected 1: First line");
	selection.Select(ass, {0, 1}, 0);

	results.Begin("dialog_accept_staging_and_history");
	auto original = DialogueData(ass);
	host->ui.state.during_dialog = [&] {
		results.Check("pending_undo_and_edits_not_visible_in_live_document", DialogueData(ass) == original && history.messages.empty());
		results.Check("reentrant_validation_guard", !edit.Validate(context) && !edit.IsActive(context));
		results.Check("reentrant_macro_guard", Rejects([&] { edit.Run(context); }, "already running"));
	};
	auto outcome = edit.Run(context);
	host->ui.state.during_dialog = {};
	results.Check("accepted_result_returned_to_host", outcome == AutomationInvocationOutcome::Completed);
#ifdef _WIN32
	results.Check("lua_text_measurement_contract", services->text_extent_calls == 7);
#endif
	results.Check("accepted_invocation_completed", host->ui.state.observation->outcomes == std::vector{AutomationInvocationOutcome::Completed});
	results.Check("dialogs_reached_real_lua", host->ui.state.dialog_count == 1 && host->ui.state.open_count == 1 && host->ui.state.save_count == 1);
	results.Check("typed_values_changed_subtitles", Line(ass, 0).Text.get() == "accepted:|%:1" && Line(ass, 1).Text.get() == "accepted:|%:2" && Line(ass, 0).Start == 1250 && Line(ass, 1).End == 5650 && Line(ass, 0).Style.get() == "Emphasis");
	results.Check("generated_line_and_selection", ass.Events.size() == 4 && Line(ass, 3).Text.get() == "Generated: two\\Nlines" && selection.Rows() == std::vector<int>{1, 3} && selection.GetActiveLine() == &Line(ass, 3));
	results.Check("separate_undo_checkpoints", history.messages == std::vector<std::string>{"Neutral staged checkpoint", "Neutral accepted values", "Neutral/Edit"} && history.states.size() == 4);
	results.Check("history_contains_intermediate_document_states", Line(*history.states[1], 0).Text.get() == "staged before prompt" && history.states[2]->Events.size() == 3 && Line(*history.states[2], 0).Text.get() == "accepted:|%:1");
	results.Check("progress_and_clipboard_services", host->ui.state.title == "Neutral edit" && host->ui.state.message == "Committed prompt values" && host->ui.state.progress_updates > 0 && services->GetClipboard() == "neutral clipboard");
	auto accepted = DialogueData(ass);
	history.Restore(2);
	selection.Select(ass, {0, 1}, 0);
	results.Check("host_undo_restores_previous_checkpoint", ass.Events.size() == 3 && Line(ass, 1).Text.get() == "accepted:|%:2");
	history.Restore(3);
	selection.Select(ass, {1, 3}, 3);
	results.Check("host_redo_restores_document_and_selection", DialogueData(ass) == accepted && selection.Rows() == std::vector<int>{1, 3});
	selection.Select(ass, {0, 1}, 0);
	results.Check("toggle_changes_after_macro", edit.IsActive(context) && edit.Validate(context) && edit.Help() == "Selected 2: accepted:|%:1");
	SaveAndCompare(ass, artifacts / "accepted.ass", results);

	results.Begin("dialog_cancel_rollback");
	host->ui.state.cancel_dialog = true;
	outcome = edit.Run(context);
	host->ui.state.cancel_dialog = false;
	results.Check("cancelled_prompt_discards_staged_checkpoint", DialogueData(ass) == accepted && history.messages.size() == 3 && selection.Rows() == std::vector<int>{0, 1});
	results.Check("cancelled_prompt_outcome", outcome == AutomationInvocationOutcome::Cancelled && host->ui.state.observation->outcomes.back() == AutomationInvocationOutcome::Cancelled);

	results.Begin("stale_invocation_cannot_commit");
	int current_revision = 1;
	context.is_current = [&] { return current_revision == 1; };
	host->ui.state.during_dialog = [&] { ++current_revision; };
	outcome = edit.Run(context);
	host->ui.state.during_dialog = {};
	results.Check("revision_change_cancels_successful_lua_before_commit", outcome == AutomationInvocationOutcome::Cancelled && host->ui.state.observation->outcomes.back() == AutomationInvocationOutcome::Cancelled);
	results.Check("stale_staging_selection_and_history_discarded", DialogueData(ass) == accepted && selection.Rows() == std::vector<int>{0, 1} && history.messages.size() == 3);
	auto dialogs_before_stale_run = host->ui.state.dialog_count;
	outcome = edit.Run(context);
	results.Check("initial_stale_run_has_no_script_side_effects", outcome == AutomationInvocationOutcome::Cancelled && host->ui.state.dialog_count == dialogs_before_stale_run && !edit.Validate(context) && !edit.IsActive(context));
	context.is_current = {};

	results.Begin("progress_cancel_rollback");
	host->ui.state.cancel_at_percent = 30;
	auto progress_before = host->ui.state.progress_updates;
	outcome = registry->Find("Neutral/Cancel progress").Run(context);
	host->ui.state.cancel_at_percent = 0;
	results.Check("finite_progress_cancel_observed", outcome == AutomationInvocationOutcome::Cancelled && host->ui.state.progress_updates - progress_before == 3 && host->ui.state.observation->outcomes.back() == AutomationInvocationOutcome::Cancelled);
	results.Check("cancel_preserves_ass_selection_and_history", DialogueData(ass) == accepted && selection.Rows() == std::vector<int>{0, 1} && history.messages.size() == 3);

	results.Begin("runtime_failure_rollback");
	outcome = registry->Find("Neutral/Failure").Run(context);
	results.Check("lua_failure_is_distinct_from_cancel", outcome == AutomationInvocationOutcome::Failed && host->ui.state.observation->outcomes.back() == AutomationInvocationOutcome::Failed && host->ui.state.log.find("neutral-fixture-runtime-failure") != std::string::npos);
	results.Check("failure_preserves_ass_selection_and_history", DialogueData(ass) == accepted && selection.Rows() == std::vector<int>{0, 1} && history.messages.size() == 3);

	results.Begin("export_config_and_execute");
	auto *filter = script->GetRuntimeFilters().front();
	results.Check("filter_metadata_and_priority", filter->Name() == "Neutral export" && filter->Priority() == 37 && filter->Description() == "Host-neutral export values");
	auto dialog = filter->GenerateConfigDialog(context);
	results.Check("export_dialog_uses_shared_model", dialog && dialog->controls.size() == 10 && !dialog->use_buttons);
	SetDialogValues(*dialog, true);
	auto serialized = dialog->Serialise();
	std::get<AutomationDialogEdit>(Control(*dialog, "prefix").value).value = "discarded";
	std::get<AutomationDialogIntEdit>(Control(*dialog, "offset").value).value = 7;
	dialog->Unserialise(serialized);
	results.Check("export_config_escaping_roundtrip", std::get<AutomationDialogEdit>(Control(*dialog, "prefix").value).value == "export:|%" && std::get<AutomationDialogIntEdit>(Control(*dialog, "offset").value).value == -125 && dialog->Serialise() == serialized);
	filter->SaveSettings(ass, *dialog);
	results.Check("shared_settings_store_serialized_configuration", ass.Properties.automation_settings.at(filter->SettingsIdentifier()) == serialized);
	dialog.reset();
	dialog = filter->GenerateConfigDialog(context);
	results.Check("replacement_configuration_starts_with_script_defaults", dialog && std::get<AutomationDialogEdit>(Control(*dialog, "prefix").value).value == "initial" && std::get<AutomationDialogIntEdit>(Control(*dialog, "offset").value).value == 100);
	filter->LoadSettings(ass, *dialog);
	results.Check("shared_settings_restore_replacement_dialog_with_alpha", dialog->Serialise() == serialized && std::get<AutomationDialogColor>(Control(*dialog, "rgba").value).value == agi::Color(0x12, 0x34, 0x56, 0x78));
	dialog.reset();
	AssFile exported(ass);
	filter->ProcessSubs(&exported, host);
	results.Check("export_values_reach_lua_without_mutating_live_document", Line(exported, 0).Text.get() == "export:|%accepted:|%:1" && Line(exported, 0).Start.GetMillisecond() == 1125 && static_cast<int>(Line(exported, 0).Start) == 1130 && DialogueData(ass) == accepted && history.messages.size() == 3);
	SaveAndCompare(exported, artifacts / "exported.ass", results);

	results.Begin("workspace_guard_and_source_binding");
	context.workspace_invocation_active = true;
	results.Check("active_workspace_disables_regular_validation", !edit.Validate(context) && !edit.IsActive(context));
	results.Check("unrelated_run_is_rejected", Rejects([&] { edit.Run(context); }, "Another Lua Workspace invocation"));
	auto& workspace = registry->Find("Neutral/Workspace");
	auto request = std::make_shared<LuaWorkspaceRunRequest>();
	request->invocation_id = 71;
	request->macro_command = workspace.Name();
	request->sources = std::make_shared<LuaWorkspaceSourceRegistry>();
	request->stop_requested = std::make_shared<std::atomic<bool>>(false);
	auto& code_line = Line(ass, 2);
	LuaWorkspaceSource source{
		.source_identity = "aegisub://lua/23/" + std::to_string(code_line.Id),
		.revision = 4,
		.display_name = "Fixture code line",
		.text = "return 'workspace-bound-result'"};
	source.uri = MakeLuaWorkspaceSourceUri(request->invocation_id, source.source_identity, source.revision);
	request->source_override = LuaWorkspaceSourceOverride{.document_generation = 23, .dialogue_id = code_line.Id, .source = source};
	host->ui.state.workspace = request;
	selection.Select(ass, {2}, 2);
	context.document_generation = 24;
	results.Check("stale_document_binding_rejected", Rejects([&] { workspace.Run(context); }, "different subtitle document"));
	context.document_generation = 23;
	request->source_override->source.source_identity = "aegisub://lua/23/wrong";
	results.Check("mismatched_source_binding_rejected", Rejects([&] { workspace.Run(context); }, "source identity"));
	request->source_override->source = source;
	results.Check("guard_failures_leave_document_intact", DialogueData(ass) == accepted && history.messages.size() == 3);
	Runner validation_runner(host->ui.state);
	results.Check("workspace_validation_accepts_bound_request", workspace.ValidateForWorkspace(context, request, validation_runner));
	outcome = workspace.Run(context);
	results.Check("bound_source_executes_without_overwriting_stored_template", ass.Events.size() == 5 && Line(ass, 4).Text.get() == "workspace-bound-result" && Line(ass, 2).Text.get() == "return \"stored-source\"" && Line(ass, 2).Comment);
	results.Check("workspace_selection_and_source_registry", selection.Rows() == std::vector<int>{4} && selection.GetActiveLine() == &Line(ass, 4) && request->sources->Find(source.uri) && request->sources->Find(source.uri)->text == source.text);
	results.Check("workspace_invocation_completed", outcome == AutomationInvocationOutcome::Completed && host->ui.state.observation->outcomes.back() == AutomationInvocationOutcome::Completed);
	host->ui.state.workspace.reset();
	context.workspace_invocation_active = false;
	SaveAndCompare(ass, artifacts / "workspace.ass", results);

	results.Begin("runtime_snapshots_and_mutation_trace");
	auto const& observation = *host->ui.state.observation;
	results.Check("runtime_snapshots_have_real_host_context", !observation.snapshots.empty() && observation.snapshots.front().context_snapshot.has_project_context && observation.snapshots.front().context_snapshot.media.video_width == 1280 && observation.snapshots.front().context_snapshot.project.style_count == 2);
	auto records = host->journal->GetRecords();
	results.Check("rollback_is_recorded_by_runtime", std::ranges::count_if(records, [](auto const& record) { return record.kind == AutomationMutationRecordKind::Cancel; }) >= 3);
	std::string trace_error;
	results.Check("mutation_trace_written", host->journal->WriteTraceFile(artifacts / "mutations.ndjson", trace_error));

	results.Begin("reload_and_deactivation");
	script->Reload();
	results.Check("reload_deactivates_old_features", script->GetLoadedState() && registry->macros.empty() && registry->filters.empty() && registry->macro_deactivations == 4 && registry->filter_deactivations == 1);
	script->CommitPendingFeatures();
	results.Check("reload_publishes_new_features", registry->macros.size() == 4 && registry->filters.size() == 1 && registry->macro_activations == 8);
	selection.Select(ass, {0, 1}, 0);
	results.Check("reloaded_macro_consumes_live_document", registry->Find("Neutral/Edit").Validate(context));
	script->SetFeatureRegistry({});
	results.Check("detaching_registry_deactivates_features", registry->macros.empty() && registry->filters.empty() && script->GetRuntimeMacros().size() == 4);
	script->SetFeatureRegistry(registry);
	script->CommitPendingFeatures();
	script.reset();
	results.Check("destroy_deactivates_all_published_features", registry->macros.empty() && registry->filters.empty() && registry->macro_activations == registry->macro_deactivations && registry->filter_activations == registry->filter_deactivations);
	results.Check("no_unexpected_service_diagnostics", services->warnings.empty() && services->errors.empty());

	json::Object evidence;
	evidence["collections"] = ExerciseCollections(fixtures, artifacts, services, host, context, results);
	evidence["fresh_hosts"] = ExerciseFreshHosts(fixtures, artifacts, services, results);
	evidence["service_scopes"] = ExerciseServiceScopes(fixtures, artifacts, results);
	json::Object outcomes;
	for (auto const& [name, outcome] : std::vector<std::pair<std::string, AutomationInvocationOutcome>>{{"completed", AutomationInvocationOutcome::Completed}, {"cancelled", AutomationInvocationOutcome::Cancelled}, {"failed", AutomationInvocationOutcome::Failed}}) {
		auto count = std::ranges::count(observation.outcomes, outcome);
		results.Check("nonzero_" + name + "_outcomes", count > 0);
		outcomes[name] = static_cast<json::Integer>(count);
	}
	evidence["outcomes"] = std::move(outcomes);
	evidence["runtime_snapshots"] = static_cast<json::Integer>(observation.snapshots.size());
	evidence["dialog_requests"] = host->ui.state.dialog_count;
	evidence["progress_updates"] = host->ui.state.progress_updates;
	evidence["host_history_commits"] = static_cast<json::Integer>(history.messages.size());
	evidence["macro_activations"] = registry->macro_activations;
	evidence["macro_deactivations"] = registry->macro_deactivations;
	evidence["filter_activations"] = registry->filter_activations;
	evidence["filter_deactivations"] = registry->filter_deactivations;
	evidence["text_extent_calls"] = services->text_extent_calls;
	json::Array files;
	for (auto const *filename : {"accepted.ass", "exported.ass", "workspace.ass", "host-snapshot.ass", "service-scope.ass", "mutations.ndjson", "runtime.log"}) files.emplace_back(filename);
	evidence["artifacts"] = std::move(files);
	return evidence;
}
}

int main(int argc, char **argv) {
	if (argc != 2) {
		std::cerr << "Usage: automation-neutral-runtime-e2e <artifact-directory>\n";
		return 2;
	}
	Results results;
	fs::path artifacts(argv[1]);
	std::error_code error;
	fs::create_directories(artifacts, error);
	if (error) {
		std::cerr << "Could not create artifact directory\n";
		return 2;
	}
	try {
		auto evidence = Run(artifacts, results);
		results.Write(artifacts, true, {}, std::move(evidence));
		std::cout << "Neutral Lua runtime E2E passed; result.json and ASS artifacts written\n";
		return 0;
	}
	catch (agi::Exception const& exception) {
		results.Write(artifacts, false, exception.GetMessage(), {});
		std::cerr << exception.GetMessage() << '\n';
	}
	catch (std::exception const& exception) {
		results.Write(artifacts, false, exception.what(), {});
		std::cerr << exception.what() << '\n';
	}
	return 1;
}
