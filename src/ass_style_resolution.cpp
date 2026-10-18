// Copyright (c) 2026

#include "ass_style_resolution.h"
#include "translation_service.h"

#include "ass_compat.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"

#include <libaegisub/string_utils.h>
#include <libaegisub/format.h>

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace {

std::string to_string(agi::util::strings::view value) {
	return std::string(value.begin(), value.end());
}

std::string definition_key(std::string const& name) {
	auto stripped = AssCompat::StripStyleCompatibilityPrefix(name);
	if (stripped.empty())
		return "Default";
	return to_string(stripped);
}

std::string event_lookup_key(std::string const& name) {
	auto stripped = AssCompat::StripStyleCompatibilityPrefix(name);
	if (agi::util::strings::iequals(stripped, "Default"))
		return "Default";
	return stripped.empty() ? "Default" : to_string(stripped);
}

bool can_canonicalize_definition(std::string const& key) {
	return !agi::util::strings::iequals(key, "Default") || key == "Default";
}

bool same_style_body(AssStyle const& a, AssStyle const& b) {
	return a.font == b.font
		&& a.fontsize == b.fontsize
		&& a.primary == b.primary
		&& a.secondary == b.secondary
		&& a.outline == b.outline
		&& a.shadow == b.shadow
		&& a.bold == b.bold
		&& a.italic == b.italic
		&& a.underline == b.underline
		&& a.strikeout == b.strikeout
		&& a.scalex == b.scalex
		&& a.scaley == b.scaley
		&& a.spacing == b.spacing
		&& a.angle == b.angle
		&& a.borderstyle == b.borderstyle
		&& a.outline_w == b.outline_w
		&& a.shadow_w == b.shadow_w
		&& a.alignment == b.alignment
		&& a.Margin == b.Margin
		&& a.encoding == b.encoding;
}

template<class AssFileT>
auto resolve_event_style_impl(AssFileT& file, std::string const& name) -> decltype(&*file.Styles.begin()) {
	for (auto it = file.Styles.rbegin(); it != file.Styles.rend(); ++it) {
		if (AssCompat::StyleNamesMatch(it->name, name))
			return &*it;
	}
	return nullptr;
}

template<class AssFileT>
auto resolve_reset_style_impl(AssFileT& file, std::string const& name) -> decltype(&*file.Styles.begin()) {
	for (auto it = file.Styles.rbegin(); it != file.Styles.rend(); ++it) {
		if (definition_key(it->name) == name)
			return &*it;
	}
	return nullptr;
}

struct ResetScanState {
	aegisub::ass_style_resolution::StyleResolutionGraph *graph = nullptr;
	AssFile *file = nullptr;
	AssDialogue *line = nullptr;
	int line_number = -1;
};

void collect_reset_usage(std::string const& tag, AssOverrideParameter *param, void *userdata) {
	if (tag != "\\r" || param->GetType() != VariableDataType::TEXT)
		return;

	auto *state = static_cast<ResetScanState *>(userdata);
	auto raw = param->Get<std::string>();
	auto *resolved = aegisub::ass_style_resolution::ResolveResetStyle(*state->file, raw);

	aegisub::ass_style_resolution::StyleUsage usage;
	usage.kind = aegisub::ass_style_resolution::StyleUsageKind::ResetTag;
	usage.line = state->line;
	usage.raw_name = std::move(raw);
	usage.line_number = state->line_number;
	usage.strict_lookup = true;
	usage.resolved = resolved != nullptr;
	if (resolved) {
		auto it = std::find_if(state->graph->styles.begin(), state->graph->styles.end(),
			[=](auto const& record) { return record.style == resolved; });
		if (it != state->graph->styles.end())
			usage.resolved_style = static_cast<int>(it - state->graph->styles.begin());
	}
	state->graph->usages.push_back(std::move(usage));
}

int find_style_index(aegisub::ass_style_resolution::StyleResolutionGraph const& graph, AssStyle const* style) {
	auto it = std::find_if(graph.styles.begin(), graph.styles.end(),
		[=](auto const& record) { return record.style == style; });
	return it == graph.styles.end() ? -1 : static_cast<int>(it - graph.styles.begin());
}

std::vector<int> reset_usage_counts(aegisub::ass_style_resolution::StyleResolutionGraph const& graph) {
	std::vector<int> counts(graph.styles.size());
	for (auto const& usage : graph.usages) {
		if (usage.kind == aegisub::ass_style_resolution::StyleUsageKind::ResetTag
			&& usage.resolved_style >= 0
			&& usage.resolved_style < static_cast<int>(counts.size()))
			++counts[usage.resolved_style];
	}
	return counts;
}

struct UpdateGroupKey {
	std::string old_name;
	std::string new_name;
	std::string resolved_style_name;

	bool operator<(UpdateGroupKey const& other) const {
		if (old_name != other.old_name)
			return old_name < other.old_name;
		if (new_name != other.new_name)
			return new_name < other.new_name;
		return resolved_style_name < other.resolved_style_name;
	}
};

std::string format_line_ranges(std::vector<int> lines) {
	bool has_unknown = false;
	lines.erase(std::remove_if(lines.begin(), lines.end(), [&](int line) {
		if (line >= 0)
			return false;
		has_unknown = true;
		return true;
	}), lines.end());
	std::sort(lines.begin(), lines.end());
	lines.erase(std::unique(lines.begin(), lines.end()), lines.end());

	std::ostringstream text;
	bool first = true;
	auto append = [&](std::string const& item) {
		if (!first)
			text << ", ";
		first = false;
		text << item;
	};

	if (has_unknown)
		append("?");

	for (size_t i = 0; i < lines.size();) {
		int start = lines[i];
		int end = start;
		while (i + 1 < lines.size() && lines[i + 1] == end + 1)
			end = lines[++i];

		if (start == end)
			append(std::to_string(start));
		else
			append(std::to_string(start) + "-" + std::to_string(end));
		++i;
	}

	return text.str();
}

}

namespace aegisub::ass_style_resolution {

AssStyle *ResolveEventStyle(AssFile& file, std::string const& name) {
	return resolve_event_style_impl(file, name);
}

AssStyle const *ResolveEventStyle(AssFile const& file, std::string const& name) {
	return resolve_event_style_impl(file, name);
}

AssStyle *ResolveResetStyle(AssFile& file, std::string const& name) {
	return resolve_reset_style_impl(file, name);
}

AssStyle const *ResolveResetStyle(AssFile const& file, std::string const& name) {
	return resolve_reset_style_impl(file, name);
}

StyleResolutionGraph BuildStyleResolutionGraph(AssFile& file) {
	StyleResolutionGraph graph;
	std::map<std::string, std::vector<int>> groups;

	int index = 0;
	for (auto& style : file.Styles) {
		StyleRecord record;
		record.style = &style;
		record.index = index++;
		record.raw_name = style.name;
		record.definition_key = definition_key(style.name);
		record.canonical_name = record.definition_key;
		record.has_compatibility_prefix = record.raw_name != record.definition_key;
		record.has_default_case_confusion = agi::util::strings::iequals(record.definition_key, "Default")
			&& record.definition_key != "Default";
		groups[record.definition_key].push_back(static_cast<int>(graph.styles.size()));
		graph.styles.push_back(std::move(record));
	}

	for (auto const& group : groups) {
		auto const& members = group.second;
		int winner = members.empty() ? -1 : members.back();
		for (auto member : members) {
			graph.styles[member].group_winner = winner;
			graph.styles[member].shadowed = member != winner;
			if (winner >= 0 && member != winner)
				graph.styles[member].same_as_winner = same_style_body(*graph.styles[member].style, *graph.styles[winner].style);
		}
	}

	int line_number = 1;
	for (auto& line : file.Events) {
		int display_line_number = line.Row >= 0 ? line.Row + 1 : line_number;
		auto raw = line.Style.get();
		auto *resolved = ResolveEventStyle(file, raw);

		StyleUsage usage;
		usage.kind = StyleUsageKind::EventStyle;
		usage.line = &line;
		usage.raw_name = raw;
		usage.line_number = display_line_number;
		usage.strict_lookup = false;
		usage.resolved = resolved != nullptr;
		usage.resolved_style = find_style_index(graph, resolved);
		graph.usages.push_back(std::move(usage));

		ResetScanState state{&graph, &file, &line, display_line_number};
		auto blocks = line.ParseTags();
		for (auto const& block : blocks) {
			if (auto *override_block = dynamic_cast<AssDialogueBlockOverride *>(block.get()))
				override_block->ProcessParameters(&collect_reset_usage, &state);
		}

		++line_number;
	}

	return graph;
}

StyleCleanPlan BuildStyleCleanPlan(StyleResolutionGraph const& graph) {
	StyleCleanPlan plan;
	auto reset_counts = reset_usage_counts(graph);
	std::set<AssStyle *> delete_set;
	std::map<AssStyle *, std::string> final_names;

	for (auto const& record : graph.styles) {
		if (!record.style)
			continue;

		if (record.shadowed) {
			if (record.index >= 0 && reset_counts[record.index] == 0) {
				delete_set.insert(record.style);
				if (record.same_as_winner)
					plan.notes.push_back(agi::format(_("%s duplicates %s and will be merged.").c_str(), record.raw_name, graph.styles[record.group_winner].raw_name));
				else
					plan.notes.push_back(agi::format(_("%s is shadowed by %s and will be removed.").c_str(), record.raw_name, graph.styles[record.group_winner].raw_name));
			}
			else {
				plan.notes.push_back(agi::format(_("%s is shadowed but has strict reset-tag usage, so it will be kept.").c_str(), record.raw_name));
			}
			continue;
		}

		if (record.raw_name != record.canonical_name && can_canonicalize_definition(record.definition_key))
			final_names[record.style] = record.canonical_name;
		else if (record.has_default_case_confusion)
			plan.notes.push_back(agi::format(_("%s is a case-confusing Default-like definition and will be kept unchanged.").c_str(), record.raw_name));
	}

	for (auto const& item : final_names) {
		if (!delete_set.count(item.first))
			plan.renames.push_back({item.first, item.second});
	}

	for (auto const& style : delete_set)
		plan.deletions.push_back(style);

	for (auto const& usage : graph.usages) {
		if (usage.kind != StyleUsageKind::EventStyle || !usage.line || usage.resolved_style < 0)
			continue;

		auto const& record = graph.styles[usage.resolved_style];
		auto target_name = event_lookup_key(record.canonical_name);
		auto final_name = final_names.find(record.style);
		if (final_name != final_names.end())
			target_name = event_lookup_key(final_name->second);

		if (usage.raw_name != target_name)
			plan.event_updates.push_back({usage.line, usage.raw_name, target_name, record.raw_name, usage.line_number});
	}

	return plan;
}

void ApplyStyleCleanPlan(StyleCleanPlan const& plan) {
	for (auto const& update : plan.event_updates) {
		if (update.line)
			update.line->Style = update.new_name;
	}

	for (auto const& rename : plan.renames) {
		if (!rename.style)
			continue;
		rename.style->name = rename.new_name;
		rename.style->UpdateData();
	}

	for (auto *style : plan.deletions)
		delete style;
}

std::string BuildStyleIssueTooltip(StyleResolutionGraph const& graph, int style_index) {
	if (style_index < 0 || style_index >= static_cast<int>(graph.styles.size()))
		return {};

	auto const& record = graph.styles[style_index];
	std::ostringstream tooltip;

	if (record.has_compatibility_prefix)
		tooltip << agi::format(_("Compatibility prefix: '%s' resolves as '%s'.\n").c_str(), record.raw_name, record.definition_key);
	if (record.has_default_case_confusion)
		tooltip << agi::format(_("Default-like definition: Event Style lookup does not use this as 'Default'. Strict \\r%s may still use it.\n").c_str(), record.definition_key);
	if (record.shadowed) {
		auto const& winner = graph.styles[record.group_winner];
		tooltip << agi::format(_("Shadowed by later style '%s'. Clean will use '%s' for Event Style rendering.\n").c_str(), winner.raw_name, winner.definition_key);
	}
	else {
		bool has_shadowed = std::any_of(graph.styles.begin(), graph.styles.end(),
			[&](auto const& other) { return other.group_winner == style_index && other.shadowed; });
		if (has_shadowed)
			tooltip << agi::format(_("Renderer winner for Event Style key '%s'.\n").c_str(), record.definition_key);
	}

	if (record.same_as_winner)
		tooltip << _("The shadowed style has the same content as the winner and can be merged.\n");

	return tooltip.str();
}

std::string BuildStyleCleanPreview(StyleCleanPlan const& plan) {
	std::ostringstream text;
	text << _("Clean style conflicts and compatibility names?\n\n");
	text << agi::format(_("Rename styles: %d\n").c_str(), plan.renames.size());
	for (auto const& rename : plan.renames)
		text << "  " << (rename.style ? rename.style->name : "") << " -> " << rename.new_name << "\n";

	text << agi::format(_("Update dialogue style references: %d\n").c_str(), plan.event_updates.size());
	std::map<UpdateGroupKey, std::vector<int>> update_groups;
	for (auto const& update : plan.event_updates) {
		update_groups[{update.old_name, update.new_name, update.resolved_style_name}].push_back(update.line_number);
	}
	for (auto const& group : update_groups) {
		text << agi::format(_("  lines %s: %s -> %s").c_str(), format_line_ranges(group.second), group.first.old_name, group.first.new_name);
		if (!group.first.resolved_style_name.empty())
			text << agi::format(_(" (renderer style: %s)").c_str(), group.first.resolved_style_name);
		text << "\n";
	}

	text << agi::format(_("Remove shadowed styles: %d\n").c_str(), plan.deletions.size());
	for (auto const& note : plan.notes)
		text << "  " << note << "\n";
	text << _("\nReset tags are not renamed automatically. They use strict \\rStyle lookup; Clean only uses them to avoid deleting styles that may still be referenced.\n");

	return text.str();
}

}
