#include "../../../src/automation/karaoke_line_classifier.h"
#include "../../../src/automation/lua_language_environment.h"
#include "../../../src/automation/lua_language_server.h"

#include <libaegisub/fs.h>
#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>

#include <lua.hpp>
#include <luajit.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Automation4::LuaLanguageConfiguration;
using Automation4::LuaLanguageDocument;
using Automation4::LuaLanguageEvent;
using Automation4::LuaLanguageRequest;
using Automation4::LuaLanguageServer;

thread_local int hook_budget = 0;

void InstructionHook(lua_State *state, lua_Debug *) {
	if (--hook_budget == 0)
		luaL_error(state, "Lua instruction budget exceeded");
}

void Require(bool condition, std::string const& message) {
	if (!condition)
		throw std::runtime_error(message);
}

fs::path RelativePath(char const *text) {
	fs::path path(text);
	Require(!path.empty() && !path.is_absolute() && !path.has_root_name(), "Server and fixture paths must be repository-relative");
	for (auto const& part : path)
		Require(part != "..", "Relative paths may not traverse outside the repository");
	return path;
}

std::string ReadFile(fs::path const& path) {
	std::ifstream file(path, std::ios::binary);
	Require(static_cast<bool>(file), "Cannot read " + path.generic_string());
	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void WriteFile(fs::path const& path, std::string_view content) {
	std::ofstream file(path, std::ios::binary);
	Require(static_cast<bool>(file), "Cannot write " + path.generic_string());
	file.write(content.data(), static_cast<std::streamsize>(content.size()));
	Require(static_cast<bool>(file), "Could not finish writing " + path.generic_string());
}

std::string Json(std::string_view text) {
	std::string result = "\"";
	for (unsigned char byte : text) {
		switch (byte) {
			case '"': result += "\\\""; break;
			case '\\': result += "\\\\"; break;
			case '\n': result += "\\n"; break;
			case '\r': result += "\\r"; break;
			case '\t': result += "\\t"; break;
			default:
				if (byte < 0x20) {
					constexpr char hex[] = "0123456789ABCDEF";
					result += "\\u00";
					result += hex[byte >> 4];
					result += hex[byte & 15];
				}
				else
					result += static_cast<char>(byte);
		}
	}
	return result + '"';
}

std::string EventJson(LuaLanguageEvent const& event) {
	std::string json = "{\"kind\":" + std::to_string(static_cast<int>(event.kind)) +
					   ",\"generation\":" + std::to_string(event.generation) +
					   ",\"request\":" + std::to_string(event.request) +
					   ",\"position\":" + std::to_string(event.position) +
					   ",\"ready\":" + (event.ready ? "true" : "false") +
					   ",\"text\":" + Json(event.text) + ",\"completions\":[";
	for (auto const& completion : event.completions) {
		if (json.back() != '[')
			json += ',';
		json += "{\"label\":" + Json(completion.label) + ",\"detail\":" + Json(completion.detail) +
				",\"caret\":" + std::to_string(completion.caret) + ",\"edits\":[";
		for (auto const& edit : completion.edits) {
			if (json.back() != '[')
				json += ',';
			json += "{\"start\":" + std::to_string(edit.start) + ",\"end\":" + std::to_string(edit.end) +
					",\"text\":" + Json(edit.text) + '}';
		}
		json += "]}";
	}
	json += "],\"diagnostics\":[";
	for (auto const& diagnostic : event.diagnostics) {
		if (json.back() != '[')
			json += ',';
		json += "{\"start\":" + std::to_string(diagnostic.start) + ",\"end\":" + std::to_string(diagnostic.end) +
				",\"severity\":" + std::to_string(diagnostic.severity) + ",\"message\":" + Json(diagnostic.message) + '}';
	}
	return json + "]}";
}

struct Observer {
	LuaLanguageServer& server;
	Clock::time_point deadline;
	fs::path artifacts;
	std::ofstream events;
	std::vector<LuaLanguageEvent> history;

	Observer(LuaLanguageServer& server, Clock::time_point deadline, fs::path const& artifacts)
		: server(server), deadline(deadline), artifacts(artifacts), events(artifacts / "events.ndjson", std::ios::binary) {
		Require(static_cast<bool>(events), "Cannot create language event artifact");
	}
	~Observer() { Snapshot("final"); }

	void Snapshot(std::string_view name) noexcept {
		try {
			auto const cache = artifacts / "cache";
			if (!fs::is_directory(cache))
				return;
			for (auto const& entry : fs::directory_iterator(cache)) {
				if (!entry.is_directory() || !entry.path().filename().string().starts_with("session-"))
					continue;
				auto const evidence = artifacts / "server-evidence" / std::string(name) / entry.path().filename();
				fs::create_directories(evidence);
				for (auto const& relative : {"config.json", "library/host.lua", "stderr.log"}) {
					auto const source = entry.path() / relative;
					if (fs::is_regular_file(source))
						fs::copy_file(source, evidence / fs::path(relative).filename(), fs::copy_options::overwrite_existing);
				}
				if (fs::is_directory(entry.path() / "log")) {
					fs::create_directories(evidence / "log");
					for (auto const& log : fs::directory_iterator(entry.path() / "log"))
						if (log.is_regular_file() && log.path().extension() == ".log")
							fs::copy_file(log.path(), evidence / "log" / log.path().filename(), fs::copy_options::overwrite_existing);
				}
			}
		}
		catch (std::exception const& error) {
			try {
				WriteFile(artifacts / "server-evidence-error.txt", error.what());
			}
			catch (...) {
			}
		}
	}

	void Pump() {
		for (auto& event : server.Drain()) {
			events << EventJson(event) << '\n';
			events.flush();
			history.push_back(std::move(event));
		}
	}

	LuaLanguageEvent Wait(std::function<bool(LuaLanguageEvent const&)> const& predicate, std::chrono::seconds timeout, std::string const& reason, std::size_t from = 0) {
		auto const end = std::min(deadline, Clock::now() + timeout);
		while (Clock::now() < end) {
			Pump();
			for (std::size_t i = from; i < history.size(); ++i)
				if (predicate(history[i]))
					return history[i];
			std::this_thread::sleep_for(20ms);
		}
		throw std::runtime_error("Timed out waiting for " + reason);
	}

	LuaLanguageEvent Ready(std::uint64_t generation) {
		return Wait([&](auto const& event) { return event.kind == LuaLanguageEvent::Kind::Status && event.generation == generation && event.ready; },
					20s, "LuaLS ready generation " + std::to_string(generation));
	}

	LuaLanguageEvent Response(LuaLanguageEvent::Kind kind, std::uint64_t generation, std::uint64_t request) {
		return Wait([&](auto const& event) { return event.kind == kind && event.generation == generation && event.request == request; },
					12s, "language response " + std::to_string(request));
	}
};

std::string CrLf(std::string_view source) {
	std::string result;
	for (char byte : source) {
		if (byte == '\n')
			result += '\r';
		result += byte;
	}
	return result;
}

std::size_t After(std::string const& source, std::string_view text) {
	auto found = source.find(text);
	Require(found != std::string::npos, "Fixture has no requested cursor marker " + std::string(text));
	return found + text.size();
}

std::string ReplaceOnce(std::string source, std::string_view before, std::string_view after) {
	auto const found = source.find(before);
	Require(found != std::string::npos, "Fixture replacement marker is missing");
	source.replace(found, before.size(), after);
	return source;
}

std::string Execute(std::string_view source) {
	using State = std::unique_ptr<lua_State, decltype(&lua_close)>;
	State state(luaL_newstate(), &lua_close);
	Require(static_cast<bool>(state), "Could not create independent LuaJIT state");
	luaL_openlibs(state.get());
	Require(luaJIT_setmode(state.get(), 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF) != 0, "Could not disable JIT compilation");
	hook_budget = 2000;
	lua_sethook(state.get(), InstructionHook, LUA_MASKCOUNT, 1000);
	if (luaL_loadbuffer(state.get(), source.data(), source.size(), "@language-e2e-source") != 0)
		throw std::runtime_error(lua_tostring(state.get(), -1));
	if (lua_pcall(state.get(), 0, 1, 0) != 0)
		throw std::runtime_error(lua_tostring(state.get(), -1));
	Require(lua_type(state.get(), -1) == LUA_TSTRING, "Independent Lua source did not return a string");
	size_t size = 0;
	auto const *value = lua_tolstring(state.get(), -1, &size);
	return {value, size};
}

LuaLanguageDocument Document(std::string identity, fs::path filename, std::string source, std::uint64_t revision = 1) {
	return {.identity = std::move(identity), .filename = std::move(filename), .source = std::move(source), .revision = revision};
}

std::string Apply(std::string source, std::vector<Automation4::LuaLanguageEdit> const& edits) {
	for (auto const& edit : std::ranges::reverse_view(edits)) {
		Require(edit.start <= edit.end && edit.end <= source.size(), "Completion edit extends outside source");
		source.replace(edit.start, edit.end - edit.start, edit.text);
	}
	return source;
}

#ifdef _WIN32
using ProcessHandle = std::unique_ptr<void, decltype(&CloseHandle)>;

std::vector<ProcessHandle> OwnedServerChildren(fs::path const& expected_executable, bool terminate_access = false) {
	HANDLE raw_snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	Require(raw_snapshot != INVALID_HANDLE_VALUE, "Could not inspect the E2E process tree");
	ProcessHandle snapshot(raw_snapshot, CloseHandle);
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	Require(Process32FirstW(snapshot.get(), &entry) != 0, "Could not begin the E2E process enumeration");
	std::vector<ProcessHandle> children;
	do {
		if (entry.th32ParentProcessID != GetCurrentProcessId() || _wcsicmp(entry.szExeFile, L"lua-language-server.exe") != 0)
			continue;
		auto const access = PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | (terminate_access ? PROCESS_TERMINATE : 0);
		HANDLE raw_process = OpenProcess(access, FALSE, entry.th32ProcessID);
		Require(raw_process != nullptr, "Could not inspect a LuaLS child of this E2E process");
		ProcessHandle process(raw_process, CloseHandle);
		std::wstring filename(32768, L'\0');
		auto length = static_cast<DWORD>(filename.size());
		Require(QueryFullProcessImageNameW(process.get(), 0, filename.data(), &length) != 0, "Could not verify the owned LuaLS executable path");
		filename.resize(length);
		Require(fs::equivalent(fs::path(filename), expected_executable), "A different LuaLS executable is a direct child of this E2E process");
		children.push_back(std::move(process));
	} while (Process32NextW(snapshot.get(), &entry));
	return children;
}

void TerminateOwnedServer(fs::path const& expected_executable) {
	auto children = OwnedServerChildren(expected_executable, true);
	Require(children.size() == 1, "Expected exactly one verified real LuaLS child before fault injection");
	Require(TerminateProcess(children.front().get(), 23) != 0, "Could not terminate the verified LuaLS child");
	Require(WaitForSingleObject(children.front().get(), 5000) == WAIT_OBJECT_0, "Verified LuaLS child did not exit within five seconds");
}

void WaitOwnedChildExit(fs::path const& expected_executable, Clock::time_point deadline) {
	auto const end = std::min(deadline, Clock::now() + 3s);
	while (Clock::now() < end) {
		if (OwnedServerChildren(expected_executable).empty())
			return;
		std::this_thread::sleep_for(20ms);
	}
	Require(OwnedServerChildren(expected_executable).empty(), "Timed-out LuaLS child did not exit within three seconds");
}

int RunSilentServer(std::string_view mode) {
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	std::ofstream observed("observed.ndjson", std::ios::binary);
	if (!observed)
		return 2;
	for (;;) {
		std::string line;
		std::size_t length = 0;
		bool has_length = false;
		while (std::getline(std::cin, line)) {
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty())
				break;
			if (line.starts_with("Content-Length: ")) {
				auto const value = std::string_view(line).substr(16);
				auto const parsed = std::from_chars(value.data(), value.data() + value.size(), length);
				if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || length > 8 * 1024 * 1024)
					return 3;
				has_length = true;
			}
		}
		if (!std::cin || !has_length)
			return 0;
		std::string body(length, '\0');
		std::cin.read(body.data(), static_cast<std::streamsize>(length));
		if (!std::cin)
			return 4;
		json::UnknownElement parsed;
		std::istringstream input(body);
		json::Reader::Read(parsed, input);
		auto const& message = static_cast<json::Object const&>(parsed);
		auto found = message.find("method");
		if (found == message.end())
			continue;
		auto const& method = static_cast<json::String const&>(found->second);
		observed << "{\"method\":" << Json(method) << "}\n";
		observed.flush();
		if (mode == "startup-silence")
			continue;
		auto id = message.find("id");
		if (id == message.end())
			continue;
		auto const number = static_cast<json::Integer const&>(id->second);
		std::string response;
		if (method == "initialize")
			response = R"({"jsonrpc":"2.0","id":)" + std::to_string(number) + R"(,"result":{"capabilities":{"positionEncoding":"utf-16"}}})";
		else if (method == "textDocument/documentSymbol")
			response = R"({"jsonrpc":"2.0","id":)" + std::to_string(number) + R"(,"result":[]})";
		else if (method == "textDocument/signatureHelp")
			response = R"({"jsonrpc":"2.0","id":)" + std::to_string(number) + R"(,"error":{"code":-32603,"message":"controlled signature failure"}})";
		else if (method == "shutdown")
			response = R"({"jsonrpc":"2.0","id":)" + std::to_string(number) + R"(,"result":null})";
		if (!response.empty()) {
			std::cout << "Content-Length: " << response.size() << "\r\n\r\n"
					  << response << std::flush;
			if (!std::cout)
				return 5;
		}
	}
}
#endif

std::string Manifest(std::vector<std::pair<std::string, std::string>> const& results, std::string_view failing = {}) {
	std::string text = "{\"steps\":[";
	for (auto const& [name, status] : results) {
		if (text.back() != '[')
			text += ',';
		text += "{\"name\":" + Json(name) + ",\"status\":" + Json(status) + '}';
	}
	for (auto const& name : {"prepare-real-source", "hover-and-signature-after-unicode", "versioned-undefined-diagnostic",
							 "unversioned-empty-is-unconfirmed", "queued-nonidentifier-completion", "saved-source-independent-business",
							 "file-to-virtual-quick-request-and-stale-isolation", "actual-karaoke-environment", "ordinary-host-isolation",
							 "include-module-completion-and-signature", "selected-completion-replacement",
							 "karaoke-scope-matrix", "chunk-identity-local-isolation", "unicode-config-switch", "null-document-disable-and-reopen",
							 "owned-server-termination-and-recovery", "missing-and-incomplete-server-no-fallback", "startup-timeout", "request-timeout"}) {
		if (std::ranges::any_of(results, [&](auto const& step) { return step.first == name; }))
			continue;
		if (text.back() != '[')
			text += ',';
		text += R"({"name":)" + Json(name) + R"(,"status":"not-run"})";
	}
	text += "],\"error\":" + Json(failing) + '}';
	return text;
}

}

int main(int argc, char **argv) {
#ifdef _WIN32
	if (argc > 0 && fs::path(argv[0]).filename() == L"lua-language-server.exe") {
		auto const marker = fs::current_path() / "main.lua";
		if (fs::is_regular_file(marker)) {
			auto const mode = ReadFile(marker);
			if (mode == "startup-silence" || mode == "request-silence")
				return RunSilentServer(mode);
		}
	}
#endif
	if (argc != 4) {
		std::cerr << "Usage: language-server-e2e <repo-relative-LuaLS-dir> <repo-relative-fixture-dir> <artifacts-dir>\n";
		return 2;
	}
	fs::path artifacts;
	std::vector<std::pair<std::string, std::string>> steps;
	std::string current_step;
	try {
		auto const started = Clock::now();
		auto const deadline = started + 110s;
		auto const server_path = RelativePath(argv[1]);
		auto const fixtures = RelativePath(argv[2]);
		artifacts = fs::path(argv[3]);
		Require(!artifacts.empty() && !fs::exists(artifacts), "Use a fresh nonempty language E2E artifact directory");
		fs::create_directories(artifacts);
		auto step = [&](std::string name, auto task) {
			current_step = name;
			task();
			steps.emplace_back(std::move(name), "passed");
			WriteFile(artifacts / "manifest.json", Manifest(steps));
		};
		LuaLanguageConfiguration configuration{.directory = server_path, .cache_directory = artifacts / "cache"};
		fs::create_directories(configuration.cache_directory);
		LuaLanguageServer server;
		Observer observer(server, deadline, artifacts);
		std::string source, expected;
		fs::path input;
		std::uint64_t generation = 0;
		step("prepare-real-source", [&] {
			source = CrLf(ReadFile(fixtures / "language-source.lua"));
			expected = ReadFile(fixtures / "language-source.expected");
			Require(source.find("\r\n") != std::string::npos, "Fixture did not preserve a CRLF request line");
			input = artifacts / "input.lua";
			WriteFile(input, source);
			WriteFile(artifacts / "expected-result.txt", expected);
			generation = server.Update(configuration, Document("file-source", input, source));
			observer.Ready(generation);
		});
		step("hover-and-signature-after-unicode", [&] {
			auto const hover_position = After(source, "object.co");
			auto hover_id = server.Request(LuaLanguageRequest::Hover, hover_position, hover_position, hover_position);
			Require(hover_id != 0, "Hover request was rejected");
			auto hover = observer.Response(LuaLanguageEvent::Kind::Hover, generation, hover_id);
			Require(hover.text.find("count") != std::string::npos, "Hover did not identify the table field after the Unicode/CRLF prefix");
			auto const signature_position = After(source, "local called = object:scale(");
			auto signature_id = server.Request(LuaLanguageRequest::Signature, signature_position, signature_position, signature_position);
			Require(signature_id != 0, "Signature request was rejected");
			auto signature = observer.Response(LuaLanguageEvent::Kind::Signature, generation, signature_id);
			Require(signature.text.find("scale(value") != std::string::npos, "Method signature is absent");
		});
		step("versioned-undefined-diagnostic", [&] {
			source = ReplaceOnce(source, "candidate + called", "candidate + called + missing_symbol_abc");
			generation = server.Update(configuration, Document("file-source", input, source, 2));
			observer.Ready(generation);
			auto diagnostic = observer.Wait([&](auto const& event) {
				return event.kind == LuaLanguageEvent::Kind::Diagnostics && event.generation == generation &&
					   std::ranges::any_of(event.diagnostics, [](auto const& item) { return item.message.find("missing_symbol_abc") != std::string::npos; });
			},
											15s, "versioned undefined-global diagnostic");
			auto const start = source.find("missing_symbol_abc");
			Require(std::ranges::any_of(diagnostic.diagnostics, [&](auto const& item) { return item.start == start && item.end == start + 18; }),
					"Undefined-global diagnostic has a wrong UTF-8 byte range");
		});
		step("unversioned-empty-is-unconfirmed", [&] {
			source = ReplaceOnce(source, " + missing_symbol_abc", "");
			generation = server.Update(configuration, Document("file-source", input, source, 3));
			observer.Ready(generation);
			auto invalidation = observer.Wait([&](auto const& event) {
				return event.kind == LuaLanguageEvent::Kind::Diagnostics && event.generation == generation &&
					   event.diagnostics.empty() && event.text.find("unconfirmed") != std::string::npos;
			},
											  15s, "unversioned empty diagnostic invalidation");
			Require(!invalidation.text.empty(), "Unversioned empty diagnostics were incorrectly presented as confirmed clean");
		});
		step("queued-nonidentifier-completion", [&] {
			source = ReplaceOnce(source, "object.count", "object.");
			generation = server.Update(configuration, Document("file-source", input, source, 4));
			auto const cursor = After(source, "object.");
			auto const request = server.Request(LuaLanguageRequest::Completion, cursor, cursor, cursor);
			Require(request != 0, "Immediate post-update completion was rejected");
			observer.Ready(generation);
			auto completion = observer.Response(LuaLanguageEvent::Kind::Completion, generation, request);
			auto item = std::ranges::find(completion.completions, "\"dot-key\"", &Automation4::LuaLanguageCompletion::label);
			Require(item != completion.completions.end(), "LuaLS did not offer the nonidentifier table key");
			Require(item->edits.size() == 2 && item->edits[0].start == cursor - 1 && item->edits[0].end == cursor && item->edits[0].text.empty() && item->edits[1].start == cursor && item->edits[1].end == cursor && item->edits[1].text == "[\"dot-key\"]",
					"Nonidentifier completion lost its textEdit or additionalTextEdit range");
			Require(item->caret == cursor - 1 + item->edits[1].text.size(), "Completion caret does not account for preceding additional edit");
			source = Apply(source, item->edits);
			WriteFile(artifacts / "completed-source.lua", source);
		});
		step("saved-source-independent-business", [&] {
			Require(Execute(source) == expected, "Applied real LuaLS completion changed the independent business result");
			WriteFile(artifacts / "output.lua", source);
			WriteFile(artifacts / "actual-result.txt", Execute(source));
		});
		step("file-to-virtual-quick-request-and-stale-isolation", [&] {
			generation = server.Update(configuration, Document("file-source", input, source, 5));
			observer.Ready(generation);
			auto old_request = server.Request(LuaLanguageRequest::Hover, After(source, "local called = object:sc"), 0, 0);
			Require(old_request != 0, "Old-file request was not accepted");
			observer.Pump();
			auto const before_switch = observer.history.size();
			auto virtual_document = Document("chunk/source", {}, source, 1);
			auto const virtual_generation = server.Update(configuration, virtual_document);
			auto const at = After(source, "local called = object:scale(");
			auto const virtual_request = server.Request(LuaLanguageRequest::Signature, at, at, at);
			Require(virtual_request != 0, "Immediate new-session request was rejected");
			observer.Ready(virtual_generation);
			auto response = observer.Response(LuaLanguageEvent::Kind::Signature, virtual_generation, virtual_request);
			Require(response.text.find("scale(value") != std::string::npos, "Quick virtual request lost the method signature");
			observer.Pump();
			for (std::size_t i = before_switch; i < observer.history.size(); ++i)
				Require(observer.history[i].generation != generation, "Stale file-generation event escaped after the virtual switch");
			generation = virtual_generation;
		});
		step("actual-karaoke-environment", [&] {
			auto environment = Automation4::BuildLuaLanguageEnvironment(Automation4::KaraokeOnce);
			Require(!environment.definitions.empty() && !environment.disabled_builtins.empty(), "Production karaoke language environment is empty");
			std::string karaoke = "local x = meta.res_x\nlocal y = _G.aegisub.text_extents\nlocal a = aegisub\nlocal b = table\nlocal c = tostring(1)\nreturn x\n";
			auto document = Document("karaoke/source", {}, karaoke, 1);
			document.definitions = environment.definitions;
			document.disabled_builtins = environment.disabled_builtins;
			generation = server.Update(configuration, std::move(document));
			observer.Ready(generation);
			observer.Snapshot("karaoke-ready");
			auto diagnostic = observer.Wait([&](auto const& event) {
				if (event.kind != LuaLanguageEvent::Kind::Diagnostics || event.generation != generation)
					return false;
				return std::ranges::any_of(event.diagnostics, [](auto const& item) { return item.message.find("aegisub") != std::string::npos; });
			},
											15s, "karaoke bare-global diagnostics");
			for (auto const& forbidden : {"aegisub", "table", "tostring"})
				Require(std::ranges::any_of(diagnostic.diagnostics, [&](auto const& item) { return item.message.find(forbidden) != std::string::npos; }),
						std::string("Karaoke environment unexpectedly exposes bare ") + forbidden);
			auto at = After(karaoke, "_G.aegisub.");
			auto request = server.Request(LuaLanguageRequest::Completion, at, at, at);
			auto completion = observer.Response(LuaLanguageEvent::Kind::Completion, generation, request);
			Require(std::ranges::any_of(completion.completions, [](auto const& item) { return item.label == "text_extents"; }),
					"Production _G.aegisub annotation did not expose text_extents");
			WriteFile(artifacts / "karaoke-source.lua", karaoke);
			WriteFile(artifacts / "karaoke-host.lua", environment.definitions);
		});
		step("ordinary-host-isolation", [&] {
			auto environment = Automation4::BuildLuaLanguageEnvironment(0);
			Require(environment.disabled_builtins.empty(), "Ordinary Lua unexpectedly disabled standard libraries");
			std::string ordinary = R"lua(local api = aegisub.text_extents
local a = meta
local b = line
local c = syl
aegisub.register_macro("Contract probe", "Host annotations", function(subs, selected, active)
	local remove = subs.delete
	subs.delete(1)
	return selected, active
end, function(subs, selected, active)
	return active == nil or subs.n > 0, "Current availability"
end)
return api
)lua";
			auto document = Document("ordinary/source", {}, ordinary);
			document.definitions = environment.definitions;
			generation = server.Update(configuration, std::move(document));
			observer.Ready(generation);
			auto diagnostic = observer.Wait([&](auto const& event) {
				return event.kind == LuaLanguageEvent::Kind::Diagnostics && event.generation == generation &&
					   std::ranges::any_of(event.diagnostics, [](auto const& item) { return item.message.find("Undefined global `meta`.") != std::string::npos; });
			},
											15s, "ordinary script exclusion of coding globals");
			auto undefined = [&](std::string_view symbol) {
				return std::ranges::any_of(diagnostic.diagnostics, [&](auto const& item) { return item.message == "Undefined global `" + std::string(symbol) + "`."; });
			};
			Require(undefined("meta") && undefined("line") && undefined("syl") && !undefined("aegisub"),
					"Ordinary Automation host leaked coding globals or lost aegisub");
			Require(std::ranges::none_of(diagnostic.diagnostics, [](auto const& item) { return item.message.find("expects a maximum") != std::string::npos; }),
					"register_macro callback aliases changed the outer function arity");
			auto at = After(ordinary, "aegisub.");
			auto request = server.Request(LuaLanguageRequest::Completion, at, at, at);
			auto completion = observer.Response(LuaLanguageEvent::Kind::Completion, generation, request);
			Require(std::ranges::any_of(completion.completions, [](auto const& item) { return item.label == "text_extents"; }),
					"Ordinary Automation host lost aegisub API completion");
			auto const macro_at = After(ordinary, "aegisub.register_macro(");
			auto const macro_request = server.Request(LuaLanguageRequest::Signature, macro_at, macro_at, macro_at);
			auto const macro_signature = observer.Response(LuaLanguageEvent::Kind::Signature, generation, macro_request);
			Require(macro_signature.text.find("run: fun(") != std::string::npos && macro_signature.text.find("):integer[]?, integer?") != std::string::npos &&
						macro_signature.text.find("validate?: fun(") != std::string::npos && macro_signature.text.find("active?: integer):boolean, string?") != std::string::npos &&
						macro_signature.text.find("isactive?: fun(subs: AegisubSubtitles, selected: integer[], active: integer):boolean") != std::string::npos,
					"register_macro signature lost its run, validation, or active callback contracts");
			auto const delete_at = After(ordinary, "subs.delete(");
			auto const delete_request = server.Request(LuaLanguageRequest::Signature, delete_at, delete_at, delete_at);
			auto const delete_signature = observer.Response(LuaLanguageEvent::Kind::Signature, generation, delete_request);
			Require(delete_signature.text.find("index?: integer|integer[]") != std::string::npos && delete_signature.text.find("...: integer") != std::string::npos,
					"Subtitles delete signature lost its empty, array, or integer-varargs forms");
			WriteFile(artifacts / "ordinary-host.lua", environment.definitions);
			WriteFile(artifacts / "ordinary-source.lua", ordinary);
		});
		std::string module_source;
		std::string module_document_source;
		std::uint64_t module_generation = 0;
		step("include-module-completion-and-signature", [&] {
			auto const include_directory = artifacts / "module-include";
			fs::create_directories(include_directory);
			module_source = R"lua(local module = { unique_module_field = 41 }
function module.unique_module_call(value)
	return value + module.unique_module_field
end
return module
)lua";
			WriteFile(include_directory / "workspace_contract.lua", module_source);
			module_document_source = R"lua(local unicode_prefix = "漢😀"
local external = require("workspace_contract")
local completion_probe = external.unique_module_field
local selected_probe = external.uniQUE
local called = external.unique_module_call(1)
return unicode_prefix, completion_probe, selected_probe, called
)lua";
			auto module_configuration = configuration;
			module_configuration.include_directories = {include_directory};
			auto const module_input = artifacts / "module-consumer.lua";
			WriteFile(module_input, module_document_source);
			auto document = Document("module/consumer", module_input, module_document_source);
			document.definitions = Automation4::BuildLuaLanguageEnvironment(0).definitions;
			module_generation = server.Update(module_configuration, std::move(document));
			observer.Ready(module_generation);
			auto const completion_at = After(module_document_source, "local completion_probe = external.");
			auto const completion_request = server.Request(LuaLanguageRequest::Completion, completion_at, completion_at, completion_at);
			Require(completion_request != 0, "Include-module completion request was rejected");
			auto const completion = observer.Response(LuaLanguageEvent::Kind::Completion, module_generation, completion_request);
			Require(std::ranges::any_of(completion.completions, [](auto const& item) { return item.label == "unique_module_field"; }),
					"Configured include module did not expose its unique field completion");
			auto const signature_at = After(module_document_source, "external.unique_module_call(");
			auto const signature_request = server.Request(LuaLanguageRequest::Signature, signature_at, signature_at, signature_at);
			Require(signature_request != 0, "Include-module signature request was rejected");
			auto const signature = observer.Response(LuaLanguageEvent::Kind::Signature, module_generation, signature_request);
			Require(signature.text.find("unique_module_call(value") != std::string::npos,
					"Configured include module did not expose its function signature");
			WriteFile(artifacts / "module-source.lua", module_source);
			WriteFile(artifacts / "module-consumer-source.lua", module_document_source);
		});
		step("selected-completion-replacement", [&] {
			auto const selection_start = module_document_source.find("uniQUE");
			Require(selection_start != std::string::npos, "Selection completion fixture has no mixed-case identifier");
			auto const selection_end = selection_start + std::string_view("uniQUE").size();
			auto const position = selection_start + std::string_view("uni").size();
			Require(module_document_source.substr(0, selection_start).find("漢😀") != std::string::npos,
					"Selection completion fixture lost its Unicode prefix");
			auto const request = server.Request(LuaLanguageRequest::Completion, position, selection_start, selection_end);
			Require(request != 0, "Nonempty-selection completion request was rejected");
			auto const completion = observer.Response(LuaLanguageEvent::Kind::Completion, module_generation, request);
			auto const item = std::ranges::find(completion.completions, "unique_module_field", &Automation4::LuaLanguageCompletion::label);
			Require(item != completion.completions.end(), "Nonempty selection lost the real include-module completion");
			Require(item->edits.size() == 1 && item->edits.front().start == selection_start && item->edits.front().end == selection_end &&
						item->edits.front().text == "unique_module_field" && item->caret == selection_start + item->edits.front().text.size(),
					"Real LuaLS completion did not replace exactly the requested nonempty selection");
			auto const applied = Apply(module_document_source, item->edits);
			Require(applied.substr(0, selection_start) == module_document_source.substr(0, selection_start),
					"Selected completion changed the Unicode-prefixed source before the selection");
			Require(applied.find("local selected_probe = external.unique_module_field") != std::string::npos,
					"Selected completion produced the wrong applied source");
			WriteFile(artifacts / "selected-completion-source.lua", applied);
		});
		step("karaoke-scope-matrix", [&] {
			struct ScopeCase {
				char const *name;
				unsigned scopes;
				bool line;
				bool syl;
				bool furi_syl;
			};
			for (auto const& scope : std::vector<ScopeCase>{
					 {.name = "once", .scopes = Automation4::KaraokeOnce, .line = false, .syl = false, .furi_syl = false},
					 {.name = "line", .scopes = Automation4::KaraokeLine, .line = true, .syl = false, .furi_syl = false},
					 {.name = "syl", .scopes = Automation4::KaraokeSyllable, .line = true, .syl = true, .furi_syl = false},
					 {.name = "furi", .scopes = Automation4::KaraokeFurigana, .line = true, .syl = true, .furi_syl = true},
					 {.name = "once-syl", .scopes = Automation4::KaraokeOnce | Automation4::KaraokeSyllable, .line = false, .syl = false, .furi_syl = false},
					 {.name = "line-syl", .scopes = Automation4::KaraokeLine | Automation4::KaraokeSyllable, .line = true, .syl = false, .furi_syl = false}}) {
				auto environment = Automation4::BuildLuaLanguageEnvironment(scope.scopes);
				std::string scoped = "local m = meta.res_x\nlocal l = line.text\nlocal kn = line.kara.n\nlocal fn = line.furi.n\nlocal s = syl.text\nlocal hn = syl.highlights.n\n";
				scoped += scope.furi_syl ? "local owner = syl.syl.text\n" : "local sn = syl.furi.n\n";
				scoped += "local parent = syl.line\nlocal f = furi\nreturn m\n";
				auto document = Document(std::string("scope/") + scope.name, {}, scoped);
				document.definitions = environment.definitions;
				document.disabled_builtins = environment.disabled_builtins;
				generation = server.Update(configuration, std::move(document));
				observer.Ready(generation);
				auto diagnostic = observer.Wait([&](auto const& event) {
					return event.kind == LuaLanguageEvent::Kind::Diagnostics && event.generation == generation &&
						   std::ranges::any_of(event.diagnostics, [](auto const& item) { return item.message == "Undefined global `furi`."; });
				},
												15s, std::string(scope.name) + " scope diagnostics");
				auto undefined = [&](std::string_view symbol) {
					return std::ranges::any_of(diagnostic.diagnostics, [&](auto const& item) { return item.message == "Undefined global `" + std::string(symbol) + "`."; });
				};
				Require(undefined("line") == !scope.line && undefined("syl") == !scope.syl && undefined("furi") && !undefined("meta"),
						std::string(scope.name) + " exposed the wrong karaoke globals");
				for (auto const& [symbol, allowed] : std::vector<std::pair<std::string_view, bool>>{{"line", scope.line}, {"syl", scope.syl}}) {
					if (!allowed)
						continue;
					auto at = After(scoped, std::string(symbol) + ".");
					auto request = server.Request(LuaLanguageRequest::Completion, at, at, at);
					auto completion = observer.Response(LuaLanguageEvent::Kind::Completion, generation, request);
					std::vector<std::string_view> expected_fields;
					if (symbol == "line")
						expected_fields = {"text", "start_time", "i", "descent", "extlead", "margin_v", "eff_margin_l", "eff_margin_r", "eff_margin_t", "eff_margin_b", "eff_margin_v", "halign", "valign", "hcenter", "vcenter", "kara", "furi"};
					else {
						expected_fields = {"text", "start_time", "line", "highlights", "prespacewidth", "postspacewidth"};
						expected_fields.emplace_back(scope.furi_syl ? "syl" : "furi");
					}
					for (auto const& field : expected_fields)
						Require(std::ranges::any_of(completion.completions, [&](auto const& item) { return item.label == field; }),
								std::string(scope.name) + " lacked " + std::string(symbol) + "." + std::string(field) + " completion");
				}
				if (scope.line) {
					auto at = After(scoped, "line.kara.");
					auto request = server.Request(LuaLanguageRequest::Completion, at, at, at);
					auto completion = observer.Response(LuaLanguageEvent::Kind::Completion, generation, request);
					Require(std::ranges::any_of(completion.completions, [](auto const& item) { return item.label == "n"; }),
							std::string(scope.name) + " lacked line.kara.n completion");
				}
				if (scope.syl) {
					std::vector<std::pair<std::string_view, std::string_view>> nested{{"syl.highlights.", "n"}};
					nested.emplace_back(scope.furi_syl ? "syl.syl." : "syl.furi.", scope.furi_syl ? "furi" : "n");
					for (auto const& [marker, field] : nested) {
						auto at = After(scoped, marker);
						auto request = server.Request(LuaLanguageRequest::Completion, at, at, at);
						auto completion = observer.Response(LuaLanguageEvent::Kind::Completion, generation, request);
						Require(std::ranges::any_of(completion.completions, [&](auto const& item) { return item.label == field; }),
								std::string(scope.name) + " lacked " + std::string(marker) + std::string(field) + " completion");
					}
				}
				WriteFile(artifacts / (std::string("scope-") + scope.name + "-host.lua"), environment.definitions);
				WriteFile(artifacts / (std::string("scope-") + scope.name + "-source.lua"), scoped);
			}
		});
		step("chunk-identity-local-isolation", [&] {
			std::string first = "local localOnlyA = 42\nlocal probe = loc\nreturn localOnlyA\n";
			generation = server.Update(configuration, Document("identity/first", {}, first));
			observer.Ready(generation);
			auto at = After(first, "local probe = loc");
			auto first_request = server.Request(LuaLanguageRequest::Completion, at, at, at);
			auto first_completion = observer.Response(LuaLanguageEvent::Kind::Completion, generation, first_request);
			Require(std::ranges::any_of(first_completion.completions, [](auto const& item) { return item.label == "localOnlyA"; }),
					"First virtual chunk did not offer its own local variable");
			std::string second = "local somethingElse = 42\nlocal probe = loc\nreturn somethingElse\n";
			generation = server.Update(configuration, Document("identity/second", {}, second));
			auto second_at = After(second, "local probe = loc");
			auto second_request = server.Request(LuaLanguageRequest::Completion, second_at, second_at, second_at);
			observer.Ready(generation);
			auto second_completion = observer.Response(LuaLanguageEvent::Kind::Completion, generation, second_request);
			Require(std::ranges::none_of(second_completion.completions, [](auto const& item) { return item.label == "localOnlyA"; }),
					"New virtual chunk inherited an unrelated local variable");
			WriteFile(artifacts / "identity-first.lua", first);
			WriteFile(artifacts / "identity-second.lua", second);
		});
		fs::path copied_executable;
		std::string switched_source;
		step("unicode-config-switch", [&] {
			auto const copied_release = artifacts / fs::path(L"LuaLS 漢字 release space");
			fs::copy(server_path, copied_release, fs::copy_options::recursive);
			for (auto const& required : {"bin/lua-language-server.exe", "main.lua", "script", "meta"})
				Require(fs::exists(copied_release / required), "Copied LuaLS release lacks " + std::string(required));
			copied_executable = fs::absolute(copied_release / "bin/lua-language-server.exe");
			configuration.directory = fs::absolute(copied_release);
			switched_source = ReplaceOnce(source, "local result", "local addon = object.count\r\nlocal result");
			generation = server.Update(configuration, Document("copied/source", {}, switched_source));
			observer.Ready(generation);
#ifdef _WIN32
			Require(OwnedServerChildren(copied_executable).size() == 1, "Unicode-path configuration did not launch one verified real LuaLS child");
#endif
			auto const at = After(switched_source, "local addon = object.co");
			auto const request = server.Request(LuaLanguageRequest::Hover, at, at, at);
			Require(request != 0, "Copied-release hover request was rejected");
			auto const hover = observer.Response(LuaLanguageEvent::Kind::Hover, generation, request);
			Require(hover.text.find("count") != std::string::npos, "Unicode-path LuaLS lost table field hover after source edit");
			WriteFile(artifacts / "copied-release-source.lua", switched_source);
			WriteFile(artifacts / "copied-release-location.txt", agi::fs::PathToGenericString(copied_release));
		});
		step("null-document-disable-and-reopen", [&] {
			auto const old_generation = generation;
			auto const at = After(switched_source, "local addon = object.co");
			auto const old_request = server.Request(LuaLanguageRequest::Hover, at, at, at);
			Require(old_request != 0, "Pre-disable hover request was rejected");
			observer.Pump();
			auto const before_disable = observer.history.size();
			generation = server.Update(configuration, std::nullopt);
			auto const inactive = observer.Wait([&](auto const& event) {
				return event.kind == LuaLanguageEvent::Kind::Status && event.generation == generation && event.text == "LuaLS: inactive";
			},
												12s, "null-document inactive status");
			Require(!inactive.ready && generation > old_generation, "Null document did not disable the language session");
			Require(server.Request(LuaLanguageRequest::Completion, at, at, at) == 0, "Inactive document accepted a completion request");
#ifdef _WIN32
			Require(OwnedServerChildren(copied_executable).empty(), "Null document retained the verified LuaLS child");
#endif
			generation = server.Update(configuration, Document("copied/source", {}, switched_source, 2));
			observer.Ready(generation);
			auto const completion_at = After(switched_source, "local addon = object.");
			auto const request = server.Request(LuaLanguageRequest::Completion, completion_at, completion_at, completion_at);
			Require(request != 0, "Reopened document completion was rejected");
			auto const completion = observer.Response(LuaLanguageEvent::Kind::Completion, generation, request);
			Require(std::ranges::any_of(completion.completions, [](auto const& item) { return item.label == "count"; }),
					"Reopened real LuaLS lost table-field completion");
			observer.Pump();
			for (std::size_t i = before_disable; i < observer.history.size(); ++i)
				Require(observer.history[i].generation != old_generation, "Queued pre-disable result escaped into the reopened session");
		});
		step("owned-server-termination-and-recovery", [&] {
#ifdef _WIN32
			TerminateOwnedServer(copied_executable);
			auto const failed = observer.Wait([&](auto const& event) {
				return event.kind == LuaLanguageEvent::Kind::Status && event.generation == generation &&
					   event.text.find("unavailable") != std::string::npos;
			},
											  12s, "owned LuaLS child termination status");
			Require(!failed.ready, "Terminated LuaLS child remained ready");
			auto const changed_source = ReplaceOnce(switched_source, "local result", "local encore = 1\r\nlocal result");
			generation = server.Update(configuration, Document("copied/source", {}, changed_source, 3));
			auto const retained_failure = observer.Wait([&](auto const& event) {
				return event.kind == LuaLanguageEvent::Kind::Status && event.generation == generation &&
					   event.text.find("unavailable") != std::string::npos;
			},
														8s, "failed LuaLS state after source edit");
			Require(!retained_failure.ready && OwnedServerChildren(copied_executable).empty(),
					"Source edit silently restarted the failed LuaLS session");
			WriteFile(artifacts / "after-terminated-server-edit.lua", changed_source);
			generation = server.Update(configuration, std::nullopt);
			observer.Wait([&](auto const& event) {
				return event.kind == LuaLanguageEvent::Kind::Status && event.generation == generation && event.text == "LuaLS: inactive";
			},
						  8s, "post-failure explicit close");
			generation = server.Update(configuration, Document("copied/source", {}, changed_source, 4));
			observer.Ready(generation);
			Require(OwnedServerChildren(copied_executable).size() == 1, "Explicit reopen did not launch one verified real LuaLS child");
			auto const at = After(changed_source, "local addon = object.");
			auto const request = server.Request(LuaLanguageRequest::Completion, at, at, at);
			Require(request != 0, "Explicitly reopened session rejected completion");
			auto const completion = observer.Response(LuaLanguageEvent::Kind::Completion, generation, request);
			Require(std::ranges::any_of(completion.completions, [](auto const& item) { return item.label == "count"; }),
					"Explicit reopen did not recover real LuaLS completion");
#else
			throw std::runtime_error("Owned LuaLS process lifecycle requires Windows");
#endif
		});
		step("missing-and-incomplete-server-no-fallback", [&] {
			for (auto const& [name, directory] : std::vector<std::pair<std::string, fs::path>>{
					 {"missing", artifacts / "missing-release"}, {"incomplete", artifacts / "incomplete-release"}}) {
				if (name == "incomplete") {
					fs::create_directories(directory);
					WriteFile(directory / "main.lua", "return true\n");
				}
				LuaLanguageServer broken;
				fs::create_directories(artifacts / (name + "-events"));
				Observer observation(broken, deadline, artifacts / (name + "-events"));
				LuaLanguageConfiguration missing_config{.directory = directory, .cache_directory = artifacts / (name + "-cache")};
				fs::create_directories(missing_config.cache_directory);
				auto bad_generation = broken.Update(missing_config, Document(name, {}, "return 'unavailable'"));
				auto request = broken.Request(LuaLanguageRequest::Completion, 0, 0, 0);
				auto status = observation.Wait([&](auto const& event) { return event.kind == LuaLanguageEvent::Kind::Status && event.generation == bad_generation && event.text.find("unavailable") != std::string::npos; },
											   8s, name + " server unavailable status");
				Require(!status.ready && request != 0, "Missing server incorrectly reported ready or rejected the guarded request");
				observation.Pump();
				Require(std::ranges::none_of(observation.history, [&](auto const& event) { return event.kind == LuaLanguageEvent::Kind::Completion && event.request == request; }),
						"Missing server returned fallback completion");
			}
		});
#ifdef _WIN32
		generation = server.Update(configuration, std::nullopt);
		observer.Wait([&](auto const& event) {
			return event.kind == LuaLanguageEvent::Kind::Status && event.generation == generation && event.text == "LuaLS: inactive";
		},
					  5s, "real LuaLS close before isolated timeout fixtures");
		WaitOwnedChildExit(copied_executable, deadline);
		auto timeout_case = [&](std::string_view mode) {
			auto const directory = artifacts / std::string(mode);
			auto const release = directory / "release";
			fs::create_directories(release / "bin");
			fs::create_directories(release / "script");
			fs::create_directories(release / "meta");
			auto const executable = fs::absolute(argv[0]);
			fs::copy_file(executable, release / "bin/lua-language-server.exe");
			Require(fs::is_regular_file(executable.parent_path() / "lua51.dll"), "Native E2E LuaJIT runtime dependency is missing");
			std::vector<fs::path> dependencies;
			for (auto const& entry : fs::directory_iterator(executable.parent_path()))
				if (entry.is_regular_file() && entry.path().extension() == ".dll")
					dependencies.push_back(entry.path());
			std::ranges::sort(dependencies, {}, [](auto const& path) { return path.filename(); });
			std::string copied;
			for (auto const& dependency : dependencies) {
				fs::copy_file(dependency, release / "bin" / dependency.filename());
				copied += agi::fs::PathToGenericString(dependency.filename()) + '\n';
			}
			Require(!copied.empty(), "Native E2E runtime DLL list is empty");
			WriteFile(directory / "copied-runtime-dlls.txt", copied);
			WriteFile(release / "main.lua", mode);
			auto const fake_executable = fs::absolute(release / "bin/lua-language-server.exe");
			auto const input_file = directory / "timeout-source.lua";
			WriteFile(input_file, switched_source);
			fs::create_directories(directory / "cache");
			LuaLanguageServer timeout_server;
			Observer timeout_observer(timeout_server, deadline, directory);
			LuaLanguageConfiguration fake_configuration{.directory = release, .cache_directory = directory / "cache"};
			auto observed_generation = timeout_server.Update(fake_configuration, Document("timeout/source", input_file, switched_source));
			if (mode == "request-silence") {
				timeout_observer.Ready(observed_generation);
				Require(OwnedServerChildren(fake_executable).size() == 1, "Ready fake request server is not the verified E2E child");
				auto const signature_at = After(switched_source, "local called = object:scale(");
				auto const signature_request = timeout_server.Request(LuaLanguageRequest::Signature, signature_at, signature_at, signature_at);
				Require(signature_request != 0, "Controlled signature-error request was rejected");
				auto const error = timeout_observer.Wait([&](auto const& event) {
					return event.kind == LuaLanguageEvent::Kind::Status && event.generation == observed_generation && event.ready &&
						   event.text.find("controlled signature failure") != std::string::npos;
				},
														 4s, "controlled signature request-level error status");
				Require(error.ready, "Request-level error changed the ready session into a failed session");
				auto const signature = timeout_observer.Response(LuaLanguageEvent::Kind::Signature, observed_generation, signature_request);
				Require(signature.text.empty(), "Request-level signature error returned fabricated language content");
				auto const at = After(switched_source, "local addon = object.");
				auto const request = timeout_server.Request(LuaLanguageRequest::Completion, at, at, at);
				Require(request != 0, "Silent completion request was rejected before timeout");
			}
			auto const begun = Clock::now();
			auto const expected_error = mode == "startup-silence" ? "initialization timed out" : "request timed out";
			auto const failed = timeout_observer.Wait([&](auto const& event) {
				return event.kind == LuaLanguageEvent::Kind::Status && event.generation == observed_generation &&
					   event.text.find(expected_error) != std::string::npos;
			},
													  15s, std::string(mode) + " bounded unavailable status");
			auto const elapsed = Clock::now() - begun;
			Require(!failed.ready && elapsed <= (mode == "startup-silence" ? 14s : 9s), "Silent server did not degrade within its timeout bound");
			WaitOwnedChildExit(fake_executable, deadline);
			auto const observed = ReadFile(release / "observed.ndjson");
			Require(observed.find("\"initialize\"") != std::string::npos, "Silent server did not receive initialize");
			if (mode == "request-silence")
				Require(observed.find("\"textDocument/completion\"") != std::string::npos, "Silent server did not receive completion request");
			auto const changed = ReplaceOnce(switched_source, "local result", "local timeout_edit = 5\r\nlocal result");
			observed_generation = timeout_server.Update(fake_configuration, Document("timeout/source", input_file, changed, 2));
			auto const retained = timeout_observer.Wait([&](auto const& event) {
				return event.kind == LuaLanguageEvent::Kind::Status && event.generation == observed_generation &&
					   event.text.find(expected_error) != std::string::npos;
			},
														3s, std::string(mode) + " retained failure after edit");
			Require(!retained.ready && OwnedServerChildren(fake_executable).empty(), "Editing silently restarted a timed-out server");
			WriteFile(directory / "after-timeout-edit.lua", changed);
			observed_generation = timeout_server.Update(fake_configuration, std::nullopt);
			timeout_observer.Wait([&](auto const& event) {
				return event.kind == LuaLanguageEvent::Kind::Status && event.generation == observed_generation && event.text == "LuaLS: inactive";
			},
								  3s, std::string(mode) + " explicit close");
			LuaLanguageConfiguration recovered{.directory = server_path, .cache_directory = directory / "cache"};
			observed_generation = timeout_server.Update(recovered, Document("timeout/source", input_file, changed, 3));
			timeout_observer.Ready(observed_generation);
			auto const at = After(changed, "local addon = object.");
			auto const request = timeout_server.Request(LuaLanguageRequest::Completion, at, at, at);
			Require(request != 0, "Explicit real-server recovery rejected completion");
			auto const completion = timeout_observer.Response(LuaLanguageEvent::Kind::Completion, observed_generation, request);
			Require(std::ranges::any_of(completion.completions, [](auto const& item) { return item.label == "count"; }),
					"Explicit close/reopen did not recover real LuaLS completion");
			observed_generation = timeout_server.Update(recovered, std::nullopt);
			timeout_observer.Wait([&](auto const& event) {
				return event.kind == LuaLanguageEvent::Kind::Status && event.generation == observed_generation && event.text == "LuaLS: inactive";
			},
								  5s, std::string(mode) + " recovered real LuaLS close");
			WaitOwnedChildExit(fs::absolute(server_path / "bin/lua-language-server.exe"), deadline);
			WriteFile(directory / "elapsed-ms.txt", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
		};
		step("startup-timeout", [&] { timeout_case("startup-silence"); });
		step("request-timeout", [&] { timeout_case("request-silence"); });
#endif
		observer.Pump();
		WriteFile(artifacts / "manifest.json", Manifest(steps));
		return 0;
	}
	catch (std::exception const& error) {
		std::cerr << error.what() << '\n';
		if (!artifacts.empty() && fs::exists(artifacts)) {
			if (!current_step.empty())
				steps.emplace_back(current_step, "failed");
			WriteFile(artifacts / "manifest.json", Manifest(steps, error.what()));
		}
		return 1;
	}
}
