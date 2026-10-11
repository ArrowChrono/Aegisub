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

#include "../automation_runtime_state_snapshot.h"
#include "../automation_runtime_trace_sink.h"

#include <libaegisub/fs_fwd.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Automation4 {
	class AutomationHost;
	class AutomationDebugSession;
	class AutomationMacro;
	class AutomationExportFilter;
	class AutomationFeatureRegistry;

	class AutomationScriptInstance {
	public:
		virtual ~AutomationScriptInstance() = default;

		virtual void Reload() = 0;
		virtual agi::fs::path GetFilename() const = 0;
		virtual std::string GetName() const = 0;
		virtual std::string GetDescription() const = 0;
		virtual std::string GetAuthor() const = 0;
		virtual std::string GetVersion() const = 0;
		virtual bool GetLoadedState() const = 0;
		virtual std::vector<AutomationMacro *> GetRuntimeMacros() const { return {}; }
		virtual std::vector<AutomationExportFilter *> GetRuntimeFilters() const { return {}; }
		virtual void SetFeatureRegistry(std::shared_ptr<AutomationFeatureRegistry>) {}
		virtual void CommitPendingFeatures() {}
		virtual std::string GetEngineName() const = 0;
		/// Eagerly validate and activate an application-managed installation.
		/// Traditional Automation autoload does not call this and remains lazy.
		virtual void ValidateApplicationActivation() { }

		virtual std::optional<AutomationRuntimeStateSnapshot> TryGetRuntimeStateSnapshot() const { return std::nullopt; }
		virtual void SetRuntimeTraceSink(AutomationRuntimeTraceSink*) { }
		virtual void SetAutomationHost(std::shared_ptr<AutomationHost>) { }
		virtual void SetDebugSession(AutomationDebugSession*) { }
	};
}
