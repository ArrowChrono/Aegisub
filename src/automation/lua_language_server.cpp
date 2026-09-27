#include "lua_language_server.h"

#include "lua_language_server_process.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/fs.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Automation4 {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::size_t message_limit = 8 * 1024 * 1024;

json::UnknownElement Parse(std::string const& text) {
	json::UnknownElement value;
	std::istringstream stream(text);
	json::Reader::Read(value, stream);
	return value;
}

std::string Encode(json::UnknownElement const& value) {
	std::ostringstream stream;
	agi::JsonWriter::Write(value, stream);
	return stream.str();
}

template <class T>
T const *Find(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end())
		return nullptr;
	try {
		return &static_cast<T const&>(it->second);
	}
	catch (json::Exception const&) {
		return nullptr;
	}
}

std::string String(json::Object const& object, std::string const& key) {
	auto value = Find<json::String>(object, key);
	return value ? *value : std::string{};
}

json::Integer Integer(json::Object const& object, std::string const& key, json::Integer fallback = 0) {
	auto value = Find<json::Integer>(object, key);
	return value ? *value : fallback;
}

std::string PercentEncode(std::string_view value, bool path) {
	constexpr char hex[] = "0123456789ABCDEF";
	std::string result;
	for (unsigned char c : value) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' || (path && c == '/'))
			result += static_cast<char>(c);
		else {
			result += '%';
			result += hex[c >> 4];
			result += hex[c & 15];
		}
	}
	return result;
}

std::string FileUri(agi::fs::path const& path) {
	auto text = agi::fs::PathToGenericString(std::filesystem::absolute(path).lexically_normal());
	if (text.size() > 1 && text[1] == ':' && text[0] >= 'A' && text[0] <= 'Z')
		text[0] += 'a' - 'A';
	if (text.starts_with("//"))
		return "file:" + PercentEncode(text, true);
	return (text.starts_with('/') ? "file://" : "file:///") + PercentEncode(text, true);
}

std::size_t CharacterSize(std::string_view text, std::size_t offset) {
	unsigned char c = text[offset];
	std::size_t length = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2
									: (c & 0xf0) == 0xe0   ? 3
									: (c & 0xf8) == 0xf0   ? 4
														   : 0;
	if (!length || offset + length > text.size())
		throw std::runtime_error("Invalid UTF-8 source position");
	for (std::size_t i = 1; i < length; ++i)
		if ((static_cast<unsigned char>(text[offset + i]) & 0xc0) != 0x80)
			throw std::runtime_error("Invalid UTF-8 source position");
	return length;
}

json::Object Position(std::string_view source, std::size_t offset) {
	if (offset > source.size())
		throw std::runtime_error("Position is outside the current source");
	json::Integer line = 0, character = 0;
	std::size_t i = 0;
	while (i < offset) {
		if (source[i] == '\r' || source[i] == '\n') {
			if (source[i] == '\r' && i + 1 < source.size() && source[i + 1] == '\n')
				++i;
			++line;
			character = 0;
			++i;
		}
		else {
			auto length = CharacterSize(source, i);
			i += length;
			character += length == 4 ? 2
									 : 1;
		}
	}
	if (i != offset)
		throw std::runtime_error("Position splits a source character");
	json::Object result;
	result["line"] = line;
	result["character"] = character;
	return result;
}

std::size_t Offset(std::string_view source, json::Object const& position) {
	auto line = Integer(position, "line", -1), character = Integer(position, "character", -1);
	if (line < 0 || character < 0)
		throw std::runtime_error("Invalid language-server position");
	std::size_t i = 0;
	while (line > 0 && i < source.size()) {
		if (source[i] == '\r' || source[i] == '\n') {
			if (source[i] == '\r' && i + 1 < source.size() && source[i + 1] == '\n')
				++i;
			--line;
		}
		++i;
	}
	while (character > 0 && i < source.size() && source[i] != '\r' && source[i] != '\n') {
		auto length = CharacterSize(source, i);
		character -= length == 4 ? 2
								 : 1;
		i += length;
	}
	if (line != 0 || character != 0)
		throw std::runtime_error("Language-server position is outside the source");
	return i;
}

std::string Documentation(json::UnknownElement const& value) {
	try {
		return static_cast<json::String const&>(value);
	}
	catch (json::Exception const&) {
	}
	try {
		auto const& object = static_cast<json::Object const&>(value);
		return String(object, "value");
	}
	catch (json::Exception const&) {
	}
	try {
		std::string text;
		for (auto const& item : static_cast<json::Array const&>(value)) {
			if (!text.empty())
				text += '\n';
			text += Documentation(item);
		}
		return text;
	}
	catch (json::Exception const&) {
	}
	return {};
}

void WriteFile(agi::fs::path const& path, std::string const& text) {
	std::ofstream file(path, std::ios::binary);
	file.write(text.data(), static_cast<std::streamsize>(text.size()));
	if (!file)
		throw std::runtime_error("Cannot write the LuaLS session configuration");
}

}

struct LuaLanguageServer::Impl {
	struct Snapshot {
		LuaLanguageConfiguration configuration;
		std::optional<LuaLanguageDocument> document;
		std::uint64_t generation = 0;
	};
	struct RequestData {
		LuaLanguageRequest kind;
		std::uint64_t generation;
		std::uint64_t id;
		std::size_t position;
		std::size_t selection_start;
		std::size_t selection_end;
	};
	struct Pending {
		RequestData request;
		Clock::time_point deadline;
	};
	std::mutex mutex;
	std::condition_variable changed;
	bool stopping = false;
	Snapshot desired;
	std::uint64_t next_request = 0;
	std::vector<RequestData> requests;
	std::vector<LuaLanguageEvent> events;
	std::thread worker;
	LuaLanguageServerProcess process;
	Snapshot current;
	bool attempted = false;
	bool initialized = false;
	bool workspace_ready = false;
	std::string server_name;
	std::string last_status;
	std::string uri;
	std::string source;
	std::string incoming;
	std::string settings;
	agi::fs::path session_directory;
	std::map<json::Integer, Pending> pending;
	json::Integer next_id = 0;
	json::Integer initialize_id = 0;
	json::Integer ready_id = 0;
	json::Integer version = 0;
	Clock::time_point initialize_deadline;

	Impl() {
		worker = std::thread([this] { Run(); });
	}
	~Impl() {
		{
			std::scoped_lock lock(mutex);
			stopping = true;
		}
		changed.notify_one();
		worker.join();
		if (!session_directory.empty()) {
			std::error_code error;
			std::filesystem::remove_all(session_directory, error);
		}
	}

	void Emit(LuaLanguageEvent event) {
		std::scoped_lock lock(mutex);
		if (event.generation != desired.generation || stopping)
			return;
		if (events.size() == 32)
			events.erase(events.begin());
		events.push_back(std::move(event));
	}

	void Status(std::string text, bool ready = false) {
		last_status = text;
		Emit({.kind = LuaLanguageEvent::Kind::Status, .generation = current.generation, .text = std::move(text), .ready = ready});
	}

	void Send(json::Object message) {
		message["jsonrpc"] = "2.0";
		auto bytes = Encode(std::move(message));
		if (bytes.size() > message_limit)
			throw std::runtime_error("LuaLS message exceeds the 8 MiB limit");
		std::string error;
		if (!process.Write("Content-Length: " + std::to_string(bytes.size()) + "\r\n\r\n" + bytes, 500ms, error))
			throw std::runtime_error(error);
	}

	void Notify(std::string method, json::Object params) {
		json::Object message;
		message["method"] = std::move(method);
		message["params"] = std::move(params);
		Send(std::move(message));
	}

	json::Integer SendRequest(std::string method, json::Object params) {
		json::Object message;
		auto id = ++next_id;
		message["id"] = id;
		message["method"] = std::move(method);
		message["params"] = std::move(params);
		Send(std::move(message));
		return id;
	}

	[[nodiscard]] json::Object DocumentId() const {
		json::Object doc;
		doc["uri"] = uri;
		return doc;
	}

	void Stop() {
		if (initialized) {
			try {
				json::Object params;
				params["textDocument"] = DocumentId();
				Notify("textDocument/didClose", std::move(params));
				auto id = SendRequest("shutdown", {});
				auto deadline = Clock::now() + 300ms;
				while (Clock::now() < deadline) {
					auto response = Receive();
					if (response && Integer(*response, "id", -1) == id)
						break;
				}
				Notify("exit", {});
			}
			catch (std::exception const&) {
			}
		}
		process.Stop();
		initialized = false;
		workspace_ready = false;
		initialize_id = 0;
		ready_id = 0;
		incoming.clear();
		pending.clear();
		version = 0;
	}

	[[nodiscard]] bool SameSession(Snapshot const& other) const {
		if (current.configuration != other.configuration || current.document.has_value() != other.document.has_value())
			return false;
		if (!current.document)
			return true;
		auto const& a = *current.document;
		auto const& b = *other.document;
		return a.identity == b.identity && a.filename == b.filename && a.definitions == b.definitions && a.disabled_builtins == b.disabled_builtins;
	}

	void Start() {
		auto const& config = current.configuration;
		auto directory = std::filesystem::absolute(config.directory).lexically_normal();
#ifdef _WIN32
		auto executable = directory / "bin/lua-language-server.exe";
#else
		auto executable = directory / "bin/lua-language-server";
#endif
		if (!std::filesystem::is_regular_file(executable) || !std::filesystem::is_regular_file(config.directory / "main.lua") || !std::filesystem::is_directory(config.directory / "script") || !std::filesystem::is_directory(config.directory / "meta"))
			throw std::runtime_error("LuaLS directory is missing a complete release (bin, main.lua, script and meta)");
		if (session_directory.empty())
			session_directory = agi::fs::UniquePath(std::filesystem::absolute(config.cache_directory) / "session-%%%%%%%%");
		std::filesystem::create_directories(session_directory / "library");
		std::filesystem::create_directories(session_directory / "log");
		std::filesystem::create_directories(session_directory / "meta");
		WriteFile(session_directory / "library/host.lua", current.document->definitions);
		auto settings_value = Parse(R"({"runtime":{"version":"LuaJIT","plugin":""},"workspace":{"checkThirdParty":false,"userThirdParty":[],"maxPreload":1000},"completion":{"callSnippet":"Disable","keywordSnippet":"Disable","autoRequire":false},"diagnostics":{"enableScheme":["file","aegisub"]},"telemetry":{"enable":false}})");
		json::Array libraries;
		libraries.emplace_back(agi::fs::PathToGenericString(session_directory / "library"));
		json::Array paths;
		paths.emplace_back("?.lua");
		paths.emplace_back("?/init.lua");
		auto includes = config.include_directories;
		if (!current.document->filename.empty()) {
			for (auto parent = current.document->filename.parent_path(); !parent.empty();) {
				if (std::filesystem::is_directory(parent / "include")) {
					includes.insert(includes.begin(), parent / "include");
					break;
				}
				auto next = parent.parent_path();
				if (next == parent)
					break;
				parent = std::move(next);
			}
		}
		for (auto const& include : includes) {
			auto path = std::filesystem::absolute(include).lexically_normal();
			if (!std::filesystem::is_directory(path))
				continue;
			libraries.emplace_back(agi::fs::PathToGenericString(path));
			paths.emplace_back(agi::fs::PathToGenericString(path / "?.lua"));
			paths.emplace_back(agi::fs::PathToGenericString(path / "?/init.lua"));
		}
		auto& settings_object = static_cast<json::Object&>(settings_value);
		static_cast<json::Object&>(settings_object.at("workspace"))["library"] = std::move(libraries);
		static_cast<json::Object&>(settings_object.at("runtime"))["path"] = std::move(paths);
		json::Object builtin;
		for (auto const& name : current.document->disabled_builtins)
			builtin[name] = "disable";
		static_cast<json::Object&>(settings_object.at("runtime"))["builtin"] = std::move(builtin);
		settings = Encode(settings_value);
		json::Object override_config;
		override_config["Lua"] = std::move(settings_value);
		WriteFile(session_directory / "config.json", Encode(std::move(override_config)));
		std::string error;
		std::vector<std::string> arguments{
			"--logpath=" + agi::fs::PathToString(session_directory / "log"),
			"--metapath=" + agi::fs::PathToString(session_directory / "meta"),
			"--configpath=" + agi::fs::PathToString(session_directory / "config.json")};
		if (!process.Start(executable, arguments, directory, session_directory / "stderr.log", error))
			throw std::runtime_error(error);
		json::Object params;
		params["processId"] = json::Null{};
		json::Object info;
		info["name"] = "Aegisub Lua Workspace";
		params["clientInfo"] = std::move(info);
		params["rootUri"] = current.document->filename.empty() ? json::UnknownElement(json::Null{}) : json::UnknownElement(FileUri(current.document->filename.parent_path()));
		params["capabilities"] = Parse(R"({"offsetEncoding":["utf-16"],"general":{"positionEncodings":["utf-16"]},"workspace":{"configuration":true},"textDocument":{"completion":{"dynamicRegistration":false,"completionItem":{"snippetSupport":false,"insertReplaceSupport":false}},"hover":{"contentFormat":["plaintext"]},"signatureHelp":{"signatureInformation":{"documentationFormat":["plaintext"]}},"publishDiagnostics":{"versionSupport":true}}})");
		uri = current.document->filename.empty() ? "aegisub://workspace/" + PercentEncode(current.document->identity, false) + ".lua" : FileUri(current.document->filename);
		initialize_id = SendRequest("initialize", std::move(params));
		initialize_deadline = Clock::now() + 10s;
		Status("LuaLS: starting...");
	}

	void SyncDocument(bool reopen = false) {
		source = current.document->source;
		json::Object params;
		auto doc = DocumentId();
		doc["version"] = ++version;
		if (version == 1 || reopen) {
			doc["languageId"] = "lua";
			doc["text"] = source;
			params["textDocument"] = std::move(doc);
			Notify("textDocument/didOpen", std::move(params));
		}
		else {
			params["textDocument"] = std::move(doc);
			json::Object change;
			change["text"] = source;
			json::Array changes;
			changes.emplace_back(std::move(change));
			params["contentChanges"] = std::move(changes);
			Notify("textDocument/didChange", std::move(params));
		}
		for (auto const& [id, request] : pending) {
			json::Object cancel;
			cancel["id"] = id;
			Notify("$/cancelRequest", std::move(cancel));
		}
		pending.clear();
		if (workspace_ready)
			Status(server_name + ": ready", true);
		else {
			if (ready_id) {
				json::Object cancel;
				cancel["id"] = ready_id;
				Notify("$/cancelRequest", std::move(cancel));
			}
			json::Object ready_params;
			ready_params["textDocument"] = DocumentId();
			ready_id = SendRequest("textDocument/documentSymbol", std::move(ready_params));
			Status(server_name + ": indexing...");
		}
	}

	std::optional<json::Object> Receive() {
		auto header = incoming.find("\r\n\r\n");
		if (header == std::string::npos || incoming.size() < header + 4) {
			if (incoming.size() > 8192)
				throw std::runtime_error("Invalid LuaLS message header");
		}
		else {
			std::optional<std::size_t> length;
			std::size_t start = 0;
			while (start < header) {
				auto end = incoming.find("\r\n", start);
				auto line = std::string_view(incoming).substr(start, end - start);
				if (line.starts_with("Content-Length:")) {
					if (length)
						throw std::runtime_error("Duplicate LuaLS content length");
					line.remove_prefix(15);
					while (line.starts_with(' '))
						line.remove_prefix(1);
					std::size_t size = 0;
					auto parsed = std::from_chars(line.data(), line.data() + line.size(), size);
					if (parsed.ec != std::errc{} || parsed.ptr != line.data() + line.size() || size > message_limit)
						throw std::runtime_error("Invalid LuaLS content length");
					length = size;
				}
				start = end + 2;
			}
			if (!length)
				throw std::runtime_error("Missing LuaLS content length");
			if (incoming.size() >= header + 4 + *length) {
				auto value = Parse(incoming.substr(header + 4, *length));
				incoming.erase(0, header + 4 + *length);
				return std::move(static_cast<json::Object&>(value));
			}
		}
		std::string data, error;
		auto state = process.Read(data, 20ms, error);
		if (state == LuaLanguageServerProcess::ReadState::Closed)
			throw std::runtime_error("LuaLS process exited");
		if (state == LuaLanguageServerProcess::ReadState::Error)
			throw std::runtime_error(error);
		incoming += data;
		if (incoming.size() > message_limit + 8192)
			throw std::runtime_error("LuaLS receive buffer exceeded its limit");
		return std::nullopt;
	}

	[[nodiscard]] LuaLanguageEdit ReadEdit(json::Object const& object) const {
		auto range = Find<json::Object>(object, "range");
		if (!range)
			throw std::runtime_error("Unsupported LuaLS edit range");
		auto start = Find<json::Object>(*range, "start"), end = Find<json::Object>(*range, "end");
		if (!start || !end)
			throw std::runtime_error("Missing LuaLS edit endpoints");
		auto a = Offset(source, *start), b = Offset(source, *end);
		if (b < a)
			throw std::runtime_error("LuaLS edit is outside the editable source");
		return {.start = a, .end = b, .text = String(object, "newText")};
	}

	void Diagnostics(json::Object const& params) {
		if (String(params, "uri") != uri)
			return;
		auto list = Find<json::Array>(params, "diagnostics");
		if (!list)
			return;
		LuaLanguageEvent event{.kind = LuaLanguageEvent::Kind::Diagnostics, .generation = current.generation};
		if (!params.contains("version") && list->empty()) {
			event.text = "LuaLS cleared diagnostics without a document version; the current diagnostic state is unconfirmed.";
			Emit(std::move(event));
			return;
		}
		if (Integer(params, "version", -1) != version)
			return;
		for (auto const& item : *list) {
			try {
				auto const& object = static_cast<json::Object const&>(item);
				auto edit = ReadEdit(object);
				event.diagnostics.push_back({.start = edit.start, .end = edit.end, .severity = static_cast<int>(Integer(object, "severity", 1)), .message = String(object, "message")});
			}
			catch (std::exception const&) {
			}
		}
		Emit(std::move(event));
	}

	void Reply(json::Object& message) {
		auto method = String(message, "method");
		json::Object response;
		response["id"] = std::move(message.at("id"));
		if (method == "workspace/configuration") {
			json::Array values;
			auto const& params = static_cast<json::Object const&>(message.at("params"));
			auto const& items = static_cast<json::Array const&>(params.at("items"));
			for (auto const& item : items) {
				auto section = String(static_cast<json::Object const&>(item), "section");
				values.emplace_back(section == "Lua" ? Parse(settings) : json::UnknownElement(json::Null{}));
			}
			response["result"] = std::move(values);
		}
		else if (method == "window/workDoneProgress/create")
			response["result"] = json::Null{};
		else if (method == "workspace/applyEdit") {
			json::Object result;
			result["applied"] = false;
			result["failureReason"] = "Workspace edits are not supported";
			response["result"] = std::move(result);
		}
		else {
			json::Object error;
			error["code"] = -32601;
			error["message"] = "Method is not supported by Lua Workspace";
			response["error"] = std::move(error);
		}
		Send(std::move(response));
	}

	void Completion(json::UnknownElement const& result, Pending const& request) {
		LuaLanguageEvent event{.kind = LuaLanguageEvent::Kind::Completion, .generation = request.request.generation, .request = request.request.id, .position = request.request.position};
		json::Array const *items = nullptr;
		try {
			items = &static_cast<json::Array const&>(result);
		}
		catch (json::Exception const&) {
			try {
				items = Find<json::Array>(static_cast<json::Object const&>(result), "items");
			}
			catch (json::Exception const&) {
			}
		}
		if (items)
			for (auto const& value : *items) {
				try {
					auto const& item = static_cast<json::Object const&>(value);
					if (Integer(item, "insertTextFormat", 1) != 1)
						continue;
					LuaLanguageCompletion completion;
					completion.label = String(item, "label");
					completion.detail = String(item, "detail");
					if (completion.label.empty() || completion.label.find_first_of("\r\n\t") != std::string::npos)
						continue;
					if (auto edit = Find<json::Object>(item, "textEdit"))
						completion.edits.push_back(ReadEdit(*edit));
					else {
						auto text = String(item, "insertText");
						completion.edits.push_back({.start = request.request.selection_start, .end = request.request.selection_end, .text = text.empty() ? completion.label : std::move(text)});
					}
					if (auto additional = Find<json::Array>(item, "additionalTextEdits"))
						for (auto const& edit : *additional)
							completion.edits.push_back(ReadEdit(static_cast<json::Object const&>(edit)));
					auto const& primary = completion.edits.front();
					completion.caret = primary.start + primary.text.size();
					for (std::size_t i = 1; i < completion.edits.size(); ++i) {
						auto const& edit = completion.edits[i];
						if (edit.end <= primary.start)
							completion.caret = completion.caret - (edit.end - edit.start) + edit.text.size();
					}
					std::ranges::sort(completion.edits, {}, &LuaLanguageEdit::start);
					bool valid = true;
					for (std::size_t i = 1; i < completion.edits.size(); ++i)
						if (completion.edits[i].start < completion.edits[i - 1].end || completion.edits[i].start == completion.edits[i - 1].start)
							valid = false;
					if (valid)
						event.completions.push_back(std::move(completion));
				}
				catch (std::exception const&) {
				}
			}
		Emit(std::move(event));
	}

	void Handle(json::Object message) {
		auto method = String(message, "method");
		if (!method.empty()) {
			if (message.contains("id"))
				Reply(message);
			else if (method == "textDocument/publishDiagnostics") {
				if (auto params = Find<json::Object>(message, "params"))
					Diagnostics(*params);
			}
			return;
		}
		auto id = Integer(message, "id", -1);
		if (ready_id && id == ready_id) {
			if (message.contains("error"))
				throw std::runtime_error("LuaLS could not prepare the analysis workspace");
			ready_id = 0;
			workspace_ready = true;
			if (current.document->filename.empty()) {
				json::Object params;
				params["textDocument"] = DocumentId();
				Notify("textDocument/didClose", std::move(params));
				SyncDocument(true);
			}
			else
				Status(server_name + ": ready", true);
			return;
		}
		if (id == initialize_id) {
			if (message.contains("error"))
				throw std::runtime_error("LuaLS rejected initialization");
			auto const& result = static_cast<json::Object const&>(message.at("result"));
			auto const& capabilities = static_cast<json::Object const&>(result.at("capabilities"));
			auto encoding = String(capabilities, "positionEncoding");
			if (encoding.empty())
				encoding = String(capabilities, "offsetEncoding");
			if (!encoding.empty() && encoding != "utf-16")
				throw std::runtime_error("LuaLS selected an unsupported position encoding");
			server_name = "LuaLS";
			if (auto info = Find<json::Object>(result, "serverInfo"))
				server_name += " " + String(*info, "version");
			initialize_id = 0;
			Notify("initialized", {});
			initialized = true;
			SyncDocument();
			return;
		}
		auto found = pending.find(id);
		if (found == pending.end())
			return;
		auto request = found->second;
		pending.erase(found);
		if (request.request.generation != current.generation)
			return;
		if (auto error = Find<json::Object>(message, "error")) {
			Status(server_name + ": " + String(*error, "message"), true);
			auto kind = request.request.kind == LuaLanguageRequest::Completion ? LuaLanguageEvent::Kind::Completion : request.request.kind == LuaLanguageRequest::Hover ? LuaLanguageEvent::Kind::Hover
																																										: LuaLanguageEvent::Kind::Signature;
			Emit({.kind = kind, .generation = request.request.generation, .request = request.request.id, .position = request.request.position});
			return;
		}
		auto result = message.find("result");
		if (result == message.end())
			throw std::runtime_error("LuaLS response has neither a result nor an error");
		if (request.request.kind == LuaLanguageRequest::Completion)
			Completion(result->second, request);
		else {
			LuaLanguageEvent event{.kind = request.request.kind == LuaLanguageRequest::Hover ? LuaLanguageEvent::Kind::Hover : LuaLanguageEvent::Kind::Signature, .generation = request.request.generation, .request = request.request.id, .position = request.request.position};
			if (auto object = Find<json::Object>(message, "result")) {
				if (event.kind == LuaLanguageEvent::Kind::Hover) {
					auto contents = object->find("contents");
					if (contents != object->end())
						event.text = Documentation(contents->second);
				}
				else if (auto signatures = Find<json::Array>(*object, "signatures"); signatures && !signatures->empty()) {
					auto active = Integer(*object, "activeSignature");
					if (active < 0 || static_cast<std::size_t>(active) >= signatures->size())
						active = 0;
					auto const& signature = static_cast<json::Object const&>((*signatures)[static_cast<std::size_t>(active)]);
					event.text = String(signature, "label");
					auto documentation = signature.find("documentation");
					if (documentation != signature.end())
						event.text += "\n" + Documentation(documentation->second);
				}
			}
			Emit(std::move(event));
		}
	}

	void Run() {
		for (;;) {
			Snapshot latest;
			std::vector<RequestData> queued;
			{
				std::unique_lock lock(mutex);
				if (!process.ProcessId())
					changed.wait(lock, [&] { return stopping || desired.generation != current.generation; });
				if (stopping)
					break;
				latest = desired;
				if (workspace_ready && SameSession(desired))
					queued.swap(requests);
			}
			try {
				bool restart = !attempted || !SameSession(latest);
				bool updated = latest.generation != current.generation;
				if (restart) {
					Stop();
					attempted = true;
				}
				current = std::move(latest);
				if (restart && current.document)
					Start();
				else if (!current.document && updated)
					Status("LuaLS: inactive");
				else if (initialized && updated)
					SyncDocument();
				if (!process.ProcessId()) {
					if (updated && current.document && !restart)
						Status(last_status);
					continue;
				}
				if ((initialize_id || ready_id) && Clock::now() > initialize_deadline)
					throw std::runtime_error("LuaLS initialization timed out");
				for (auto const& [id, request] : pending)
					if (Clock::now() > request.deadline)
						throw std::runtime_error("LuaLS request timed out; reopen the Workspace or change its settings to retry");
				for (auto const& request : queued) {
					if (!initialized || request.generation != current.generation)
						continue;
					json::Object params;
					params["textDocument"] = DocumentId();
					params["position"] = Position(source, request.position);
					auto method = request.kind == LuaLanguageRequest::Completion ? "textDocument/completion" : request.kind == LuaLanguageRequest::Signature ? "textDocument/signatureHelp"
																																							 : "textDocument/hover";
					for (auto it = pending.begin(); it != pending.end();) {
						if (it->second.request.kind != request.kind) {
							++it;
							continue;
						}
						json::Object cancel;
						cancel["id"] = it->first;
						Notify("$/cancelRequest", std::move(cancel));
						it = pending.erase(it);
					}
					auto id = SendRequest(method, std::move(params));
					pending.emplace(id, Pending{.request = request, .deadline = Clock::now() + 5s});
				}
				if (auto message = Receive())
					Handle(std::move(*message));
			}
			catch (std::exception const& error) {
				Status(std::string("LuaLS unavailable: ") + error.what());
				Stop();
			}
			catch (...) {
				Status("LuaLS unavailable: unexpected language service failure");
				Stop();
			}
		}
		Stop();
	}
};

LuaLanguageServer::LuaLanguageServer() : impl(std::make_unique<Impl>()) {}
LuaLanguageServer::~LuaLanguageServer() = default;

std::uint64_t LuaLanguageServer::Update(LuaLanguageConfiguration configuration, std::optional<LuaLanguageDocument> document) {
	std::scoped_lock lock(impl->mutex);
	if (impl->desired.configuration == configuration && impl->desired.document == document)
		return impl->desired.generation;
	impl->desired.configuration = std::move(configuration);
	impl->desired.document = std::move(document);
	++impl->desired.generation;
	impl->requests.clear();
	impl->events.clear();
	impl->changed.notify_one();
	return impl->desired.generation;
}

std::uint64_t LuaLanguageServer::Request(LuaLanguageRequest kind, std::size_t position, std::size_t selection_start, std::size_t selection_end) {
	std::scoped_lock lock(impl->mutex);
	if (!impl->desired.document || position > impl->desired.document->source.size() || selection_start > selection_end || selection_end > impl->desired.document->source.size())
		return 0;
	std::erase_if(impl->requests, [&](auto const& item) { return item.kind == kind; });
	auto id = ++impl->next_request;
	impl->requests.push_back({.kind = kind, .generation = impl->desired.generation, .id = id, .position = position, .selection_start = selection_start, .selection_end = selection_end});
	impl->changed.notify_one();
	return id;
}

std::vector<LuaLanguageEvent> LuaLanguageServer::Drain() {
	std::scoped_lock lock(impl->mutex);
	return std::exchange(impl->events, {});
}

}
