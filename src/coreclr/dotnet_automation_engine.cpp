#include "dotnet_automation_engine.h"

#include "adapter_bridge.h"
#include "declarative_ui_host.h"
#include "package_transaction_host.h"
#include "dotnet_query_state.h"
#include "dotnet_subtitle_bridge.h"
#include "plugin_service_registry.h"

#include "auto4_base.h"
#include "automation/automation_host.h"
#include "automation/automation_live_host.h"
#include "automation/engine/automation_script_instance.h"
#include "command/command.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "project.h"
#include "selection_controller.h"
#include "subs_controller.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Automation4 {
namespace {

enum class DotNetSubtitleAccess {
	None,
	Read,
	ReadWrite
};

enum class DotNetRuntimeKind {
	CoreClr,
	Native
};

struct MacroManifest {
	std::string id;
	std::string name;
	std::string description;
	std::string name_resource_key;
	std::string description_resource_key;
	bool requires_project = false;
	DotNetSubtitleAccess subtitle_access = DotNetSubtitleAccess::None;
	DotNetSubtitleSnapshotScope snapshot_scope = DotNetSubtitleSnapshotScope::Full;
	DotNetMacroQueryState query_state;
};

struct ContributionManifest {
	std::string id;
	std::string kind;
	std::string name;
	std::string description;
	std::string name_resource_key;
	std::string description_resource_key;
	std::vector<MacroManifest> macros;
	std::vector<std::string> operations;
};

struct ExtensionManifest {
	std::string id;
	std::string name;
	std::string description;
	std::string name_resource_key;
	std::string description_resource_key;
	std::string author;
	std::string version;
	std::string contracts_version;
	DotNetRuntimeKind runtime_kind = DotNetRuntimeKind::CoreClr;
	std::filesystem::path entry_assembly;
	std::string entry_type;
	std::filesystem::path runtime_library;
	std::string entry_point;
	std::vector<ContributionManifest> contributions;
};

std::optional<std::string> FindString(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end()) return std::nullopt;
	try {
		return static_cast<json::String const&>(it->second);
	}
	catch (...) {
		throw std::runtime_error("C# extension field '" + key + "' must be a string");
	}
}

std::optional<int> FindInt(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end()) return std::nullopt;
	int64_t value = 0;
	try {
		value = static_cast<int64_t>(static_cast<json::Integer const&>(it->second));
	}
	catch (...) {
		throw std::runtime_error("C# extension field '" + key + "' must be an integer");
	}
	if (value < std::numeric_limits<int>::min() ||
		value > std::numeric_limits<int>::max())
		throw std::runtime_error(
			"C# extension manifest integer field '" + key + "' is outside the native range");
	return static_cast<int>(value);
}

std::optional<bool> FindBool(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end()) return std::nullopt;
	try {
		return static_cast<json::Boolean const&>(it->second);
	}
	catch (...) {
		throw std::runtime_error("C# extension field '" + key + "' must be boolean");
	}
}

std::filesystem::path ParseRelativeManifestPath(
	std::string value,
	std::string const& field_name) {
	if (value.empty())
		throw std::runtime_error(
			"C# extension manifest requires a non-empty '" + field_name + "'");
	auto is_ascii_letter = [](unsigned char character) {
		return (character >= 'A' && character <= 'Z') ||
			(character >= 'a' && character <= 'z');
	};
	if (value.front() == '/' || value.front() == '\\' ||
		(value.size() >= 2 && is_ascii_letter(static_cast<unsigned char>(value[0])) &&
			value[1] == ':'))
		throw std::runtime_error(
			"C# extension " + field_name + " must be relative to the manifest");

	for (size_t begin = 0; begin <= value.size();) {
		auto end = value.find_first_of("/\\", begin);
		auto component = std::string_view(value).substr(
			begin, end == std::string::npos ? value.size() - begin : end - begin);
		if (component == "..")
			throw std::runtime_error(
				"C# extension " + field_name + " cannot escape the manifest directory");
		if (end == std::string::npos) break;
		begin = end + 1;
	}

	// Manifest paths use portable separators even when authored on another OS.
	std::replace(value.begin(), value.end(), '\\', '/');
	auto path = std::filesystem::u8path(value);
	if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
		throw std::runtime_error(
			"C# extension " + field_name + " must be relative to the manifest");
	return path;
}

std::filesystem::path ParseRelativeEntryAssembly(std::string value) {
	return ParseRelativeManifestPath(std::move(value), "entryAssembly");
}

std::string CurrentNativeRid() {
#ifdef _WIN32
#if defined(_M_ARM64)
	return "win-arm64";
#elif defined(_M_IX86)
	return "win-x86";
#else
	return "win-x64";
#endif
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
	return "osx-arm64";
#else
	return "osx-x64";
#endif
#else
#if defined(__aarch64__)
	return "linux-arm64";
#else
	return "linux-x64";
#endif
#endif
}

std::string RequireString(json::Object const& object, std::string const& key) {
	auto value = FindString(object, key);
	if (!value || value->empty())
		throw std::runtime_error("C# extension manifest requires a non-empty '" + key + "'");
	return std::move(*value);
}

json::Object const& RequireObject(
	json::Object const& object,
	std::string const& key,
	std::string const& source) {
	auto it = object.find(key);
	if (it == object.end())
		throw std::runtime_error(source + " requires a '" + key + "' object");
	try {
		return static_cast<json::Object const&>(it->second);
	}
	catch (...) {
		throw std::runtime_error(source + " field '" + key + "' must be an object");
	}
}

json::Array const& RequireArray(
	json::Object const& object,
	std::string const& key,
	std::string const& source) {
	auto it = object.find(key);
	if (it == object.end())
		throw std::runtime_error(source + " requires a '" + key + "' array");
	try {
		return static_cast<json::Array const&>(it->second);
	}
	catch (...) {
		throw std::runtime_error(source + " field '" + key + "' must be an array");
	}
}

bool IsKnownContributionKind(std::string const& kind) {
	return kind == "automation" || kind == "command" || kind == "settings" ||
		kind == "toolView" || kind == "serviceProvider";
}

DotNetSubtitleAccess ParseSubtitleAccess(std::string const& value) {
	if (value == "none") return DotNetSubtitleAccess::None;
	if (value == "read") return DotNetSubtitleAccess::Read;
	if (value == "readWrite") return DotNetSubtitleAccess::ReadWrite;
	throw std::runtime_error("Unsupported C# Macro subtitleAccess '" + value + "'");
}

DotNetSubtitleSnapshotScope ParseSubtitleSnapshotScope(std::string const& value) {
	if (value == "full") return DotNetSubtitleSnapshotScope::Full;
	if (value == "selection") return DotNetSubtitleSnapshotScope::Selection;
	throw std::runtime_error("Unsupported C# Macro snapshotScope '" + value + "'");
}

DotNetMacroQueryState ParseQueryState(
	json::Object const& macro,
	std::string const& source) {
	auto query_state_it = macro.find("queryState");
	if (query_state_it == macro.end())
		return {};

	json::Object const* object = nullptr;
	try {
		object = &static_cast<json::Object const&>(query_state_it->second);
	}
	catch (...) {
		throw std::runtime_error(source + " queryState must be an object");
	}
	for (auto const& [key, value] : *object) {
		(void)value;
		if (key != "requiresSubtitleFile" && key != "minimumSelectedEvents" &&
			key != "requiresActiveEvent" && key != "requiresVideo" &&
			key != "requiresAudio" && key != "requiresKeyframes")
			throw std::runtime_error(
				source + " queryState contains unsupported field '" + key + "'");
	}

	auto read_bool = [&](std::string const& key) {
		auto it = object->find(key);
		if (it == object->end()) return false;
		try {
			return static_cast<bool>(static_cast<json::Boolean const&>(it->second));
		}
		catch (...) {
			throw std::runtime_error(
				source + " queryState field '" + key + "' must be boolean");
		}
	};
	auto read_int = [&](std::string const& key) {
		auto it = object->find(key);
		if (it == object->end()) return 0;
		try {
			auto value = static_cast<int64_t>(static_cast<json::Integer const&>(it->second));
			if (value < std::numeric_limits<int>::min() ||
				value > std::numeric_limits<int>::max())
				throw std::out_of_range("queryState integer is outside the native range");
			return static_cast<int>(value);
		}
		catch (...) {
			throw std::runtime_error(
				source + " queryState field '" + key + "' must be an integer");
		}
	};

	DotNetMacroQueryState result;
	result.requires_subtitle_file = read_bool("requiresSubtitleFile");
	result.minimum_selected_events = read_int("minimumSelectedEvents");
	result.requires_active_event = read_bool("requiresActiveEvent");
	result.requires_video = read_bool("requiresVideo");
	result.requires_audio = read_bool("requiresAudio");
	result.requires_keyframes = read_bool("requiresKeyframes");
	if (result.minimum_selected_events < 0)
		throw std::runtime_error(
			source + " queryState minimumSelectedEvents cannot be negative");
	return result;
}

MacroManifest ParseMacroManifest(
	json::Object const& macro_object,
	std::set<std::string, std::less<>>& macro_ids) {
	MacroManifest macro;
	macro.id = RequireString(macro_object, "id");
	if (!macro_ids.insert(macro.id).second)
		throw std::runtime_error(
			"C# extension manifest contains duplicate Macro ID '" + macro.id + "'");
	macro.name = RequireString(macro_object, "name");
	macro.description = FindString(macro_object, "description").value_or("");
	macro.name_resource_key = FindString(macro_object, "nameResourceKey").value_or("");
	macro.description_resource_key = FindString(
		macro_object, "descriptionResourceKey").value_or("");
	macro.requires_project = FindBool(macro_object, "requiresProject").value_or(false);
	macro.subtitle_access = ParseSubtitleAccess(RequireString(macro_object, "subtitleAccess"));
	macro.snapshot_scope = ParseSubtitleSnapshotScope(
		FindString(macro_object, "snapshotScope").value_or("full"));
	if (macro.subtitle_access == DotNetSubtitleAccess::None &&
		macro.snapshot_scope != DotNetSubtitleSnapshotScope::Full)
		throw std::runtime_error(
			"C# extension manifest Macro '" + macro.id +
			"' cannot request snapshotScope when subtitleAccess is none");
	macro.query_state = ParseQueryState(
		macro_object, "C# extension manifest Macro '" + macro.id + "'");
	return macro;
}

ExtensionManifest ParseManifest(agi::fs::path const& filename) {
	std::ifstream stream(filename, std::ios::binary);
	if (!stream)
		throw std::runtime_error("Could not open C# extension manifest");

	json::UnknownElement root;
	json::Reader::Read(root, stream);
	auto const& object = static_cast<json::Object const&>(root);
	if (FindInt(object, "manifestVersion").value_or(0) != 2)
		throw std::runtime_error("Unsupported C# extension manifestVersion");

	ExtensionManifest manifest;
	manifest.id = RequireString(object, "id");
	manifest.name = RequireString(object, "name");
	manifest.description = FindString(object, "description").value_or("");
	manifest.name_resource_key = FindString(object, "nameResourceKey").value_or("");
	manifest.description_resource_key = FindString(object, "descriptionResourceKey").value_or("");
	manifest.author = FindString(object, "author").value_or("");
	manifest.version = FindString(object, "version").value_or("");
	manifest.contracts_version = RequireString(object, "contractsVersion");

	auto const& runtime = RequireObject(object, "runtime", "C# extension manifest");
	auto kind = RequireString(runtime, "kind");
	if (kind == "coreclr") {
		manifest.runtime_kind = DotNetRuntimeKind::CoreClr;
		manifest.entry_type = RequireString(runtime, "entryType");
		auto relative_entry_assembly = ParseRelativeEntryAssembly(
			RequireString(runtime, "entryAssembly"));
		manifest.entry_assembly = std::filesystem::absolute(
			filename.parent_path() / relative_entry_assembly).lexically_normal();
	}
	else if (kind == "native") {
		manifest.runtime_kind = DotNetRuntimeKind::Native;
		manifest.entry_point = FindString(runtime, "entryPoint").value_or(
			agi::coreclr::kNativePluginEntryPoint);
		if (manifest.entry_point != agi::coreclr::kNativePluginEntryPoint)
			throw std::runtime_error(
				"Native C ABI plugins must export '" +
					std::string(agi::coreclr::kNativePluginEntryPoint) + "'");

		auto library = FindString(runtime, "library");
		auto libraries_it = runtime.find("libraries");
		if (library && libraries_it != runtime.end())
			throw std::runtime_error(
				"Native runtime cannot declare both 'library' and 'libraries'");
		std::string selected_library;
		if (library) {
			selected_library = std::move(*library);
		}
		else {
			if (libraries_it == runtime.end())
				throw std::runtime_error(
					"Native runtime requires 'library' or a RID 'libraries' map");
			json::Object const* libraries = nullptr;
			try {
				libraries = &static_cast<json::Object const&>(libraries_it->second);
			}
			catch (...) {
				throw std::runtime_error(
					"Native runtime field 'libraries' must be an object");
			}
			auto rid = CurrentNativeRid();
			auto selected = libraries->find(rid);
			if (selected == libraries->end())
				throw std::runtime_error(
					"Native runtime does not provide a library for RID '" + rid + "'");
			try {
				selected_library = static_cast<json::String const&>(selected->second);
			}
			catch (...) {
				throw std::runtime_error(
					"Native runtime library for RID '" + rid + "' must be a string");
			}
		}
		auto relative_library = ParseRelativeManifestPath(
			std::move(selected_library), "native library");
		manifest.runtime_library = std::filesystem::absolute(
			filename.parent_path() / relative_library).lexically_normal();
	}
	else {
		throw std::runtime_error("Unsupported C# extension runtime kind '" + kind + "'");
	}

	auto const& contribution_items = RequireArray(
		object, "contributions", "C# extension manifest");
	std::set<std::string, std::less<>> contribution_ids;
	std::set<std::string, std::less<>> operation_ids;
	for (auto const& item : contribution_items) {
		json::Object const* contribution_object = nullptr;
		try {
			contribution_object = &static_cast<json::Object const&>(item);
		}
		catch (...) {
			throw std::runtime_error("C# extension manifest contributions must be objects");
		}
		ContributionManifest contribution;
		contribution.id = RequireString(*contribution_object, "id");
		if (!contribution_ids.insert(contribution.id).second)
			throw std::runtime_error(
				"C# extension manifest contains duplicate contribution ID '" +
				contribution.id + "'");
		contribution.kind = RequireString(*contribution_object, "kind");
		if (!IsKnownContributionKind(contribution.kind))
			throw std::runtime_error(
				"Unsupported C# extension contribution kind '" + contribution.kind + "'");
		contribution.name = FindString(*contribution_object, "name").value_or("");
		contribution.description = FindString(
			*contribution_object, "description").value_or("");
		contribution.name_resource_key = FindString(
			*contribution_object, "nameResourceKey").value_or("");
		contribution.description_resource_key = FindString(
			*contribution_object, "descriptionResourceKey").value_or("");
		if (contribution.kind == "automation") {
			auto const& macros = RequireArray(
				*contribution_object,
				"macros",
				"C# extension Automation contribution '" + contribution.id + "'");
			for (auto const& macro_item : macros) {
				json::Object const* macro_object = nullptr;
				try {
					macro_object = &static_cast<json::Object const&>(macro_item);
				}
				catch (...) {
					throw std::runtime_error(
						"C# extension Automation contribution Macros must be objects");
				}
				contribution.macros.emplace_back(
					ParseMacroManifest(*macro_object, operation_ids));
			}
		}
		else if (contribution.kind == "serviceProvider") {
			auto const& operations = RequireArray(
				*contribution_object,
				"operations",
				"C# extension service-provider contribution '" +
					contribution.id + "'");
			for (auto const& operation_item : operations) {
				std::string operation;
				try {
					operation = static_cast<json::String const&>(operation_item);
				}
				catch (...) {
					throw std::runtime_error(
						"C# extension service-provider operations must be strings");
				}
				if (operation.empty())
					throw std::runtime_error(
						"C# extension service-provider operation IDs cannot be empty");
				if (!operation_ids.insert(operation).second)
					throw std::runtime_error(
						"C# extension manifest contains duplicate operation ID '" +
							operation + "'");
				contribution.operations.emplace_back(std::move(operation));
			}
			if (contribution.operations.empty())
				throw std::runtime_error(
					"C# extension service-provider contribution must declare at least one operation");
		}
		if (contribution.kind != "automation" &&
			contribution_object->find("macros") != contribution_object->end()) {
			throw std::runtime_error(
				"C# extension contribution '" + contribution.id +
				"' cannot declare Automation Macros for kind '" + contribution.kind + "'");
		}
		if (contribution.kind != "serviceProvider" &&
			contribution_object->find("operations") != contribution_object->end())
			throw std::runtime_error(
				"C# extension contribution '" + contribution.id +
					"' cannot declare service operations for kind '" +
					contribution.kind + "'");
		manifest.contributions.emplace_back(std::move(contribution));
	}
	return manifest;
}

void ValidateManagedMacroMetadata(
	MacroManifest const& expected,
	json::Object const& actual,
	auto const& require_match) {
	require_match("macro.name", expected.name, RequireString(actual, "name"));
	require_match(
		"macro.description", expected.description,
		FindString(actual, "description").value_or(""));
	require_match(
		"macro.nameResourceKey", expected.name_resource_key,
		FindString(actual, "nameResourceKey").value_or(""));
	require_match(
		"macro.descriptionResourceKey", expected.description_resource_key,
		FindString(actual, "descriptionResourceKey").value_or(""));
	if (FindBool(actual, "requiresProject").value_or(false) != expected.requires_project)
		throw std::runtime_error(
			".NET extension Macro '" + expected.id + "' requiresProject does not match its manifest");
	require_match(
		"macro.subtitleAccess",
		expected.subtitle_access == DotNetSubtitleAccess::None ? "none" :
			expected.subtitle_access == DotNetSubtitleAccess::Read ? "read" : "readWrite",
		RequireString(actual, "subtitleAccess"));
	require_match(
		"macro.snapshotScope",
		expected.snapshot_scope == DotNetSubtitleSnapshotScope::Full ? "full" : "selection",
		RequireString(actual, "snapshotScope"));
	if (ParseQueryState(actual, ".NET extension Macro '" + expected.id + "'") !=
		expected.query_state)
		throw std::runtime_error(
			".NET extension Macro '" + expected.id +
			"' queryState does not match its manifest");
}

void ValidateManagedMetadata(
	ExtensionManifest const& manifest,
	std::string const& metadata_json) {
	std::istringstream stream(metadata_json);
	json::UnknownElement root;
	json::Reader::Read(root, stream);
	auto const& object = static_cast<json::Object const&>(root);

	auto require_match = [](std::string const& field, std::string const& expected, std::string const& actual) {
		if (expected != actual)
			throw std::runtime_error(
				".NET extension metadata field '" + field + "' does not match its manifest");
	};
	require_match("id", manifest.id, RequireString(object, "id"));
	require_match("name", manifest.name, RequireString(object, "name"));
	require_match("description", manifest.description, FindString(object, "description").value_or(""));
	require_match(
		"nameResourceKey", manifest.name_resource_key,
		FindString(object, "nameResourceKey").value_or(""));
	require_match(
		"descriptionResourceKey", manifest.description_resource_key,
		FindString(object, "descriptionResourceKey").value_or(""));
	require_match("author", manifest.author, FindString(object, "author").value_or(""));
	require_match("version", manifest.version, FindString(object, "version").value_or(""));
	require_match(
		"contractsVersion", manifest.contracts_version, RequireString(object, "contractsVersion"));

	auto const& managed_contributions = RequireArray(
		object, "contributions", ".NET extension metadata");
	if (managed_contributions.size() != manifest.contributions.size())
		throw std::runtime_error(
			".NET extension contribution count does not match its manifest");

	for (auto const& expected_contribution : manifest.contributions) {
		auto found_contribution = std::find_if(
			managed_contributions.begin(),
			managed_contributions.end(),
			[&](json::UnknownElement const& item) {
				auto const& candidate = static_cast<json::Object const&>(item);
				return FindString(candidate, "id").value_or("") == expected_contribution.id;
			});
		if (found_contribution == managed_contributions.end())
			throw std::runtime_error(
				".NET extension is missing manifest contribution '" +
				expected_contribution.id + "'");
		auto const& actual_contribution =
			static_cast<json::Object const&>(*found_contribution);
		require_match(
			"contribution.kind",
			expected_contribution.kind,
			RequireString(actual_contribution, "kind"));
		require_match(
			"contribution.name", expected_contribution.name,
			FindString(actual_contribution, "name").value_or(""));
		require_match(
			"contribution.description", expected_contribution.description,
			FindString(actual_contribution, "description").value_or(""));
		require_match(
			"contribution.nameResourceKey", expected_contribution.name_resource_key,
			FindString(actual_contribution, "nameResourceKey").value_or(""));
		require_match(
			"contribution.descriptionResourceKey",
			expected_contribution.description_resource_key,
			FindString(actual_contribution, "descriptionResourceKey").value_or(""));

		auto const& managed_operations = RequireArray(
			actual_contribution,
			"operations",
			".NET extension contribution '" + expected_contribution.id + "'");
		if (managed_operations.size() != expected_contribution.operations.size())
			throw std::runtime_error(
				".NET extension contribution '" + expected_contribution.id +
					"' operation count does not match its manifest");
		for (size_t operation_index = 0;
			operation_index < managed_operations.size();
			++operation_index) {
			std::string actual_operation;
			try {
				actual_operation = static_cast<json::String const&>(
					managed_operations[operation_index]);
			}
			catch (...) {
				throw std::runtime_error(
					".NET extension contribution operations must be strings");
			}
			if (actual_operation != expected_contribution.operations[operation_index])
				throw std::runtime_error(
					".NET extension contribution operation order does not match its manifest");
		}

		auto const& managed_macros = RequireArray(
			actual_contribution,
			"macros",
			".NET extension contribution '" + expected_contribution.id + "'");
		if (managed_macros.size() != expected_contribution.macros.size())
			throw std::runtime_error(
				".NET extension contribution '" + expected_contribution.id +
				"' Macro count does not match its manifest");
		for (auto const& expected_macro : expected_contribution.macros) {
			auto found_macro = std::find_if(
				managed_macros.begin(),
				managed_macros.end(),
				[&](json::UnknownElement const& item) {
					auto const& candidate = static_cast<json::Object const&>(item);
					return FindString(candidate, "id").value_or("") == expected_macro.id;
				});
			if (found_macro == managed_macros.end())
				throw std::runtime_error(
					".NET extension is missing manifest Macro '" + expected_macro.id + "'");
			ValidateManagedMacroMetadata(
				expected_macro,
				static_cast<json::Object const&>(*found_macro),
				require_match);
		}
	}
}

#ifdef _WIN32
constexpr wchar_t kNetHostFileName[] = L"nethost.dll";
#elif defined(__APPLE__)
constexpr char kNetHostFileName[] = "libnethost.dylib";
#else
constexpr char kNetHostFileName[] = "libnethost.so";
#endif

void AEGISUB_PLUGIN_BRIDGE_CALL AdapterLog(uint8_t const* message, int32_t length) {
	if (!message || length < 0) return;
	std::clog << "[automation/plugin_bridge] "
		<< std::string(reinterpret_cast<char const*>(message), static_cast<size_t>(length))
		<< '\n';
}

void WriteCSharpDebugContext(std::string const& context_json) {
	auto const* configured = std::getenv("AEGISUB_CSHARP_CONTEXT_DUMP");
	if (!configured || !*configured)
		return;

	auto path = std::filesystem::absolute(std::filesystem::u8path(configured));
	if (!path.parent_path().empty())
		std::filesystem::create_directories(path.parent_path());
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	if (!stream)
		throw std::runtime_error(
			"Could not open AEGISUB_CSHARP_CONTEXT_DUMP output '" +
				agi::fs::PathToString(path) + "'");
	stream.write(context_json.data(), static_cast<std::streamsize>(context_json.size()));
	if (!stream)
		throw std::runtime_error(
			"Could not write AEGISUB_CSHARP_CONTEXT_DUMP output '" +
				agi::fs::PathToString(path) + "'");
	LOG_I("automation/plugin_bridge")
		<< "Wrote C# Macro debug context to " << agi::fs::PathToString(path);
}

std::mutex invocation_services_mutex;
struct CSharpInvocationServices {
	AutomationProgress *progress = nullptr;
	agi::Context* context = nullptr;
};
std::map<int64_t, CSharpInvocationServices> invocation_services;

class ScopedCSharpInvocationServices final {
	int64_t token;

public:
	ScopedCSharpInvocationServices(
		int64_t token,
		AutomationProgress* progress,
		agi::Context* context)
	: token(token) {
		std::lock_guard<std::mutex> lock(invocation_services_mutex);
		if (!progress || !context || token <= 0 ||
			!invocation_services.emplace(
				token, CSharpInvocationServices{progress, context}).second)
			throw std::logic_error("Could not register C# Macro invocation services");
	}

	~ScopedCSharpInvocationServices() {
		std::lock_guard<std::mutex> lock(invocation_services_mutex);
		invocation_services.erase(token);
	}
};

int32_t AEGISUB_PLUGIN_BRIDGE_CALL IsManagedInvocationCancellationRequested(
	int64_t invocation_token) {
	std::lock_guard<std::mutex> lock(invocation_services_mutex);
	auto it = invocation_services.find(invocation_token);
	return it != invocation_services.end() && it->second.progress->IsCancelled() ? 1 : 0;
}

int32_t AEGISUB_PLUGIN_BRIDGE_CALL ReportManagedInvocationProgress(
	int64_t invocation_token,
	int64_t current,
	int64_t maximum,
	uint8_t const* message,
	int32_t message_length) {
	using agi::coreclr::BridgeStatus;
	if (invocation_token <= 0 || current < 0 || maximum < 0 ||
		(maximum > 0 && current > maximum) || message_length < 0 ||
		(message_length > 0 && !message))
		return static_cast<int32_t>(BridgeStatus::InvalidArgument);

	std::lock_guard<std::mutex> lock(invocation_services_mutex);
	auto it = invocation_services.find(invocation_token);
	if (it == invocation_services.end())
		return static_cast<int32_t>(BridgeStatus::InvalidHandle);
	try {
		if (maximum == 0)
			it->second.progress->SetIndeterminate();
		else
			it->second.progress->SetProgress(current, maximum);
		if (message_length > 0)
			it->second.progress->SetMessage(std::string(
				reinterpret_cast<char const*>(message),
				static_cast<size_t>(message_length)));
		return static_cast<int32_t>(BridgeStatus::Success);
	}
	catch (...) {
		return static_cast<int32_t>(BridgeStatus::AdapterException);
	}
}

struct PendingHostServiceCall {
	uint64_t plugin_handle = 0;
	int64_t invocation_token = 0;
	std::string service_id;
	std::string request_json;
	std::string result_json;
};

thread_local std::optional<PendingHostServiceCall> pending_host_service_call;

void DispatchHostEventToManagedPlugin(
	uint64_t plugin_handle,
	std::string const& event_id,
	std::string const& payload_json);

int32_t AEGISUB_PLUGIN_BRIDGE_CALL InvokeManagedPluginHostService(
	uint64_t plugin_handle,
	int64_t invocation_token,
	uint8_t const* service_id,
	int32_t service_id_length,
	uint8_t const* request_json,
	int32_t request_json_length,
	uint8_t* buffer,
	int32_t capacity,
	int32_t* payload_length) {
	using agi::coreclr::BridgeStatus;
	if (!plugin_handle || invocation_token < 0 || !service_id || service_id_length <= 0 ||
		!request_json || request_json_length <= 0 || capacity < 0 || !payload_length)
		return static_cast<int32_t>(BridgeStatus::InvalidArgument);

	auto service = std::string(
		reinterpret_cast<char const*>(service_id), static_cast<size_t>(service_id_length));
	auto request = std::string(
		reinterpret_cast<char const*>(request_json), static_cast<size_t>(request_json_length));
	auto& pending = pending_host_service_call;
	if (!pending || pending->plugin_handle != plugin_handle ||
		pending->invocation_token != invocation_token || pending->service_id != service ||
		pending->request_json != request) {
		pending.reset();
		std::string result;
		try {
			agi::Context* context = nullptr;
			if (invocation_token > 0) {
				std::lock_guard<std::mutex> lock(invocation_services_mutex);
				auto it = invocation_services.find(invocation_token);
				if (it == invocation_services.end())
					return static_cast<int32_t>(BridgeStatus::InvalidHandle);
				context = it->second.context;
			}
			auto ui_result = agi::coreclr::ui::InvokeDeclarativeUiHostService(
				plugin_handle,
				context,
				service,
				request,
				DispatchHostEventToManagedPlugin);
			if (ui_result)
				result = std::move(*ui_result);
			else if (service == "aegisub.bridge.echo")
				result = request;
			else if (service == "aegisub.host.info")
				result = R"({"host":"Aegisub","transport":"coreclr"})";
			else if (auto dependency_control_result =
				InvokePackageTransactionHostService(plugin_handle, service, request))
				result = std::move(*dependency_control_result);
			else
				return static_cast<int32_t>(BridgeStatus::NotFound);
		}
		catch (std::exception const& error) {
			LOG_E("automation/plugin_bridge")
				<< "CLR plugin host service '" << service << "' failed: " << error.what();
			return static_cast<int32_t>(BridgeStatus::HostServiceFailed);
		}
		pending = PendingHostServiceCall{
			plugin_handle,
			invocation_token,
			std::move(service),
			std::move(request),
			std::move(result)
		};
	}

	if (pending->result_json.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
		pending.reset();
		return static_cast<int32_t>(BridgeStatus::HostServiceFailed);
	}
	*payload_length = static_cast<int32_t>(pending->result_json.size());
	if (!buffer || capacity <= *payload_length)
		return static_cast<int32_t>(BridgeStatus::BufferTooSmall);
	std::memcpy(buffer, pending->result_json.data(), pending->result_json.size());
	buffer[pending->result_json.size()] = 0;
	pending.reset();
	return static_cast<int32_t>(BridgeStatus::Success);
}

int32_t AEGISUB_PLUGIN_BRIDGE_CALL EmitManagedPluginEvent(
	uint64_t plugin_handle,
	uint8_t const* event_id,
	int32_t event_id_length,
	uint8_t const* payload_json,
	int32_t payload_json_length) {
	using agi::coreclr::BridgeStatus;
	if (!plugin_handle || !event_id || event_id_length <= 0 ||
		!payload_json || payload_json_length <= 0)
		return static_cast<int32_t>(BridgeStatus::InvalidArgument);
	try {
		LOG_I("automation/plugin_bridge")
			<< "CLR plugin " << plugin_handle << " emitted event '"
			<< std::string(
				reinterpret_cast<char const*>(event_id), static_cast<size_t>(event_id_length))
			<< "': "
			<< std::string(
				reinterpret_cast<char const*>(payload_json),
				static_cast<size_t>(payload_json_length));
		return static_cast<int32_t>(BridgeStatus::Success);
	}
	catch (...) {
		return static_cast<int32_t>(BridgeStatus::EventDispatchFailed);
	}
}

class DotNetAutomationRuntime final {
	struct ExtensionEntry {
		explicit ExtensionEntry(ExtensionManifest manifest)
		: manifest(std::move(manifest)) {
		}

		std::mutex execution_mutex;
		ExtensionManifest manifest;
		size_t reference_count = 0;
		uint64_t handle = 0;
		bool metadata_validated = false;
		std::unique_ptr<agi::coreclr::AdapterBridge> native_bridge;
	};

	std::mutex registry_mutex;
	std::mutex bridge_mutex;
	std::unique_ptr<agi::coreclr::AdapterBridge> bridge;
	std::map<std::string, std::shared_ptr<ExtensionEntry>, std::less<>> extensions;
	PluginServiceProviderRegistry service_providers;
	std::atomic<uint64_t> next_native_handle{uint64_t{1} << 63};

	static std::string ExtensionKey(ExtensionManifest const& manifest) {
		auto path = manifest.runtime_kind == DotNetRuntimeKind::CoreClr
			? agi::fs::PathToGenericString(manifest.entry_assembly)
			: agi::fs::PathToGenericString(manifest.runtime_library);
		return std::to_string(static_cast<int>(manifest.runtime_kind)) +
			"\n" + path + "\n" + manifest.entry_type + "\n" + manifest.entry_point;
	}

	static bool SameManifestDefinition(
		ExtensionManifest const& left,
		ExtensionManifest const& right) {
		if (left.id != right.id || left.name != right.name ||
			left.description != right.description ||
			left.name_resource_key != right.name_resource_key ||
			left.description_resource_key != right.description_resource_key ||
			left.author != right.author ||
			left.version != right.version || left.contracts_version != right.contracts_version ||
			left.runtime_kind != right.runtime_kind ||
			left.entry_assembly != right.entry_assembly ||
			left.runtime_library != right.runtime_library ||
			left.entry_type != right.entry_type ||
			left.entry_point != right.entry_point ||
			left.contributions.size() != right.contributions.size())
			return false;
		for (size_t contribution_index = 0;
			contribution_index < left.contributions.size();
			++contribution_index) {
			auto const& left_contribution = left.contributions[contribution_index];
			auto const& right_contribution = right.contributions[contribution_index];
			if (left_contribution.id != right_contribution.id ||
				left_contribution.kind != right_contribution.kind ||
				left_contribution.name != right_contribution.name ||
				left_contribution.description != right_contribution.description ||
				left_contribution.name_resource_key != right_contribution.name_resource_key ||
				left_contribution.description_resource_key !=
					right_contribution.description_resource_key ||
				left_contribution.operations != right_contribution.operations ||
				left_contribution.macros.size() != right_contribution.macros.size())
				return false;
			for (size_t macro_index = 0;
				macro_index < left_contribution.macros.size();
				++macro_index) {
				auto const& lhs = left_contribution.macros[macro_index];
				auto const& rhs = right_contribution.macros[macro_index];
				if (lhs.id != rhs.id || lhs.name != rhs.name ||
					lhs.description != rhs.description ||
					lhs.name_resource_key != rhs.name_resource_key ||
					lhs.description_resource_key != rhs.description_resource_key ||
					lhs.requires_project != rhs.requires_project ||
					lhs.subtitle_access != rhs.subtitle_access ||
					lhs.snapshot_scope != rhs.snapshot_scope ||
					lhs.query_state != rhs.query_state)
					return false;
			}
		}
		return true;
	}

	static agi::coreclr::NativeHostApi BuildNativeHostApi() {
		agi::coreclr::NativeHostApi native_api;
		native_api.log_utf8 = AdapterLog;
		native_api.is_cancellation_requested = IsManagedInvocationCancellationRequested;
		native_api.report_progress = ReportManagedInvocationProgress;
		native_api.invoke_host_service_utf8 = InvokeManagedPluginHostService;
		native_api.emit_plugin_event_utf8 = EmitManagedPluginEvent;
		return native_api;
	}

	uint64_t AllocateNativeHandle() {
		auto handle = next_native_handle.fetch_add(1, std::memory_order_relaxed);
		if (handle == 0 || handle < (uint64_t{1} << 63))
			throw std::overflow_error("Native plugin handle space is exhausted");
		return handle;
	}

	agi::coreclr::AdapterBridge& EnsureInitialized() {
		std::lock_guard<std::mutex> bridge_lock(bridge_mutex);
		if (bridge) return *bridge;

		auto executable_dir = agi::coreclr::GetCurrentExecutableDirectory();
		auto component_dir = executable_dir / "plugins" / "coreclr";
		auto runtime_config = component_dir / "Aegisub.CoreClr.Adapter.runtimeconfig.json";
		auto nethost_path = component_dir / kNetHostFileName;
		agi::fs::path dotnet_root;
		if (auto const* configured_root = std::getenv("AEGISUB_DOTNET_ROOT"); configured_root && *configured_root) {
			dotnet_root = std::filesystem::absolute(std::filesystem::u8path(configured_root));
			LOG_I("automation/plugin_bridge")
				<< "Using explicit Plugin Bridge dotnet root "
				<< agi::fs::PathToString(dotnet_root);
		}
		else {
			auto app_local_root = executable_dir / ".dotnet";
			if (std::filesystem::is_directory(app_local_root)) {
				try {
					auto probe = agi::coreclr::Probe({
						nethost_path,
						runtime_config,
						app_local_root
					});
					dotnet_root = std::move(app_local_root);
					LOG_I("automation/plugin_bridge")
						<< "Using app-local Plugin Bridge Runtime "
						<< agi::fs::PathToString(dotnet_root)
						<< " (hostfxr=" << agi::fs::PathToString(probe.hostfxr_path) << ")";
				}
				catch (std::exception const& error) {
					LOG_W("automation/plugin_bridge")
						<< "Ignoring unusable app-local .dotnet Runtime at "
						<< agi::fs::PathToString(app_local_root) << ": " << error.what()
						<< "; falling back to the system-registered Runtime";
				}
			}
			if (dotnet_root.empty())
				LOG_I("automation/plugin_bridge")
					<< "Using the system-registered Plugin Bridge Runtime";
		}

		auto native_api = BuildNativeHostApi();
		bridge = std::make_unique<agi::coreclr::AdapterBridge>(
			agi::coreclr::AdapterBridgeOptions{
				{
					nethost_path,
					runtime_config,
					dotnet_root
				},
				component_dir / "Aegisub.CoreClr.Adapter.dll",
				native_api
			});
		return *bridge;
	}

	void EnsureEntryLoaded(ExtensionEntry& entry) {
		if (entry.handle && !entry.metadata_validated) {
			// Defensive cleanup: never invoke a handle which has not completed
			// manifest/assembly metadata validation.
			auto stale_handle = std::exchange(entry.handle, 0);
			entry.metadata_validated = false;
			AbortPackageTransactions(stale_handle);
			if (entry.native_bridge)
				entry.native_bridge->UnloadPlugin(stale_handle);
			else
				bridge->UnloadPlugin(stale_handle);
		}
		if (entry.handle) return;

		agi::coreclr::LoadedPlugin loaded;
		if (entry.manifest.runtime_kind == DotNetRuntimeKind::CoreClr) {
			loaded = EnsureInitialized().LoadPlugin(
				entry.manifest.entry_assembly, entry.manifest.entry_type);
		}
		else {
			auto native_handle = AllocateNativeHandle();
			auto native_bridge = std::make_unique<agi::coreclr::AdapterBridge>(
				agi::coreclr::NativeAdapterBridgeOptions{
					entry.manifest.runtime_library,
					native_handle,
					BuildNativeHostApi()
				});
			loaded = native_bridge->LoadNativePlugin(native_handle);
			entry.native_bridge = std::move(native_bridge);
		}
		try {
			ValidateManagedMetadata(entry.manifest, loaded.metadata_json);
		}
		catch (...) {
			try {
				AbortPackageTransactions(loaded.handle);
				if (entry.native_bridge)
					entry.native_bridge->UnloadPlugin(loaded.handle);
				else
					bridge->UnloadPlugin(loaded.handle);
			}
			catch (std::exception const& unload_error) {
				LOG_E("automation/plugin_bridge")
					<< "CLR plugin cleanup after metadata validation reported a failure: "
					<< unload_error.what();
			}
			throw;
		}
		entry.handle = loaded.handle;
		entry.metadata_validated = true;
	}

public:
	void Shutdown() noexcept {
		ShutdownPackageTransactionHost();
		service_providers.Clear();
		std::vector<std::shared_ptr<ExtensionEntry>> native_entries;
		{
			std::lock_guard<std::mutex> lock(registry_mutex);
			for (auto const& [key, entry] : extensions) {
				(void)key;
				if (entry->native_bridge) native_entries.push_back(entry);
			}
		}
		for (auto const& entry : native_entries) {
			std::lock_guard<std::mutex> execution_lock(entry->execution_mutex);
			if (!entry->native_bridge) continue;
			try {
				if (entry->handle) {
					agi::coreclr::ui::CloseDeclarativeUiViewsForPlugin(entry->handle);
					AbortPackageTransactions(entry->handle);
					entry->native_bridge->UnloadPlugin(entry->handle);
				}
				entry->native_bridge->Shutdown();
			}
			catch (std::exception const& error) {
				std::cerr << "[automation/plugin_bridge] Native plugin shutdown failed: "
					<< error.what() << '\n';
			}
			entry->handle = 0;
			entry->metadata_validated = false;
		}
		std::unique_ptr<agi::coreclr::AdapterBridge> active_bridge;
		{
			std::lock_guard<std::mutex> lock(bridge_mutex);
			active_bridge = std::move(bridge);
		}
		if (!active_bridge) return;
		try {
			active_bridge->Shutdown();
		}
		catch (agi::coreclr::BridgeException const& error) {
			std::cerr << "[automation/plugin_bridge] CoreCLR Adapter Bridge shutdown failed: ";
			if (auto const& envelope = error.Error())
				std::cerr << '[' << envelope->code << "] " << envelope->message;
			else if (!error.RawError().empty())
				std::cerr << error.RawError();
			else
				std::cerr << error.what();
			std::cerr << '\n';
		}
		catch (std::exception const& error) {
			std::cerr << "[automation/plugin_bridge] CoreCLR Adapter Bridge shutdown failed: "
				<< error.what() << '\n';
		}
		std::lock_guard<std::mutex> lock(registry_mutex);
		for (auto const& [key, entry] : extensions) {
			(void)key;
			entry->handle = 0;
			entry->metadata_validated = false;
		}
	}

	~DotNetAutomationRuntime() { Shutdown(); }

	std::string AcquireExtension(ExtensionManifest const& manifest) {
		std::lock_guard<std::mutex> lock(registry_mutex);
		auto key = ExtensionKey(manifest);
		for (auto const& [existing_key, existing] : extensions) {
			if (existing_key != key &&
				existing->manifest.runtime_kind == DotNetRuntimeKind::Native &&
				existing->manifest.id == manifest.id)
				throw std::runtime_error(
					"Native plugin '" + manifest.id +
					"' changed binaries while the process is running; restart Aegisub to activate it");
		}
		auto [it, inserted] = extensions.try_emplace(
			key,
			std::make_shared<ExtensionEntry>(manifest));
		if (!inserted && !SameManifestDefinition(it->second->manifest, manifest))
			throw std::runtime_error("Conflicting C# manifests reference the same entry assembly and type");
		++it->second->reference_count;
		return key;
	}

	void ReleaseExtension(std::string const& key) noexcept {
		std::shared_ptr<ExtensionEntry> entry;
		bool retain_native = false;
		{
			std::lock_guard<std::mutex> lock(registry_mutex);
			auto it = extensions.find(key);
			if (it == extensions.end()) return;
			if (it->second->reference_count > 1) {
				--it->second->reference_count;
				return;
			}
			entry = it->second;
			entry->reference_count = 0;
			retain_native = entry->manifest.runtime_kind == DotNetRuntimeKind::Native;
		}

		std::lock_guard<std::mutex> execution_lock(entry->execution_mutex);
		auto handle = entry->handle;
		if (retain_native) {
			if (handle) {
				try {
					agi::coreclr::ui::CloseDeclarativeUiViewsForPlugin(handle);
				}
				catch (std::exception const& error) {
					LOG_E("automation/plugin_bridge")
						<< "Native plugin view cleanup failed: " << error.what();
				}
			}
			return;
		}
		if (handle && bridge) {
			try {
				agi::coreclr::ui::CloseDeclarativeUiViewsForPlugin(handle);
				AbortPackageTransactions(handle);
				bridge->UnloadPlugin(handle);
			}
			catch (std::exception const& error) {
				LOG_E("automation/plugin_bridge")
					<< "CLR plugin unload reported a failure after consuming its handle: "
					<< error.what();
			}
			entry->handle = 0;
			entry->metadata_validated = false;
		}
		{
			std::lock_guard<std::mutex> lock(registry_mutex);
			auto it = extensions.find(key);
			if (it != extensions.end() && it->second == entry &&
				entry->reference_count == 0)
				extensions.erase(it);
		}
	}

	std::string InvokeContribution(
		std::string const& key,
		std::string const& contribution_id,
		std::string const& operation_id,
		std::string const& request_json) {
		std::shared_ptr<ExtensionEntry> entry;
		{
			std::lock_guard<std::mutex> lock(registry_mutex);
			auto it = extensions.find(key);
			if (it == extensions.end())
				throw std::logic_error("CLR plugin reference is no longer registered");
			entry = it->second;
		}
		std::lock_guard<std::mutex> execution_lock(entry->execution_mutex);
		EnsureEntryLoaded(*entry);
		auto& active_bridge = entry->native_bridge
			? *entry->native_bridge
			: EnsureInitialized();
		return active_bridge.InvokeContributionUtf8(
			entry->handle,
			contribution_id,
			operation_id,
			request_json);
	}

	void ValidateExtension(std::string const& key) {
		std::shared_ptr<ExtensionEntry> entry;
		{
			std::lock_guard<std::mutex> lock(registry_mutex);
			auto it = extensions.find(key);
			if (it == extensions.end())
				throw std::logic_error("CLR plugin reference is no longer registered");
			entry = it->second;
		}
		std::lock_guard<std::mutex> execution_lock(entry->execution_mutex);
		EnsureEntryLoaded(*entry);
	}

	void DispatchPluginEvent(
		uint64_t plugin_handle,
		std::string const& event_id,
		std::string const& payload_json) {
		std::vector<std::shared_ptr<ExtensionEntry>> entries;
		{
			std::lock_guard<std::mutex> lock(registry_mutex);
			for (auto const& [key, entry] : extensions) {
				(void)key;
				entries.push_back(entry);
			}
		}
		for (auto const& entry : entries) {
			std::lock_guard<std::mutex> execution_lock(entry->execution_mutex);
			if (entry->handle != plugin_handle) continue;
			if (!entry->metadata_validated)
				throw std::logic_error("CLR plugin is not ready to receive host events");
			if (entry->native_bridge)
				entry->native_bridge->DispatchPluginEventUtf8(
					plugin_handle, event_id, payload_json);
			else
				EnsureInitialized().DispatchPluginEventUtf8(
					plugin_handle, event_id, payload_json);
			return;
		}
		throw std::runtime_error("CLR plugin handle is no longer registered");
	}

	void RegisterServiceProvider(
		std::string const& contribution_id,
		std::string const& extension_key,
		std::vector<std::string> operations) {
		service_providers.Register(
			contribution_id, extension_key, std::move(operations));
	}

	void UnregisterServiceProvider(
		std::string const& contribution_id,
		std::string const& extension_key) noexcept {
		service_providers.Unregister(contribution_id, extension_key);
	}

	bool HasServiceProvider(std::string const& contribution_id) const {
		return service_providers.Contains(contribution_id);
	}

	std::string InvokeServiceContribution(
		std::string const& contribution_id,
		std::string const& operation_id,
		std::string const& request_json) {
		auto registration = service_providers.Find(contribution_id);
		if (!registration)
			throw std::runtime_error(
				"No loaded plugin provides service contribution '" +
				contribution_id + "'");

		if (!registration->operations.empty() &&
			std::find(
				registration->operations.begin(),
				registration->operations.end(),
				operation_id) == registration->operations.end()) {
			throw std::runtime_error(
				"Plugin service contribution '" + contribution_id +
				"' does not declare operation '" + operation_id + "'");
		}

		return InvokeContribution(
			registration->extension_key,
			contribution_id,
			operation_id,
			request_json);
	}
};

std::shared_ptr<DotNetAutomationRuntime> Runtime() {
	static auto runtime = std::make_shared<DotNetAutomationRuntime>();
	return runtime;
}

void DispatchHostEventToManagedPlugin(
	uint64_t plugin_handle,
	std::string const& event_id,
	std::string const& payload_json) {
	Runtime()->DispatchPluginEvent(plugin_handle, event_id, payload_json);
}

class DotNetExtensionReference final {
	std::shared_ptr<DotNetAutomationRuntime> runtime;
	std::string key;

public:
	DotNetExtensionReference(
		std::shared_ptr<DotNetAutomationRuntime> runtime,
		ExtensionManifest const& manifest)
	: runtime(std::move(runtime))
	, key(this->runtime->AcquireExtension(manifest)) {
	}

	~DotNetExtensionReference() {
		runtime->ReleaseExtension(key);
	}

	std::string const& Key() const noexcept { return key; }
	void UnregisterServiceProvider(std::string const& contribution_id) noexcept {
		runtime->UnregisterServiceProvider(contribution_id, key);
	}

	std::string InvokeContribution(
		std::string const& contribution_id,
		std::string const& operation_id,
		std::string const& request_json) const {
		return runtime->InvokeContribution(
			key,
			contribution_id,
			operation_id,
			request_json);
	}

	void ValidateApplicationActivation() const {
		runtime->ValidateExtension(key);
	}
};

/// Keeps lazily bootstrapped extensions alive so their serviceProvider entries
/// remain registered without an Autoload ScriptInstance (headless, pre-autoload
/// Lua require("l0.DependencyControl"), etc.).
struct LazyServiceBootstrapHold {
	std::shared_ptr<DotNetExtensionReference> extension;
	std::vector<std::string> service_ids;
	size_t registered_count = 0;

	~LazyServiceBootstrapHold() {
		if (!extension)
			return;
		for (size_t index = 0; index < registered_count; ++index)
			extension->UnregisterServiceProvider(service_ids[index]);
	}
};

struct LazyServiceBootstrapState {
	std::mutex mutex;
	std::vector<std::unique_ptr<LazyServiceBootstrapHold>> holds;
};

LazyServiceBootstrapState& LazyServiceBootstrap() {
	static LazyServiceBootstrapState state;
	return state;
}

void ClearLazyServiceBootstrap() noexcept {
	auto& state = LazyServiceBootstrap();
	std::lock_guard<std::mutex> lock(state.mutex);
	state.holds.clear();
}

std::optional<agi::fs::path> FindAppLocalPluginManifestPath(agi::fs::path const& plugin_dir) {
	static constexpr char const* names[] = {
		"plugin.aegisub-plugin.json",
		"plugin.json",
	};
	for (auto const* name : names) {
		auto candidate = plugin_dir / name;
		if (agi::fs::FileExists(candidate))
			return candidate;
	}
	return std::nullopt;
}

/// On service registry miss, scan <exe>/plugins/*/ for a payload that declares
/// the requested serviceProvider and load it. Does not register automation
/// macros (menus stay autoload-only).
void EnsureServiceProviderFromAppLocalPlugins(std::string const& contribution_id) {
	if (contribution_id.empty())
		return;
	if (Runtime()->HasServiceProvider(contribution_id))
		return;

	auto& state = LazyServiceBootstrap();
	std::lock_guard<std::mutex> lock(state.mutex);
	if (Runtime()->HasServiceProvider(contribution_id))
		return;

	agi::fs::path plugins_root;
	try {
		plugins_root = agi::coreclr::GetCurrentExecutableDirectory() / "plugins";
	}
	catch (std::exception const& error) {
		LOG_W("automation/plugin_bridge")
			<< "Could not resolve app-local plugins for service bootstrap: "
			<< error.what();
		return;
	}
	if (!agi::fs::DirectoryExists(plugins_root))
		return;

	try {
		for (auto entry : agi::fs::DirectoryIterator(plugins_root, "*")) {
			auto plugin_dir = plugins_root / agi::fs::PathFromString(entry);
			if (!agi::fs::DirectoryExists(plugin_dir))
				continue;
			auto dirname = agi::fs::PathToString(plugin_dir.filename());
			if (dirname == "coreclr")
				continue;

			auto manifest_path = FindAppLocalPluginManifestPath(plugin_dir);
			if (!manifest_path)
				continue;

			try {
				auto manifest = ParseManifest(*manifest_path);
				bool provides = false;
				for (auto const& contribution : manifest.contributions) {
					if (contribution.kind == "serviceProvider" &&
						contribution.id == contribution_id) {
						provides = true;
						break;
					}
				}
				if (!provides)
					continue;

				auto hold = std::make_unique<LazyServiceBootstrapHold>();
				hold->extension = std::make_shared<DotNetExtensionReference>(
					Runtime(), manifest);
				hold->extension->ValidateApplicationActivation();
				for (auto const& contribution : manifest.contributions) {
					if (contribution.kind == "serviceProvider")
						hold->service_ids.push_back(contribution.id);
				}
				state.holds.push_back(std::move(hold));
				auto& registered_hold = *state.holds.back();
				try {
					for (auto const& contribution : manifest.contributions) {
						if (contribution.kind != "serviceProvider")
							continue;
						Runtime()->RegisterServiceProvider(
							contribution.id,
							registered_hold.extension->Key(),
							contribution.operations);
						++registered_hold.registered_count;
					}
				}
				catch (...) {
					state.holds.pop_back();
					throw;
				}
				LOG_I("automation/plugin_bridge")
					<< "Lazily bootstrapped app-local plugin '" << dirname
					<< "' for service contribution '" << contribution_id << "'";
				if (Runtime()->HasServiceProvider(contribution_id))
					return;
			}
			catch (std::exception const& error) {
				LOG_W("automation/plugin_bridge")
					<< "App-local service bootstrap failed for '" << dirname
					<< "': " << error.what();
			}
		}
	}
	catch (std::exception const& error) {
		LOG_W("automation/plugin_bridge")
			<< "Could not enumerate app-local plugins for service bootstrap: "
			<< error.what();
	}
}

struct DotNetAutomationHostState {
	std::shared_ptr<AutomationHost> host;
};

std::atomic<int64_t> next_invocation_token{1};

class DotNetMacro final : public cmd::Command {
	std::shared_ptr<DotNetExtensionReference> extension;
	std::shared_ptr<DotNetAutomationHostState> host_state;
	std::string command_name;
	std::string contribution_id;
	std::string macro_id;
	wxString display;
	wxString help;
	bool requires_project = false;
	DotNetSubtitleAccess subtitle_access = DotNetSubtitleAccess::None;
	DotNetSubtitleSnapshotScope snapshot_scope = DotNetSubtitleSnapshotScope::Full;
	DotNetMacroQueryState query_state;

public:
	DotNetMacro(
		std::shared_ptr<DotNetExtensionReference> extension,
		std::shared_ptr<DotNetAutomationHostState> host_state,
		std::string contribution_id,
		MacroManifest metadata)
	:	extension(std::move(extension))
	,	host_state(std::move(host_state))
	,	command_name(metadata.id)
	,	contribution_id(std::move(contribution_id))
	,	macro_id(std::move(metadata.id))
	,	display(to_wx(metadata.name))
	,	help(to_wx(metadata.description))
	,	requires_project(metadata.requires_project)
	,	subtitle_access(metadata.subtitle_access)
	,	snapshot_scope(metadata.snapshot_scope)
	,	query_state(metadata.query_state) {
	}

	char const* name() const override { return command_name.c_str(); }
	wxString StrMenu(agi::Context const*) const override { return display; }
	wxString StrDisplay(agi::Context const*) const override { return display; }
	wxString StrHelp() const override { return help; }
	int Type() const override {
		return requires_project || !query_state.IsUnconditional()
			? cmd::COMMAND_VALIDATE
			: cmd::COMMAND_NORMAL;
	}
	bool Validate(agi::Context const* context) override {
		if (!context)
			return !requires_project && query_state.IsUnconditional();
		if (query_state.IsUnconditional())
			return true;

		auto core = context->GetCore();
		DotNetMacroHostStateSnapshot state;
		state.has_subtitle_file = core.subsController->HasFile();
		state.selected_event_count = core.selectionController->GetSelectedSet().size();
		state.has_active_event = core.selectionController->GetActiveLine() != nullptr;
		state.has_video = core.project->VideoProvider() != nullptr;
		state.has_audio = core.project->AudioProvider() != nullptr;
		state.has_keyframes = !core.project->Keyframes().empty();
		return MatchesDotNetMacroQueryState(query_state, state);
	}

	void operator()(agi::Context* context) override {
		try {
			auto host = host_state->host;
			if (!host || host->ProjectContextIdentity() != context) {
				host = CreateAutomationLiveHost(context);
				host_state->host = host;
			}

			auto token = next_invocation_token.fetch_add(1, std::memory_order_relaxed);
			if (token <= 0)
				throw std::overflow_error("C# Macro invocation token space was exhausted");

			DotNetMacroExecutionResult result;
			std::exception_ptr invocation_error;
			auto runner = host->Ui().CreateBackgroundScriptRunner(from_wx(display));
			runner->Run([&](AutomationProgress *progress) {
				ScopedCSharpInvocationServices services(token, progress, context);
				try {
					auto context_json = BuildDotNetMacroContextJson(
						context,
						token,
						subtitle_access != DotNetSubtitleAccess::None,
						snapshot_scope,
						progress);
					WriteCSharpDebugContext(context_json);
					result = ParseDotNetMacroResultJson(
						extension->InvokeContribution(
							contribution_id,
							macro_id,
							context_json));
				}
				catch (...) {
					invocation_error = std::current_exception();
				}
			});
			if (invocation_error)
				std::rethrow_exception(invocation_error);
			if (result.mutation && subtitle_access != DotNetSubtitleAccess::ReadWrite)
				throw std::runtime_error(
					"C# Macro returned subtitle mutations without readWrite subtitleAccess");
			if (result.mutation)
				ApplyDotNetMacroMutation(context, host.get(), *result.mutation);
			if (host && !result.status_message.empty())
				host->Ui().ShowStatus(result.status_message);
		}
		catch (agi::UserCancelException const&) {
			LOG_I("automation/plugin_bridge") << "C# Automation Macro cancelled";
			return;
		}
		catch (agi::coreclr::InvocationCancelled const& error) {
			LOG_I("automation/plugin_bridge") << "C# Automation Macro cancelled: " << error.what();
			throw agi::UserCancelException("C# Automation Macro cancelled");
		}
		catch (agi::coreclr::BridgeException const& error) {
			if (auto const& envelope = error.Error()) {
				LOG_E("automation/plugin_bridge")
					<< "C# Automation Macro failed: status=" << error.Status()
					<< ", category=" << agi::coreclr::ToString(envelope->category)
					<< ", code=" << envelope->code
					<< ", retryable=" << (envelope->retryable ? "true" : "false")
					<< ", message=" << envelope->message;
				if (!envelope->details.empty())
					LOG_D("automation/plugin_bridge") << envelope->details;
			}
			else {
				LOG_E("automation/plugin_bridge") << "C# Automation Macro failed: " << error.what();
			}
			throw;
		}
		catch (std::exception const& error) {
			LOG_E("automation/plugin_bridge") << "C# Automation Macro failed: " << error.what();
			throw;
		}
	}
};

class DotNetAutomationScriptInstance final : public Script {
	struct PendingServiceProvider {
		std::string contribution_id;
		std::vector<std::string> operations;
	};

	bool loaded = false;
	std::string name;
	std::string description;
	std::string author;
	std::string version;
	std::vector<std::unique_ptr<DotNetMacro>> pending_macros;
	std::vector<std::pair<std::string, DotNetMacro*>> committed_macros;
	std::vector<cmd::Command*> macros;
	std::vector<PendingServiceProvider> pending_services;
	std::vector<std::string> committed_services;
	std::shared_ptr<DotNetExtensionReference> extension;
	std::shared_ptr<DotNetAutomationHostState> host_state =
		std::make_shared<DotNetAutomationHostState>();

	void DestroyMacros() {
		pending_macros.clear();
		pending_services.clear();
		for (auto const& [command_name, command] : committed_macros) {
			if (cmd::get_if(command_name) == command)
				cmd::unreg(command_name);
		}
		committed_macros.clear();
		macros.clear();
		if (extension) {
			for (auto const& contribution_id : committed_services)
				Runtime()->UnregisterServiceProvider(contribution_id, extension->Key());
		}
		committed_services.clear();
		extension.reset();
	}

public:
	explicit DotNetAutomationScriptInstance(agi::fs::path filename)
	:	Script(filename) {
		Reload();
	}
	~DotNetAutomationScriptInstance() override { DestroyMacros(); }

	void Reload() override {
		loaded = false;
		DestroyMacros();
		try {
			auto manifest = ParseManifest(GetFilename());
			extension = std::make_shared<DotNetExtensionReference>(Runtime(), manifest);
			name = std::move(manifest.name);
			description = std::move(manifest.description);
			author = std::move(manifest.author);
			version = std::move(manifest.version);
			for (auto& contribution : manifest.contributions) {
				if (contribution.kind == "automation") {
					for (auto& metadata : contribution.macros) {
						auto macro = std::make_unique<DotNetMacro>(
							extension,
							host_state,
							contribution.id,
							std::move(metadata));
						macros.push_back(macro.get());
						pending_macros.emplace_back(std::move(macro));
					}
				}
				else if (contribution.kind == "serviceProvider") {
					pending_services.push_back(PendingServiceProvider{
						contribution.id,
						contribution.operations});
				}
			}
			loaded = true;
		}
		catch (std::exception const& error) {
			DestroyMacros();
			description = error.what();
		}
	}

	void CommitPendingFeatures() override {
		for (auto& macro : pending_macros) {
			if (!macro) continue;
			auto command_name = std::string(macro->name());
			if (cmd::get_if(command_name)) {
				LOG_W("automation/plugin_bridge")
					<< "Skipping C# Automation Macro '" << from_wx(macro->StrDisplay(nullptr))
					<< "' because command '" << command_name << "' is already registered.";
				macros.erase(std::remove(macros.begin(), macros.end(), macro.get()), macros.end());
				continue;
			}
			auto* command = macro.get();
			committed_macros.emplace_back(command_name, command);
			cmd::reg(std::move(macro));
		}
		pending_macros.clear();

		if (!extension) {
			pending_services.clear();
			return;
		}
		for (auto& service : pending_services) {
			try {
				Runtime()->RegisterServiceProvider(
					service.contribution_id,
					extension->Key(),
					service.operations);
				try {
					committed_services.push_back(service.contribution_id);
				}
				catch (...) {
					extension->UnregisterServiceProvider(service.contribution_id);
					throw;
				}
			}
			catch (std::exception const& error) {
				LOG_W("automation/plugin_bridge")
					<< "Skipping plugin service contribution '"
					<< service.contribution_id << "': " << error.what();
			}
		}
		pending_services.clear();
	}

	std::string GetName() const override { return name; }
	std::string GetDescription() const override { return description; }
	std::string GetAuthor() const override { return author; }
	std::string GetVersion() const override { return version; }
	bool GetLoadedState() const override { return loaded; }
	std::vector<cmd::Command*> GetMacros() const override { return macros; }
	std::vector<ExportFilter*> GetFilters() const override { return {}; }
	std::string GetEngineName() const override { return "Plugin Bridge"; }
	void ValidateApplicationActivation() override {
		if (!extension)
			throw std::logic_error("CLR plugin reference is not available");
		extension->ValidateApplicationActivation();
	}
	void SetAutomationHost(std::shared_ptr<AutomationHost> host) override {
		host_state->host = std::move(host);
	}
};

std::string LowerAsciiFilename(agi::fs::path const& filename) {
	auto name = agi::fs::PathToString(filename.filename());
	std::transform(name.begin(), name.end(), name.begin(), [](char value) {
		return value >= 'A' && value <= 'Z'
			? static_cast<char>(value - 'A' + 'a')
			: value;
	});
	return name;
}

/// Names accepted by explicit discovery (app-local plugins/ and managed store).
bool IsPluginBridgeManifestName(agi::fs::path const& filename) {
	auto const name = LowerAsciiFilename(filename);
	return name.ends_with(".aegisub-plugin.json") || name == "plugin.json";
}

/// Names exposed to autoload globs / file dialogs. Bare plugin.json is only
/// accepted via LoadPluginBridgeManifest after app-local directory discovery.
bool HasCSharpManifestSuffix(agi::fs::path const& filename) {
	return LowerAsciiFilename(filename).ends_with(".aegisub-plugin.json");
}

std::unique_ptr<AutomationScriptInstance> LoadPluginBridgeManifestImpl(
	agi::fs::path const& filename) {
	if (!IsPluginBridgeManifestName(filename))
		return nullptr;
	return std::make_unique<DotNetAutomationScriptInstance>(filename);
}

} // namespace

std::unique_ptr<AutomationScriptInstance> LoadPluginBridgeManifest(
	agi::fs::path const& filename) {
	return LoadPluginBridgeManifestImpl(filename);
}

std::string InvokePluginServiceContribution(
	std::string const& contribution_id,
	std::string const& operation_id,
	std::string const& request_json) {
	// Menus/macros still come only from autoload registration. Services can be
	// bootstrapped on demand so headless and early Lua require() work without a
	// prior global autoload pass.
	EnsureServiceProviderFromAppLocalPlugins(contribution_id);
	return Runtime()->InvokeServiceContribution(
		contribution_id,
		operation_id,
		request_json);
}

void ShutdownManagedPluginRuntime() noexcept {
	ClearLazyServiceBootstrap();
	Runtime()->Shutdown();
}

std::string DotNetAutomationEngine::EngineName() const {
	return "Plugin Bridge";
}

std::string DotNetAutomationEngine::FilenamePattern() const {
	return "*.aegisub-plugin.json";
}

bool DotNetAutomationEngine::SupportsFile(agi::fs::path const& filename) const {
	return HasCSharpManifestSuffix(filename);
}

std::unique_ptr<AutomationScriptInstance> DotNetAutomationEngine::LoadScript(
	agi::fs::path const& filename) const {
	if (!SupportsFile(filename)) return nullptr;
	return std::make_unique<DotNetAutomationScriptInstance>(filename);
}

} // namespace Automation4
