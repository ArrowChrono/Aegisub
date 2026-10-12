#include "project_save_references.h"

#include "ass_file.h"
#include "automation/local_script_references.h"

#include <libaegisub/fs.h>
#include <libaegisub/path.h>

namespace aegisub {

void RelocateProjectReferences(ProjectProperties& properties,
							   agi::Path const& source_paths,
							   agi::fs::path const& destination_directory) {
	auto const destination = destination_directory.empty() ? std::filesystem::current_path() : std::filesystem::absolute(destination_directory);
	auto relocate = [&](std::string& reference) {
		auto const path = agi::fs::PathFromString(reference);
		if (path.empty() || path.is_absolute() || agi::IsNonFilesystemMediaPath(path)) {
			return;
		}
		auto const absolute = std::filesystem::absolute(source_paths.MakeAbsolute(path, "?script"));
		reference = agi::fs::PathToGenericString(source_paths.MakeRelative(absolute, destination));
	};

	relocate(properties.audio_file);
	relocate(properties.video_file);
	relocate(properties.timecodes_file);
	relocate(properties.keyframes_file);
	relocate(properties.secondary_subtitles_file);

	auto references = Automation4::ParseLocalScriptReferences(properties.automation_scripts);
	bool relocated = false;
	for (auto& reference : references) {
		if (reference.location != '~') {
			continue;
		}
		auto const location = Automation4::ResolveLocalScriptReference(reference, source_paths.MakeAbsolute(".", "?script"), "");
		auto const absolute = std::filesystem::absolute(location->filename);
		auto const relative = source_paths.MakeRelative(absolute, destination);
		reference.location = relative.is_absolute() ? '/' : '~';
		reference.filename = agi::fs::PathToGenericString(relative);
		relocated = true;
	}
	if (relocated) {
		properties.automation_scripts.clear();
		for (auto const& reference : references) {
			if (!properties.automation_scripts.empty()) {
				properties.automation_scripts += '|';
			}
			properties.automation_scripts += reference.location;
			properties.automation_scripts += reference.filename;
		}
	}
}

}
