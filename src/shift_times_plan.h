#pragma once

#include <libaegisub/ass/time.h>
#include <libaegisub/vfr.h>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace aegisub {
struct ShiftTimesLine {
	agi::Time start;
	agi::Time end;
	bool selected = false;
};
struct ShiftTimesParameters {
	int amount = 0;
	int mode = 0;
	int fields = 0;
	bool by_time = true;
};
struct ShiftTimesEdit {
	size_t row;
	agi::Time start;
	agi::Time end;
};
struct ShiftTimesPlan {
	std::vector<ShiftTimesEdit> edits;
	std::vector<std::pair<int, int>> selected_blocks;
};
[[nodiscard]] ShiftTimesPlan PrepareShiftTimes(std::span<ShiftTimesLine const> lines,
											   ShiftTimesParameters const& parameters, agi::vfr::Framerate const& timecodes);
}
