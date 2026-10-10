#include <main.h>

#include "../../src/subtitle_edit_ops.h"

#include <libaegisub/ass/dialogue_parser.h>

#include <memory>
#include <utility>
#include <vector>

TEST(subtitle_edit_ops, character_position_ignores_complete_blocks_at_interior_carets) {
	using aegisub::subtitle_edit_ops::CharacterPos;
	for (int pos = 3; pos <= 13; ++pos) {
		SCOPED_TRACE(pos);
		EXPECT_EQ(3u, CharacterPos("ABC{\\b1}{\\i1}DEF", pos));
	}
	EXPECT_EQ(4u, CharacterPos("ABC{\\b1}{\\i1}DEF", 14));
	EXPECT_EQ(2u, CharacterPos("\xe4\xb8\xad\xf0\x9f\x98\x80{\\b1}{\\i1}\xc3\xa9Z", 15));
	EXPECT_EQ(1u, CharacterPos("\xe4\xb8\xad{\\b1}{\\i1}\xf0\x9f\x98\x80\xc3\xa9", 11));
	EXPECT_EQ(1u, CharacterPos("A{x{y}B", 5));
}

TEST(subtitle_edit_ops, character_position_preserves_graphemes_literals_and_clamping) {
	using aegisub::subtitle_edit_ops::CharacterPos;
	EXPECT_EQ(1u, CharacterPos("e\xcc\x81{\\b1}Z", 6));
	EXPECT_EQ(2u, CharacterPos("e{\\b1}\xcc\x81", 8));
	EXPECT_EQ(1u, CharacterPos("\\{\\b1}Z", 4));
	EXPECT_EQ(5u, CharacterPos("ABC{\\b1", 5));
	EXPECT_EQ(7u, CharacterPos("ABC{\\b1", 100));
	EXPECT_EQ(0u, CharacterPos("ABC{\\b1}", -1));
	EXPECT_EQ(0u, CharacterPos("", 100));
	EXPECT_EQ(6u, CharacterPos("ABC{\\b1}{\\i1}DEF", 100));

	AssDialogue shorter;
	shorter.Text = "{\\i1}X";
	auto const mapped = aegisub::subtitle_edit_ops::RemapPosForLine(&shorter, 2);
	EXPECT_EQ(1, mapped.plain);
	EXPECT_EQ(6, mapped.raw);
}

namespace {

struct edit_fixture {
	std::vector<std::unique_ptr<AssDialogue>> storage;

	AssDialogue *AddLine(int start, int end, std::string text) {
		storage.push_back(std::make_unique<AssDialogue>());
		auto *line = storage.back().get();
		line->Start = start;
		line->End = end;
		line->Text = std::move(text);
		return line;
	}
};

struct marked_text {
	std::string text;
	int caret = 0;
};

marked_text UnmarkCaret(std::string marked) {
	auto const pos = marked.find('|');
	EXPECT_NE(std::string::npos, pos);
	marked.erase(pos, 1);
	return {std::move(marked), static_cast<int>(pos)};
}

std::string ApplyAutoClose(std::string marked, aegisub::subtitle_edit_ops::AutoCloseKey key) {
	auto input = UnmarkCaret(std::move(marked));
	auto edit = aegisub::subtitle_edit_ops::BuildAutoCloseEdit(input.text, input.caret, input.caret, key);
	EXPECT_TRUE(edit.handled);

	auto text = aegisub::subtitle_edit_ops::ReplaceRangeWithText(input.text, edit.replace_start, edit.replace_end, edit.replacement);
	text.insert(static_cast<size_t>(edit.caret), "|");
	return text;
}

std::vector<int> WalkHomeBlocks(std::string const& text, int start_pos, bool karaoke_templater = false) {
	auto tokens = agi::ass::TokenizeDialogueBody(text, karaoke_templater);
	agi::ass::SplitWords(text, tokens);

	std::vector<int> positions;
	int pos = start_pos;
	while (true) {
		int next = aegisub::subtitle_edit_ops::GetPreviousBlockStart(tokens, pos);
		if (next == pos && !positions.empty())
			break;
		positions.push_back(next);
		if (next == pos)
			break;
		pos = next;
	}
	return positions;
}

std::vector<int> WalkEndBlocks(std::string const& text, int start_pos, bool karaoke_templater = false) {
	auto tokens = agi::ass::TokenizeDialogueBody(text, karaoke_templater);
	agi::ass::SplitWords(text, tokens);

	std::vector<int> positions;
	int pos = start_pos;
	while (true) {
		int next = aegisub::subtitle_edit_ops::GetNextBlockEnd(tokens, pos);
		if (next == pos && !positions.empty())
			break;
		positions.push_back(next);
		if (next == pos)
			break;
		pos = next;
	}
	return positions;
}

/// Tokenize `marked` with the double-click position marked by '|' and return the
/// text GetBoundsOfTagAtPosition would select ("" when it selects nothing).
std::string TagAtCaret(std::string marked, bool karaoke_templater = false) {
	auto input = UnmarkCaret(std::move(marked));
	auto tokens = agi::ass::TokenizeDialogueBody(input.text, karaoke_templater);
	agi::ass::SplitWords(input.text, tokens);

	auto const bounds = aegisub::subtitle_edit_ops::GetBoundsOfTagAtPosition(tokens, input.caret);
	return input.text.substr(static_cast<size_t>(bounds.first), static_cast<size_t>(bounds.second));
}

/// Tokenize `marked` with the double-click position marked by '|' and return the
/// text GetBoundsOfTagNameAtPosition would select ("" when it selects nothing).
std::string TagNameAtCaret(std::string marked) {
	auto input = UnmarkCaret(std::move(marked));
	auto tokens = agi::ass::TokenizeDialogueBody(input.text);
	agi::ass::SplitWords(input.text, tokens);

	auto const bounds = aegisub::subtitle_edit_ops::GetBoundsOfTagNameAtPosition(tokens, input.caret);
	return input.text.substr(static_cast<size_t>(bounds.first), static_cast<size_t>(bounds.second));
}

struct planned_tag_text {
	std::string selection;
	std::pair<int, int> repeat_tag_name_bounds;
};

planned_tag_text PlanTagAtCaret(std::string marked, std::pair<int, int> repeat_tag_name_bounds = {-1, 0}) {
	auto input = UnmarkCaret(std::move(marked));
	auto tokens = agi::ass::TokenizeDialogueBody(input.text);
	agi::ass::SplitWords(input.text, tokens);

	auto const plan = aegisub::subtitle_edit_ops::PlanTagDoubleClick(
		input.text, tokens, input.caret, repeat_tag_name_bounds);
	return {
		input.text.substr(static_cast<size_t>(plan.selection.first), static_cast<size_t>(plan.selection.second)),
		plan.repeat_tag_name_bounds
	};
}

/// Apply one block nudge to `marked` (caret marked by '|') and return the
/// resulting text with the caret marked again ("" when nothing was movable).
std::string NudgeBlock(std::string marked, aegisub::subtitle_edit_ops::BlockMoveDirection direction) {
	auto input = UnmarkCaret(std::move(marked));
	auto tokens = agi::ass::TokenizeDialogueBody(input.text);

	auto const edit = aegisub::subtitle_edit_ops::MoveBlockAtPosition(
		input.text, tokens, input.caret, direction);
	if (!edit.handled)
		return "";

	auto text = aegisub::subtitle_edit_ops::ReplaceRangeWithText(
		input.text, edit.replace_start, edit.replace_end, edit.replacement);
	int caret = input.caret;
	if (caret >= edit.block_start && caret <= edit.block_end)
		caret += edit.delta;
	text.insert(static_cast<size_t>(caret), "|");
	return text;
}

std::string NudgeLeft(std::string marked) {
	return NudgeBlock(std::move(marked), aegisub::subtitle_edit_ops::BlockMoveDirection::Left);
}

std::string NudgeRight(std::string marked) {
	return NudgeBlock(std::move(marked), aegisub::subtitle_edit_ops::BlockMoveDirection::Right);
}

/// Tokenize `marked` with the double-click position marked by '|' and return the
/// text GetBoundsOfEscapeAtPosition would select ("" when it selects nothing).
std::string EscapeAtCaret(std::string marked) {
	auto input = UnmarkCaret(std::move(marked));
	auto tokens = agi::ass::TokenizeDialogueBody(input.text);
	agi::ass::SplitWords(input.text, tokens);

	auto const bounds = aegisub::subtitle_edit_ops::GetBoundsOfEscapeAtPosition(tokens, input.caret);
	return input.text.substr(static_cast<size_t>(bounds.first), static_cast<size_t>(bounds.second));
}

}

TEST(subtitle_edit_ops, join_selection_into_first_adds_karaoke_tags_and_extends_end) {
	edit_fixture fixture;
	auto *first = fixture.AddLine(100, 1100, "Hello");
	auto *second = fixture.AddLine(1200, 2000, "World");

	ASSERT_TRUE(aegisub::subtitle_edit_ops::JoinSelectionIntoFirst(
		{first, second},
		aegisub::subtitle_edit_ops::JoinMode::Karaoke));

	EXPECT_EQ("{\\k100}Hello{\\k80}World", first->Text.get());
	EXPECT_EQ(2000, static_cast<int>(first->End));
}

TEST(subtitle_edit_ops, join_selection_into_first_can_keep_first_text) {
	edit_fixture fixture;
	auto *first = fixture.AddLine(100, 1100, "Hello");
	auto *second = fixture.AddLine(1200, 2000, "World");

	ASSERT_TRUE(aegisub::subtitle_edit_ops::JoinSelectionIntoFirst(
		{first, second},
		aegisub::subtitle_edit_ops::JoinMode::KeepFirst));

	EXPECT_EQ("Hello", first->Text.get());
	EXPECT_EQ(2000, static_cast<int>(first->End));
}

TEST(subtitle_edit_ops, recombine_selection_trims_edges_and_marks_duplicate_lines_for_removal) {
	edit_fixture fixture;
	auto *first = fixture.AddLine(100, 500, "Hello");
	auto *second = fixture.AddLine(600, 900, " Hello\\N");

	auto result = aegisub::subtitle_edit_ops::RecombineSelection({first, second});

	ASSERT_EQ(1u, result.lines_to_remove.size());
	EXPECT_EQ(first, result.lines_to_remove[0]);
	EXPECT_EQ("Hello", second->Text.get());
	EXPECT_EQ(100, static_cast<int>(second->Start));
	EXPECT_EQ(900, static_cast<int>(second->End));
}

TEST(subtitle_edit_ops, recombine_selection_can_trim_suffix_from_previous_line) {
	edit_fixture fixture;
	auto *first = fixture.AddLine(100, 500, "HelloWorld");
	auto *second = fixture.AddLine(600, 900, "World");

	auto result = aegisub::subtitle_edit_ops::RecombineSelection({first, second});

	EXPECT_TRUE(result.lines_to_remove.empty());
	EXPECT_EQ("Hello", first->Text.get());
	EXPECT_EQ(100, static_cast<int>(second->Start));
	EXPECT_EQ(900, static_cast<int>(second->End));
}

TEST(subtitle_edit_ops, split_text_at_position_trims_both_halves) {
	auto split = aegisub::subtitle_edit_ops::SplitTextAtPosition("alpha beta", 5);

	EXPECT_EQ("alpha", split.first);
	EXPECT_EQ("beta", split.second);
}

TEST(subtitle_edit_ops, split_text_at_position_trims_unicode_boundaries_on_both_halves) {
	std::string const boundaries = "\xC2\xA0\xE3\x80\x80\xEF\xBB\xBF ";
	std::string text = "alpha" + boundaries;
	auto const split_position = static_cast<int>(text.size());
	text += boundaries + "beta";

	auto split = aegisub::subtitle_edit_ops::SplitTextAtPosition(text, split_position);

	EXPECT_EQ("alpha", split.first);
	EXPECT_EQ("beta", split.second);
}

TEST(subtitle_edit_ops, estimate_split_time_uses_text_length_ratio) {
	auto split = aegisub::subtitle_edit_ops::EstimateSplitTime(1000, 2000, "abc", "de");

	ASSERT_TRUE(split.has_value());
	EXPECT_EQ(1600, *split);
	EXPECT_FALSE(aegisub::subtitle_edit_ops::EstimateSplitTime(1000, 2000, "", "").has_value());
}

TEST(subtitle_edit_ops, build_tag_only_text_keeps_non_plain_blocks) {
	AssDialogue line;
	line.Text = "{\\i1}Hello{note}";

	EXPECT_EQ("{\\i1}{note}", aegisub::subtitle_edit_ops::BuildTagOnlyText(line));
}

TEST(subtitle_edit_ops, replace_range_with_text_clamps_invalid_ranges) {
	EXPECT_EQ("Z", aegisub::subtitle_edit_ops::ReplaceRangeWithText("abcde", -3, 99, "Z"));
	EXPECT_EQ("aXYde", aegisub::subtitle_edit_ops::ReplaceRangeWithText("abcde", 1, 3, "XY"));
}

TEST(subtitle_edit_ops, minimal_text_change_is_the_changed_tag_argument) {
	auto const change = aegisub::subtitle_edit_ops::FindMinimalTextChange(
		"{\\pos(100,200)}Text",
		"{\\pos(101,200)}Text");
	EXPECT_TRUE(change.changed);
	EXPECT_EQ(8u, change.old_begin);
	EXPECT_EQ(9u, change.old_end);
	EXPECT_EQ(8u, change.new_begin);
	EXPECT_EQ(9u, change.new_end);
}

TEST(subtitle_edit_ops, minimal_text_change_handles_insert_delete_and_equal_text) {
	auto const inserted = aegisub::subtitle_edit_ops::FindMinimalTextChange("abcd", "abXYcd");
	EXPECT_EQ(2u, inserted.old_begin);
	EXPECT_EQ(2u, inserted.old_end);
	EXPECT_EQ(2u, inserted.new_begin);
	EXPECT_EQ(4u, inserted.new_end);

	auto const deleted = aegisub::subtitle_edit_ops::FindMinimalTextChange("abXYcd", "abcd");
	EXPECT_EQ(2u, deleted.old_begin);
	EXPECT_EQ(4u, deleted.old_end);
	EXPECT_EQ(2u, deleted.new_begin);
	EXPECT_EQ(2u, deleted.new_end);

	auto const equal = aegisub::subtitle_edit_ops::FindMinimalTextChange("same", "same");
	EXPECT_FALSE(equal.changed);
	EXPECT_EQ(4u, equal.old_begin);
	EXPECT_EQ(4u, equal.old_end);
}

TEST(subtitle_edit_ops, minimal_text_change_never_splits_utf8_codepoints) {
	auto const shared_lead = aegisub::subtitle_edit_ops::FindMinimalTextChange(
		"A\xC3\xA9Z",
		"A\xC3\xAAZ");
	EXPECT_EQ(1u, shared_lead.old_begin);
	EXPECT_EQ(3u, shared_lead.old_end);
	EXPECT_EQ(1u, shared_lead.new_begin);
	EXPECT_EQ(3u, shared_lead.new_end);

	auto const shared_tail = aegisub::subtitle_edit_ops::FindMinimalTextChange(
		"\xE4\xB8\x80",
		"\xE5\x80\x80");
	EXPECT_EQ(0u, shared_tail.old_begin);
	EXPECT_EQ(3u, shared_tail.old_end);
	EXPECT_EQ(0u, shared_tail.new_begin);
	EXPECT_EQ(3u, shared_tail.new_end);
}

TEST(subtitle_edit_ops, autoclose_inserts_and_skips_override_braces) {
	using aegisub::subtitle_edit_ops::AutoCloseKey;

	EXPECT_EQ("a{|}b", ApplyAutoClose("a|b", AutoCloseKey::OpenBrace));
	EXPECT_EQ("a{}|b", ApplyAutoClose("a{|}b", AutoCloseKey::CloseBrace));
	EXPECT_EQ("|", ApplyAutoClose("{|}", AutoCloseKey::Backspace));
}

TEST(subtitle_edit_ops, autoclose_parentheses_only_inside_override_blocks) {
	using aegisub::subtitle_edit_ops::AutoCloseKey;

	EXPECT_EQ("{\\pos(|)}", ApplyAutoClose("{\\pos|}", AutoCloseKey::OpenParen));
	EXPECT_EQ("{\\pos()|}", ApplyAutoClose("{\\pos(|)}", AutoCloseKey::CloseParen));
	EXPECT_EQ("{\\pos|}", ApplyAutoClose("{\\pos(|)}", AutoCloseKey::Backspace));

	EXPECT_FALSE(aegisub::subtitle_edit_ops::BuildAutoCloseEdit("plain text", 5, 5, AutoCloseKey::OpenParen).handled);
	EXPECT_FALSE(aegisub::subtitle_edit_ops::BuildAutoCloseEdit("plain ) text", 6, 6, AutoCloseKey::CloseParen).handled);
}

TEST(subtitle_edit_ops, autoclose_wraps_selected_text_with_braces) {
	using aegisub::subtitle_edit_ops::AutoCloseKey;

	auto edit = aegisub::subtitle_edit_ops::BuildAutoCloseEdit("abc", 0, 1, AutoCloseKey::OpenBrace);
	EXPECT_TRUE(edit.handled);
	EXPECT_EQ(0, edit.replace_start);
	EXPECT_EQ(1, edit.replace_end);
	EXPECT_EQ("{a}", edit.replacement);
	EXPECT_EQ(3, edit.caret);

	edit = aegisub::subtitle_edit_ops::BuildAutoCloseEdit("abc", 1, 3, AutoCloseKey::OpenBrace);
	EXPECT_TRUE(edit.handled);
	EXPECT_EQ(1, edit.replace_start);
	EXPECT_EQ(3, edit.replace_end);
	EXPECT_EQ("{bc}", edit.replacement);
	EXPECT_EQ(5, edit.caret);
}

TEST(subtitle_edit_ops, autoclose_ignores_selected_text_for_parentheses) {
	using aegisub::subtitle_edit_ops::AutoCloseKey;

	EXPECT_FALSE(aegisub::subtitle_edit_ops::BuildAutoCloseEdit("{\\pos}", 1, 5, AutoCloseKey::OpenParen).handled);
}

TEST(subtitle_edit_ops, text_drag_preview_moves_selection_in_both_directions) {
	using aegisub::subtitle_edit_ops::BuildTextDragPreview;

	auto preview = BuildTextDragPreview("one TWO three", 4, 7, 13, false);
	EXPECT_TRUE(preview.changed);
	EXPECT_EQ("one  threeTWO", preview.text);
	EXPECT_EQ(10, preview.selection_start);
	EXPECT_EQ(13, preview.selection_end);

	preview = BuildTextDragPreview("one TWO three", 4, 7, 0, false);
	EXPECT_TRUE(preview.changed);
	EXPECT_EQ("TWOone  three", preview.text);
	EXPECT_EQ(0, preview.selection_start);
	EXPECT_EQ(3, preview.selection_end);

	preview = BuildTextDragPreview("\xE7\x94\xB2T\xE4\xB9\x99", 3, 4, 7, false);
	EXPECT_TRUE(preview.changed);
	EXPECT_EQ("\xE7\x94\xB2\xE4\xB9\x99T", preview.text);
	EXPECT_EQ(6, preview.selection_start);
	EXPECT_EQ(7, preview.selection_end);
}

TEST(subtitle_edit_ops, text_drag_preview_handles_copy_and_rejected_internal_drops) {
	using aegisub::subtitle_edit_ops::BuildTextDragPreview;

	auto preview = BuildTextDragPreview("one TWO", 4, 7, 0, true);
	EXPECT_TRUE(preview.changed);
	EXPECT_EQ("TWOone TWO", preview.text);
	EXPECT_EQ(0, preview.selection_start);
	EXPECT_EQ(3, preview.selection_end);

	preview = BuildTextDragPreview("one TWO", 4, 7, 5, false);
	EXPECT_FALSE(preview.changed);
	EXPECT_EQ("one TWO", preview.text);
	EXPECT_EQ(4, preview.selection_start);
	EXPECT_EQ(7, preview.selection_end);

	preview = BuildTextDragPreview("one TWO", 4, 7, 5, true);
	EXPECT_FALSE(preview.changed);
	EXPECT_EQ("one TWO", preview.text);
}

TEST(subtitle_edit_ops, text_drag_preview_maps_live_positions_back_to_source) {
	using aegisub::subtitle_edit_ops::MapTextDragPreviewPosition;

	// "one TWO three" -> "TWOone  three"
	EXPECT_EQ(0, MapTextDragPreviewPosition(2, 13, 4, 7, 0, false, true));
	EXPECT_EQ(2, MapTextDragPreviewPosition(5, 13, 4, 7, 0, false, true));
	EXPECT_EQ(7, MapTextDragPreviewPosition(7, 13, 4, 7, 0, false, true));

	// "one TWO three" -> "one  threeTWO"
	EXPECT_EQ(8, MapTextDragPreviewPosition(5, 13, 4, 7, 13, false, true));
	EXPECT_EQ(13, MapTextDragPreviewPosition(11, 13, 4, 7, 13, false, true));

	// "one TWO" -> "TWOone TWO"
	EXPECT_EQ(0, MapTextDragPreviewPosition(2, 7, 4, 7, 0, true, true));
	EXPECT_EQ(2, MapTextDragPreviewPosition(5, 7, 4, 7, 0, true, true));
}

TEST(subtitle_edit_ops, home_blocks_step_over_ass_blocks) {
	std::string const text = "hello{\\i1}world";

	EXPECT_EQ((std::vector<int>{10, 5, 0}), WalkHomeBlocks(text, text.size()));
	EXPECT_EQ(5, aegisub::subtitle_edit_ops::GetPreviousBlockStart(
		agi::ass::TokenizeDialogueBody(text),
		10));
}

TEST(subtitle_edit_ops, home_blocks_treat_plain_text_as_one_block) {
	EXPECT_EQ((std::vector<int>{0}), WalkHomeBlocks("hello there", 11));
}

TEST(subtitle_edit_ops, home_blocks_stop_at_zero) {
	EXPECT_EQ((std::vector<int>{0}), WalkHomeBlocks("hello", 0));
}

TEST(subtitle_edit_ops, home_blocks_include_line_breaks) {
	EXPECT_EQ((std::vector<int>{7, 5, 0}), WalkHomeBlocks("hello\\Nthere", 12));
}

TEST(subtitle_edit_ops, end_blocks_step_over_ass_blocks) {
	std::string const text = "hello{\\i1}world";

	EXPECT_EQ((std::vector<int>{5, 10, 15}), WalkEndBlocks(text, 0));
	EXPECT_EQ(10, aegisub::subtitle_edit_ops::GetNextBlockEnd(
		agi::ass::TokenizeDialogueBody(text),
		5));
}

TEST(subtitle_edit_ops, end_blocks_treat_plain_text_as_one_block) {
	EXPECT_EQ((std::vector<int>{11}), WalkEndBlocks("hello there", 0));
}

TEST(subtitle_edit_ops, end_blocks_stay_at_end) {
	EXPECT_EQ((std::vector<int>{5}), WalkEndBlocks("hello", 5));
}

TEST(subtitle_edit_ops, end_blocks_include_line_breaks) {
	EXPECT_EQ((std::vector<int>{5, 7, 12}), WalkEndBlocks("hello\\Nthere", 0));
}

TEST(subtitle_edit_ops, nudge_walks_an_override_block_one_character_at_a_time) {
	EXPECT_EQ("hell{\\|i1}oworld", NudgeLeft("hello{\\|i1}world"));
	EXPECT_EQ("hellow{\\|i1}orld", NudgeRight("hello{\\|i1}world"));
}

TEST(subtitle_edit_ops, nudge_carries_the_caret_along_from_either_edge_of_the_block) {
	EXPECT_EQ("hellow{\\i1}|orld", NudgeRight("hello{\\i1}|world"));
	EXPECT_EQ("hellow|{\\i1}orld", NudgeRight("hello|{\\i1}world"));
	EXPECT_EQ("hell{\\i1}|oworld", NudgeLeft("hello{\\i1}|world"));
}

TEST(subtitle_edit_ops, nudge_moves_line_break_escapes_too) {
	EXPECT_EQ("hell\\|Noworld", NudgeLeft("hello\\|Nworld"));
	EXPECT_EQ("hellow\\|Norld", NudgeRight("hello\\|Nworld"));
}

TEST(subtitle_edit_ops, nudge_steps_over_a_neighbouring_block_whole) {
	// Landing between the braces of the neighbour, or between the two bytes of
	// an escape, would leave a line no further nudge could continue from.
	EXPECT_EQ("{\\|i1}{\\b1}", NudgeLeft("{\\b1}{\\|i1}"));
	EXPECT_EQ("{\\b1}{\\|i1}", NudgeRight("{\\|i1}{\\b1}"));
	EXPECT_EQ("{\\|i1}\\N", NudgeLeft("\\N{\\|i1}"));
	EXPECT_EQ("\\N{\\|i1}", NudgeRight("{\\|i1}\\N"));
}

TEST(subtitle_edit_ops, nudge_steps_over_whole_utf8_codepoints) {
	EXPECT_EQ("\xe6\x97\xa5{\\|i1}\xe6\x9c\xac\xe8\xaa\x9e",
		NudgeLeft("\xe6\x97\xa5\xe6\x9c\xac{\\|i1}\xe8\xaa\x9e"));
	EXPECT_EQ("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e{\\|i1}",
		NudgeRight("\xe6\x97\xa5\xe6\x9c\xac{\\|i1}\xe8\xaa\x9e"));
}

TEST(subtitle_edit_ops, nudge_keeps_backslash_runs_glued_to_what_they_escape) {
	// Splitting the run would newly escape the block's own '{'.
	EXPECT_EQ("a{\\|i1}\\\\b", NudgeLeft("a\\\\{\\|i1}b"));
	EXPECT_EQ("a\\\\b{\\|i1}", NudgeRight("a\\\\{\\|i1}b"));
	// A literal '\{' hops whole: leaving its '{' bare would open a block.
	EXPECT_EQ("\\{{\\|i1}x", NudgeRight("{\\|i1}\\{x"));
	// That result is the documented dead end: the lexer reads the block after
	// an escaped brace as an error region, so it stops being movable.
	EXPECT_EQ("", NudgeRight("\\{{\\|i1}x"));
}

TEST(subtitle_edit_ops, nudge_does_nothing_without_a_block_under_the_caret) {
	EXPECT_EQ("", NudgeLeft("hel|lo"));
	EXPECT_EQ("", NudgeRight("hel|lo"));
	EXPECT_EQ("", NudgeLeft("hello{\\i1}wo|rld"));
	// An unterminated brace has no closing brace to carry along.
	EXPECT_EQ("", NudgeRight("hello{\\|i1"));
}

TEST(subtitle_edit_ops, nudge_stops_at_the_ends_of_the_line) {
	EXPECT_EQ("", NudgeLeft("{\\|i1}world"));
	EXPECT_EQ("", NudgeRight("hello{\\|i1}"));
}

TEST(subtitle_edit_ops, nudge_moves_a_selection_inside_the_block_with_it) {
	std::string const text = "hello{\\i1}world";
	auto const tokens = agi::ass::TokenizeDialogueBody(text);
	auto const edit = aegisub::subtitle_edit_ops::MoveBlockAtPosition(
		text, tokens, 8, aegisub::subtitle_edit_ops::BlockMoveDirection::Right);

	ASSERT_TRUE(edit.handled);
	EXPECT_EQ(5, edit.block_start);
	EXPECT_EQ(10, edit.block_end);
	EXPECT_EQ(1, edit.delta);
	EXPECT_EQ(5, edit.replace_start);
	EXPECT_EQ(11, edit.replace_end);
	EXPECT_EQ("w{\\i1}", edit.replacement);
}

TEST(subtitle_edit_ops, escape_bounds_select_the_whole_escape_from_either_character) {
	EXPECT_EQ("\\N", EscapeAtCaret("a|\\Nb"));
	EXPECT_EQ("\\N", EscapeAtCaret("a\\|Nb"));
	EXPECT_EQ("\\n", EscapeAtCaret("a|\\nb"));
	EXPECT_EQ("\\n", EscapeAtCaret("a\\|nb"));
	EXPECT_EQ("\\h", EscapeAtCaret("a|\\hb"));
	EXPECT_EQ("\\h", EscapeAtCaret("a\\|hb"));
}

TEST(subtitle_edit_ops, escape_bounds_select_only_one_of_adjacent_escapes) {
	EXPECT_EQ("\\n", EscapeAtCaret("a\\N|\\n\\hb"));
	EXPECT_EQ("\\h", EscapeAtCaret("a\\N\\n\\|hb"));
}

TEST(subtitle_edit_ops, escape_bounds_select_nothing_outside_escapes) {
	EXPECT_EQ("", EscapeAtCaret("hel|lo"));
	EXPECT_EQ("", EscapeAtCaret("{|\\b1}"));
	EXPECT_EQ("", EscapeAtCaret("a\\N|b"));
	EXPECT_EQ("", EscapeAtCaret("|"));
}

TEST(subtitle_edit_ops, tag_bounds_select_whole_tag_from_backslash_or_name) {
	EXPECT_EQ("\\bord5", TagAtCaret("{|\\bord5}"));
	EXPECT_EQ("\\bord5", TagAtCaret("{\\bo|rd5}"));
	EXPECT_EQ("\\1c&H0000FF&", TagAtCaret("{|\\1c&H0000FF&}"));
	EXPECT_EQ("\\pos(100,200)", TagAtCaret("{|\\pos(100,200)\\b1}"));
	EXPECT_EQ("\\b1", TagAtCaret("{\\pos(100,200)\\|b1}"));
}

TEST(subtitle_edit_ops, tag_name_bounds_select_only_the_clicked_name) {
	EXPECT_EQ("pos", TagNameAtCaret("{\\|pos(100,200)}"));
	EXPECT_EQ("pos", TagNameAtCaret("{\\po|s(100,200)}"));
	EXPECT_EQ("move", TagNameAtCaret("{\\|move(0,0,100,100)}"));
	EXPECT_EQ("move", TagNameAtCaret("{\\mo|ve(0,0,100,100)}"));
}

TEST(subtitle_edit_ops, tag_name_bounds_select_nothing_outside_the_name) {
	EXPECT_EQ("", TagNameAtCaret("{|\\pos(100,200)}"));
	EXPECT_EQ("", TagNameAtCaret("{\\pos|(100,200)}"));
	EXPECT_EQ("", TagNameAtCaret("{\\pos(1|00,200)}"));
	EXPECT_EQ("", TagNameAtCaret("a|bc"));
}

TEST(subtitle_edit_ops, position_tags_expand_on_a_repeated_double_click) {
	auto const first_pos = PlanTagAtCaret("{\\po|s(100,200)}");
	EXPECT_EQ("pos", first_pos.selection);
	EXPECT_EQ("\\pos(100,200)",
		PlanTagAtCaret("{\\po|s(100,200)}", first_pos.repeat_tag_name_bounds).selection);

	auto const first_move = PlanTagAtCaret("{\\mo|ve(0,0,100,100)}");
	EXPECT_EQ("move", first_move.selection);
	EXPECT_EQ("\\move(0,0,100,100)",
		PlanTagAtCaret("{\\mo|ve(0,0,100,100)}", first_move.repeat_tag_name_bounds).selection);
}

TEST(subtitle_edit_ops, position_tag_expansion_requires_the_same_tag_name) {
	auto const first = PlanTagAtCaret("{\\po|s(100,200)\\pos(300,400)}");
	EXPECT_EQ("pos", first.selection);
	EXPECT_EQ("pos", PlanTagAtCaret(
		"{\\pos(100,200)\\po|s(300,400)}", first.repeat_tag_name_bounds).selection);
}

TEST(subtitle_edit_ops, other_tags_keep_single_stage_whole_tag_selection) {
	EXPECT_EQ("\\bord5", PlanTagAtCaret("{\\bo|rd5}").selection);
	EXPECT_EQ("100", PlanTagAtCaret("{\\pos(1|00,200)}").selection);
}

TEST(subtitle_edit_ops, tag_bounds_select_only_the_argument_on_a_value) {
	EXPECT_EQ("5", TagAtCaret("{\\bord|5}"));
	EXPECT_EQ("&H0000FF&", TagAtCaret("{\\1c&H00|00FF&}"));
	EXPECT_EQ("100", TagAtCaret("{\\pos(1|00,200)}"));
	EXPECT_EQ("200", TagAtCaret("{\\pos(100,2|00)}"));
}

TEST(subtitle_edit_ops, tag_bounds_keep_parens_balanced_for_nested_tags) {
	// The outer tag must not be truncated at the inner backslash.
	EXPECT_EQ("\\t(0,500,\\frz30)", TagAtCaret("{|\\t(0,500,\\frz30)}"));
	EXPECT_EQ("\\t(0,500,\\frz30)", TagAtCaret("{\\|t(0,500,\\frz30)}"));
	// ...and the inner tag must not swallow the paren closing the outer one.
	EXPECT_EQ("\\frz30", TagAtCaret("{\\t(0,500,|\\frz30)}"));
	EXPECT_EQ("\\frz30", TagAtCaret("{\\t(0,500,\\fr|z30)}"));

	// The "))" here is lexed as a single CLOSE_PAREN token, so the inner tag
	// ends in the middle of a token.
	EXPECT_EQ("\\clip(1,m 0 0 l 10 10)",
		TagAtCaret("{\\t(0,100,|\\clip(1,m 0 0 l 10 10))}"));
	EXPECT_EQ("\\t(0,100,\\clip(1,m 0 0 l 10 10))",
		TagAtCaret("{|\\t(0,100,\\clip(1,m 0 0 l 10 10))}"));
}

TEST(subtitle_edit_ops, tag_bounds_stop_at_block_boundaries) {
	// A stray '{' inside the block ends the tag.
	EXPECT_EQ("\\b1", TagAtCaret("{|\\b1{\\i1}"));
	// VSFilter renders an unclosed block as literal text, and MarkDrawings
	// retypes it as TEXT, so there is no tag to select.
	EXPECT_EQ("", TagAtCaret("{|\\pos(100,200"));
}

TEST(subtitle_edit_ops, tag_bounds_keep_whitespace_inside_the_tag) {
	// Whitespace in the ARG state is part of the argument (VSFilter semantics).
	EXPECT_EQ("\\b1 ", TagAtCaret("{|\\b1 \\i1}"));
	EXPECT_EQ("\\ fn Comic Sans MS ", TagAtCaret("{ |\\ fn Comic Sans MS }asd"));
	EXPECT_EQ("\\ fn Comic Sans MS ", TagAtCaret("{ \\ f|n Comic Sans MS }asd"));
}

TEST(subtitle_edit_ops, tag_bounds_select_nothing_outside_tags) {
	EXPECT_EQ("", TagAtCaret("hel|lo world"));
	EXPECT_EQ("", TagAtCaret("|{\\b1}"));
	EXPECT_EQ("", TagAtCaret("{\\pos|(100,200)}"));
	EXPECT_EQ("", TagAtCaret("{\\pos(100|,200)}"));
	EXPECT_EQ("", TagAtCaret("{\\b1|}"));
	EXPECT_EQ("", TagAtCaret("{\\b1}|"));
	EXPECT_EQ("", TagAtCaret("|"));
}

namespace {
std::optional<int> TagEndAt(std::string const& text, int caret, std::string const& tag, std::string const& alt = "") {
	AssDialogue line;
	line.Text = text;
	return aegisub::subtitle_edit_ops::GetTagEndInBlock(line.ParseTags(), caret, tag, alt);
}
} // namespace

TEST(subtitle_edit_ops, tag_end_sits_just_past_the_tag_inside_its_block) {
	// "{\3c&H112233&}" is 14 bytes; the caret lands after the '&', before '}'.
	EXPECT_EQ(13, TagEndAt("{\\3c&H112233&}text", 6, "\\3c"));
	EXPECT_EQ(13, TagEndAt("{\\3c&H112233&}text", 13, "\\3c"));
	// A caret in the trailing plain text resolves to the plain block; the
	// neighbour fallback still finds the tag beside it.
	EXPECT_EQ(13, TagEndAt("{\\3c&H112233&}text", 15, "\\3c"));
	// Byte offsets: a two-byte codepoint before the block shifts everything.
	EXPECT_EQ(15, TagEndAt("\xc3\xa9{\\3c&H112233&}", 4, "\\3c"));
}

TEST(subtitle_edit_ops, tag_end_matches_the_alt_spelling) {
	// set_tag reuses an existing \1c in place, so \c writes must find \1c.
	EXPECT_EQ(13, TagEndAt("{\\1c&HFFFFFF&}text", 5, "\\c", "\\1c"));
	// An empty alt matches nothing.
	EXPECT_EQ(std::nullopt, TagEndAt("{\\1c&HFFFFFF&}text", 5, "\\c"));
}

TEST(subtitle_edit_ops, tag_end_prefers_the_block_at_the_caret) {
	EXPECT_EQ(30, TagEndAt("{\\3c&H111111&}mid{\\3c&H222222&}", 20, "\\3c"));
	EXPECT_EQ(30, TagEndAt("{\\3c&H111111&}mid{\\3c&H222222&}", 31, "\\3c"));
	// The first block's tag still resolves when the caret is in it or in the
	// plain text right after it.
	EXPECT_EQ(13, TagEndAt("{\\3c&H111111&}mid{\\3c&H222222&}", 5, "\\3c"));
	EXPECT_EQ(13, TagEndAt("{\\3c&H111111&}mid{\\3c&H222222&}", 15, "\\3c"));
}

TEST(subtitle_edit_ops, tag_end_lands_between_neighbouring_tags) {
	// "{\i1\3c&H112233&\b1}": after the colour value, before the \b1.
	EXPECT_EQ(16, TagEndAt("{\\i1\\3c&H112233&\\b1}", 3, "\\3c"));
}

TEST(subtitle_edit_ops, tag_end_returns_nullopt_when_no_such_tag_is_near) {
	EXPECT_EQ(std::nullopt, TagEndAt("{\\b1}text", 3, "\\3c"));
	EXPECT_EQ(std::nullopt, TagEndAt("plain text", 3, "\\3c"));
	EXPECT_EQ(std::nullopt, TagEndAt("{\\b1}{\\i1}tail", 7, "\\3c"));
}

TEST(subtitle_edit_ops, written_block_lookup_finds_the_inserted_tag_not_an_earlier_namesake) {
	// Post-write state of picking a colour with the caret at the end of
	// "{\3c&H111111&}hello world": the write inserted a new override at the
	// caret, after the plain text. The index-based lookup must land there,
	// while the caret-based fallback demonstrably picks the line-start tag
	// (offset 13) — the reason set_tag reports the written block.
	AssDialogue inserted_at_end;
	inserted_at_end.Text = "{\\3c&H111111&}hello world{\\3c&H445566&}";
	auto blocks = inserted_at_end.ParseTags();
	EXPECT_EQ(38, aegisub::subtitle_edit_ops::GetTagEndInWrittenBlock(blocks, 2, "\\3c", ""));
	EXPECT_EQ(13, aegisub::subtitle_edit_ops::GetTagEndInBlock(blocks, 24, "\\3c", ""));

	// Same namesake trap between two overrides: caret was in the plain text,
	// the write landed in a new override after it.
	AssDialogue inserted_between;
	inserted_between.Text = "{\\3c&H111111&}mid{\\3c&H445566&}";
	blocks = inserted_between.ParseTags();
	EXPECT_EQ(30, aegisub::subtitle_edit_ops::GetTagEndInWrittenBlock(blocks, 2, "\\3c", ""));
}
