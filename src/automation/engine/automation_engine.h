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

#include "automation_script_instance.h"

#include <libaegisub/fs_fwd.h>

#include <memory>
#include <string>

namespace Automation4 {
class AutomationRuntimeServices;

class AutomationEngine {
	public:
		virtual ~AutomationEngine() = default;

		virtual std::string EngineName() const = 0;
		virtual std::string FilenamePattern() const = 0;
		virtual bool SupportsFile(agi::fs::path const& filename) const = 0;
		virtual std::unique_ptr<AutomationScriptInstance> LoadScript(agi::fs::path const& filename) const = 0;
		virtual std::unique_ptr<AutomationScriptInstance> LoadScript(
			agi::fs::path const& filename, std::shared_ptr<AutomationRuntimeServices>) const {
			return LoadScript(filename);
		}
	};
}
