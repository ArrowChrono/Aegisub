#pragma once

#include <libaegisub/ass/time.h>
#include <libaegisub/vfr.h>
#include <span>
#include <vector>

namespace aegisub {
struct TimingProcessorLine {
	agi::Time start;
	agi::Time end;
};
struct TimingProcessorParameters {
	int lead_in_ms = 0;
	int lead_out_ms = 0;
	int adjacent_gap_ms = 0;
	int adjacent_overlap_ms = 0;
	int adjacent_bias_percent = 0;
	int key_start_before_ms = 0;
	int key_start_after_ms = 0;
	int key_end_before_ms = 0;
	int key_end_after_ms = 0;
	bool lead_in_enabled = false;
	bool lead_out_enabled = false;
	bool adjacent_enabled = false;
	bool keyframes_enabled = false;
};
[[nodiscard]] std::vector<TimingProcessorLine> PrepareTimingProcessor(
	std::span<TimingProcessorLine const> ordered_lines, TimingProcessorParameters const& parameters,
	agi::vfr::Framerate const& timecodes, std::span<int const> keyframes);
}
