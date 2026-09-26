#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Automation4 {
struct LuaSourceDiagnostic {
	std::string message;
	int line = 0;
	int column = 0;
};

struct LuaSourceResult {
	std::string source;
	std::optional<LuaSourceDiagnostic> diagnostic;
	[[nodiscard]] bool Succeeded() const { return !diagnostic; }
};

std::optional<LuaSourceDiagnostic> ValidateLuaSource(std::string_view source);
LuaSourceResult SerializeLuaSource(std::string_view source);
LuaSourceResult FormatLuaSource(std::string_view source);
}
