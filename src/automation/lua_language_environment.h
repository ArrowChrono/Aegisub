#pragma once

#include <string>
#include <vector>

namespace Automation4 {

struct LuaLanguageEnvironment {
	std::string definitions;
	std::vector<std::string> disabled_builtins;
};

LuaLanguageEnvironment BuildLuaLanguageEnvironment(unsigned scopes);

}
