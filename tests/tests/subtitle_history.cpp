#include <main.h>

#include "../../src/ass_attachment.h"
#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_info.h"
#include "../../src/ass_io_core.h"
#include "../../src/ass_style.h"
#include "../../src/subtitle_history.h"

#include <libaegisub/fs.h>
#include <libaegisub/scope_exit.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

class subtitle_history : public testing::Test {
	protected:
	AssFile file;
	SubtitleHistory history;
	SubtitleHistoryBookmark bookmark;
	agi::signal::Connection connection;
	int depth = 20;
	int bookmark_captures = 0;

	subtitle_history() {
		file.Info.emplace_back("Title", "original title");
		file.Styles.push_back(*new AssStyle);
		file.Styles.front().font = "Original Font";
		file.Events.push_back(*new AssDialogue);
		file.Events.push_back(*new AssDialogue);
		file.Events.front().Text = "original first";
		file.Events.back().Text = "original second";
		file.Attachments.emplace_back(std::string("fontname: original.ttf"), AssEntryGroup::FONT);
		file.Attachments.back().AddData("!!!!");
		auto extra_id = file.AddExtradata("history-test", "original value");
		file.Events.front().ExtradataIds = std::vector<uint32_t>{extra_id};
		bookmark.selected_line_ids = {file.Events.front().Id};
		bookmark.active_line_id = file.Events.front().Id;
		connection = file.AddUndoManager([this](AssFileCommit commit) {
			history.Record(file, commit, [this] {
				++bookmark_captures;
				return bookmark; }, depth);
		});
		history.MarkSaved(1);
		file.Commit("", AssFile::COMMIT_NEW);
	}

	int Edit(std::string const& text, int amend = -1, std::string const& description = "typing") {
		file.Events.front().Text = text;
		return file.Commit(description, AssFile::COMMIT_DIAG_TEXT, amend, &file.Events.front());
	}

	void Restore(SubtitleHistoryBookmark const& restored) { bookmark = restored; }
	bool Undo() {
		return history.Undo(file, [this](SubtitleHistoryBookmark const& restored) { Restore(restored); });
	}
	bool Redo() {
		return history.Redo(file, [this](SubtitleHistoryBookmark const& restored) { Restore(restored); });
	}
};

}

TEST_F(subtitle_history, restores_payload_ids_and_bookmarks_but_not_project_properties) {
	int const first_id = file.Events.front().Id;
	int const second_id = file.Events.back().Id;
	auto const attachment = file.Attachments.front().GetEntryData();
	auto const original_extra_id = file.Extradata.front().id;
	history.UpdateActiveLine(second_id);
	history.UpdateSelection({second_id, first_id});
	history.UpdateTextSelection(12, 4, 12);

	file.Info.front().SetValue("edited title");
	file.Styles.front().font = "Edited Font";
	file.Events.front().Text = "edited first";
	file.Attachments.clear();
	file.Extradata.front().value = "edited value";
	file.AddExtradata("additional", "retained allocator");
	file.Properties.video_file = "media/retained.mkv";
	auto const next_extra_id = file.next_extradata_id;
	bookmark = {.selected_line_ids = {first_id}, .active_line_id = first_id, .insertion_point = 2, .selection_start = 1, .selection_end = 2};
	file.Commit("edit document", AssFile::COMMIT_NEW);

	ASSERT_TRUE(Undo());
	EXPECT_EQ("original title", file.Info.front().Value());
	EXPECT_EQ("Original Font", file.Styles.front().font);
	ASSERT_EQ(2u, file.Events.size());
	EXPECT_EQ(first_id, file.Events.front().Id);
	EXPECT_EQ(second_id, file.Events.back().Id);
	EXPECT_EQ(0, file.Events.front().Row);
	EXPECT_EQ(1, file.Events.back().Row);
	EXPECT_EQ("original first", file.Events.front().Text.get());
	ASSERT_EQ(1u, file.Attachments.size());
	EXPECT_EQ(attachment, file.Attachments.front().GetEntryData());
	ASSERT_EQ(1u, file.Extradata.size());
	EXPECT_EQ(original_extra_id, file.Extradata.front().id);
	EXPECT_EQ("original value", file.Extradata.front().value);
	EXPECT_EQ((std::vector<uint32_t>{original_extra_id}), file.Events.front().ExtradataIds.get());
	EXPECT_EQ("media/retained.mkv", file.Properties.video_file);
	EXPECT_EQ(next_extra_id, file.next_extradata_id);
	EXPECT_EQ((std::vector<int>{second_id, first_id}), bookmark.selected_line_ids);
	EXPECT_EQ(second_id, bookmark.active_line_id);
	EXPECT_EQ(12, bookmark.insertion_point);
	EXPECT_EQ(4, bookmark.selection_start);
	EXPECT_EQ(12, bookmark.selection_end);

	ASSERT_TRUE(Redo());
	EXPECT_EQ("edited title", file.Info.front().Value());
	EXPECT_EQ("Edited Font", file.Styles.front().font);
	EXPECT_EQ("edited first", file.Events.front().Text.get());
	EXPECT_TRUE(file.Attachments.empty());
	EXPECT_EQ(2u, file.Extradata.size());
	EXPECT_EQ((std::vector<int>{first_id}), bookmark.selected_line_ids);
	EXPECT_EQ(first_id, bookmark.active_line_id);
	EXPECT_EQ(2, bookmark.insertion_point);
	EXPECT_EQ(1, bookmark.selection_start);
	EXPECT_EQ(2, bookmark.selection_end);
}

TEST_F(subtitle_history, retains_old_objects_through_commit_observers_and_bookmark_restore) {
	Edit("old row still referenced");
	auto const *old_line = &file.Events.front();
	auto const *old_style = &file.Styles.front();
	std::vector<std::string> order;
	auto observer = agi::signal::Connection(file.AddCommitListener([&](int type, AssDialogue const *) {
		EXPECT_EQ(AssFile::COMMIT_NEW, type);
		EXPECT_EQ("old row still referenced", old_line->Text.get());
		EXPECT_EQ("Original Font", old_style->font);
		EXPECT_NE(old_line, &file.Events.front());
		EXPECT_NE(old_style, &file.Styles.front());
		EXPECT_EQ("original first", file.Events.front().Text.get());
		order.emplace_back("commit");
	}));
	ASSERT_TRUE(history.Undo(file, [&](SubtitleHistoryBookmark const& restored) {
		EXPECT_EQ((std::vector<std::string>{"commit"}), order);
		EXPECT_EQ("old row still referenced", old_line->Text.get());
		EXPECT_EQ("Original Font", old_style->font);
		EXPECT_EQ(file.Events.front().Id, restored.active_line_id);
		order.emplace_back("bookmark");
	}));
	EXPECT_EQ((std::vector<std::string>{"commit", "bookmark"}), order);
	EXPECT_EQ(1u, history.UndoDepth());
	EXPECT_TRUE(history.CanRedo());
}

TEST_F(subtitle_history, save_barrier_prevents_merge_and_new_edit_invalidates_redo) {
	int const first_edit = Edit("saved edit");
	history.MarkSaved(first_edit);
	EXPECT_FALSE(history.IsModified());
	int const second_edit = Edit("unsaved edit", first_edit);
	EXPECT_GT(second_edit, first_edit);
	EXPECT_EQ(3u, history.UndoDepth());
	EXPECT_TRUE(history.IsModified());

	ASSERT_TRUE(Undo());
	EXPECT_EQ("saved edit", file.Events.front().Text.get());
	EXPECT_EQ(first_edit, history.CurrentRevision());
	EXPECT_FALSE(history.IsModified());
	ASSERT_TRUE(Redo());
	EXPECT_EQ("unsaved edit", file.Events.front().Text.get());
	EXPECT_EQ(second_edit, history.CurrentRevision());
	EXPECT_TRUE(history.IsModified());

	ASSERT_TRUE(Undo());
	int const replacement_edit = Edit("new branch", first_edit);
	EXPECT_GT(replacement_edit, second_edit);
	EXPECT_TRUE(history.IsModified());
	EXPECT_FALSE(history.CanRedo());
	EXPECT_FALSE(Redo());
	EXPECT_EQ("new branch", file.Events.front().Text.get());
	ASSERT_TRUE(Undo());
	EXPECT_EQ("saved edit", file.Events.front().Text.get());
	EXPECT_FALSE(history.IsModified());
}

TEST_F(subtitle_history, exact_dialogue_amendments_coalesce_without_losing_other_rows) {
	int revision = Edit("first part", -1, "type phrase");
	int const captures_before_amend = bookmark_captures;
	file.Events.front().Text = "complete first";
	file.Events.back().Text = "complete second";
	std::vector<AssDialogue const *> changed = {&file.Events.back(), &file.Events.front()};
	revision = file.Commit("type phrase", AssFile::COMMIT_DIAG_TEXT, revision, nullptr, changed);
	EXPECT_EQ(captures_before_amend, bookmark_captures);
	EXPECT_EQ(2u, history.UndoDepth());
	EXPECT_EQ("type phrase", history.UndoDescription());
	EXPECT_EQ(revision, history.CurrentRevision());

	ASSERT_TRUE(Undo());
	EXPECT_EQ("original first", file.Events.front().Text.get());
	EXPECT_EQ("original second", file.Events.back().Text.get());
	EXPECT_EQ("type phrase", history.RedoDescription());
	ASSERT_TRUE(Redo());
	EXPECT_EQ("complete first", file.Events.front().Text.get());
	EXPECT_EQ("complete second", file.Events.back().Text.get());
	EXPECT_FALSE(history.CanRedo());
}

TEST_F(subtitle_history, non_dialogue_merge_replaces_complete_snapshot) {
	int const revision = Edit("first part");
	file.Styles.front().font = "Coalesced Font";
	file.Info.front().SetValue("coalesced title");
	file.Events.front().Text = "coalesced text";
	bookmark.insertion_point = 17;
	file.Commit("combined edit", AssFile::COMMIT_STYLES | AssFile::COMMIT_SCRIPTINFO | AssFile::COMMIT_DIAG_TEXT,
				revision, &file.Events.front());
	EXPECT_EQ(2u, history.UndoDepth());
	EXPECT_EQ("combined edit", history.UndoDescription());
	ASSERT_TRUE(Undo());
	EXPECT_EQ("original title", file.Info.front().Value());
	EXPECT_EQ("Original Font", file.Styles.front().font);
	ASSERT_TRUE(Redo());
	EXPECT_EQ("coalesced title", file.Info.front().Value());
	EXPECT_EQ("Coalesced Font", file.Styles.front().font);
	EXPECT_EQ("coalesced text", file.Events.front().Text.get());
	EXPECT_EQ(17, bookmark.insertion_point);
}

TEST_F(subtitle_history, depth_floor_keeps_one_undo_and_empty_notifications_do_not_record) {
	depth = 0;
	Edit("one");
	Edit("two");
	int const revision = Edit("three");
	EXPECT_EQ(2u, history.UndoDepth());
	file.Commit("", AssFile::COMMIT_NEW);
	EXPECT_EQ(revision, history.CurrentRevision());
	EXPECT_EQ(2u, history.UndoDepth());
	ASSERT_TRUE(Undo());
	EXPECT_EQ("two", file.Events.front().Text.get());
	EXPECT_FALSE(Undo());
	EXPECT_TRUE(history.UndoDescription().empty());
	ASSERT_TRUE(Redo());
	EXPECT_EQ("three", file.Events.front().Text.get());
}

TEST_F(subtitle_history, bookmark_callback_can_update_history_without_mutating_restore_value) {
	history.UpdateTextSelection(10, 3, 10);
	Edit("changed");
	ASSERT_TRUE(history.Undo(file, [&](SubtitleHistoryBookmark const& restored) {
		history.UpdateTextSelection(0, 0, 0);
		history.UpdateSelection({file.Events.back().Id});
		EXPECT_EQ(10, restored.insertion_point);
		EXPECT_EQ(3, restored.selection_start);
		EXPECT_EQ(10, restored.selection_end);
		EXPECT_EQ((std::vector<int>{file.Events.front().Id}), restored.selected_line_ids);
	}));
}

TEST(subtitle_history_empty, initial_snapshot_supplies_default_rows_and_clear_retains_revision_allocator) {
	AssFile file;
	SubtitleHistory history;
	EXPECT_FALSE(history.Undo(file, {}));
	EXPECT_FALSE(history.Redo(file, {}));
	int commit_id = -1;
	history.Record(file, {.message = "", .commit_id = &commit_id, .single_line = nullptr, .type = AssFile::COMMIT_NEW, .changed_lines = {}}, {}, 10);
	ASSERT_EQ(1u, file.Events.size());
	ASSERT_EQ(1u, file.Styles.size());
	EXPECT_EQ(0, file.Events.front().Row);
	EXPECT_EQ(1, commit_id);
	EXPECT_FALSE(history.CanUndo());
	file.Events.front().Text = "changed";
	commit_id = -1;
	history.Record(file, {.message = "edit", .commit_id = &commit_id, .single_line = nullptr, .type = AssFile::COMMIT_DIAG_TEXT, .changed_lines = {}}, {}, 10);
	int const allocated_revision = history.CurrentRevision();
	ASSERT_TRUE(history.Undo(file, [](SubtitleHistoryBookmark const&) {}));
	history.Clear();
	EXPECT_TRUE(history.Empty());
	EXPECT_FALSE(history.CanRedo());
	commit_id = -1;
	history.Record(file, {.message = "", .commit_id = &commit_id, .single_line = nullptr, .type = AssFile::COMMIT_NEW, .changed_lines = {}}, {}, 10);
	EXPECT_GT(commit_id, allocated_revision);
	EXPECT_EQ(1u, history.UndoDepth());
}

TEST(subtitle_history_empty, preserve_policy_restores_empty_baseline_and_inserted_identities) {
	AssFile file;
	SubtitleHistory history;
	SubtitleHistoryBookmark bookmark{.selected_line_ids = {}, .active_line_id = -1};
	auto capture = [&] { return bookmark; };
	auto restore = [&](SubtitleHistoryBookmark const& value) { bookmark = value; };
	int commit_id = -1;
	history.Record(file, {.message = "", .commit_id = &commit_id, .single_line = nullptr, .type = AssFile::COMMIT_NEW, .changed_lines = {}}, capture, 10, SubtitleHistory::EmptyDialoguePolicy::Preserve);
	history.MarkSaved(commit_id);
	EXPECT_TRUE(file.Events.empty());
	EXPECT_EQ(1u, file.Styles.size());
	EXPECT_FALSE(history.CanUndo());
	EXPECT_FALSE(history.IsModified());

	file.Events.push_back(*new AssDialogue);
	file.Events.push_back(*new AssDialogue);
	file.Events.front().Text = "first pasted row";
	file.Events.back().Text = "second pasted row";
	int const first_id = file.Events.front().Id;
	int const second_id = file.Events.back().Id;
	bookmark = {.selected_line_ids = {first_id, second_id}, .active_line_id = first_id};
	file.Commit("paste", AssFile::COMMIT_DIAG_ADDREM);
	commit_id = -1;
	history.Record(file, {.message = "paste", .commit_id = &commit_id, .single_line = nullptr, .type = AssFile::COMMIT_DIAG_ADDREM, .changed_lines = {}}, capture, 10, SubtitleHistory::EmptyDialoguePolicy::Preserve);
	EXPECT_TRUE(history.IsModified());
	ASSERT_EQ(2u, history.UndoDepth());

	ASSERT_TRUE(history.Undo(file, restore));
	EXPECT_TRUE(file.Events.empty());
	EXPECT_TRUE(bookmark.selected_line_ids.empty());
	EXPECT_EQ(-1, bookmark.active_line_id);
	EXPECT_FALSE(history.CanUndo());
	EXPECT_FALSE(history.IsModified());
	ASSERT_TRUE(history.Redo(file, restore));
	ASSERT_EQ(2u, file.Events.size());
	EXPECT_EQ(first_id, file.Events.front().Id);
	EXPECT_EQ(second_id, file.Events.back().Id);
	EXPECT_EQ("first pasted row", file.Events.front().Text.get());
	EXPECT_EQ("second pasted row", file.Events.back().Text.get());
	EXPECT_EQ(0, file.Events.front().Row);
	EXPECT_EQ(1, file.Events.back().Row);
	EXPECT_EQ((std::vector<int>{first_id, second_id}), bookmark.selected_line_ids);
	EXPECT_EQ(first_id, bookmark.active_line_id);
	EXPECT_TRUE(history.IsModified());
}

TEST(subtitle_history_empty, preserves_loaded_empty_sections_through_baseline_undo_and_redo) {
	auto const path = agi::fs::UniquePath(
		std::filesystem::temp_directory_path() / "aegisub-history-empty-%%%%%%%%.ass");
	auto cleanup = agi::make_scope_exit([&] {
		std::error_code error;
		std::filesystem::remove(path, error);
	});
	{
		std::ofstream source(path);
		source << "[Script Info]\nScriptType: v4.00+\nTitle: empty baseline\n"
			   << "[V4+ Styles]\n[Events]\n";
		ASSERT_TRUE(source.good());
	}
	AssFile file = ReadAssFileForCore(path, "UTF-8");
	ASSERT_TRUE(file.Styles.empty());
	ASSERT_TRUE(file.Events.empty());
	SubtitleHistory history;
	SubtitleHistoryBookmark bookmark{.selected_line_ids = {}, .active_line_id = -1};
	int captures = 0;
	auto capture = [&] {
		++captures;
		return bookmark;
	};
	auto restore = [&](SubtitleHistoryBookmark const& value) { bookmark = value; };
	auto connection = agi::signal::Connection(file.AddUndoManager([&](AssFileCommit commit) {
		history.Record(file, commit, capture, 10, SubtitleHistory::EmptyDialoguePolicy::Preserve,
					   SubtitleHistory::EmptyStylePolicy::Preserve);
	}));
	int const baseline = file.Commit("", AssFile::COMMIT_NEW);
	history.MarkSaved(baseline);
	EXPECT_TRUE(file.Styles.empty());
	EXPECT_TRUE(file.Events.empty());
	EXPECT_EQ("empty baseline", file.GetScriptInfo("Title"));
	EXPECT_FALSE(history.IsModified());
	EXPECT_FALSE(history.CanUndo());
	EXPECT_FALSE(history.CanRedo());
	file.Commit("", AssFile::COMMIT_NEW);
	EXPECT_EQ(baseline, history.CurrentRevision());
	EXPECT_EQ(1, captures);
	EXPECT_EQ(1u, history.UndoDepth());
	EXPECT_TRUE(file.Styles.empty());
	EXPECT_TRUE(file.Events.empty());

	file.Styles.push_back(*new AssStyle);
	file.Styles.back().name = "Generated";
	file.Styles.back().font = "Generated Font";
	file.Events.push_back(*new AssDialogue);
	file.Events.push_back(*new AssDialogue);
	file.Events.front().Text = "first generated row";
	file.Events.back().Text = "second generated row";
	file.Events.front().Style = "Generated";
	file.Events.back().Style = "Generated";
	int const first_id = file.Events.front().Id;
	int const second_id = file.Events.back().Id;
	bookmark = {.selected_line_ids = {first_id, second_id}, .active_line_id = second_id};
	int const edit = file.Commit("generate subtitles", AssFile::COMMIT_STYLES | AssFile::COMMIT_DIAG_ADDREM);
	EXPECT_GT(edit, baseline);
	EXPECT_TRUE(history.IsModified());
	EXPECT_EQ(2u, history.UndoDepth());

	ASSERT_TRUE(history.Undo(file, restore));
	EXPECT_TRUE(file.Styles.empty());
	EXPECT_TRUE(file.Events.empty());
	EXPECT_EQ(baseline, history.CurrentRevision());
	EXPECT_FALSE(history.IsModified());
	EXPECT_TRUE(bookmark.selected_line_ids.empty());
	EXPECT_EQ(-1, bookmark.active_line_id);
	EXPECT_EQ(2, captures);

	ASSERT_TRUE(history.Redo(file, restore));
	ASSERT_EQ(1u, file.Styles.size());
	EXPECT_EQ("Generated", file.Styles.front().name);
	EXPECT_EQ("Generated Font", file.Styles.front().font);
	ASSERT_EQ(2u, file.Events.size());
	EXPECT_EQ(first_id, file.Events.front().Id);
	EXPECT_EQ(second_id, file.Events.back().Id);
	EXPECT_EQ("first generated row", file.Events.front().Text.get());
	EXPECT_EQ("second generated row", file.Events.back().Text.get());
	EXPECT_EQ(0, file.Events.front().Row);
	EXPECT_EQ(1, file.Events.back().Row);
	EXPECT_EQ((std::vector<int>{first_id, second_id}), bookmark.selected_line_ids);
	EXPECT_EQ(second_id, bookmark.active_line_id);
	EXPECT_EQ(edit, history.CurrentRevision());
	EXPECT_TRUE(history.IsModified());
	EXPECT_EQ(2, captures);

	file.Styles.clear_and_dispose([](AssStyle *style) { delete style; });
	file.Commit("remove generated style", AssFile::COMMIT_STYLES);
	EXPECT_TRUE(file.Styles.empty());
	ASSERT_TRUE(history.Undo(file, restore));
	ASSERT_EQ(1u, file.Styles.size());
	EXPECT_EQ("Generated", file.Styles.front().name);
	ASSERT_TRUE(history.Redo(file, restore));
	EXPECT_TRUE(file.Styles.empty());
	EXPECT_EQ(2u, file.Events.size());
}
