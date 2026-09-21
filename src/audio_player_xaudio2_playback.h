#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace aegisub::xaudio2 {

enum class EndUpdate : uint8_t { Unchanged,
								 Stop,
								 Refill,
								 Rebuild };

[[nodiscard]] inline EndUpdate DecideEndUpdate(int64_t consumed, int64_t submitted_end, int64_t old_end, int64_t new_end) noexcept {
	if (new_end == old_end)
		return EndUpdate::Unchanged;
	if (new_end <= consumed)
		return EndUpdate::Stop;
	// Submitted buffers cannot be shortened, nor can an already queued EOS
	// marker be extended in place: it would reset SamplesPlayed mid-stream.
	if (new_end <= submitted_end || submitted_end >= old_end)
		return EndUpdate::Rebuild;
	return EndUpdate::Refill;
}

[[nodiscard]] inline int64_t ConsumedFrame(int64_t first_frame, int64_t submitted_end, uint64_t sample_base, uint64_t samples_played, bool drained_eos) noexcept {
	if (drained_eos)
		return submitted_end;
	auto const count = samples_played >= sample_base ? samples_played - sample_base : 0;
	auto const submitted = static_cast<uint64_t>(submitted_end - first_frame);
	return first_frame + static_cast<int64_t>(std::min(count, submitted));
}

// The worker publishes device progress; the UI only reads this value object.
// Its owner serializes Publish/Position, never borrowing an XAudio2 voice.
class PlaybackClock final {
	public:
	using Clock = std::chrono::steady_clock;

	private:
	int64_t anchor = 0;
	int64_t submitted_end = 0;
	int64_t reported = 0;
	Clock::time_point observed{};
	bool advancing = false;

	public:
	void Reset(int64_t frame, Clock::time_point now) noexcept {
		anchor = submitted_end = reported = frame;
		observed = now;
		advancing = false;
	}

	void Publish(int64_t consumed, int64_t queued_end, Clock::time_point now, bool running) noexcept {
		anchor = consumed;
		submitted_end = std::max(consumed, queued_end);
		observed = now;
		advancing = running;
	}

	[[nodiscard]] int64_t Position(Clock::time_point now, int sample_rate) noexcept {
		int64_t frame = anchor;
		if (advancing && now > observed && sample_rate > 0) {
			double const elapsed = std::chrono::duration<double>(now - observed).count();
			auto const available = submitted_end - anchor;
			double const advance = elapsed * sample_rate;
			frame = advance >= static_cast<double>(available)
						? submitted_end
						: anchor + static_cast<int64_t>(advance);
		}
		// Engine counters update in processing quanta. Preserve smooth monotonic
		// progress between observations, but never advance into unsubmitted PCM
		// even when the worker is blocked decoding the next buffer.
		reported = std::min(submitted_end, std::max(reported, frame));
		return reported;
	}
};

}
