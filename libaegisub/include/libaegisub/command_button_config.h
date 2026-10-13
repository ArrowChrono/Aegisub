#pragma once

#include <libaegisub/option_value.h>
#include <libaegisub/string_utils.h>
#include <string>
#include <utility>
#include <vector>

namespace agi::command_buttons {
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
}
