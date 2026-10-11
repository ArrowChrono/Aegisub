#include "subtitle_history.h"

#include "ass_attachment.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_style.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace subtitle_history_detail {

bool TryAmendDialogueSnapshot(std::vector<AssDialogueBase>& snapshot,
							  std::span<AssDialogue const *const> changed_lines) {
	if (changed_lines.empty())
		return false;

	for (auto const *line : changed_lines) {
		if (!line || line->Row < 0 || static_cast<std::size_t>(line->Row) >= snapshot.size())
			return false;
		auto const& previous = snapshot[static_cast<std::size_t>(line->Row)];
		if (previous.Row != line->Row || previous.Id != line->Id)
			return false;
	}
	for (auto const *line : changed_lines)
		snapshot[static_cast<std::size_t>(line->Row)] = *line;
	return true;
}

}

struct SubtitleHistory::Entry {
	std::string description;
	int commit_id;
	std::vector<AssInfo> script_info;
	std::vector<AssStyle> styles;
	std::vector<AssDialogueBase> events;
	std::vector<AssAttachment> attachments;
	std::vector<ExtradataEntry> extradata;
	SubtitleHistoryBookmark bookmark;

	Entry(AssFile const& file, std::string const& description, int commit_id,
		  SubtitleHistoryBookmark bookmark)
		: description(description), commit_id(commit_id), script_info(file.Info),
		  styles(file.Styles.begin(), file.Styles.end()), events(file.Events.begin(), file.Events.end()),
		  attachments(file.Attachments), extradata(file.Extradata), bookmark(std::move(bookmark)) {}

	void Apply(AssFile& file, RestoreBookmark const& restore_bookmark) const {
		// Construct before replacing live state. AssDialogueBase preserves IDs,
		// unlike AssDialogue's copy constructor, which creates a new identity.
		AssFile replacement;
		replacement.Info = script_info;
		for (auto const& style : styles)
			replacement.Styles.push_back(*new AssStyle(style));
		for (auto const& event : events)
			replacement.Events.push_back(*new AssDialogue(event));
		replacement.Attachments = attachments;
		replacement.Extradata = extradata;
		// Selection notifications can update this entry during restoration.
		auto restored_bookmark = bookmark;

		// Do not replace project/media properties, transient fonts, the monotonic
		// extradata allocator, or signal connections: these are not undo state.
		replacement.Info.swap(file.Info);
		replacement.Styles.swap(file.Styles);
		replacement.Events.swap(file.Events);
		replacement.Attachments.swap(file.Attachments);
		replacement.Extradata.swap(file.Extradata);
		file.Commit("", AssFile::COMMIT_NEW);
		restore_bookmark(restored_bookmark);
		// replacement owns the old rows until all commit and UI observers finish.
	}
};

SubtitleHistory::SubtitleHistory() = default;
SubtitleHistory::~SubtitleHistory() = default;

void SubtitleHistory::Clear() {
	undo_stack.clear();
	redo_stack.clear();
}

void SubtitleHistory::Record(AssFile& file, AssFileCommit const& commit,
							 CaptureBookmark const& capture_bookmark, int depth,
							 EmptyDialoguePolicy empty_dialogues, EmptyStylePolicy empty_styles) {
	if (commit.message.empty() && !undo_stack.empty())
		return;

	commit_id = next_commit_id++;
	if (commit_id == *commit.commit_id + 1 && redo_stack.empty() && saved_commit_id + 1 != commit_id && !undo_stack.empty()) {
		bool const dialogue_only = commit.type != AssFile::COMMIT_NEW && (commit.type & AssFile::COMMIT_DIAG_FULL) != 0 && (commit.type & ~AssFile::COMMIT_DIAG_FULL) == 0;
		if (dialogue_only && subtitle_history_detail::TryAmendDialogueSnapshot(
								 undo_stack.back().events, commit.changed_lines)) {
			*commit.commit_id = commit_id;
			return;
		}
		undo_stack.pop_back();
	}

	if (file.Styles.empty() && empty_styles == EmptyStylePolicy::AddDefault)
		file.Styles.push_back(*new AssStyle);
	if (file.Events.empty() && empty_dialogues == EmptyDialoguePolicy::AddDefault) {
		file.Events.push_back(*new AssDialogue);
		file.Events.back().Row = 0;
	}
	redo_stack.clear();
	undo_stack.emplace_back(file, commit.message, commit_id,
							capture_bookmark ? capture_bookmark() : SubtitleHistoryBookmark{});
	while (undo_stack.size() > static_cast<std::size_t>(std::max(depth, 2)))
		undo_stack.pop_front();
	*commit.commit_id = commit_id;
}

bool SubtitleHistory::Undo(AssFile& file, RestoreBookmark const& restore_bookmark) {
	if (!CanUndo())
		return false;
	redo_stack.splice(redo_stack.end(), undo_stack, std::prev(undo_stack.end()));
	commit_id = undo_stack.back().commit_id;
	undo_stack.back().Apply(file, restore_bookmark);
	return true;
}

bool SubtitleHistory::Redo(AssFile& file, RestoreBookmark const& restore_bookmark) {
	if (!CanRedo())
		return false;
	undo_stack.splice(undo_stack.end(), redo_stack, std::prev(redo_stack.end()));
	commit_id = undo_stack.back().commit_id;
	undo_stack.back().Apply(file, restore_bookmark);
	return true;
}

void SubtitleHistory::UpdateActiveLine(int line_id) {
	if (!undo_stack.empty())
		undo_stack.back().bookmark.active_line_id = line_id;
}

void SubtitleHistory::UpdateSelection(std::vector<int> selected_line_ids) {
	if (!undo_stack.empty())
		undo_stack.back().bookmark.selected_line_ids = std::move(selected_line_ids);
}

void SubtitleHistory::UpdateTextSelection(int insertion_point, int selection_start, int selection_end) {
	if (!undo_stack.empty()) {
		auto& bookmark = undo_stack.back().bookmark;
		bookmark.insertion_point = insertion_point;
		bookmark.selection_start = selection_start;
		bookmark.selection_end = selection_end;
	}
}

bool SubtitleHistory::Empty() const { return undo_stack.empty(); }
std::size_t SubtitleHistory::UndoDepth() const { return undo_stack.size(); }
bool SubtitleHistory::CanUndo() const { return undo_stack.size() > 1; }
bool SubtitleHistory::CanRedo() const { return !redo_stack.empty(); }
std::string SubtitleHistory::UndoDescription() const { return CanUndo() ? undo_stack.back().description : std::string(); }
std::string SubtitleHistory::RedoDescription() const { return CanRedo() ? redo_stack.back().description : std::string(); }
