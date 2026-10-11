// Copyright (c) 2006, 2007, Niels Martin Hansen
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

/// @file auto4_lua_progresssink.cpp
/// @brief Lua 5.1-based scripting engine
/// @ingroup scripting
///

#include "auto4_lua.h"

#include "automation/automation_host.h"
#include "automation/automation_lua_runtime.h"

#include <libaegisub/lua/utils.h>

#include <cassert>

using namespace agi::lua;

namespace {
	constexpr char kAutomationDebugProgressRegistryKey[] = "automation_debug_progress_state";
	constexpr char kAutomationDebugDialogRegistryKey[] = "automation_debug_dialog_state";

	template<lua_CFunction fn>
	void set_field_to_closure(lua_State *L, const char *name, int ps_idx = -3)
	{
		lua_pushvalue(L, ps_idx);
		lua_pushcclosure(L, exception_wrapper<fn>, 1);
		lua_setfield(L, -2, name);
	}

	void set_field_to_nil(lua_State *L, int idx, const char *name)
	{
		lua_pushnil(L);
		lua_setfield(L, idx, name);
	}

	void ensure_registry_table(lua_State *L, char const* key, int narr = 0, int nrec = 0)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, key);
		if (lua_istable(L, -1))
			return;
		lua_pop(L, 1);
		lua_createtable(L, narr, nrec);
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, key);
	}

	void clear_registry_table(lua_State *L, char const* key)
	{
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, key);
	}

	void record_dialog_controls(lua_State *L, int table_index)
	{
		table_index = table_index < 0 ? lua_gettop(L) + table_index + 1 : table_index;
		size_t const count = lua_objlen(L, table_index);
		lua_createtable(L, static_cast<int>(count), 0);
		for (size_t i = 1; i <= count; ++i) {
			lua_rawgeti(L, table_index, static_cast<int>(i));
			if (!lua_istable(L, -1)) {
				lua_pop(L, 1);
				continue;
			}

			lua_createtable(L, 0, 8);
			for (auto const* string_name : { "class", "name", "label", "hint" }) {
				lua_getfield(L, -2, string_name);
				if (lua_isstring(L, -1))
					lua_setfield(L, -2, string_name);
				else
					lua_pop(L, 1);
			}
			for (auto const* numeric_name : { "x", "y", "width", "height" }) {
				lua_getfield(L, -2, numeric_name);
				if (lua_isnumber(L, -1))
					lua_setfield(L, -2, numeric_name);
				else
					lua_pop(L, 1);
			}
			lua_rawseti(L, -3, static_cast<int>(i));
			lua_pop(L, 1);
		}
		lua_setfield(L, -2, "controls");
	}

	void record_dialog_buttons(lua_State *L, int table_index, char const* field_name)
	{
		table_index = table_index < 0 ? lua_gettop(L) + table_index + 1 : table_index;
		if (!lua_istable(L, table_index))
			return;

		size_t const count = lua_objlen(L, table_index);
		lua_createtable(L, static_cast<int>(count), 0);
		int out_index = 1;
		for (size_t i = 1; i <= count; ++i) {
			lua_rawgeti(L, table_index, static_cast<int>(i));
			if (lua_isstring(L, -1))
				lua_rawseti(L, -2, out_index++);
			else
				lua_pop(L, 1);
		}
		lua_setfield(L, -2, field_name);
	}

	void record_dialog_display_request(lua_State *L)
	{
		ensure_registry_table(L, kAutomationDebugDialogRegistryKey, 0, 8);
		set_field(L, "kind", "display");
		set_field(L, "state", "requested");
		set_field(L, "control_count", static_cast<int>(lua_objlen(L, 1)));
		record_dialog_controls(L, 1);
		if (lua_istable(L, 2)) {
			set_field(L, "button_count", static_cast<int>(lua_objlen(L, 2)));
			record_dialog_buttons(L, 2, "buttons");
		}
		else {
			set_field(L, "button_count", 0);
		}
		lua_pop(L, 1);
	}

	void record_dialog_display_result(lua_State *L, int result_count)
	{
		ensure_registry_table(L, kAutomationDebugDialogRegistryKey, 0, 8);
		set_field(L, "state", "completed");
		if (result_count == 2) {
			if (lua_isstring(L, -2)) {
				set_field(L, "accepted", true);
				set_field(L, "button", std::string(lua_tostring(L, -2)));
			}
			else {
				set_field(L, "accepted", false);
				set_field(L, "button", std::string("cancel"));
			}
			if (lua_istable(L, -1))
				set_field(L, "result_field_count", static_cast<int>(lua_objlen(L, -1)));
		}
		lua_pop(L, 1);
	}

	void record_file_dialog_request(lua_State *L, char const* kind, std::string const& title, std::string const& path, std::string const& filter)
	{
		ensure_registry_table(L, kAutomationDebugDialogRegistryKey, 0, 8);
		set_field(L, "kind", kind);
		set_field(L, "state", "requested");
		set_field(L, "title", title);
		set_field(L, "path", path);
		set_field(L, "filter", filter);
		lua_pop(L, 1);
	}

	void record_file_dialog_result_none(lua_State *L)
	{
		ensure_registry_table(L, kAutomationDebugDialogRegistryKey, 0, 8);
		set_field(L, "state", "cancelled");
		lua_pop(L, 1);
	}

	void record_file_dialog_result_single(lua_State *L, std::string const& path)
	{
		ensure_registry_table(L, kAutomationDebugDialogRegistryKey, 0, 8);
		set_field(L, "state", "completed");
		set_field(L, "result", path);
		lua_pop(L, 1);
	}

	void record_file_dialog_result_multiple(lua_State *L, std::vector<agi::fs::path> const& files)
	{
		ensure_registry_table(L, kAutomationDebugDialogRegistryKey, 0, 8);
		set_field(L, "state", "completed");
		lua_createtable(L, static_cast<int>(files.size()), 0);
		for (size_t i = 0; i < files.size(); ++i) {
			lua_pushstring(L, agi::fs::PathToString(files[i]).c_str());
			lua_rawseti(L, -2, static_cast<int>(i + 1));
		}
		lua_setfield(L, -2, "results");
		lua_pop(L, 1);
	}
}

namespace Automation4 {
LuaProgressSink::LuaProgressSink(lua_State *L, AutomationProgress *ps, AutomationInvocation const& invocation)
	: L(L) {
	auto ud = static_cast<AutomationProgress **>(lua_newuserdata(L, sizeof(AutomationProgress *)));
	*ud = ps;

		// register progress reporting stuff
	lua_getglobal(L, "aegisub");

		// Create aegisub.progress table
	lua_createtable(L, 0, 5);
	set_field_to_closure<LuaSetProgress>(L, "set");
	set_field_to_closure<LuaSetTask>(L, "task");
	set_field_to_closure<LuaSetTitle>(L, "title");
	set_field_to_closure<LuaGetCancelled>(L, "is_cancelled");
	lua_setfield(L, -2, "progress");

		// Create aegisub.debug table
	lua_createtable(L, 0, 4);
	set_field_to_closure<LuaDebugOut>(L, "out");
	lua_setfield(L, -2, "debug");

		// Set aegisub.log
	set_field_to_closure<LuaDebugOut>(L, "log", -2);

	auto *host = LuaGetAutomationHost(L);
	if (invocation.capabilities.allow_dialog && host && host->Ui().SupportsInteractiveDialogs()) {
		lua_createtable(L, 0, 3);
		set_field_to_closure<LuaDisplayDialog>(L, "display");
		set_field_to_closure<LuaDisplayOpenDialog>(L, "open");
		set_field_to_closure<LuaDisplaySaveDialog>(L, "save");
		lua_setfield(L, -2, "dialog");
	}

		// reference so other objects can also find the progress sink
	lua_pushvalue(L, -2);
	lua_setfield(L, LUA_REGISTRYINDEX, "progress_sink");

	ensure_registry_table(L, kAutomationDebugProgressRegistryKey, 0, 6);
	set_field(L, "percent", 0.0);
	set_field(L, "task", std::string());
	set_field(L, "title", std::string());
	set_field(L, "cancelled", false);
	set_field(L, "dialog_available", invocation.capabilities.allow_dialog && host && host->Ui().SupportsInteractiveDialogs());
	lua_pop(L, 1);
	clear_registry_table(L, kAutomationDebugDialogRegistryKey);

	lua_pop(L, 2);
}

	LuaProgressSink::~LuaProgressSink()
	{
		// remove progress reporting stuff
		lua_getglobal(L, "aegisub");
		set_field_to_nil(L, -2, "progress");
		set_field_to_nil(L, -2, "debug");
		lua_pop(L, 1);

		set_field_to_nil(L, LUA_REGISTRYINDEX, "progress_sink");
		clear_registry_table(L, kAutomationDebugProgressRegistryKey);
		clear_registry_table(L, kAutomationDebugDialogRegistryKey);
	}

	AutomationProgress *LuaProgressSink::GetObjPointer(lua_State *L, int idx) {
		assert(lua_type(L, idx) == LUA_TUSERDATA);
		return *static_cast<AutomationProgress **>(lua_touserdata(L, idx));
	}

	int LuaProgressSink::LuaSetProgress(lua_State *L)
	{
		auto progress = lua_tonumber(L, 1);
		GetObjPointer(L, lua_upvalueindex(1))->SetProgress(progress, 100);
		ensure_registry_table(L, kAutomationDebugProgressRegistryKey, 0, 6);
		set_field(L, "percent", progress);
		lua_pop(L, 1);
		return 0;
	}

	int LuaProgressSink::LuaSetTask(lua_State *L)
	{
		auto task = check_string(L, 1);
		GetObjPointer(L, lua_upvalueindex(1))->SetMessage(task);
		ensure_registry_table(L, kAutomationDebugProgressRegistryKey, 0, 6);
		set_field(L, "task", task);
		lua_pop(L, 1);
		return 0;
	}

	int LuaProgressSink::LuaSetTitle(lua_State *L)
	{
		auto title = check_string(L, 1);
		GetObjPointer(L, lua_upvalueindex(1))->SetTitle(title);
		ensure_registry_table(L, kAutomationDebugProgressRegistryKey, 0, 6);
		set_field(L, "title", title);
		lua_pop(L, 1);
		return 0;
	}

	int LuaProgressSink::LuaGetCancelled(lua_State *L)
	{
		auto cancelled = GetObjPointer(L, lua_upvalueindex(1))->IsCancelled();
		ensure_registry_table(L, kAutomationDebugProgressRegistryKey, 0, 6);
		set_field(L, "cancelled", cancelled);
		lua_pop(L, 1);
		lua_pushboolean(L, cancelled);
		return 1;
	}

	int LuaProgressSink::LuaDebugOut(lua_State *L)
	{
		AutomationProgress *ps = GetObjPointer(L, lua_upvalueindex(1));

		// Check trace level
		if (lua_type(L, 1) == LUA_TNUMBER) {
			if (lua_tointeger(L, 1) > ps->GetTraceLevel())
				return 0;
			// remove trace level
			lua_remove(L, 1);
		}

		// Only do format-string handling if there's more than one argument left
		// (If there's more than one argument left, assume first is a format string and rest are format arguments)
		if (lua_gettop(L) > 1) {
			// Format the string
			lua_getglobal(L, "string");
			lua_getfield(L, -1, "format");
			// Here stack contains format string, format arguments, 'string' table, format function
			// remove 'string' table
			lua_remove(L, -2);
			// put the format function into place
			lua_insert(L, 1);
			// call format function
			if (lua_pcall(L, lua_gettop(L) - 1, 1, 0)) {
				// format failed so top of the stack now has an error message
				// which we want to add position information to
				luaL_where(L, 1);
				lua_insert(L, 1);
				lua_concat(L, 2);
				throw error_tag{};
			}
		}

		// Top of stack is now a string to output
		ps->Log(check_string(L, 1));
		return 0;
	}

	int LuaProgressSink::LuaDisplayDialog(lua_State *L)
	{
		AutomationProgress *ps = GetObjPointer(L, lua_upvalueindex(1));
		auto *host = LuaGetAutomationHost(L);
		if (!host)
			return error(L, "Automation host unavailable for dialog.display");
		record_dialog_display_request(L);

		LuaDialog dlg(L, true);
		host->Ui().ShowDialog(*ps, dlg);
		int result_count = dlg.LuaReadBack(L);
		record_dialog_display_result(L, result_count);
		return result_count;
	}

	int LuaProgressSink::LuaDisplayOpenDialog(lua_State *L)
	{
		AutomationProgress *ps = GetObjPointer(L, lua_upvalueindex(1));
		auto *host = LuaGetAutomationHost(L);
		if (!host)
			return error(L, "Automation host unavailable for dialog.open");
		AutomationOpenFileDialogRequest request{
			check_string(L, 1),
			check_string(L, 2),
			check_string(L, 3),
			check_string(L, 4),
			!!lua_toboolean(L, 5),
			lua_toboolean(L, 6) || lua_isnil(L, 6)
		};
		record_file_dialog_request(L, "open", request.message, request.dir, request.wildcard);

		auto files = host->Ui().RequestOpenFiles(*ps, request);
		if (files.empty()) {
			record_file_dialog_result_none(L);
			lua_pushnil(L);
			return 1;
		}

		if (request.multiple) {
			record_file_dialog_result_multiple(L, files);
			lua_createtable(L, files.size(), 0);
			for (size_t i = 0; i < files.size(); ++i) {
				lua_pushstring(L, agi::fs::PathToString(files[i]).c_str());
				lua_rawseti(L, -2, i + 1);
			}

			return 1;
		}

		record_file_dialog_result_single(L, agi::fs::PathToString(files.front()));
		lua_pushstring(L, agi::fs::PathToString(files.front()).c_str());
		return 1;
	}

	int LuaProgressSink::LuaDisplaySaveDialog(lua_State *L)
	{
		AutomationProgress *ps = GetObjPointer(L, lua_upvalueindex(1));
		auto *host = LuaGetAutomationHost(L);
		if (!host)
			return error(L, "Automation host unavailable for dialog.save");
		auto path = host->Ui().RequestSaveFile(*ps, {
			check_string(L, 1),
			check_string(L, 2),
			check_string(L, 3),
			check_string(L, 4),
			!lua_toboolean(L, 5)
		});
		record_file_dialog_request(L, "save", check_string(L, 1), check_string(L, 2), check_string(L, 4));
		if (path.empty()) {
			record_file_dialog_result_none(L);
			lua_pushnil(L);
			return 1;
		}

		record_file_dialog_result_single(L, agi::fs::PathToString(path));
		lua_pushstring(L, agi::fs::PathToString(path).c_str());
		return 1;
	}
}
