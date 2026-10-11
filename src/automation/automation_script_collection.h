#pragma once

#include "automation_error.h"
#include "engine/automation_script_instance.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Automation4 {

class AutomationRuntimeServices;

enum class AutomationScriptScope { Global,
								   Local };
enum class AutomationScriptMutation { Add,
									  Remove,
									  Clear,
									  Reload,
									  Replace,
									  ReplaceAll };
using AutomationScriptMutationGuard = std::function<void(AutomationScriptMutation, std::shared_ptr<AutomationDebugSession> const&)>;

struct AutomationScriptSnapshot {
	std::string id;
	AutomationScriptScope scope;
	agi::fs::path filename;
	std::string name;
	std::string description;
	std::string author;
	std::string version;
	std::string engine;
	bool loaded = false;
};

std::string AutomationScriptId(agi::fs::path const& filename);

template <class GlobalScript, class LocalScript>
bool IsAutomationScriptLoaded(agi::fs::path const& filename,
							  std::vector<std::unique_ptr<GlobalScript>> const& global_scripts,
							  std::vector<std::unique_ptr<LocalScript>> const& local_scripts) {
	auto matches = [&](auto const& script) { return filename == script->GetFilename(); };
	return std::ranges::any_of(local_scripts, matches) || std::ranges::any_of(global_scripts, matches);
}

template <class ScriptType = AutomationScriptInstance>
class AutomationScriptCollection {
	AutomationScriptMutationGuard mutation_guard;
	std::function<void()> changed;

	void CheckMutation(AutomationScriptMutation mutation, std::shared_ptr<AutomationDebugSession> const& owner = {}) const {
		if (mutation_guard) mutation_guard(mutation, owner);
	}

	void NotifyChanged() {
		if (changed) changed();
	}

	protected:
	std::vector<std::unique_ptr<ScriptType>> scripts;

	public:
	explicit AutomationScriptCollection(AutomationScriptMutationGuard mutation_guard = {}, std::function<void()> changed = {})
		: mutation_guard(std::move(mutation_guard)), changed(std::move(changed)) {}
	virtual ~AutomationScriptCollection() = default;
	AutomationScriptCollection(AutomationScriptCollection const&) = delete;
	AutomationScriptCollection& operator=(AutomationScriptCollection const&) = delete;

	void Add(std::unique_ptr<ScriptType> script) {
		CheckMutation(AutomationScriptMutation::Add);
		if (!script) return;
		script->CommitPendingFeatures();
		scripts.emplace_back(std::move(script));
		NotifyChanged();
	}

	void Remove(ScriptType *script) {
		CheckMutation(AutomationScriptMutation::Remove);
		auto found = std::ranges::find_if(scripts, [=](auto const& entry) { return entry.get() == script; });
		if (found != scripts.end()) scripts.erase(found);
		NotifyChanged();
	}

	void RemoveAll() {
		CheckMutation(AutomationScriptMutation::Clear);
		scripts.clear();
		NotifyChanged();
	}

	void Reload(ScriptType *script, std::shared_ptr<AutomationDebugSession> const& owner = {}) {
		CheckMutation(AutomationScriptMutation::Reload, owner);
		if (!script || std::ranges::none_of(scripts, [=](auto const& entry) { return entry.get() == script; }))
			throw AutomationError("The script is no longer managed");
		script->Reload();
		script->CommitPendingFeatures();
		NotifyChanged();
	}

	ScriptType *Replace(ScriptType *current, std::unique_ptr<ScriptType> replacement, std::shared_ptr<AutomationDebugSession> const& owner = {}) {
		CheckMutation(AutomationScriptMutation::Replace, owner);
		if (!current || !replacement || !replacement->GetLoadedState())
			throw AutomationError("Cannot replace a script with an unloaded Workspace candidate");
		auto found = std::ranges::find_if(scripts, [=](auto const& script) { return script.get() == current; });
		if (found == scripts.end())
			throw AutomationError("The Workspace script is no longer managed");
		if (replacement->GetFilename() != (*found)->GetFilename())
			throw AutomationError("The Workspace replacement does not match the managed script");
		auto *published = replacement.get();
		std::swap(*found, replacement);
		replacement.reset();
		published->CommitPendingFeatures();
		NotifyChanged();
		return published;
	}

	void ReplaceAll(std::vector<std::unique_ptr<ScriptType>> replacements) {
		CheckMutation(AutomationScriptMutation::ReplaceAll);
		scripts.clear();
		for (auto& script : replacements) {
			if (script && script->GetLoadedState()) script->CommitPendingFeatures();
		}
		scripts = std::move(replacements);
		NotifyChanged();
	}

	std::vector<std::unique_ptr<ScriptType>> const& GetScripts() const { return scripts; }
	ScriptType *FindById(std::string_view id) const {
		for (auto const& script : scripts) {
			if (script && AutomationScriptId(script->GetFilename()) == id) return script.get();
		}
		return nullptr;
	}

	std::vector<AutomationScriptSnapshot> GetSnapshot(AutomationScriptScope scope) const {
		std::vector<AutomationScriptSnapshot> result;
		result.reserve(scripts.size());
		for (auto const& script : scripts) {
			if (!script) continue;
			result.push_back({AutomationScriptId(script->GetFilename()), scope, script->GetFilename(), script->GetName(),
							  script->GetDescription(), script->GetAuthor(), script->GetVersion(), script->GetEngineName(), script->GetLoadedState()});
		}
		return result;
	}
};

struct AutomationScriptLoadAttempt {
	std::unique_ptr<AutomationScriptInstance> script;
	bool recognised = false;
};

struct AutomationScriptLoadDiagnostic {
	std::string message;
	bool error = false;
};

struct AutomationScriptLoadResult {
	std::vector<std::unique_ptr<AutomationScriptInstance>> scripts;
	int error_count = 0;
	std::vector<AutomationScriptLoadDiagnostic> diagnostics;
};

AutomationScriptLoadAttempt LoadAutomationScript(agi::fs::path const& filename, bool create_unknown = false,
												 std::shared_ptr<AutomationRuntimeServices> const& services = {});
AutomationScriptLoadResult LoadAutoloadScripts(std::span<agi::fs::path const> directories,
											   std::shared_ptr<AutomationRuntimeServices> const& services = {});
AutomationScriptLoadResult LoadLocalScripts(std::string const& references, agi::fs::path const& subtitle_directory,
											std::string const& automation_directory, std::shared_ptr<AutomationRuntimeServices> const& services = {});

class AutomationScriptReloadGate {
	struct State {
		std::atomic<uint64_t> generation{0};
	};
	std::shared_ptr<State> state = std::make_shared<State>();

	public:
	class Ticket {
		std::weak_ptr<State> state;
		uint64_t generation;
		friend class AutomationScriptReloadGate;
		Ticket(std::weak_ptr<State> state, uint64_t generation) : state(std::move(state)), generation(generation) {}

		public:
		uint64_t Id() const { return generation; }
		bool IsAlive() const { return !state.expired(); }
		bool IsCurrent() const {
			auto current = state.lock();
			return current && current->generation.load(std::memory_order_relaxed) == generation;
		}
	};

	AutomationScriptReloadGate() = default;
	AutomationScriptReloadGate(AutomationScriptReloadGate const&) = delete;
	AutomationScriptReloadGate& operator=(AutomationScriptReloadGate const&) = delete;
	Ticket Begin() { return Ticket(state, state->generation.fetch_add(1, std::memory_order_relaxed) + 1); }
};

}
