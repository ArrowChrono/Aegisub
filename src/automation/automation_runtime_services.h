#pragma once

#include <string>

class AssStyle;
struct lua_State;

namespace Automation4 {
class AutomationRuntimeServices {
	public:
	virtual ~AutomationRuntimeServices() = default;
	virtual std::string Translate(std::string const& text) const = 0;
	virtual std::string GetClipboard() const = 0;
	virtual bool SetClipboard(std::string const& text) const = 0;
	virtual bool CalculateTextExtents(AssStyle const& style, std::string const& text,
									  double& width, double& height, double& descent, double& external_leading) const = 0;
	virtual void InstallLuaModules(lua_State *state) const = 0;
	virtual void LogWarning(std::string const& message) const = 0;
	virtual void LogError(std::string const& message) const = 0;
};
}
