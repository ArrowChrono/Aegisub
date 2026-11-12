#include "subtitle_history.h"

#include "ass_attachment.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_style.h"

#include <algorithm>
#include <iterator>
#include <optional>
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

struct SubtitleHistory::PreparedRecord::State {
	AssFile *file = nullptr;
	std::list<Entry> entries;
	std::unique_ptr<AssStyle> default_style;
	std::unique_ptr<AssDialogue> default_dialogue;
	std::optional<SubtitleHistoryBookmark> previous_bookmark;
	int *output_commit_id = nullptr;
	int revision = 0;
	int depth = 0;
	bool replace_previous = false;
};

SubtitleHistory::PreparedRecord::PreparedRecord() : state(std::make_unique<State>()) {}
SubtitleHistory::PreparedRecord::~PreparedRecord() = default;

std::unique_ptr<SubtitleHistory::PreparedRecord> SubtitleHistory::PrepareRecord(
	AssFile& file, AssFileCommit const& commit, CaptureBookmark const& capture_bookmark,
	int depth, EmptyDialoguePolicy empty_dialogues, EmptyStylePolicy empty_styles,
	SubtitleHistoryBookmark const *previous_bookmark) {
	if (commit.message.empty() && !undo_stack.empty())
		return {};
	auto prepared = std::make_unique<PreparedRecord>();
	prepared->state->file = &file;
	prepared->state->output_commit_id = commit.commit_id;
	prepared->state->revision = next_commit_id;
	prepared->state->depth = std::max(depth, 2);
	if (previous_bookmark)
		prepared->state->previous_bookmark = *previous_bookmark;
	prepared->state->replace_previous = next_commit_id == *commit.commit_id + 1 && redo_stack.empty() &&
										saved_commit_id + 1 != next_commit_id && !undo_stack.empty();
	if (prepared->state->replace_previous) {
		bool const dialogue_only = commit.type != AssFile::COMMIT_NEW &&
								   (commit.type & AssFile::COMMIT_DIAG_FULL) != 0 && (commit.type & ~AssFile::COMMIT_DIAG_FULL) == 0;
		if (dialogue_only) {
			prepared->state->entries.push_back(undo_stack.back());
			if (subtitle_history_detail::TryAmendDialogueSnapshot(prepared->state->entries.back().events, commit.changed_lines))
				return prepared;
			prepared->state->entries.clear();
		}
	}
	if (file.Styles.empty() && empty_styles == EmptyStylePolicy::AddDefault)
		prepared->state->default_style = std::make_unique<AssStyle>();
	if (file.Events.empty() && empty_dialogues == EmptyDialoguePolicy::AddDefault) {
		prepared->state->default_dialogue = std::make_unique<AssDialogue>();
		prepared->state->default_dialogue->Row = 0;
	}
	auto bookmark = capture_bookmark ? capture_bookmark() : SubtitleHistoryBookmark{};
	prepared->state->entries.emplace_back(file, commit.message, next_commit_id, std::move(bookmark));
	if (prepared->state->default_style)
		prepared->state->entries.back().styles.push_back(*prepared->state->default_style);
	if (prepared->state->default_dialogue)
		prepared->state->entries.back().events.push_back(*prepared->state->default_dialogue);
	return prepared;
}

void SubtitleHistory::InstallRecord(std::unique_ptr<PreparedRecord> prepared) {
	if (!prepared)
		return;
	if (prepared->state->default_style)
		prepared->state->file->Styles.push_back(*prepared->state->default_style.release());
	if (prepared->state->default_dialogue)
		prepared->state->file->Events.push_back(*prepared->state->default_dialogue.release());
	if (prepared->state->previous_bookmark && !undo_stack.empty())
		std::swap(undo_stack.back().bookmark, *prepared->state->previous_bookmark);
	if (prepared->state->replace_previous)
		undo_stack.pop_back();
	redo_stack.clear();
	undo_stack.splice(undo_stack.end(), prepared->state->entries);
	commit_id = prepared->state->revision;
	next_commit_id = prepared->state->revision + 1;
	while (undo_stack.size() > static_cast<std::size_t>(prepared->state->depth))
		undo_stack.pop_front();
	*prepared->state->output_commit_id = commit_id;
}

void SubtitleHistory::Record(AssFile& file, AssFileCommit const& commit,
							 CaptureBookmark const& capture_bookmark, int depth,
							 EmptyDialoguePolicy empty_dialogues, EmptyStylePolicy empty_styles) {
	InstallRecord(PrepareRecord(file, commit, capture_bookmark, depth, empty_dialogues, empty_styles));
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
