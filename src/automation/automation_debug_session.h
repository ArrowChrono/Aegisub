// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include "automation_breakpoint_store.h"
#include "automation_runtime_state_snapshot.h"

#include <libaegisub/fs_fwd.h>

#include <cstddef>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <deque>
#include <vector>

namespace Automation4 {
	class LuaWorkspaceSourceRegistry;

	enum class AutomationDebugPauseReason {
		Entry,
		Breakpoint,
		Step,
		Pause
	};

	enum class AutomationDebugSessionState {
		Created,
		Prepared,
		Running,
		Paused,
		Completed,
		Detached
	};

	enum class AutomationDebugResumeAction {
		Continue,
		StepIn,
		Next,
		StepOut,
		Detach
	};

	struct AutomationDebugLaunchRequest {
		bool enabled = false;
		bool stop_on_entry = false;
		bool nonblocking = false;
		int auto_step_count = 0;
		size_t max_pauses = 128;
		std::vector<AutomationDebugBreakpoint> breakpoints;
	};

	struct AutomationDebugTarget {
		std::string engine_name;
		agi::fs::path script_file;
		std::string feature_name;
	};

	struct AutomationDebugLocation {
		std::string source_path;
		std::string source_kind;
		std::string display_name;
		int line = 0;
		int column = 0;
	};

	struct AutomationDebugVariable {
		std::string name;
		std::string value;
		std::string value_type;
		std::vector<AutomationDebugVariable> children;
	};

	struct AutomationDebugScope {
		std::string name;
		std::vector<AutomationDebugVariable> variables;
	};

	struct AutomationDebugFrame {
		int level = 0;
		std::string kind;
		std::string function_name;
		AutomationDebugLocation location;
		std::vector<AutomationDebugVariable> locals;
		std::vector<AutomationDebugVariable> upvalues;
	};

	struct AutomationDebugPauseRecord {
		size_t sequence = 0;
		AutomationDebugPauseReason reason = AutomationDebugPauseReason::Breakpoint;
		AutomationDebugLocation location;
		std::vector<AutomationDebugFrame> frames;
		std::vector<AutomationDebugScope> scopes;
		std::optional<AutomationRuntimeStateSnapshot> runtime_snapshot;
	};

	struct AutomationDebugCapturedState {
		std::vector<AutomationDebugFrame> frames;
		std::vector<AutomationDebugScope> scopes;
		std::optional<AutomationRuntimeStateSnapshot> runtime_snapshot;
	};

	struct AutomationDebugStateSnapshot {
		size_t version = 0;
		AutomationDebugSessionState state = AutomationDebugSessionState::Created;
		bool invocation_active = false;
		bool attached = true;
		bool pause_requested = false;
		AutomationDebugTarget target;
		std::optional<AutomationDebugPauseRecord> current_pause;
		std::string message;
		int exit_code = 0;
	};

	std::string ToString(AutomationDebugPauseReason reason);
	std::string ToString(AutomationDebugSessionState state);

	class AutomationDebugSession final {
		enum class StepMode {
			None,
			Into,
			Over,
			Out
		};

		mutable std::mutex mutex;
		mutable std::condition_variable cv;
		AutomationDebugLaunchRequest request;
		AutomationBreakpointStore breakpoints;
		AutomationDebugTarget target;
		std::shared_ptr<LuaWorkspaceSourceRegistry> source_registry;
		std::deque<AutomationDebugPauseRecord> pauses;
		std::optional<AutomationDebugPauseRecord> current_pause;
		std::optional<AutomationDebugLocation> resume_skip_location;
		size_t resume_skip_depth = 0;
		AutomationDebugSessionState state = AutomationDebugSessionState::Created;
		bool invocation_active = false;
		bool attached = true;
		bool pause_requested = false;
		bool pause_command_queued = false;
		bool entry_pause_pending = false;
		int pending_step_pauses = 0;
		StepMode step_mode = StepMode::None;
		size_t step_depth = 0;
		size_t state_version = 0;
		size_t pause_count = 0;
		size_t entry_pause_count = 0;
		size_t breakpoint_pause_count = 0;
		size_t step_pause_count = 0;
		size_t manual_pause_count = 0;
		size_t dropped_pause_count = 0;
		AutomationDebugResumeAction resume_action = AutomationDebugResumeAction::Continue;
		std::string completion_message;
		int completion_exit_code = 0;

		bool PauseMatchesStepMode(size_t stack_depth) const;
		void BumpStateVersion();
		void UpdateStateLocked(AutomationDebugSessionState next_state);

	public:
		explicit AutomationDebugSession(AutomationDebugLaunchRequest request);

		bool Enabled() const;
		void SetTarget(AutomationDebugTarget target);
		AutomationDebugTarget GetTarget() const;
		void SetSourceRegistry(std::shared_ptr<LuaWorkspaceSourceRegistry> registry);
		std::shared_ptr<LuaWorkspaceSourceRegistry> GetSourceRegistry() const;
		void SetBreakpoints(std::vector<AutomationDebugBreakpoint> values);
		std::vector<AutomationDebugBreakpoint> GetBreakpoints() const;
		void BeginInvocation(AutomationInvocation const& invocation);
		void EndInvocation();
		void MarkCompleted(int exit_code, std::string message);
		void RequestPause();
		void Detach();
		bool Resume(AutomationDebugResumeAction action);
		AutomationDebugStateSnapshot GetStateSnapshot() const;
		AutomationDebugStateSnapshot WaitForStateChange(size_t after_version) const;
		bool HandleHookPause(
			AutomationDebugLocation location,
			size_t stack_depth,
			std::function<AutomationDebugCapturedState()> capture_state);

		size_t PauseCount() const;
		size_t EntryPauseCount() const;
		size_t BreakpointPauseCount() const;
		size_t StepPauseCount() const;
		size_t ManualPauseCount() const;
		size_t DroppedPauseCount() const;
		size_t BreakpointCount() const;
		std::vector<AutomationDebugPauseRecord> GetPauses() const;

		bool WriteTraceFile(agi::fs::path const& path, std::string& error) const;
	};
}
