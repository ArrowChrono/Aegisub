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

/// @file auto4_lua_dialog.cpp
/// @brief Lua 5.1-based scripting engine (configuration-dialogue interface)
/// @ingroup scripting
///

#include "auto4_lua.h"

#include "string_codec.h"

#include <libaegisub/log.h>
#include <libaegisub/lua/utils.h>
#include <libaegisub/split.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <type_traits>
#include <unordered_map>
#include <utility>

using namespace agi::lua;

namespace {
void get_if_right_type(lua_State *L, std::string& value) {
	if (lua_isstring(L, -1)) value = lua_tostring(L, -1);
}

void get_if_right_type(lua_State *L, double& value) {
	if (lua_isnumber(L, -1)) value = lua_tonumber(L, -1);
}

void get_if_right_type(lua_State *L, int& value) {
	if (lua_isnumber(L, -1)) value = lua_tointeger(L, -1);
}

void get_if_right_type(lua_State *L, bool& value) {
	if (lua_isboolean(L, -1)) value = !!lua_toboolean(L, -1);
}

template <class T>
T get_field(lua_State *L, char const *name, T value) {
	lua_getfield(L, -1, name);
	get_if_right_type(L, value);
	lua_pop(L, 1);
	return value;
}

std::string get_field(lua_State *L, char const *name) {
	return get_field(L, name, std::string());
}

Automation4::AutomationDialogButtonRole button_role(std::string const& name) {
	using Role = Automation4::AutomationDialogButtonRole;
	static std::unordered_map<std::string, Role> const roles{
		{"ok", Role::Ok}, {"yes", Role::Yes}, {"save", Role::Save}, {"apply", Role::Apply}, {"close", Role::Close}, {"no", Role::No}, {"cancel", Role::Cancel}, {"help", Role::Help}, {"context_help", Role::ContextHelp}};
	auto const it = roles.find(name);
	return it == roles.end() ? Role::Custom : it->second;
}

Automation4::AutomationDialogValue read_control_value(lua_State *L, std::string const& type) {
	using namespace Automation4;
	if (type == "label")
		return AutomationDialogLabel{.label = get_field(L, "label")};
	if (type == "edit" || type == "textbox" || type == "alpha")
		return AutomationDialogEdit{.value = get_field(L, "text", get_field(L, "value")), .multiline = type == "textbox"};
	if (type == "color" || type == "coloralpha")
		return AutomationDialogColor{.value = agi::Color(get_field(L, "value")), .alpha = type == "coloralpha"};
	if (type == "intedit") {
		AutomationDialogIntEdit value;
		value.value = get_field(L, "value", value.value);
		value.min = get_field(L, "min", value.min);
		value.max = get_field(L, "max", value.max);
		if (value.min >= value.max) {
			value.min = std::numeric_limits<int>::lowest();
			value.max = std::numeric_limits<int>::max();
		}
		return value;
	}
	if (type == "floatedit") {
		AutomationDialogFloatEdit value;
		value.value = get_field(L, "value", value.value);
		value.min = get_field(L, "min", value.min);
		value.max = get_field(L, "max", value.max);
		value.step = get_field(L, "step", value.step);
		if (value.min >= value.max) {
			value.min = std::numeric_limits<double>::lowest();
			value.max = std::numeric_limits<double>::max();
		}
		if (value.step != 0.0) {
			if (value.min == std::numeric_limits<double>::lowest()) value.min = 0.0;
			if (value.max == std::numeric_limits<double>::max()) value.max = 100.0;
		}
		return value;
	}
	if (type == "dropdown") {
		AutomationDialogDropdown value;
		value.value = get_field(L, "value");
		lua_getfield(L, -1, "items");
		lua_for_each(L, [&] {
			if (lua_isstring(L, -1)) value.items.emplace_back(lua_tostring(L, -1));
		});
		return value;
	}
	if (type == "checkbox")
		return AutomationDialogCheckbox{.label = get_field(L, "label"), .value = get_field(L, "value", false)};
	error(L, "bad control table entry");
}

void push_control_value(lua_State *L, Automation4::AutomationDialogValue const& value) {
	using namespace Automation4;
	std::visit([&](auto const& data) {
		using T = std::decay_t<decltype(data)>;
		if constexpr (std::is_same_v<T, AutomationDialogLabel>)
			lua_pushnil(L);
		else if constexpr (std::is_same_v<T, AutomationDialogEdit> || std::is_same_v<T, AutomationDialogDropdown>)
			lua_pushstring(L, data.value.c_str());
		else if constexpr (std::is_same_v<T, AutomationDialogColor>)
			lua_pushstring(L, data.value.GetHexFormatted(data.alpha).c_str());
		else if constexpr (std::is_same_v<T, AutomationDialogIntEdit>)
			lua_pushinteger(L, data.value);
		else if constexpr (std::is_same_v<T, AutomationDialogFloatEdit>)
			lua_pushnumber(L, data.value);
		else if constexpr (std::is_same_v<T, AutomationDialogCheckbox>)
			lua_pushboolean(L, data.value);
	},
			   value);
}

std::string serialise_control_value(Automation4::AutomationDialogValue const& value) {
	using namespace Automation4;
	return std::visit([](auto const& data) -> std::string {
		using T = std::decay_t<decltype(data)>;
		if constexpr (std::is_same_v<T, AutomationDialogLabel>)
			return {};
		else if constexpr (std::is_same_v<T, AutomationDialogEdit> || std::is_same_v<T, AutomationDialogDropdown>)
			return inline_string_encode(data.value);
		else if constexpr (std::is_same_v<T, AutomationDialogColor>)
			return inline_string_encode(data.value.GetHexFormatted(data.alpha));
		else if constexpr (std::is_same_v<T, AutomationDialogCheckbox>)
			return data.value ? "1" : "0";
		else if constexpr (std::is_same_v<T, AutomationDialogFloatEdit>) {
			char buffer[64];
			auto result = std::to_chars(std::begin(buffer), std::end(buffer), data.value);
			return {buffer, result.ptr};
		}
		else
			return std::to_string(data.value);
	},
					  value);
}

void unserialise_control_value(Automation4::AutomationDialogValue& value, std::string const& serialised) {
	using namespace Automation4;
	std::visit([&](auto& data) {
		using T = std::decay_t<decltype(data)>;
		if constexpr (std::is_same_v<T, AutomationDialogEdit> || std::is_same_v<T, AutomationDialogDropdown> || std::is_same_v<T, AutomationDialogColor>)
			data.value = inline_string_decode(serialised);
		else if constexpr (std::is_same_v<T, AutomationDialogIntEdit>)
			data.value = std::atoi(serialised.c_str());
		else if constexpr (std::is_same_v<T, AutomationDialogFloatEdit>) {
			auto first = serialised.find_first_not_of(" \t\n\r\f\v");
			auto last = serialised.find_last_not_of(" \t\n\r\f\v");
			if (first != std::string::npos) {
				if (serialised[first] == '+' && first < last && serialised[first + 1] != '-') ++first;
				auto result = std::from_chars(serialised.data() + first, serialised.data() + last + 1, data.value);
				if (result.ec == std::errc{} && result.ptr == serialised.data() + last + 1) return;
			}
			data.value = std::atof(serialised.c_str());
		}
		else if constexpr (std::is_same_v<T, AutomationDialogCheckbox>)
			data.value = serialised != "0";
	},
			   value);
}
}

namespace Automation4 {
LuaDialog::LuaDialog(lua_State *L, bool include_buttons) {
	auto const build_model_started = std::chrono::steady_clock::now();
	use_buttons = include_buttons;
	LOG_D("automation/lua/dialog") << "creating LuaDialog, addr: " << this;
	if (!lua_istable(L, 1))
		error(L, "Cannot create config dialog from something non-table");

	controls.reserve(static_cast<size_t>(lua_objlen(L, 1)));
	lua_pushvalue(L, 1);
	lua_for_each(L, [&] {
		if (!lua_istable(L, -1)) error(L, "bad control table entry");
		std::string type = get_field(L, "class");
		agi::util::strings::to_lower_inplace(type);
		AutomationDialogControl control;
		control.name = get_field(L, "name");
		control.hint = get_field(L, "hint");
		control.x = get_field(L, "x", 0);
		control.y = get_field(L, "y", 0);
		control.width = get_field(L, "width", 1);
		control.height = get_field(L, "height", 1);
		control.value = read_control_value(L, type);
		controls.push_back(std::move(control));
	});

	if (include_buttons && lua_istable(L, 2)) {
		buttons.reserve(static_cast<size_t>(lua_objlen(L, 2)));
		lua_pushvalue(L, 2);
		lua_for_each(L, [&] {
			buttons.push_back({.role = AutomationDialogButtonRole::Custom, .label = check_string(L, -1)});
		});
	}

	if (include_buttons && lua_istable(L, 3)) {
		std::unordered_map<std::string, size_t> button_indices;
		button_indices.reserve(buttons.size());
		for (size_t i = 0; i < buttons.size(); ++i) button_indices.emplace(buttons[i].label, i);
		lua_pushvalue(L, 3);
		lua_for_each(L, [&] {
			auto const role = button_role(check_string(L, -2));
			auto const button = button_indices.find(check_string(L, -1));
			if (button == button_indices.end()) error(L, "Invalid button for id %s", lua_tostring(L, -2));
			buttons[button->second].role = role;
		});
	}

	if (use_buttons && buttons.empty()) {
		buttons.push_back({.role = AutomationDialogButtonRole::Ok, .label = ""});
		buttons.push_back({.role = AutomationDialogButtonRole::Cancel, .label = ""});
	}
	model_build_duration_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - build_model_started).count();
}

int LuaDialog::LuaReadBack(lua_State *L) {
	if (use_buttons) {
		if (button_pushed == -1 || buttons[button_pushed].role == AutomationDialogButtonRole::Cancel)
			lua_pushboolean(L, false);
		else
			lua_pushstring(L, buttons[button_pushed].label.c_str());
	}
	lua_createtable(L, 0, static_cast<int>(controls.size()));
	for (auto const& control : controls) {
		push_control_value(L, control.value);
		lua_setfield(L, -2, control.name.c_str());
	}
	return use_buttons ? 2 : 1;
}

std::string LuaDialog::Serialise() {
	std::string result;
	for (auto const& control : controls) {
		if (std::holds_alternative<AutomationDialogLabel>(control.value)) continue;
		if (!result.empty()) result += "|";
		result += inline_string_encode(control.name) + ":" + serialise_control_value(control.value);
	}
	return result;
}

void LuaDialog::Unserialise(std::string const& serialised) {
	for (auto token : agi::Split(serialised, '|')) {
		auto const separator = std::ranges::find(token, ':');
		if (separator == end(token)) continue;
		auto const name = inline_string_decode(std::string(begin(token), separator));
		std::string const value(separator + 1, end(token));
		for (auto& control : controls) {
			if (control.name == name) unserialise_control_value(control.value, value);
		}
	}
}
}
