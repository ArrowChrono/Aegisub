// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS; WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "automation_debug_adapter.h"

#include "automation_breakpoint_store.h"
#include "automation_debug_service.h"
#include "automation_debug_session.h"
#include "lua_workspace_run.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/fs.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace Automation4 {
namespace {

constexpr int kAutomationThreadId = 1;

std::string SerializeJson(json::Object const& value)
{
	std::ostringstream out;
	agi::JsonWriter::Write(value, out);
	return out.str();
}

std::optional<json::Object const*> FindObject(json::Object const& object, std::string const& key)
{
	auto it = object.find(key);
	if (it == object.end())
		return std::nullopt;
	try {
		return &static_cast<json::Object const&>(it->second);
	}
	catch (...) {
		return std::nullopt;
	}
}

std::optional<json::Array const*> FindArray(json::Object const& object, std::string const& key)
{
	auto it = object.find(key);
	if (it == object.end())
		return std::nullopt;
	try {
		return &static_cast<json::Array const&>(it->second);
	}
	catch (...) {
		return std::nullopt;
	}
}

std::optional<std::string> FindString(json::Object const& object, std::string const& key)
{
	auto it = object.find(key);
	if (it == object.end())
		return std::nullopt;
	try {
		return static_cast<json::String const&>(it->second);
	}
	catch (...) {
		return std::nullopt;
	}
}

std::optional<int> FindInt(json::Object const& object, std::string const& key)
{
	auto it = object.find(key);
	if (it == object.end())
		return std::nullopt;
	try {
		return static_cast<int>(static_cast<json::Integer const&>(it->second));
	}
	catch (...) {
		try {
			return static_cast<int>(static_cast<json::Double const&>(it->second));
		}
		catch (...) {
			return std::nullopt;
		}
	}
}

std::optional<bool> FindBool(json::Object const& object, std::string const& key)
{
	auto it = object.find(key);
	if (it == object.end())
		return std::nullopt;
	try {
		return static_cast<json::Boolean const&>(it->second);
	}
	catch (...) {
		return std::nullopt;
	}
}

std::vector<int> ParseBreakpointLines(json::Object const& arguments)
{
	std::vector<int> lines;

	if (auto breakpoints = FindArray(arguments, "breakpoints")) {
		for (auto const& item : **breakpoints) {
			try {
				auto const& breakpoint = static_cast<json::Object const&>(item);
				auto line = FindInt(breakpoint, "line");
				if (line && *line > 0)
					lines.push_back(*line);
			}
			catch (...) {
			}
		}
	}
	else if (auto raw_lines = FindArray(arguments, "lines")) {
		for (auto const& item : **raw_lines) {
			try {
				auto line = static_cast<int>(static_cast<json::Integer const&>(item));
				if (line > 0)
					lines.push_back(line);
			}
			catch (...) {
			}
		}
	}

	return lines;
}

std::string BuildTemplateSourceContent(AutomationDebugPauseRecord const& pause)
{
	if (!pause.runtime_snapshot || !pause.runtime_snapshot->template_debug)
		return {};

	auto const& state = *pause.runtime_snapshot->template_debug;
	if (state.expression && !state.expression->empty())
		return *state.expression;
	if (state.template_code && !state.template_code->empty())
		return *state.template_code;
	if (state.template_text && !state.template_text->empty())
		return *state.template_text;
	if (state.source && !state.source->fragments.empty()) {
		std::ostringstream out;
		for (size_t i = 0; i < state.source->fragments.size(); ++i) {
			if (i != 0)
				out << "\n";
			out << state.source->fragments[i].text.value_or("");
		}
		return out.str();
	}
	if (state.source && state.source->text)
		return *state.source->text;
	return {};
}

json::Object BuildSourceObject(
	AutomationDebugLocation const& location,
	std::map<std::string, int> const& source_refs)
{
	json::Object source;
	source["name"] = location.display_name;
	source["path"] = location.source_path;

	auto it = source_refs.find(location.source_path);
	if (it != source_refs.end())
		source["sourceReference"] = static_cast<json::Integer>(it->second);
	else
		source["sourceReference"] = static_cast<json::Integer>(0);
	return source;
}

json::String StoppedReason(AutomationDebugPauseReason reason)
{
	switch (reason) {
	case AutomationDebugPauseReason::Entry:
		return "entry";
	case AutomationDebugPauseReason::Breakpoint:
		return "breakpoint";
	case AutomationDebugPauseReason::Step:
		return "step";
	case AutomationDebugPauseReason::Pause:
		return "pause";
	}
	return "breakpoint";
}

std::string BuildSessionMessage(AutomationDebugTarget const& target)
{
	std::ostringstream out;
	out << "Attached to " << target.engine_name << " automation target";
	if (!target.feature_name.empty())
		out << ": " << target.feature_name;
	if (!target.script_file.empty())
		out << " (" << agi::fs::PathToGenericString(target.script_file) << ")";
	out << "\n";
	return out.str();
}

bool IsTemplateScope(std::string const& scope_name)
{
	return scope_name == "Template"
		|| scope_name == "Template Identity"
		|| scope_name == "Template Source"
		|| scope_name == "Template Target"
		|| scope_name == "Generated Lines";
}

bool IsStandaloneScope(std::string const& scope_name)
{
	return scope_name == "Globals"
		|| scope_name == "Functions"
		|| scope_name == "Runtime Globals"
		|| IsTemplateScope(scope_name);
}

std::optional<AutomationDebugScope> BuildMergedAutomationContextScope(std::vector<AutomationDebugScope> const& scopes)
{
	AutomationDebugScope merged_scope;
	merged_scope.name = "Automation Context";

	for (auto const& scope : scopes) {
		if (IsStandaloneScope(scope.name))
			continue;

		if (scope.name == "Miscellaneous APIs") {
			for (auto const& variable : scope.variables)
				merged_scope.variables.push_back(variable);
			continue;
		}

		AutomationDebugVariable grouped_scope;
		grouped_scope.name = scope.name;
		grouped_scope.value = "scope[" + std::to_string(scope.variables.size()) + "]";
		grouped_scope.value_type = "scope";
		grouped_scope.children = scope.variables;
		merged_scope.variables.push_back(std::move(grouped_scope));
	}

	if (merged_scope.variables.empty())
		return std::nullopt;
	return merged_scope;
}

std::vector<AutomationDebugScope> BuildDisplayScopes(std::vector<AutomationDebugScope> const& scopes)
{
	std::vector<AutomationDebugScope> display_scopes;
	display_scopes.reserve(scopes.size());

	for (auto const& scope : scopes) {
		if (scope.name == "Globals" || scope.name == "Functions" || scope.name == "Runtime Globals")
			display_scopes.push_back(scope);
	}

	if (auto automation_context_scope = BuildMergedAutomationContextScope(scopes))
		display_scopes.push_back(std::move(*automation_context_scope));

	for (auto const& scope : scopes) {
		if (scope.name != "Globals" && scope.name != "Functions" && scope.name != "Runtime Globals" && IsTemplateScope(scope.name))
			display_scopes.push_back(scope);
	}

	return display_scopes;
}

}

class AutomationDebugAdapter::Impl final {
	AutomationDebugService& service;
	std::string expected_token;
	std::unique_ptr<AutomationDebugAdapterConnection> connection;

	mutable std::mutex mutex;
	std::condition_variable cv;
	std::atomic<int> outgoing_seq{1};
	std::map<std::string, std::set<int>> breakpoint_lines;
	std::map<std::string, int> source_references;
	std::map<int, std::string> source_contents;
	std::map<int, std::vector<AutomationDebugVariable>> variable_handles;
	int next_source_reference = 1;
	int next_variables_reference = 1;
	bool attach_received = false;
	bool configuration_done = false;
	bool shutdown_requested = false;
	bool disconnect_requested = false;
	bool terminated_sent = false;
	bool process_announced = false;
	bool thread_started = false;
	std::thread event_thread;

	void RequestShutdown()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			shutdown_requested = true;
		}
		cv.notify_all();
		service.NotifyStateChange();
	}

	bool ShouldStop() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return shutdown_requested;
	}

	void Send(json::Object&& message)
	{
		if (!connection)
			return;

		std::string error;
		if (!connection->WriteProtocolMessage(SerializeJson(message), error))
			RequestShutdown();
	}

	void SendResponse(int request_seq, std::string const& command, json::Object body)
	{
		json::Object response;
		response["seq"] = static_cast<json::Integer>(outgoing_seq.fetch_add(1));
		response["type"] = "response";
		response["request_seq"] = static_cast<json::Integer>(request_seq);
		response["success"] = true;
		response["command"] = command;
		response["body"] = std::move(body);
		Send(std::move(response));
	}

	void SendErrorResponse(int request_seq, std::string const& command, std::string message)
	{
		json::Object response;
		response["seq"] = static_cast<json::Integer>(outgoing_seq.fetch_add(1));
		response["type"] = "response";
		response["request_seq"] = static_cast<json::Integer>(request_seq);
		response["success"] = false;
		response["command"] = command;
		response["message"] = std::move(message);
		Send(std::move(response));
	}

	void SendEvent(std::string const& event_name, json::Object body)
	{
		json::Object event;
		event["seq"] = static_cast<json::Integer>(outgoing_seq.fetch_add(1));
		event["type"] = "event";
		event["event"] = event_name;
		event["body"] = std::move(body);
		Send(std::move(event));
	}

	void SendOutput(std::string category, std::string output)
	{
		json::Object body;
		body["category"] = std::move(category);
		body["output"] = std::move(output);
		SendEvent("output", std::move(body));
	}

	void SendInitialized()
	{
		SendEvent("initialized", json::Object{});
	}

	void SendProcess(AutomationDebugTarget const& target)
	{
		json::Object body;
		body["name"] = target.feature_name.empty()
			? json::String("Aegisub Automation")
			: json::String("Aegisub Automation: " + target.feature_name);
		body["isLocalProcess"] = true;
		body["startMethod"] = "attach";
		SendEvent("process", std::move(body));
	}

	void SendThreadEvent(std::string reason)
	{
		json::Object body;
		body["reason"] = std::move(reason);
		body["threadId"] = static_cast<json::Integer>(kAutomationThreadId);
		SendEvent("thread", std::move(body));
	}

	void SendStopped(AutomationDebugPauseRecord const& pause)
	{
		json::Object body;
		body["reason"] = StoppedReason(pause.reason);
		body["threadId"] = static_cast<json::Integer>(kAutomationThreadId);
		body["allThreadsStopped"] = true;
		body["description"] = ToString(pause.reason);
		body["text"] = pause.location.display_name;
		SendEvent("stopped", std::move(body));
	}

	void SendContinued()
	{
		json::Object body;
		body["threadId"] = static_cast<json::Integer>(kAutomationThreadId);
		body["allThreadsContinued"] = true;
		SendEvent("continued", std::move(body));
	}

	void SendTerminatedOnce()
	{
		bool should_send = false;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!terminated_sent) {
				terminated_sent = true;
				should_send = true;
			}
		}
		if (!should_send)
			return;
		SendEvent("terminated", json::Object{});
	}

	std::vector<AutomationDebugBreakpoint> BuildBreakpointsLocked() const
	{
		std::vector<AutomationDebugBreakpoint> values;
		for (auto const& [source_path, lines] : breakpoint_lines) {
			for (int line : lines)
				values.push_back({ source_path, line, true });
		}
		return values;
	}

	void ApplyLaunchConfiguration()
	{
		AutomationDebugLaunchRequest request;
		{
			std::lock_guard<std::mutex> lock(mutex);
			request = service.GetLaunchConfiguration();
			request.enabled = true;
			request.breakpoints = BuildBreakpointsLocked();
		}
		service.SetLaunchConfiguration(std::move(request));
		if (auto session = service.GetCurrentSession())
			session->SetBreakpoints(service.GetLaunchConfiguration().breakpoints);
	}

	void ResetPauseArtifactsLocked(
		AutomationDebugPauseRecord const& pause,
		std::shared_ptr<LuaWorkspaceSourceRegistry> const& registry)
	{
		variable_handles.clear();
		source_references.clear();
		source_contents.clear();
		next_source_reference = 1;
		next_variables_reference = 1;

		if (registry) {
			for (auto const& frame : pause.frames) {
				auto const& path = frame.location.source_path;
				if (source_references.contains(path))
					continue;
				if (auto source = registry->Find(path)) {
					int const source_reference = next_source_reference++;
					source_references[path] = source_reference;
					source_contents[source_reference] = source->text;
				}
			}
		}

		if (pause.location.source_kind == "template" && !source_references.contains(pause.location.source_path)) {
			auto content = BuildTemplateSourceContent(pause);
			if (!content.empty()) {
				int const source_reference = next_source_reference++;
				source_references[pause.location.source_path] = source_reference;
				source_contents[source_reference] = std::move(content);
			}
		}
	}

	int RegisterVariablesLocked(std::vector<AutomationDebugVariable> variables)
	{
		int const handle = next_variables_reference++;
		variable_handles.emplace(handle, std::move(variables));
		return handle;
	}

	std::optional<AutomationDebugStateSnapshot> CurrentState() const
	{
		auto session = service.GetCurrentSession();
		if (!session)
			return std::nullopt;
		return session->GetStateSnapshot();
	}

	std::optional<AutomationDebugPauseRecord> CurrentPause() const
	{
		auto state = CurrentState();
		if (!state || !state->current_pause)
			return std::nullopt;
		return state->current_pause;
	}

	void HandleInitialize(int request_seq, std::string const& command)
	{
		json::Object capabilities;
		capabilities["supportsConfigurationDoneRequest"] = true;
		capabilities["supportsStepInRequest"] = true;
		capabilities["supportsStepOutRequest"] = true;
		capabilities["supportsTerminateRequest"] = true;
		capabilities["supportsPauseRequest"] = true;
		capabilities["supportsEvaluateForHovers"] = false;
		capabilities["supportsLoadedSourcesRequest"] = false;
		capabilities["supportsConditionalBreakpoints"] = false;
		capabilities["supportsFunctionBreakpoints"] = false;
		capabilities["supportsExceptionInfoRequest"] = false;
		SendResponse(request_seq, command, std::move(capabilities));
	}

	void HandleAttach(int request_seq, std::string const& command, json::Object const& arguments)
	{
		auto token = FindString(arguments, "token");
		if (!expected_token.empty() && (!token || *token != expected_token)) {
			SendErrorResponse(request_seq, command, "attach requires a valid debug token");
			return;
		}

		auto request = service.GetLaunchConfiguration();
		request.enabled = true;
		request.stop_on_entry = FindBool(arguments, "stopOnEntry").value_or(false);
		request.auto_step_count = FindInt(arguments, "autoStepCount").value_or(0);
		auto max_pauses = FindInt(arguments, "maxPauses").value_or(static_cast<int>(request.max_pauses == 0 ? 512 : request.max_pauses));
		if (max_pauses <= 0)
			max_pauses = 512;
		request.max_pauses = static_cast<size_t>(max_pauses);

		if (!service.ReportClientState(true, false)) {
			SendErrorResponse(request_seq, command, "a local automation debug session is active");
			return;
		}
		service.SetLaunchConfiguration(std::move(request));

		{
			std::lock_guard<std::mutex> lock(mutex);
			attach_received = true;
		}
		cv.notify_all();

		SendResponse(request_seq, command, json::Object{});
		SendInitialized();
	}

	void HandleSetBreakpoints(int request_seq, std::string const& command, json::Object const& arguments)
	{
		auto source_object = FindObject(arguments, "source");
		auto source_path = source_object ? FindString(**source_object, "path") : std::nullopt;
		if (!source_path || source_path->empty()) {
			SendErrorResponse(request_seq, command, "setBreakpoints requires source.path");
			return;
		}

		auto normalized_source = NormalizeAutomationDebugSource(*source_path);
		auto lines = ParseBreakpointLines(arguments);
		std::sort(lines.begin(), lines.end());
		lines.erase(std::unique(lines.begin(), lines.end()), lines.end());

		json::Array response_breakpoints;
		{
			std::lock_guard<std::mutex> lock(mutex);
			auto& bucket = breakpoint_lines[normalized_source];
			bucket.clear();
			for (int line : lines)
				bucket.insert(line);
		}
		ApplyLaunchConfiguration();

		for (int line : lines) {
			json::Object breakpoint;
			breakpoint["verified"] = true;
			breakpoint["line"] = static_cast<json::Integer>(line);
			response_breakpoints.push_back(std::move(breakpoint));
		}

		json::Object body;
		body["breakpoints"] = std::move(response_breakpoints);
		SendResponse(request_seq, command, std::move(body));
	}

	void HandleSetExceptionBreakpoints(int request_seq, std::string const& command)
	{
		SendResponse(request_seq, command, json::Object{});
	}

	void HandleConfigurationDone(int request_seq, std::string const& command)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!attach_received) {
				SendErrorResponse(request_seq, command, "configurationDone received before attach");
				return;
			}
		}
		if (!service.ReportClientState(true, true)) {
			SendErrorResponse(request_seq, command, "a local automation debug session is active");
			return;
		}
		{
			std::scoped_lock lock(mutex);
			configuration_done = true;
		}
		cv.notify_all();

		SendResponse(request_seq, command, json::Object{});
		SendOutput("console", "Waiting for an Automation invocation in Aegisub.\n");
	}

	void HandleThreads(int request_seq, std::string const& command)
	{
		json::Object thread;
		thread["id"] = static_cast<json::Integer>(kAutomationThreadId);
		thread["name"] = "Automation";
		json::Array threads;
		threads.push_back(std::move(thread));

		json::Object body;
		body["threads"] = std::move(threads);
		SendResponse(request_seq, command, std::move(body));
	}

	void HandleStackTrace(int request_seq, std::string const& command)
	{
		auto pause = CurrentPause();
		if (!pause) {
			SendErrorResponse(request_seq, command, "stackTrace requested while not paused");
			return;
		}

		json::Array stack_frames;
		std::map<std::string, int> source_refs;
		{
			std::lock_guard<std::mutex> lock(mutex);
			source_refs = source_references;
		}

		for (auto const& frame : pause->frames) {
			json::Object stack_frame;
			stack_frame["id"] = static_cast<json::Integer>(frame.level + 1);
			stack_frame["name"] = frame.function_name;
			stack_frame["line"] = static_cast<json::Integer>(frame.location.line);
			stack_frame["column"] = static_cast<json::Integer>(frame.location.column <= 0 ? 1 : frame.location.column);
			stack_frame["source"] = BuildSourceObject(frame.location, source_refs);
			stack_frames.push_back(std::move(stack_frame));
		}

		json::Object body;
		body["stackFrames"] = std::move(stack_frames);
		body["totalFrames"] = static_cast<json::Integer>(pause->frames.size());
		SendResponse(request_seq, command, std::move(body));
	}

	void HandleScopes(int request_seq, std::string const& command, json::Object const& arguments)
	{
		auto pause = CurrentPause();
		if (!pause) {
			SendErrorResponse(request_seq, command, "scopes requested while not paused");
			return;
		}

		auto frame_id = FindInt(arguments, "frameId");
		if (!frame_id || *frame_id <= 0 || static_cast<size_t>(*frame_id) > pause->frames.size()) {
			SendErrorResponse(request_seq, command, "invalid frameId");
			return;
		}

		size_t const frame_index = static_cast<size_t>(*frame_id - 1);
		auto const& frame = pause->frames[frame_index];
		json::Array scopes;

		{
			std::lock_guard<std::mutex> lock(mutex);

			json::Object locals_scope;
			locals_scope["name"] = "Locals";
			locals_scope["presentationHint"] = "locals";
			locals_scope["variablesReference"] = static_cast<json::Integer>(RegisterVariablesLocked(frame.locals));
			locals_scope["expensive"] = false;
			scopes.push_back(std::move(locals_scope));

			if (!frame.upvalues.empty()) {
				json::Object captured_scope;
				captured_scope["name"] = "Captured Variables";
				captured_scope["presentationHint"] = "registers";
				captured_scope["variablesReference"] = static_cast<json::Integer>(RegisterVariablesLocked(frame.upvalues));
				captured_scope["expensive"] = false;
				scopes.push_back(std::move(captured_scope));
			}

			if (frame_index == 0) {
				for (auto const& scope : BuildDisplayScopes(pause->scopes)) {
					json::Object dap_scope;
					dap_scope["name"] = scope.name;
					dap_scope["presentationHint"] = (scope.name == "Globals" || scope.name == "Functions" || scope.name == "Runtime Globals")
						? json::String("globals")
						: json::String("locals");
					dap_scope["variablesReference"] = static_cast<json::Integer>(RegisterVariablesLocked(scope.variables));
					dap_scope["expensive"] = scope.name == "Runtime Globals";
					scopes.push_back(std::move(dap_scope));
				}
			}
		}

		json::Object body;
		body["scopes"] = std::move(scopes);
		SendResponse(request_seq, command, std::move(body));
	}

	void HandleVariables(int request_seq, std::string const& command, json::Object const& arguments)
	{
		auto reference = FindInt(arguments, "variablesReference");
		if (!reference || *reference <= 0) {
			SendErrorResponse(request_seq, command, "variables requires a valid variablesReference");
			return;
		}

		std::vector<AutomationDebugVariable> variables;
		{
			std::lock_guard<std::mutex> lock(mutex);
			auto it = variable_handles.find(*reference);
			if (it == variable_handles.end()) {
				SendErrorResponse(request_seq, command, "unknown variablesReference");
				return;
			}
			variables = it->second;
		}

		json::Array dap_variables;
		{
			std::lock_guard<std::mutex> lock(mutex);
			for (auto const& variable : variables) {
				json::Object item;
				item["name"] = variable.name;
				item["value"] = variable.value;
				item["type"] = variable.value_type;
				if (variable.children.empty())
					item["variablesReference"] = static_cast<json::Integer>(0);
				else
					item["variablesReference"] = static_cast<json::Integer>(RegisterVariablesLocked(variable.children));
				dap_variables.push_back(std::move(item));
			}
		}

		json::Object body;
		body["variables"] = std::move(dap_variables);
		SendResponse(request_seq, command, std::move(body));
	}

	void HandleSource(int request_seq, std::string const& command, json::Object const& arguments)
	{
		auto source_reference = FindInt(arguments, "sourceReference");
		if (!source_reference || *source_reference <= 0) {
			SendErrorResponse(request_seq, command, "source requires sourceReference");
			return;
		}

		std::string content;
		{
			std::lock_guard<std::mutex> lock(mutex);
			auto it = source_contents.find(*source_reference);
			if (it == source_contents.end()) {
				SendErrorResponse(request_seq, command, "unknown sourceReference");
				return;
			}
			content = it->second;
		}

		json::Object body;
		body["content"] = std::move(content);
		body["mimeType"] = "text/x-lua";
		SendResponse(request_seq, command, std::move(body));
	}

	void HandleContinueLike(int request_seq, std::string const& command, AutomationDebugResumeAction action)
	{
		auto session = service.GetCurrentSession();
		if (!session || !session->Resume(action)) {
			SendErrorResponse(request_seq, command, "debugger is not currently paused");
			return;
		}

		json::Object body;
		body["allThreadsContinued"] = true;
		SendResponse(request_seq, command, std::move(body));
		SendContinued();
	}

	void HandlePause(int request_seq, std::string const& command)
	{
		auto session = service.GetCurrentSession();
		if (!session) {
			SendErrorResponse(request_seq, command, "debug session is not active");
			return;
		}
		session->RequestPause();
		SendResponse(request_seq, command, json::Object{});
	}

	void HandleDisconnect(int request_seq, std::string const& command)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			disconnect_requested = true;
		}
		if (auto session = service.GetCurrentSession())
			session->Detach();
		service.SetLaunchConfiguration({});
		service.ReportClientState(false, false);
		SendResponse(request_seq, command, json::Object{});
		RequestShutdown();
	}

	void HandleTerminate(int request_seq, std::string const& command)
	{
		HandleDisconnect(request_seq, command);
	}

	void HandleRequest(json::Object const& request)
	{
		auto request_seq = FindInt(request, "seq").value_or(0);
		auto command = FindString(request, "command").value_or("");
		auto arguments = FindObject(request, "arguments");
		json::Object empty_arguments;

		if (command == "initialize") {
			HandleInitialize(request_seq, command);
			return;
		}
		if (service.HasLocalSession() && command != "disconnect" && command != "terminate") {
			SendErrorResponse(request_seq, command, "a local automation debug session is active");
			return;
		}
		if (command == "attach") {
			HandleAttach(request_seq, command, arguments ? **arguments : empty_arguments);
			return;
		}
		if (command == "setBreakpoints") {
			HandleSetBreakpoints(request_seq, command, arguments ? **arguments : empty_arguments);
			return;
		}
		if (command == "setExceptionBreakpoints") {
			HandleSetExceptionBreakpoints(request_seq, command);
			return;
		}
		if (command == "configurationDone") {
			HandleConfigurationDone(request_seq, command);
			return;
		}
		if (command == "threads") {
			HandleThreads(request_seq, command);
			return;
		}
		if (command == "stackTrace") {
			HandleStackTrace(request_seq, command);
			return;
		}
		if (command == "scopes") {
			HandleScopes(request_seq, command, arguments ? **arguments : empty_arguments);
			return;
		}
		if (command == "variables") {
			HandleVariables(request_seq, command, arguments ? **arguments : empty_arguments);
			return;
		}
		if (command == "source") {
			HandleSource(request_seq, command, arguments ? **arguments : empty_arguments);
			return;
		}
		if (command == "continue") {
			HandleContinueLike(request_seq, command, AutomationDebugResumeAction::Continue);
			return;
		}
		if (command == "next") {
			HandleContinueLike(request_seq, command, AutomationDebugResumeAction::Next);
			return;
		}
		if (command == "stepIn") {
			HandleContinueLike(request_seq, command, AutomationDebugResumeAction::StepIn);
			return;
		}
		if (command == "stepOut") {
			HandleContinueLike(request_seq, command, AutomationDebugResumeAction::StepOut);
			return;
		}
		if (command == "pause") {
			HandlePause(request_seq, command);
			return;
		}
		if (command == "disconnect") {
			HandleDisconnect(request_seq, command);
			return;
		}
		if (command == "terminate") {
			HandleTerminate(request_seq, command);
			return;
		}

		SendErrorResponse(request_seq, command, "unsupported request: " + command);
	}

	void InputLoop()
	{
		while (!ShouldStop()) {
			std::string payload;
			std::string error;
			if (!connection || !connection->ReadProtocolMessage(payload, error))
				break;

			try {
				json::UnknownElement message;
				std::stringstream stream(payload);
				json::Reader::Read(message, stream);
				auto const& request = static_cast<json::Object const&>(message);
				auto type = FindString(request, "type").value_or("");
				if (type != "request")
					continue;
				HandleRequest(request);
			}
			catch (std::exception const& e) {
				SendOutput("stderr", std::string("DAP request error: ") + e.what() + "\n");
			}
			catch (...) {
				SendOutput("stderr", "DAP request error\n");
			}
		}

		RequestShutdown();
	}

	void EventLoop()
	{
		{
			std::unique_lock<std::mutex> lock(mutex);
			cv.wait(lock, [&] {
				return shutdown_requested || (attach_received && configuration_done);
			});
			if (shutdown_requested)
				return;
		}

		auto service_snapshot = service.GetStateSnapshot();
		auto service_version = service_snapshot.version;
		std::shared_ptr<AutomationDebugSession> observed_session;
		size_t session_version = 0;
		size_t last_pause_sequence = 0;

		for (;;) {
			if (ShouldStop())
				return;

			service_snapshot = service.GetStateSnapshot();
			service_version = service_snapshot.version;
			if (!service_snapshot.enabled) {
				SendOutput("console", "Automation debug mode was disabled in Aegisub.\n");
				SendTerminatedOnce();
				RequestShutdown();
				return;
			}

			auto session = service.GetCurrentSession();
			if (!session) {
				if (thread_started) {
					SendThreadEvent("exited");
					thread_started = false;
				}
				process_announced = false;
				observed_session.reset();
				last_pause_sequence = 0;
				auto next_state = service.WaitForStateChange(service_version);
				bool disconnected = false;
				{
					std::lock_guard<std::mutex> lock(mutex);
					disconnected = disconnect_requested;
				}
				if (!next_state.enabled && !disconnected) {
					SendOutput("console", "Automation debug mode was disabled in Aegisub.\n");
					SendTerminatedOnce();
					RequestShutdown();
					return;
				}
				continue;
			}

			if (session != observed_session) {
				observed_session = session;
				session_version = 0;
				last_pause_sequence = 0;
				if (!process_announced) {
					SendProcess(observed_session->GetTarget());
					process_announced = true;
				}
				if (!thread_started) {
					SendThreadEvent("started");
					thread_started = true;
				}
				SendOutput("console", BuildSessionMessage(observed_session->GetTarget()));
			}

			auto snapshot = observed_session->GetStateSnapshot();
			if (snapshot.version == session_version)
				snapshot = observed_session->WaitForStateChange(session_version);
			session_version = snapshot.version;

			if (snapshot.current_pause && snapshot.state == AutomationDebugSessionState::Paused) {
				if (snapshot.current_pause->sequence != last_pause_sequence) {
					auto registry = observed_session->GetSourceRegistry();
					{
						std::lock_guard<std::mutex> lock(mutex);
						ResetPauseArtifactsLocked(*snapshot.current_pause, registry);
					}
					last_pause_sequence = snapshot.current_pause->sequence;
					SendStopped(*snapshot.current_pause);
				}
			}

			if (snapshot.state == AutomationDebugSessionState::Detached && !snapshot.invocation_active) {
				if (thread_started) {
					SendThreadEvent("exited");
					thread_started = false;
				}
				process_announced = false;
				observed_session.reset();
				last_pause_sequence = 0;
			}
		}
	}

public:
	Impl(
		AutomationDebugService& service,
		std::string expected_token,
		std::unique_ptr<AutomationDebugAdapterConnection> connection)
	: service(service)
	, expected_token(std::move(expected_token))
	, connection(std::move(connection))
	{
	}

	void Stop()
	{
		RequestShutdown();
		if (connection)
			connection->Close();
		if (auto session = service.GetCurrentSession())
			session->Detach();
	}

	void Run()
	{
		if (service.ReportClientState(true, false))
			service.SetLaunchConfiguration({});

		event_thread = std::thread([this] { EventLoop(); });
		InputLoop();

		service.SetLaunchConfiguration({});
		service.ReportClientState(false, false);
		if (auto session = service.GetCurrentSession())
			session->Detach();
		if (connection)
			connection->Close();
		if (event_thread.joinable())
			event_thread.join();
	}
};

AutomationDebugAdapter::AutomationDebugAdapter(
	AutomationDebugService& service,
	std::string expected_token,
	std::unique_ptr<AutomationDebugAdapterConnection> connection)
: impl(std::make_unique<Impl>(service, std::move(expected_token), std::move(connection)))
{
}

AutomationDebugAdapter::~AutomationDebugAdapter() = default;

void AutomationDebugAdapter::Stop()
{
	impl->Stop();
}

void AutomationDebugAdapter::Run()
{
	impl->Run();
}

}
