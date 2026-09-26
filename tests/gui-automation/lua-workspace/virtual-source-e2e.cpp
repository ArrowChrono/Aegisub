#include "../../../src/automation/automation_debug_backend.h"
#include "../../../src/automation/automation_debug_session.h"
#include "../../../src/automation/automation_lua_debug_backend.h"
#include "../../../src/automation/automation_lua_runtime.h"
#include "../../../src/automation/lua_workspace_run.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/writer.h>

#include <lua.hpp>
extern "C" {
#include <luajit.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace Automation4;

struct Case {
	std::string name;
	bool breakpoint_a;
	bool breakpoint_b;
	std::uint64_t invocation_id;
};

void Require(bool condition, std::string const& message) {
	if (!condition)
		throw std::runtime_error(message);
}

void CreateDirectory(fs::path const& path) {
	std::error_code error;
	fs::create_directories(path, error);
	Require(!error, "Could not create artifact directory: " + error.message());
}

void WriteJson(fs::path const& path, json::Object const& value) {
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	Require(output.is_open(), "Could not open JSON artifact " + path.filename().string());
	agi::JsonWriter::Write(value, output);
	output << '\n';
	output.close();
	Require(!output.fail(), "Could not write JSON artifact " + path.filename().string());
}

std::string ReadSource(fs::path const& path) {
	std::ifstream input(path, std::ios::binary);
	Require(input.is_open(), "Could not open fixture " + path.filename().string());
	std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	Require(!input.bad(), "Could not read fixture " + path.filename().string());
	return source;
}

void CheckLua(lua_State *state, int status, std::string const& operation) {
	if (status == 0)
		return;
	auto *error = lua_tostring(state, -1);
	auto message = operation + ": " + (error ? error : "non-string Lua error");
	lua_pop(state, 1);
	throw std::runtime_error(message);
}

json::Object SourceJson(LuaWorkspaceSource const& source) {
	json::Object result;
	result["uri"] = source.uri;
	result["source_identity"] = source.source_identity;
	result["revision"] = static_cast<json::Integer>(source.revision);
	result["display_name"] = source.display_name;
	result["text"] = source.text;
	return result;
}

void CheckNumber(std::vector<AutomationDebugVariable> const& values, std::string const& name, int expected, std::string const& context) {
	auto found = std::ranges::find(values, name, &AutomationDebugVariable::name);
	Require(found != values.end(), context + " missing variable " + name);
	Require(found->value_type == "integer", context + " variable " + name + " has type " + found->value_type);
	Require(found->value == std::to_string(expected), context + " variable " + name + " expected " + std::to_string(expected) + ", got " + found->value);
}

void CheckFrame(AutomationDebugFrame const& frame, int level, LuaWorkspaceSource const& source, int line, std::string const& function, LuaWorkspaceSourceRegistry const& registry) {
	auto context = "frame " + std::to_string(level) + " (" + function + ")";
	Require(frame.level == level, context + " has incorrect level");
	Require(frame.function_name == function, context + " has function name " + frame.function_name);
	Require(frame.kind == "template-frame", context + " has incorrect frame kind");
	Require(frame.location.source_kind == "template", context + " has incorrect source kind");
	Require(frame.location.source_path == source.uri, context + " points to " + frame.location.source_path + " instead of " + source.uri);
	Require(frame.location.display_name == source.display_name, context + " has incorrect source display name");
	Require(frame.location.line == line && frame.location.column == 0, context + " has incorrect source position");
	auto recorded = registry.Find(frame.location.source_path);
	Require(recorded != nullptr, context + " source is absent from the registry");
	Require(recorded->source_identity == source.source_identity && recorded->revision == source.revision, context + " has incorrect source identity or revision");
	Require(recorded->text == source.text, context + " source bytes differ from the compiled fixture");
}

void CheckPause(AutomationDebugPauseRecord const& pause, bool in_a, int iteration, std::array<LuaWorkspaceSource, 3> const& sources, LuaWorkspaceSourceRegistry const& registry) {
	auto const& expected = sources[in_a ? 0 : 1];
	Require(pause.reason == AutomationDebugPauseReason::Breakpoint, "Unexpected non-breakpoint pause");
	Require(pause.location.source_path == expected.uri && pause.location.line == 5, "Breakpoint matched the wrong virtual source or line");
	Require(pause.frames.size() == (in_a ? 4u : 2u), "Unexpected complete Lua stack depth");
	if (in_a) {
		CheckFrame(pause.frames[0], 0, sources[0], 5, "worker", registry);
		CheckFrame(pause.frames[1], 1, sources[0], 12, "source_a", registry);
		CheckFrame(pause.frames[2], 2, sources[1], 5, "source_b", registry);
		CheckFrame(pause.frames[3], 3, sources[2], 4, "main", registry);
		CheckNumber(pause.frames[0].locals, "value", iteration * 3 + 1, "worker locals");
		CheckNumber(pause.frames[0].upvalues, "bias", 12, "worker closure");
		CheckNumber(pause.frames[1].locals, "value", iteration * 3 + 1, "source_a locals");
		CheckNumber(pause.frames[2].locals, "value", iteration, "source_b caller locals");
	}
	else {
		CheckFrame(pause.frames[0], 0, sources[1], 5, "source_b", registry);
		CheckFrame(pause.frames[1], 1, sources[2], 4, "main", registry);
		CheckNumber(pause.frames[0].locals, "value", iteration, "source_b locals");
		CheckNumber(pause.frames[0].locals, "scaled", iteration * 3, "source_b locals");
		CheckNumber(pause.frames[0].locals, "before_call", iteration * 3 + 1, "source_b locals");
		CheckNumber(pause.frames[0].upvalues, "multiplier", 3, "source_b closure");
	}
	Require(pause.runtime_snapshot.has_value(), "Pause lacks its real runtime snapshot");
	Require(pause.runtime_snapshot->invocation.feature_name == "e12/multi-chunk", "Pause belongs to another invocation");
}

json::Object RunCase(Case const& test, fs::path const& fixtures, fs::path const& artifacts) {
	CreateDirectory(artifacts);
	json::Object report;
	report["case"] = test.name;
	report["status"] = "running";
	report["invocation_id"] = static_cast<json::Integer>(test.invocation_id);
	WriteJson(artifacts / "result.json", report);
	std::ofstream(artifacts / "pauses.ndjson", std::ios::trunc).close();

	try {
		std::array<LuaWorkspaceSource, 3> sources;
		std::array<std::string, 3> const filenames{"virtual-source-a.lua", "virtual-source-b.lua", "virtual-source-driver.lua"};
		json::Array source_records;
		for (std::size_t i = 0; i < sources.size(); ++i) {
			auto identity = "aegisub://lua/23/" + std::to_string(101 + i);
			auto revision = i == 2 ? 9u : 0u;
			sources[i] = {
				.uri = MakeLuaWorkspaceSourceUri(test.invocation_id, identity, revision),
				.source_identity = std::move(identity),
				.revision = revision,
				.display_name = i == 2 ? "driver.lua" : "code.lua",
				.text = ReadSource(fixtures / filenames[i])};
			source_records.emplace_back(SourceJson(sources[i]));
		}
		Require(sources[0].uri != sources[1].uri, "Fixture virtual sources must have different full identities");
		Require(sources[0].uri.substr(sources[0].uri.find_last_of('/') + 1) == "0" && sources[1].uri.substr(sources[1].uri.find_last_of('/') + 1) == "0", "Fixture virtual sources do not share a basename");
		json::Object source_manifest;
		source_manifest["sources"] = std::move(source_records);
		source_manifest["scope"] = "Real Lua engine and production debug backend; not GUI or templater-adapter acceptance";
		WriteJson(artifacts / "sources.json", source_manifest);

		AutomationDebugLaunchRequest launch;
		launch.enabled = true;
		launch.nonblocking = true;
		launch.max_pauses = 64;
		if (test.breakpoint_a)
			launch.breakpoints.push_back({.source_path = sources[0].uri, .line = 5, .enabled = true});
		if (test.breakpoint_b)
			launch.breakpoints.push_back({.source_path = sources[1].uri, .line = 5, .enabled = true});
		json::Array breakpoint_records;
		for (auto const& breakpoint : launch.breakpoints) {
			json::Object value;
			value["source_uri"] = breakpoint.source_path;
			value["line"] = static_cast<json::Integer>(breakpoint.line);
			breakpoint_records.emplace_back(std::move(value));
		}
		report["breakpoints"] = std::move(breakpoint_records);
		WriteJson(artifacts / "result.json", report);

		auto session = std::make_shared<AutomationDebugSession>(std::move(launch));
		auto request = std::make_shared<LuaWorkspaceRunRequest>();
		request->invocation_id = test.invocation_id;
		request->macro_command = "e12/multi-chunk";
		request->sources = std::make_shared<LuaWorkspaceSourceRegistry>();
		request->stop_requested = std::make_shared<std::atomic<bool>>(false);
		request->debug_session = session;
		request->source_override = LuaWorkspaceSourceOverride{.document_generation = 23, .dialogue_id = 103, .source = sources[2]};
		for (auto const& source : sources)
			request->sources->Register(source);
		session->SetSourceRegistry(request->sources);
		session->SetTarget({.engine_name = "Lua", .script_file = fixtures / filenames[2], .feature_name = request->macro_command});

		std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
		Require(state != nullptr, "Could not create Lua state");
		auto *L = state.get();
		luaL_openlibs(L);
		lua_getglobal(L, "package");
		auto module_path = (fixtures / "?.lua").generic_string();
		lua_pushlstring(L, module_path.data(), module_path.size());
		lua_setfield(L, -2, "path");
		lua_pushliteral(L, "");
		lua_setfield(L, -2, "cpath");
		lua_pop(L, 1);
		Require(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_FLUSH) == 1, "Could not flush LuaJIT traces");
		Require(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF) == 1, "Could not select interpreted execution");
		auto invocation = MakeMacroRunInvocation(request->macro_command);
		LuaSetAutomationRuntimeState(L, {}, invocation, {}, 0);
		LuaSetWorkspaceRunRequest(L, request);
		AutomationLuaDebugBackend backend(L, fixtures / filenames[2]);
		backend.SetSession(session.get());
		backend.CaptureRuntimeBaseline();
		for (std::size_t i = 0; i < sources.size(); ++i) {
			auto const& source = sources[i];
			CheckLua(L, luaL_loadbufferx(L, source.text.data(), source.text.size(), ("=" + source.uri).c_str(), "t"), "Compile " + filenames[i]);
			if (i != 2)
				CheckLua(L, lua_pcall(L, 0, 0, 0), "Initialize " + filenames[i]);
		}
		int status;
		{
			ScopedAutomationDebugInvocation active(&backend, invocation);
			backend.SetWorkspaceRunRequest(request);
			status = lua_pcall(L, 0, 2, 0);
			backend.SetWorkspaceRunRequest({});
		}
		session->MarkCompleted(status, status == 0 ? "Fixture completed" : "Fixture failed");
		std::string trace_error;
		Require(session->WriteTraceFile(artifacts / "pauses.ndjson", trace_error), "Could not write production pause trace");
		CheckLua(L, status, "Execute multi-chunk fixture");
		Require(lua_isnumber(L, -2) && lua_type(L, -1) == LUA_TSTRING, "Fixture returned incorrect Lua result types");
		auto total = lua_tonumber(L, -2);
		std::string results(lua_tostring(L, -1));
		report["actual_total"] = total;
		report["actual_results"] = results;
		WriteJson(artifacts / "result.json", report);
		Require(total == 190, "Business result sum expected 190, got " + std::to_string(total));
		Require(results == "37,44,51,58", "Business result sequence is incorrect: " + results);
		lua_pop(L, 2);
		Require(lua_gettop(L) == 0, "Harness left an unexpected value on the Lua stack");

		auto pauses = session->GetPauses();
		std::size_t expected_count = 4u * (static_cast<unsigned>(test.breakpoint_a) + static_cast<unsigned>(test.breakpoint_b));
		Require(pauses.size() == expected_count && session->PauseCount() == expected_count, "Unexpected pause count; a virtual source may have matched by basename");
		Require(session->BreakpointPauseCount() == expected_count && session->EntryPauseCount() == 0 && session->StepPauseCount() == 0 && session->ManualPauseCount() == 0 && session->DroppedPauseCount() == 0, "Pause classification or retention is incorrect");
		std::size_t index = 0;
		for (int iteration = 1; iteration <= 4; ++iteration) {
			for (bool in_a : {false, true}) {
				if (in_a ? !test.breakpoint_a : !test.breakpoint_b)
					continue;
				auto const& pause = pauses[index];
				Require(pause.sequence == index + 1, "Pause sequence contains a gap or reordering");
				CheckPause(pause, in_a, iteration, sources, *request->sources);
				++index;
			}
		}
		report["checked_pauses"] = static_cast<json::Integer>(index);
		report["status"] = "passed";
	}
	catch (std::exception const& error) {
		report["status"] = "failed";
		report["error"] = error.what();
	}
	WriteJson(artifacts / "result.json", report);
	return report;
}
}

int main(int argc, char **argv) {
	if (argc != 3) {
		std::cerr << "Usage: lua-workspace-virtual-source-e2e <repo-relative-fixtures> <artifact-directory>\n";
		return 2;
	}
	try {
		fs::path fixtures(argv[1]);
		Require(!fixtures.empty() && !fixtures.is_absolute() && !fixtures.has_root_name(), "Fixtures must use a repository-relative path");
		for (auto const& component : fixtures)
			Require(component != "..", "Fixture parent traversal is not allowed");
		fs::path artifacts(argv[2]);
		Require(!artifacts.empty(), "An artifact directory is required");
		CreateDirectory(artifacts);
		json::Object manifest;
		manifest["status"] = "running";
		manifest["scope"] = "Engine-level E12 integration; does not replace GUI acceptance";
		manifest["lua_runtime"] = LUAJIT_VERSION;
		manifest["started_unix_ms"] = static_cast<json::Integer>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
		WriteJson(artifacts / "manifest.json", manifest);
		std::array<Case, 3> const cases{{{.name = "a-only", .breakpoint_a = true, .breakpoint_b = false, .invocation_id = 41},
										 {.name = "b-only", .breakpoint_a = false, .breakpoint_b = true, .invocation_id = 42},
										 {.name = "a-and-b", .breakpoint_a = true, .breakpoint_b = true, .invocation_id = 43}}};
		manifest["cases"] = json::Array{};
		auto& results = static_cast<json::Array&>(manifest["cases"]);
		bool passed = true;
		for (auto const& test : cases) {
			auto result = RunCase(test, fixtures, artifacts / test.name);
			passed = static_cast<json::String const&>(result["status"]) == "passed" && passed;
			results.emplace_back(std::move(result));
			WriteJson(artifacts / "manifest.json", manifest);
		}
		manifest["status"] = passed ? "passed" : "failed";
		WriteJson(artifacts / "manifest.json", manifest);
		agi::JsonWriter::Write(manifest, std::cout);
		std::cout << '\n';
		return passed ? 0 : 1;
	}
	catch (std::exception const& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
