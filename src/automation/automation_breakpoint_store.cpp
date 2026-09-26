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

#include "automation_breakpoint_store.h"

#include <libaegisub/fs.h>

#include <algorithm>

namespace Automation4 {
namespace {

std::string FilenameComponent(std::string const& source)
{
	return agi::fs::PathToGenericString(agi::fs::PathFromString(source).filename());
}

}

std::string NormalizeAutomationDebugSource(std::string const& source)
{
	if (source.empty())
		return {};

	std::string normalized = source;
	if (!normalized.empty() && normalized.front() == '@')
		normalized.erase(normalized.begin());

	std::replace(normalized.begin(), normalized.end(), '\\', '/');

	// Virtual sources and unnamed chunks should keep their source text.
	if (normalized.find("://") != std::string::npos)
		return normalized;
	if (!normalized.empty() && normalized.front() == '=')
		return normalized;
	if (!normalized.empty() && normalized.front() == '[')
		return normalized;

	auto generic = agi::fs::PathToGenericString(agi::fs::PathFromString(normalized).lexically_normal());
	if (!generic.empty())
		return generic;
	return normalized;
}

void AutomationBreakpointStore::SetBreakpoints(std::vector<AutomationDebugBreakpoint> values)
{
	for (auto& breakpoint : values)
		breakpoint.source_path = NormalizeAutomationDebugSource(breakpoint.source_path);
	breakpoints = std::move(values);
}

std::vector<AutomationDebugBreakpoint> AutomationBreakpointStore::GetBreakpoints() const
{
	return breakpoints;
}

size_t AutomationBreakpointStore::Count() const
{
	return breakpoints.size();
}

bool AutomationBreakpointStore::Matches(std::string const& source_path, int line) const
{
	if (line <= 0)
		return false;

	auto const normalized_source = NormalizeAutomationDebugSource(source_path);
	auto const source_filename = FilenameComponent(normalized_source);

	for (auto const& breakpoint : breakpoints) {
		if (!breakpoint.enabled || breakpoint.line != line)
			continue;

		if (breakpoint.source_path == normalized_source)
			return true;
		if (normalized_source.find("://") != std::string::npos || breakpoint.source_path.find("://") != std::string::npos)
			continue;

		if (!source_filename.empty() && breakpoint.source_path == source_filename)
			return true;

		auto const breakpoint_filename = FilenameComponent(breakpoint.source_path);
		if (!source_filename.empty() && !breakpoint_filename.empty() && breakpoint_filename == source_filename)
			return true;
	}

	return false;
}
}
