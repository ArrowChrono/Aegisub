#ifdef WITH_LSMASNATIVE

#include <libaegisub/audio/provider.h>

#include "lsmas_native_api.h"
#include "audio_provider_timeline.h"
#include "mkv_wrap.h"
#include "lsmas_provider_common.h"
#include "options.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/fs.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>
#include <string>

namespace {
bool GetConfiguredLsmasNativeAudioDownmix() {
    return config::GetBoolOptionOrDefault("Provider/Audio/LsmasNative/Downmix", false);
}

class LsmasAudioProvider final : public agi::AudioProvider {
	lsmas_handle_t *handle = nullptr;
	std::optional<MkvAudioTimeline> timeline;
	std::string cache_filename_utf8;
	int64_t preroll_samples = 0;
	mutable int64_t next_frame = 0;
	mutable std::vector<unsigned char> preroll_buffer;

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		lsmas_provider::ErrorString error;
		auto const frame_bytes = static_cast<int64_t>(channels) * bytes_per_sample;
		auto read = [&](void *dst, int64_t at, int64_t frames) {
			auto const got = lsmas_provider::ReadAudioFramesWithRetry(
				dst, at, frames, frame_bytes,
				[&error, this](void *target, int64_t position, int64_t wanted) {
					error.Reset();
					return lsmas::GetApi().audio_get_samples(handle, target, position, wanted, error.Out());
				});
			if (got < 0)
				throw agi::AudioDecodeError(error.Message("failed to decode audio samples"));
		};
		bool const discontinuity = start != next_frame;
		next_frame = -1;
		if (preroll_samples > 0 && discontinuity && start > 0) {
			// The native decoder's minimum internal preroll does not guarantee
			// editing accuracy after an Opus seek. Decode a container-declared
			// convergence window through the public API, then continue at the
			// requested sample without shifting it. Sequential cache fills do
			// not pay this cost. Keep scratch space bounded for large metadata.
			int64_t position = start - std::min(start, preroll_samples);
			int64_t const chunk = std::min<int64_t>(4096, start - position);
			preroll_buffer.resize(static_cast<size_t>(chunk * frame_bytes));
			while (position < start) {
				auto const frames = std::min(chunk, start - position);
				read(preroll_buffer.data(), position, frames);
				position += frames;
			}
		}
		read(buf, start, count);
		next_frame = start + count;
	}

	public:
	LsmasAudioProvider(agi::fs::path const& filename, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink);
	~LsmasAudioProvider() override {
		if (handle)
			lsmas::GetApi().audio_close(handle);
	}

	std::optional<MkvAudioTimeline> const& GetTimeline() const { return timeline; }
	bool NeedsCache() const override { return true; }
	agi::AudioProviderMemoryStats GetMemoryStats() const override { return BuildMemoryStats("LsmasNative"); }
};

LsmasAudioProvider::LsmasAudioProvider(agi::fs::path const& filename, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) {
    auto const& api = lsmas::GetApi();
	auto const filename_utf8 = agi::fs::PathToString(filename);
    bool const downmix = GetConfiguredLsmasNativeAudioDownmix();

	int audio_ordinal = -1;
	int audio_count = 0;
	int stream_index = lsmas_provider::SelectTrack(filename, lsmas_provider::TrackType::Audio, choice_sink, &audio_ordinal, &audio_count);
	if (stream_index < 0)
		throw agi::AudioDataNotFound("no audio tracks found");

	timeline = MatroskaWrapper::GetOpusAudioTimeline(filename, audio_ordinal, audio_count);

	auto cache_name = lsmas_provider::GetIndexCacheFilename(filename);
	cache_filename_utf8 = agi::fs::PathToString(cache_name);

	auto options = lsmas_provider::MakeAudioOpenOptions(stream_index, downmix);
    options.cachefile = cache_filename_utf8.c_str();
	if (timeline)
		options.av_sync = 0;

	lsmas_provider::ErrorString error;
	if (br) {
		br->Run([&](agi::ProgressSink *ps) {
            ps->SetTitle("Indexing");
            ps->SetMessage("Reading audio sample data");
            handle = api.audio_open_with_progress_utf8(filename_utf8.c_str(), &options, lsmas_provider::ProgressCallback, ps, error.Out());
        });
	}
	else {
        handle = api.audio_open_with_progress_utf8(filename_utf8.c_str(), &options, nullptr, nullptr, error.Out());
    }
    if (!handle)
        throw agi::AudioProviderError(error.Message("failed to open audio"));
    auto close_handle_on_error = agi::make_scope_exit([&] {
        if (handle) {
            api.audio_close(handle);
            handle = nullptr;
        }
    });
    agi::fs::Touch(cache_name);
    lsmas_provider::CleanIndexCache();

    lsmas_audio_info_t info = {};
    error.Reset();
    if (api.audio_get_info(handle, &info, error.Out()) < 0)
        throw agi::AudioProviderError(error.Message("failed to query audio info"));

    channels = info.channels;
    sample_rate = info.sample_rate;
    bytes_per_sample = info.bytes_per_sample > 0 ? info.bytes_per_sample : 2;
    float_samples = false;
	num_samples = info.total_samples;
	decoded_samples = num_samples;
	if (channels <= 0 || sample_rate <= 0 || num_samples <= 0)
        throw agi::AudioProviderError("invalid audio properties returned by LsmasNative");
	if (timeline) {
		// Opus recommends at least 80 ms; honor a longer container declaration.
		auto const preroll_ns = std::max<uint64_t>(80000000, timeline->seek_pre_roll_ns);
		long double const samples = std::ceil(static_cast<long double>(preroll_ns) * sample_rate / 1000000000.0L);
		if (samples >= static_cast<long double>(std::numeric_limits<int64_t>::max()))
			throw agi::AudioProviderError("Matroska Opus seek preroll is too long");
		preroll_samples = static_cast<int64_t>(samples);
	}
	close_handle_on_error.release();
}
}

std::unique_ptr<agi::AudioProvider> CreateLsmasNativeAudioProvider(agi::fs::path const& file, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) {
	auto provider = agi::make_unique<LsmasAudioProvider>(file, br, std::move(choice_sink));
	auto const timeline = provider->GetTimeline();
	return ApplyMatroskaAudioTimeline(std::move(provider), timeline);
}

#endif
