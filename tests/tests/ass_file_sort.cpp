#include <gtest/gtest.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"

#include <set>
#include <string>
#include <vector>

namespace {

/// Append a line whose start time doubles as its identity, so the resulting
/// order can be asserted by reading the start times back out.
/// Use multiples of 10 only: agi::Time reads back at centisecond precision, so
/// any other value would come out of StartOrder() rounded.
AssDialogue *AddLine(AssFile &file, int start_ms) {
	auto line = new AssDialogue;
	line->Start = start_ms;
	line->End = start_ms + 1000;
	file.Events.push_back(*line);
	return line;
}

std::vector<int> StartOrder(AssFile const& file) {
	std::vector<int> order;
	for (auto const& line : file.Events)
		order.push_back(static_cast<int>(line.Start));
	return order;
}

// Shared text for comparator tests: "{\i1}ab" is raw 7 characters / rendered 2,
// "abc" is 3 / 3, and "a\Nb" is 4 / 2 (a line break renders as no character).
void AddTextSortLines(AssFile& file, std::vector<AssDialogue *>& lines) {
	for (int start : {10, 20, 30})
		lines.push_back(AddLine(file, start));
	lines[0]->Text = "{\\i1}ab";
	lines[1]->Text = "abc";
	lines[2]->Text = "a\\Nb";
}

} // namespace

TEST(ass_file_sort, whole_file_sorts_every_line_by_start_time) {
	AssFile file;
	for (int start : {50, 10, 40, 20, 30})
		AddLine(file, start);

	file.Sort();

	EXPECT_EQ(std::vector<int>({10, 20, 30, 40, 50}), StartOrder(file));
}

TEST(ass_file_sort, partial_sort_leaves_unselected_lines_in_place) {
	AssFile file;
	std::vector<AssDialogue *> lines;
	for (int start : {50, 40, 30, 20, 10})
		lines.push_back(AddLine(file, start));

	// Select only the middle block (40, 30, 20).
	std::set<AssDialogue *> limit{lines[1], lines[2], lines[3]};
	file.Sort(AssFile::CompStart, limit);

	// The selected block is sorted in place; the outer lines never move.
	EXPECT_EQ(std::vector<int>({50, 20, 30, 40, 10}), StartOrder(file));
}

TEST(ass_file_sort, disjoint_selected_blocks_sort_independently) {
	AssFile file;
	std::vector<AssDialogue *> lines;
	for (int start : {60, 50, 990, 40, 30, 980, 20, 10})
		lines.push_back(AddLine(file, start));

	// Two separate runs, split by the unselected 990 / 980 sentinels.
	std::set<AssDialogue *> limit{lines[0], lines[1], lines[3], lines[4], lines[6], lines[7]};
	file.Sort(AssFile::CompStart, limit);

	EXPECT_EQ(std::vector<int>({50, 60, 990, 30, 40, 980, 10, 20}), StartOrder(file));
}

TEST(ass_file_sort, selected_block_running_to_the_end_of_the_file_sorts) {
	AssFile file;
	std::vector<AssDialogue *> lines;
	for (int start : {990, 30, 10, 20})
		lines.push_back(AddLine(file, start));

	std::set<AssDialogue *> limit{lines[1], lines[2], lines[3]};
	file.Sort(AssFile::CompStart, limit);

	EXPECT_EQ(std::vector<int>({990, 10, 20, 30}), StartOrder(file));
}

TEST(ass_file_sort, selected_block_at_the_start_of_the_file_sorts) {
	AssFile file;
	std::vector<AssDialogue *> lines;
	for (int start : {30, 10, 20, 990})
		lines.push_back(AddLine(file, start));

	std::set<AssDialogue *> limit{lines[0], lines[1], lines[2]};
	file.Sort(AssFile::CompStart, limit);

	EXPECT_EQ(std::vector<int>({10, 20, 30, 990}), StartOrder(file));
}

// Single-line blocks are skipped rather than spliced; nothing may move.
TEST(ass_file_sort, isolated_single_line_selections_are_a_no_op) {
	AssFile file;
	std::vector<AssDialogue *> lines;
	for (int start : {50, 990, 10, 980, 30})
		lines.push_back(AddLine(file, start));

	std::set<AssDialogue *> limit{lines[0], lines[2], lines[4]};
	file.Sort(AssFile::CompStart, limit);

	EXPECT_EQ(std::vector<int>({50, 990, 10, 980, 30}), StartOrder(file));
}

// The scan stops once every selected line is accounted for. Lines after the
// last selected one must still be untouched.
TEST(ass_file_sort, early_exit_does_not_disturb_the_tail_of_the_file) {
	AssFile file;
	std::vector<AssDialogue *> lines;
	for (int start : {30, 20, 10, 500, 400, 300, 200, 100})
		lines.push_back(AddLine(file, start));

	std::set<AssDialogue *> limit{lines[0], lines[1], lines[2]};
	file.Sort(AssFile::CompStart, limit);

	EXPECT_EQ(std::vector<int>({10, 20, 30, 500, 400, 300, 200, 100}), StartOrder(file));
}

// A selection holding pointers that are not in this list must not corrupt it.
TEST(ass_file_sort, limit_entries_absent_from_the_list_are_ignored) {
	AssFile file;
	std::vector<AssDialogue *> lines;
	for (int start : {30, 10, 20})
		lines.push_back(AddLine(file, start));

	AssFile other;
	auto foreign = AddLine(other, 70);

	std::set<AssDialogue *> limit{lines[0], lines[1], lines[2], foreign};
	file.Sort(AssFile::CompStart, limit);

	EXPECT_EQ(std::vector<int>({10, 20, 30}), StartOrder(file));
	EXPECT_EQ(std::vector<int>({70}), StartOrder(other));
}

TEST(ass_file_sort, partial_sort_is_stable_for_equal_keys) {
	AssFile file;
	std::vector<AssDialogue *> lines;
	for (int start : {200, 100, 100, 50})
		lines.push_back(AddLine(file, start));
	// Tag the two equal-key lines so their relative order is observable.
	lines[1]->Actor = "first";
	lines[2]->Actor = "second";

	std::set<AssDialogue *> limit{lines[0], lines[1], lines[2], lines[3]};
	file.Sort(AssFile::CompStart, limit);

	EXPECT_EQ(std::vector<int>({50, 100, 100, 200}), StartOrder(file));
	auto it = std::next(file.Events.begin());
	EXPECT_EQ("first", it->Actor.get());
	EXPECT_EQ("second", std::next(it)->Actor.get());
}

TEST(ass_file_sort, other_comparators_sort_selected_blocks_too) {
	AssFile file;
	std::vector<AssDialogue *> lines;
	for (int i = 0; i < 3; ++i)
		lines.push_back(AddLine(file, i * 10));
	lines[0]->Layer = 5;
	lines[1]->Layer = 1;
	lines[2]->Layer = 3;

	std::set<AssDialogue *> limit{lines[0], lines[1], lines[2]};
	file.Sort(AssFile::CompLayer, limit);

	std::vector<int> layers;
	for (auto const& line : file.Events)
		layers.push_back(line.Layer);
	EXPECT_EQ(std::vector<int>({1, 3, 5}), layers);
}

TEST(ass_file_sort, text_comparators_use_raw_and_stripped_text) {
	AssFile file;
	std::vector<AssDialogue *> lines;
	AddTextSortLines(file, lines);

	// Raw byte order: '{' (0x7B) sorts after 'a', '\' (0x5C) before 'b'.
	file.Sort(AssFile::CompText);
	EXPECT_EQ(std::vector<int>({30, 20, 10}), StartOrder(file));

	// Override blocks are dropped, but \N inside plain text is kept.
	file.Sort(AssFile::CompTextStripped);
	EXPECT_EQ(std::vector<int>({30, 10, 20}), StartOrder(file));
}

TEST(ass_file_sort, text_length_counts_raw_text_including_tags) {
	AssFile file;
	std::vector<AssDialogue *> lines;
	AddTextSortLines(file, lines);

	// 3 ("abc") < 4 ("a\Nb") < 7 ("{\i1}ab")
	file.Sort(AssFile::CompTextLength);
	EXPECT_EQ(std::vector<int>({20, 30, 10}), StartOrder(file));
}

TEST(ass_file_sort, stripped_length_counts_rendered_characters) {
	AssFile file;
	std::vector<AssDialogue *> lines;
	AddTextSortLines(file, lines);

	// "{\i1}ab" and "a\Nb" both render as two characters and keep their
	// relative order (stable sort); "abc" renders as three.
	file.Sort(AssFile::CompTextStrippedLength);
	EXPECT_EQ(std::vector<int>({10, 30, 20}), StartOrder(file));
}
