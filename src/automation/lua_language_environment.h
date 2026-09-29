#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Automation4 {

struct LuaHostHint {
	std::string name;
	std::string type;
	bool optional = false;
};

struct LuaHostClass {
	std::string parent;
	std::vector<LuaHostHint> fields;
};

struct LuaHostHints {
	std::map<std::string, std::string> roots;
	std::map<std::string, LuaHostClass> classes;
	std::vector<LuaHostHint> Members(std::string_view path) const;
	std::optional<LuaHostHint> Find(std::string_view path, std::string_view name) const;
};

struct LuaLanguageEnvironment {
	std::string definitions;
	std::vector<std::string> disabled_builtins;
	LuaHostHints host_hints;
};

LuaLanguageEnvironment BuildLuaLanguageEnvironment(unsigned scopes);

}
