#include <main.h>
#include "../../src/shift_times_plan.h"

#include <limits>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {
using aegisub::ShiftTimesLine;
using Edits = std::vector<std::tuple<size_t, int, int>>;
Edits Milliseconds(aegisub::ShiftTimesPlan const& plan) {
	Edits result;
	result.reserve(plan.edits.size());
	for (auto const& edit : plan.edits)
		result.emplace_back(edit.row, edit.start.GetMillisecond(), edit.end.GetMillisecond());
	return result;
}
}

TEST(shift_times_plan, converts_only_shifted_endpoints_once_and_clamps_negative_results) {
	std::vector<ShiftTimesLine> const input{{.start = 105, .end = 205, .selected = true}};
	EXPECT_EQ((Edits{{0, 117, 205}}), Milliseconds(aegisub::PrepareShiftTimes(input, {.amount = 7, .fields = 1}, {})));
	EXPECT_EQ((Edits{{0, 105, 217}}), Milliseconds(aegisub::PrepareShiftTimes(input, {.amount = 7, .fields = 2}, {})));
	EXPECT_EQ((Edits{{0, 0, 10}}), Milliseconds(aegisub::PrepareShiftTimes(input, {.amount = -200}, {})));
	EXPECT_EQ(105, input[0].start.GetMillisecond());
	EXPECT_EQ(205, input[0].end.GetMillisecond());
}

TEST(shift_times_plan, retains_original_disjoint_selection_blocks_in_every_mode) {
	std::vector<ShiftTimesLine> const input{{.start = 505, .end = 605}, {.start = 105, .end = 205, .selected = true}, {.start = 305, .end = 405, .selected = true}, {.start = 5, .end = 95}, {.start = 205, .end = 305, .selected = true}};
	std::vector<std::pair<int, int>> const blocks{{2, 3}, {5, 5}};
	auto all = aegisub::PrepareShiftTimes(input, {.amount = 7}, {});
	EXPECT_EQ((Edits{{0, 517, 617}, {1, 117, 217}, {2, 317, 417}, {3, 17, 107}, {4, 217, 317}}), Milliseconds(all));
	EXPECT_EQ(blocks, all.selected_blocks);
	auto selected = aegisub::PrepareShiftTimes(input, {.amount = 7, .mode = 1}, {});
	EXPECT_EQ((Edits{{1, 117, 217}, {2, 317, 417}, {4, 217, 317}}), Milliseconds(selected));
	EXPECT_EQ(blocks, selected.selected_blocks);
	auto onward = aegisub::PrepareShiftTimes(input, {.amount = 7, .mode = 2}, {});
	EXPECT_EQ((Edits{{1, 117, 217}, {2, 317, 417}, {3, 17, 107}, {4, 217, 317}}), Milliseconds(onward));
	EXPECT_EQ(blocks, onward.selected_blocks);
}

TEST(shift_times_plan, distinguishes_time_zero_from_frame_zero_midpoint_normalization) {
	std::vector<ShiftTimesLine> const input{{.start = 105, .end = 205, .selected = true}};
	auto time = aegisub::PrepareShiftTimes(input, {}, {});
	EXPECT_TRUE(time.edits.empty());
	EXPECT_TRUE(time.selected_blocks.empty());
	auto frames = aegisub::PrepareShiftTimes(input, {.by_time = false}, agi::vfr::Framerate(25.0));
	EXPECT_EQ((Edits{{0, 100, 220}}), Milliseconds(frames));
	EXPECT_EQ((std::vector<std::pair<int, int>>{{1, 1}}), frames.selected_blocks);
}

TEST(shift_times_plan, shifts_vfr_endpoints_and_large_representable_cfr_offsets) {
	std::vector<ShiftTimesLine> const input{{.start = 105, .end = 205}};
	auto vfr = aegisub::PrepareShiftTimes(input, {.amount = 1, .by_time = false}, agi::vfr::Framerate(std::vector<int>{0, 40, 100, 160, 240}));
	EXPECT_EQ((Edits{{0, 200, 270}}), Milliseconds(vfr));
	auto large = aegisub::PrepareShiftTimes(input, {.amount = 10000000, .by_time = false}, agi::vfr::Framerate(25.0));
	EXPECT_EQ((Edits{{0, 400000100, 400000220}}), Milliseconds(large));
}

TEST(shift_times_plan, later_overflow_does_not_publish_candidates_or_mutate_input) {
	std::vector<ShiftTimesLine> const input{{.start = 105, .end = 205, .selected = true}, {.start = 1000, .end = std::numeric_limits<int>::max(), .selected = true}};
	aegisub::ShiftTimesPlan published;
	EXPECT_THROW(published = aegisub::PrepareShiftTimes(input, {.amount = 100}, {}), std::overflow_error);
	EXPECT_TRUE(published.edits.empty());
	EXPECT_TRUE(published.selected_blocks.empty());
	EXPECT_EQ(105, input[0].start.GetMillisecond());
	EXPECT_EQ(205, input[0].end.GetMillisecond());
	EXPECT_EQ(1000, input[1].start.GetMillisecond());
	EXPECT_EQ(std::numeric_limits<int>::max(), input[1].end.GetMillisecond());
	std::vector<ShiftTimesLine> const frame_input{{.start = 105, .end = 205}, {.start = 1000, .end = 1100}};
	EXPECT_THROW(published = aegisub::PrepareShiftTimes(frame_input, {.amount = 2147483000, .by_time = false}, agi::vfr::Framerate(1000.0)), std::overflow_error);
	std::vector<ShiftTimesLine> const time_input{{.start = 105, .end = 205}, {.start = 200000000, .end = 200000100}};
	EXPECT_THROW(published = aegisub::PrepareShiftTimes(time_input, {.amount = 50000000, .by_time = false}, agi::vfr::Framerate(25.0)), std::overflow_error);
	EXPECT_TRUE(published.edits.empty());
	EXPECT_EQ(1000, frame_input[1].start.GetMillisecond());
	EXPECT_EQ(200000000, time_input[1].start.GetMillisecond());
}

TEST(shift_times_plan, rejects_unsupported_parameters_and_missing_frame_reference) {
	std::vector<ShiftTimesLine> const input{{.start = 105, .end = 205}};
	EXPECT_THROW(aegisub::PrepareShiftTimes(input, {.amount = 7, .mode = 3}, {}), std::invalid_argument);
	EXPECT_THROW(aegisub::PrepareShiftTimes(input, {.amount = 7, .fields = -1}, {}), std::invalid_argument);
	EXPECT_THROW(aegisub::PrepareShiftTimes(input, {.by_time = false}, {}), std::invalid_argument);
}
