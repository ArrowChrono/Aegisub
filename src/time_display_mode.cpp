// Copyright (c) 2026, Aegisub Project
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#include "time_display_mode.h"

#include <libaegisub/fs.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace {
constexpr int kMaxInternalTimeMs = std::numeric_limits<int>::max();

int clamp_internal_time_ms(long long ms) {
	return static_cast<int>(std::clamp(ms, 0LL, static_cast<long long>(kMaxInternalTimeMs)));
}

std::string lowercase(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
		return static_cast<char>(std::tolower(ch));
	});
	return value;
}

int normalize_display_duration_ms(agi::Time const& duration, SubtitleTimeDisplayMode mode) {
	if (mode == SubtitleTimeDisplayMode::Ass)
		return std::max(0, static_cast<int>(duration));
	if (mode == SubtitleTimeDisplayMode::Exact)
		return std::max(0, duration.GetMillisecond());
	throw std::logic_error("Frame display mode does not expose millisecond durations");
}
}

SubtitleTimeDisplayMode DefaultTimeDisplayModeForFile(agi::fs::path const& filename) {
	if (filename.empty())
		return SubtitleTimeDisplayMode::Ass;

	auto const extension = lowercase(agi::fs::PathToString(filename.extension()));
	if (extension == ".ass" || extension == ".ssa")
		return SubtitleTimeDisplayMode::Ass;
	return SubtitleTimeDisplayMode::Exact;
}

std::pair<int, int> GetDialogueTimesForDisplay(
	agi::Time const& start,
	agi::Time const& end,
	SubtitleTimeDisplayMode mode,
	agi::vfr::Framerate const*) {
	if (mode == SubtitleTimeDisplayMode::Ass)
		return { static_cast<int>(start), static_cast<int>(end) };
	if (mode == SubtitleTimeDisplayMode::Exact)
		return { start.GetMillisecond(), end.GetMillisecond() };
	throw std::logic_error("Frame display mode does not expose millisecond timestamps");
}

int GetDurationForDisplay(
	agi::Time const& start,
	agi::Time const& end,
	SubtitleTimeDisplayMode mode,
	agi::vfr::Framerate const* fps) {
	auto const displayed = GetDialogueTimesForDisplay(start, end, mode, fps);
	return std::max(0, displayed.second - displayed.first);
}

agi::Time GetEndTimeForDisplayedDuration(
	agi::Time const& start,
	agi::Time const& current_end,
	agi::Time const& displayed_duration,
	SubtitleTimeDisplayMode mode,
	agi::vfr::Framerate const* fps) {
	if (mode == SubtitleTimeDisplayMode::Exact)
		return start + displayed_duration;
	if (mode != SubtitleTimeDisplayMode::Ass)
		throw std::logic_error("Frame display mode does not expose millisecond durations");

	int const target_duration_ms = normalize_display_duration_ms(displayed_duration, mode);
	if (GetDurationForDisplay(start, current_end, mode, fps) == target_duration_ms)
		return current_end;
	if (target_duration_ms == 0)
		return start;

	return agi::Time(clamp_internal_time_ms(
		static_cast<long long>(static_cast<int>(start)) + target_duration_ms));
}

std::string FormatTimeForDisplay(int ms, SubtitleTimeDisplayMode mode) {
	agi::Time const time(ms);
	if (mode == SubtitleTimeDisplayMode::Ass)
		return time.GetAssFormatted();
	if (mode == SubtitleTimeDisplayMode::Exact)
		return time.GetAssFormatted(true);
	throw std::logic_error("Frame display mode does not format timestamps");
}

std::string FormatTimeForDisplay(agi::Time const& time, SubtitleTimeDisplayMode mode) {
	return FormatTimeForDisplay(time.GetMillisecond(), mode);
}
