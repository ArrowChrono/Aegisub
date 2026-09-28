// Copyright (c) 2010, Amar Takhar <verm@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include <libaegisub/hotkey.h>

#include "include/aegisub/hotkey.h"

#include "hotkey_migration.h"
#include "libresrc/libresrc.h"
#include "command/command.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "status_sink.h"

#include <libaegisub/path.h>

#include <wx/intl.h>

#include <algorithm>
namespace {
	const char* added_hotkeys_cj[][3] = {
		{"time/align", "Video", "KP_TAB"},
		{nullptr}
	};

	const char *added_hotkeys_7035[][3] = {
		{"audio/play/line", "Audio", "R"},
		{nullptr}
	};

	const char *added_hotkeys_7070[][3] = {
		{"edit/color/primary", "Subtitle Edit Box", "Alt-1"},
		{"edit/color/secondary", "Subtitle Edit Box", "Alt-2"},
		{"edit/color/outline", "Subtitle Edit Box", "Alt-3"},
		{"edit/color/shadow", "Subtitle Edit Box", "Alt-4"},
		{nullptr}
	};

	const char *added_hotkeys_shift_back[][3] = {
		{"edit/line/duplicate/shift_back", "Default", "Ctrl-Shift-D"},
		{nullptr}
	};

	const char *added_hotkeys_selection_history[][3] = {
		{"grid/selection/back", "Default", "MouseBack"},
		{"grid/selection/forward", "Default", "MouseForward"},
		{nullptr}
	};

	void migrate_lua_code_edit_box_hotkey() {
		constexpr auto context = "Lua Code Edit Box";
		constexpr auto command = "automation/lua/open-current-line";
		constexpr auto keys = "Shift-Enter";

		auto hk_map = hotkey::inst->GetHotkeyMap();
		for (auto const& entry : hk_map) {
			auto const& combo = entry.second;
			if (combo.Context() == context && (entry.first == command || combo.Str() == keys))
				return;
		}

		hk_map.insert({command, agi::hotkey::Combo(context, command, keys)});
		hotkey::inst->SetHotkeyMap(std::move(hk_map));
	}

	// Large step uses Alt rather than Shift so Shift-Left/Right keep Video
	// keyframe navigation while a visual tool is active.
	const char *added_hotkeys_visual_tool_nudge[][3] = {
		{"video/tool/nudge/left", "Visual Rotate Z", "Left"},
		{"video/tool/nudge/right", "Visual Rotate Z", "Right"},
		{"video/tool/nudge/up", "Visual Rotate Z", "Up"},
		{"video/tool/nudge/down", "Visual Rotate Z", "Down"},
		{"video/tool/nudge/left/large", "Visual Rotate Z", "Alt-Left"},
		{"video/tool/nudge/right/large", "Visual Rotate Z", "Alt-Right"},
		{"video/tool/nudge/up/large", "Visual Rotate Z", "Alt-Up"},
		{"video/tool/nudge/down/large", "Visual Rotate Z", "Alt-Down"},
		{"video/tool/nudge/left", "Visual Rotate XY", "Left"},
		{"video/tool/nudge/right", "Visual Rotate XY", "Right"},
		{"video/tool/nudge/up", "Visual Rotate XY", "Up"},
		{"video/tool/nudge/down", "Visual Rotate XY", "Down"},
		{"video/tool/nudge/left/large", "Visual Rotate XY", "Alt-Left"},
		{"video/tool/nudge/right/large", "Visual Rotate XY", "Alt-Right"},
		{"video/tool/nudge/up/large", "Visual Rotate XY", "Alt-Up"},
		{"video/tool/nudge/down/large", "Visual Rotate XY", "Alt-Down"},
		{"video/tool/nudge/left", "Visual Scale", "Left"},
		{"video/tool/nudge/right", "Visual Scale", "Right"},
		{"video/tool/nudge/up", "Visual Scale", "Up"},
		{"video/tool/nudge/down", "Visual Scale", "Down"},
		{"video/tool/nudge/left/large", "Visual Scale", "Alt-Left"},
		{"video/tool/nudge/right/large", "Visual Scale", "Alt-Right"},
		{"video/tool/nudge/up/large", "Visual Scale", "Alt-Up"},
		{"video/tool/nudge/down/large", "Visual Scale", "Alt-Down"},
		{nullptr}
	};

#ifdef __WXMAC__
	const char *added_hotkeys_minimize[][3] = {
		{"app/minimize", "Default", "Ctrl-M"},
		{nullptr}
	};
#endif

	void migrate_hotkeys(const char *added[][3]) {
		auto hk_map = hotkey::inst->GetHotkeyMap();
		bool changed = false;

		for (size_t i = 0; added[i] && added[i][0]; ++i) {
			agi::hotkey::Combo combo(added[i][1], added[i][0], added[i][2]);

			if (hotkey::inst->HasHotkey(combo.Context(), combo.Str()))
				continue;

			hk_map.insert(make_pair(std::string(added[i][0]), std::move(combo)));
			changed = true;
		}

		if (changed)
			hotkey::inst->SetHotkeyMap(std::move(hk_map));
	}

	/// Remap default Shift-* large-nudge bindings to Alt-* so keyframe nav is free.
	/// Only touches the shipped defaults; user-customized keys are left alone.
	void migrate_visual_tool_nudge_large_to_alt() {
		static const char *const large_cmds[] = {
			"video/tool/nudge/left/large",
			"video/tool/nudge/right/large",
			"video/tool/nudge/up/large",
			"video/tool/nudge/down/large",
		};
		static const char *const old_keys[] = {
			"Shift-Left", "Shift-Right", "Shift-Up", "Shift-Down",
		};
		static const char *const new_keys[] = {
			"Alt-Left", "Alt-Right", "Alt-Up", "Alt-Down",
		};

		auto hk_map = hotkey::inst->GetHotkeyMap();
		bool changed = false;
		agi::hotkey::Hotkey::HotkeyMap rebuilt;

		for (auto const& entry : hk_map) {
			auto const& cmd = entry.first;
			auto const& combo = entry.second;
			bool remapped = false;

			for (size_t i = 0; i < 4; ++i) {
				if (cmd != large_cmds[i] || combo.Str() != old_keys[i])
					continue;
				// Skip if the Alt binding is already taken in this context.
				if (hotkey::inst->HasHotkey(combo.Context(), new_keys[i]))
					break;
				rebuilt.insert({cmd, agi::hotkey::Combo(combo.Context(), cmd, new_keys[i])});
				changed = true;
				remapped = true;
				break;
			}

			if (!remapped)
				rebuilt.insert(entry);
		}

		if (changed)
			hotkey::inst->SetHotkeyMap(std::move(rebuilt));
	}
}

namespace hotkey {

agi::hotkey::Hotkey *inst = nullptr;
void init() {
	inst = new agi::hotkey::Hotkey(
		config::path->Decode("?user/hotkey.json"),
		libresrc_getconfig(default_hotkey, default_hotkey_size));

	auto migrations = OPT_GET("App/Hotkey Migrations")->GetListString();

	if (std::find(begin(migrations), end(migrations), "cj") == end(migrations)) {
		migrate_hotkeys(added_hotkeys_cj);
		migrations.emplace_back("cj");
	}

	if (std::find(begin(migrations), end(migrations), "7035") == end(migrations)) {
		migrate_hotkeys(added_hotkeys_7035);
		migrations.emplace_back("7035");
	}

	if (std::find(begin(migrations), end(migrations), "7070") == end(migrations)) {
		migrate_hotkeys(added_hotkeys_7070);
		migrations.emplace_back("7070");
	}

	if (std::find(begin(migrations), end(migrations), "edit/line/duplicate/shift_back") == end(migrations)) {
		migrate_hotkeys(added_hotkeys_shift_back);
		migrations.emplace_back("edit/line/duplicate/shift_back");
	}

	if (std::find(begin(migrations), end(migrations), "grid/selection/history") == end(migrations)) {
		migrate_hotkeys(added_hotkeys_selection_history);
		migrations.emplace_back("grid/selection/history");
	}

	if (std::ranges::find(migrations, "lua/code-edit-box/open-workspace") == end(migrations)) {
		migrate_lua_code_edit_box_hotkey();
		migrations.emplace_back("lua/code-edit-box/open-workspace");
	}

	if (std::find(begin(migrations), end(migrations), "visual_tool_nudge") == end(migrations)) {
		migrate_hotkeys(added_hotkeys_visual_tool_nudge);
		migrations.emplace_back("visual_tool_nudge");
		// First install already uses Alt large steps; mark remap done.
		migrations.emplace_back("visual_tool_nudge_alt_large");
	}

	if (std::find(begin(migrations), end(migrations), "visual_tool_nudge_alt_large") == end(migrations)) {
		migrate_visual_tool_nudge_large_to_alt();
		migrations.emplace_back("visual_tool_nudge_alt_large");
	}

	if (std::find(begin(migrations), end(migrations), "duplicate -> split") == end(migrations)) {
		auto hk_map = hotkey::inst->GetHotkeyMap();
		aegisub::hotkey_migration::RenameCommand(
			hk_map, "edit/line/duplicate/shift", "edit/line/split/before");
		aegisub::hotkey_migration::RenameCommand(
			hk_map, "edit/line/duplicate/shift_back", "edit/line/split/after");

		hotkey::inst->SetHotkeyMap(std::move(hk_map));
		migrations.emplace_back("duplicate -> split");
	}

#ifdef __WXMAC__
	if (std::find(begin(migrations), end(migrations), "app/minimize") == end(migrations)) {
		migrate_hotkeys(added_hotkeys_minimize);
		migrations.emplace_back("app/minimize");
	}
#endif

	OPT_SET("App/Hotkey Migrations")->SetListString(std::move(migrations));
}

void clear() {
	delete inst;
}

static const char *keycode_name(int code) {
	switch (code) {
	case WXK_BACK:            return "Backspace";
	case WXK_TAB:             return "Tab";
	case WXK_RETURN:          return "Enter";
	case WXK_ESCAPE:          return "Escape";
	case WXK_SPACE:           return "Space";
	case WXK_DELETE:          return "Delete";
	case WXK_SHIFT:           return "Shift";
	case WXK_ALT:             return "Alt";
	case WXK_CONTROL:         return "Control";
	case WXK_PAUSE:           return "Pause";
	case WXK_END:             return "End";
	case WXK_HOME:            return "Home";
	case WXK_LEFT:            return "Left";
	case WXK_UP:              return "Up";
	case WXK_RIGHT:           return "Right";
	case WXK_DOWN:            return "Down";
	case WXK_PRINT:           return "Print";
	case WXK_INSERT:          return "Insert";
	case WXK_NUMPAD0:         return "KP_0";
	case WXK_NUMPAD1:         return "KP_1";
	case WXK_NUMPAD2:         return "KP_2";
	case WXK_NUMPAD3:         return "KP_3";
	case WXK_NUMPAD4:         return "KP_4";
	case WXK_NUMPAD5:         return "KP_5";
	case WXK_NUMPAD6:         return "KP_6";
	case WXK_NUMPAD7:         return "KP_7";
	case WXK_NUMPAD8:         return "KP_8";
	case WXK_NUMPAD9:         return "KP_9";
	case WXK_MULTIPLY:        return "Asterisk";
	case WXK_ADD:             return "Plus";
	case WXK_SUBTRACT:        return "Hyphen";
	case WXK_DECIMAL:         return "Period";
	case WXK_DIVIDE:          return "Slash";
	case WXK_F1:              return "F1";
	case WXK_F2:              return "F2";
	case WXK_F3:              return "F3";
	case WXK_F4:              return "F4";
	case WXK_F5:              return "F5";
	case WXK_F6:              return "F6";
	case WXK_F7:              return "F7";
	case WXK_F8:              return "F8";
	case WXK_F9:              return "F9";
	case WXK_F10:             return "F10";
	case WXK_F11:             return "F11";
	case WXK_F12:             return "F12";
	case WXK_F13:             return "F13";
	case WXK_F14:             return "F14";
	case WXK_F15:             return "F15";
	case WXK_F16:             return "F16";
	case WXK_F17:             return "F17";
	case WXK_F18:             return "F18";
	case WXK_F19:             return "F19";
	case WXK_F20:             return "F20";
	case WXK_F21:             return "F21";
	case WXK_F22:             return "F22";
	case WXK_F23:             return "F23";
	case WXK_F24:             return "F24";
	case WXK_NUMLOCK:         return "Num_Lock";
	case WXK_SCROLL:          return "Scroll_Lock";
	case WXK_PAGEUP:          return "PageUp";
	case WXK_PAGEDOWN:        return "PageDown";
	case WXK_NUMPAD_SPACE:    return "KP_Space";
	case WXK_NUMPAD_TAB:      return "KP_Tab";
	case WXK_NUMPAD_ENTER:    return "KP_Enter";
	case WXK_NUMPAD_F1:       return "KP_F1";
	case WXK_NUMPAD_F2:       return "KP_F2";
	case WXK_NUMPAD_F3:       return "KP_F3";
	case WXK_NUMPAD_F4:       return "KP_F4";
	case WXK_NUMPAD_HOME:     return "KP_Home";
	case WXK_NUMPAD_LEFT:     return "KP_Left";
	case WXK_NUMPAD_UP:       return "KP_Up";
	case WXK_NUMPAD_RIGHT:    return "KP_Right";
	case WXK_NUMPAD_DOWN:     return "KP_Down";
	case WXK_NUMPAD_PAGEUP:   return "KP_PageUp";
	case WXK_NUMPAD_PAGEDOWN: return "KP_PageDown";
	case WXK_NUMPAD_END:      return "KP_End";
	case WXK_NUMPAD_BEGIN:    return "KP_Begin";
	case WXK_NUMPAD_INSERT:   return "KP_insert";
	case WXK_NUMPAD_DELETE:   return "KP_Delete";
	case WXK_NUMPAD_EQUAL:    return "KP_Equal";
	case WXK_NUMPAD_MULTIPLY: return "KP_Multiply";
	case WXK_NUMPAD_ADD:      return "KP_Add";
	case WXK_NUMPAD_SUBTRACT: return "KP_Subtract";
	case WXK_NUMPAD_DECIMAL:  return "KP_Decimal";
	case WXK_NUMPAD_DIVIDE:   return "KP_Divide";
	default: return "";
	}
}

static void append_modifiers(std::string& combo, int modifier) {
	if ((modifier != wxMOD_NONE)) {
		if ((modifier & wxMOD_CMD) != 0) combo.append("Ctrl-");
		if ((modifier & wxMOD_ALT) != 0) combo.append("Alt-");
		if ((modifier & wxMOD_SHIFT) != 0) combo.append("Shift-");
	}
}

std::string keypress_to_str(int key_code, int modifier) {
	std::string combo;
	append_modifiers(combo, modifier);

	if (key_code > 32 && key_code < 127)
		combo += (char)key_code;
	else
		combo += keycode_name(key_code);

	return combo;
}

std::string mousepress_to_str(wxMouseEvent const& evt) {
	std::string combo;
	char const *button_name = nullptr;
	if (evt.Aux1Down())
		button_name = "MouseBack";
	else if (evt.Aux2Down())
		button_name = "MouseForward";

	if (!button_name)
		return combo;

	append_modifiers(combo, evt.GetModifiers());
	combo += button_name;
	return combo;
}

static bool check(std::string const& context, agi::Context *c, int key_code, int modifier) {
	std::string combo = keypress_to_str(key_code, modifier);
	if (combo.empty()) return false;

	std::string command = inst->Scan(context, combo, OPT_GET("Audio/Medusa Timing Hotkeys")->GetBool());
	if (!command.empty()) {
		cmd::call(command, c);
		return true;
	}
	return false;
}

bool check(std::string const& context, agi::Context *c, wxKeyEvent &evt) {
	try {
		if (!check(context, c, evt.GetKeyCode(), evt.GetModifiers())) {
			evt.Skip();
			return false;
		}
		return true;
	}
	catch (cmd::CommandNotFound const& e) {
		c->ShowError(e.GetMessage(), from_wx(_("Invalid command name for hotkey")));
		return true;
	}
}

bool check_exact(std::string const& context, agi::Context *c, wxKeyEvent &evt) {
	if (context.empty())
		return false;

	try {
		std::string combo = keypress_to_str(evt.GetKeyCode(), evt.GetModifiers());
		if (combo.empty())
			return false;

		std::string command = inst->ScanExact(context, combo);
		if (command.empty())
			return false;

		cmd::Command *cmd = cmd::get(command);
		if (!cmd->Validate(c))
			return false;

		cmd::call(command, c);
		return true;
	}
	catch (cmd::CommandNotFound const& e) {
		c->ShowError(e.GetMessage(), from_wx(_("Invalid command name for hotkey")));
		return true;
	}
}

bool check(std::string const& context, agi::Context *c, wxMouseEvent &evt) {
	try {
		std::string combo = mousepress_to_str(evt);
		if (combo.empty())
			return false;

		std::string command = inst->Scan(context, combo, OPT_GET("Audio/Medusa Timing Hotkeys")->GetBool());
		if (!command.empty()) {
			cmd::call(command, c);
			return true;
		}

		evt.Skip();
		return false;
	}
	catch (cmd::CommandNotFound const& e) {
		c->ShowError(e.GetMessage(), from_wx(_("Invalid command name for hotkey")));
		return true;
	}
}

std::vector<std::string> get_hotkey_strs(std::string const& context, std::string const& command) {
	return inst->GetHotkeys(context, command);
}

std::string get_hotkey_str_first(std::string const& context, std::string const& command) {
	return inst->GetHotkey(context, command);
}

} // namespace hotkey
