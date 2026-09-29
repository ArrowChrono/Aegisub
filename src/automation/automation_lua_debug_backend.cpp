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

#include "automation_lua_debug_backend.h"

#include "../auto4_lua.h"
#include "automation_lua_runtime.h"
#include "lua_workspace_run.h"

#include <libaegisub/fs.h>
#include <libaegisub/lua/utils.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

using namespace agi::lua;

namespace Automation4 {
namespace {

constexpr char kLuaDebugBackendRegistryKey[] = "automation_lua_debug_backend";
constexpr int kMaxVariableDepth = 3;
constexpr size_t kMaxVariableChildren = 24;
constexpr size_t kExpandedRuntimeTableChildren = 64;
constexpr size_t kMaxGlobalVariables = 64;
constexpr size_t kMaxSubsPreviewRows = 6;
constexpr size_t kMaxStringPreviewLength = 120;
constexpr size_t kMaxLinePreviewLength = 48;
constexpr char kAutomationDebugProgressRegistryKey[] = "automation_debug_progress_state";
constexpr char kAutomationDebugDialogRegistryKey[] = "automation_debug_dialog_state";

struct LuaVariableCaptureState {
	std::set<void const*> active_tables;
};

struct RankedGlobalVariable {
	bool function = false;
	AutomationDebugVariable variable;
};

struct CapturedGlobalScopes {
	std::vector<AutomationDebugVariable> globals;
	std::vector<AutomationDebugVariable> functions;
	std::vector<AutomationDebugVariable> runtime_globals;
};

int AbsoluteIndex(lua_State *L, int index)
{
	if (index > 0 || index <= LUA_REGISTRYINDEX)
		return index;
	return lua_gettop(L) + index + 1;
}

std::string FormatPointer(void const* pointer)
{
	std::ostringstream out;
	out << "0x" << std::hex << reinterpret_cast<uintptr_t>(pointer);
	return out.str();
}

struct ValuePreview {
	std::string text;
	size_t shown_bytes = 0;
	size_t total_bytes = 0;
};

size_t Utf8SequenceLength(std::string_view value, size_t index) {
	auto const lead = static_cast<unsigned char>(value[index]);
	if (lead < 0x80)
		return 1;
	size_t length = 0;
	if (lead >= 0xc2 && lead <= 0xdf)
		length = 2;
	else if (lead >= 0xe0 && lead <= 0xef)
		length = 3;
	else if (lead >= 0xf0 && lead <= 0xf4)
		length = 4;
	if (!length || index + length > value.size())
		return 0;
	for (size_t i = 1; i < length; ++i) {
		auto const next = static_cast<unsigned char>(value[index + i]);
		if (next < 0x80 || next > 0xbf)
			return 0;
	}
	auto const second = static_cast<unsigned char>(value[index + 1]);
	if ((lead == 0xe0 && second < 0xa0) ||
		(lead == 0xed && second > 0x9f) ||
		(lead == 0xf0 && second < 0x90) ||
		(lead == 0xf4 && second > 0x8f))
		return 0;
	return length;
}

ValuePreview TruncateValue(std::string_view value, size_t max_length = kMaxStringPreviewLength) {
	ValuePreview preview;
	preview.total_bytes = value.size();
	for (size_t index = 0; index < value.size();) {
		auto const byte = static_cast<unsigned char>(value[index]);
		auto const sequence_length = Utf8SequenceLength(value, index);
		std::array<char, 4> escaped{};
		std::string_view token;
		if (byte == '\n')
			token = "\\n";
		else if (byte == '\r')
			token = "\\r";
		else if (byte == '\t')
			token = "\\t";
		else if (byte == '\\')
			token = "\\\\";
		else if (byte == '"')
			token = "\\\"";
		else if (!sequence_length || byte < 0x20 || byte == 0x7f) {
			static constexpr char hex[] = "0123456789ABCDEF";
			escaped = {'\\', 'x', hex[byte >> 4], hex[byte & 0x0f]};
			token = std::string_view(escaped.data(), escaped.size());
		}
		else
			token = value.substr(index, sequence_length);
		auto const consumed = sequence_length ? sequence_length : 1;
		if (preview.text.size() + token.size() + (index + consumed < value.size() ? 3 : 0) > max_length)
			break;
		preview.text.append(token);
		index += consumed;
		preview.shown_bytes = index;
	}
	if (preview.shown_bytes < preview.total_bytes)
		preview.text += "...";
	return preview;
}

std::string PreviewLengthSuffix(ValuePreview const& preview) {
	if (preview.shown_bytes == preview.total_bytes)
		return {};
	return " (" + std::to_string(preview.shown_bytes) + "/" + std::to_string(preview.total_bytes) + " bytes)";
}

std::string FormatStringPreview(lua_State *L, int index) {
	size_t length = 0;
	char const *value = lua_tolstring(L, index, &length);
	auto preview = TruncateValue(value ? std::string_view(value, length) : std::string_view());
	return "\"" + preview.text + "\"" + PreviewLengthSuffix(preview);
}

std::string FormatNumberPreview(lua_State *L, int index, std::string& value_type)
{
	auto const number = lua_tonumber(L, index);
	auto const integer = lua_tointeger(L, index);
	std::ostringstream out;
	if (static_cast<lua_Number>(integer) == number) {
		value_type = "integer";
		out << integer;
	}
	else {
		value_type = "number";
		out << number;
	}
	return out.str();
}

std::string FormatTableName(lua_State *L, int index)
{
	std::ostringstream out;
	out << "table";
	auto const array_length = lua_objlen(L, index);
	if (array_length != 0)
		out << "[a=" << array_length << "]";
	return out.str();
}

void RawGetTableField(lua_State *L, int table_index, char const* key)
{
	table_index = AbsoluteIndex(L, table_index);
	lua_pushstring(L, key);
	lua_rawget(L, table_index);
}

std::optional<std::string> TryGetTableStringField(lua_State *L, int table_index, char const* key)
{
	table_index = AbsoluteIndex(L, table_index);
	RawGetTableField(L, table_index, key);
	auto cleanup = agi::make_scope_exit([&] { lua_pop(L, 1); });
	if (!lua_isstring(L, -1))
		return std::nullopt;
	return get_string_or_default(L, -1);
}

std::optional<int> TryGetTableIntegerField(lua_State *L, int table_index, char const* key)
{
	table_index = AbsoluteIndex(L, table_index);
	RawGetTableField(L, table_index, key);
	auto cleanup = agi::make_scope_exit([&] { lua_pop(L, 1); });
	if (!lua_isnumber(L, -1))
		return std::nullopt;
	return static_cast<int>(lua_tointeger(L, -1));
}

std::optional<double> TryGetTableDoubleField(lua_State *L, int table_index, char const* key)
{
	table_index = AbsoluteIndex(L, table_index);
	RawGetTableField(L, table_index, key);
	auto cleanup = agi::make_scope_exit([&] { lua_pop(L, 1); });
	if (!lua_isnumber(L, -1))
		return std::nullopt;
	return lua_tonumber(L, -1);
}

std::optional<bool> TryGetTableBooleanField(lua_State *L, int table_index, char const* key)
{
	table_index = AbsoluteIndex(L, table_index);
	RawGetTableField(L, table_index, key);
	auto cleanup = agi::make_scope_exit([&] { lua_pop(L, 1); });
	if (!lua_isboolean(L, -1))
		return std::nullopt;
	return lua_toboolean(L, -1) != 0;
}

std::string FormatInlineString(std::string_view value, size_t max_length = kMaxLinePreviewLength) {
	auto preview = TruncateValue(value, max_length);
	return "\"" + preview.text + "\"" + PreviewLengthSuffix(preview);
}

std::string BuildTimeRangePreview(
	std::optional<int> const& start_time,
	std::optional<int> const& end_time)
{
	if (!start_time && !end_time)
		return {};

	std::ostringstream out;
	out << "[";
	out << (start_time ? std::to_string(*start_time) : "?");
	out << "..";
	out << (end_time ? std::to_string(*end_time) : "?");
	out << "ms]";
	return out.str();
}

std::string BuildDialogueTablePreview(lua_State *L, int table_index)
{
	std::ostringstream out;
	bool const is_comment = TryGetTableBooleanField(L, table_index, "comment").value_or(false);
	out << (is_comment ? "comment-line" : "dialogue-line");

	if (auto style = TryGetTableStringField(L, table_index, "style"); style && !style->empty())
		out << " <" << *style << ">";

	auto const start_time = TryGetTableIntegerField(L, table_index, "start_time");
	auto const end_time = TryGetTableIntegerField(L, table_index, "end_time");
	auto time_range = BuildTimeRangePreview(start_time, end_time);
	if (!time_range.empty())
		out << " " << time_range;

	if (auto text = TryGetTableStringField(L, table_index, "text"); text && !text->empty())
		out << " " << FormatInlineString(*text);

	return out.str();
}

std::string BuildStyleTablePreview(lua_State *L, int table_index)
{
	std::ostringstream out;
	out << "style-line";
	if (auto name = TryGetTableStringField(L, table_index, "name"); name && !name->empty())
		out << " <" << *name << ">";
	if (auto font = TryGetTableStringField(L, table_index, "fontname"); font && !font->empty())
		out << " " << *font;
	return out.str();
}

std::string BuildInfoTablePreview(lua_State *L, int table_index)
{
	std::ostringstream out;
	out << "info-line";
	if (auto key = TryGetTableStringField(L, table_index, "key"); key && !key->empty()) {
		out << " " << *key;
		if (auto value = TryGetTableStringField(L, table_index, "value"); value && !value->empty()) {
			auto preview = TruncateValue(*value, kMaxLinePreviewLength);
			out << "=" << preview.text << PreviewLengthSuffix(preview);
		}
	}
	return out.str();
}

bool TableHasField(lua_State *L, int table_index, char const* key)
{
	table_index = AbsoluteIndex(L, table_index);
	RawGetTableField(L, table_index, key);
	bool const present = !lua_isnil(L, -1);
	lua_pop(L, 1);
	return present;
}

std::string BuildKaraskelMetaPreview(lua_State *L, int table_index)
{
	auto res_x = TryGetTableIntegerField(L, table_index, "res_x");
	auto res_y = TryGetTableIntegerField(L, table_index, "res_y");
	std::ostringstream out;
	out << "karaskel-meta";
	if (res_x && res_y)
		out << " " << *res_x << "x" << *res_y;
	return out.str();
}

std::string BuildKaraskelSyllablePreview(lua_State *L, int table_index)
{
	std::ostringstream out;
	bool const is_furi = TryGetTableBooleanField(L, table_index, "isfuri").value_or(false)
		|| TryGetTableBooleanField(L, table_index, "is_furi").value_or(false);
	out << (is_furi ? "kara-furi" : "kara-syllable");
	if (auto index = TryGetTableIntegerField(L, table_index, "i"); index)
		out << " #" << *index;
	if (auto text = TryGetTableStringField(L, table_index, "text_stripped"); text && !text->empty())
		out << " " << FormatInlineString(*text);
	else if (auto text = TryGetTableStringField(L, table_index, "text"); text && !text->empty())
		out << " " << FormatInlineString(*text);
	auto time_range = BuildTimeRangePreview(
		TryGetTableIntegerField(L, table_index, "start_time"),
		TryGetTableIntegerField(L, table_index, "end_time"));
	if (!time_range.empty())
		out << " " << time_range;
	return out.str();
}

std::string BuildKaraskelLinePreview(lua_State *L, int table_index)
{
	std::ostringstream out;
	out << "kara-line";
	if (auto style = TryGetTableStringField(L, table_index, "style"); style && !style->empty())
		out << " <" << *style << ">";
	if (auto text = TryGetTableStringField(L, table_index, "text_stripped"); text && !text->empty())
		out << " " << FormatInlineString(*text);
	else if (auto text = TryGetTableStringField(L, table_index, "text"); text && !text->empty())
		out << " " << FormatInlineString(*text);
	auto time_range = BuildTimeRangePreview(
		TryGetTableIntegerField(L, table_index, "start_time"),
		TryGetTableIntegerField(L, table_index, "end_time"));
	if (!time_range.empty())
		out << " " << time_range;
	return out.str();
}

std::string BuildKaraskelCollectionPreview(lua_State *L, int table_index, char const* label)
{
	auto count = TryGetTableIntegerField(L, table_index, "n").value_or(static_cast<int>(lua_objlen(L, table_index)));
	std::ostringstream out;
	out << label << "[" << count << "]";
	return out.str();
}

int KnownTableChildPriority(std::string_view parent_name, std::string_view child_name)
{
	if (parent_name == "_G") {
		static constexpr std::array<std::string_view, 24> order{{
			"aegisub",
			"_VERSION",
			"string",
			"table",
			"math",
			"package",
			"print",
			"pairs",
			"ipairs",
			"require",
			"type",
			"tostring",
			"tonumber",
			"error",
			"os",
			"io",
			"debug",
			"coroutine",
			"bit",
			"jit",
			"re",
			"unicode",
			"lfs",
			"include",
		}};
		auto it = std::find(order.begin(), order.end(), child_name);
		if (it != order.end())
			return static_cast<int>(std::distance(order.begin(), it));
	}
	if (parent_name == "aegisub") {
		static constexpr std::array<std::string_view, 25> order{{
			"register_macro",
			"register_filter",
			"progress",
			"dialog",
			"debug",
			"log",
			"text_extents",
			"frame_from_ms",
			"ms_from_frame",
			"video_size",
			"keyframes",
			"decode_path",
			"cancel",
			"file_name",
			"gettext",
			"project_properties",
			"get_audio_selection",
			"scroll_audio_to",
			"set_status_text",
			"focus_edit_box",
			"get_edit_box_cursor",
			"set_edit_box_cursor",
			"parse_karaoke_data",
			"set_undo_point",
			"lua_automation_version",
		}};
		auto it = std::find(order.begin(), order.end(), child_name);
		if (it != order.end())
			return static_cast<int>(std::distance(order.begin(), it));
	}
	if (parent_name == "progress") {
		static constexpr std::array<std::string_view, 4> order{{ "title", "task", "set", "is_cancelled" }};
		auto it = std::find(order.begin(), order.end(), child_name);
		if (it != order.end())
			return static_cast<int>(std::distance(order.begin(), it));
	}
	if (parent_name == "dialog") {
		static constexpr std::array<std::string_view, 3> order{{ "display", "open", "save" }};
		auto it = std::find(order.begin(), order.end(), child_name);
		if (it != order.end())
			return static_cast<int>(std::distance(order.begin(), it));
	}
	if (parent_name == "debug") {
		if (child_name == "out")
			return 0;
	}
	if (parent_name == "package") {
		static constexpr std::array<std::string_view, 8> order{{ "loaded", "preload", "loaders", "searchers", "path", "cpath", "searchpath", "loadlib" }};
		auto it = std::find(order.begin(), order.end(), child_name);
		if (it != order.end())
			return static_cast<int>(std::distance(order.begin(), it));
	}
	if (parent_name == "string") {
		static constexpr std::array<std::string_view, 14> order{{ "format", "find", "match", "gmatch", "gsub", "sub", "len", "lower", "upper", "byte", "char", "rep", "reverse", "dump" }};
		auto it = std::find(order.begin(), order.end(), child_name);
		if (it != order.end())
			return static_cast<int>(std::distance(order.begin(), it));
	}
	if (parent_name == "table") {
		static constexpr std::array<std::string_view, 11> order{{ "insert", "remove", "sort", "concat", "unpack", "pack", "move", "foreach", "foreachi", "getn", "maxn" }};
		auto it = std::find(order.begin(), order.end(), child_name);
		if (it != order.end())
			return static_cast<int>(std::distance(order.begin(), it));
	}
	if (parent_name == "math") {
		static constexpr std::array<std::string_view, 30> order{{ "pi", "huge", "abs", "min", "max", "floor", "ceil", "sqrt", "pow", "random", "randomseed", "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "deg", "rad", "exp", "log", "log10", "frexp", "ldexp", "fmod", "modf", "sinh", "cosh", "tanh" }};
		auto it = std::find(order.begin(), order.end(), child_name);
		if (it != order.end())
			return static_cast<int>(std::distance(order.begin(), it));
	}
	if (parent_name == "coroutine") {
		static constexpr std::array<std::string_view, 6> order{{ "create", "resume", "running", "status", "wrap", "yield" }};
		auto it = std::find(order.begin(), order.end(), child_name);
		if (it != order.end())
			return static_cast<int>(std::distance(order.begin(), it));
	}
	if (parent_name == "os") {
		static constexpr std::array<std::string_view, 11> order{{ "clock", "date", "difftime", "time", "execute", "exit", "getenv", "remove", "rename", "setlocale", "tmpname" }};
		auto it = std::find(order.begin(), order.end(), child_name);
		if (it != order.end())
			return static_cast<int>(std::distance(order.begin(), it));
	}
	if (parent_name == "io") {
		static constexpr std::array<std::string_view, 14> order{{ "open", "lines", "input", "output", "read", "write", "flush", "close", "popen", "tmpfile", "stdin", "stdout", "stderr", "type" }};
		auto it = std::find(order.begin(), order.end(), child_name);
		if (it != order.end())
			return static_cast<int>(std::distance(order.begin(), it));
	}
	if (parent_name == "bit") {
		static constexpr std::array<std::string_view, 12> order{{ "tobit", "tohex", "bnot", "bor", "band", "bxor", "lshift", "rshift", "arshift", "rol", "ror", "bswap" }};
		auto it = std::find(order.begin(), order.end(), child_name);
		if (it != order.end())
			return static_cast<int>(std::distance(order.begin(), it));
	}
	if (parent_name == "debug") {
		static constexpr std::array<std::string_view, 18> order{{ "traceback", "getinfo", "getlocal", "setlocal", "getupvalue", "setupvalue", "gethook", "sethook", "getregistry", "debug", "getmetatable", "setmetatable", "getfenv", "setfenv", "getuservalue", "setuservalue", "upvalueid", "upvaluejoin" }};
		auto it = std::find(order.begin(), order.end(), child_name);
		if (it != order.end())
			return static_cast<int>(std::distance(order.begin(), it));
	}
	if (parent_name == "jit") {
		static constexpr std::array<std::string_view, 10> order{{ "on", "off", "flush", "status", "version", "version_num", "arch", "os", "opt", "attach" }};
		auto it = std::find(order.begin(), order.end(), child_name);
		if (it != order.end())
			return static_cast<int>(std::distance(order.begin(), it));
	}
	if (parent_name == "loaded" || parent_name == "preload") {
		static constexpr std::array<std::string_view, 23> order{{
			"aegisub",
			"karaskel",
			"utils",
			"ffi",
			"re",
			"unicode",
			"lfs",
			"moonscript",
			"moonscript.base",
			"string",
			"table",
			"math",
			"os",
			"io",
			"debug",
			"coroutine",
			"bit",
			"jit",
			"jit.opt",
			"package",
			"_G",
			"aegisub.ffi",
			"aegisub.unicode",
		}};
		auto it = std::find(order.begin(), order.end(), child_name);
		if (it != order.end())
			return static_cast<int>(std::distance(order.begin(), it));
	}
	return 1000;
}

bool ShouldExpandRuntimeLibraryTable(std::string_view variable_name)
{
	static constexpr std::array<std::string_view, 16> names{{
		"aegisub",
		"string",
		"table",
		"math",
		"package",
		"debug",
		"coroutine",
		"os",
		"io",
		"bit",
		"jit",
		"re",
		"unicode",
		"lfs",
		"loaded",
		"preload",
	}};
	return std::find(names.begin(), names.end(), variable_name) != names.end();
}

size_t ChildCaptureLimit(std::string_view variable_name, bool runtime_global_table)
{
	if (runtime_global_table)
		return std::numeric_limits<size_t>::max();
	if (ShouldExpandRuntimeLibraryTable(variable_name))
		return kExpandedRuntimeTableChildren;
	return kMaxVariableChildren;
}

std::string BuildStructuredTablePreview(lua_State *L, int table_index, char const* variable_name, std::string& value_type)
{
	auto table_class = TryGetTableStringField(L, table_index, "class");
	if (!table_class) {
		if (TableHasField(L, table_index, "res_x") && TableHasField(L, table_index, "res_y")) {
			value_type = "karaskel-meta";
			return BuildKaraskelMetaPreview(L, table_index);
		}
		if (TableHasField(L, table_index, "styleref") && TableHasField(L, table_index, "text_stripped")) {
			value_type = "kara-line";
			return BuildKaraskelLinePreview(L, table_index);
		}
		if (TableHasField(L, table_index, "highlights") && TableHasField(L, table_index, "duration")) {
			value_type = TryGetTableBooleanField(L, table_index, "isfuri").value_or(false) ? "kara-furi" : "kara-syllable";
			return BuildKaraskelSyllablePreview(L, table_index);
		}
		if (TableHasField(L, table_index, "n")) {
			lua_rawgeti(L, table_index, 1);
			if (lua_isnil(L, -1)) {
				lua_pop(L, 1);
				lua_rawgeti(L, table_index, 0);
			}
			bool const first_is_style = lua_istable(L, -1) && TryGetTableStringField(L, -1, "class").value_or("") == "style";
			bool const first_is_highlight = lua_istable(L, -1) && TableHasField(L, -1, "start_time") && !TableHasField(L, -1, "text");
			bool const first_is_syllable = lua_istable(L, -1) && TableHasField(L, -1, "duration") && TableHasField(L, -1, "text_stripped");
			lua_pop(L, 1);
			if (first_is_style) {
				value_type = "style-map";
				return BuildKaraskelCollectionPreview(L, table_index, "styles");
			}
			if (first_is_highlight) {
				value_type = "highlight-array";
				return BuildKaraskelCollectionPreview(L, table_index, "highlights");
			}
			if (first_is_syllable) {
				value_type = "kara-array";
				return BuildKaraskelCollectionPreview(L, table_index, "kara");
			}
		}
		return FormatTableName(L, table_index);
	}

	value_type = *table_class;
	if (*table_class == "dialogue")
		return BuildDialogueTablePreview(L, table_index);
	if (*table_class == "style")
		return BuildStyleTablePreview(L, table_index);
	if (*table_class == "info")
		return BuildInfoTablePreview(L, table_index);
	return *table_class + "-table";
}

bool ShouldHideDebugLocal(char const* name)
{
	return name && name[0] == '(';
}

bool TryGetMetatableDebugType(lua_State *L, int index, std::string& debug_type)
{
	index = AbsoluteIndex(L, index);
	if (!lua_getmetatable(L, index))
		return false;

	RawGetTableField(L, -1, "__aegisub_debug_type");
	bool const ok = lua_isstring(L, -1);
	if (ok)
		debug_type = get_string_or_default(L, -1);
	lua_pop(L, 2);
	return ok;
}

std::string FunctionNameFor(lua_Debug const& ar);
AutomationLuaDebugBackend *LoadBackend(lua_State *L);

std::string DescribeFunctionWhat(char const* what)
{
	if (!what || !*what)
		return {};
	switch (what[0]) {
	case 'C':
		return "c-function";
	case 'L':
		return "lua-function";
	case 'm':
		return "main-chunk";
	case 't':
		return "tail-call";
	default:
		break;
	}
	if (std::strcmp(what, "main") == 0)
		return "main-chunk";
	if (std::strcmp(what, "tail") == 0)
		return "tail-call";
	if (std::strcmp(what, "Lua") == 0)
		return "lua-function";
	if (std::strcmp(what, "C") == 0)
		return "c-function";
	return what;
}

std::string DescribeFunctionNameWhat(char const* namewhat)
{
	if (!namewhat || !*namewhat)
		return {};
	if (std::strcmp(namewhat, "global") == 0)
		return "global";
	if (std::strcmp(namewhat, "local") == 0)
		return "local";
	if (std::strcmp(namewhat, "method") == 0)
		return "method";
	if (std::strcmp(namewhat, "field") == 0)
		return "field";
	if (std::strcmp(namewhat, "upvalue") == 0)
		return "upvalue";
	return namewhat;
}

AutomationLuaDebugBackend *LoadBackend(lua_State *L)
{
	lua_getfield(L, LUA_REGISTRYINDEX, kLuaDebugBackendRegistryKey);
	auto *backend = static_cast<AutomationLuaDebugBackend *>(lua_touserdata(L, -1));
	lua_pop(L, 1);
	return backend;
}

void StoreBackend(lua_State *L, AutomationLuaDebugBackend *backend)
{
	if (backend)
		lua_pushlightuserdata(L, backend);
	else
		lua_pushnil(L);
	lua_setfield(L, LUA_REGISTRYINDEX, kLuaDebugBackendRegistryKey);
}

std::string SanitizeUriSegment(std::string value)
{
	for (char& ch : value) {
		if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.')
			continue;
		ch = '_';
	}
	if (value.empty())
		return "template";
	return value;
}

std::string BuildTemplateSourcePath(AutomationTemplateDebugState const& state, agi::fs::path const& script_file)
{
	std::string owner = agi::fs::PathToString(script_file.filename());
	std::string fragment_kind = state.kind.value_or("template");
	std::string debug_id = "runtime";

	if (state.identity) {
		if (state.identity->owner_script && !state.identity->owner_script->empty())
			owner = *state.identity->owner_script;
		if (state.identity->fragment_kind && !state.identity->fragment_kind->empty())
			fragment_kind = *state.identity->fragment_kind;
		else if (state.identity->template_kind && !state.identity->template_kind->empty())
			fragment_kind = *state.identity->template_kind;
		if (state.identity->template_debug_id)
			debug_id = std::to_string(*state.identity->template_debug_id);
	}
	else if (state.kind && !state.kind->empty()) {
		fragment_kind = *state.kind;
	}

	return "aegisub-template://" + SanitizeUriSegment(owner) + "/" + debug_id + "/" + SanitizeUriSegment(fragment_kind) + ".lua";
}

std::string BuildTemplateDisplayName(AutomationTemplateDebugState const& state)
{
	std::string label = state.kind.value_or("template");
	if (state.identity && state.identity->template_debug_id)
		label += " #" + std::to_string(*state.identity->template_debug_id);
	return label;
}

AutomationDebugVariable CaptureVariable(lua_State *L, char const* name, int value_index, int depth, LuaVariableCaptureState& state);

AutomationDebugVariable MakeScalarVariable(
	std::string name,
	std::string value,
	std::string value_type)
{
	AutomationDebugVariable variable;
	variable.name = std::move(name);
	variable.value = std::move(value);
	variable.value_type = std::move(value_type);
	return variable;
}

std::string FormatDebugTableKeyName(lua_State *L, int key_index)
{
	switch (lua_type(L, key_index)) {
	case LUA_TSTRING:
		return get_string_or_default(L, key_index);
	case LUA_TNUMBER:
		return "[" + std::to_string(lua_tointeger(L, key_index)) + "]";
	default:
		return "[" + std::string(lua_typename(L, lua_type(L, key_index))) + "]";
	}
}

bool IsKnownRuntimeGlobalName(std::string_view name)
{
	static constexpr std::array<std::string_view, 48> runtime_names{{
		"_G",
		"_VERSION",
		"aegisub",
		"arg",
		"assert",
		"bit",
		"collectgarbage",
		"coroutine",
		"debug",
		"dofile",
		"error",
		"ffi",
		"gcinfo",
		"getfenv",
		"getmetatable",
		"include",
		"io",
		"ipairs",
		"jit",
		"lfs",
		"load",
		"loadfile",
		"loadstring",
		"math",
		"module",
		"newproxy",
		"next",
		"os",
		"package",
		"pairs",
		"pcall",
		"print",
		"rawequal",
		"rawget",
		"rawset",
		"re",
		"require",
		"select",
		"setfenv",
		"setmetatable",
		"string",
		"table",
		"tonumber",
		"tostring",
		"type",
		"unicode",
		"unpack",
		"xpcall",
	}};

	return std::find(runtime_names.begin(), runtime_names.end(), name) != runtime_names.end();
}

bool IsRuntimeGlobalTable(lua_State *L, int table_index, std::string const& variable_name)
{
	(void)L;
	(void)table_index;
	return variable_name == "_G";
}

bool ShouldIncludeRuntimeGlobalTableChild(lua_State *L, std::string const& child_name)
{
	if (child_name.empty() || child_name == "_G")
		return false;

	if (auto *backend = LoadBackend(L))
		return backend->IsRuntimeGlobal(child_name);
	return IsKnownRuntimeGlobalName(child_name);
}

std::string NormalizeDebugName(char const* name, char const* fallback_prefix, int index = 0)
{
	if (name && *name)
		return name;
	if (index > 0)
		return std::string(fallback_prefix) + " " + std::to_string(index);
	return fallback_prefix;
}

void CaptureTableChildren(lua_State *L, int table_index, AutomationDebugVariable& variable, int depth, LuaVariableCaptureState& state);
void CaptureLuaAssFileChildren(lua_State *L, int value_index, AutomationDebugVariable& variable, int depth, LuaVariableCaptureState& state);
void CaptureFunctionChildren(lua_State *L, int value_index, AutomationDebugVariable& variable, int depth, LuaVariableCaptureState& state);
std::optional<AutomationDebugScope> CaptureRegistryScope(lua_State *L, char const* registry_key, char const* scope_name);

AutomationDebugVariable CaptureVariable(lua_State *L, char const* name, int value_index, int depth, LuaVariableCaptureState& state)
{
	value_index = AbsoluteIndex(L, value_index);

	auto variable = AutomationDebugVariable{};
	variable.name = name ? name : "(anonymous)";

	switch (lua_type(L, value_index)) {
	case LUA_TNIL:
		variable.value = "nil";
		variable.value_type = "nil";
		break;
	case LUA_TBOOLEAN:
		variable.value = lua_toboolean(L, value_index) ? "true" : "false";
		variable.value_type = "boolean";
		break;
	case LUA_TNUMBER:
		variable.value = FormatNumberPreview(L, value_index, variable.value_type);
		break;
	case LUA_TSTRING:
		variable.value = FormatStringPreview(L, value_index);
		variable.value_type = "string";
		break;
	case LUA_TTABLE: {
		variable.value_type = "table";
		variable.value = BuildStructuredTablePreview(L, value_index, variable.name.c_str(), variable.value_type);
		auto const* pointer = lua_topointer(L, value_index);
		if (depth < kMaxVariableDepth && pointer && state.active_tables.insert(pointer).second) {
			CaptureTableChildren(L, value_index, variable, depth + 1, state);
			state.active_tables.erase(pointer);
		}
		break;
	}
	case LUA_TFUNCTION:
		variable.value_type = "function";
		CaptureFunctionChildren(L, value_index, variable, depth + 1, state);
		break;
	case LUA_TUSERDATA: {
		std::string debug_type;
		if (TryGetMetatableDebugType(L, value_index, debug_type) && debug_type == "LuaAssFile") {
			CaptureLuaAssFileChildren(L, value_index, variable, depth + 1, state);
			break;
		}
		variable.value = "userdata:" + FormatPointer(lua_topointer(L, value_index));
		variable.value_type = "userdata";
		break;
	}
	case LUA_TLIGHTUSERDATA:
		variable.value = "lightuserdata:" + FormatPointer(lua_topointer(L, value_index));
		variable.value_type = "lightuserdata";
		break;
	case LUA_TTHREAD:
		variable.value = "thread:" + FormatPointer(lua_topointer(L, value_index));
		variable.value_type = "thread";
		break;
	default:
		variable.value = lua_typename(L, lua_type(L, value_index));
		variable.value_type = variable.value;
		break;
	}

	return variable;
}

void AddOverflowChild(AutomationDebugVariable& variable, size_t hidden_count)
{
	if (hidden_count == 0)
		return;
	variable.children.push_back(MakeScalarVariable(
		"...",
		std::to_string(hidden_count) + " more item(s)",
		"summary"));
}

void CaptureTableChildren(lua_State *L, int table_index, AutomationDebugVariable& variable, int depth, LuaVariableCaptureState& state)
{
	table_index = AbsoluteIndex(L, table_index);
	bool const runtime_global_table = IsRuntimeGlobalTable(L, table_index, variable.name);
	size_t const child_limit = ChildCaptureLimit(variable.name, runtime_global_table);
	bool const unlimited_children = child_limit == std::numeric_limits<size_t>::max();
	size_t captured = 0;
	size_t hidden = 0;
	std::vector<AutomationDebugVariable> associative_children;

	size_t const array_length = lua_objlen(L, table_index);
	size_t const numeric_limit = unlimited_children
		? array_length
		: std::min(array_length, child_limit);
	for (size_t i = 1; i <= numeric_limit; ++i) {
		lua_rawgeti(L, table_index, static_cast<int>(i));
		variable.children.push_back(CaptureVariable(
			L,
			("[" + std::to_string(i) + "]").c_str(),
			-1,
			depth,
			state));
		lua_pop(L, 1);
		++captured;
	}
	if (array_length > numeric_limit)
		hidden += array_length - numeric_limit;

	lua_pushnil(L);
	while (lua_next(L, table_index) != 0) {
		bool skip = false;
		std::string child_name;
		switch (lua_type(L, -2)) {
		case LUA_TSTRING:
			child_name = get_string_or_default(L, -2);
			break;
		case LUA_TNUMBER: {
			auto const key = lua_tonumber(L, -2);
			auto const integer = lua_tointeger(L, -2);
			if (static_cast<lua_Number>(integer) == key && integer >= 1 && static_cast<size_t>(integer) <= array_length)
				skip = true;
			else
				child_name = "[" + std::to_string(integer) + "]";
			break;
		}
		default:
			child_name = "[" + std::string(lua_typename(L, lua_type(L, -2))) + "]";
			break;
		}

		if (runtime_global_table && !ShouldIncludeRuntimeGlobalTableChild(L, child_name)) {
			lua_pop(L, 1);
			continue;
		}

		if (skip) {
			lua_pop(L, 1);
			continue;
		}

		if (!unlimited_children && captured >= child_limit) {
			++hidden;
			lua_pop(L, 1);
			continue;
		}

		associative_children.push_back(CaptureVariable(L, child_name.c_str(), -1, depth, state));
		lua_pop(L, 1);
		if (!unlimited_children)
			++captured;
	}

	std::sort(associative_children.begin(), associative_children.end(), [&](auto const& a, auto const& b) {
		int const priority_a = KnownTableChildPriority(variable.name, a.name);
		int const priority_b = KnownTableChildPriority(variable.name, b.name);
		if (priority_a != priority_b)
			return priority_a < priority_b;
		return a.name < b.name;
	});
	if (runtime_global_table) {
		for (auto& child : associative_children)
			variable.children.push_back(std::move(child));
		captured += associative_children.size();
	}
	else {
		for (auto& child : associative_children)
			variable.children.push_back(std::move(child));
	}

	if (depth <= kMaxVariableDepth && lua_getmetatable(L, table_index)) {
		if (unlimited_children || captured < child_limit) {
			variable.children.push_back(CaptureVariable(L, "__metatable", -1, depth, state));
			if (!unlimited_children)
				++captured;
		}
		else {
			++hidden;
		}
		lua_pop(L, 1);
	}

	AddOverflowChild(variable, hidden);
}

void CaptureLuaAssFileChildren(lua_State *L, int value_index, AutomationDebugVariable& variable, int depth, LuaVariableCaptureState& state)
{
	auto *ass_file = LuaAssFile::GetObjPointer(L, value_index, true);
	size_t const line_count = ass_file ? ass_file->DebugLineCount() : 0;
	size_t const info_count = ass_file ? ass_file->DebugInfoCount() : 0;
	size_t const style_count = ass_file ? ass_file->DebugStyleCount() : 0;
	size_t const dialogue_count = ass_file ? ass_file->DebugDialogueCount() : 0;

	std::ostringstream summary;
	summary << "subtitle-file[n=" << line_count;
	if (style_count != 0 || dialogue_count != 0 || info_count != 0)
		summary << ", info=" << info_count << ", style=" << style_count << ", dialogue=" << dialogue_count;
	summary << "]";
	variable.value = summary.str();
	variable.value_type = "subtitle-file";
	variable.children.push_back(MakeScalarVariable("n", std::to_string(line_count), "integer"));
	variable.children.push_back(MakeScalarVariable("info_count", std::to_string(info_count), "integer"));
	variable.children.push_back(MakeScalarVariable("style_count", std::to_string(style_count), "integer"));
	variable.children.push_back(MakeScalarVariable("dialogue_count", std::to_string(dialogue_count), "integer"));
	if (ass_file) {
		variable.children.push_back(MakeScalarVariable("can_modify", ass_file->DebugCanModify() ? "true" : "false", "boolean"));
		variable.children.push_back(MakeScalarVariable("can_set_undo", ass_file->DebugCanSetUndo() ? "true" : "false", "boolean"));
		variable.children.push_back(MakeScalarVariable("pending_commits", std::to_string(ass_file->DebugPendingCommitCount()), "integer"));
		variable.children.push_back(MakeScalarVariable("has_pending_modifications", ass_file->DebugHasPendingModifications() ? "true" : "false", "boolean"));
	}

	auto runtime_snapshot = LuaGetAutomationRuntimeStateSnapshot(L);
	if (!runtime_snapshot)
		return;

	auto const& selection = runtime_snapshot->context_snapshot.selection;
	variable.children.push_back(MakeScalarVariable("active_row", std::to_string(selection.active_row), "integer"));

	AutomationDebugVariable selected_rows;
	selected_rows.name = "selected_rows";
	selected_rows.value = "table[" + std::to_string(selection.selected_rows.size()) + "]";
	selected_rows.value_type = "table";
	for (size_t i = 0; i < selection.selected_rows.size(); ++i) {
		selected_rows.children.push_back(MakeScalarVariable(
			"[" + std::to_string(i + 1) + "]",
			std::to_string(selection.selected_rows[i]),
			"integer"));
	}
	variable.children.push_back(std::move(selected_rows));

	if (depth > kMaxVariableDepth || !ass_file)
		return;

	std::set<int> preview_rows;
	if (selection.active_row > 0)
		preview_rows.insert(selection.active_row);
	for (int row : selection.selected_rows) {
		preview_rows.insert(row);
		if (preview_rows.size() >= kMaxSubsPreviewRows)
			break;
	}

	for (int row : preview_rows) {
		if (!ass_file->DebugTryPushLineAsLua(L, static_cast<size_t>(row)))
			continue;
		variable.children.push_back(CaptureVariable(
			L,
			("line[" + std::to_string(row) + "]").c_str(),
			-1,
			depth,
			state));
		lua_pop(L, 1);
	}
}

void CaptureFunctionChildren(lua_State *L, int value_index, AutomationDebugVariable& variable, int depth, LuaVariableCaptureState& state)
{
	value_index = AbsoluteIndex(L, value_index);
	lua_pushvalue(L, value_index);
	lua_Debug ar{};
	if (!lua_getinfo(L, ">Snu", &ar)) {
		lua_pop(L, 1);
		return;
	}

	auto function_name = FunctionNameFor(ar);
	auto binding_name = variable.name;
	bool const has_binding_name = !binding_name.empty() && binding_name != "(anonymous)";
	bool const function_name_is_anonymous = function_name.empty() || function_name == "(anonymous)";
	auto location = AutomationDebugLocation{};
	location.line = ar.currentline > 0 ? ar.currentline : ar.linedefined;
	location.column = 0;
	auto raw_source = std::string(ar.source ? ar.source : "");
	location.source_path = NormalizeAutomationDebugSource(raw_source);
	auto const extension = agi::fs::PathToString(agi::fs::PathFromString(location.source_path).extension());
	bool const looks_like_script_file =
		!location.source_path.empty() &&
		(extension == ".lua" || extension == ".moon");
	location.source_kind = ((!raw_source.empty() && raw_source.front() == '@') || looks_like_script_file) ? "script" : "chunk";
	location.display_name = ar.short_src ? ar.short_src : "";
	if (location.source_kind == "script" && !location.source_path.empty())
		location.display_name = agi::fs::PathToString(agi::fs::PathFromString(location.source_path).filename());
	std::ostringstream preview;
	preview << ((ar.what && std::strcmp(ar.what, "C") == 0) ? "cfunction" : "function");
	if (!function_name_is_anonymous)
		preview << " " << function_name;
	else if (has_binding_name)
		preview << " " << binding_name;
	if (ar.linedefined > 0 && !location.display_name.empty())
		preview << " @" << location.display_name << ":" << ar.linedefined;
	else if (!location.display_name.empty())
		preview << " @" << location.display_name;
	else
		preview << ":" << FormatPointer(lua_topointer(L, value_index));
	variable.value = preview.str();

	variable.children.push_back(MakeScalarVariable("function_name", function_name, "string"));
	if (function_name_is_anonymous && has_binding_name)
		variable.children.push_back(MakeScalarVariable("binding_name", binding_name, "string"));
	if (auto function_kind = DescribeFunctionWhat(ar.what); !function_kind.empty())
		variable.children.push_back(MakeScalarVariable("what", function_kind, "string"));
	if (auto name_kind = DescribeFunctionNameWhat(ar.namewhat); !name_kind.empty())
		variable.children.push_back(MakeScalarVariable("namewhat", name_kind, "string"));
	variable.children.push_back(MakeScalarVariable("source_path", location.source_path, "string"));
	if (!location.display_name.empty())
		variable.children.push_back(MakeScalarVariable("display_name", location.display_name, "string"));
	variable.children.push_back(MakeScalarVariable("source_kind", location.source_kind, "string"));
	if (ar.linedefined >= 0)
		variable.children.push_back(MakeScalarVariable("linedefined", std::to_string(ar.linedefined), "integer"));
	if (ar.lastlinedefined >= 0)
		variable.children.push_back(MakeScalarVariable("lastlinedefined", std::to_string(ar.lastlinedefined), "integer"));
	if (ar.nups > 0)
		variable.children.push_back(MakeScalarVariable("upvalue_count", std::to_string(ar.nups), "integer"));

	if (depth <= kMaxVariableDepth && ar.nups > 0) {
		lua_pushvalue(L, value_index);
		AutomationDebugVariable upvalues;
		upvalues.name = "upvalues";
		upvalues.value = "table[" + std::to_string(ar.nups) + "]";
		upvalues.value_type = "table";
		for (int upvalue_index = 1; upvalue_index <= ar.nups; ++upvalue_index) {
			char const* upvalue_name = lua_getupvalue(L, -1, upvalue_index);
			if (!upvalue_name)
				break;
			auto upvalue_label = NormalizeDebugName(upvalue_name, "upvalue", upvalue_index);
			upvalues.children.push_back(CaptureVariable(L, upvalue_label.c_str(), -1, depth, state));
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
		variable.children.push_back(std::move(upvalues));
	}
}

AutomationDebugVariable BuildFrameInfoVariable(lua_Debug const& ar, AutomationDebugFrame const& frame)
{
	AutomationDebugVariable variable;
	variable.name = "__frame";
	variable.value = frame.function_name;
	variable.value_type = "frame";
	variable.children.push_back(MakeScalarVariable("function_name", frame.function_name, "string"));
	variable.children.push_back(MakeScalarVariable("kind", frame.kind, "string"));
	variable.children.push_back(MakeScalarVariable("source_path", frame.location.source_path, "string"));
	variable.children.push_back(MakeScalarVariable("source_kind", frame.location.source_kind, "string"));
	variable.children.push_back(MakeScalarVariable("line", std::to_string(frame.location.line), "integer"));
	variable.children.push_back(MakeScalarVariable("column", std::to_string(frame.location.column), "integer"));
	if (auto function_kind = DescribeFunctionWhat(ar.what); !function_kind.empty())
		variable.children.push_back(MakeScalarVariable("what", function_kind, "string"));
	if (auto name_kind = DescribeFunctionNameWhat(ar.namewhat); !name_kind.empty())
		variable.children.push_back(MakeScalarVariable("namewhat", name_kind, "string"));
	if (ar.linedefined >= 0)
		variable.children.push_back(MakeScalarVariable("linedefined", std::to_string(ar.linedefined), "integer"));
	if (ar.lastlinedefined >= 0)
		variable.children.push_back(MakeScalarVariable("lastlinedefined", std::to_string(ar.lastlinedefined), "integer"));
	if (ar.nups > 0)
		variable.children.push_back(MakeScalarVariable("upvalue_count", std::to_string(ar.nups), "integer"));
	return variable;
}

bool PushNamedGlobal(lua_State *L, char const* name)
{
	lua_getfield(L, LUA_GLOBALSINDEX, name);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		return false;
	}
	return true;
}

std::set<std::string> CollectRuntimeNamesVisibleInEnvironmentRoot(lua_State *L)
{
	std::set<std::string> names;
	if (!PushNamedGlobal(L, "_G"))
		return names;
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return names;
	}

	int const table_index = AbsoluteIndex(L, -1);
	lua_pushnil(L);
	while (lua_next(L, table_index) != 0) {
		auto child_name = FormatDebugTableKeyName(L, -2);
		if (ShouldIncludeRuntimeGlobalTableChild(L, child_name))
			names.insert(std::move(child_name));
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return names;
}

void SortRankedGlobals(std::vector<RankedGlobalVariable>& values)
{
	std::sort(values.begin(), values.end(), [](auto const& a, auto const& b) {
		if (a.function != b.function)
			return !a.function;
		return a.variable.name < b.variable.name;
	});
}

std::vector<AutomationDebugVariable> FinalizeGlobalVariables(
	std::vector<RankedGlobalVariable> values,
	size_t max_visible = kMaxGlobalVariables)
{
	SortRankedGlobals(values);

	std::vector<AutomationDebugVariable> globals;
	size_t const visible_count = max_visible == 0
		? values.size()
		: std::min(values.size(), max_visible);
	globals.reserve(visible_count + 1);
	for (size_t i = 0; i < visible_count; ++i)
		globals.push_back(std::move(values[i].variable));

	if (max_visible != 0 && values.size() > visible_count)
		globals.push_back(MakeScalarVariable(
			"...",
			std::to_string(values.size() - visible_count) + " more item(s)",
			"summary"));

	return globals;
}

CapturedGlobalScopes CaptureGlobalVariables(lua_State *L, LuaVariableCaptureState& state)
{
	auto *backend = LoadBackend(L);
	auto runtime_names_in_environment_root = CollectRuntimeNamesVisibleInEnvironmentRoot(L);
	std::vector<RankedGlobalVariable> script_globals;
	std::vector<RankedGlobalVariable> script_functions;
	std::vector<RankedGlobalVariable> runtime_globals;
	script_globals.reserve(64);
	script_functions.reserve(64);
	runtime_globals.reserve(64);

	lua_pushnil(L);
	while (lua_next(L, LUA_GLOBALSINDEX) != 0) {
		auto global_name = FormatDebugTableKeyName(L, -2);
		auto variable = CaptureVariable(L, global_name.c_str(), -1, 0, state);
		lua_pop(L, 1);

		RankedGlobalVariable entry;
		entry.function = variable.value_type == "function";
		entry.variable = std::move(variable);

		bool const runtime_global = backend
			? backend->IsRuntimeGlobal(global_name)
			: IsKnownRuntimeGlobalName(global_name);

		if (runtime_global) {
			if (global_name == "_G")
				runtime_globals.push_back(std::move(entry));
			else if (runtime_names_in_environment_root.find(global_name) == runtime_names_in_environment_root.end())
				runtime_globals.push_back(std::move(entry));
		}
		else if (entry.function)
			script_functions.push_back(std::move(entry));
		else
			script_globals.push_back(std::move(entry));
	}

	return {
		FinalizeGlobalVariables(std::move(script_globals), 0),
		FinalizeGlobalVariables(std::move(script_functions), 0),
		FinalizeGlobalVariables(std::move(runtime_globals))
	};
}

std::vector<AutomationDebugScope> CaptureLuaScopes(lua_State *L)
{
	std::vector<AutomationDebugScope> scopes;
	LuaVariableCaptureState state;

	auto globals = CaptureGlobalVariables(L, state);
	if (!globals.globals.empty()) {
		AutomationDebugScope globals_scope;
		globals_scope.name = "Globals";
		globals_scope.variables = std::move(globals.globals);
		scopes.push_back(std::move(globals_scope));
	}
	if (!globals.functions.empty()) {
		AutomationDebugScope functions_scope;
		functions_scope.name = "Functions";
		functions_scope.variables = std::move(globals.functions);
		scopes.push_back(std::move(functions_scope));
	}
	if (!globals.runtime_globals.empty()) {
		AutomationDebugScope runtime_globals_scope;
		runtime_globals_scope.name = "Runtime Globals";
		runtime_globals_scope.variables = std::move(globals.runtime_globals);
		scopes.push_back(std::move(runtime_globals_scope));
	}

	if (auto progress_scope = CaptureRegistryScope(L, kAutomationDebugProgressRegistryKey, "Progress"))
		scopes.push_back(std::move(*progress_scope));
	if (auto dialog_scope = CaptureRegistryScope(L, kAutomationDebugDialogRegistryKey, "Dialog"))
		scopes.push_back(std::move(*dialog_scope));

	return scopes;
}

std::optional<AutomationDebugScope> CaptureRegistryScope(lua_State *L, char const* registry_key, char const* scope_name)
{
	LuaVariableCaptureState state;
	lua_getfield(L, LUA_REGISTRYINDEX, registry_key);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return std::nullopt;
	}

	auto root = CaptureVariable(L, scope_name, -1, 0, state);
	lua_pop(L, 1);
	if (root.children.empty())
		return std::nullopt;

	AutomationDebugScope scope;
	scope.name = scope_name;
	scope.variables = std::move(root.children);
	return scope;
}

std::string FrameKindFor(lua_Debug const& ar, AutomationDebugLocation const& location)
{
	if (ar.what && std::strcmp(ar.what, "C") == 0)
		return "host-frame";
	if (location.source_kind == "template")
		return "template-frame";
	return "script-frame";
}

std::string FunctionNameFor(lua_Debug const& ar)
{
	if (ar.name && *ar.name)
		return ar.name;
	if (ar.what && std::strcmp(ar.what, "main") == 0)
		return "main";
	return "(anonymous)";
}

}

AutomationLuaDebugBackend::AutomationLuaDebugBackend(lua_State *L, agi::fs::path script_file)
: L(L)
, script_file(std::move(script_file))
{
	StoreBackend(L, this);
}

AutomationLuaDebugBackend::~AutomationLuaDebugBackend()
{
	if (!L)
		return;
	lua_sethook(L, nullptr, 0, 0);
	StoreBackend(L, nullptr);
}

void AutomationLuaDebugBackend::CaptureRuntimeBaseline()
{
	runtime_globals_baseline.clear();
	if (!L)
		return;

	lua_pushnil(L);
	while (lua_next(L, LUA_GLOBALSINDEX) != 0) {
		runtime_globals_baseline.insert(FormatDebugTableKeyName(L, -2));
		lua_pop(L, 1);
	}
}

bool AutomationLuaDebugBackend::IsRuntimeGlobal(std::string_view name) const
{
	if (runtime_globals_baseline.find(std::string(name)) != runtime_globals_baseline.end())
		return true;
	return IsKnownRuntimeGlobalName(name);
}

void AutomationLuaDebugBackend::OnDebugStateChanged()
{
	UpdateHookState();
}

void AutomationLuaDebugBackend::SetWorkspaceRunRequest(std::shared_ptr<LuaWorkspaceRunRequest const> request) {
	workspace_request = std::move(request);
	UpdateHookState();
}

void AutomationLuaDebugBackend::InstallSetHookGuard() {
	lua_getglobal(L, "debug");
	lua_getfield(L, -1, "sethook");
	original_sethook = lua_tocfunction(L, -1);
	lua_pop(L, 1);
	if (!original_sethook) {
		lua_pop(L, 1);
		throw AutomationError("Lua debug.sethook is unavailable");
	}
	lua_pushcfunction(L, &AutomationLuaDebugBackend::GuardedSetHook);
	lua_setfield(L, -2, "sethook");
	lua_pop(L, 1);
}

int AutomationLuaDebugBackend::GuardedSetHook(lua_State *L) {
	auto *backend = LoadBackend(L);
	if (backend && backend->workspace_request)
		return luaL_error(L, "debug.sethook cannot replace the Lua Workspace cancellation hook");
	if (!backend || !backend->original_sethook)
		return luaL_error(L, "Lua debug.sethook is unavailable");
	int nargs = lua_gettop(L);
	lua_pushcfunction(L, backend->original_sethook);
	lua_insert(L, 1);
	lua_call(L, nargs, LUA_MULTRET);
	return lua_gettop(L);
}

void AutomationLuaDebugBackend::UpdateHookState()
{
	if (!L)
		return;

	if (workspace_request)
		lua_sethook(L, &AutomationLuaDebugBackend::Hook, LUA_MASKLINE | LUA_MASKCOUNT, 1000);
	else if (DebugActive())
		lua_sethook(L, &AutomationLuaDebugBackend::Hook, LUA_MASKLINE, 0);
	else
		lua_sethook(L, nullptr, 0, 0);
}

AutomationDebugLocation AutomationLuaDebugBackend::BuildLocation(lua_State *hook_L, lua_Debug const& ar) const
{
	AutomationDebugLocation location;
	location.line = ar.currentline > 0 ? ar.currentline : ar.linedefined;
	location.column = 0;

	std::string source = ar.source ? ar.source : "";
	if (workspace_request) {
		if (auto recorded = workspace_request->sources->Find(source)) {
			location.source_path = recorded->uri;
			location.display_name = recorded->display_name;
			location.source_kind = recorded->uri.starts_with("aegisub-workspace://") ? "template" : "script";
			return location;
		}
		location.source_path = NormalizeAutomationDebugSource(source);
		location.display_name = ar.short_src;
		location.source_kind = "chunk";
		return location;
	}
	location.source_path = NormalizeAutomationDebugSource(source);
	location.display_name = ar.short_src ? ar.short_src : "";
	auto runtime_snapshot = LuaGetAutomationRuntimeStateSnapshot(hook_L);

	auto const script_source = NormalizeAutomationDebugSource(agi::fs::PathToString(script_file));
	auto const extension = agi::fs::PathToString(agi::fs::PathFromString(location.source_path).extension());
	bool const looks_like_script_file =
		location.source_path == script_source ||
		extension == ".lua" ||
		extension == ".moon";

	if ((!source.empty() && source.front() == '@') || looks_like_script_file)
		location.source_kind = "script";
	else {
		if (runtime_snapshot && runtime_snapshot->template_debug) {
			location.source_kind = "template";
			location.source_path = BuildTemplateSourcePath(*runtime_snapshot->template_debug, script_file);
			location.display_name = BuildTemplateDisplayName(*runtime_snapshot->template_debug);
		}
		else {
			location.source_kind = "chunk";
			if (location.source_path.empty())
				location.source_path = location.display_name;
		}
	}

	if (location.source_kind == "script" && !location.source_path.empty())
		location.display_name = agi::fs::PathToString(agi::fs::PathFromString(location.source_path).filename());
	else if (location.display_name.empty())
		location.display_name = agi::fs::PathToString(script_file.filename());

	return location;
}

size_t AutomationLuaDebugBackend::CaptureStackDepth(lua_State *hook_L) const {
	size_t depth = 0;
	lua_Debug ar;
	while (lua_getstack(hook_L, static_cast<int>(depth), &ar))
		++depth;
	return depth;
}

std::vector<AutomationDebugFrame> AutomationLuaDebugBackend::CaptureFrames(lua_State *hook_L) const {
	std::vector<AutomationDebugFrame> frames;
	LuaVariableCaptureState state;

	for (int level = 0;; ++level) {
		lua_Debug ar;
		if (!lua_getstack(hook_L, level, &ar))
			break;

		lua_getinfo(hook_L, "nSlu", &ar);

		AutomationDebugFrame frame;
		frame.level = level;
		frame.location = BuildLocation(hook_L, ar);
		frame.kind = FrameKindFor(ar, frame.location);
		frame.function_name = FunctionNameFor(ar);

		for (int local_index = 1;; ++local_index) {
			char const *local_name = lua_getlocal(hook_L, &ar, local_index);
			if (!local_name)
				break;
			if (!ShouldHideDebugLocal(local_name))
				frame.locals.push_back(CaptureVariable(hook_L, local_name, -1, 0, state));
			lua_pop(hook_L, 1);
		}

		if (lua_getinfo(hook_L, "f", &ar) != 0 && lua_isfunction(hook_L, -1)) {
			for (int upvalue_index = 1;; ++upvalue_index) {
				char const *upvalue_name = lua_getupvalue(hook_L, -1, upvalue_index);
				if (!upvalue_name)
					break;
				if (!ShouldHideDebugLocal(upvalue_name)) {
					auto upvalue_label = NormalizeDebugName(upvalue_name, "upvalue", upvalue_index);
					frame.upvalues.push_back(CaptureVariable(hook_L, upvalue_label.c_str(), -1, 0, state));
				}
				lua_pop(hook_L, 1);
			}
			lua_pop(hook_L, 1);
		}

		frame.locals.push_back(BuildFrameInfoVariable(ar, frame));

		frames.push_back(std::move(frame));
	}

	return frames;
}

void AutomationLuaDebugBackend::Hook(lua_State *L, lua_Debug *ar) {
	if (auto *backend = LoadBackend(L))
		backend->OnHook(L, ar);
}

void AutomationLuaDebugBackend::OnHook(lua_State *hook_L, lua_Debug *ar) {
	if (workspace_request && workspace_request->stop_requested->load())
		LuaRaiseWorkspaceCancellation(hook_L);
	auto *session = GetSession();
	if (!session || !InvocationActive() || !ar || ar->event != LUA_HOOKLINE)
		return;

	{
		lua_getinfo(hook_L, "nSlu", ar);
		auto location = BuildLocation(hook_L, *ar);
		session->HandleHookPause(
			std::move(location),
			CaptureStackDepth(hook_L),
			[this, hook_L] {
				AutomationDebugCapturedState captured;
				captured.frames = CaptureFrames(hook_L);
				captured.scopes = CaptureLuaScopes(hook_L);
				captured.runtime_snapshot = LuaGetAutomationRuntimeStateSnapshot(hook_L);
				return captured;
			});
	}
	if (workspace_request && workspace_request->stop_requested->load())
		LuaRaiseWorkspaceCancellation(hook_L);
}
}
