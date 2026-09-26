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

#include "automation_lua_runtime.h"

#include "automation_context_snapshot.h"
#include "automation_host.h"
#include "lua_workspace_run.h"

#include <libaegisub/lua/utils.h>

#include <memory>
#include <optional>

using namespace agi::lua;
using namespace Automation4;

namespace {
	constexpr char kAutomationRuntimeStateRegistryKey[] = "automation_runtime_state";
	constexpr char kAutomationHostRegistryKey[] = "automation_host";
	constexpr char kAutomationHostRegistryMetatable[] = "automation_host_registry_metatable";
	constexpr char kInvocationRegistryKey[] = "automation_invocation";
	constexpr char kContextSnapshotRegistryKey[] = "automation_context_snapshot";
	constexpr char kTemplateDebugContextRegistryKey[] = "automation_template_debug_context";
	constexpr char kRuntimeTraceSinkRegistryKey[] = "automation_runtime_trace_sink";
	constexpr char kWorkspaceRunRegistryKey[] = "automation_workspace_run";
	constexpr char kWorkspaceRunMetatable[] = "automation_workspace_run_metatable";
	char workspace_cancel_token;

	int destroy_workspace_run(lua_State *L) {
		using Request = std::shared_ptr<LuaWorkspaceRunRequest const>;
		static_cast<Request *>(lua_touserdata(L, 1))->~Request();
		return 0;
	}

	int absolute_index(lua_State *L, int index)
	{
		if (index > 0 || index <= LUA_REGISTRYINDEX)
			return index;
		return lua_gettop(L) + index + 1;
	}

	std::optional<std::string> optional_string(lua_State *L, int index)
	{
		if (lua_isnil(L, index) || !lua_isstring(L, index))
			return std::nullopt;
		char const* value = lua_tostring(L, index);
		if (!value)
			return std::nullopt;
		return std::string(value);
	}

	std::optional<int> optional_int(lua_State *L, int index)
	{
		if (lua_isnil(L, index) || !lua_isnumber(L, index))
			return std::nullopt;
		return static_cast<int>(lua_tointeger(L, index));
	}

	std::optional<double> optional_double(lua_State *L, int index)
	{
		if (lua_isnil(L, index) || !lua_isnumber(L, index))
			return std::nullopt;
		return lua_tonumber(L, index);
	}

	std::optional<bool> optional_bool(lua_State *L, int index)
	{
		if (lua_isnil(L, index))
			return std::nullopt;
		return !!lua_toboolean(L, index);
	}

	template<typename T>
	std::optional<T> optional_field(lua_State *L, int table_index, char const* field_name, std::optional<T> (*reader)(lua_State *, int))
	{
		table_index = absolute_index(L, table_index);
		lua_getfield(L, table_index, field_name);
		auto value = reader(L, -1);
		lua_pop(L, 1);
		return value;
	}

	template<typename T, typename Parser>
	std::optional<T> optional_table_field(lua_State *L, int table_index, char const* field_name, Parser&& parser)
	{
		table_index = absolute_index(L, table_index);
		lua_getfield(L, table_index, field_name);
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			return std::nullopt;
		}

		T value = parser(L, -1);
		lua_pop(L, 1);
		return value;
	}

	std::vector<int> int_array_field(lua_State *L, int table_index, char const* field_name)
	{
		table_index = absolute_index(L, table_index);
		std::vector<int> values;
		lua_getfield(L, table_index, field_name);
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			return values;
		}

		size_t const count = lua_objlen(L, -1);
		values.reserve(count);
		for (size_t i = 1; i <= count; ++i) {
			lua_rawgeti(L, -1, static_cast<int>(i));
			if (auto value = optional_int(L, -1))
				values.push_back(*value);
			lua_pop(L, 1);
		}

		lua_pop(L, 1);
		return values;
	}

	std::vector<std::string> string_array_field(lua_State *L, int table_index, char const* field_name)
	{
		table_index = absolute_index(L, table_index);
		std::vector<std::string> values;
		lua_getfield(L, table_index, field_name);
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			return values;
		}

		size_t const count = lua_objlen(L, -1);
		values.reserve(count);
		for (size_t i = 1; i <= count; ++i) {
			lua_rawgeti(L, -1, static_cast<int>(i));
			if (auto value = optional_string(L, -1))
				values.push_back(std::move(*value));
			lua_pop(L, 1);
		}

		lua_pop(L, 1);
		return values;
	}

	AutomationInvocationKind invocation_kind_from_string(std::string const& kind) noexcept
	{
		if (kind == "macro_validate") return AutomationInvocationKind::MacroValidate;
		if (kind == "macro_run") return AutomationInvocationKind::MacroRun;
		if (kind == "macro_is_active") return AutomationInvocationKind::MacroIsActive;
		if (kind == "export_filter_config") return AutomationInvocationKind::ExportFilterConfig;
		if (kind == "export_filter_run") return AutomationInvocationKind::ExportFilterRun;
		if (kind == "test") return AutomationInvocationKind::Test;
		return AutomationInvocationKind::MacroRun;
	}

	AutomationInvocation parse_invocation_table(lua_State *L, int table_index)
	{
		table_index = absolute_index(L, table_index);

		AutomationInvocation invocation;
		if (auto kind = optional_field(L, table_index, "kind", optional_string))
			invocation.kind = invocation_kind_from_string(*kind);
		if (auto feature_name = optional_field(L, table_index, "feature_name", optional_string))
			invocation.feature_name = std::move(*feature_name);
		invocation.capabilities.allow_modify = optional_field(L, table_index, "allow_modify", optional_bool).value_or(false);
		invocation.capabilities.allow_undo = optional_field(L, table_index, "allow_undo", optional_bool).value_or(false);
		invocation.capabilities.allow_dialog = optional_field(L, table_index, "allow_dialog", optional_bool).value_or(false);
		return invocation;
	}

	AutomationContextSnapshot parse_context_snapshot_table(lua_State *L, int table_index)
	{
		table_index = absolute_index(L, table_index);

		AutomationContextSnapshot snapshot;
		snapshot.has_project_context = optional_field(L, table_index, "has_project_context", optional_bool).value_or(false);

		if (auto selection = optional_table_field<AutomationSelectionSnapshot>(L, table_index, "selection", [](lua_State *L, int index) {
			AutomationSelectionSnapshot value;
			value.selected_rows = int_array_field(L, index, "selected_rows");
			value.active_row = optional_field(L, index, "active_row", optional_int).value_or(0);
			return value;
		})) {
			snapshot.selection = std::move(*selection);
		}

		if (auto media = optional_table_field<AutomationMediaSnapshot>(L, table_index, "media", [](lua_State *L, int index) {
			AutomationMediaSnapshot value;
			value.has_video = optional_field(L, index, "has_video", optional_bool).value_or(false);
			value.has_audio = optional_field(L, index, "has_audio", optional_bool).value_or(false);
			value.video_width = optional_field(L, index, "video_width", optional_int).value_or(0);
			value.video_height = optional_field(L, index, "video_height", optional_int).value_or(0);
			value.video_aspect_ratio = optional_field(L, index, "video_aspect_ratio", optional_double).value_or(0.0);
			value.video_aspect_ratio_type = optional_field(L, index, "video_aspect_ratio_type", optional_int).value_or(0);
			value.has_timecodes = optional_field(L, index, "has_timecodes", optional_bool).value_or(false);
			value.has_keyframes = optional_field(L, index, "has_keyframes", optional_bool).value_or(false);
			value.keyframes = int_array_field(L, index, "keyframes");
			value.has_audio_selection = optional_field(L, index, "has_audio_selection", optional_bool).value_or(false);
			value.audio_selection_begin = optional_field(L, index, "audio_selection_begin", optional_int).value_or(0);
			value.audio_selection_end = optional_field(L, index, "audio_selection_end", optional_int).value_or(0);
			return value;
		})) {
			snapshot.media = std::move(*media);
		}

		if (auto project = optional_table_field<AutomationProjectSnapshot>(L, table_index, "project", [](lua_State *L, int index) {
			AutomationProjectSnapshot value;
			value.script_filename = optional_field(L, index, "script_filename", optional_string).value_or("");
			value.subtitle_file = optional_field(L, index, "subtitle_file", optional_string).value_or("");
			value.automation_scripts = optional_field(L, index, "automation_scripts", optional_string).value_or("");
			value.export_filters = optional_field(L, index, "export_filters", optional_string).value_or("");
			value.export_encoding = optional_field(L, index, "export_encoding", optional_string).value_or("");
			value.style_storage = optional_field(L, index, "style_storage", optional_string).value_or("");
			value.audio_file = optional_field(L, index, "audio_file", optional_string).value_or("");
			value.video_file = optional_field(L, index, "video_file", optional_string).value_or("");
			value.timecodes_file = optional_field(L, index, "timecodes_file", optional_string).value_or("");
			value.keyframes_file = optional_field(L, index, "keyframes_file", optional_string).value_or("");
			value.play_res_x = optional_field(L, index, "play_res_x", optional_int).value_or(0);
			value.play_res_y = optional_field(L, index, "play_res_y", optional_int).value_or(0);
			value.info_count = optional_field(L, index, "info_count", optional_int).value_or(0);
			value.style_count = optional_field(L, index, "style_count", optional_int).value_or(0);
			value.event_count = optional_field(L, index, "event_count", optional_int).value_or(0);
			value.dialogue_count = optional_field(L, index, "dialogue_count", optional_int).value_or(0);
			value.comment_count = optional_field(L, index, "comment_count", optional_int).value_or(0);
			value.attachment_count = optional_field(L, index, "attachment_count", optional_int).value_or(0);
			value.extradata_count = optional_field(L, index, "extradata_count", optional_int).value_or(0);
			value.video_zoom = optional_field(L, index, "video_zoom", optional_double).value_or(0.0);
			value.ar_value = optional_field(L, index, "ar_value", optional_double).value_or(0.0);
			value.scroll_position = optional_field(L, index, "scroll_position", optional_int).value_or(0);
			value.ar_mode = optional_field(L, index, "ar_mode", optional_int).value_or(0);
			value.active_row = optional_field(L, index, "active_row", optional_int).value_or(0);
			value.video_position = optional_field(L, index, "video_position", optional_int).value_or(0);
			return value;
		})) {
			snapshot.project = std::move(*project);
		}

		return snapshot;
	}

	AutomationTemplateLineSnapshot parse_template_line_snapshot(lua_State *L, int table_index)
	{
		table_index = absolute_index(L, table_index);

		AutomationTemplateLineSnapshot snapshot;
		snapshot.index = optional_field(L, table_index, "i", optional_int);
		snapshot.line_class = optional_field(L, table_index, "class", optional_string);
		snapshot.layer = optional_field(L, table_index, "layer", optional_int);
		snapshot.style = optional_field(L, table_index, "style", optional_string);
		snapshot.actor = optional_field(L, table_index, "actor", optional_string);
		snapshot.effect = optional_field(L, table_index, "effect", optional_string);
		snapshot.comment = optional_field(L, table_index, "comment", optional_bool);
		snapshot.text = optional_field(L, table_index, "text", optional_string);
		snapshot.start_time = optional_field(L, table_index, "start_time", optional_int);
		snapshot.end_time = optional_field(L, table_index, "end_time", optional_int);
		return snapshot;
	}

	AutomationTemplateSyllableSnapshot parse_template_syllable_snapshot(lua_State *L, int table_index)
	{
		table_index = absolute_index(L, table_index);

		AutomationTemplateSyllableSnapshot snapshot;
		snapshot.index = optional_field(L, table_index, "i", optional_int);
		snapshot.text = optional_field(L, table_index, "text", optional_string);
		snapshot.text_stripped = optional_field(L, table_index, "text_stripped", optional_string);
		snapshot.inline_fx = optional_field(L, table_index, "inline_fx", optional_string);
		snapshot.start_time = optional_field(L, table_index, "start_time", optional_int);
		snapshot.end_time = optional_field(L, table_index, "end_time", optional_int);
		snapshot.duration = optional_field(L, table_index, "duration", optional_int);
		snapshot.is_furi = optional_field(L, table_index, "isfuri", optional_bool);
		snapshot.left = optional_field(L, table_index, "left", optional_double);
		snapshot.center = optional_field(L, table_index, "center", optional_double);
		snapshot.right = optional_field(L, table_index, "right", optional_double);
		snapshot.width = optional_field(L, table_index, "width", optional_double);
		snapshot.height = optional_field(L, table_index, "height", optional_double);
		return snapshot;
	}

	AutomationTemplateHighlightSnapshot parse_template_highlight_snapshot(lua_State *L, int table_index)
	{
		table_index = absolute_index(L, table_index);

		AutomationTemplateHighlightSnapshot snapshot;
		snapshot.index = optional_field(L, table_index, "i", optional_int);
		snapshot.start_time = optional_field(L, table_index, "start_time", optional_int);
		snapshot.end_time = optional_field(L, table_index, "end_time", optional_int);
		snapshot.duration = optional_field(L, table_index, "duration", optional_int);
		return snapshot;
	}

	AutomationTemplateCharSnapshot parse_template_char_snapshot(lua_State *L, int table_index)
	{
		table_index = absolute_index(L, table_index);

		AutomationTemplateCharSnapshot snapshot;
		snapshot.index = optional_field(L, table_index, "i", optional_int);
		snapshot.text = optional_field(L, table_index, "text", optional_string);
		return snapshot;
	}

	AutomationTemplateSourceFragment parse_template_source_fragment(lua_State *L, int table_index)
	{
		table_index = absolute_index(L, table_index);

		AutomationTemplateSourceFragment fragment;
		fragment.source_line_index = optional_field(L, table_index, "source_line_index", optional_int);
		fragment.fragment_kind = optional_field(L, table_index, "fragment_kind", optional_string);
		fragment.text = optional_field(L, table_index, "text", optional_string);
		fragment.effect = optional_field(L, table_index, "effect", optional_string);
		return fragment;
	}

	AutomationTemplateIdentity parse_template_identity(lua_State *L, int table_index)
	{
		table_index = absolute_index(L, table_index);

		AutomationTemplateIdentity identity;
		identity.owner_script = optional_field(L, table_index, "owner_script", optional_string);
		identity.template_debug_id = optional_field(L, table_index, "template_debug_id", optional_int);
		identity.template_kind = optional_field(L, table_index, "template_kind", optional_string);
		identity.template_kinds = string_array_field(L, table_index, "template_kinds");
		identity.fragment_kind = optional_field(L, table_index, "fragment_kind", optional_string);
		identity.template_id = optional_field(L, table_index, "template_id", optional_string);
		identity.source_line_index = optional_field(L, table_index, "source_line_index", optional_int);
		identity.source_line_indices = int_array_field(L, table_index, "source_line_indices");
		return identity;
	}

	AutomationTemplateSource parse_template_source(lua_State *L, int table_index)
	{
		table_index = absolute_index(L, table_index);

		AutomationTemplateSource source;
		source.style = optional_field(L, table_index, "style", optional_string);
		source.effect = optional_field(L, table_index, "effect", optional_string);
		source.text = optional_field(L, table_index, "text", optional_string);

		lua_getfield(L, table_index, "fragments");
		if (lua_istable(L, -1)) {
			size_t const count = lua_objlen(L, -1);
			source.fragments.reserve(count);
			for (size_t i = 1; i <= count; ++i) {
				lua_rawgeti(L, -1, static_cast<int>(i));
				if (lua_istable(L, -1))
					source.fragments.push_back(parse_template_source_fragment(L, -1));
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);

		return source;
	}

	AutomationTemplateTargetSnapshot parse_template_target_snapshot(lua_State *L, int table_index)
	{
		table_index = absolute_index(L, table_index);

		AutomationTemplateTargetSnapshot target;
		target.scope_kind = optional_field(L, table_index, "scope_kind", optional_string);
		target.original_line = optional_table_field<AutomationTemplateLineSnapshot>(L, table_index, "orgline", parse_template_line_snapshot);
		target.line = optional_table_field<AutomationTemplateLineSnapshot>(L, table_index, "line", parse_template_line_snapshot);
		target.syllable = optional_table_field<AutomationTemplateSyllableSnapshot>(L, table_index, "syl", parse_template_syllable_snapshot);
		target.base_syllable = optional_table_field<AutomationTemplateSyllableSnapshot>(L, table_index, "basesyl", parse_template_syllable_snapshot);
		target.highlight = optional_table_field<AutomationTemplateHighlightSnapshot>(L, table_index, "highlight", parse_template_highlight_snapshot);
		target.character = optional_table_field<AutomationTemplateCharSnapshot>(L, table_index, "char", parse_template_char_snapshot);
		return target;
	}

	AutomationGeneratedLineSnapshot parse_generated_line_snapshot(lua_State *L, int table_index)
	{
		table_index = absolute_index(L, table_index);

		AutomationGeneratedLineSnapshot snapshot;
		snapshot.generated_index = optional_field(L, table_index, "generated_index", optional_int);
		snapshot.text = optional_field(L, table_index, "text", optional_string);
		snapshot.style = optional_field(L, table_index, "style", optional_string);
		snapshot.layer = optional_field(L, table_index, "layer", optional_int);
		snapshot.effect = optional_field(L, table_index, "effect", optional_string);
		snapshot.start_time = optional_field(L, table_index, "start_time", optional_int);
		snapshot.end_time = optional_field(L, table_index, "end_time", optional_int);
		snapshot.source_line_index = optional_field(L, table_index, "source_line_index", optional_int);
		snapshot.template_debug_id = optional_field(L, table_index, "template_debug_id", optional_int);
		snapshot.template_kind = optional_field(L, table_index, "template_kind", optional_string);
		snapshot.scope_kind = optional_field(L, table_index, "scope_kind", optional_string);
		snapshot.syllable_index = optional_field(L, table_index, "syl_i", optional_int);
		snapshot.highlight_index = optional_field(L, table_index, "highlight_i", optional_int);
		snapshot.char_index = optional_field(L, table_index, "char_i", optional_int);
		return snapshot;
	}

	AutomationGeneratedLinesSnapshot parse_generated_lines_snapshot(lua_State *L, int table_index)
	{
		table_index = absolute_index(L, table_index);

		AutomationGeneratedLinesSnapshot snapshot;
		snapshot.count = optional_field(L, table_index, "count", optional_int);
		snapshot.last_line = optional_table_field<AutomationGeneratedLineSnapshot>(L, table_index, "last_line", parse_generated_line_snapshot);
		return snapshot;
	}

	AutomationTemplateIdentity derive_template_identity(lua_State *L, int table_index)
	{
		table_index = absolute_index(L, table_index);

		AutomationTemplateIdentity identity;
		identity.owner_script = std::optional<std::string>("kara-templater.lua");
		identity.template_debug_id = optional_field(L, table_index, "template_debug_id", optional_int);
		identity.template_kind = optional_field(L, table_index, "template_kind", optional_string);
		identity.template_kinds = string_array_field(L, table_index, "template_kinds");
		identity.fragment_kind = optional_field(L, table_index, "template_fragment_kind", optional_string);
		identity.template_id = optional_field(L, table_index, "template_id", optional_string);
		identity.source_line_index = optional_field(L, table_index, "template_source_line_index", optional_int);
		identity.source_line_indices = int_array_field(L, table_index, "template_source_line_indices");
		return identity;
	}

	AutomationTemplateSource derive_template_source(lua_State *L, int table_index)
	{
		table_index = absolute_index(L, table_index);

		AutomationTemplateSource source;
		source.style = optional_field(L, table_index, "template_style", optional_string);

		lua_getfield(L, table_index, "template_source_fragments");
		if (lua_istable(L, -1)) {
			size_t const count = lua_objlen(L, -1);
			source.fragments.reserve(count);
			for (size_t i = 1; i <= count; ++i) {
				lua_rawgeti(L, -1, static_cast<int>(i));
				if (lua_istable(L, -1))
					source.fragments.push_back(parse_template_source_fragment(L, -1));
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);

		return source;
	}

	AutomationTemplateDebugState parse_template_debug_state(lua_State *L, int table_index)
	{
		table_index = absolute_index(L, table_index);

		AutomationTemplateDebugState state;
		state.kind = optional_field(L, table_index, "kind", optional_string);
		state.template_code = optional_field(L, table_index, "template_code", optional_string);
		state.template_text = optional_field(L, table_index, "template_text", optional_string);
		state.loop_index = optional_field(L, table_index, "j", optional_int);
		state.loop_count = optional_field(L, table_index, "maxj", optional_int);
		state.line_text = optional_field(L, table_index, "line_text", optional_string);
		state.line_style = optional_field(L, table_index, "line_style", optional_string);
		state.syllable_text = optional_field(L, table_index, "syl_text", optional_string);
		state.syllable_index = optional_field(L, table_index, "syl_i", optional_int);
		state.base_syllable_text = optional_field(L, table_index, "basesyl_text", optional_string);
		state.phase = optional_field(L, table_index, "template_phase", optional_string);
		state.scope_kind = optional_field(L, table_index, "debug_scope", optional_string);
		state.highlight_index = optional_field(L, table_index, "highlight_i", optional_int);
		state.char_index = optional_field(L, table_index, "char_i", optional_int);
		state.char_text = optional_field(L, table_index, "char_text", optional_string);
		state.expression = optional_field(L, table_index, "expression", optional_string);
		state.parse_error = optional_field(L, table_index, "parse_error", optional_string);
		state.runtime_error = optional_field(L, table_index, "runtime_error", optional_string);

		state.identity = optional_table_field<AutomationTemplateIdentity>(L, table_index, "template_identity", parse_template_identity);
		if (!state.identity) {
			auto derived = derive_template_identity(L, table_index);
			if (derived.template_debug_id || derived.template_kind || derived.source_line_index || !derived.template_kinds.empty())
				state.identity = std::move(derived);
		}

		state.source = optional_table_field<AutomationTemplateSource>(L, table_index, "template_source", parse_template_source);
		if (!state.source) {
			auto derived = derive_template_source(L, table_index);
			if (derived.style || derived.effect || derived.text || !derived.fragments.empty())
				state.source = std::move(derived);
		}

		state.target = optional_table_field<AutomationTemplateTargetSnapshot>(L, table_index, "target", parse_template_target_snapshot);
		state.generated = optional_table_field<AutomationGeneratedLinesSnapshot>(L, table_index, "generated", parse_generated_lines_snapshot);
		return state;
	}

	std::optional<AutomationInvocation> load_runtime_invocation(lua_State *L)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, kAutomationRuntimeStateRegistryKey);
		if (lua_istable(L, -1)) {
			auto invocation = optional_table_field<AutomationInvocation>(L, -1, "invocation", parse_invocation_table);
			lua_pop(L, 1);
			if (invocation)
				return invocation;
		}
		else {
			lua_pop(L, 1);
		}

		lua_getfield(L, LUA_REGISTRYINDEX, kInvocationRegistryKey);
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			return std::nullopt;
		}

		auto invocation = parse_invocation_table(L, -1);
		lua_pop(L, 1);
		return invocation;
	}

	std::optional<AutomationContextSnapshot> load_runtime_context_snapshot(lua_State *L)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, kAutomationRuntimeStateRegistryKey);
		if (lua_istable(L, -1)) {
			auto snapshot = optional_table_field<AutomationContextSnapshot>(L, -1, "context_snapshot", parse_context_snapshot_table);
			lua_pop(L, 1);
			if (snapshot)
				return snapshot;
		}
		else {
			lua_pop(L, 1);
		}

		lua_getfield(L, LUA_REGISTRYINDEX, kContextSnapshotRegistryKey);
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			return std::nullopt;
		}

		auto snapshot = parse_context_snapshot_table(L, -1);
		lua_pop(L, 1);
		return snapshot;
	}

	std::optional<AutomationTemplateDebugState> load_runtime_template_debug_state(lua_State *L)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, kAutomationRuntimeStateRegistryKey);
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, "template_debug_context");
			if (lua_istable(L, -1)) {
				auto state = parse_template_debug_state(L, -1);
				lua_pop(L, 2);
				return state;
			}
			lua_pop(L, 2);
		}
		else {
			lua_pop(L, 1);
		}

		lua_getfield(L, LUA_REGISTRYINDEX, kTemplateDebugContextRegistryKey);
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			return std::nullopt;
		}

		auto state = parse_template_debug_state(L, -1);
		lua_pop(L, 1);
		return state;
	}

	void push_visual_guide_point(lua_State *L, Automation4::AutomationVisualGuidePoint const& point)
	{
		lua_createtable(L, 0, 2);
		set_field(L, "x", point.x);
		set_field(L, "y", point.y);
	}

	void push_visual_guide(lua_State *L, Automation4::AutomationVisualGuide const& guide)
	{
		// id, kind, coordinate_space, first, second, delta_x, delta_y, distance,
		// angle_degrees, shear_x, shear_y, angle_horizontal_degrees,
		// angle_vertical_degrees
		lua_createtable(L, 0, 13);
		set_field(L, "id", guide.id);
		set_field(L, "kind", guide.kind);
		set_field(L, "coordinate_space", guide.coordinate_space);
		push_visual_guide_point(L, guide.first);
		lua_setfield(L, -2, "first");
		push_visual_guide_point(L, guide.second);
		lua_setfield(L, -2, "second");
		set_field(L, "delta_x", guide.delta_x);
		set_field(L, "delta_y", guide.delta_y);
		set_field(L, "distance", guide.distance);
		set_field(L, "angle_degrees", guide.angle_degrees);
		set_field(L, "shear_x", guide.shear_x);
		set_field(L, "shear_y", guide.shear_y);
		set_field(L, "angle_horizontal_degrees", guide.angle_horizontal_degrees);
		set_field(L, "angle_vertical_degrees", guide.angle_vertical_degrees);
	}

	void push_visual_guide_resolution(lua_State *L, int width, int height)
	{
		lua_createtable(L, 0, 2);
		set_field(L, "width", width);
		set_field(L, "height", height);
	}

	void push_visual_guide_snapshot_table(lua_State *L, Automation4::AutomationVisualGuideSnapshot const& snapshot)
	{
		lua_createtable(L, 0, 9);
		set_field(L, "schema_version", 1);
		set_field(L, "available", snapshot.available);

		if (!snapshot.available) {
			set_field(L, "generation", 0);
			set_field(L, "frame", -1);
			push_visual_guide_resolution(L, 0, 0);
			lua_setfield(L, -2, "script_resolution");
			push_visual_guide_resolution(L, 0, 0);
			lua_setfield(L, -2, "frame_resolution");
			lua_createtable(L, 0, 0);
			lua_setfield(L, -2, "guides");
			return;
		}

		push_value(L, snapshot.generation);
		lua_setfield(L, -2, "generation");
		set_field(L, "frame", snapshot.frame);
		push_visual_guide_resolution(L, snapshot.script_width, snapshot.script_height);
		lua_setfield(L, -2, "script_resolution");
		push_visual_guide_resolution(L, snapshot.frame_width, snapshot.frame_height);
		lua_setfield(L, -2, "frame_resolution");
		if (snapshot.selected_id)
			set_field(L, "selected_id", *snapshot.selected_id);
		if (snapshot.last_measurement_id)
			set_field(L, "last_measurement_id", *snapshot.last_measurement_id);

		lua_createtable(L, static_cast<int>(snapshot.guides.size()), 0);
		for (size_t i = 0; i < snapshot.guides.size(); ++i) {
			push_visual_guide(L, snapshot.guides[i]);
			lua_rawseti(L, -2, static_cast<int>(i + 1));
		}
		lua_setfield(L, -2, "guides");
	}

	void push_invocation_table(lua_State *L, Automation4::AutomationInvocation const& invocation)
	{
		lua_createtable(L, 0, 5);
		set_field(L, "kind", ToString(invocation.kind));
		set_field(L, "feature_name", invocation.feature_name);
		set_field(L, "allow_modify", invocation.capabilities.allow_modify);
		set_field(L, "allow_undo", invocation.capabilities.allow_undo);
		set_field(L, "allow_dialog", invocation.capabilities.allow_dialog);
	}

	void push_context_snapshot_table(lua_State *L, Automation4::AutomationContextSnapshot const& snapshot)
	{
		lua_createtable(L, 0, 4);

		lua_createtable(L, 0, 2);
		push_value(L, snapshot.selection.selected_rows);
		lua_setfield(L, -2, "selected_rows");
		set_field(L, "active_row", snapshot.selection.active_row);
		lua_setfield(L, -2, "selection");

		lua_createtable(L, 0, 11);
		set_field(L, "has_video", snapshot.media.has_video);
		set_field(L, "has_audio", snapshot.media.has_audio);
		set_field(L, "video_width", snapshot.media.video_width);
		set_field(L, "video_height", snapshot.media.video_height);
		set_field(L, "video_aspect_ratio", snapshot.media.video_aspect_ratio);
		set_field(L, "video_aspect_ratio_type", snapshot.media.video_aspect_ratio_type);
		set_field(L, "has_timecodes", snapshot.media.has_timecodes);
		set_field(L, "has_keyframes", snapshot.media.has_keyframes);
		push_value(L, snapshot.media.keyframes);
		lua_setfield(L, -2, "keyframes");
		set_field(L, "has_audio_selection", snapshot.media.has_audio_selection);
		set_field(L, "audio_selection_begin", snapshot.media.audio_selection_begin);
		set_field(L, "audio_selection_end", snapshot.media.audio_selection_end);
		lua_setfield(L, -2, "media");

		lua_createtable(L, 0, 25);
		set_field(L, "script_filename", snapshot.project.script_filename);
		set_field(L, "subtitle_file", snapshot.project.subtitle_file);
		set_field(L, "automation_scripts", snapshot.project.automation_scripts);
		set_field(L, "export_filters", snapshot.project.export_filters);
		set_field(L, "export_encoding", snapshot.project.export_encoding);
		set_field(L, "style_storage", snapshot.project.style_storage);
		set_field(L, "audio_file", snapshot.project.audio_file);
		set_field(L, "video_file", snapshot.project.video_file);
		set_field(L, "timecodes_file", snapshot.project.timecodes_file);
		set_field(L, "keyframes_file", snapshot.project.keyframes_file);
		set_field(L, "play_res_x", snapshot.project.play_res_x);
		set_field(L, "play_res_y", snapshot.project.play_res_y);
		set_field(L, "info_count", snapshot.project.info_count);
		set_field(L, "style_count", snapshot.project.style_count);
		set_field(L, "event_count", snapshot.project.event_count);
		set_field(L, "dialogue_count", snapshot.project.dialogue_count);
		set_field(L, "comment_count", snapshot.project.comment_count);
		set_field(L, "attachment_count", snapshot.project.attachment_count);
		set_field(L, "extradata_count", snapshot.project.extradata_count);
		set_field(L, "video_zoom", snapshot.project.video_zoom);
		set_field(L, "ar_value", snapshot.project.ar_value);
		set_field(L, "scroll_position", snapshot.project.scroll_position);
		set_field(L, "ar_mode", snapshot.project.ar_mode);
		set_field(L, "active_row", snapshot.project.active_row);
		set_field(L, "video_position", snapshot.project.video_position);
		lua_setfield(L, -2, "project");

		set_field(L, "has_project_context", snapshot.has_project_context);
	}

	void push_runtime_state_table(
		lua_State *L,
		Automation4::AutomationInvocation const& invocation,
		Automation4::AutomationContextSnapshot const& context_snapshot)
	{
		lua_createtable(L, 0, 3);

		push_invocation_table(L, invocation);
		lua_setfield(L, -2, "invocation");

		push_context_snapshot_table(L, context_snapshot);
		lua_setfield(L, -2, "context_snapshot");

		lua_pushnil(L);
		lua_setfield(L, -2, "template_debug_context");
	}

	void set_legacy_registry_field(lua_State *L, char const* field_name, char const* registry_key)
	{
		lua_getfield(L, -1, field_name);
		lua_setfield(L, LUA_REGISTRYINDEX, registry_key);
	}

	void push_or_create_runtime_state(lua_State *L)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, kAutomationRuntimeStateRegistryKey);
		if (lua_istable(L, -1))
			return;

		lua_pop(L, 1);
		lua_createtable(L, 0, 4);
		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, kAutomationRuntimeStateRegistryKey);
	}

	int destroy_automation_host_userdata(lua_State *L)
	{
		auto *host = static_cast<std::shared_ptr<AutomationHost> *>(lua_touserdata(L, 1));
		host->~shared_ptr<AutomationHost>();
		return 0;
	}

	void ensure_automation_host_metatable(lua_State *L)
	{
		if (!luaL_newmetatable(L, kAutomationHostRegistryMetatable)) {
			lua_pop(L, 1);
			luaL_getmetatable(L, kAutomationHostRegistryMetatable);
			return;
		}

		lua_pushcfunction(L, destroy_automation_host_userdata);
		lua_setfield(L, -2, "__gc");
	}

	void store_automation_host(lua_State *L, std::shared_ptr<AutomationHost> host)
	{
		if (host) {
			auto *storage = static_cast<std::shared_ptr<AutomationHost> *>(
				lua_newuserdata(L, sizeof(std::shared_ptr<AutomationHost>)));
			new(storage) std::shared_ptr<AutomationHost>(std::move(host));
			ensure_automation_host_metatable(L);
			lua_setmetatable(L, -2);
		}
		else {
			lua_pushnil(L);
		}
		lua_setfield(L, LUA_REGISTRYINDEX, kAutomationHostRegistryKey);
	}

	std::shared_ptr<AutomationHost> load_automation_host(lua_State *L)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, kAutomationHostRegistryKey);
		if (!lua_isuserdata(L, -1)) {
			lua_pop(L, 1);
			return {};
		}

		auto host = *static_cast<std::shared_ptr<AutomationHost> *>(lua_touserdata(L, -1));
		lua_pop(L, 1);
		return host;
	}

	AutomationHost *load_automation_host_raw(lua_State *L)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, kAutomationHostRegistryKey);
		if (!lua_isuserdata(L, -1)) {
			lua_pop(L, 1);
			return nullptr;
		}

		auto *host = static_cast<std::shared_ptr<AutomationHost> *>(lua_touserdata(L, -1))->get();
		lua_pop(L, 1);
		return host;
	}

	AutomationRuntimeTraceSink *load_runtime_trace_sink(lua_State *L)
	{
		lua_getfield(L, LUA_REGISTRYINDEX, kRuntimeTraceSinkRegistryKey);
		if (!lua_islightuserdata(L, -1)) {
			lua_pop(L, 1);
			return nullptr;
		}

		auto *sink = static_cast<AutomationRuntimeTraceSink *>(lua_touserdata(L, -1));
		lua_pop(L, 1);
		return sink;
	}

	void notify_runtime_trace_sink(lua_State *L)
	{
		auto *sink = load_runtime_trace_sink(L);
		if (!sink)
			return;

		auto snapshot = Automation4::LuaGetAutomationRuntimeStateSnapshot(L);
		if (snapshot)
			sink->OnRuntimeStateSnapshot(*snapshot);
	}
}

namespace Automation4 {
void LuaSetWorkspaceRunRequest(lua_State *L, std::shared_ptr<LuaWorkspaceRunRequest const> request) {
	if (request) {
		using Request = std::shared_ptr<LuaWorkspaceRunRequest const>;
		auto *storage = static_cast<Request *>(lua_newuserdata(L, sizeof(Request)));
		new (storage) Request(std::move(request));
		if (luaL_newmetatable(L, kWorkspaceRunMetatable)) {
			lua_pushcfunction(L, destroy_workspace_run);
			lua_setfield(L, -2, "__gc");
		}
		lua_setmetatable(L, -2);
	}
	else
		lua_pushnil(L);
	lua_setfield(L, LUA_REGISTRYINDEX, kWorkspaceRunRegistryKey);
}

std::shared_ptr<LuaWorkspaceRunRequest const> LuaGetWorkspaceRunRequest(lua_State *L) {
	lua_getfield(L, LUA_REGISTRYINDEX, kWorkspaceRunRegistryKey);
	std::shared_ptr<LuaWorkspaceRunRequest const> request;
	if (lua_isuserdata(L, -1))
		request = *static_cast<std::shared_ptr<LuaWorkspaceRunRequest const> *>(lua_touserdata(L, -1));
	lua_pop(L, 1);
	return request;
}

int LuaRaiseWorkspaceCancellation(lua_State *L) {
	lua_pushlightuserdata(L, &workspace_cancel_token);
	return lua_error(L);
}

bool LuaIsWorkspaceCancellation(lua_State *L, int index) {
	return lua_islightuserdata(L, index) && lua_touserdata(L, index) == &workspace_cancel_token;
}

	void LuaSetAutomationHost(lua_State *L, std::shared_ptr<AutomationHost> host)
	{
		store_automation_host(L, std::move(host));
	}

	std::shared_ptr<AutomationHost> LuaGetAutomationHostShared(lua_State *L)
	{
		return load_automation_host(L);
	}

	AutomationHost *LuaGetAutomationHost(lua_State *L)
	{
		return load_automation_host_raw(L);
	}

	void LuaSetAutomationRuntimeTraceSink(lua_State *L, AutomationRuntimeTraceSink *sink)
	{
		if (sink)
			lua_pushlightuserdata(L, sink);
		else
			lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, kRuntimeTraceSinkRegistryKey);
	}

	void LuaSetAutomationRuntimeState(
		lua_State *L,
		std::shared_ptr<AutomationHost> host,
		AutomationInvocation const& invocation,
		std::vector<int> selection,
		int active_row)
	{
		if (!host)
			host = LuaGetAutomationHostShared(L);
		LuaSetAutomationHost(L, host);

		auto snapshot = CaptureAutomationContextSnapshot(host.get(), std::move(selection), active_row);
		push_runtime_state_table(L, invocation, snapshot);

		lua_pushvalue(L, -1);
		lua_setfield(L, LUA_REGISTRYINDEX, kAutomationRuntimeStateRegistryKey);

		set_legacy_registry_field(L, "invocation", kInvocationRegistryKey);
		set_legacy_registry_field(L, "context_snapshot", kContextSnapshotRegistryKey);
		set_legacy_registry_field(L, "template_debug_context", kTemplateDebugContextRegistryKey);

		lua_pop(L, 1);
		notify_runtime_trace_sink(L);
	}

	void LuaSetAutomationTemplateDebugContext(lua_State *L, int value_index)
	{
		int const abs_index = value_index ? absolute_index(L, value_index) : 0;
		push_or_create_runtime_state(L);

		if (abs_index)
			lua_pushvalue(L, abs_index);
		else
			lua_pushnil(L);
		lua_setfield(L, -2, "template_debug_context");

		set_legacy_registry_field(L, "template_debug_context", kTemplateDebugContextRegistryKey);
		lua_pop(L, 1);
		notify_runtime_trace_sink(L);
	}

	std::optional<AutomationRuntimeStateSnapshot> LuaGetAutomationRuntimeStateSnapshot(lua_State *L)
	{
		auto invocation = load_runtime_invocation(L);
		auto context_snapshot = load_runtime_context_snapshot(L);
		if (!invocation || !context_snapshot)
			return std::nullopt;

		AutomationRuntimeStateSnapshot snapshot;
		snapshot.invocation = std::move(*invocation);
		snapshot.context_snapshot = std::move(*context_snapshot);
		snapshot.template_debug = load_runtime_template_debug_state(L);
		return snapshot;
	}

	std::optional<AutomationTemplateDebugState> LuaGetAutomationTemplateDebugState(lua_State *L)
	{
		return load_runtime_template_debug_state(L);
	}

	void LuaPushVisualGuideSnapshot(lua_State *L, AutomationVisualGuideSnapshot const& snapshot)
	{
		push_visual_guide_snapshot_table(L, snapshot);
	}
}
