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

#include "automation_engine_registry.h"

#include "automation_engine.h"
#include "automation_script_instance.h"

#include <libaegisub/exception.h>

#include <string>
#include <utility>

namespace Automation4 {
namespace {
std::vector<std::unique_ptr<AutomationEngine>>& Engines()
{
	static std::vector<std::unique_ptr<AutomationEngine>> engines;
	return engines;
}
}

void AutomationEngineRegistry::Register(std::unique_ptr<AutomationEngine> engine)
{
	if (!engine)
		throw agi::InternalError("Automation: attempted to register a null engine.");

	auto const engine_name = engine->EngineName();
	if (engine_name.empty())
		throw agi::InternalError("Automation: attempted to register an engine without a name.");

	if (FindEngine(engine_name))
		throw agi::InternalError("Automation: attempted to register the same engine multiple times.");

	Engines().emplace_back(std::move(engine));
}

const std::vector<std::unique_ptr<AutomationEngine>>& AutomationEngineRegistry::GetEngines()
{
	return Engines();
}

AutomationEngine const* AutomationEngineRegistry::FindEngine(std::string_view engine_name)
{
	for (auto const& engine : Engines()) {
		if (engine && engine->EngineName() == engine_name)
			return engine.get();
	}
	return nullptr;
}

std::unique_ptr<AutomationScriptInstance> AutomationEngineRegistry::CreateFromFile(
	agi::fs::path const& filename, std::shared_ptr<AutomationRuntimeServices> const& services) {
	for (auto const& engine : Engines()) {
		if (!engine || !engine->SupportsFile(filename))
			continue;

		if (auto script = engine->LoadScript(filename, services))
			return script;
	}

	return nullptr;
}

std::unique_ptr<AutomationScriptInstance> AutomationEngineRegistry::CreateForEngine(
	std::string_view engine_name, agi::fs::path const& filename,
	std::shared_ptr<AutomationRuntimeServices> services) {
	if (auto const* engine = FindEngine(engine_name))
		return engine->LoadScript(filename, std::move(services));
	return nullptr;
}
}
