#include "local_script_references.h"

#include <libaegisub/fs.h>
#include <libaegisub/path.h>
#include <libaegisub/split.h>
#include <libaegisub/string_utils.h>

#include <utility>

namespace Automation4 {

std::vector<LocalScriptReference> ParseLocalScriptReferences(std::string const& references) {
	std::vector<LocalScriptReference> result;
	for (auto tok : agi::Split(references, '|')) {
		auto token = agi::util::strings::trim_copy(agi::str(tok));
		if (token.empty()) continue;
		result.push_back({.location = token[0], .filename = std::string(token.begin() + 1, token.end())});
	}
	return result;
}

std::optional<LocalScriptLocation> ResolveLocalScriptReference(
	LocalScriptReference const& reference,
	agi::fs::path const& subtitle_directory,
	std::string const& automation_directory) {
	agi::fs::path base_directory;
	if (reference.location == '~')
		base_directory = subtitle_directory;
	else if (reference.location == '$')
		base_directory = automation_directory;
	else if (reference.location != '/')
		return std::nullopt;
	auto filename = base_directory / agi::fs::PathFromString(reference.filename);
	return LocalScriptLocation{.base_directory = std::move(base_directory), .filename = std::move(filename)};
}

std::string SerializeLocalScriptReferences(
	std::span<agi::fs::path const> filenames,
	agi::Path const& paths,
	agi::fs::path const& automation_directory) {
	std::string result;
	for (auto const& filename : filenames) {
		if (!result.empty())
			result += "|";

		auto const filename_str = agi::fs::PathToString(filename);
		auto const automation_relative = paths.MakeRelative(filename, automation_directory);
		auto const subtitle_relative = paths.MakeRelative(filename, "?script");
		auto const automation_relative_str = agi::fs::PathToGenericString(automation_relative);
		auto const subtitle_relative_str = agi::fs::PathToGenericString(subtitle_relative);

		if (automation_relative_str.size() <= filename_str.size() && automation_relative_str.size() <= subtitle_relative_str.size())
			result += "$" + automation_relative_str;
		else if (subtitle_relative_str.size() <= filename_str.size() && subtitle_relative_str.size() <= automation_relative_str.size())
			result += "~" + subtitle_relative_str;
		else
			result += "/" + agi::fs::PathToGenericString(filename);
	}
	return result;
}

}
