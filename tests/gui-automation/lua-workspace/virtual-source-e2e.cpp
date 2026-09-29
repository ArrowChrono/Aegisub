#include "../../../src/automation/automation_debug_backend.h"
#include "../../../src/automation/automation_debug_session.h"
#include "../../../src/automation/automation_lua_debug_backend.h"
#include "../../../src/automation/automation_lua_runtime.h"
#include "../../../src/automation/lua_workspace_run.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/scope_exit.h>

#include <lua.hpp>
extern "C" {
#include <luajit.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
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

enum class AdditionalCase : std::uint8_t {
	Coroutine,
	CoroutineCancel,
	Utf8Preview
};

int RequestStop(lua_State *L) {
	auto *stop = static_cast<std::atomic<bool> *>(lua_touserdata(L, lua_upvalueindex(1)));
	stop->store(true);
	return 0;
}

void CheckStringPreview(AutomationDebugFrame const& frame, std::string const& name, std::string const& expected) {
	auto found = std::ranges::find(frame.locals, name, &AutomationDebugVariable::name);
	Require(found != frame.locals.end(), "Missing UTF-8 preview local " + name);
	Require(found->value_type == "string", "Incorrect preview type for " + name);
	Require(found->value == expected, "Incorrect or invalid UTF-8 preview for " + name + ": " + found->value);
}

json::Object RunAdditionalCase(AdditionalCase kind, fs::path const& fixtures, fs::path const& artifacts) {
	char const *name = kind == AdditionalCase::Coroutine ? "coroutine-breakpoints" : kind == AdditionalCase::CoroutineCancel ? "coroutine-cancel"
																															 : "utf8-previews";
	char const *filename = kind == AdditionalCase::Coroutine ? "virtual-source-coroutine.lua" : kind == AdditionalCase::CoroutineCancel ? "virtual-source-coroutine-cancel.lua"
																																		: "virtual-source-utf8.lua";
	int breakpoint_line = 4;
	std::uint64_t const invocation_id = kind == AdditionalCase::Coroutine ? 44 : kind == AdditionalCase::CoroutineCancel ? 45
																														 : 46;
	CreateDirectory(artifacts);
	json::Object report;
	report["case"] = name;
	report["status"] = "running";
	report["invocation_id"] = static_cast<json::Integer>(invocation_id);
	WriteJson(artifacts / "result.json", report);
	std::ofstream(artifacts / "pauses.ndjson", std::ios::trunc).close();
	std::shared_ptr<AutomationDebugSession> session;

	try {
		LuaWorkspaceSource source{
			.uri = MakeLuaWorkspaceSourceUri(invocation_id, "aegisub://lua/23/104", 0),
			.source_identity = "aegisub://lua/23/104",
			.revision = 0,
			.display_name = filename,
			.text = ReadSource(fixtures / filename)};
		if (kind == AdditionalCase::Utf8Preview) {
			auto const marker = source.text.find("local marker = 42");
			Require(marker != std::string::npos, "UTF-8 fixture is missing its breakpoint statement");
			breakpoint_line = 1 + static_cast<int>(std::count(source.text.begin(), source.text.begin() + marker, '\n'));
		}
		json::Object source_manifest;
		json::Array source_records;
		source_records.emplace_back(SourceJson(source));
		source_manifest["sources"] = std::move(source_records);
		source_manifest["scope"] = "Real Lua engine and production debug backend; not GUI acceptance";
		WriteJson(artifacts / "sources.json", source_manifest);

		AutomationDebugLaunchRequest launch;
		launch.enabled = true;
		launch.nonblocking = true;
		launch.max_pauses = 16;
		launch.breakpoints.push_back({.source_path = source.uri, .line = breakpoint_line, .enabled = true});
		session = std::make_shared<AutomationDebugSession>(std::move(launch));
		auto request = std::make_shared<LuaWorkspaceRunRequest>();
		request->invocation_id = invocation_id;
		request->macro_command = "e12/" + std::string(name);
		request->sources = std::make_shared<LuaWorkspaceSourceRegistry>();
		request->sources->Register(source);
		request->stop_requested = std::make_shared<std::atomic<bool>>(false);
		request->debug_session = session;
		session->SetSourceRegistry(request->sources);
		session->SetTarget({.engine_name = "Lua", .script_file = fixtures / filename, .feature_name = request->macro_command});

		std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
		Require(state != nullptr, "Could not create Lua state");
		auto *L = state.get();
		luaL_openlibs(L);
		Require(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_FLUSH) == 1, "Could not flush LuaJIT traces");
		Require(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF) == 1, "Could not select interpreted execution");
		auto invocation = MakeMacroRunInvocation(request->macro_command);
		LuaSetAutomationRuntimeState(L, {}, invocation, {}, 0);
		LuaSetWorkspaceRunRequest(L, request);
		AutomationLuaDebugBackend backend(L, fixtures / filename);
		backend.SetSession(session.get());
		backend.CaptureRuntimeBaseline();
		if (kind == AdditionalCase::CoroutineCancel) {
			lua_pushlightuserdata(L, request->stop_requested.get());
			lua_pushcclosure(L, RequestStop, 1);
			lua_setglobal(L, "request_stop");
		}
		CheckLua(L, luaL_loadbufferx(L, source.text.data(), source.text.size(), ("=" + source.uri).c_str(), "t"), "Compile " + std::string(filename));
		{
			ScopedAutomationDebugInvocation active(&backend, invocation);
			backend.SetWorkspaceRunRequest(request);
			int const result_count = kind == AdditionalCase::Coroutine ? 2 : 1;
			int status = lua_pcall(L, 0, result_count, 0);
			CheckLua(L, status, "Execute " + std::string(filename));
			if (kind == AdditionalCase::Coroutine) {
				Require(lua_isstring(L, -2) && lua_isstring(L, -1), "Coroutine fixture returned incorrect result types");
				std::string outputs(lua_tostring(L, -2));
				std::string completion(lua_tostring(L, -1));
				report["actual_results"] = outputs;
				Require(outputs == "13,15,17,60" && completion == "complete", "Coroutine business results are incorrect");
				lua_pop(L, 2);
			}
			else if (kind == AdditionalCase::CoroutineCancel) {
				Require(lua_isthread(L, -1), "Cancellation fixture did not return a coroutine");
				auto *thread = lua_tothread(L, -1);
				status = lua_resume(thread, 0);
				Require(status != 0 && LuaIsWorkspaceCancellation(thread, -1), "Stop did not raise the cancellation token in the coroutine");
				lua_settop(thread, 0);
				request->stop_requested->store(false);
				lua_pop(L, 1);
				char const post_cancel[] = "return 6 * 7, coroutine.status(coroutine.create(function() end))";
				CheckLua(L, luaL_loadbufferx(L, post_cancel, sizeof(post_cancel) - 1, "=post-cancel-main", "t"), "Compile post-cancel main state");
				CheckLua(L, lua_pcall(L, 0, 2, 0), "Execute post-cancel main state");
				Require(lua_tointeger(L, -2) == 42 && std::string(lua_tostring(L, -1)) == "suspended", "Main Lua state was corrupted after coroutine cancellation");
				lua_pop(L, 2);
				report["main_state_result"] = 42;
			}
			else {
				Require(lua_isnumber(L, -1) && lua_tointeger(L, -1) == 42, "UTF-8 fixture did not complete");
				lua_pop(L, 1);
			}
			backend.SetWorkspaceRunRequest({});
		}
		Require(lua_gettop(L) == 0, "Additional fixture left an unexpected value on the Lua stack");
		session->MarkCompleted(0, "Fixture completed");
		std::string trace_error;
		Require(session->WriteTraceFile(artifacts / "pauses.ndjson", trace_error), "Could not write production pause trace: " + trace_error);
		auto pauses = session->GetPauses();
		std::size_t const expected_pauses = kind == AdditionalCase::Utf8Preview ? 1 : 3;
		Require(pauses.size() == expected_pauses && session->BreakpointPauseCount() == expected_pauses, "Coroutine or UTF-8 breakpoint count is incorrect");
		Require(session->DroppedPauseCount() == 0 && session->EntryPauseCount() == 0 && session->StepPauseCount() == 0, "Unexpected pause classification or loss");
		for (std::size_t index = 0; index < pauses.size(); ++index) {
			auto const& pause = pauses[index];
			Require(pause.sequence == index + 1 && pause.reason == AutomationDebugPauseReason::Breakpoint, "Incorrect pause order or reason");
			Require(pause.location.source_path == source.uri && pause.location.line == breakpoint_line, "Breakpoint did not resolve to coroutine or UTF-8 fixture source");
			Require(!pause.frames.empty(), "Breakpoint did not capture the executing frame");
			CheckFrame(pause.frames[0], 0, source, breakpoint_line,
					   kind == AdditionalCase::Coroutine ? "transform" : kind == AdditionalCase::Utf8Preview ? "main"
																											 : "(anonymous)",
					   *request->sources);
			Require(pause.runtime_snapshot.has_value() && pause.runtime_snapshot->invocation.feature_name == request->macro_command, "Breakpoint lost the runtime invocation context");
			if (kind == AdditionalCase::Coroutine) {
				Require(pauses[index].frames.size() == 2, "Coroutine stack did not contain its own caller frame");
				Require(pauses[index].frames[1].location.source_path == source.uri, "Coroutine caller frame points to the wrong source");
				CheckNumber(pause.frames[0].locals, "value", static_cast<int>(index) + 1, "Coroutine transform locals");
				CheckNumber(pause.frames[0].upvalues, "bias", 11, "Coroutine transform closure");
			}
			else if (kind == AdditionalCase::CoroutineCancel)
				CheckNumber(pause.frames[0].locals, "value", static_cast<int>(index) + 1, "Cancelled coroutine locals");
			else {
				CheckStringPreview(pause.frames[0], "ascii_cjk", "\"" + std::string(114, 'A') + "\xE4\xB8\xAD...\" (117/121 bytes)");
				CheckStringPreview(pause.frames[0], "emoji", "\"" + std::string(113, 'B') + "\xF0\x9F\x99\x82...\" (117/121 bytes)");
				CheckStringPreview(pause.frames[0], "invalid", "\"" + std::string(109, 'C') + R"(\xFF\x80..." (111/121 bytes))");
				CheckStringPreview(pause.frames[0], "ascii118", "\"" + std::string(118, 'A') + "\"");
				CheckStringPreview(pause.frames[0], "ascii119", "\"" + std::string(119, 'A') + "\"");
				CheckStringPreview(pause.frames[0], "ascii120", "\"" + std::string(120, 'A') + "\"");
				CheckStringPreview(pause.frames[0], "ascii121", "\"" + std::string(117, 'A') + "...\" (117/121 bytes)");
				CheckStringPreview(pause.frames[0], "utf8_exact", "\"" + std::string(117, 'U') + "中\"");
				CheckStringPreview(pause.frames[0], "utf8_over", "\"" + std::string(117, 'U') + "...\" (117/121 bytes)");
				CheckStringPreview(pause.frames[0], "escape_exact", "\"" + std::string(116, 'V') + R"(\xFF")");
				CheckStringPreview(pause.frames[0], "escape_over", "\"" + std::string(117, 'V') + "...\" (117/118 bytes)");
			}
		}
		report["checked_pauses"] = static_cast<json::Integer>(pauses.size());
		report["status"] = "passed";
	}
	catch (std::exception const& error) {
		report["status"] = "failed";
		report["error"] = error.what();
		if (session) {
			std::string trace_error;
			if (!session->WriteTraceFile(artifacts / "pauses.ndjson", trace_error))
				report["trace_error"] = trace_error;
		}
	}
	WriteJson(artifacts / "result.json", report);
	return report;
}

enum class ControlledCase : std::uint8_t { SameLine,
										   SameLineNext,
										   OverResume,
										   YieldOut,
										   Display };

int RequestPause(lua_State *L) {
	auto *session = static_cast<AutomationDebugSession *>(lua_touserdata(L, lua_upvalueindex(1)));
	session->RequestPause();
	return 0;
}

AutomationDebugVariable const& FindVariable(std::vector<AutomationDebugVariable> const& variables, std::string_view name) {
	auto found = std::ranges::find(variables, name, &AutomationDebugVariable::name);
	Require(found != variables.end(), "Missing display variable " + std::string(name));
	return *found;
}

std::string CheckObjectKey(std::vector<AutomationDebugVariable> const& children, std::string_view type, int expected) {
	auto const value = std::to_string(expected);
	auto found = std::ranges::find_if(children, [&](auto const& child) { return child.value_type == "integer" && child.value == value; });
	Require(found != children.end(), "Missing object-key value " + value);
	Require(std::ranges::count_if(children, [&](auto const& child) { return child.value_type == "integer" && child.value == value; }) == 1,
			"Object-key value is ambiguous: " + value);
	auto const& name = found->name;
	auto const prefix = "[" + std::string(type) + ":0x";
	Require(name.starts_with(prefix) && name.size() > prefix.size() + 1 && name.back() == ']', "Incorrect object-key label " + name);
	Require(std::ranges::all_of(std::string_view(name).substr(prefix.size(), name.size() - prefix.size() - 1),
								[](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }),
			"Object-key label has a nonhexadecimal identity " + name);
	return name;
}

void CheckSummary(std::vector<AutomationDebugVariable> const& locals, std::string_view name, std::string const& expected) {
	auto const& variable = FindVariable(locals, name);
	Require(variable.value_type == "info" && variable.value == expected, "Incorrect 48-byte structured summary for " + std::string(name) + ": " + variable.value);
}

json::Object RunControlledCase(ControlledCase kind, fs::path const& fixtures, fs::path const& artifacts) {
	bool const same_line = kind == ControlledCase::SameLine || kind == ControlledCase::SameLineNext;
	bool const display = kind == ControlledCase::Display;
	auto filename = kind == ControlledCase::SameLineNext ? "virtual-source-same-line-step.lua" : same_line ? "virtual-source-same-line.lua"
																							 : display     ? "virtual-source-display.lua"
																										   : "virtual-source-stepping.lua";
	CreateDirectory(artifacts);
	json::Object report;
	report["status"] = "running";
	WriteJson(artifacts / "result.json", report);
	std::shared_ptr<AutomationDebugSession> session;
	try {
		LuaWorkspaceSource source{
			.uri = MakeLuaWorkspaceSourceUri(47 + static_cast<unsigned>(kind), "aegisub://lua/23/105", 0),
			.source_identity = "aegisub://lua/23/105",
			.revision = 0,
			.display_name = filename,
			.text = ReadSource(fixtures / filename)};
		auto line_of = [&](std::string_view statement) {
			auto offset = source.text.find(statement);
			Require(offset != std::string::npos, "Fixture is missing statement " + std::string(statement));
			return 1 + static_cast<int>(std::count(source.text.begin(), source.text.begin() + offset, '\n'));
		};
		json::Object source_manifest;
		json::Array source_records;
		source_records.emplace_back(SourceJson(source));
		source_manifest["sources"] = std::move(source_records);
		source_manifest["scope"] = "Real Lua engine with blocking production debug session; not GUI acceptance";
		WriteJson(artifacts / "sources.json", source_manifest);
		AutomationDebugLaunchRequest launch;
		launch.enabled = true;
		launch.max_pauses = 32;
		int const initial_line = line_of(same_line ? "while index" : display                        ? "local total"
																 : kind == ControlledCase::YieldOut ? "local value = nested(seed)"
																									: "local ok, first");
		launch.breakpoints.push_back({.source_path = source.uri, .line = initial_line, .enabled = true});
		session = std::make_shared<AutomationDebugSession>(std::move(launch));
		auto request = std::make_shared<LuaWorkspaceRunRequest>();
		request->invocation_id = 47 + static_cast<unsigned>(kind);
		request->macro_command = "e12/controlled-debug";
		request->sources = std::make_shared<LuaWorkspaceSourceRegistry>();
		request->sources->Register(source);
		request->stop_requested = std::make_shared<std::atomic<bool>>(false);
		request->debug_session = session;
		session->SetSourceRegistry(request->sources);
		session->SetTarget({.engine_name = "Lua", .script_file = fixtures / filename, .feature_name = request->macro_command});
		std::unique_ptr<lua_State, decltype(&lua_close)> state(luaL_newstate(), lua_close);
		Require(state != nullptr, "Could not create Lua state");
		auto *L = state.get();
		luaL_openlibs(L);
		Require(luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF) == 1, "Could not disable LuaJIT");
		auto invocation = MakeMacroRunInvocation(request->macro_command);
		LuaSetAutomationRuntimeState(L, {}, invocation, {}, 0);
		LuaSetWorkspaceRunRequest(L, request);
		AutomationLuaDebugBackend backend(L, fixtures / filename);
		backend.SetSession(session.get());
		backend.CaptureRuntimeBaseline();
		lua_pushlightuserdata(L, session.get());
		lua_pushcclosure(L, RequestPause, 1);
		lua_setglobal(L, "request_pause");
		CheckLua(L, luaL_loadbufferx(L, source.text.data(), source.text.size(), ("=" + source.uri).c_str(), "t"), "Compile controlled fixture");
		int status = 0;
		{
			ScopedAutomationDebugInvocation active(&backend, invocation);
			backend.SetWorkspaceRunRequest(request);
			std::atomic<bool> finished = false;
			std::thread worker([&] {
				status = lua_pcall(L, 0, 1, 0);
				finished.store(true);
			});
			auto cleanup = agi::make_scope_exit([&] {
				if (worker.joinable()) {
					request->stop_requested->store(true);
					session->Detach();
					worker.join();
				}
			});
			auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			size_t expected_sequence = 0;
			auto pause_at = [&](int line, AutomationDebugPauseReason reason) {
				++expected_sequence;
				for (;;) {
					auto snapshot = session->GetStateSnapshot();
					if (snapshot.state == AutomationDebugSessionState::Paused && snapshot.current_pause && snapshot.current_pause->sequence == expected_sequence) {
						auto pause = *snapshot.current_pause;
						Require(pause.location.source_path == source.uri && pause.location.line == line,
								"Unexpected execution location at pause " + std::to_string(expected_sequence) + ": " + std::to_string(pause.location.line));
						Require(pause.reason == reason, "Unexpected pause reason at pause " + std::to_string(expected_sequence));
						Require(pause.thread_id != 0 && pause.stack_depth == pause.frames.size(), "Pause did not retain executing thread/depth");
						return pause;
					}
					Require(!finished.load(), "Fixture completed before expected pause " + std::to_string(expected_sequence));
					Require(std::chrono::steady_clock::now() < deadline, "Controlled debug case exceeded its total timeout");
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
			};
			auto resume = [&](AutomationDebugResumeAction action) { Require(session->Resume(action), "Could not resume paused session"); };
			auto initial = pause_at(initial_line, AutomationDebugPauseReason::Breakpoint);
			if (kind == ControlledCase::SameLineNext) {
				session->SetBreakpoints({});
				for (int expected = 1; expected <= 2; ++expected) {
					resume(AutomationDebugResumeAction::Next);
					auto next = pause_at(initial_line, AutomationDebugPauseReason::Step);
					Require(next.thread_id == initial.thread_id, "Same-line step changed the executing thread");
					CheckNumber(next.frames[0].locals, "index", expected, "Same-line Step Over nested call");
				}
				resume(AutomationDebugResumeAction::Continue);
			}
			else if (same_line) {
				resume(AutomationDebugResumeAction::Continue);
				auto repeated = pause_at(initial_line, AutomationDebugPauseReason::Breakpoint);
				Require(repeated.thread_id == initial.thread_id, "Same-line repeat changed the executing thread");
				session->SetBreakpoints({});
				resume(AutomationDebugResumeAction::Continue);
				auto manual = pause_at(initial_line, AutomationDebugPauseReason::Pause);
				CheckNumber(manual.frames[0].locals, "index", 4, "Manual same-line pause");
				resume(AutomationDebugResumeAction::Continue);
			}
			else if (display) {
				auto const& locals = initial.frames[0].locals;
				auto const& values = FindVariable(locals, "values");
				CheckNumber(values.children, R"(["k\x00a"])", 1, "NUL table key a");
				CheckNumber(values.children, R"(["k\x00b"])", 2, "NUL table key b");
				CheckNumber(values.children, R"(["\xFF"])", 3, "Invalid UTF-8 table key");
				CheckNumber(values.children, R"(["k\\x00a"])", 4, "Literal escape table key");
				Require(FindVariable(locals, "line").value == "dialogue-line <样式\\xFF\\x00尾> \"OK\"", "Dialogue summary lost safe display encoding");
				Require(FindVariable(locals, "style").value == "style-line <名\\x80> 字\\xFF", "Style summary lost safe display encoding");
				Require(FindVariable(locals, "info").value == "info-line k\\x00ey=值\\xFF", "Info summary lost safe display encoding");
				auto const& long_keys = FindVariable(locals, "long_keys");
				CheckNumber(long_keys.children, std::string(160, 'k') + "a", 5, "Long table key a");
				CheckNumber(long_keys.children, std::string(160, 'k') + "b", 6, "Long table key b");
				auto const& custom = FindVariable(locals, "custom");
				Require(custom.value_type == R"(tag\xFF\x00end)" && custom.value == R"(tag\xFF\x00end-table)", "Custom type label lost safe display encoding");
				auto const& numeric_keys = FindVariable(locals, "numeric_keys");
				for (auto const& [name, number] : std::array<std::pair<std::string_view, int>, 11>{{{"[1.25]", 11}, {"[1.75]", 12}, {"[-0.25]", 13}, {"[-0.75]", 14}, {"[1]", 15}, {"[1.0000000000000002]", 16}, {"[1e+20]", 17}, {"[\"1.25\"]", 18}, {"[\"[1.25]\"]", 19}, {"[true]", 20}, {"[false]", 21}}})
					CheckNumber(numeric_keys.children, std::string(name), number, "Mixed Lua table key");
				Require(numeric_keys.children.size() == 11, "Mixed-key table lost or duplicated a child");
				auto const& object_keys = FindVariable(locals, "object_keys");
				auto const& shared_keys = FindVariable(locals, "shared_keys");
				Require(object_keys.children.size() == 8 && shared_keys.children.size() == 4, "Object-key table lost or duplicated children");
				std::vector<std::string> object_names;
				for (auto const& [type, first, second, shared] : std::array<std::tuple<std::string_view, int, int, int>, 4>{{{"table", 31, 32, 41},
																															 {"function", 33, 34, 43},
																															 {"thread", 35, 36, 45},
																															 {"userdata", 37, 38, 47}}}) {
					auto first_name = CheckObjectKey(object_keys.children, type, first);
					auto second_name = CheckObjectKey(object_keys.children, type, second);
					Require(first_name != second_name, "Distinct " + std::string(type) + " keys have the same display identity");
					Require(CheckObjectKey(shared_keys.children, type, shared) == first_name, "Shared " + std::string(type) + " key changed identity between tables");
					object_names.push_back(std::move(first_name));
					object_names.push_back(std::move(second_name));
				}
				std::ranges::sort(object_names);
				Require(std::ranges::adjacent_find(object_names) == object_names.end(), "Object-key labels are not unique");
				auto globals = std::ranges::find(initial.scopes, "Globals", &AutomationDebugScope::name);
				Require(globals != initial.scopes.end(), "Missing script Globals scope");
				CheckNumber(globals->variables, "[1.25]", 51, "Numeric global key");
				CheckNumber(globals->variables, "[\"[1.25]\"]", 52, "String global key");
				CheckNumber(globals->variables, CheckObjectKey(object_keys.children, "table", 31), 53, "Object global key");
				CheckNumber(locals, "tostring_calls", 0, "Object-key metamethod side effect");
				CheckSummary(locals, "summary48", "info-line limit=" + std::string(48, 'Q'));
				CheckSummary(locals, "summary49", "info-line limit=" + std::string(45, 'Q') + "... (45/49 bytes)");
				CheckSummary(locals, "summary_utf8_exact", "info-line limit=" + std::string(45, 'Q') + "中");
				CheckSummary(locals, "summary_utf8_over", "info-line limit=" + std::string(45, 'Q') + "... (45/49 bytes)");
				CheckSummary(locals, "summary_escape_exact", "info-line limit=" + std::string(44, 'Q') + R"(\xFF)");
				CheckSummary(locals, "summary_escape_over", "info-line limit=" + std::string(45, 'Q') + "... (45/46 bytes)");
				report["key_assertions"] = "numeric, string, boolean, object-identity, globals, no-__tostring";
				report["summary_preview_assertions"] = "48-display-byte full and overflow, UTF-8 and escaped-token boundaries";
				session->SetBreakpoints({});
				resume(AutomationDebugResumeAction::Continue);
			}
			else {
				session->SetBreakpoints({});
				resume(kind == ControlledCase::YieldOut ? AutomationDebugResumeAction::StepOut : AutomationDebugResumeAction::Next);
				auto first_result = pause_at(line_of("assert(ok and first"), AutomationDebugPauseReason::Step);
				if (kind == ControlledCase::OverResume)
					Require(first_result.thread_id == initial.thread_id, "Step Over resume entered the coroutine");
				else
					Require(first_result.thread_id != initial.thread_id, "Step Out did not leave the yielded coroutine root");
				CheckNumber(first_result.frames[0].locals, "first", 10, "First coroutine result");
				resume(AutomationDebugResumeAction::Next);
				auto second_resume = pause_at(line_of("local ok_again"), AutomationDebugPauseReason::Step);
				Require(second_resume.thread_id == first_result.thread_id, "Step changed threads before resume");
				resume(AutomationDebugResumeAction::StepIn);
				auto resumed = pause_at(line_of("value = nested(value + 1)"), AutomationDebugPauseReason::Step);
				Require(resumed.thread_id != first_result.thread_id, "Step In failed to enter the resumed coroutine");
				if (kind == ControlledCase::YieldOut)
					Require(resumed.thread_id == initial.thread_id, "Coroutine identity changed across yield/resume");
				resume(AutomationDebugResumeAction::Next);
				auto returning = pause_at(line_of("return value"), AutomationDebugPauseReason::Step);
				Require(returning.thread_id == resumed.thread_id, "Step Over entered a nested function or changed coroutine");
				CheckNumber(returning.frames[0].locals, "value", 122, "Coroutine return value");
				resume(AutomationDebugResumeAction::StepOut);
				auto second_result = pause_at(line_of("assert(ok_again"), AutomationDebugPauseReason::Step);
				Require(second_result.thread_id == first_result.thread_id, "Step Out from a completed coroutine did not return to the resumer");
				CheckNumber(second_result.frames[0].locals, "second", 122, "Second coroutine result");
				resume(AutomationDebugResumeAction::Continue);
			}
			while (!finished.load()) {
				Require(std::chrono::steady_clock::now() < deadline, "Controlled debug case did not finish within its total timeout");
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			worker.join();
			backend.SetWorkspaceRunRequest({});
			report["checked_pauses"] = static_cast<json::Integer>(expected_sequence);
			Require(session->PauseCount() == expected_sequence && session->DroppedPauseCount() == 0, "Unexpected extra or lost pauses");
		}
		CheckLua(L, status, "Execute controlled fixture");
		int const expected_result = kind == ControlledCase::SameLineNext ? 8 : same_line ? 12
																		   : display     ? 10
																						 : 132;
		Require(lua_isnumber(L, -1), "Controlled fixture returned a nonnumeric business result");
		auto const actual_result = lua_tointeger(L, -1);
		report["actual_result"] = static_cast<json::Integer>(actual_result);
		report["expected_result"] = expected_result;
		Require(actual_result == expected_result, "Controlled fixture business result changed");
		lua_pop(L, 1);
		Require(lua_gettop(L) == 0, "Controlled fixture corrupted the Lua stack");
		session->MarkCompleted(0, "Fixture completed");
		report["status"] = "passed";
	}
	catch (std::exception const& error) {
		report["status"] = "failed";
		report["error"] = error.what();
	}
	if (session) {
		std::string error;
		if (!session->WriteTraceFile(artifacts / "pauses.ndjson", error)) {
			report["status"] = "failed";
			report["trace_error"] = error;
		}
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
		for (auto kind : {AdditionalCase::Coroutine, AdditionalCase::CoroutineCancel, AdditionalCase::Utf8Preview}) {
			auto name = kind == AdditionalCase::Coroutine ? "coroutine-breakpoints" : kind == AdditionalCase::CoroutineCancel ? "coroutine-cancel" : "utf8-previews";
			auto result = RunAdditionalCase(kind, fixtures, artifacts / name);
			passed = static_cast<json::String const&>(result["status"]) == "passed" && passed;
			results.emplace_back(std::move(result));
			WriteJson(artifacts / "manifest.json", manifest);
		}
		for (auto kind : {ControlledCase::SameLine, ControlledCase::SameLineNext, ControlledCase::OverResume, ControlledCase::YieldOut, ControlledCase::Display}) {
			auto name = kind == ControlledCase::SameLineNext ? "same-line-step-over" : kind == ControlledCase::SameLine ? "same-line-pauses"
																				   : kind == ControlledCase::OverResume ? "step-over-resume"
																				   : kind == ControlledCase::YieldOut   ? "step-out-yield-return"
																														: "display-boundaries";
			auto result = RunControlledCase(kind, fixtures, artifacts / name);
			result["case"] = name;
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
