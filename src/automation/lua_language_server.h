#pragma once

#include <libaegisub/fs_fwd.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Automation4 {

struct LuaLanguageConfiguration {
	agi::fs::path directory;
	agi::fs::path cache_directory;
	std::vector<agi::fs::path> include_directories;
	bool operator==(LuaLanguageConfiguration const&) const = default;
};

struct LuaLanguageDocument {
	std::string identity;
	agi::fs::path filename;
	std::string source;
	std::string definitions;
	std::vector<std::string> disabled_builtins;
	std::uint64_t revision = 0;
	bool operator==(LuaLanguageDocument const&) const = default;
};

enum class LuaLanguageRequest { Completion,
								Signature,
								Hover };

struct LuaLanguageEdit {
	std::size_t start = 0;
	std::size_t end = 0;
	std::string text;
};

struct LuaLanguageCompletion {
	std::string label;
	std::string detail;
	std::size_t caret = 0;
	std::vector<LuaLanguageEdit> edits;
};

struct LuaLanguageDiagnostic {
	std::size_t start = 0;
	std::size_t end = 0;
	int severity = 1;
	std::string message;
};

struct LuaLanguageEvent {
	enum class Kind { Status,
					  Diagnostics,
					  Completion,
					  Signature,
					  Hover };
	Kind kind = Kind::Status;
	std::uint64_t generation = 0;
	std::uint64_t request = 0;
	std::size_t position = 0;
	std::string text;
	bool ready = false;
	std::vector<LuaLanguageCompletion> completions;
	std::vector<LuaLanguageDiagnostic> diagnostics;
};

class LuaLanguageServer final {
	struct Impl;
	std::unique_ptr<Impl> impl;

	public:
	LuaLanguageServer();
	~LuaLanguageServer();
	LuaLanguageServer(LuaLanguageServer const&) = delete;
	LuaLanguageServer& operator=(LuaLanguageServer const&) = delete;
	std::uint64_t Update(LuaLanguageConfiguration configuration, std::optional<LuaLanguageDocument> document);
	std::uint64_t Request(LuaLanguageRequest kind, std::size_t position, std::size_t selection_start, std::size_t selection_end);
	std::vector<LuaLanguageEvent> Drain();
};

}
