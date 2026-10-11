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

#pragma once

#include "automation_engine.h"

#include <libaegisub/fs_fwd.h>

#include <memory>
#include <string_view>
#include <vector>

namespace Automation4 {
	class AutomationScriptInstance;

	class AutomationEngineRegistry final {
	public:
		static void Register(std::unique_ptr<AutomationEngine> engine);
		static const std::vector<std::unique_ptr<AutomationEngine>>& GetEngines();
		static AutomationEngine const* FindEngine(std::string_view engine_name);
		static std::unique_ptr<AutomationScriptInstance> CreateFromFile(
			agi::fs::path const& filename, std::shared_ptr<AutomationRuntimeServices> const& services = {});
		static std::unique_ptr<AutomationScriptInstance> CreateForEngine(
			std::string_view engine_name, agi::fs::path const& filename,
			std::shared_ptr<AutomationRuntimeServices> services = {});
	};
}
