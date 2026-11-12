#include "subtitle_grid_folding.h"

#include "ass_dialogue.h"
#include "ass_file.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {
constexpr std::string_view fold_key = "_aegi_folddata";

struct Boundary {
	uint64_t id;
	bool end;
	bool collapsed;
};

std::optional<uint64_t> ParseId(std::string_view value) {
	auto separator = value.rfind(';');
	if (separator == std::string_view::npos)
		return {};
	value.remove_prefix(separator + 1);
	uint64_t id = 0;
	auto result = std::from_chars(value.data(), value.data() + value.size(), id);
	if (result.ec != std::errc{} || result.ptr != value.data() + value.size())
		return {};
	return id;
}

std::optional<Boundary> ParseBoundary(std::string_view value) {
	if (value.size() < 5 || (value[0] != '0' && value[0] != '1') || value[1] != ';' || (value[2] != '0' && value[2] != '1') || value[3] != ';' || value.find(';', 4) != std::string_view::npos)
		return {};
	if (auto id = ParseId(value))
		return Boundary{.id = *id, .end = value[0] == '1', .collapsed = value[2] == '1'};
	return {};
}

using MarkerMap = std::unordered_map<uint32_t, std::string_view>;

MarkerMap Markers(AssFile const& file) {
	MarkerMap result;
	for (auto const& entry : file.Extradata) {
		if (entry.key == fold_key)
			result.emplace(entry.id, entry.value);
	}
	return result;
}

template <typename Predicate>
bool RemoveMarkers(AssDialogue& line, MarkerMap const& markers, Predicate predicate) {
	auto ids = line.ExtradataIds.get();
	auto removed = std::erase_if(ids, [&](uint32_t id) {
		auto marker = markers.find(id);
		return marker != markers.end() && predicate(marker->second);
	});
	if (removed)
		line.ExtradataIds = std::move(ids);
	return removed != 0;
}

bool RemoveGroups(AssFile& file, std::unordered_set<uint64_t> const& groups) {
	if (groups.empty())
		return false;
	auto markers = Markers(file);
	bool changed = false;
	for (auto& line : file.Events) {
		changed |= RemoveMarkers(line, markers, [&](std::string_view value) {
			auto id = ParseId(value);
			return id && groups.contains(*id);
		});
	}
	return changed;
}

uint64_t NextGroupId(AssFile const& file) {
	std::unordered_set<uint64_t> used;
	for (auto const& entry : file.Extradata) {
		if (entry.key == fold_key) {
			if (auto id = ParseId(entry.value))
				used.insert(*id);
		}
	}
	uint64_t id = 1;
	while (used.contains(id))
		++id;
	return id;
}

void WriteBoundary(AssFile& file, AssDialogue& line, uint64_t id, bool end, bool collapsed) {
	SubtitleGridFolding::StripMarkers(file, line);
	auto value = std::string(end ? "1;" : "0;") + (collapsed ? "1;" : "0;") + std::to_string(id);
	auto ids = line.ExtradataIds.get();
	ids.push_back(file.AddExtradata(std::string(fold_key), value));
	std::ranges::sort(ids);
	line.ExtradataIds = std::move(ids);
}
}

void SubtitleGridFolding::RemapPreparedRows(std::function<AssDialogue *(AssDialogue *)> const& resolve) {
	for (auto *& row : rows)
		row = resolve(row);
	for (auto& row : display_rows)
		row.dialogue = resolve(row.dialogue);
}

void SubtitleGridFolding::Rebuild(AssFile& file, bool reset_temporary) {
	rows.clear();
	row_ids.clear();
	groups.clear();
	if (reset_temporary)
		temporarily_expanded.clear();

	struct Pair {
		int start = -1;
		int end = -1;
		bool collapsed = false;
		bool invalid = false;
	};
	std::unordered_map<uint64_t, Pair> pairs;
	auto markers = Markers(file);
	for (auto& line : file.Events) {
		auto row = static_cast<int>(rows.size());
		rows.push_back(&line);
		row_ids.push_back(line.Id);
		auto const& references = line.ExtradataIds.get();
		auto marker_count = std::count_if(references.begin(), references.end(), [&](uint32_t id) {
			return markers.contains(id);
		});
		for (auto reference : references) {
			auto marker = markers.find(reference);
			if (marker == markers.end())
				continue;
			auto boundary = ParseBoundary(marker->second);
			if (!boundary) {
				if (auto id = ParseId(marker->second))
					pairs[*id].invalid = true;
				continue;
			}
			auto& pair = pairs[boundary->id];
			int& position = boundary->end ? pair.end : pair.start;
			if (position != -1 || marker_count != 1)
				pair.invalid = true;
			position = row;
			if (!boundary->end)
				pair.collapsed = boundary->collapsed;
		}
	}
	for (auto const& [id, pair] : pairs) {
		if (!pair.invalid && pair.start >= 0 && pair.end > pair.start)
			groups.push_back({.id = id, .start = pair.start, .end = pair.end, .collapsed = pair.collapsed});
	}
	std::ranges::sort(groups, [](Group const& first, Group const& second) {
		return first.start < second.start;
	});

	// Reject every interval in an overlapping component, including nested groups.
	std::vector<Group> valid;
	for (size_t first = 0; first < groups.size();) {
		size_t next = first + 1;
		int end = groups[first].end;
		while (next < groups.size() && groups[next].start <= end) {
			end = std::max(end, groups[next].end);
			++next;
		}
		if (next == first + 1)
			valid.push_back(groups[first]);
		first = next;
	}
	groups = std::move(valid);
	row_groups.assign(rows.size(), -1);
	std::unordered_set<uint64_t> valid_ids;
	for (size_t index = 0; index < groups.size(); ++index) {
		auto const& group = groups[index];
		valid_ids.insert(group.id);
		std::fill(row_groups.begin() + group.start, row_groups.begin() + group.end + 1, static_cast<int>(index));
	}
	std::erase_if(temporarily_expanded, [&](uint64_t id) { return !valid_ids.contains(id); });
	BuildDisplayRows();
}

void SubtitleGridFolding::BuildDisplayRows() {
	display_rows.clear();
	source_to_display.assign(rows.size(), -1);
	for (int row = 0; std::cmp_less(row, rows.size()); ++row) {
		auto group = GroupAt(row);
		bool collapsed = group && IsCollapsed(*group);
		int display = static_cast<int>(display_rows.size());
		source_to_display[row] = display;
		display_rows.push_back({.dialogue = rows[row], .source_row = row, .display_row = display, .is_fold_start = group && row == group->start, .is_fold_end = group && row == group->end, .is_collapsed = collapsed, .hidden_count = collapsed ? group->end - group->start : 0});
		if (collapsed)
			row = group->end;
	}
}

SubtitleGridFolding::Group const *SubtitleGridFolding::GroupAt(int source_row) const {
	if (source_row < 0 || std::cmp_greater_equal(source_row, row_groups.size()) || row_groups[source_row] < 0)
		return nullptr;
	return &groups[row_groups[source_row]];
}

bool SubtitleGridFolding::IsCollapsed(Group const& group) const {
	return group.collapsed && !temporarily_expanded.contains(group.id);
}

int SubtitleGridFolding::DisplayToSource(int display_row) const {
	if (display_row < 0 || std::cmp_greater_equal(display_row, display_rows.size()))
		return -1;
	return display_rows[display_row].source_row;
}

int SubtitleGridFolding::SourceToDisplay(int source_row) const {
	if (source_row < 0 || std::cmp_greater_equal(source_row, source_to_display.size()))
		return -1;
	return source_to_display[source_row];
}

bool SubtitleGridFolding::EnsureVisible(int source_row) {
	auto group = GroupAt(source_row);
	if (!group || source_row == group->start || !IsCollapsed(*group))
		return false;
	temporarily_expanded.insert(group->id);
	BuildDisplayRows();
	return true;
}

bool SubtitleGridFolding::CanCreate(int first, int last) const {
	if (first < 0 || std::cmp_greater_equal(last, rows.size()) || first >= last)
		return false;
	return std::ranges::none_of(groups, [&](Group const& group) {
		return first <= group.end && last >= group.start;
	});
}

bool SubtitleGridFolding::Create(AssFile& file, int first, int last, bool collapsed) {
	if (!CanCreate(first, last))
		return false;
	AddCopiedGroup(file, *rows[first], *rows[last], collapsed);
	Rebuild(file);
	return true;
}

bool SubtitleGridFolding::SetCollapsed(AssFile& file, int source_row, bool collapsed) {
	auto group = GroupAt(source_row);
	if (!group || (group->collapsed == collapsed && IsCollapsed(*group) == collapsed))
		return false;
	WriteBoundary(file, *rows[group->start], group->id, false, collapsed);
	WriteBoundary(file, *rows[group->end], group->id, true, collapsed);
	temporarily_expanded.erase(group->id);
	Rebuild(file);
	return true;
}

bool SubtitleGridFolding::Toggle(AssFile& file, int source_row) {
	auto group = GroupAt(source_row);
	return group && SetCollapsed(file, source_row, !IsCollapsed(*group));
}

bool SubtitleGridFolding::Remove(AssFile& file, int source_row) {
	if (source_row < 0 || std::cmp_greater_equal(source_row, rows.size()))
		return false;
	std::unordered_set<uint64_t> ids;
	if (auto group = GroupAt(source_row))
		ids.insert(group->id);
	auto markers = Markers(file);
	for (auto reference : rows[source_row]->ExtradataIds.get()) {
		auto marker = markers.find(reference);
		if (marker != markers.end()) {
			if (auto id = ParseId(marker->second))
				ids.insert(*id);
		}
	}
	bool changed = RemoveGroups(file, ids);
	changed |= StripMarkers(file, *rows[source_row]);
	if (changed)
		Rebuild(file);
	return changed;
}

bool SubtitleGridFolding::SetAllCollapsed(AssFile& file, bool collapsed) {
	bool changed = false;
	for (auto const& group : groups) {
		if (group.collapsed == collapsed && IsCollapsed(group) == collapsed)
			continue;
		WriteBoundary(file, *rows[group.start], group.id, false, collapsed);
		WriteBoundary(file, *rows[group.end], group.id, true, collapsed);
		temporarily_expanded.erase(group.id);
		changed = true;
	}
	if (changed)
		Rebuild(file);
	return changed;
}

bool SubtitleGridFolding::Clear(AssFile& file) {
	auto markers = Markers(file);
	bool changed = false;
	for (auto& line : file.Events)
		changed |= RemoveMarkers(line, markers, [](std::string_view) { return true; });
	if (changed)
		Rebuild(file);
	return changed;
}

bool SubtitleGridFolding::StripMarkers(AssFile const& file, AssDialogue& line) {
	return RemoveMarkers(line, Markers(file), [](std::string_view) { return true; });
}

void SubtitleGridFolding::AddCopiedGroup(AssFile& file, AssDialogue& first, AssDialogue& last, bool collapsed) {
	auto id = NextGroupId(file);
	WriteBoundary(file, first, id, false, collapsed);
	WriteBoundary(file, last, id, true, collapsed);
}

bool SubtitleGridFolding::RepairStructure(AssFile& file) {
	if (groups.empty())
		return false;
	std::unordered_map<int, int> old_positions;
	for (size_t row = 0; row < row_ids.size(); ++row)
		old_positions.emplace(row_ids[row], static_cast<int>(row));
	std::unordered_map<int, int> positions;
	std::vector<int> current_ids;
	for (auto const& line : file.Events) {
		positions.emplace(line.Id, static_cast<int>(current_ids.size()));
		current_ids.push_back(line.Id);
	}
	std::unordered_set<uint64_t> invalid;
	for (auto const& group : groups) {
		auto first = positions.find(row_ids[group.start]);
		auto last = positions.find(row_ids[group.end]);
		if (first == positions.end() || last == positions.end() || first->second >= last->second) {
			invalid.insert(group.id);
			continue;
		}
		bool valid = true;
		int previous = group.start - 1;
		for (int row = first->second; row <= last->second; ++row) {
			auto old = old_positions.find(current_ids[row]);
			if (old == old_positions.end())
				continue; // A newly inserted row belongs to its surrounding boundaries.
			if (old->second < group.start || old->second > group.end || old->second <= previous) {
				valid = false;
				break;
			}
			previous = old->second;
		}
		for (int row = group.start; valid && row <= group.end; ++row) {
			auto current = positions.find(row_ids[row]);
			if (current != positions.end() && (current->second < first->second || current->second > last->second))
				valid = false;
		}
		if (!valid)
			invalid.insert(group.id);
	}
	return RemoveGroups(file, invalid);
}

bool SubtitleGridFolding::PrepareCommit(AssFile& file, int type) {
	bool changed = false;
	if (type != AssFile::COMMIT_NEW && (type & (AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_ORDER)))
		changed = RepairStructure(file);
	Rebuild(file, type == AssFile::COMMIT_NEW);
	return changed;
}
