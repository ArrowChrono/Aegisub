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

#include "automation_debug_backend.h"

#include <libaegisub/fs_fwd.h>

#include <set>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;
struct lua_Debug;

namespace Automation4 {
struct LuaWorkspaceRunRequest;
class AutomationLuaDebugBackend final : public AutomationDebugBackend {
	lua_State *L = nullptr;
	agi::fs::path script_file;
	std::set<std::string> runtime_globals_baseline;
	std::shared_ptr<LuaWorkspaceRunRequest const> workspace_request;
	int (*original_sethook)(lua_State *) = nullptr;

	void UpdateHookState();
	void OnHook(lua_State *hook_L, lua_Debug *ar);
	void OnDebugStateChanged() override;

	AutomationDebugLocation BuildLocation(lua_State *hook_L, lua_Debug const& ar) const;
	size_t CaptureStackDepth(lua_State *hook_L) const;
	std::vector<AutomationDebugFrame> CaptureFrames(lua_State *hook_L) const;

	static void Hook(lua_State *L, lua_Debug *ar);
	static int GuardedSetHook(lua_State *L);

	public:
		AutomationLuaDebugBackend(lua_State *L, agi::fs::path script_file);
		~AutomationLuaDebugBackend();
		void SetWorkspaceRunRequest(std::shared_ptr<LuaWorkspaceRunRequest const> request);
		void InstallSetHookGuard();

		void CaptureRuntimeBaseline() override;
		bool IsRuntimeGlobal(std::string_view name) const;
};
}
