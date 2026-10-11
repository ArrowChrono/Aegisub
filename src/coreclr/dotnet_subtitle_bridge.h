#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace agi { struct Context; }

namespace Automation4 {
class AutomationHost;
class AutomationProgress;

enum class DotNetSubtitleSnapshotScope {
	Full,
	Selection
};

struct DotNetSubtitleEventPatch {
	int event_id = -1;
	std::optional<bool> comment;
	std::optional<int> layer;
	std::optional<int> start_milliseconds;
	std::optional<int> end_milliseconds;
	std::optional<std::string> style;
	std::optional<std::string> actor;
	std::optional<std::string> effect;
	std::optional<int> margin_left;
	std::optional<int> margin_right;
	std::optional<int> margin_vertical;
	std::optional<std::string> text;
};

struct DotNetSubtitleMutationBatch {
	int64_t expected_document_token = 0;
	std::string undo_description;
	std::vector<DotNetSubtitleEventPatch> event_patches;
	std::optional<std::vector<int>> selected_event_ids;
	std::optional<int> active_event_id;
};

struct DotNetMacroExecutionResult {
	std::string status_message;
	std::optional<DotNetSubtitleMutationBatch> mutation;
};

std::string BuildDotNetMacroContextJson(
	agi::Context const *context,
	int64_t invocation_token,
	bool include_subtitles,
	DotNetSubtitleSnapshotScope snapshot_scope,
	AutomationProgress *progress = nullptr);

DotNetMacroExecutionResult ParseDotNetMacroResultJson(std::string const& result_json);

size_t ApplyDotNetMacroMutation(
	agi::Context* context,
	AutomationHost* host,
	DotNetSubtitleMutationBatch const& mutation);

} // namespace Automation4
