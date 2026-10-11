#include "dotnet_subtitle_bridge.h"

#include "auto4_base.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_style.h"
#include "automation/automation_host.h"
#include "automation/automation_mutation_journal.h"
#include "include/aegisub/context.h"
#include "selection_controller.h"
#include "subs_controller.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace Automation4 {
namespace {

bool IsJsonNull(json::UnknownElement const& value) {
	try {
		(void)static_cast<json::Null const&>(value);
		return true;
	}
	catch (...) {
		return false;
	}
}

std::optional<json::Object const*> FindObject(
	json::Object const& object,
	std::string const& key) {
	auto it = object.find(key);
	if (it == object.end()) return std::nullopt;
	if (IsJsonNull(it->second)) return std::nullopt;
	try {
		return &static_cast<json::Object const&>(it->second);
	}
	catch (...) {
		throw std::runtime_error("C# Macro result field '" + key + "' must be an object or null");
	}
}

std::optional<json::Array const*> FindArray(
	json::Object const& object,
	std::string const& key) {
	auto it = object.find(key);
	if (it == object.end()) return std::nullopt;
	if (IsJsonNull(it->second)) return std::nullopt;
	try {
		return &static_cast<json::Array const&>(it->second);
	}
	catch (...) {
		throw std::runtime_error("C# Macro result field '" + key + "' must be an array or null");
	}
}

std::optional<std::string> FindString(
	json::Object const& object,
	std::string const& key) {
	auto it = object.find(key);
	if (it == object.end()) return std::nullopt;
	if (IsJsonNull(it->second)) return std::nullopt;
	try {
		return static_cast<json::String const&>(it->second);
	}
	catch (...) {
		throw std::runtime_error("C# Macro result field '" + key + "' must be a string or null");
	}
}

std::optional<int64_t> FindInt64(
	json::Object const& object,
	std::string const& key) {
	auto it = object.find(key);
	if (it == object.end()) return std::nullopt;
	if (IsJsonNull(it->second)) return std::nullopt;
	try {
		return static_cast<int64_t>(static_cast<json::Integer const&>(it->second));
	}
	catch (...) {
		throw std::runtime_error("C# Macro result field '" + key + "' must be an integer or null");
	}
}

std::optional<int> FindInt(json::Object const& object, std::string const& key) {
	auto value = FindInt64(object, key);
	if (!value) return std::nullopt;
	if (*value < std::numeric_limits<int>::min() ||
		*value > std::numeric_limits<int>::max())
		throw std::runtime_error(
			"C# Macro result field '" + key + "' is outside the native integer range");
	return static_cast<int>(*value);
}

std::optional<bool> FindBool(json::Object const& object, std::string const& key) {
	auto it = object.find(key);
	if (it == object.end()) return std::nullopt;
	if (IsJsonNull(it->second)) return std::nullopt;
	try {
		return static_cast<json::Boolean const&>(it->second);
	}
	catch (...) {
		throw std::runtime_error("C# Macro result field '" + key + "' must be boolean or null");
	}
}

std::string RequireString(json::Object const& object, std::string const& key) {
	auto value = FindString(object, key);
	if (!value)
		throw std::runtime_error("C# Macro result requires string field '" + key + "'");
	return std::move(*value);
}

int RequireInt(json::Object const& object, std::string const& key) {
	auto value = FindInt(object, key);
	if (!value)
		throw std::runtime_error("C# Macro result requires integer field '" + key + "'");
	return *value;
}

int64_t RequireInt64(json::Object const& object, std::string const& key) {
	auto value = FindInt64(object, key);
	if (!value)
		throw std::runtime_error("C# Macro result requires integer field '" + key + "'");
	return *value;
}

json::Array const& RequireArray(json::Object const& object, std::string const& key) {
	auto value = FindArray(object, key);
	if (!value)
		throw std::runtime_error("C# Macro result requires array field '" + key + "'");
	return **value;
}

json::Object ColorJson(agi::Color const& color) {
	json::Object result;
	result["red"] = static_cast<json::Integer>(color.r);
	result["green"] = static_cast<json::Integer>(color.g);
	result["blue"] = static_cast<json::Integer>(color.b);
	result["alpha"] = static_cast<json::Integer>(color.a);
	return result;
}

std::string SerializeJson(json::Object const& value) {
	std::ostringstream stream;
	agi::JsonWriter::Write(value, stream);
	return stream.str();
}

AutomationMutationLineSnapshot CaptureMutationLine(AssDialogue const& line) {
	return {"dialogue", line.GroupHeader(), line.GetEntryData()};
}

bool Equals(std::string const& left, boost::flyweight<std::string> const& right) {
	return left == right.get();
}

int CommitMaskForPatch(AssDialogue const& line, DotNetSubtitleEventPatch const& patch) {
	int mask = 0;
	if (patch.text && !Equals(*patch.text, line.Text))
		mask |= AssFile::COMMIT_DIAG_TEXT;
	if ((patch.start_milliseconds && line.Start.GetMillisecond() != *patch.start_milliseconds) ||
		(patch.end_milliseconds && line.End.GetMillisecond() != *patch.end_milliseconds))
		mask |= AssFile::COMMIT_DIAG_TIME;
	if ((patch.comment && line.Comment != *patch.comment) ||
		(patch.layer && line.Layer != *patch.layer) ||
		(patch.style && !Equals(*patch.style, line.Style)) ||
		(patch.actor && !Equals(*patch.actor, line.Actor)) ||
		(patch.effect && !Equals(*patch.effect, line.Effect)) ||
		(patch.margin_left && line.Margin[0] != *patch.margin_left) ||
		(patch.margin_right && line.Margin[1] != *patch.margin_right) ||
		(patch.margin_vertical && line.Margin[2] != *patch.margin_vertical))
		mask |= AssFile::COMMIT_DIAG_META;
	return mask;
}

void ApplyPatch(AssDialogue& line, DotNetSubtitleEventPatch const& patch) {
	if (patch.comment) line.Comment = *patch.comment;
	if (patch.layer) line.Layer = *patch.layer;
	if (patch.start_milliseconds) line.Start = *patch.start_milliseconds;
	if (patch.end_milliseconds) line.End = *patch.end_milliseconds;
	if (patch.style) line.Style = *patch.style;
	if (patch.actor) line.Actor = *patch.actor;
	if (patch.effect) line.Effect = *patch.effect;
	if (patch.margin_left) line.Margin[0] = *patch.margin_left;
	if (patch.margin_right) line.Margin[1] = *patch.margin_right;
	if (patch.margin_vertical) line.Margin[2] = *patch.margin_vertical;
	if (patch.text) line.Text = *patch.text;
}

} // namespace

std::string BuildDotNetMacroContextJson(
	agi::Context const *context,
	int64_t invocation_token,
	bool include_subtitles,
	DotNetSubtitleSnapshotScope snapshot_scope,
	AutomationProgress *progress) {
	if (invocation_token <= 0)
		throw std::invalid_argument("C# Macro invocation token must be positive");

	json::Object root;
	root["invocationToken"] = static_cast<json::Integer>(invocation_token);
	if (!context || !include_subtitles) {
		root["subtitles"] = json::Null{};
		return SerializeJson(root);
	}

	auto core = context->GetCore();
	std::set<int> selected_event_ids;
	std::set<uint32_t> referenced_extradata_ids;
	for (auto const* line : core.selectionController->GetSortedSelection()) {
		selected_event_ids.insert(line->Id);
		if (snapshot_scope == DotNetSubtitleSnapshotScope::Selection) {
			for (auto id : line->ExtradataIds.get())
				referenced_extradata_ids.insert(id);
		}
	}
	auto event_count = snapshot_scope == DotNetSubtitleSnapshotScope::Selection
		? static_cast<size_t>(std::count_if(
			core.ass->Events.begin(), core.ass->Events.end(),
			[&](AssDialogue const& line) {
				return selected_event_ids.contains(line.Id);
			}))
		: core.ass->Events.size();
	auto extradata_count = snapshot_scope == DotNetSubtitleSnapshotScope::Selection
		? static_cast<size_t>(std::count_if(
			core.ass->Extradata.begin(), core.ass->Extradata.end(),
			[&](ExtradataEntry const& entry) {
				return referenced_extradata_ids.contains(entry.id);
			}))
		: core.ass->Extradata.size();
	auto total_items = static_cast<int64_t>(
		core.ass->Info.size() + core.ass->Styles.size() + event_count + extradata_count);
	int64_t completed_items = 0;
	auto report_snapshot_progress = [&] {
		if (!progress) return;
		if (progress->IsCancelled())
			throw agi::UserCancelException("C# subtitle snapshot cancelled");
		if (completed_items == total_items || completed_items % 256 == 0)
			progress->SetProgress(completed_items, std::max<int64_t>(total_items, 1));
	};
	if (progress) {
		progress->SetMessage("Preparing C# subtitle snapshot");
		progress->SetProgress(0, std::max<int64_t>(total_items, 1));
	}
	json::Object subtitles;
	subtitles["documentToken"] = static_cast<json::Integer>(
		core.subsController->GetDocumentRevision());
	subtitles["fileName"] = core.subsController->HasFile()
		? agi::fs::PathToString(core.subsController->Filename())
		: std::string();

	json::Array script_info;
	for (auto const& info : core.ass->Info) {
		json::Object item;
		item["key"] = info.Key();
		item["value"] = info.Value();
		script_info.push_back(std::move(item));
		++completed_items;
		report_snapshot_progress();
	}
	subtitles["scriptInfo"] = std::move(script_info);

	json::Array styles;
	for (auto const& style : core.ass->Styles) {
		json::Object item;
		item["name"] = style.name;
		item["fontName"] = style.font;
		item["fontSize"] = style.fontsize;
		item["primaryColor"] = ColorJson(style.primary);
		item["secondaryColor"] = ColorJson(style.secondary);
		item["outlineColor"] = ColorJson(style.outline);
		item["shadowColor"] = ColorJson(style.shadow);
		item["bold"] = style.bold;
		item["italic"] = style.italic;
		item["underline"] = style.underline;
		item["strikeout"] = style.strikeout;
		item["scaleX"] = style.scalex;
		item["scaleY"] = style.scaley;
		item["spacing"] = style.spacing;
		item["angle"] = style.angle;
		item["borderStyle"] = static_cast<json::Integer>(style.borderstyle);
		item["outlineWidth"] = style.outline_w;
		item["shadowWidth"] = style.shadow_w;
		item["alignment"] = static_cast<json::Integer>(style.alignment);
		item["marginLeft"] = static_cast<json::Integer>(style.Margin[0]);
		item["marginRight"] = static_cast<json::Integer>(style.Margin[1]);
		item["marginVertical"] = static_cast<json::Integer>(style.Margin[2]);
		item["encoding"] = static_cast<json::Integer>(style.encoding);
		styles.push_back(std::move(item));
		++completed_items;
		report_snapshot_progress();
	}
	subtitles["styles"] = std::move(styles);

	json::Array extradata;
	for (auto const& entry : core.ass->Extradata) {
		if (snapshot_scope == DotNetSubtitleSnapshotScope::Selection &&
			!referenced_extradata_ids.contains(entry.id))
			continue;
		json::Object item;
		item["id"] = static_cast<json::Integer>(entry.id);
		item["key"] = entry.key;
		item["value"] = entry.value;
		extradata.push_back(std::move(item));
		++completed_items;
		report_snapshot_progress();
	}
	subtitles["extradata"] = std::move(extradata);

	json::Array events;
	for (auto const& line : core.ass->Events) {
		if (snapshot_scope == DotNetSubtitleSnapshotScope::Selection &&
			!selected_event_ids.contains(line.Id))
			continue;
		json::Object item;
		item["eventId"] = static_cast<json::Integer>(line.Id);
		item["rowIndex"] = static_cast<json::Integer>(line.Row);
		item["comment"] = line.Comment;
		item["layer"] = static_cast<json::Integer>(line.Layer);
		item["startMilliseconds"] = static_cast<json::Integer>(line.Start.GetMillisecond());
		item["endMilliseconds"] = static_cast<json::Integer>(line.End.GetMillisecond());
		item["style"] = line.Style.get();
		item["actor"] = line.Actor.get();
		item["effect"] = line.Effect.get();
		item["marginLeft"] = static_cast<json::Integer>(line.Margin[0]);
		item["marginRight"] = static_cast<json::Integer>(line.Margin[1]);
		item["marginVertical"] = static_cast<json::Integer>(line.Margin[2]);
		item["text"] = line.Text.get();
		json::Array extradata_ids;
		for (auto id : line.ExtradataIds.get())
			extradata_ids.push_back(static_cast<json::Integer>(id));
		item["extradataIds"] = std::move(extradata_ids);
		events.push_back(std::move(item));
		++completed_items;
		report_snapshot_progress();
	}
	subtitles["events"] = std::move(events);

	json::Array selected_ids;
	for (auto const* line : core.selectionController->GetSortedSelection())
		selected_ids.push_back(static_cast<json::Integer>(line->Id));
	subtitles["selectedEventIds"] = std::move(selected_ids);
	if (auto const* active = core.selectionController->GetActiveLine())
		subtitles["activeEventId"] = static_cast<json::Integer>(active->Id);
	else
		subtitles["activeEventId"] = json::Null{};

	root["subtitles"] = std::move(subtitles);
	if (progress)
		progress->SetMessage("Serializing C# subtitle snapshot");
	return SerializeJson(root);
}

DotNetMacroExecutionResult ParseDotNetMacroResultJson(std::string const& result_json) {
	std::istringstream stream(result_json);
	json::UnknownElement root;
	json::Reader::Read(root, stream);
	auto const& object = static_cast<json::Object const&>(root);

	DotNetMacroExecutionResult result;
	result.status_message = RequireString(object, "statusMessage");
	auto mutation_object = FindObject(object, "mutation");
	if (!mutation_object) return result;

	DotNetSubtitleMutationBatch mutation;
	mutation.expected_document_token = RequireInt64(**mutation_object, "expectedDocumentToken");
	mutation.undo_description = RequireString(**mutation_object, "undoDescription");
	if (mutation.undo_description.empty())
		throw std::runtime_error("C# subtitle mutation requires a non-empty undoDescription");

	auto const& patches = RequireArray(**mutation_object, "eventPatches");
	mutation.event_patches.reserve(patches.size());
	for (auto const& item : patches) {
		try {
			auto const& patch_object = static_cast<json::Object const&>(item);
			DotNetSubtitleEventPatch patch;
			patch.event_id = RequireInt(patch_object, "eventId");
			patch.comment = FindBool(patch_object, "comment");
			patch.layer = FindInt(patch_object, "layer");
			patch.start_milliseconds = FindInt(patch_object, "startMilliseconds");
			patch.end_milliseconds = FindInt(patch_object, "endMilliseconds");
			patch.style = FindString(patch_object, "style");
			patch.actor = FindString(patch_object, "actor");
			patch.effect = FindString(patch_object, "effect");
			patch.margin_left = FindInt(patch_object, "marginLeft");
			patch.margin_right = FindInt(patch_object, "marginRight");
			patch.margin_vertical = FindInt(patch_object, "marginVertical");
			patch.text = FindString(patch_object, "text");
			mutation.event_patches.emplace_back(std::move(patch));
		}
		catch (std::runtime_error const&) {
			throw;
		}
		catch (...) {
			throw std::runtime_error("C# subtitle mutation eventPatches must contain objects");
		}
	}

	if (auto selected = FindArray(**mutation_object, "selectedEventIds")) {
		std::vector<int> ids;
		ids.reserve((*selected)->size());
		for (auto const& item : **selected) {
			try {
				auto value = static_cast<int64_t>(static_cast<json::Integer const&>(item));
				if (value < std::numeric_limits<int>::min() ||
					value > std::numeric_limits<int>::max())
					throw std::out_of_range("selected event ID is outside the native range");
				ids.push_back(static_cast<int>(value));
			}
			catch (...) {
				throw std::runtime_error("C# subtitle mutation selectedEventIds must contain integers");
			}
		}
		mutation.selected_event_ids = std::move(ids);
	}
	mutation.active_event_id = FindInt(**mutation_object, "activeEventId");
	result.mutation = std::move(mutation);
	return result;
}

size_t ApplyDotNetMacroMutation(
	agi::Context* context,
	AutomationHost* host,
	DotNetSubtitleMutationBatch const& mutation) {
	if (!context)
		throw std::runtime_error("C# subtitle mutation requires a live project context");
	auto core = context->GetCore();
	if (mutation.expected_document_token != core.subsController->GetDocumentRevision())
		throw std::runtime_error(
			"C# subtitle mutation document revision is stale; rerun the Macro");
	std::map<int, AssDialogue*> events_by_id;
	for (auto& line : core.ass->Events)
		events_by_id.emplace(line.Id, &line);

	std::set<int> patched_ids;
	for (auto const& patch : mutation.event_patches) {
		if (!patched_ids.insert(patch.event_id).second)
			throw std::runtime_error("C# subtitle mutation contains duplicate event IDs");
		auto found = events_by_id.find(patch.event_id);
		if (found == events_by_id.end())
			throw std::runtime_error("C# subtitle mutation references an unknown event ID");

		auto const& line = *found->second;
		int start = patch.start_milliseconds.value_or(line.Start.GetMillisecond());
		int end = patch.end_milliseconds.value_or(line.End.GetMillisecond());
		if (start < 0 || end < 0 || start > end)
			throw std::runtime_error("C# subtitle mutation contains an invalid time range");
		for (auto margin : {patch.margin_left, patch.margin_right, patch.margin_vertical}) {
			if (margin && *margin < 0)
				throw std::runtime_error("C# subtitle mutation contains a negative margin");
		}
		if (patch.style && patch.style->empty())
			throw std::runtime_error("C# subtitle mutation cannot assign an empty style name");
	}

	Selection new_selection;
	AssDialogue* new_active = nullptr;
	if (mutation.selected_event_ids) {
		if (mutation.selected_event_ids->empty())
			throw std::runtime_error("C# subtitle mutation selection cannot be empty");
		std::set<int> selected_ids;
		for (auto id : *mutation.selected_event_ids) {
			if (!selected_ids.insert(id).second)
				throw std::runtime_error("C# subtitle mutation selection contains duplicate event IDs");
			auto found = events_by_id.find(id);
			if (found == events_by_id.end())
				throw std::runtime_error("C# subtitle mutation selection references an unknown event ID");
			new_selection.insert(found->second);
		}
		if (mutation.active_event_id) {
			auto found = events_by_id.find(*mutation.active_event_id);
			if (found == events_by_id.end() || !new_selection.count(found->second))
				throw std::runtime_error("C# subtitle mutation active event must be part of its selection");
			new_active = found->second;
		}
		else {
			new_active = *new_selection.begin();
		}
	}
	else if (mutation.active_event_id) {
		throw std::runtime_error("C# subtitle mutation cannot change active event without a selection");
	}

	auto journal = host ? host->GetMutationJournal() : nullptr;
	int commit_mask = 0;
	size_t applied = 0;
	AssDialogue* single_changed_line = nullptr;
	for (auto const& patch : mutation.event_patches) {
		auto& line = *events_by_id.at(patch.event_id);
		int patch_mask = CommitMaskForPatch(line, patch);
		if (!patch_mask) continue;
		auto before = CaptureMutationLine(line);
		ApplyPatch(line, patch);
		auto after = CaptureMutationLine(line);
		if (journal)
			journal->RecordReplace(line.Row, std::move(before), std::move(after));
		commit_mask |= patch_mask;
		++applied;
		single_changed_line = applied == 1 ? &line : nullptr;
	}

	if (commit_mask) {
		if (journal)
			journal->RecordCommit(
				mutation.undo_description,
				commit_mask,
				static_cast<int>(applied),
				true);
		core.ass->Commit(
			mutation.undo_description,
			commit_mask,
			-1,
			applied == 1 ? single_changed_line : nullptr);
	}
	if (mutation.selected_event_ids)
		core.selectionController->SetSelectionAndActive(std::move(new_selection), new_active);
	return applied;
}

} // namespace Automation4
