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

#include "automation_runtime_trace_sink.h"
#include "automation_runtime_state_snapshot.h"
#include "automation_template_debug_state.h"
#include "automation_visual_guide_snapshot.h"

#include <memory>
#include <optional>
#include <vector>

namespace agi { struct Context; }
struct lua_State;

namespace Automation4 {
	class AutomationHost;
	struct LuaWorkspaceRunRequest;

	void LuaSetWorkspaceRunRequest(lua_State *L, std::shared_ptr<LuaWorkspaceRunRequest const> request);
	std::shared_ptr<LuaWorkspaceRunRequest const> LuaGetWorkspaceRunRequest(lua_State *L);
	int LuaRaiseWorkspaceCancellation(lua_State *L);
	bool LuaIsWorkspaceCancellation(lua_State *L, int index);

	void LuaSetAutomationRuntimeState(
		lua_State *L,
		std::shared_ptr<AutomationHost> host,
		AutomationInvocation const& invocation,
		std::vector<int> selection,
		int active_row);

	void LuaSetAutomationHost(lua_State *L, std::shared_ptr<AutomationHost> host);
	void LuaSetAutomationRuntimeTraceSink(lua_State *L, AutomationRuntimeTraceSink *sink);

	std::shared_ptr<AutomationHost> LuaGetAutomationHostShared(lua_State *L);
	AutomationHost *LuaGetAutomationHost(lua_State *L);

	void LuaSetAutomationTemplateDebugContext(lua_State *L, int value_index);

	std::optional<AutomationRuntimeStateSnapshot> LuaGetAutomationRuntimeStateSnapshot(lua_State *L);
	std::optional<AutomationTemplateDebugState> LuaGetAutomationTemplateDebugState(lua_State *L);

	// Encodes an Automation value snapshot on the current Lua thread. The
	// caller must obtain the snapshot from its host before entering Lua code.
	void LuaPushVisualGuideSnapshot(lua_State *L, AutomationVisualGuideSnapshot const& snapshot);
}
