#include "shift_times_plan.h"

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {
agi::Time Shift(agi::Time initial, aegisub::ShiftTimesParameters const& parameters,
				agi::vfr::Framerate const& timecodes, agi::vfr::Time type) {
	int const initial_time = initial;
	int64_t const shifted = static_cast<int64_t>(parameters.amount) +
							(parameters.by_time ? initial_time : timecodes.CheckedFrameAtTime(initial_time, type));
	if (shifted < std::numeric_limits<int>::min() || shifted > std::numeric_limits<int>::max())
		throw std::overflow_error(parameters.by_time ? "Shifted time exceeds the signed integer range" : "Shifted frame exceeds the signed integer range");
	return parameters.by_time ? static_cast<int>(shifted) : timecodes.CheckedTimeAtFrame(shifted, type);
}
}

namespace aegisub {
ShiftTimesPlan PrepareShiftTimes(std::span<ShiftTimesLine const> lines,
								 ShiftTimesParameters const& parameters, agi::vfr::Framerate const& timecodes) {
	if (parameters.mode < 0 || parameters.mode > 2 || parameters.fields < 0 || parameters.fields > 2 ||
		(!parameters.by_time && !timecodes.IsLoaded()))
		throw std::invalid_argument("Invalid shift times parameters");
	ShiftTimesPlan plan;
	if (parameters.by_time && parameters.amount == 0)
		return plan;
	bool selection_seen = false;
	bool in_block = false;
	for (size_t row = 0; row < lines.size(); ++row) {
		auto const& line = lines[row];
		if (line.selected) {
			selection_seen = true;
			int const selected_row = static_cast<int>(row) + 1;
			if (in_block)
				plan.selected_blocks.back().second = selected_row;
			else
				plan.selected_blocks.emplace_back(selected_row, selected_row);
		}
		in_block = line.selected;
		if ((parameters.mode == 1 && !line.selected) || (parameters.mode == 2 && !selection_seen))
			continue;
		plan.edits.push_back({.row = row, .start = line.start, .end = line.end});
		if (parameters.fields != 2)
			plan.edits.back().start = Shift(line.start, parameters, timecodes, agi::vfr::START);
		if (parameters.fields != 1)
			plan.edits.back().end = Shift(line.end, parameters, timecodes, agi::vfr::END);
	}
	return plan;
}
}
