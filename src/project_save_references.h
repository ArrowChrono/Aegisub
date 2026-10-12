#pragma once

#include <libaegisub/fs_fwd.h>

struct ProjectProperties;
namespace agi {
class Path;
}

namespace aegisub {

void RelocateProjectReferences(ProjectProperties& properties,
							   agi::Path const& source_paths,
							   agi::fs::path const& destination_directory);

}
