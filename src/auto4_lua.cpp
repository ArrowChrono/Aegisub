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

/// @file auto4_lua.cpp
/// @brief Lua 5.1-based scripting engine
/// @ingroup scripting
///

#include "auto4_lua.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_style.h"
#include "automation/automation_edit_box_request_policy.h"
#include "automation/automation_debug_session.h"
#include "automation/automation_debug_service.h"
#include "automation/automation_invocation_observer.h"
#include "automation/automation_live_host.h"
#include "automation/automation_lua_debug_backend.h"
#include "automation/automation_lua_runtime.h"
#include "async_video_provider.h"
#include "auto4_lua_factory.h"
#include "audio_controller.h"
#include "audio_timing.h"
#include "command/command.h"
#include "compat.h"
#ifdef WITH_PLUGIN_BRIDGE
#include "coreclr/plugin_lua_api.h"
#endif
#include "frame_main.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "subs_controller.h"
#include "video_controller.h"
#include "wx_automation_file_dialog_service.h"
#include "utils.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/format.h>
#include <libaegisub/log.h>
#include <libaegisub/lua/ffi.h>
#include <libaegisub/lua/modules.h>
#include <libaegisub/lua/script_reader.h>
#include <libaegisub/lua/utils.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>
#include <libaegisub/scope_exit.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <cassert>
#include <exception>
#include <unordered_set>
#include <wx/clipbrd.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/translation.h>

using namespace agi::lua;
using namespace Automation4;

namespace {
	constexpr char kTemplateDebugEnabledRegistryKey[] = "automation_template_debug_enabled";
	constexpr char kEditBoxRequestRegistryKey[] = "automation_edit_box_request";

	struct PendingEditBoxRequest {
		bool requested = false;
		bool focus = false;
		bool has_cursor = false;
		int character_index = 0;
		bool after = false;
		bool has_selection = false;
		int selection_start = 0;
		int selection_stop = 0;
	};

	wxString get_wxstring(lua_State *L, int idx)
	{
		return wxString::FromUTF8(lua_tostring(L, idx));
	}

	wxString check_wxstring(lua_State *L, int idx)
	{
		return to_wx(check_string(L, idx));
	}

	AutomationHost *get_host(lua_State *L)
	{
		return LuaGetAutomationHost(L);
	}

	void ensure_edit_box_request_table(lua_State *L)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, kEditBoxRequestRegistryKey);
		if (lua_istable(L, -1))
			return;

		lua_pop(L, 1);
		lua_createtable(L, 0, 4);
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, kEditBoxRequestRegistryKey);
	}

	void clear_pending_edit_box_request(lua_State *L)
	{
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, kEditBoxRequestRegistryKey);
	}

	bool table_bool_field(lua_State *L, int index, char const* field)
	{
		lua_getfield(L, index, field);
		bool value = !!lua_toboolean(L, -1);
		lua_pop(L, 1);
		return value;
	}

	int table_int_field(lua_State *L, int index, char const* field)
	{
		lua_getfield(L, index, field);
		int value = lua_isnumber(L, -1) ? static_cast<int>(lua_tointeger(L, -1)) : 0;
		lua_pop(L, 1);
		return value;
	}

	PendingEditBoxRequest take_pending_edit_box_request(lua_State *L)
	{
		PendingEditBoxRequest request;
		lua_getfield(L, LUA_REGISTRYINDEX, kEditBoxRequestRegistryKey);
		if (lua_istable(L, -1)) {
			request.focus = table_bool_field(L, -1, "focus");
			request.has_cursor = table_bool_field(L, -1, "has_cursor");
			request.character_index = table_int_field(L, -1, "character_index");
			request.after = table_bool_field(L, -1, "after");
			request.has_selection = table_bool_field(L, -1, "has_selection");
			request.selection_start = table_int_field(L, -1, "selection_start");
			request.selection_stop = table_int_field(L, -1, "selection_stop");
			request.requested = request.focus || request.has_cursor || request.has_selection;
		}
		lua_pop(L, 1);
		clear_pending_edit_box_request(L);
		return request;
	}

	void queue_pending_edit_box_focus(lua_State *L)
	{
		ensure_edit_box_request_table(L);
		set_field(L, "focus", true);
		lua_pop(L, 1);
	}

	void queue_pending_edit_box_cursor(lua_State *L, int character_index, bool after)
	{
		ensure_edit_box_request_table(L);
		set_field(L, "focus", true);
		set_field(L, "has_cursor", true);
		set_field(L, "character_index", character_index);
		set_field(L, "after", after);
		lua_pop(L, 1);
	}

	void queue_pending_edit_box_selection(lua_State *L, int start, int stop)
	{
		ensure_edit_box_request_table(L);
		set_field(L, "focus", true);
		set_field(L, "has_selection", true);
		set_field(L, "selection_start", start);
		set_field(L, "selection_stop", stop);
		lua_pop(L, 1);
	}

	void apply_pending_edit_box_request(lua_State *L, AutomationHost *host)
	{
		auto request = take_pending_edit_box_request(L);
		if (!request.requested || !host)
			return;

		if (request.has_selection)
			host->Ui().SetSubtitleEditBoxSelection(request.selection_start, request.selection_stop);
		else if (request.has_cursor)
			host->Ui().SetSubtitleEditBoxCursor(request.character_index, request.after);
		else if (request.focus)
			host->Ui().FocusSubtitleEditBox();
	}

	bool can_queue_edit_box_request(lua_State *L, AutomationHost *host)
	{
		auto runtime_state = LuaGetAutomationRuntimeStateSnapshot(L);
		bool can_focus_edit_box = host && host->Ui().CanFocusSubtitleEditBox();
		return CanQueueSubtitleEditBoxRequest(runtime_state, can_focus_edit_box);
	}

	int get_file_name(lua_State *L)
	{
		if (auto *host = get_host(L)) {
			if (auto filename = host->TryGetFileName()) {
				push_value(L, *filename);
				return 1;
			}
		}
		lua_pushnil(L);
		return 1;
	}

	int get_translation(lua_State *L)
	{
		wxString str(check_wxstring(L, 1));
		if (wxTranslations::Get())
			push_value(L, wxGetTranslation(str).utf8_str());
		else
			push_value(L, str.utf8_str());
		return 1;
	}

	char *clipboard_get()
	{
		std::string data;
		agi::dispatch::Main().Sync([&] { data = GetClipboard(); });
		if (data.empty())
			return nullptr;
		return strndup(data);
	}

	bool clipboard_set(const char *str)
	{
		bool succeeded = false;

		agi::dispatch::Main().Sync([&] {
			wxClipboard &cb = *wxTheClipboard;
			if (cb.Open()) {
				succeeded = cb.SetData(new wxTextDataObject(wxString::FromUTF8(str)));
				cb.Close();
				cb.Flush();
			}
		});

		return succeeded;
	}

	int clipboard_init(lua_State *L)
	{
		agi::lua::register_lib_table(L, {}, "get", clipboard_get, "set", clipboard_set);
		return 1;
	}

	int frame_from_ms(lua_State *L)
	{
		int ms = lua_tointeger(L, -1);
		lua_pop(L, 1);
		if (auto *host = get_host(L)) {
			if (auto frame = host->Media().FrameFromMs(ms)) {
				push_value(L, *frame);
				return 1;
			}
		}
		lua_pushnil(L);

		return 1;
	}

	int ms_from_frame(lua_State *L)
	{
		int frame = lua_tointeger(L, -1);
		lua_pop(L, 1);
		if (auto *host = get_host(L)) {
			if (auto ms = host->Media().MsFromFrame(frame)) {
				push_value(L, *ms);
				return 1;
			}
		}
		lua_pushnil(L);
		return 1;
	}

	int video_size(lua_State *L)
	{
		if (auto *host = get_host(L)) {
			if (auto video = host->Media().TryGetVideoInfo()) {
				push_value(L, video->width);
				push_value(L, video->height);
				push_value(L, video->aspect_ratio);
				push_value(L, video->aspect_ratio_type);
				return 4;
			}
		}
		lua_pushnil(L);
		return 1;
	}

	int get_keyframes(lua_State *L)
	{
		if (auto *host = get_host(L)) {
			push_value(L, host->Media().GetKeyframes());
		}
		else
			lua_pushnil(L);
		return 1;
	}

	int decode_path(lua_State *L)
	{
		std::string path = check_string(L, 1);
		lua_pop(L, 1);
		if (auto *host = get_host(L))
			push_value(L, host->DecodePath(path));
		else
			push_value(L, config::path->Decode(path));
		return 1;
	}

	int cancel_script(lua_State *L)
	{
		lua_pushnil(L);
		throw error_tag();
	}

	int observed_stack_trace(lua_State *L) {
		auto *cancelled = static_cast<bool *>(lua_touserdata(L, lua_upvalueindex(1)));
		if (lua_isnil(L, 1)) {
			lua_Debug frame;
			for (int level = 1; lua_getstack(L, level, &frame); ++level) {
				lua_getinfo(L, "f", &frame);
				bool matches = lua_tocfunction(L, -1) == exception_wrapper<cancel_script>;
				lua_pop(L, 1);
				if (matches) {
					*cancelled = true;
					break;
				}
			}
		}
		return add_stack_trace(L);
	}

	int lua_set_debug_template_context(lua_State *L)
	{
		LuaSetAutomationTemplateDebugContext(L, lua_gettop(L) >= 1 ? 1 : 0);
		return 0;
	}

	int lua_is_debug_template_enabled(lua_State *L)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, kTemplateDebugEnabledRegistryKey);
		bool enabled = !!lua_toboolean(L, -1);
		lua_pop(L, 1);
		push_value(L, enabled);
		return 1;
	}

	int lua_text_textents(lua_State *L)
	{
		argcheck(L, !!lua_istable(L, 1), 1, "");
		argcheck(L, !!lua_isstring(L, 2), 2, "");

		// have to check that it looks like a style table before actually converting
		// if it's a dialogue table then an active AssFile object is required
		{
			lua_getfield(L, 1, "class");
			std::string actual_class{lua_tostring(L, -1)};
			agi::util::strings::to_lower_inplace(actual_class);
			if (actual_class != "style")
				return error(L, "Not a style entry");
			lua_pop(L, 1);
		}

		lua_pushvalue(L, 1);
		std::unique_ptr<AssEntry> et(Automation4::LuaAssFile::LuaToAssEntry(L));
		lua_pop(L, 1);
		if (typeid(*et) != typeid(AssStyle))
			return error(L, "Not a style entry");

		double width, height, descent, extlead;
		if (!Automation4::CalculateTextExtents(static_cast<AssStyle*>(et.get()),
				check_string(L, 2), width, height, descent, extlead))
			return error(L, "Some internal error occurred calculating text_extents");

		push_value(L, width);
		push_value(L, height);
		push_value(L, descent);
		push_value(L, extlead);
		return 4;
	}

	int lua_get_audio_selection(lua_State *L)
	{
		if (auto *host = get_host(L)) {
			if (auto audio_selection = host->Media().TryGetAudioSelection()) {
				push_value(L, audio_selection->begin);
				push_value(L, audio_selection->end);
				return 2;
			}
		}
		lua_pushnil(L);
		return 1;
	}

	int lua_get_visual_guides(lua_State *L)
	{
		AutomationVisualGuideSnapshot snapshot;
		if (auto *host = get_host(L)) {
			if (auto guides = host->Ui().TryGetVisualGuides())
				snapshot = std::move(*guides);
		}

		LuaPushVisualGuideSnapshot(L, snapshot);
		return 1;
	}

	int lua_scroll_audio_to(lua_State *L)
	{
		int time_ms = check_int(L, 1);
		auto *host = get_host(L);
		push_value(L, host && host->Ui().ScrollAudioToTime(time_ms));
		return 1;
	}

	int lua_set_status_text(lua_State *L)
	{
		auto *host = get_host(L);
		if (!host) {
			lua_pushnil(L);
			return 1;
		}
		std::string text = check_string(L, 1);
		lua_pop(L, 1);
		host->Ui().ShowStatus(text);
		return 0;
	}

	int lua_focus_edit_box(lua_State *L)
	{
		auto *host = get_host(L);
		bool available = can_queue_edit_box_request(L, host);
		if (available)
			queue_pending_edit_box_focus(L);
		push_value(L, available);
		return 1;
	}

	int lua_get_edit_box_cursor(lua_State *L)
	{
		if (auto *host = get_host(L)) {
			if (auto cursor = host->Ui().TryGetSubtitleEditBoxCursor()) {
				push_value(L, cursor->start);
				push_value(L, cursor->stop);
				return 2;
			}
		}

		lua_pushnil(L);
		return 1;
	}

	int lua_set_edit_box_cursor(lua_State *L)
	{
		auto *host = get_host(L);
		bool available = can_queue_edit_box_request(L, host);

		if (available) {
			int top = lua_gettop(L);
			if (top >= 2 && lua_isboolean(L, 2)) {
				// (character_index, after) — legacy cursor mode
				int character_index = check_int(L, 1);
				bool after = !!lua_toboolean(L, 2);
				queue_pending_edit_box_cursor(L, character_index, after);
			}
			else if (top >= 2) {
				// (start, stop) — selection mode
				int start = check_int(L, 1);
				int stop = check_int(L, 2);
				queue_pending_edit_box_selection(L, start, stop);
			}
			else {
				// (character_index) — cursor at offset, after=false
				int character_index = check_int(L, 1);
				queue_pending_edit_box_cursor(L, character_index, false);
			}
		}

		push_value(L, available);
		return 1;
	}

	int project_properties(lua_State *L)
	{
		auto *host = get_host(L);
		if (!host)
			lua_pushnil(L);
		else if (auto project = host->TryGetProjectProperties()) {
			lua_createtable(L, 0, 14);
#define PUSH_FIELD(name) set_field(L, #name, project->name)
			PUSH_FIELD(automation_scripts);
			PUSH_FIELD(export_filters);
			PUSH_FIELD(export_encoding);
			PUSH_FIELD(style_storage);
			PUSH_FIELD(video_zoom);
			PUSH_FIELD(ar_value);
			PUSH_FIELD(scroll_position);
			PUSH_FIELD(active_row);
			PUSH_FIELD(ar_mode);
			PUSH_FIELD(video_position);
#undef PUSH_FIELD
			set_field(L, "audio_file", project->audio_file);
			set_field(L, "video_file", project->video_file);
			set_field(L, "timecodes_file", project->timecodes_file);
			set_field(L, "keyframes_file", project->keyframes_file);
		}
		else
			lua_pushnil(L);
		return 1;
	}

	class LuaFeature {
		int myid = 0;
	protected:
		lua_State *L;

		void RegisterFeature();
		void UnregisterFeature();

		void GetFeatureFunction(const char *function) const;

		LuaFeature(lua_State *L) : L(L) { }
	};

	/// Run a lua function on a background thread
	/// @param L Lua state
	/// @param nargs Number of arguments the function takes
	/// @param nresults Number of values the function returns
	/// @param bsr Background script runner to use for the progress dialog
	/// @param invocation Structured invocation metadata for the current feature call.
	/// @throws agi::UserCancelException if the function fails to run to completion (either due to cancelling or errors)
	void LuaThreadedCall(lua_State *L, int nargs, int nresults, BackgroundScriptRunner& bsr, AutomationInvocation const& invocation, AutomationInvocationOutcome *outcome = nullptr);

	class LuaCommand final : public cmd::Command, private LuaFeature {
		std::string cmd_name;
		wxString display;
		wxString help;
		int cmd_type;

	public:
		LuaCommand(lua_State *L);
		~LuaCommand();

		const char* name() const override { return cmd_name.c_str(); }
		wxString StrMenu(const agi::Context *) const override { return display; }
		wxString StrDisplay(const agi::Context *) const override { return display; }
		wxString StrHelp() const override { return help; }

		int Type() const override { return cmd_type; }

		void operator()(agi::Context *c) override;
		bool Validate(const agi::Context *c) override;
		virtual bool IsActive(const agi::Context *c) override;

		static int LuaRegister(lua_State *L);
	};

	class LuaExportFilter final : public ExportFilter, private LuaFeature {
		bool has_config;
		LuaDialog *config_dialog;

	protected:
		std::unique_ptr<ScriptDialog> GenerateConfigDialog(wxWindow *parent, agi::Context *c) override;

	public:
		LuaExportFilter(lua_State *L);
		static int LuaRegister(lua_State *L);

		void ProcessSubs(AssFile *subs, wxWindow *export_dialog) override;
	};
	class LuaScript final : public Script {
		lua_State *L = nullptr;
		std::shared_ptr<AutomationHost> automation_host;
		void const* automation_host_identity = nullptr;
		AutomationRuntimeTraceSink *runtime_trace_sink = nullptr;
		AutomationDebugSession *debug_session = nullptr;
		std::unique_ptr<AutomationLuaDebugBackend> debug_backend;

		std::string name;
		std::string description;
		std::string author;
		std::string version;

		std::vector<cmd::Command*> macros;
		std::vector<std::unique_ptr<LuaCommand>> pending_macros;
		std::vector<std::unique_ptr<ExportFilter>> filters;
		std::vector<LuaExportFilter*> pending_filters;

		/// load script and create internal structures etc.
		void Create();
		/// destroy internal structures, unreg features and delete environment
		void Destroy();

		static int LuaInclude(lua_State *L);

	public:
		LuaScript(agi::fs::path const& filename);
		~LuaScript() { Destroy(); }

		void RegisterCommand(LuaCommand *command);
		void UnregisterCommand(LuaCommand *command);
		void RegisterFilter(LuaExportFilter *filter);
		void QueueCommand(std::unique_ptr<LuaCommand> command);
		void QueueFilter(std::unique_ptr<LuaExportFilter> filter);

		static LuaScript* GetScriptObject(lua_State *L);
		std::shared_ptr<AutomationHost> GetAutomationHost() const { return automation_host; }
		[[nodiscard]] AutomationRuntimeTraceSink *GetRuntimeTraceSink() const { return runtime_trace_sink; }
		bool MatchesAutomationHostContext(agi::Context const* context) const
		{
			return automation_host && automation_host_identity == context;
		}
		AutomationDebugBackend *GetDebugBackend() const { return debug_backend.get(); }
		AutomationDebugSession *GetDebugSession() const { return debug_session; }

		// Script implementation
		void Reload() override { Create(); }

		std::string GetName() const override { return name; }
		std::string GetDescription() const override { return description; }
		std::string GetAuthor() const override { return author; }
		std::string GetVersion() const override { return version; }
		bool GetLoadedState() const override { return L != nullptr; }

		std::vector<cmd::Command*> GetMacros() const override { return macros; }
		std::vector<ExportFilter*> GetFilters() const override;
		void CommitPendingFeatures() override;
		std::string GetEngineName() const override { return "Lua"; }
		std::optional<AutomationRuntimeStateSnapshot> TryGetRuntimeStateSnapshot() const override;
		void SetRuntimeTraceSink(AutomationRuntimeTraceSink *sink) override;
		void SetAutomationHost(std::shared_ptr<AutomationHost> host) override;
		void SetAutomationHostForContext(std::shared_ptr<AutomationHost> host, agi::Context const* context);
		void SetDebugSession(AutomationDebugSession *session) override;

	private:
		void UpdateTemplateDebugEnabledFlag();
	};

	class ScopedInvocationObservation final : public AutomationRuntimeTraceSink {
		LuaScript *script;
		AutomationRuntimeTraceSink *previous;
		std::shared_ptr<AutomationInvocationObserver> observer;
		AutomationInvocationOutcome outcome = AutomationInvocationOutcome::Failed;
		bool template_failed = false;
		bool observer_failed = false;

		public:
		ScopedInvocationObservation(LuaScript *script, std::shared_ptr<AutomationInvocationObserver> observer)
			: script(script), previous(script->GetRuntimeTraceSink()), observer(std::move(observer)) {
			if (this->observer)
				script->SetRuntimeTraceSink(this);
		}

		~ScopedInvocationObservation() override {
			if (!observer)
				return;
			script->SetRuntimeTraceSink(previous);
			if (outcome == AutomationInvocationOutcome::Cancelled && template_failed)
				outcome = AutomationInvocationOutcome::Failed;
			try {
				observer->OnInvocationFinished(outcome);
			}
			catch (...) {
				LOG_E("automation/observer") << "Invocation observer failed while publishing its final state";
			}
		}

		void SetOutcome(AutomationInvocationOutcome value) { outcome = value; }

		void OnRuntimeStateSnapshot(AutomationRuntimeStateSnapshot const& snapshot) override {
			if (previous)
				previous->OnRuntimeStateSnapshot(snapshot);
			if (snapshot.invocation.kind != AutomationInvocationKind::MacroRun)
				return;
			if (snapshot.template_debug) {
				auto const& state = *snapshot.template_debug;
				template_failed = state.parse_error.has_value() || state.runtime_error.has_value() || state.kind == "code-parse" || state.kind == "code-error" || state.kind == "expression-parse" || state.kind == "expression-error";
			}
			if (observer_failed)
				return;
			try {
				observer->OnRuntimeStateSnapshot(snapshot);
			}
			catch (...) {
				observer_failed = true;
				LOG_E("automation/observer") << "Invocation observer failed while receiving a runtime snapshot";
			}
		}
	};

	LuaScript::LuaScript(agi::fs::path const& filename)
	: Script(filename)
	{
		Create();
	}

	void LuaScript::Create()
	{
		Destroy();

		name = agi::fs::PathToString(GetPrettyFilename());

		// create lua environment
		L = luaL_newstate();
		if (!L) {
			description = "Could not initialize Lua state";
			return;
		}
		debug_backend = agi::make_unique<AutomationLuaDebugBackend>(L, GetFilename());
		if (debug_session)
			debug_backend->SetSession(debug_session);

		bool loaded = false;
		auto cleanup_on_failure = agi::make_scope_exit([&] {
			if (!loaded)
				Destroy();
		});
		LuaStackcheck stackcheck(L);

		// register standard libs
		preload_modules(L);
		stackcheck.check_stack(0);

		// dofile and loadfile are replaced with include
		lua_pushnil(L);
		lua_setglobal(L, "dofile");
		lua_pushnil(L);
		lua_setglobal(L, "loadfile");
		push_value(L, exception_wrapper<LuaInclude>);
		lua_setglobal(L, "include");

		// Replace the default lua module loader with our unicode compatible
		// one and set the module search path
		if (!Install(L, include_path)) {
			description = get_string_or_default(L, 1);
			lua_pop(L, 1);
			return;
		}
		stackcheck.check_stack(0);

		// prepare stuff in the registry

		// store the script's filename
		push_value(L, GetFilename().stem());
		lua_setfield(L, LUA_REGISTRYINDEX, "filename");
		stackcheck.check_stack(0);

		// reference to the script object
		push_value(L, this);
		lua_setfield(L, LUA_REGISTRYINDEX, "aegisub");
		stackcheck.check_stack(0);

		if (automation_host)
			LuaSetAutomationHost(L, automation_host);
		if (runtime_trace_sink)
			LuaSetAutomationRuntimeTraceSink(L, runtime_trace_sink);
		UpdateTemplateDebugEnabledFlag();

		// make "aegisub" table
		lua_pushstring(L, "aegisub");
		lua_createtable(L, 0, 23);

		set_field<LuaCommand::LuaRegister>(L, "register_macro");
		set_field<LuaExportFilter::LuaRegister>(L, "register_filter");
		set_field<lua_text_textents>(L, "text_extents");
		set_field<frame_from_ms>(L, "frame_from_ms");
		set_field<ms_from_frame>(L, "ms_from_frame");
		set_field<video_size>(L, "video_size");
		set_field<get_keyframes>(L, "keyframes");
		set_field<decode_path>(L, "decode_path");
		set_field<cancel_script>(L, "cancel");
		set_field<lua_set_debug_template_context>(L, "__set_debug_template_context");
		set_field<lua_is_debug_template_enabled>(L, "__is_debug_template_enabled");
		set_field(L, "lua_automation_version", 4);
		set_field<clipboard_init>(L, "__init_clipboard");
		set_field<get_file_name>(L, "file_name");
		set_field<get_translation>(L, "gettext");
		set_field<project_properties>(L, "project_properties");
		set_field<lua_get_audio_selection>(L, "get_audio_selection");
		set_field<lua_scroll_audio_to>(L, "scroll_audio_to");
		set_field<lua_get_visual_guides>(L, "get_visual_guides");
		set_field<lua_set_status_text>(L, "set_status_text");
		set_field<lua_focus_edit_box>(L, "focus_edit_box");
		set_field<lua_get_edit_box_cursor>(L, "get_edit_box_cursor");
		set_field<lua_set_edit_box_cursor>(L, "set_edit_box_cursor");

		// store aegisub table to globals
		lua_settable(L, LUA_GLOBALSINDEX);
		stackcheck.check_stack(0);
#ifdef WITH_PLUGIN_BRIDGE
		RegisterPluginLuaApi(L);
		stackcheck.check_stack(0);
#endif
		debug_backend->CaptureRuntimeBaseline();

		// load user script
		if (!LoadFile(L, GetFilename())) {
			description = get_string_or_default(L, 1);
			lua_pop(L, 1);
			return;
		}
		stackcheck.check_stack(1);

		// Insert our error handler under the user's script
		lua_pushcclosure(L, add_stack_trace, 0);
		lua_insert(L, -2);

		// and execute it
		// this is where features are registered
		if (lua_pcall(L, 0, 0, -2)) {
			// error occurred, assumed to be on top of Lua stack
			description = agi::format("Error initialising Lua script \"%s\":\n\n%s", agi::fs::PathToString(GetPrettyFilename()), get_string_or_default(L, -1));
			lua_pop(L, 2); // error + error handler
			return;
		}
		lua_pop(L, 1); // error handler
		stackcheck.check_stack(0);

		lua_getglobal(L, "version");
		if (lua_isnumber(L, -1) && lua_tointeger(L, -1) == 3) {
			lua_pop(L, 1); // just to avoid tripping the stackcheck in debug
			description = "Attempted to load an Automation 3 script as an Automation 4 Lua script. Automation 3 is no longer supported.";
			return;
		}

		name = get_global_string(L, "script_name");
		description = get_global_string(L, "script_description");
		author = get_global_string(L, "script_author");
		version = get_global_string(L, "script_version");

		if (name.empty())
			name = agi::fs::PathToString(GetPrettyFilename());

		lua_pop(L, 1);
		// if we got this far, the script should be ready
		loaded = true;
	}

	void LuaScript::Destroy()
	{
		// Assume the script object is clean if there's no Lua state
		if (!L) return;
		debug_backend.reset();

		pending_macros.clear();
		pending_filters.clear();

		// loops backwards because commands remove themselves from macros when
		// they're unregistered
		for (int i = macros.size() - 1; i >= 0; --i)
			if (cmd::get_if(macros[i]->name()) == macros[i])
				cmd::unreg(macros[i]->name());

		filters.clear();

		lua_close(L);
		L = nullptr;
	}

	std::vector<ExportFilter*> LuaScript::GetFilters() const
	{
		std::vector<ExportFilter *> ret;
		ret.reserve(filters.size());
		for (auto& filter : filters) ret.push_back(filter.get());
		return ret;
	}

	std::optional<AutomationRuntimeStateSnapshot> LuaScript::TryGetRuntimeStateSnapshot() const
	{
		if (!L)
			return std::nullopt;
		return LuaGetAutomationRuntimeStateSnapshot(L);
	}

	void LuaScript::UpdateTemplateDebugEnabledFlag()
	{
		if (!L)
			return;
		push_value(L, debug_session || runtime_trace_sink);
		lua_setfield(L, LUA_REGISTRYINDEX, kTemplateDebugEnabledRegistryKey);
	}

	void LuaScript::SetRuntimeTraceSink(AutomationRuntimeTraceSink *sink)
	{
		runtime_trace_sink = sink;
		if (L) {
			LuaSetAutomationRuntimeTraceSink(L, sink);
			UpdateTemplateDebugEnabledFlag();
		}
	}

	void LuaScript::SetAutomationHost(std::shared_ptr<AutomationHost> host)
	{
		SetAutomationHostForContext(std::move(host), nullptr);
	}

	void LuaScript::SetAutomationHostForContext(std::shared_ptr<AutomationHost> host, agi::Context const* context)
	{
		automation_host = std::move(host);
		automation_host_identity = automation_host
			? (context ? static_cast<void const*>(context) : automation_host->ProjectContextIdentity())
			: nullptr;
		if (L)
			LuaSetAutomationHost(L, automation_host);
	}

	void LuaScript::SetDebugSession(AutomationDebugSession *session)
	{
		debug_session = session;
		UpdateTemplateDebugEnabledFlag();
		if (debug_backend)
			debug_backend->SetSession(session);
	}

	void LuaScript::RegisterCommand(LuaCommand *command)
	{
		for (auto macro : macros) {
			if (macro->name() == command->name()) {
				error(L, "A macro named '%s' is already defined in script '%s'",
					command->StrDisplay(nullptr).utf8_str().data(), name.c_str());
			}
		}
		macros.push_back(command);
	}

	void LuaScript::UnregisterCommand(LuaCommand *command)
	{
		macros.erase(remove(macros.begin(), macros.end(), command), macros.end());
	}

	void LuaScript::RegisterFilter(LuaExportFilter *filter)
	{
		filters.emplace_back(filter);
	}

	void LuaScript::QueueCommand(std::unique_ptr<LuaCommand> command)
	{
		RegisterCommand(command.get());
		pending_macros.emplace_back(std::move(command));
	}

	void LuaScript::QueueFilter(std::unique_ptr<LuaExportFilter> filter)
	{
		pending_filters.push_back(filter.get());
		filters.emplace_back(std::move(filter));
	}

	void LuaScript::CommitPendingFeatures()
	{
		if (pending_macros.empty() && pending_filters.empty())
			return;

		std::unordered_set<std::string> committed_names;
		for (auto *macro : macros) {
			if (macro && find_if(pending_macros.begin(), pending_macros.end(), [&](std::unique_ptr<LuaCommand> const& pending) { return pending.get() == macro; }) == pending_macros.end())
				committed_names.insert(macro->name());
		}

		for (auto it = pending_macros.begin(); it != pending_macros.end(); ) {
			auto& macro = *it;
			auto const name = std::string(macro->name());
			if (committed_names.count(name) || cmd::get_if(name)) {
				wxLogWarning(wxS("Skipping Automation macro '%s' from script '%s' because command '%s' is already registered."),
					macro->StrDisplay(nullptr), GetFilename().wstring(), to_wx(name));
				macros.erase(remove(macros.begin(), macros.end(), macro.get()), macros.end());
				it = pending_macros.erase(it);
				continue;
			}

			committed_names.insert(name);
			cmd::reg(std::move(macro));
			it = pending_macros.erase(it);
		}

		for (auto *filter : pending_filters)
			AssExportFilterChain::Register(filter);
		pending_filters.clear();
	}

	LuaScript* LuaScript::GetScriptObject(lua_State *L)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, "aegisub");
		void *ptr = lua_touserdata(L, -1);
		lua_pop(L, 1);
		return (LuaScript*)ptr;
	}

	std::shared_ptr<AutomationHost> EnsureLuaScriptHost(lua_State *L, agi::Context const* context)
	{
		auto *script = LuaScript::GetScriptObject(L);
		auto host = script->GetAutomationHost();
		if (context && !script->MatchesAutomationHostContext(context)) {
			host = CreateAutomationLiveHost(context);
			script->SetAutomationHostForContext(host, context);
		}
		return host;
	}

	std::shared_ptr<AutomationDebugSession> PrepareLuaDebugSession(lua_State *L, AutomationInvocation const& invocation)
	{
		auto *script = LuaScript::GetScriptObject(L);
		if (!script)
			return {};

		if (script->GetDebugSession())
			return {};

		if (!config::automation_debug_service || !config::automation_debug_service->IsEnabled())
			return {};

		auto session = config::automation_debug_service->PrepareSession({
			script->GetEngineName(),
			script->GetFilename(),
			invocation.feature_name
		});
		script->SetDebugSession(session.get());
		return session;
	}

	void FinalizeLuaDebugSession(lua_State *L, std::shared_ptr<AutomationDebugSession> const& session)
	{
		if (!session)
			return;
		if (auto *script = LuaScript::GetScriptObject(L))
			script->SetDebugSession(nullptr);
		if (config::automation_debug_service)
			config::automation_debug_service->ClearSession(session);
	}


	int LuaScript::LuaInclude(lua_State *L)
	{
		const LuaScript *s = GetScriptObject(L);

		const std::string filename(check_string(L, 1));
		agi::fs::path filepath;

		// Relative or absolute path
		if (std::any_of(filename.begin(), filename.end(), [](char c) { return c == '/' || c == '\\'; }))
			filepath = s->GetFilename().parent_path()/filename;
		else { // Plain filename
			for (auto const& dir : s->include_path) {
				filepath = dir/filename;
				if (agi::fs::FileExists(filepath))
					break;
			}

			if (!agi::fs::FileExists(filepath)) {
				for (auto probe = s->GetFilename().parent_path(); !probe.empty();) {
					auto candidate = probe / "include" / filename;
					if (agi::fs::FileExists(candidate)) {
						filepath = std::move(candidate);
						break;
					}
					auto parent = probe.parent_path();
					if (parent == probe)
						break;
					probe = std::move(parent);
				}
			}
		}

		if (!agi::fs::FileExists(filepath))
			return error(L, "Lua include not found: %s", filename.c_str());

		if (!LoadFile(L, filepath))
			return error(L, "Error loading Lua include \"%s\":\n%s", filename.c_str(), check_string(L, 1).c_str());

		int pretop = lua_gettop(L) - 1; // don't count the function value itself
		lua_call(L, 0, LUA_MULTRET);
		return lua_gettop(L) - pretop;
	}

	void LuaThreadedCall(lua_State *L, int nargs, int nresults, BackgroundScriptRunner& bsr, AutomationInvocation const& invocation, AutomationInvocationOutcome *outcome) {
		bool failed = false;
		bool failure_recorded = false;
		auto result = AutomationInvocationOutcome::Failed;
		std::exception_ptr worker_exception;
		int stack_base = lua_gettop(L) - nargs - 1;
		auto publish_outcome = agi::make_scope_exit([&] {
			if (outcome)
				*outcome = result;
		});
		try {
			bsr.Run([&](ProgressSink *ps) {
				try {
					LuaProgressSink lps(L, ps, invocation);
					ScopedAutomationDebugInvocation debug_invocation(
						LuaScript::GetScriptObject(L)->GetDebugBackend(),
						invocation);
					bool cancelled = false;
					if (outcome) {
						lua_pushlightuserdata(L, &cancelled);
						lua_pushcclosure(L, observed_stack_trace, 1);
					}
					else
						lua_pushcclosure(L, add_stack_trace, 0);
					lua_insert(L, -nargs - 2);

					int status = lua_pcall(L, nargs, nresults, -nargs - 2);
					if (status) {
						failed = true;
						result = status == LUA_ERRRUN && cancelled ? AutomationInvocationOutcome::Cancelled : AutomationInvocationOutcome::Failed;
						failure_recorded = result == AutomationInvocationOutcome::Failed;
						if (!lua_isnil(L, -1)) {
							ps->Log("\n\nLua reported a runtime error:\n");
							ps->Log(get_string_or_default(L, -1));
						}
						else if (outcome && !cancelled)
							ps->Log("\n\nLua reported a runtime error with a nil error value.\n");
						lua_pop(L, 2);
					}
					else {
						result = AutomationInvocationOutcome::Completed;
						lua_remove(L, -nresults - 1);
					}

					if (outcome) {
						auto *sink = LuaScript::GetScriptObject(L)->GetRuntimeTraceSink();
						auto snapshot = LuaGetAutomationRuntimeStateSnapshot(L);
						if (sink && snapshot)
							sink->OnRuntimeStateSnapshot(*snapshot);
					}
					lua_gc(L, LUA_GCCOLLECT, 0);
				}
				catch (agi::UserCancelException const&) {
					if (!outcome)
						throw;
					if (!failure_recorded)
						result = AutomationInvocationOutcome::Cancelled;
					worker_exception = std::current_exception();
					lua_settop(L, stack_base);
				}
				catch (...) {
					if (!outcome)
						throw;
					result = AutomationInvocationOutcome::Failed;
					failure_recorded = true;
					worker_exception = std::current_exception();
					lua_settop(L, stack_base);
				}
			});
		}
		catch (agi::UserCancelException const&) {
			if (!failure_recorded)
				result = AutomationInvocationOutcome::Cancelled;
			if (outcome)
				lua_settop(L, stack_base);
			throw;
		}
		catch (...) {
			result = AutomationInvocationOutcome::Failed;
			if (outcome)
				lua_settop(L, stack_base);
			throw;
		}
		if (worker_exception)
			std::rethrow_exception(worker_exception);
		if (failed)
			throw agi::UserCancelException("Script threw an error");
	}

	// LuaFeature
	void LuaFeature::RegisterFeature()
	{
		myid = luaL_ref(L, LUA_REGISTRYINDEX);
	}

	void LuaFeature::UnregisterFeature()
	{
		luaL_unref(L, LUA_REGISTRYINDEX, myid);
	}

	void LuaFeature::GetFeatureFunction(const char *function) const
	{
		// get this feature's function pointers
		lua_rawgeti(L, LUA_REGISTRYINDEX, myid);
		// get pointer for validation function
		push_value(L, function);
		lua_rawget(L, -2);
		// remove the function table
		lua_remove(L, -2);
		assert(lua_isfunction(L, -1));
	}

	// LuaFeatureMacro
	int LuaCommand::LuaRegister(lua_State *L)
	{
		auto command = agi::make_unique<LuaCommand>(L);
		LuaScript::GetScriptObject(L)->QueueCommand(std::move(command));
		return 0;
	}

	LuaCommand::LuaCommand(lua_State *L)
	: LuaFeature(L)
	, display(check_wxstring(L, 1))
	, help(get_wxstring(L, 2))
	, cmd_type(cmd::COMMAND_NORMAL)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, "filename");
		cmd_name = agi::format("automation/lua/%s/%s", check_string(L, -1), check_string(L, 1));

		if (!lua_isfunction(L, 3))
			error(L, "The macro processing function must be a function");

		if (lua_isfunction(L, 4))
			cmd_type |= cmd::COMMAND_VALIDATE;

		if (lua_isfunction(L, 5))
			cmd_type |= cmd::COMMAND_TOGGLE;

		// new table for containing the functions for this feature
		lua_createtable(L, 0, 3);

		// store processing function
		push_value(L, "run");
		lua_pushvalue(L, 3);
		lua_rawset(L, -3);

		// store validation function
		push_value(L, "validate");
		lua_pushvalue(L, 4);
		lua_rawset(L, -3);

		// store active function
		push_value(L, "isactive");
		lua_pushvalue(L, 5);
		lua_rawset(L, -3);

		// store the table in the registry
		RegisterFeature();

	}

	LuaCommand::~LuaCommand()
	{
		UnregisterFeature();
		LuaScript::GetScriptObject(L)->UnregisterCommand(this);
	}

	static std::vector<int> selected_rows(const agi::Context *c)
	{
		auto core = c->GetCore();
		auto const& sel = core.selectionController->GetSelectedSet();
		int offset = core.ass->Info.size() + core.ass->Styles.size();
		std::vector<int> rows;
		rows.reserve(sel.size());
		for (auto line : sel)
			rows.push_back(line->Row + offset + 1);
		sort(begin(rows), end(rows));
		return rows;
	}

	bool LuaCommand::Validate(const agi::Context *c)
	{
		if (!(cmd_type & cmd::COMMAND_VALIDATE)) return true;
		auto core = c->GetCore();
		auto invocation = MakeMacroValidateInvocation(cmd_name);
		auto rows = selected_rows(c);
		int active_row = 0;
		if (auto active_line = core.selectionController->GetActiveLine())
			active_row = active_line->Row + core.ass->Info.size() + core.ass->Styles.size() + 1;

		auto host = EnsureLuaScriptHost(L, c);
		auto debug_session = PrepareLuaDebugSession(L, invocation);
		auto clear_debug_session = agi::make_scope_exit([&] {
			FinalizeLuaDebugSession(L, debug_session);
		});
		LuaSetAutomationRuntimeState(L, host, invocation, rows, active_row);

		// Error handler goes under the function to call
		lua_pushcclosure(L, add_stack_trace, 0);

		GetFeatureFunction("validate");
		auto subsobj = LuaAssFile::Create(
			L, core.ass.get(),
			invocation.capabilities.allow_modify,
			invocation.capabilities.allow_undo);

		push_value(L, rows);
		if (active_row)
			push_value(L, active_row);
		else
			lua_pushnil(L);

		int err = lua_pcall(L, 3, 2, -5 /* three args, function, error handler */);
		subsobj->ProcessingComplete();

		if (err) {
			wxLogWarning(wxS("Runtime error in Lua macro validation function:\n%s"), get_wxstring(L, -1));
			lua_pop(L, 2);
			return false;
		}

		bool result = !!lua_toboolean(L, -2);

		wxString new_help_string(get_wxstring(L, -1));
		if (new_help_string.size()) {
			help = new_help_string;
			cmd_type |= cmd::COMMAND_DYNAMIC_HELP;
		}

		lua_pop(L, 3); // two return values and error handler

		return result;
	}

	void LuaCommand::operator()(agi::Context *c)
	{
		LuaStackcheck stackcheck(L);
		auto core = c->GetCore();
		auto invocation = MakeMacroRunInvocation(cmd_name);
		int original_offset = core.ass->Info.size() + core.ass->Styles.size() + 1;
		auto original_sel = selected_rows(c);
		int original_active = 0;
		if (auto active_line = core.selectionController->GetActiveLine())
			original_active = active_line->Row + original_offset;
		auto host = EnsureLuaScriptHost(L, c);
		auto observer = host ? host->Ui().BeginInvocationObservation(invocation) : nullptr;
		ScopedInvocationObservation observation(LuaScript::GetScriptObject(L), observer);
		auto debug_session = PrepareLuaDebugSession(L, invocation);
		auto clear_debug_session = agi::make_scope_exit([&] {
			FinalizeLuaDebugSession(L, debug_session);
		});
		LuaSetAutomationRuntimeState(L, host, invocation, original_sel, original_active);
		clear_pending_edit_box_request(L);
		stackcheck.check_stack(0);

		GetFeatureFunction("run");
		auto subsobj = LuaAssFile::Create(
			L, core.ass.get(),
			invocation.capabilities.allow_modify,
			invocation.capabilities.allow_undo);

		push_value(L, original_sel);
		push_value(L, original_active);

		auto runner = host ? host->Ui().CreateBackgroundScriptRunner(from_wx(StrDisplay(c))) : std::unique_ptr<BackgroundScriptRunner>{};
		if (!runner)
			throw AutomationError("Automation background runner unavailable");

		auto call_outcome = AutomationInvocationOutcome::Failed;
		try {
			LuaThreadedCall(L, 3, 2, *runner, invocation, observer ? &call_outcome : nullptr);
		}
		catch (agi::UserCancelException const&) {
			observation.SetOutcome(call_outcome);
			subsobj->Cancel();
			clear_pending_edit_box_request(L);
			stackcheck.check_stack(0);
			return;
		}
		catch (...) {
			subsobj->Cancel();
			clear_pending_edit_box_request(L);
			throw;
		}

		auto lines = subsobj->ProcessingComplete(StrDisplay(c));

		AssDialogue *active_line = nullptr;
		int active_idx = original_active;

		// Check for a new active row
		if (lua_isnumber(L, -1)) {
			active_idx = lua_tointeger(L, -1);
			if (active_idx < 1 || active_idx > (int)lines.size()) {
				wxLogError(wxS("Active row %d is out of bounds (must be 1-%u)"), active_idx, lines.size());
				active_idx = original_active;
			}
		}

		stackcheck.check_stack(2);
		lua_pop(L, 1);

		// top of stack will be selected lines array, if any was returned
		if (lua_istable(L, -1)) {
			std::set<AssDialogue*> sel;
			lua_for_each(L, [&] {
				if (!lua_isnumber(L, -1))
					return;
				int cur = lua_tointeger(L, -1);
				if (cur < 1 || cur > (int)lines.size()) {
					wxLogError(wxS("Selected row %d is out of bounds (must be 1-%u)"), cur, lines.size());
					throw LuaForEachBreak();
				}

				if (typeid(*lines[cur - 1]) != typeid(AssDialogue)) {
					wxLogError(wxS("Selected row %d is not a dialogue line"), cur);
					throw LuaForEachBreak();
				}

				auto diag = static_cast<AssDialogue*>(lines[cur - 1]);
				sel.insert(diag);
				if (!active_line || active_idx == cur)
					active_line = diag;
			});

			AssDialogue *new_active = core.selectionController->GetActiveLine();
			if (active_line && (active_idx > 0 || !sel.count(new_active)))
				new_active = active_line;
			if (sel.empty())
				sel.insert(new_active);
			core.selectionController->SetSelectionAndActive(std::move(sel), new_active);
		}
		else {
			lua_pop(L, 1);

			Selection new_sel;
			AssDialogue *new_active = nullptr;

			int prev = original_offset;
			auto it = core.ass->Events.begin();
			for (int row : original_sel) {
				while (row > prev && it != core.ass->Events.end()) {
					++prev;
					++it;
				}
				if (it == core.ass->Events.end()) break;
				new_sel.insert(&*it);
				if (row == original_active)
					new_active = &*it;
			}

			if (new_sel.empty()) {
				if (!core.ass->Events.empty()) {
					new_active = &core.ass->Events.front();
					new_sel.insert(new_active);
				}
				else
					new_active = nullptr;
			}
			else if (!new_sel.count(new_active))
				new_active = *new_sel.begin();
			core.selectionController->SetSelectionAndActive(std::move(new_sel), new_active);
		}

		apply_pending_edit_box_request(L, host.get());
		stackcheck.check_stack(0);
		observation.SetOutcome(AutomationInvocationOutcome::Completed);
	}

	bool LuaCommand::IsActive(const agi::Context *c)
	{
		if (!(cmd_type & cmd::COMMAND_TOGGLE)) return false;
		auto core = c->GetCore();
		auto invocation = MakeMacroIsActiveInvocation(cmd_name);
		auto rows = selected_rows(c);
		int active_row = 0;
		if (auto active_line = core.selectionController->GetActiveLine())
			active_row = active_line->Row + core.ass->Info.size() + core.ass->Styles.size() + 1;

		LuaStackcheck stackcheck(L);

		auto host = EnsureLuaScriptHost(L, c);
		auto debug_session = PrepareLuaDebugSession(L, invocation);
		auto clear_debug_session = agi::make_scope_exit([&] {
			FinalizeLuaDebugSession(L, debug_session);
		});
		LuaSetAutomationRuntimeState(L, host, invocation, rows, active_row);
		stackcheck.check_stack(0);

		GetFeatureFunction("isactive");
		auto subsobj = LuaAssFile::Create(
			L, core.ass.get(),
			invocation.capabilities.allow_modify,
			invocation.capabilities.allow_undo);
		push_value(L, rows);
		if (active_row)
			push_value(L, active_row);

		int err = lua_pcall(L, 3, 1, 0);
		subsobj->ProcessingComplete();

		bool result = false;
		if (err)
			wxLogWarning(wxS("Runtime error in Lua macro IsActive function:\n%s"), get_wxstring(L, -1));
		else
			result = !!lua_toboolean(L, -1);

		// clean up stack (result or error message)
		stackcheck.check_stack(1);
		lua_pop(L, 1);

		return result;
	}

	// LuaFeatureFilter
	LuaExportFilter::LuaExportFilter(lua_State *L)
	: ExportFilter(check_string(L, 1), lua_tostring(L, 2), lua_tointeger(L, 3))
	, LuaFeature(L)
	{
		if (!lua_isfunction(L, 4))
			error(L, "The filter processing function must be a function");

		// new table for containing the functions for this feature
		lua_createtable(L, 0, 2);

		// store processing function
		push_value(L, "run");
		lua_pushvalue(L, 4);
		lua_rawset(L, -3);

		// store config function
		push_value(L, "config");
		lua_pushvalue(L, 5);
		has_config = lua_isfunction(L, -1);
		lua_rawset(L, -3);

		// store the table in the registry
		RegisterFeature();

	}

	int LuaExportFilter::LuaRegister(lua_State *L)
	{
		auto filter = agi::make_unique<LuaExportFilter>(L);
		LuaScript::GetScriptObject(L)->QueueFilter(std::move(filter));
		return 0;
	}

	void LuaExportFilter::ProcessSubs(AssFile *subs, wxWindow *export_dialog)
	{
		LuaStackcheck stackcheck(L);
		auto invocation = MakeExportFilterRunInvocation(GetName());
		auto debug_session = PrepareLuaDebugSession(L, invocation);
		auto clear_debug_session = agi::make_scope_exit([&] {
			FinalizeLuaDebugSession(L, debug_session);
		});
		auto host = LuaScript::GetScriptObject(L)->GetAutomationHost();
		if (!host) {
			host = GetAutomationHost();
			if (host)
				LuaScript::GetScriptObject(L)->SetAutomationHost(host);
		}
		LuaSetAutomationRuntimeState(L, host, invocation, {}, 0);
		clear_pending_edit_box_request(L);

		GetFeatureFunction("run");
		stackcheck.check_stack(1);

		// The entire point of an export filter is to modify the file, but
		// setting undo points makes no sense
		auto subsobj = LuaAssFile::Create(
			L, subs,
			invocation.capabilities.allow_modify,
			invocation.capabilities.allow_undo);
		assert(lua_isuserdata(L, -1));
		stackcheck.check_stack(2);

		// config
		if (has_config && config_dialog) {
			int results_produced = config_dialog->LuaReadBack(L);
			assert(results_produced == 1);
			(void) results_produced;	// avoid warning on release builds
			// TODO, write back stored options here
		} else {
			// no config so put an empty table instead
			lua_newtable(L);
		}
		assert(lua_istable(L, -1));
		stackcheck.check_stack(3);

		std::unique_ptr<BackgroundScriptRunner> runner;
		if (host)
			runner = host->Ui().CreateBackgroundScriptRunner(GetName(), AutomationUiAnchor{ export_dialog });
		if (!runner) {
			auto file_dialog_service = Automation4::ResolveAutomationFileDialogService(
				host ? host->Ui().GetFileDialogService() : std::shared_ptr<agi::FileDialogService>{},
				export_dialog);
			runner = std::make_unique<BackgroundScriptRunner>(export_dialog, GetName(), std::move(file_dialog_service));
		}
		try {
			LuaThreadedCall(L, 2, 0, *runner, invocation);
			stackcheck.check_stack(0);
			subsobj->ProcessingComplete();
			clear_pending_edit_box_request(L);
		}
		catch (agi::UserCancelException const&) {
			subsobj->Cancel();
			clear_pending_edit_box_request(L);
			throw;
		}
	}

	std::unique_ptr<ScriptDialog> LuaExportFilter::GenerateConfigDialog(wxWindow *parent, agi::Context *c)
	{
		if (!has_config)
			return nullptr;
		auto core = c->GetCore();
		auto invocation = MakeExportFilterConfigInvocation(GetName());
		auto host = EnsureLuaScriptHost(L, c);
		auto debug_session = PrepareLuaDebugSession(L, invocation);
		auto clear_debug_session = agi::make_scope_exit([&] {
			FinalizeLuaDebugSession(L, debug_session);
		});

		LuaSetAutomationRuntimeState(L, host, invocation, {}, 0);

		GetFeatureFunction("config");

		// prepare function call
		auto subsobj = LuaAssFile::Create(
			L, core.ass.get(),
			invocation.capabilities.allow_modify,
			invocation.capabilities.allow_undo);
		// stored options
		lua_newtable(L); // TODO, nothing for now

		// do call
		int err = lua_pcall(L, 2, 1, 0);
		subsobj->ProcessingComplete();

		if (err) {
			wxLogWarning(wxS("Runtime error in Lua config dialog function:\n%s"), get_wxstring(L, -1));
			lua_pop(L, 1); // remove error message
		} else {
			// Create config dialogue from table on top of stack
			config_dialog = new LuaDialog(L, false);
		}

		return std::unique_ptr<ScriptDialog>{config_dialog};
	}
}

namespace Automation4 {
	std::unique_ptr<AutomationScriptInstance> CreateLuaAutomationScriptInstance(agi::fs::path const& filename)
	{
		if (agi::fs::HasExtension(filename, "lua") || agi::fs::HasExtension(filename, "moon"))
			return agi::make_unique<LuaScript>(filename);
		return nullptr;
	}

	std::string LuaAutomationEngine::EngineName() const
	{
		return "Lua";
	}

	std::string LuaAutomationEngine::FilenamePattern() const
	{
		return "*.lua,*.moon";
	}

	bool LuaAutomationEngine::SupportsFile(agi::fs::path const& filename) const
	{
		return agi::fs::HasExtension(filename, "lua") || agi::fs::HasExtension(filename, "moon");
	}

	std::unique_ptr<AutomationScriptInstance> LuaAutomationEngine::LoadScript(agi::fs::path const& filename) const
	{
		return CreateLuaAutomationScriptInstance(filename);
	}
}
