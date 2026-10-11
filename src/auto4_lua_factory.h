// Copyright (c) 2006, Niels Martin Hansen
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file auto4_lua_factory.h
/// @brief Factory-class for Lua 5.1-based scripting engine
/// @ingroup scripting
///

#pragma once

#include "automation/engine/automation_engine.h"

namespace Automation4 {
class AutomationRuntimeServices;
class AutomationRunner;
struct LuaWorkspaceRunRequest;

std::unique_ptr<AutomationScriptInstance> CreateLuaAutomationScriptInstance(
	agi::fs::path const& filename, std::shared_ptr<AutomationRuntimeServices> services);
std::unique_ptr<AutomationScriptInstance> CreateLuaAutomationScriptInstanceForWorkspace(
	agi::fs::path const& filename, std::shared_ptr<AutomationRuntimeServices> services,
	std::shared_ptr<LuaWorkspaceRunRequest const> request, AutomationRunner& runner);

class LuaAutomationEngine final : public AutomationEngine {
	std::shared_ptr<AutomationRuntimeServices> services;

	public:
	explicit LuaAutomationEngine(std::shared_ptr<AutomationRuntimeServices> services = {});
	std::string EngineName() const override;
	std::string FilenamePattern() const override;
	bool SupportsFile(agi::fs::path const& filename) const override;
	std::unique_ptr<AutomationScriptInstance> LoadScript(agi::fs::path const& filename) const override;
	std::unique_ptr<AutomationScriptInstance> LoadScript(
		agi::fs::path const& filename, std::shared_ptr<AutomationRuntimeServices> services) const override;
};
}
