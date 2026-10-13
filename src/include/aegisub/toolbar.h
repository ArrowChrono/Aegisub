// Copyright (c) 2011, Thomas Goyne <plorkyeran@aegisub.org>
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

/// @file toolbar.h
/// @brief Dynamic toolbar generator.
/// @ingroup menu toolbar

#include <libaegisub/option_value.h>
#include <libaegisub/string_utils.h>
#include <vector>
#include <string>
#include <utility>

namespace agi {
struct Context;
}
class wxFrame;
class wxToolBar;
class wxWindow;
class wxPanel;

namespace toolbar {
inline std::string NormalizeCommandButtonValue(std::string value) {
	agi::util::strings::trim_inplace(value);
	return value == "-" ? std::string() : value;
}

inline std::vector<std::string> LoadCommandButtonCommands(agi::OptionValue const& option) {
	if (option.GetType() == agi::OptionType::ListString)
		return option.GetListString();
	std::vector<std::string> commands;
	agi::util::strings::for_each_split_any(option.GetString(), "\r\n,;", false, [&](agi::util::strings::view item) {
		commands.emplace_back(NormalizeCommandButtonValue(std::string(item)));
	});
	return commands;
}

inline std::vector<std::string> NormalizeCommandButtonCommands(std::vector<std::string> const& raw) {
	std::vector<std::string> commands;
	commands.reserve(raw.size());
	for (auto const& item : raw) {
		auto value = NormalizeCommandButtonValue(item);
		if (!value.empty())
			commands.emplace_back(std::move(value));
		else if (!commands.empty() && !commands.back().empty())
			commands.emplace_back();
	}
	if (!commands.empty() && commands.back().empty())
		commands.pop_back();
	return commands;
}

	/// Add the named toolbar to a window
	/// @param frame Frame to attach the toolbar to
	/// @param name Name of the toolbar
	/// @param context Project context
	/// @param hotkey Hotkey context for the tooltip
void AttachToolbar(wxFrame *frame, std::string const& name, agi::Context *context, std::string const& hotkey);
wxToolBar *GetToolbar(wxWindow *parent, std::string const& name, agi::Context *context, std::string const& hotkey, bool vertical = false);
wxToolBar *GetOptionToolbar(wxWindow *parent, std::string const& name, std::string const& command_option, agi::Context *context, std::string const& hotkey, bool vertical = false);
	/// Create a wrapping command button bar with wxWrapSizer
wxPanel *GetOptionToolbarWrapping(wxWindow *parent, std::string const& command_option, agi::Context *context, std::string const& hotkey);

	/// Parse "command_name|Display Name" format for configurable command buttons
	/// Returns {command_name, display_name_or_empty}
inline std::pair<std::string, std::string> ParseCommandEntry(std::string const& entry) {
	auto pipe_pos = entry.find('|');
	if (pipe_pos == std::string::npos)
		return {entry, std::string()};
	return {entry.substr(0, pipe_pos), entry.substr(pipe_pos + 1)};
}

	/// Build "command_name|Display Name" string
	inline std::string MakeCommandEntry(std::string const& command_name, std::string const& display_name) {
		if (display_name.empty())
			return command_name;
		return command_name + "|" + display_name;
	}
	}
