#include <main.h>
#include "../../src/timing_processor_plan.h"
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using aegisub::TimingProcessorLine;
using aegisub::TimingProcessorParameters;
std::vector<std::pair<int, int>> Milliseconds(std::vector<TimingProcessorLine> const& lines) {
	std::vector<std::pair<int, int>> result;
	result.reserve(lines.size());
	for (auto const& line : lines)
		result.emplace_back(line.start.GetMillisecond(), line.end.GetMillisecond());
	return result;
}
}

TEST(timing_processor_plan, preserves_collision_limits_and_existing_centisecond_conversion) {
	std::vector<TimingProcessorLine> const input{{.start = 50, .end = 500}, {.start = 1005, .end = 2005}, {.start = 2154, .end = 3004}};
	TimingProcessorParameters settings;
	settings.lead_in_enabled = settings.lead_out_enabled = true;
	settings.lead_in_ms = 100;
	settings.lead_out_ms = 200;
	auto result = aegisub::PrepareTimingProcessor(input, settings, {}, {});
	EXPECT_EQ((std::vector<std::pair<int, int>>{{0, 700}, {910, 2050}, {2050, 3200}}), Milliseconds(result));
	EXPECT_EQ((std::vector<std::pair<int, int>>{{50, 500}, {1005, 2005}, {2154, 3004}}), Milliseconds(input));
}

TEST(timing_processor_plan, retains_raw_signed_adjacency_rounding_and_pipeline_order) {
	std::vector<TimingProcessorLine> const input{{.start = 10000, .end = 11000}, {.start = 11400, .end = 12000}, {.start = 20000, .end = 21000}, {.start = 20800, .end = 22000}};
	TimingProcessorParameters settings;
	settings.lead_in_enabled = settings.lead_out_enabled = settings.adjacent_enabled = true;
	settings.lead_in_ms = 100;
	settings.lead_out_ms = 200;
	settings.adjacent_gap_ms = 200;
	settings.adjacent_overlap_ms = 600;
	settings.adjacent_bias_percent = 25;
	auto result = aegisub::PrepareTimingProcessor(input, settings, {}, {});
	EXPECT_EQ((std::vector<std::pair<int, int>>{{9900, 11225}, {11225, 12200}, {19900, 21076}, {21076, 22200}}), Milliseconds(result));
}

TEST(timing_processor_plan, consumes_equal_start_lines_in_the_supplied_order) {
	std::vector<TimingProcessorLine> const input{{.start = 1000, .end = 2000}, {.start = 1000, .end = 1500}, {.start = 2300, .end = 3000}};
	TimingProcessorParameters settings;
	settings.adjacent_enabled = true;
	settings.adjacent_gap_ms = settings.adjacent_overlap_ms = 1000;
	settings.adjacent_bias_percent = 50;
	auto result = aegisub::PrepareTimingProcessor(input, settings, {}, {});
	EXPECT_EQ((std::vector<std::pair<int, int>>{{1000, 1501}, {1501, 1900}, {1900, 3000}}), Milliseconds(result));
}

TEST(timing_processor_plan, preserves_keyframe_ties_equal_frame_noop_and_end_minus_one) {
	std::vector<TimingProcessorLine> const input{{.start = 1000, .end = 2000}, {.start = 1200, .end = 2400}, {.start = 1800, .end = 2440}, {.start = 3500, .end = 3900}};
	std::vector<int> const keys{0, 30, 60, 99};
	TimingProcessorParameters settings;
	settings.keyframes_enabled = true;
	settings.key_start_before_ms = 500;
	settings.key_start_after_ms = 620;
	settings.key_end_before_ms = 500;
	settings.key_end_after_ms = 60;
	auto result = aegisub::PrepareTimingProcessor(input, settings, agi::vfr::Framerate(25.0), keys);
	EXPECT_EQ((std::vector<std::pair<int, int>>{{1180, 2380}, {1200, 2400}, {1180, 2380}, {3940, 3940}}), Milliseconds(result));
}

TEST(timing_processor_plan, keeps_raw_times_on_equal_vfr_frames_and_uses_midpoints) {
	std::vector<TimingProcessorLine> const input{{.start = 65, .end = 185}, {.start = 180, .end = 260}};
	std::vector<int> const keys{0, 2, 5};
	TimingProcessorParameters settings;
	settings.keyframes_enabled = true;
	settings.key_start_before_ms = settings.key_start_after_ms = 500;
	settings.key_end_before_ms = settings.key_end_after_ms = 500;
	auto result = aegisub::PrepareTimingProcessor(input, settings, agi::vfr::Framerate(std::vector<int>{0, 40, 100, 160, 220, 300}), keys);
	EXPECT_EQ((std::vector<std::pair<int, int>>{{65, 70}, {260, 260}}), Milliseconds(result));
}

TEST(timing_processor_plan, late_overflow_does_not_mutate_input) {
	std::vector<TimingProcessorLine> const input{{.start = 1000, .end = 2000}, {.start = 3000, .end = 4000}, {.start = 5000, .end = 6000}};
	TimingProcessorParameters settings;
	settings.lead_out_enabled = true;
	settings.lead_out_ms = std::numeric_limits<int>::max();
	EXPECT_THROW(static_cast<void>(aegisub::PrepareTimingProcessor(input, settings, {}, {})), std::overflow_error);
	EXPECT_EQ((std::vector<std::pair<int, int>>{{1000, 2000}, {3000, 4000}, {5000, 6000}}), Milliseconds(input));
}

TEST(timing_processor_plan, rejects_parameters_that_cannot_be_calculated_safely) {
	std::vector<TimingProcessorLine> const input{{.start = 1000, .end = 2000}, {.start = 3000, .end = 4000}};
	TimingProcessorParameters settings;
	settings.lead_in_enabled = true;
	settings.lead_in_ms = -1;
	EXPECT_THROW(static_cast<void>(aegisub::PrepareTimingProcessor(input, settings, {}, {})), std::invalid_argument);
	settings.lead_in_ms = 0;
	settings.adjacent_enabled = true;
	settings.adjacent_bias_percent = 101;
	EXPECT_THROW(static_cast<void>(aegisub::PrepareTimingProcessor(input, settings, {}, {})), std::invalid_argument);
	settings.adjacent_bias_percent = 50;
	settings.keyframes_enabled = true;
	EXPECT_THROW(static_cast<void>(aegisub::PrepareTimingProcessor(input, settings, {}, {})), std::invalid_argument);
}
