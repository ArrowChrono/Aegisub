#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <span>
#include <string>
#include <vector>

class AssDialogue;
struct AssDialogueBase;
class AssFile;
struct AssFileCommit;

struct SubtitleHistoryBookmark {
	std::vector<int> selected_line_ids;
	int active_line_id = 0;
	int insertion_point = 0;
	int selection_start = 0;
	int selection_end = 0;
};

namespace subtitle_history_detail {
bool TryAmendDialogueSnapshot(std::vector<AssDialogueBase>& snapshot,
							  std::span<AssDialogue const *const> changed_lines);
}

// A document's history, independent of editor controls and file I/O. The host
// forwards AssFile commits and bookmarks, and restores its own UI after undo.
class SubtitleHistory {
	struct Entry;
	std::list<Entry> undo_stack;
	std::list<Entry> redo_stack;
	int commit_id = 0;
	int saved_commit_id = 0;
	int next_commit_id = 1;

	public:
	using CaptureBookmark = std::function<SubtitleHistoryBookmark()>;
	using RestoreBookmark = std::function<void(SubtitleHistoryBookmark const&)>;
	enum class EmptyDialoguePolicy { AddDefault,
									 Preserve };
	enum class EmptyStylePolicy { AddDefault,
								  Preserve };

	SubtitleHistory();
	~SubtitleHistory();
	SubtitleHistory(SubtitleHistory const&) = delete;
	SubtitleHistory& operator=(SubtitleHistory const&) = delete;

	// Preserve the monotonic revision allocator when opening another document.
	void Clear();
	// Hosts which allow empty loaded event lists can retain that baseline for
	// Undo. The default keeps the existing wx normalization behavior.
	void Record(AssFile& file, AssFileCommit const& commit,
				CaptureBookmark const& capture_bookmark, int depth,
				EmptyDialoguePolicy empty_dialogues = EmptyDialoguePolicy::AddDefault,
				EmptyStylePolicy empty_styles = EmptyStylePolicy::AddDefault);
	// Empty COMMIT_NEW notifications precede bookmark restoration. Replaced
	// dialogue/style objects survive both the notification and restore callback.
	bool Undo(AssFile& file, RestoreBookmark const& restore_bookmark);
	bool Redo(AssFile& file, RestoreBookmark const& restore_bookmark);

	void UpdateActiveLine(int line_id);
	void UpdateSelection(std::vector<int> selected_line_ids);
	void UpdateTextSelection(int insertion_point, int selection_start, int selection_end);
	void MarkSaved(int revision) { saved_commit_id = revision; }

	[[nodiscard]] bool Empty() const;
	[[nodiscard]] std::size_t UndoDepth() const;
	[[nodiscard]] bool CanUndo() const;
	[[nodiscard]] bool CanRedo() const;
	[[nodiscard]] bool IsModified() const { return commit_id != saved_commit_id; }
	[[nodiscard]] int CurrentRevision() const { return commit_id; }
	[[nodiscard]] int SavedRevision() const { return saved_commit_id; }
	[[nodiscard]] std::string UndoDescription() const;
	[[nodiscard]] std::string RedoDescription() const;
};
