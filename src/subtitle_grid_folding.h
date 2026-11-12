#pragma once

#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

class AssDialogue;
class AssFile;

struct SubtitleGridDisplayRow {
	AssDialogue *dialogue = nullptr;
	int source_row = -1;
	int display_row = -1;
	bool is_fold_start = false;
	bool is_fold_end = false;
	bool is_collapsed = false;
	int hidden_count = 0;
};

/// View state and persisted, non-overlapping fold boundaries for one document.
/// Cached pointers are replaced on every document reload or structural commit.
class SubtitleGridFolding {
	public:
	struct Group {
		uint64_t id;
		int start;
		int end;
		bool collapsed;
	};

	private:
	std::vector<AssDialogue *> rows;
	std::vector<int> row_ids;
	std::vector<Group> groups;
	std::vector<int> row_groups;
	std::vector<SubtitleGridDisplayRow> display_rows;
	std::vector<int> source_to_display;
	std::unordered_set<uint64_t> temporarily_expanded;

	void BuildDisplayRows();
	bool RepairStructure(AssFile& file);

	public:
	void Rebuild(AssFile& file, bool reset_temporary = false);
	void RemapPreparedRows(std::function<AssDialogue *(AssDialogue *)> const& resolve);
	/// Repair before the undo snapshot is recorded; returns whether markers changed.
	bool PrepareCommit(AssFile& file, int type);

	[[nodiscard]] std::vector<Group> const& Groups() const { return groups; }
	[[nodiscard]] Group const *GroupAt(int source_row) const;
	[[nodiscard]] bool IsCollapsed(Group const& group) const;
	[[nodiscard]] std::vector<SubtitleGridDisplayRow> const& DisplayRows() const { return display_rows; }
	[[nodiscard]] int DisplayToSource(int display_row) const;
	/// Returns -1 for hidden rows as well as rows outside the document.
	[[nodiscard]] int SourceToDisplay(int source_row) const;
	/// Expand only the view. Does not modify ExtraData or commit the document.
	bool EnsureVisible(int source_row);

	[[nodiscard]] bool CanCreate(int first, int last) const;
	bool Create(AssFile& file, int first, int last, bool collapsed = true);
	bool SetCollapsed(AssFile& file, int source_row, bool collapsed);
	bool Toggle(AssFile& file, int source_row);
	bool Remove(AssFile& file, int source_row);
	bool SetAllCollapsed(AssFile& file, bool collapsed);
	bool Clear(AssFile& file);

	/// Clipboard and duplicate operations preserve unrelated ExtraData.
	static bool StripMarkers(AssFile const& file, AssDialogue& line);
	/// Attach fresh boundaries to copies, which need not be inserted yet.
	static void AddCopiedGroup(AssFile& file, AssDialogue& first, AssDialogue& last, bool collapsed);
};
