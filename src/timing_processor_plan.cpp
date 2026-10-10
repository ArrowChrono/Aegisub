#include "timing_processor_plan.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace {
using aegisub::TimingProcessorLine;

bool collides(TimingProcessorLine const& line, TimingProcessorLine const& other) {
	return line.start < other.start ? other.start < line.end : line.start < other.end;
}

int get_closest_kf(std::span<int const> keyframes, int frame) {
	auto const pos = std::ranges::upper_bound(keyframes, frame);
	if (pos == keyframes.end()) return keyframes.back();
	return (pos == keyframes.begin() || *pos - frame < frame - *(pos - 1)) ? *pos : *(pos - 1);
}

template <class Iter, class Compare>
int safe_time(Iter begin, Iter end, TimingProcessorLine const& line, int64_t initial,
			  agi::Time TimingProcessorLine::*field, Compare compare) {
	for (; begin != end; ++begin) {
		if (!collides(line, *begin))
			initial = compare(initial, static_cast<int64_t>(static_cast<int>((*begin).*field)));
	}
	if (initial < std::numeric_limits<int>::min() || initial > std::numeric_limits<int>::max())
		throw std::overflow_error("Timing processor lead-in/out exceeds the time range");
	return static_cast<int>(initial);
}
}

namespace aegisub {
std::vector<TimingProcessorLine> PrepareTimingProcessor(
	std::span<TimingProcessorLine const> ordered_lines, TimingProcessorParameters const& parameters,
	agi::vfr::Framerate const& timecodes, std::span<int const> keyframes) {
	if ((parameters.lead_in_enabled && parameters.lead_in_ms < 0) ||
		(parameters.lead_out_enabled && parameters.lead_out_ms < 0) ||
		(parameters.adjacent_enabled && (parameters.adjacent_gap_ms < 0 || parameters.adjacent_overlap_ms < 0 ||
										 parameters.adjacent_bias_percent < 0 || parameters.adjacent_bias_percent > 100)) ||
		(parameters.keyframes_enabled && (parameters.key_start_before_ms < 0 || parameters.key_start_after_ms < 0 ||
										  parameters.key_end_before_ms < 0 || parameters.key_end_after_ms < 0 || keyframes.empty() || !timecodes.IsLoaded())))
		throw std::invalid_argument("Invalid timing processor parameters");

	std::vector<TimingProcessorLine> pending(ordered_lines.begin(), ordered_lines.end());
	if (parameters.lead_in_enabled && parameters.lead_in_ms) {
		for (size_t i = 0; i < pending.size(); ++i)
			pending[i].start = safe_time(pending.rend() - i, pending.rend(), pending[i],
										 static_cast<int64_t>(static_cast<int>(pending[i].start)) - parameters.lead_in_ms,
										 &TimingProcessorLine::end, [](int64_t a, int64_t b) { return std::max(a, b); });
	}
	if (parameters.lead_out_enabled && parameters.lead_out_ms) {
		for (size_t i = 0; i < pending.size(); ++i)
			pending[i].end = safe_time(pending.begin() + i + 1, pending.end(), pending[i],
									   static_cast<int64_t>(static_cast<int>(pending[i].end)) + parameters.lead_out_ms,
									   &TimingProcessorLine::start, [](int64_t a, int64_t b) { return std::min(a, b); });
	}
	if (parameters.adjacent_enabled) {
		double bias = parameters.adjacent_bias_percent / 100.0;
		for (size_t i = 1; i < pending.size(); ++i) {
			auto& prev = pending[i - 1];
			auto& cur = pending[i];
			int dist = cur.start.GetMillisecond() - prev.end.GetMillisecond();
			if ((dist < 0 && -dist <= parameters.adjacent_overlap_ms) || (dist > 0 && dist <= parameters.adjacent_gap_ms)) {
				int set_pos = prev.end.GetMillisecond() + static_cast<int>(dist * bias + 0.5);
				cur.start = set_pos;
				prev.end = set_pos;
			}
		}
	}
	if (parameters.keyframes_enabled) {
		for (auto& cur : pending) {
			int start_frame = timecodes.FrameAtTime(cur.start, agi::vfr::START);
			int end_frame = timecodes.FrameAtTime(cur.end, agi::vfr::END);
			int closest = get_closest_kf(keyframes, start_frame);
			int time = timecodes.TimeAtFrame(closest, agi::vfr::START);
			if ((closest > start_frame && time - cur.start <= parameters.key_start_before_ms) ||
				(closest < start_frame && cur.start - time <= parameters.key_start_after_ms))
				cur.start = time;
			closest = get_closest_kf(keyframes, end_frame) - 1;
			time = timecodes.TimeAtFrame(closest, agi::vfr::END);
			if ((closest > end_frame && time - cur.end <= parameters.key_end_before_ms) ||
				(closest < end_frame && cur.end - time <= parameters.key_end_after_ms))
				cur.end = time;
		}
	}
	return pending;
}
}
