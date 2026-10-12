#include "automation_script_collection.h"

#include "engine/automation_engine_registry.h"
#include "local_script_references.h"

#include <libaegisub/exception.h>
#include <libaegisub/format.h>
#include <libaegisub/fs.h>

#include <exception>
#include <future>
#include <system_error>

namespace Automation4 {
namespace {

class UnavailableScript final : public AutomationScriptInstance {
	agi::fs::path filename;
	std::string description;

	public:
	UnavailableScript(agi::fs::path filename, std::string description)
		: filename(std::move(filename)), description(std::move(description)) {}
	void Reload() override {}
	[[nodiscard]] agi::fs::path GetFilename() const override { return filename; }
	[[nodiscard]] std::string GetName() const override { return agi::fs::PathToString(filename.stem()); }
	[[nodiscard]] std::string GetDescription() const override { return description; }
	[[nodiscard]] std::string GetAuthor() const override { return {}; }
	[[nodiscard]] std::string GetVersion() const override { return {}; }
	[[nodiscard]] std::string GetEngineName() const override { return {}; }
	[[nodiscard]] bool GetLoadedState() const override { return false; }
};

agi::fs::path CanonicalScriptPath(agi::fs::path filename) {
#ifdef _WIN32
	auto const& native = filename.native();
	if (native.size() >= 260 && !native.starts_with(L"\\\\?\\") && !native.starts_with(L"\\\\.\\"))
		filename = filename.lexically_normal();
#endif
	try {
		return agi::fs::Canonicalize(filename);
	}
	catch (agi::fs::FileSystemError const&) {
		return filename;
	}
}

std::string DescribeCurrentException() {
	try {
		throw;
	}
	catch (agi::Exception const& error) {
		return error.GetMessage();
	}
	catch (std::exception const& error) {
		return error.what();
	}
	catch (...) {
		return "Unknown error";
	}
}

void AppendLoadAttempt(AutomationScriptLoadResult& result, AutomationScriptLoadAttempt attempt) {
	if (!attempt.script) return;
	if (!attempt.recognised) {
		++result.error_count;
		result.diagnostics.push_back({.message = "The file was not recognised as an Automation script: " +
												 agi::fs::PathToString(attempt.script->GetFilename()),
									  .error = true});
	}
	else if (!attempt.script->GetLoadedState()) {
		++result.error_count;
		result.diagnostics.push_back({.message = "Failed to load Automation script '" +
												 agi::fs::PathToString(attempt.script->GetFilename()) + "':\n" + attempt.script->GetDescription(),
									  .error = true});
	}
	result.scripts.emplace_back(std::move(attempt.script));
}

}

std::string AutomationScriptId(agi::fs::path const& filename) {
	std::error_code error;
	auto absolute_filename = std::filesystem::absolute(filename, error);
	return agi::fs::PathToGenericString((error ? filename : absolute_filename).lexically_normal());
}

AutomationScriptLoadAttempt LoadAutomationScript(agi::fs::path const& filename, bool create_unknown,
												 std::shared_ptr<AutomationRuntimeServices> const& services) {
	AutomationScriptLoadAttempt result;
	try {
		result.script = AutomationEngineRegistry::CreateFromFile(filename, services);
		result.recognised = result.script != nullptr;
		if (!result.script && create_unknown)
			result.script = std::make_unique<UnavailableScript>(filename, "File was not recognized as a script");
	}
	catch (...) {
		result.recognised = true;
		result.script = std::make_unique<UnavailableScript>(filename, DescribeCurrentException());
	}
	return result;
}

AutomationScriptLoadResult LoadAutoloadScripts(std::span<agi::fs::path const> directories,
											   std::shared_ptr<AutomationRuntimeServices> const& services) {
	AutomationScriptLoadResult result;
	std::vector<agi::fs::path> script_filenames;
	for (auto const& directory : directories) {
		if (!agi::fs::DirectoryExists(directory)) continue;
		auto dirname = CanonicalScriptPath(directory);
		for (auto const& filename : agi::fs::DirectoryIterator(dirname, "*.*")) {
			auto script_filename = dirname / agi::fs::PathFromString(filename);
			auto leaf = agi::fs::PathToString(script_filename.filename());
			std::ranges::transform(leaf, leaf.begin(), [](char value) {
				return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
			});
			if (leaf == "plugin.json") continue;
			script_filename = CanonicalScriptPath(script_filename);
			if (std::ranges::find(script_filenames, script_filename) == script_filenames.end())
				script_filenames.emplace_back(std::move(script_filename));
		}
	}

	std::vector<std::future<AutomationScriptLoadAttempt>> futures;
	for (auto const& filename : script_filenames) {
		try {
			futures.emplace_back(std::async(std::launch::async, [filename, services] { return LoadAutomationScript(filename, false, services); }));
		}
		catch (...) {
			AppendLoadAttempt(result, LoadAutomationScript(filename, false, services));
		}
	}
	for (auto& future : futures) AppendLoadAttempt(result, future.get());
	return result;
}

AutomationScriptLoadResult LoadLocalScripts(std::string const& references, agi::fs::path const& subtitle_directory,
											std::string const& automation_directory, std::shared_ptr<AutomationRuntimeServices> const& services) {
	AutomationScriptLoadResult result;
	for (auto const& reference : ParseLocalScriptReferences(references)) {
		auto location = ResolveLocalScriptReference(reference, subtitle_directory, automation_directory);
		if (!location) {
			result.diagnostics.push_back({.message = agi::format(
											  "Automation Script referenced with unknown location specifier character.\n"
											  "Location specifier found: %c\nFilename specified: %s",
											  reference.location, reference.filename),
										  .error = false});
			continue;
		}
		if (!agi::fs::FileExists(location->filename)) {
			result.diagnostics.push_back({.message = agi::format(
											  "Automation Script referenced could not be found.\n"
											  "Filename specified: %c%s\nSearched relative to: %s\nResolved filename: %s",
											  reference.location, reference.filename, agi::fs::PathToString(location->base_directory),
											  agi::fs::PathToString(location->filename)),
										  .error = false});
			continue;
		}
		AppendLoadAttempt(result, LoadAutomationScript(CanonicalScriptPath(location->filename), true, services));
	}
	return result;
}

}
