#include <gtest/gtest.h>

#include "../../src/time_display_mode.h"

#include <libaegisub/fs.h>
#include <libaegisub/vfr.h>

TEST(time_display_mode, defaults_to_ass_for_ass_and_exact_for_other_formats) {
	EXPECT_EQ(SubtitleTimeDisplayMode::Ass, DefaultTimeDisplayModeForFile(agi::fs::path("subtitle.ass")));
	EXPECT_EQ(SubtitleTimeDisplayMode::Ass, DefaultTimeDisplayModeForFile(agi::fs::path("subtitle.ASS")));
	EXPECT_EQ(SubtitleTimeDisplayMode::Ass, DefaultTimeDisplayModeForFile(agi::fs::path("subtitle.ssa")));
	EXPECT_EQ(SubtitleTimeDisplayMode::Exact, DefaultTimeDisplayModeForFile(agi::fs::path("subtitle.Srt")));
	EXPECT_EQ(SubtitleTimeDisplayMode::Exact, DefaultTimeDisplayModeForFile(agi::fs::path("subtitle.srt")));
	EXPECT_EQ(SubtitleTimeDisplayMode::Ass, DefaultTimeDisplayModeForFile({}));
}

TEST(time_display_mode, ass_display_uses_legacy_rounding_without_projection) {
	auto const fps = agi::vfr::Framerate(100.0);
	auto const displayed = GetDialogueTimesForDisplay(agi::Time(19), agi::Time(21), SubtitleTimeDisplayMode::Ass, &fps);

	EXPECT_EQ(20, displayed.first);
	EXPECT_EQ(20, displayed.second);
	EXPECT_EQ("0:00:00.02", FormatTimeForDisplay(displayed.first, SubtitleTimeDisplayMode::Ass));
}

TEST(time_display_mode, ass_display_can_collapse_short_intervals_like_legacy_storage_rounding) {
	auto const fps = agi::vfr::Framerate(100.0);
	auto const displayed = GetDialogueTimesForDisplay(agi::Time(18), agi::Time(19), SubtitleTimeDisplayMode::Ass, &fps);

	EXPECT_EQ(20, displayed.first);
	EXPECT_EQ(20, displayed.second);
}

TEST(time_display_mode, exact_display_preserves_internal_milliseconds) {
	auto const displayed = GetDialogueTimesForDisplay(agi::Time(1234), agi::Time(2345), SubtitleTimeDisplayMode::Exact);

	EXPECT_EQ(1234, displayed.first);
	EXPECT_EQ(2345, displayed.second);
	EXPECT_EQ("0:00:01.234", FormatTimeForDisplay(displayed.first, SubtitleTimeDisplayMode::Exact));
}

TEST(time_display_mode, ass_display_preserves_times_past_ten_hours) {
	auto const displayed = GetDialogueTimesForDisplay(
		agi::Time(12 * 60 * 60 * 1000),
		agi::Time(12 * 60 * 60 * 1000 + 1230),
		SubtitleTimeDisplayMode::Ass);

	EXPECT_EQ(12 * 60 * 60 * 1000, displayed.first);
	EXPECT_EQ(12 * 60 * 60 * 1000 + 1230, displayed.second);
	EXPECT_EQ("12:00:00.00", FormatTimeForDisplay(displayed.first, SubtitleTimeDisplayMode::Ass));
}

TEST(time_display_mode, ass_display_text_does_not_uniquely_identify_internal_time) {
	auto const displayed_zero = GetDialogueTimesForDisplay(
		agi::Time(0),
		agi::Time(100),
		SubtitleTimeDisplayMode::Ass);
	auto const displayed_one_ms = GetDialogueTimesForDisplay(
		agi::Time(1),
		agi::Time(100),
		SubtitleTimeDisplayMode::Ass);

	EXPECT_NE(agi::Time(0).GetMillisecond(), agi::Time(1).GetMillisecond());
	EXPECT_EQ(displayed_zero.first, displayed_one_ms.first);
	EXPECT_EQ(
		FormatTimeForDisplay(displayed_zero.first, SubtitleTimeDisplayMode::Ass),
		FormatTimeForDisplay(displayed_one_ms.first, SubtitleTimeDisplayMode::Ass));
}

TEST(time_display_mode, ass_displayed_boundary_is_independent_of_the_other_boundary) {
	auto const collapsed_interval = GetDialogueTimesForDisplay(
		agi::Time(18),
		agi::Time(19),
		SubtitleTimeDisplayMode::Ass);
	auto const regular_interval = GetDialogueTimesForDisplay(
		agi::Time(18),
		agi::Time(30),
		SubtitleTimeDisplayMode::Ass);

	EXPECT_EQ(20, collapsed_interval.first);
	EXPECT_EQ(20, regular_interval.first);
}

TEST(time_display_mode, duration_follows_selected_display_semantics) {
	auto const fps = agi::vfr::Framerate(100.0);

	EXPECT_EQ(10, GetDurationForDisplay(agi::Time(5), agi::Time(15), SubtitleTimeDisplayMode::Ass, &fps));
	EXPECT_EQ(10, GetDurationForDisplay(agi::Time(5), agi::Time(15), SubtitleTimeDisplayMode::Exact));
	EXPECT_EQ(10, GetDurationForDisplay(agi::Time(14), agi::Time(15), SubtitleTimeDisplayMode::Ass, &fps));
	EXPECT_EQ(1, GetDurationForDisplay(agi::Time(14), agi::Time(15), SubtitleTimeDisplayMode::Exact));
}

TEST(time_display_mode, ass_duration_edit_keeps_existing_internal_end_for_same_display_duration) {
	auto const fps = agi::vfr::Framerate(100.0);

	auto const end = GetEndTimeForDisplayedDuration(
		agi::Time(14),
		agi::Time(24),
		agi::Time(10),
		SubtitleTimeDisplayMode::Ass,
		&fps);

	EXPECT_EQ(24, end.GetMillisecond());
}

TEST(time_display_mode, ass_duration_edit_preserves_current_displayed_start_when_representable) {
	auto const fps = agi::vfr::Framerate(100.0);

	auto const end = GetEndTimeForDisplayedDuration(
		agi::Time(14),
		agi::Time(40),
		agi::Time(10),
		SubtitleTimeDisplayMode::Ass,
		&fps);
	auto const displayed = GetDialogueTimesForDisplay(agi::Time(14), end, SubtitleTimeDisplayMode::Ass, &fps);

	EXPECT_EQ(20, end.GetMillisecond());
	EXPECT_EQ(10, displayed.first);
	EXPECT_EQ(20, displayed.second);
}

TEST(time_display_mode, ass_duration_edit_adds_legacy_display_duration_to_internal_start) {
	auto const fps = agi::vfr::Framerate(100.0);

	auto const end = GetEndTimeForDisplayedDuration(
		agi::Time(14),
		agi::Time(15),
		agi::Time(20),
		SubtitleTimeDisplayMode::Ass,
		&fps);
	auto const displayed = GetDialogueTimesForDisplay(agi::Time(14), end, SubtitleTimeDisplayMode::Ass, &fps);

	EXPECT_EQ(30, end.GetMillisecond());
	EXPECT_EQ(10, displayed.first);
	EXPECT_EQ(30, displayed.second);
}

TEST(time_display_mode, ass_duration_edit_supports_times_past_ten_hours) {
	auto const start = agi::Time(12 * 60 * 60 * 1000);
	auto const end = GetEndTimeForDisplayedDuration(
		start,
		agi::Time(12 * 60 * 60 * 1000 + 1000),
		agi::Time(2000),
		SubtitleTimeDisplayMode::Ass);

	EXPECT_EQ(12 * 60 * 60 * 1000 + 2000, end.GetMillisecond());
}
