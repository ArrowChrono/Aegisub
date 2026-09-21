#pragma once

#include "mkv_wrap_common.h"

#include <libaegisub/audio/provider.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>

// The source must expose decoded PCM without backend A/V padding or trimming.
// Apply the container origin before conversion/caching so every consumer sees
// the same timeline, including negative offsets and the final decoded sample.
class AudioTimelineProvider final : public agi::AudioProviderWrapper {
	int64_t offset;

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		source->GetAudioChecked(buf, start - offset, count);
	}

	void FillBufferInt16Mono(int16_t *buf, int64_t start, int64_t count) const override {
		source->GetInt16MonoAudioChecked(buf, start - offset, count);
	}

	public:
	AudioTimelineProvider(std::unique_ptr<agi::AudioProvider> source, int64_t offset)
		: AudioProviderWrapper(std::move(source)), offset(offset) {
		if (offset > std::numeric_limits<int64_t>::max() - num_samples)
			throw agi::AudioProviderError("Audio timeline is too long");
		if (offset <= -num_samples)
			throw agi::AudioProviderError("Audio ends before the video timeline begins");
		num_samples += offset;
		decoded_samples = num_samples;
	}

	bool NeedsCache() const override { return source->NeedsCache(); }
};

inline int64_t GetMatroskaAudioOffset(MkvAudioTimeline const& timeline, int sample_rate) {
	// Keep CodecDelay in nanoseconds and Opus pre-skip in 48 kHz samples.
	// Rounding either through the demuxer's millisecond time base loses samples.
	long double const samples =
		(static_cast<long double>(timeline.first_audio_ns) - timeline.first_video_ns - timeline.codec_delay_ns) * sample_rate / 1000000000.0L + static_cast<long double>(timeline.pre_skip) * sample_rate / 48000;
	if (!std::isfinite(samples) || samples >= static_cast<long double>(std::numeric_limits<int64_t>::max()) || samples <= static_cast<long double>(std::numeric_limits<int64_t>::min()))
		throw agi::AudioProviderError("Invalid Matroska audio timeline");
	return std::llround(samples);
}

inline std::unique_ptr<agi::AudioProvider> ApplyMatroskaAudioTimeline(
	std::unique_ptr<agi::AudioProvider> source, std::optional<MkvAudioTimeline> const& timeline) {
	if (!timeline)
		return source;
	auto const offset = GetMatroskaAudioOffset(*timeline, source->GetSampleRate());
	return std::make_unique<AudioTimelineProvider>(std::move(source), offset);
}
