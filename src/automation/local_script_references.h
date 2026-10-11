#pragma once

#include <libaegisub/fs_fwd.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace agi {
class Path;
}

namespace Automation4 {

struct LocalScriptReference {
	char location;
	std::string filename;
};

struct LocalScriptLocation {
	agi::fs::path base_directory;
	agi::fs::path filename;
};

std::vector<LocalScriptReference> ParseLocalScriptReferences(std::string const& references);
std::optional<LocalScriptLocation> ResolveLocalScriptReference(
	LocalScriptReference const& reference,
	agi::fs::path const& subtitle_directory,
	std::string const& automation_directory);
std::string SerializeLocalScriptReferences(
	std::span<agi::fs::path const> filenames,
	agi::Path const& paths,
	agi::fs::path const& automation_directory);

}
