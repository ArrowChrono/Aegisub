// Copyright (c) 2026

#include <main.h>

#include "../../src/audio_display_analysis.h"
#include "../../src/audio_display_invalidation_planner.h"
#include "../../src/audio_latest_range_scheduler.h"
#include "../../src/audio_marker_drag_dead_zone.h"
#include "../../src/audio_marker_pixel_aggregation.h"
#include "../../src/audio_display_source.h"
#include "../../src/audio_mix_policy.h"
#include "../../src/audio_scroll_position.h"
#include "../../src/audio_spectrum_analysis_cache.h"
#include "../../src/audio_waveform_column_ref.h"
#include "../../src/audio_waveform_summary_cache.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include <wx/pen.h>

namespace {
class SyntheticAudioMarker final : public AudioMarker {
	int position;
	Kind kind;
	FeetStyle feet;

public:
	SyntheticAudioMarker(int position, Kind kind, FeetStyle feet = Feet_None)
	: position(position), kind(kind), feet(feet) { }
	int GetPosition() const override { return position; }
	wxPen GetStyle() const override { return wxPen(); }
	FeetStyle GetFeet() const override { return feet; }
	Kind GetKind() const override { return kind; }
};

TEST(lagi_audio_display, dense_markers_collapse_to_device_pixels_with_semantic_priority) {
	std::vector<SyntheticAudioMarker> storage;
	storage.reserve(44289 * 2 + 10);
	AudioMarkerVector markers;
	markers.reserve(storage.capacity());
	for (int line = 0; line < 44289; ++line) {
		storage.emplace_back(line % 1000, AudioMarker::Kind::Inactive);
		markers.push_back(&storage.back());
		storage.emplace_back(line % 1000, AudioMarker::Kind::Inactive);
		markers.push_back(&storage.back());
	}
	auto const collapsed = AggregateAudioMarkersByPixel(
		markers, 0, 999, [](AudioMarker const& marker) { return marker.GetPosition(); });
	EXPECT_EQ(88578u, markers.size());
	EXPECT_EQ(1000u, collapsed.size());

	storage.emplace_back(500, AudioMarker::Kind::Active, AudioMarker::Feet_Left);
	markers.push_back(&storage.back());
	storage.emplace_back(500, AudioMarker::Kind::Inactive, AudioMarker::Feet_Right);
	markers.push_back(&storage.back());
	storage.emplace_back(500, AudioMarker::Kind::Active, AudioMarker::Feet_Right);
	markers.push_back(&storage.back());
	auto const prioritized = AggregateAudioMarkersByPixel(
		markers, 0, 999, [](AudioMarker const& marker) { return marker.GetPosition(); });
	ASSERT_EQ(1000u, prioritized.size());
	auto const it = std::find_if(prioritized.begin(), prioritized.end(), [](auto const& marker) {
		return marker.x == 500;
	});
	ASSERT_NE(prioritized.end(), it);
	EXPECT_EQ(AudioMarker::Kind::Active, it->marker->GetKind());
	EXPECT_EQ(AudioMarker::Feet_Both, it->feet);
	ASSERT_NE(nullptr, it->left_foot_marker);
	ASSERT_NE(nullptr, it->right_foot_marker);
	EXPECT_EQ(AudioMarker::Kind::Active, it->left_foot_marker->GetKind());
	EXPECT_EQ(AudioMarker::Kind::Active, it->right_foot_marker->GetKind());

	storage.emplace_back(501, AudioMarker::Kind::Selected);
	markers.push_back(&storage.back());
	storage.emplace_back(501, AudioMarker::Kind::Inactive);
	markers.push_back(&storage.back());
	storage.emplace_back(502, AudioMarker::Kind::Keyframe);
	markers.push_back(&storage.back());
	storage.emplace_back(502, AudioMarker::Kind::Active);
	markers.push_back(&storage.back());
	storage.emplace_back(503, AudioMarker::Kind::VideoPosition);
	markers.push_back(&storage.back());
	storage.emplace_back(503, AudioMarker::Kind::Keyframe);
	markers.push_back(&storage.back());
	auto const all_priorities = AggregateAudioMarkersByPixel(
		markers, 0, 999, [](AudioMarker const& marker) { return marker.GetPosition(); });
	auto kind_at = [&](int x) {
		auto const marker = std::find_if(all_priorities.begin(), all_priorities.end(), [=](auto const& item) {
			return item.x == x;
		});
		return marker == all_priorities.end() ? AudioMarker::Kind::Generic : marker->marker->GetKind();
	};
	EXPECT_EQ(AudioMarker::Kind::Selected, kind_at(501));
	EXPECT_EQ(AudioMarker::Kind::Keyframe, kind_at(502));
	EXPECT_EQ(AudioMarker::Kind::VideoPosition, kind_at(503));
}

TEST(lagi_audio_display, shared_boundaries_keep_foot_styles_from_lower_priority_markers) {
	std::vector<SyntheticAudioMarker> storage;
	storage.reserve(4);
	AudioMarkerVector markers;

	storage.emplace_back(100, AudioMarker::Kind::Inactive, AudioMarker::Feet_Left);
	markers.push_back(&storage.back());
	storage.emplace_back(100, AudioMarker::Kind::Active, AudioMarker::Feet_Right);
	markers.push_back(&storage.back());
	storage.emplace_back(200, AudioMarker::Kind::Active, AudioMarker::Feet_Right);
	markers.push_back(&storage.back());
	storage.emplace_back(200, AudioMarker::Kind::Inactive, AudioMarker::Feet_Left);
	markers.push_back(&storage.back());

	auto const collapsed = AggregateAudioMarkersByPixel(
		markers, 0, 300, [](AudioMarker const& marker) { return marker.GetPosition(); });
	ASSERT_EQ(2u, collapsed.size());
	EXPECT_EQ(markers[1], collapsed[0].marker);
	EXPECT_EQ(markers[0], collapsed[0].left_foot_marker);
	EXPECT_EQ(markers[1], collapsed[0].right_foot_marker);
	EXPECT_EQ(AudioMarker::Feet_Both, collapsed[0].feet);
	EXPECT_EQ(markers[2], collapsed[1].marker);
	EXPECT_EQ(markers[3], collapsed[1].left_foot_marker);
	EXPECT_EQ(markers[2], collapsed[1].right_foot_marker);
	EXPECT_EQ(AudioMarker::Feet_Both, collapsed[1].feet);
}

class ScopedTestDeadline {
	std::jthread watchdog;

public:
	explicit ScopedTestDeadline(char const* test_name)
	: watchdog([test_name](std::stop_token stop_token) {
		std::mutex mutex;
		std::condition_variable_any condition;
		std::unique_lock<std::mutex> lock(mutex);
		condition.wait_for(
			lock,
			stop_token,
			std::chrono::seconds(10),
			[] { return false; });
		if (stop_token.stop_requested())
			return;

		std::fprintf(stderr, "Timed out waiting for %s\n", test_name);
		std::fflush(stderr);
		std::abort();
	}) {
	}
};

struct Int16StereoProvider final : agi::AudioProvider {
	Int16StereoProvider() {
		channels = 2;
		num_samples = 4;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		static const int16_t samples[] = {
			32767, -32768,
			16384, -16384,
			8192, -8192,
			4096, -4096,
		};
		auto out = static_cast<int16_t *>(buf);
		for (int64_t i = 0; i < count; ++i) {
			out[i * 2 + 0] = samples[(start + i) * 2 + 0];
			out[i * 2 + 1] = samples[(start + i) * 2 + 1];
		}
	}
};

struct Int16MonoProvider final : agi::AudioProvider {
	Int16MonoProvider() {
		channels = 1;
		num_samples = 4;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		static const int16_t samples[] = {
			32767,
			-32768,
			0,
			16384,
		};
		auto out = static_cast<int16_t *>(buf);
		for (int64_t i = 0; i < count; ++i)
			out[i] = samples[start + i];
	}
};

struct FloatStereoProvider final : agi::AudioProvider {
	FloatStereoProvider() {
		channels = 2;
		num_samples = 2;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(float);
		float_samples = true;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		static const float samples[] = {
			0.25f, -0.5f,
			0.75f, -1.0f,
		};
		auto out = static_cast<float *>(buf);
		for (int64_t i = 0; i < count; ++i) {
			out[i * 2 + 0] = samples[(start + i) * 2 + 0];
			out[i * 2 + 1] = samples[(start + i) * 2 + 1];
		}
	}
};

struct CountingStereoProvider final : agi::AudioProvider {
	mutable int fill_calls = 0;

	CountingStereoProvider() {
		channels = 2;
		num_samples = 1 << 21;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		++fill_calls;
		auto out = static_cast<int16_t *>(buf);
		for (int64_t i = 0; i < count; ++i) {
			out[i * 2 + 0] = static_cast<int16_t>(((start + i) * 17) % 32767);
			out[i * 2 + 1] = static_cast<int16_t>(-(((start + i) * 29) % 32768));
		}
	}
};

struct PatternMonoProvider final : agi::AudioProvider {
	explicit PatternMonoProvider(int64_t sample_count = 1 << 20) {
		channels = 1;
		num_samples = sample_count;
		decoded_samples = num_samples;
		sample_rate = 1000;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
	}

	static int16_t SampleAt(int64_t index) {
		return static_cast<int16_t>(((index * 7919 + 1237) % 60001) - 30000);
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		auto out = static_cast<int16_t *>(buf);
		for (int64_t i = 0; i < count; ++i)
			out[i] = SampleAt(start + i);
	}
};

struct ProgressiveMonoProvider final : agi::AudioProvider {
	mutable std::atomic<int> unread_requests{0};

	explicit ProgressiveMonoProvider(int64_t sample_count) {
		channels = 1;
		num_samples = sample_count;
		decoded_samples = 0;
		sample_rate = 1000;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
	}

	void Publish(int64_t count) { decoded_samples = count; }

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		const int64_t available = decoded_samples.load();
		if (start > available || count > available - start) {
			++unread_requests;
			throw agi::AudioDecodeError("audio samples are not ready");
		}
		auto *out = static_cast<int16_t *>(buf);
		for (int64_t i = 0; i < count; ++i)
			out[i] = PatternMonoProvider::SampleAt(start + i);
	}
};

struct RecoverableReadProvider final : agi::AudioProvider {
	mutable std::atomic<int> fill_calls{0};
	mutable std::atomic<int> failures_remaining{0};
	mutable std::atomic<int64_t> last_read_count{0};
	bool silent = false;

	explicit RecoverableReadProvider(int failures = 0, bool silent = false)
		: failures_remaining(failures), silent(silent) {
		channels = 1;
		num_samples = 1 << 20;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		++fill_calls;
		last_read_count = count;
		auto *samples = static_cast<int16_t *>(buf);
		if (failures_remaining > 0) {
			--failures_remaining;
			std::fill_n(samples, static_cast<size_t>(count / 2), int16_t{12345});
			throw agi::AudioDecodeError("injected partial audio read");
		}
		for (int64_t i = 0; i < count; ++i)
			samples[i] = silent ? 0 : PatternMonoProvider::SampleAt(start + i);
	}
};
struct ExtremeMetadataMonoProvider final : agi::AudioProvider {
	ExtremeMetadataMonoProvider() {
		channels = 1;
		num_samples = std::numeric_limits<int64_t>::max();
		decoded_samples = num_samples;
		sample_rate = 1;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
	}

	void FillBuffer(void *, int64_t, int64_t) const override {
	}
};

struct ChunkTrackingMonoProvider final : agi::AudioProvider {
	mutable std::atomic<int> fill_calls { 0 };
	mutable std::atomic<int64_t> max_frames_requested { 0 };

	ChunkTrackingMonoProvider() {
		channels = 1;
		sample_rate = 96000;
		num_samples = int64_t { 32 } * 2 * sample_rate;
		decoded_samples = num_samples;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
	}

	void FillBuffer(void *buf, int64_t, int64_t count) const override {
		++fill_calls;
		auto observed = max_frames_requested.load();
		while (observed < count
			&& !max_frames_requested.compare_exchange_weak(observed, count)) {
		}
		std::fill_n(static_cast<int16_t *>(buf), static_cast<size_t>(count), int16_t { 1234 });
	}
};

struct BlockingSpectrumProvider final : agi::AudioProvider {
	mutable std::mutex mutex;
	mutable std::condition_variable condition;
	mutable bool entered = false;
	mutable bool released = false;
	mutable std::atomic<int> fill_calls { 0 };

	BlockingSpectrumProvider() {
		channels = 2;
		num_samples = 1 << 21;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		++fill_calls;
		{
			std::unique_lock<std::mutex> lock(mutex);
			entered = true;
			condition.notify_all();
			condition.wait(lock, [&] { return released; });
		}
		auto out = static_cast<int16_t *>(buf);
		for (int64_t i = 0; i < count; ++i) {
			out[i * 2 + 0] = static_cast<int16_t>(((start + i) * 17) % 32767);
			out[i * 2 + 1] = static_cast<int16_t>(-(((start + i) * 29) % 32768));
		}
	}

	bool WaitUntilEntered(std::chrono::milliseconds timeout = std::chrono::seconds(2)) const {
		std::unique_lock<std::mutex> lock(mutex);
		return condition.wait_for(lock, timeout, [&] { return entered; });
	}

	void Release() const {
		{
			std::lock_guard<std::mutex> lock(mutex);
			released = true;
		}
		condition.notify_all();
	}
};

struct SequencedBlockingMonoProvider final : agi::AudioProvider {
	mutable std::mutex mutex;
	mutable std::condition_variable condition;
	mutable std::vector<int64_t> starts;
	mutable size_t released_calls = 0;
	mutable bool released_all = false;

	SequencedBlockingMonoProvider() {
		channels = 1;
		sample_rate = 96000;
		num_samples = int64_t { 3 } * AudioWaveformSummaryBlock::width * 2 * sample_rate;
		decoded_samples = num_samples;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		size_t call_number = 0;
		{
			std::unique_lock<std::mutex> lock(mutex);
			starts.push_back(start);
			call_number = starts.size();
			condition.notify_all();
			condition.wait(lock, [&] {
				return released_all || released_calls >= call_number;
			});
		}
		std::fill_n(static_cast<int16_t *>(buf), static_cast<size_t>(count), int16_t { 1234 });
	}

	bool WaitForCalls(
		size_t count,
		std::chrono::milliseconds timeout = std::chrono::seconds(2)) const {
		std::unique_lock<std::mutex> lock(mutex);
		return condition.wait_for(lock, timeout, [&] { return starts.size() >= count; });
	}

	int64_t StartAt(size_t index) const {
		std::lock_guard<std::mutex> lock(mutex);
		return starts.at(index);
	}

	std::vector<int64_t> ReadStarts() const {
		std::scoped_lock lock(mutex);
		return starts;
	}

	void ReleaseThrough(size_t count) const {
		{
			std::lock_guard<std::mutex> lock(mutex);
			released_calls = std::max(released_calls, count);
		}
		condition.notify_all();
	}

	void ReleaseAll() const {
		{
			std::lock_guard<std::mutex> lock(mutex);
			released_all = true;
		}
		condition.notify_all();
	}
};

bool WaitForBlockingProvider(BlockingSpectrumProvider const& provider) {
	const bool entered = provider.WaitUntilEntered();
	if (!entered)
		provider.Release();
	return entered;
}

TEST(lagi_audio_display, centered_scroll_position_places_time_at_viewport_center) {
	EXPECT_EQ(300, aegisub::audio::CenteredScrollLeft(10000, 400, 20.0));
	EXPECT_EQ(9800, aegisub::audio::CenteredScrollLeft(10000, 400, 1.0));
}

TEST(lagi_audio_display, centered_scroll_position_clamps_invalid_or_early_targets) {
	EXPECT_EQ(0, aegisub::audio::CenteredScrollLeft(-100, 400, 20.0));
	EXPECT_EQ(0, aegisub::audio::CenteredScrollLeft(1000, 0, 20.0));
	EXPECT_EQ(0, aegisub::audio::CenteredScrollLeft(1000, 400, 0.0));
	EXPECT_EQ(0, aegisub::audio::CenteredScrollLeft(1000, 400, std::numeric_limits<double>::infinity()));
}

bool WaitForSpectrumPrefetchBuilds(
	AudioSpectrumAnalysisCache const& cache,
	uint64_t expected,
	std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
	auto const deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline) {
		if (cache.GetMetricsSnapshot().prefetch_builds >= expected)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

bool WaitForWaveformPrefetchBuilds(
	AudioWaveformSummaryCache const& cache,
	uint64_t expected,
	std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
	auto const deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline) {
		if (cache.GetMetricsSnapshot().prefetch_builds >= expected)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

template<typename Cache>
bool WaitForVisibleLockContention(
	Cache const& cache,
	uint64_t expected = 1,
	std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
	auto const deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline) {
		if (cache.GetMetricsSnapshot().visible_lock_contention >= expected)
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return false;
}

int64_t LegacyWaveformColumnStart(size_t pixel_index, double samples_per_pixel) {
	const size_t block_first_pixel = pixel_index
		- pixel_index % AudioWaveformSummaryBlock::width;
	double current_sample = block_first_pixel * samples_per_pixel;
	for (size_t pixel = block_first_pixel; pixel < pixel_index; ++pixel)
		current_sample += samples_per_pixel;
	return static_cast<int64_t>(current_sample);
}

AudioWaveformPcm16Summary AnalyzeLegacyWaveformColumn(
	agi::AudioProvider &provider,
	size_t pixel_index,
	double samples_per_pixel) {
	const int64_t sample_count = static_cast<int64_t>(samples_per_pixel);
	const int64_t sample_start = LegacyWaveformColumnStart(pixel_index, samples_per_pixel);
	std::vector<int16_t> samples(static_cast<size_t>(std::max<int64_t>(0, sample_count)));
	if (!samples.empty())
		provider.GetInt16MonoAudio(samples.data(), sample_start, sample_count);

	AudioWaveformPcm16Summary summary;
	for (int16_t sample : samples) {
		if (sample > 0) {
			summary.peak_max = std::max(summary.peak_max, static_cast<int>(sample));
			summary.avg_max_accum += sample;
		}
		else {
			summary.peak_min = std::min(summary.peak_min, static_cast<int>(sample));
			summary.avg_min_accum += sample;
		}
	}
	return summary;
}
}

TEST(lagi_audio_display, display_source_converts_s16_stereo_to_float) {
	Int16StereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	ASSERT_TRUE(!!source);
	EXPECT_EQ(2, source->GetChannels());
	EXPECT_EQ(48000, source->GetSampleRate());

	float samples[4] = { 0.f, 0.f, 0.f, 0.f };
	source->GetFloatAudio(samples, 0, 2);
	EXPECT_NEAR(32767.0f / 32768.0f, samples[0], 1e-6f);
	EXPECT_NEAR(-1.0f, samples[1], 1e-6f);
	EXPECT_NEAR(16384.0f / 32768.0f, samples[2], 1e-6f);
	EXPECT_NEAR(-16384.0f / 32768.0f, samples[3], 1e-6f);
}

TEST(lagi_audio_display, int16_mono_display_source_uses_provider_mono_path) {
	Int16MonoProvider provider;
	auto source = CreateInt16MonoAudioDisplaySource(&provider);
	ASSERT_TRUE(!!source);
	EXPECT_EQ(1, source->GetChannels());
	EXPECT_EQ(48000, source->GetSampleRate());

	float samples[4] = { 0.f, 0.f, 0.f, 0.f };
	source->GetFloatAudio(samples, 0, 4);
	EXPECT_NEAR(32767.0f / 32768.0f, samples[0], 1e-6f);
	EXPECT_NEAR(-1.0f, samples[1], 1e-6f);
	EXPECT_NEAR(0.0f, samples[2], 1e-6f);
	EXPECT_NEAR(16384.0f / 32768.0f, samples[3], 1e-6f);
}

TEST(lagi_audio_display, display_source_preserves_float_samples) {
	FloatStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	ASSERT_TRUE(!!source);

	float samples[4] = { 0.f, 0.f, 0.f, 0.f };
	source->GetFloatAudio(samples, 0, 2);
	EXPECT_NEAR(0.25f, samples[0], 1e-6f);
	EXPECT_NEAR(-0.5f, samples[1], 1e-6f);
	EXPECT_NEAR(0.75f, samples[2], 1e-6f);
	EXPECT_NEAR(-1.0f, samples[3], 1e-6f);
}

TEST(lagi_audio_display, single_channel_display_source_extracts_requested_s16_channel) {
	Int16StereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	auto right = CreateSingleChannelAudioDisplaySource(source.get(), 1);
	ASSERT_TRUE(!!right);

	float samples[2] = { 0.f, 0.f };
	right->GetFloatAudio(samples, 0, 2);
	EXPECT_NEAR(-1.0f, samples[0], 1e-6f);
	EXPECT_NEAR(-16384.0f / 32768.0f, samples[1], 1e-6f);
}

TEST(lagi_audio_display, single_channel_display_source_extracts_requested_float_channel) {
	FloatStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	auto left = CreateSingleChannelAudioDisplaySource(source.get(), 0);
	ASSERT_TRUE(!!left);

	float samples[2] = { 0.f, 0.f };
	left->GetFloatAudio(samples, 0, 2);
	EXPECT_NEAR(0.25f, samples[0], 1e-6f);
	EXPECT_NEAR(0.75f, samples[1], 1e-6f);
}

TEST(lagi_audio_display, mix_policy_average_and_maxabs) {
	const float frame[] = { 0.25f, -0.75f, 0.5f };
	EXPECT_NEAR(0.0f, MixAudioFrameToMono(AudioMixPolicy::MonoAverage, frame, 3), 1e-6f);
	EXPECT_NEAR(-0.75f, MixAudioFrameToMono(AudioMixPolicy::MonoMaxAbs, frame, 3), 1e-6f);
}

TEST(lagi_audio_display, waveform_analysis_uses_requested_mix_policy) {
	const float interleaved[] = {
		0.25f, -0.5f,
		0.5f, -0.25f,
		-0.75f, 0.25f,
		0.1f, -0.9f,
	};

	auto average = AnalyzeWaveformInterleaved(interleaved, 4, 2, AudioMixPolicy::MonoAverage);
	auto maxabs = AnalyzeWaveformInterleaved(interleaved, 4, 2, AudioMixPolicy::MonoMaxAbs);

	EXPECT_LT(average.peak_max, maxabs.peak_max);
	EXPECT_GT(average.peak_min, maxabs.peak_min);
	EXPECT_NEAR(-0.9f, maxabs.peak_min, 1e-6f);
	EXPECT_NEAR(0.5f, maxabs.peak_max, 1e-6f);
}

TEST(lagi_audio_display, spectrum_power_merge_max_uses_strongest_channel_per_bin) {
	const float left[] = { 0.10f, 0.80f, 0.30f, 0.20f };
	const float right[] = { 0.50f, 0.20f, 0.70f, 0.40f };
	const std::vector<const float *> channels = { left, right };
	float merged[4] = { 0.f, 0.f, 0.f, 0.f };

	MergeSpectrumPowerBinsMax(channels, 4, merged);

	EXPECT_NEAR(0.50f, merged[0], 1e-6f);
	EXPECT_NEAR(0.80f, merged[1], 1e-6f);
	EXPECT_NEAR(0.70f, merged[2], 1e-6f);
	EXPECT_NEAR(0.40f, merged[3], 1e-6f);
}

TEST(lagi_audio_display, spectrum_power_merge_average_averages_each_bin) {
	const float left[] = { 0.10f, 0.80f, 0.30f, 0.20f };
	const float right[] = { 0.50f, 0.20f, 0.70f, 0.40f };
	const float center[] = { 0.40f, 0.10f, 0.20f, 0.90f };
	const std::vector<const float *> channels = { left, right, center };
	float merged[4] = { 0.f, 0.f, 0.f, 0.f };

	MergeSpectrumPowerBinsAverage(channels, 4, merged);

	EXPECT_NEAR((0.10f + 0.50f + 0.40f) / 3.0f, merged[0], 1e-6f);
	EXPECT_NEAR((0.80f + 0.20f + 0.10f) / 3.0f, merged[1], 1e-6f);
	EXPECT_NEAR((0.30f + 0.70f + 0.20f) / 3.0f, merged[2], 1e-6f);
	EXPECT_NEAR((0.20f + 0.40f + 0.90f) / 3.0f, merged[3], 1e-6f);
}

TEST(lagi_audio_display, waveform_summary_cache_reuses_hot_block) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);

	auto first = cache.Get(0);
	int calls_after_first = provider.fill_calls;
	auto second = cache.Get(0);

	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	EXPECT_EQ(1, calls_after_first);
	EXPECT_EQ(calls_after_first, provider.fill_calls);
	EXPECT_EQ(first->summaries[0].peak_max, second->summaries[0].peak_max);
}

TEST(lagi_audio_display, waveform_summary_cache_does_not_build_adjacent_blocks_implicitly) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);

	cache.Get(0);
	const int calls_after_first = provider.fill_calls;
	EXPECT_FALSE(cache.GetIfReady(1));
	cache.Get(1);

	EXPECT_EQ(1, calls_after_first);
	EXPECT_GT(provider.fill_calls, calls_after_first);
}

TEST(lagi_audio_display, waveform_summary_cache_invalidates_on_zoom_change) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);

	cache.Get(0);
	int calls_after_first = provider.fill_calls;
	cache.SetMillisecondsPerPixel(10.0);
	cache.Get(0);

	EXPECT_GT(provider.fill_calls, calls_after_first);
}

TEST(lagi_audio_display, waveform_summary_cache_metrics_count_hits_and_misses) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);

	cache.Get(0);
	cache.Get(0);
	auto metrics = cache.GetMetricsSnapshot();
	EXPECT_EQ(1u, metrics.cache_misses);
	EXPECT_EQ(1u, metrics.cache_hits);
	EXPECT_EQ(1u, metrics.visible_builds);
}

TEST(lagi_audio_display, waveform_summary_cache_prefetch_records_metrics) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);

	cache.Get(0);
	cache.Prefetch(40, 41);
	ASSERT_TRUE(WaitForWaveformPrefetchBuilds(cache, 2));
	cache.Get(40);
	auto metrics = cache.GetMetricsSnapshot();
	EXPECT_GE(metrics.prefetch_requests, 1u);
	EXPECT_GE(metrics.prefetch_builds, 1u);
}

TEST(lagi_audio_display, waveform_summary_cache_get_if_ready_becomes_available_after_prefetch) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);

	EXPECT_EQ(nullptr, cache.GetIfReady(0));
	EXPECT_EQ(0, provider.fill_calls);

	cache.Prefetch(0, 0);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
	AudioWaveformSummaryCache::BlockHandle block;
	while (!(block = cache.GetIfReady(0)) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));

	ASSERT_NE(nullptr, block);
	EXPECT_GT(provider.fill_calls, 0);
}

TEST(lagi_audio_display, waveform_summary_cache_get_if_ready_does_not_mutate_metrics) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetMixPolicy(AudioMixPolicy::MonoMaxAbs);

	cache.Get(0);
	auto const before = cache.GetMetricsSnapshot();
	EXPECT_NE(nullptr, cache.GetIfReady(0));
	EXPECT_EQ(nullptr, cache.GetIfReady(40));
	auto const after = cache.GetMetricsSnapshot();

	EXPECT_EQ(before.cache_hits, after.cache_hits);
	EXPECT_EQ(before.cache_misses, after.cache_misses);
}

TEST(lagi_audio_display, waveform_summary_cache_matches_legacy_non_integer_column_ranges) {
	PatternMonoProvider provider;
	auto source = CreateInt16MonoAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	double const samples_per_pixel = 1.3;
	cache.SetMillisecondsPerPixel(samples_per_pixel);

	auto first = cache.Get(0);
	auto second = cache.Get(1);
	ASSERT_TRUE(first);
	ASSERT_TRUE(second);
	ASSERT_TRUE(first->has_exact_pcm16);
	ASSERT_TRUE(second->has_exact_pcm16);

	EXPECT_NE(
		static_cast<int64_t>(30 * samples_per_pixel),
		LegacyWaveformColumnStart(30, samples_per_pixel));
	for (size_t pixel : { size_t { 0 }, size_t { 1 }, size_t { 2 }, size_t { 25 }, size_t { 30 }, size_t { 31 }, size_t { 32 }, size_t { 33 } }) {
		auto const expected = AnalyzeLegacyWaveformColumn(provider, pixel, samples_per_pixel);
		auto const& block = pixel < AudioWaveformSummaryBlock::width ? *first : *second;
		auto const& actual = block.pcm16_summaries[pixel % AudioWaveformSummaryBlock::width];
		EXPECT_EQ(expected.peak_min, actual.peak_min) << "pixel=" << pixel;
		EXPECT_EQ(expected.peak_max, actual.peak_max) << "pixel=" << pixel;
		EXPECT_EQ(expected.avg_min_accum, actual.avg_min_accum) << "pixel=" << pixel;
		EXPECT_EQ(expected.avg_max_accum, actual.avg_max_accum) << "pixel=" << pixel;
	}
}

TEST(lagi_audio_display, waveform_summary_cache_matches_legacy_short_file_and_eof_silence) {
	PatternMonoProvider provider(63);
	auto source = CreateInt16MonoAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	double const samples_per_pixel = 2.5;
	cache.SetMillisecondsPerPixel(samples_per_pixel);

	auto block = cache.Get(0);
	ASSERT_TRUE(block);
	ASSERT_TRUE(block->has_exact_pcm16);
	for (size_t pixel : { size_t { 0 }, size_t { 24 }, size_t { 25 }, size_t { 26 }, size_t { 31 } }) {
		auto const expected = AnalyzeLegacyWaveformColumn(provider, pixel, samples_per_pixel);
		auto const& actual = block->pcm16_summaries[pixel];
		EXPECT_EQ(expected.peak_min, actual.peak_min) << "pixel=" << pixel;
		EXPECT_EQ(expected.peak_max, actual.peak_max) << "pixel=" << pixel;
		EXPECT_EQ(expected.avg_min_accum, actual.avg_min_accum) << "pixel=" << pixel;
		EXPECT_EQ(expected.avg_max_accum, actual.avg_max_accum) << "pixel=" << pixel;
	}

	auto const& partial = block->pcm16_summaries[25];
	ASSERT_LT(PatternMonoProvider::SampleAt(62), 0);
	EXPECT_EQ(PatternMonoProvider::SampleAt(62), partial.peak_min);
	EXPECT_EQ(PatternMonoProvider::SampleAt(62), partial.avg_min_accum);
	auto const& silence = block->pcm16_summaries[26];
	EXPECT_EQ(0, silence.peak_min);
	EXPECT_EQ(0, silence.peak_max);
	EXPECT_EQ(0, silence.avg_min_accum);
	EXPECT_EQ(0, silence.avg_max_accum);
	EXPECT_FALSE(cache.Get(1));
}

TEST(lagi_audio_display, waveform_summary_cache_preserves_final_block_at_file_boundaries) {
	struct Case {
		int64_t samples;
		double pixel_ms;
		size_t blocks;
	};
	for (auto const& test : {Case{.samples = 0, .pixel_ms = 1.0, .blocks = 0}, Case{.samples = 1, .pixel_ms = 1.0, .blocks = 1}, Case{.samples = 31, .pixel_ms = 1.0, .blocks = 1},
							 Case{.samples = 32, .pixel_ms = 1.0, .blocks = 1}, Case{.samples = 33, .pixel_ms = 1.0, .blocks = 2}, Case{.samples = 64, .pixel_ms = 1.0, .blocks = 2},
							 Case{.samples = 65, .pixel_ms = 1.0, .blocks = 3}, Case{.samples = 1000, .pixel_ms = 20.0, .blocks = 2}}) {
		SCOPED_TRACE(test.samples);
		PatternMonoProvider provider(test.samples);
		auto source = CreateInt16MonoAudioDisplaySource(&provider);
		AudioWaveformSummaryCache cache;
		cache.SetSource(source.get());
		cache.SetMillisecondsPerPixel(test.pixel_ms);

		EXPECT_EQ(test.blocks, GetAudioDisplayBlockCount(test.samples, 1000, test.pixel_ms, 32));
		EXPECT_FALSE(cache.Get(test.blocks));
		if (test.blocks == 0) {
			EXPECT_FALSE(cache.IsReady());
			continue;
		}
		auto tail = cache.Get(test.blocks - 1);
		ASSERT_TRUE(tail);
		ASSERT_TRUE(tail->has_exact_pcm16);
		for (size_t column = 0; column < AudioWaveformSummaryBlock::width; ++column) {
			const size_t pixel = (test.blocks - 1) * AudioWaveformSummaryBlock::width + column;
			auto const expected = AnalyzeLegacyWaveformColumn(provider, pixel, test.pixel_ms);
			auto const& actual = tail->pcm16_summaries[column];
			EXPECT_EQ(expected.peak_min, actual.peak_min) << "pixel=" << pixel;
			EXPECT_EQ(expected.peak_max, actual.peak_max) << "pixel=" << pixel;
			EXPECT_EQ(expected.avg_min_accum, actual.avg_min_accum) << "pixel=" << pixel;
			EXPECT_EQ(expected.avg_max_accum, actual.avg_max_accum) << "pixel=" << pixel;
		}
	}
}

TEST(lagi_audio_display, legacy_render_length_exposes_partial_tail_only_after_decoding_finishes) {
	EXPECT_EQ(0, GetAudioDisplayRenderLength(0, 64, 1, 0, 1000, 1.0, 32));
	EXPECT_EQ(1, GetAudioDisplayRenderLength(0, 64, 1, 1, 1000, 1.0, 32));
	EXPECT_EQ(0, GetAudioDisplayRenderLength(0, 64, 33, 31, 1000, 1.0, 32));
	EXPECT_EQ(32, GetAudioDisplayRenderLength(0, 64, 33, 32, 1000, 1.0, 32));
	EXPECT_EQ(33, GetAudioDisplayRenderLength(0, 64, 33, 33, 1000, 1.0, 32));
	EXPECT_EQ(32, GetAudioDisplayRenderLength(0, 64, 64, 63, 1000, 1.0, 32));
	EXPECT_EQ(64, GetAudioDisplayRenderLength(0, 96, 64, 64, 1000, 1.0, 32));
	EXPECT_EQ(32, GetAudioDisplayRenderLength(0, 64, 1000, 999, 1000, 20.0, 32));
	EXPECT_EQ(50, GetAudioDisplayRenderLength(0, 64, 1000, 1000, 1000, 20.0, 32));
}

TEST(lagi_audio_display, legacy_render_length_clips_audio_tail_across_viewports) {
	// A 50-column file must retain all 18 columns of its second bitmap, but
	// no part of the bitmap after column 49 may be drawn as audio.
	EXPECT_EQ(50, GetAudioDisplayRenderLength(0, 96, 1000, 1000, 1000, 20.0, 32));
	EXPECT_EQ(19, GetAudioDisplayRenderLength(31, 96, 1000, 1000, 1000, 20.0, 32));
	EXPECT_EQ(18, GetAudioDisplayRenderLength(32, 96, 1000, 1000, 1000, 20.0, 32));
	EXPECT_EQ(1, GetAudioDisplayRenderLength(49, 96, 1000, 1000, 1000, 20.0, 32));
	EXPECT_EQ(0, GetAudioDisplayRenderLength(50, 96, 1000, 1000, 1000, 20.0, 32));
	EXPECT_EQ(0, GetAudioDisplayRenderLength(64, 96, 1000, 1000, 1000, 20.0, 32));
	EXPECT_EQ(7, GetAudioDisplayRenderLength(31, 7, 1000, 1000, 1000, 20.0, 32));
	// A partial final pixel is visible, and decoding metadata cannot extend
	// the visible duration beyond the actual file.
	EXPECT_EQ(1, GetAudioDisplayRenderLength(50, 96, 1001, 1001, 1000, 20.0, 32));
	EXPECT_EQ(0, GetAudioDisplayRenderLength(51, 96, 1001, 1001, 1000, 20.0, 32));
	EXPECT_EQ(50, GetAudioDisplayRenderLength(0, 96, 1000, 2000, 1000, 20.0, 32));
	EXPECT_EQ(0, GetAudioDisplayRenderLength(0, 96, 0, 0, 1000, 20.0, 32));
}

TEST(lagi_audio_display, spectrum_ready_ranges_require_window_lookahead_until_complete_eof) {
	EXPECT_EQ(0, GetAudioDisplayReadySamples(2000, 511, 512));
	EXPECT_EQ(0, GetAudioDisplayReadySamples(2000, 512, 512));
	EXPECT_EQ(1, GetAudioDisplayReadySamples(2000, 513, 512));
	EXPECT_EQ(1487, GetAudioDisplayReadySamples(2000, 1999, 512));
	EXPECT_EQ(2000, GetAudioDisplayReadySamples(2000, 2000, 512));
	EXPECT_EQ(1000, GetAudioDisplayReadySamples(2000, 1000, 0));

	EXPECT_EQ(0u, GetAudioSpectrumReadyBlockCount(2000, 511, 512, 128));
	EXPECT_EQ(1u, GetAudioSpectrumReadyBlockCount(2000, 512, 512, 128));
	EXPECT_EQ(1u, GetAudioSpectrumReadyBlockCount(2000, 639, 512, 128));
	EXPECT_EQ(2u, GetAudioSpectrumReadyBlockCount(2000, 640, 512, 128));
	EXPECT_EQ(12u, GetAudioSpectrumReadyBlockCount(2000, 1999, 512, 128));
	EXPECT_EQ(16u, GetAudioSpectrumReadyBlockCount(2000, 2000, 512, 128));
	EXPECT_EQ(16u, GetAudioSpectrumReadyBlockCount(2048, 2048, 512, 128));
	EXPECT_EQ(0u, GetAudioSpectrumReadyBlockCount(1, 0, 512, 128));
	EXPECT_EQ(1u, GetAudioSpectrumReadyBlockCount(1, 1, 512, 128));

	auto const before = GetAudioDisplayReadySamples(2000, 1151, 512);
	auto const after = GetAudioDisplayReadySamples(2000, 1152, 512);
	EXPECT_EQ(0, GetAudioDisplayRenderLength(0, 64, 2000, before, 1000, 20.0, 32));
	EXPECT_EQ(32, GetAudioDisplayRenderLength(0, 64, 2000, after, 1000, 20.0, 32));
	EXPECT_EQ(100, GetAudioDisplayRenderLength(0, 128, 2000,
											   GetAudioDisplayReadySamples(2000, 2000, 512), 1000, 20.0, 32));
}

TEST(lagi_audio_display, spectrum_prefetch_ready_ranges_never_read_unprocessed_samples) {
	ProgressiveMonoProvider provider(2000);
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetResolution(9, 7);
	PatternMonoProvider complete_provider(2000);
	auto complete_source = CreateAudioDisplaySource(&complete_provider);
	AudioSpectrumAnalysisCache reference;
	reference.SetSource(complete_source.get());
	reference.SetResolution(9, 7);

	struct Stage {
		int64_t decoded;
		size_t ready_blocks;
	};
	for (auto const& stage : {Stage{.decoded = 512, .ready_blocks = 1},
							  Stage{.decoded = 640, .ready_blocks = 2}, Stage{.decoded = 1999, .ready_blocks = 12},
							  Stage{.decoded = 2000, .ready_blocks = 16}}) {
		SCOPED_TRACE(stage.decoded);
		provider.Publish(stage.decoded);
		const size_t ready_blocks = GetAudioSpectrumReadyBlockCount(
			provider.GetNumSamples(), provider.GetDecodedSamples(), 512, 128);
		ASSERT_EQ(stage.ready_blocks, ready_blocks);
		cache.Prefetch(0, ready_blocks - 1);
		ASSERT_TRUE(WaitForSpectrumPrefetchBuilds(cache, ready_blocks));
		EXPECT_EQ(0, provider.unread_requests.load());
		EXPECT_EQ(nullptr, cache.GetIfReady(ready_blocks));
		auto const tail = cache.GetIfReady(ready_blocks - 1);
		auto const expected = reference.Get(ready_blocks - 1);
		ASSERT_NE(nullptr, tail);
		ASSERT_NE(nullptr, expected);
		for (size_t bin = 0; bin < 512; ++bin)
			EXPECT_FLOAT_EQ(expected[bin], tail[bin]) << "bin=" << bin;
	}
}

TEST(lagi_audio_display, waveform_summary_cache_chunks_extreme_zoom_reads) {
	ChunkTrackingMonoProvider provider;
	auto source = CreateInt16MonoAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(2000.0);

	auto block = cache.Get(0);
	ASSERT_TRUE(block);
	ASSERT_TRUE(block->has_exact_pcm16);
	EXPECT_GT(provider.fill_calls.load(), 1);
	EXPECT_LE(provider.max_frames_requested.load(), int64_t { 2 } * provider.GetSampleRate());
	for (size_t column : { size_t { 0 }, size_t { 15 }, size_t { 31 } }) {
		auto const& summary = block->pcm16_summaries[column];
		EXPECT_EQ(0, summary.peak_min);
		EXPECT_EQ(1234, summary.peak_max);
		EXPECT_EQ(0, summary.avg_min_accum);
		EXPECT_EQ(int64_t { 1234 } * 192000, summary.avg_max_accum);
	}
}

TEST(lagi_audio_display, waveform_summary_cache_sub_sample_columns_do_not_read_provider) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(0.01);

	auto block = cache.Get(0);
	ASSERT_TRUE(block);
	EXPECT_EQ(0, provider.fill_calls);
	for (auto const& summary : block->summaries) {
		EXPECT_EQ(0.0f, summary.peak_min);
		EXPECT_EQ(0.0f, summary.peak_max);
		EXPECT_EQ(0.0f, summary.avg_min);
		EXPECT_EQ(0.0f, summary.avg_max);
	}
}

TEST(lagi_audio_display, waveform_summary_cache_rejects_rounded_size_t_block_count) {
	ExtremeMetadataMonoProvider provider;
	auto source = CreateInt16MonoAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());

	EXPECT_NO_THROW(cache.SetMillisecondsPerPixel(15.625));
	EXPECT_FALSE(cache.IsReady());
	auto const metrics = cache.GetMetricsSnapshot();
	EXPECT_EQ(0u, metrics.cache_entries);
	EXPECT_EQ(0u, metrics.cache_bytes);
}

TEST(lagi_audio_display, waveform_summary_cache_preserves_provider_int16_mono_downmix) {
	Int16StereoProvider provider;
	auto source = CreateInt16MonoAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(4.0 * 1000.0 / provider.GetSampleRate());

	auto block = cache.Get(0);
	ASSERT_TRUE(block);
	ASSERT_TRUE(block->has_exact_pcm16);
	auto const expected = AnalyzeLegacyWaveformColumn(provider, 0, 4.0);
	auto const& actual = block->pcm16_summaries[0];
	EXPECT_EQ(expected.peak_min, actual.peak_min);
	EXPECT_EQ(expected.peak_max, actual.peak_max);
	EXPECT_EQ(expected.avg_min_accum, actual.avg_min_accum);
	EXPECT_EQ(expected.avg_max_accum, actual.avg_max_accum);
}

TEST(lagi_audio_display, waveform_summary_cache_handle_survives_lru_eviction) {
	PatternMonoProvider provider;
	auto source = CreateInt16MonoAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(2.5);
	cache.Age(sizeof(AudioWaveformSummaryBlock));

	auto first = cache.Get(0);
	ASSERT_TRUE(first);
	auto const first_value = first->pcm16_summaries[0].peak_min;
	ASSERT_TRUE(cache.Get(8));
	EXPECT_FALSE(cache.GetIfReady(0));
	EXPECT_EQ(first_value, first->pcm16_summaries[0].peak_min);
}

TEST(lagi_audio_display, waveform_summary_cache_compacts_stale_lru_touches) {
	PatternMonoProvider provider;
	auto source = CreateInt16MonoAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(2.5);
	ASSERT_TRUE(cache.Get(0));

	for (size_t i = 0; i < 4096; ++i)
		ASSERT_TRUE(cache.Get(0));

	auto const metrics = cache.GetMetricsSnapshot();
	EXPECT_EQ(1u, metrics.cache_entries);
	EXPECT_LE(metrics.cache_touch_entries, 1024u);
}

TEST(lagi_audio_display, waveform_summary_cache_prefetch_caps_sparse_range_at_both_edges) {
	PatternMonoProvider provider;
	auto source = CreateInt16MonoAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(2.5);
	cache.SetPrefetchBuildMaxBlocks(4);

	cache.Prefetch(10, 300);
	ASSERT_TRUE(WaitForWaveformPrefetchBuilds(cache, 4));
	EXPECT_TRUE(cache.GetIfReady(10));
	EXPECT_TRUE(cache.GetIfReady(11));
	EXPECT_FALSE(cache.GetIfReady(12));
	EXPECT_FALSE(cache.GetIfReady(298));
	EXPECT_TRUE(cache.GetIfReady(299));
	EXPECT_TRUE(cache.GetIfReady(300));
	EXPECT_EQ(4u, cache.GetMetricsSnapshot().prefetch_builds);
}

TEST(lagi_audio_display, waveform_summary_cache_visible_get_shares_inflight_prefetch_build) {
	ScopedTestDeadline deadline("waveform visible/prefetch shared build");
	BlockingSpectrumProvider provider;
	auto source = CreateInt16MonoAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);

	cache.Prefetch(0, 0);
	ASSERT_TRUE(WaitForBlockingProvider(provider));
	auto visible = std::async(std::launch::async, [&] { return cache.Get(0); });
	if (!WaitForVisibleLockContention(cache)) {
		provider.Release();
		EXPECT_EQ(std::future_status::ready, visible.wait_for(std::chrono::seconds(2)));
		FAIL() << "visible waveform miss did not register build-lock contention";
	}
	EXPECT_EQ(std::future_status::timeout, visible.wait_for(std::chrono::milliseconds(0)));
	provider.Release();
	ASSERT_EQ(std::future_status::ready, visible.wait_for(std::chrono::seconds(2)));
	EXPECT_TRUE(visible.get());
	EXPECT_EQ(1, provider.fill_calls.load());
	auto const metrics = cache.GetMetricsSnapshot();
	EXPECT_EQ(1u, metrics.prefetch_builds);
	EXPECT_GE(metrics.visible_lock_contention, 1u);
	EXPECT_GE(metrics.cache_hits, 1u);
}

TEST(lagi_audio_display, waveform_summary_cache_visible_miss_preempts_remaining_prefetch) {
	ScopedTestDeadline deadline("waveform visible cross-block preemption");
	SequencedBlockingMonoProvider provider;
	auto source = CreateInt16MonoAudioDisplaySource(&provider);
	std::promise<void> prefetch_finished;
	auto finished = prefetch_finished.get_future();
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	cache.SetReadyCallback([&] { prefetch_finished.set_value(); });
	std::future<AudioWaveformSummaryCache::BlockHandle> visible;
	auto release = agi::make_scope_exit([&] { provider.ReleaseAll(); });

	cache.Prefetch(0, 3);
	ASSERT_TRUE(provider.WaitForCalls(1));
	visible = std::async(std::launch::async, [&] { return cache.Get(3); });
	ASSERT_TRUE(WaitForVisibleLockContention(cache));
	EXPECT_EQ(std::future_status::timeout, visible.wait_for(std::chrono::milliseconds(0)));
	provider.ReleaseThrough(1);
	ASSERT_TRUE(provider.WaitForCalls(2));
	const int64_t samples_per_block = AudioWaveformSummaryBlock::width * provider.GetSampleRate() / 50;
	EXPECT_EQ((std::vector<int64_t>{0, 3 * samples_per_block}), provider.ReadStarts());
	ASSERT_EQ(std::future_status::ready, finished.wait_for(std::chrono::seconds(2)));
	EXPECT_EQ(std::future_status::timeout, visible.wait_for(std::chrono::milliseconds(0)));
	EXPECT_TRUE(cache.GetIfReady(0));
	EXPECT_FALSE(cache.GetIfReady(1));
	EXPECT_FALSE(cache.GetIfReady(2));
	EXPECT_FALSE(cache.GetIfReady(3));
	auto const pending = cache.GetMetricsSnapshot();
	EXPECT_EQ(1u, pending.prefetch_builds);
	EXPECT_EQ(0u, pending.visible_builds);
	EXPECT_EQ(1u, pending.prefetch_busy_skips);

	provider.ReleaseAll();
	ASSERT_EQ(std::future_status::ready, visible.wait_for(std::chrono::seconds(2)));
	EXPECT_TRUE(visible.get());
	EXPECT_EQ((std::vector<int64_t>{0, 3 * samples_per_block}), provider.ReadStarts());
	EXPECT_TRUE(cache.GetIfReady(0));
	EXPECT_TRUE(cache.GetIfReady(3));
	EXPECT_FALSE(cache.GetIfReady(1));
	EXPECT_FALSE(cache.GetIfReady(2));
	auto const metrics = cache.GetMetricsSnapshot();
	EXPECT_EQ(1u, metrics.prefetch_builds);
	EXPECT_EQ(1u, metrics.visible_builds);
	EXPECT_EQ(1u, metrics.prefetch_busy_skips);
}
TEST(lagi_audio_display, waveform_summary_cache_visible_miss_preempts_current_extreme_zoom_block) {
	ScopedTestDeadline deadline("waveform extreme-zoom current-block preemption");
	SequencedBlockingMonoProvider provider;
	auto source = CreateInt16MonoAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(2000.0);

	cache.Prefetch(0, 0);
	if (!provider.WaitForCalls(1)) {
		provider.ReleaseAll();
		FAIL() << "waveform prefetch did not enter its first provider read";
	}
	auto visible = std::async(std::launch::async, [&] { return cache.Get(1); });
	if (!WaitForVisibleLockContention(cache)) {
		provider.ReleaseAll();
		EXPECT_EQ(std::future_status::ready, visible.wait_for(std::chrono::seconds(2)));
		FAIL() << "visible waveform miss did not register build-lock contention";
	}
	EXPECT_EQ(std::future_status::timeout, visible.wait_for(std::chrono::milliseconds(0)));
	provider.ReleaseThrough(1);
	if (!provider.WaitForCalls(2)) {
		provider.ReleaseAll();
		EXPECT_EQ(std::future_status::ready, visible.wait_for(std::chrono::seconds(2)));
		FAIL() << "visible waveform miss did not reach the provider";
	}

	const int64_t expected_visible_start = static_cast<int64_t>(
		AudioWaveformSummaryBlock::width) * 2 * provider.GetSampleRate();
	EXPECT_EQ(expected_visible_start, provider.StartAt(1));
	provider.ReleaseAll();
	ASSERT_EQ(std::future_status::ready, visible.wait_for(std::chrono::seconds(2)));
	EXPECT_TRUE(visible.get());
	EXPECT_FALSE(cache.GetIfReady(0));
	EXPECT_GE(cache.GetMetricsSnapshot().prefetch_busy_skips, 1u);
}

TEST(lagi_audio_display, waveform_summary_cache_latest_range_drops_obsolete_result) {
	ScopedTestDeadline deadline("waveform latest-range cancellation");
	BlockingSpectrumProvider provider;
	auto source = CreateInt16MonoAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);

	cache.Prefetch(0, 0);
	ASSERT_TRUE(WaitForBlockingProvider(provider));
	cache.Prefetch(50, 50);
	provider.Release();
	ASSERT_TRUE(WaitForWaveformPrefetchBuilds(cache, 1));
	EXPECT_FALSE(cache.GetIfReady(0));
	EXPECT_TRUE(cache.GetIfReady(50));
	EXPECT_EQ(2, provider.fill_calls.load());
	auto const metrics = cache.GetMetricsSnapshot();
	EXPECT_EQ(1u, metrics.prefetch_builds);
	EXPECT_GE(metrics.stale_drops, 1u);
}

TEST(lagi_audio_display, waveform_summary_cache_provider_detach_drops_inflight_prefetch) {
	ScopedTestDeadline deadline("waveform provider detach");
	BlockingSpectrumProvider provider;
	auto source = CreateInt16MonoAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);

	cache.Prefetch(0, 0);
	ASSERT_TRUE(WaitForBlockingProvider(provider));
	auto detach = std::async(std::launch::async, [&] { cache.SetSource(nullptr); });
	EXPECT_EQ(std::future_status::timeout, detach.wait_for(std::chrono::milliseconds(50)));
	provider.Release();
	ASSERT_EQ(std::future_status::ready, detach.wait_for(std::chrono::seconds(2)));
	detach.get();
	source.reset();
	EXPECT_FALSE(cache.GetIfReady(0));
	EXPECT_EQ(0u, cache.GetMetricsSnapshot().prefetch_builds);
	EXPECT_GE(cache.GetMetricsSnapshot().stale_drops, 1u);
}

TEST(lagi_audio_display, waveform_summary_cache_nonzero_age_does_not_wait_for_inflight_prefetch) {
	ScopedTestDeadline deadline("waveform nonzero cache aging");
	BlockingSpectrumProvider provider;
	auto source = CreateInt16MonoAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);

	cache.Prefetch(0, 0);
	ASSERT_TRUE(WaitForBlockingProvider(provider));
	auto age = std::async(std::launch::async, [&] {
		cache.Age(sizeof(AudioWaveformSummaryBlock));
	});
	if (age.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
		provider.Release();
		FAIL() << "nonzero cache aging waited for the in-flight provider read";
	}
	age.get();
	EXPECT_EQ(sizeof(AudioWaveformSummaryBlock), cache.GetMetricsSnapshot().cache_budget_bytes);
	provider.Release();
}

TEST(lagi_audio_display, waveform_summary_cache_zoom_change_drops_inflight_prefetch) {
	ScopedTestDeadline deadline("waveform zoom cancellation");
	BlockingSpectrumProvider provider;
	auto source = CreateInt16MonoAudioDisplaySource(&provider);
	AudioWaveformSummaryCache cache;
	cache.SetSource(source.get());
	cache.SetMillisecondsPerPixel(20.0);
	auto const generation_before = cache.GetMetricsSnapshot().generation;

	cache.Prefetch(0, 0);
	ASSERT_TRUE(WaitForBlockingProvider(provider));
	auto change_zoom = std::async(std::launch::async, [&] {
		cache.SetMillisecondsPerPixel(10.0);
	});
	EXPECT_EQ(std::future_status::timeout, change_zoom.wait_for(std::chrono::milliseconds(50)));
	provider.Release();
	ASSERT_EQ(std::future_status::ready, change_zoom.wait_for(std::chrono::seconds(2)));
	change_zoom.get();

	auto const after_change = cache.GetMetricsSnapshot();
	EXPECT_GT(after_change.generation, generation_before);
	EXPECT_FALSE(cache.GetIfReady(0));
	EXPECT_EQ(0u, after_change.cache_entries);
	EXPECT_EQ(0u, after_change.cache_bytes);
	EXPECT_EQ(0u, after_change.prefetch_builds);
	EXPECT_GE(after_change.stale_drops, 1u);

	cache.Prefetch(0, 0);
	ASSERT_TRUE(WaitForWaveformPrefetchBuilds(cache, 1));
	auto const rebuilt = cache.GetIfReady(0);
	ASSERT_TRUE(rebuilt);
	EXPECT_TRUE(rebuilt->has_exact_pcm16);
	EXPECT_EQ(2, provider.fill_calls.load());
}

TEST(lagi_audio_display, waveform_summary_column_refs_respect_sub_block_offsets) {
	auto refs = BuildWaveformSummaryColumnRefs(8, 40);

	ASSERT_EQ(40u, refs.size());
	EXPECT_EQ(0u, refs[0].block_index);
	EXPECT_EQ(8u, refs[0].summary_index);
	EXPECT_EQ(0u, refs[23].block_index);
	EXPECT_EQ(31u, refs[23].summary_index);
	EXPECT_EQ(1u, refs[24].block_index);
	EXPECT_EQ(0u, refs[24].summary_index);
	EXPECT_EQ(1u, refs[39].block_index);
	EXPECT_EQ(15u, refs[39].summary_index);
}

TEST(lagi_audio_display, waveform_summary_column_refs_cover_multi_block_tiles) {
	auto refs = BuildWaveformSummaryColumnRefs(0, 96);

	ASSERT_EQ(96u, refs.size());
	EXPECT_EQ(0u, refs[0].block_index);
	EXPECT_EQ(0u, refs[0].summary_index);
	EXPECT_EQ(0u, refs[31].block_index);
	EXPECT_EQ(31u, refs[31].summary_index);
	EXPECT_EQ(1u, refs[32].block_index);
	EXPECT_EQ(0u, refs[32].summary_index);
	EXPECT_EQ(2u, refs[64].block_index);
	EXPECT_EQ(0u, refs[64].summary_index);
	EXPECT_EQ(2u, refs[95].block_index);
	EXPECT_EQ(31u, refs[95].summary_index);
}

TEST(lagi_audio_display, waveform_prefetch_blocks_exclude_incomplete_decoded_tail) {
	auto const range = PlanWaveformPrefetchBlocks(
		9 * static_cast<int>(AudioWaveformSummaryBlock::width),
		2 * static_cast<int>(AudioWaveformSummaryBlock::width),
		10 * static_cast<int64_t>(AudioWaveformSummaryBlock::width) + 31,
		1000,
		1000,
		1.0);
	ASSERT_TRUE(range);
	EXPECT_EQ(1u, range->first);
	EXPECT_EQ(9u, range->last);

	auto const tail_margin = PlanWaveformPrefetchBlocks(
		10 * static_cast<int>(AudioWaveformSummaryBlock::width),
		static_cast<int>(AudioWaveformSummaryBlock::width),
		10 * static_cast<int64_t>(AudioWaveformSummaryBlock::width) + 31,
		1000,
		1000,
		1.0);
	ASSERT_TRUE(tail_margin);
	EXPECT_EQ(2u, tail_margin->first);
	EXPECT_EQ(9u, tail_margin->last);
	EXPECT_FALSE(PlanWaveformPrefetchBlocks(
		10 * static_cast<int>(AudioWaveformSummaryBlock::width),
		static_cast<int>(AudioWaveformSummaryBlock::width),
		10 * static_cast<int64_t>(AudioWaveformSummaryBlock::width) + 31,
		1000,
		1000,
		1.0,
		0));
}

TEST(lagi_audio_display, waveform_prefetch_blocks_use_exact_legacy_column_end) {
	constexpr double pixel_ms = 16.0;
	constexpr int sample_rate = 44100;
	const double samples_per_pixel = pixel_ms * sample_rate / 1000.0;
	auto const sample_end = GetWaveformSummaryBlockSampleEnd(0, samples_per_pixel);
	ASSERT_TRUE(sample_end);
	EXPECT_EQ(22578, *sample_end);

	auto const exact = PlanWaveformPrefetchBlocks(
		0,
		static_cast<int>(AudioWaveformSummaryBlock::width),
		*sample_end,
		*sample_end + 1,
		sample_rate,
		pixel_ms,
		0);
	ASSERT_TRUE(exact);
	EXPECT_EQ(0u, exact->first);
	EXPECT_EQ(0u, exact->last);
	EXPECT_FALSE(PlanWaveformPrefetchBlocks(
		0,
		static_cast<int>(AudioWaveformSummaryBlock::width),
		*sample_end - 1,
		*sample_end + 1,
		sample_rate,
		pixel_ms,
		0));

	auto const rounded_size_limit = PlanWaveformPrefetchBlocks(
		std::numeric_limits<int>::max(),
		1,
		std::numeric_limits<int64_t>::max(),
		std::numeric_limits<int64_t>::max(),
		1,
		15.625,
		0);
	ASSERT_TRUE(rounded_size_limit);
	const size_t expected_block = static_cast<size_t>(std::numeric_limits<int>::max())
		/ AudioWaveformSummaryBlock::width;
	EXPECT_EQ(expected_block, rounded_size_limit->first);
	EXPECT_EQ(expected_block, rounded_size_limit->last);
}

TEST(lagi_audio_display, waveform_prefetch_blocks_include_completed_partial_tail_without_exceeding_eof) {
	EXPECT_FALSE(PlanWaveformPrefetchBlocks(0, 64, 0, 1, 1000, 1.0, 0));
	auto const short_file = PlanWaveformPrefetchBlocks(0, 64, 1, 1, 1000, 1.0, 0);
	ASSERT_TRUE(short_file);
	EXPECT_EQ(0u, short_file->first);
	EXPECT_EQ(0u, short_file->last);

	auto const incomplete = PlanWaveformPrefetchBlocks(0, 64, 32, 33, 1000, 1.0, 0);
	ASSERT_TRUE(incomplete);
	EXPECT_EQ(0u, incomplete->first);
	EXPECT_EQ(0u, incomplete->last);
	EXPECT_FALSE(PlanWaveformPrefetchBlocks(32, 32, 32, 33, 1000, 1.0, 0));

	auto const complete = PlanWaveformPrefetchBlocks(31, 64, 33, 33, 1000, 1.0, 0);
	ASSERT_TRUE(complete);
	EXPECT_EQ(0u, complete->first);
	EXPECT_EQ(1u, complete->last);
	auto const tail = PlanWaveformPrefetchBlocks(32, 64, 33, 33, 1000, 1.0, 0);
	ASSERT_TRUE(tail);
	EXPECT_EQ(1u, tail->first);
	EXPECT_EQ(1u, tail->last);
	EXPECT_FALSE(PlanWaveformPrefetchBlocks(64, 32, 33, 33, 1000, 1.0, 0));

	auto const exact = PlanWaveformPrefetchBlocks(0, 96, 64, 64, 1000, 1.0, 0);
	ASSERT_TRUE(exact);
	EXPECT_EQ(0u, exact->first);
	EXPECT_EQ(1u, exact->last);
	EXPECT_FALSE(PlanWaveformPrefetchBlocks(64, 32, 64, 64, 1000, 1.0, 0));
}

TEST(lagi_audio_display, latest_range_scheduler_request_increments_generation) {
	AudioLatestRangeScheduler scheduler([](size_t, size_t, uint64_t) {});
	const uint64_t before = scheduler.CurrentGeneration();
	scheduler.Request(0, 1);
	const uint64_t after = scheduler.CurrentGeneration();

	EXPECT_GT(after, before);
	EXPECT_TRUE(scheduler.IsCurrent(after));
	EXPECT_FALSE(scheduler.IsCurrent(before));
}

TEST(lagi_audio_display, latest_range_scheduler_invalidate_bumps_generation) {
	AudioLatestRangeScheduler scheduler([](size_t, size_t, uint64_t) {});
	scheduler.Request(2, 3);
	const uint64_t after_request = scheduler.CurrentGeneration();
	scheduler.Invalidate();
	const uint64_t after_invalidate = scheduler.CurrentGeneration();

	EXPECT_GT(after_invalidate, after_request);
	EXPECT_TRUE(scheduler.IsCurrent(after_invalidate));
	EXPECT_FALSE(scheduler.IsCurrent(after_request));
}

TEST(lagi_audio_display, spectrum_analysis_cache_reuses_hot_block) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	auto first = cache.Get(0);
	int calls_after_first = provider.fill_calls;
	auto second = cache.Get(0);

	EXPECT_GT(calls_after_first, 0);
	EXPECT_EQ(calls_after_first, provider.fill_calls);
	EXPECT_EQ(first[0], second[0]);
}

TEST(lagi_audio_display, spectrum_analysis_cache_does_not_build_adjacent_blocks_implicitly) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	cache.Get(0);
	const int calls_after_first = provider.fill_calls;
	EXPECT_EQ(nullptr, cache.GetIfReady(1));
	cache.Get(1);

	EXPECT_EQ(1, calls_after_first);
	EXPECT_GT(provider.fill_calls, calls_after_first);
}

TEST(lagi_audio_display, spectrum_analysis_cache_metrics_count_hits_and_misses) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	cache.Get(0);
	cache.Get(0);
	auto metrics = cache.GetMetricsSnapshot();
	EXPECT_EQ(1u, metrics.cache_misses);
	EXPECT_EQ(1u, metrics.cache_hits);
	EXPECT_EQ(1u, metrics.visible_builds);
}

TEST(lagi_audio_display, spectrum_analysis_cache_resolution_change_still_keeps_one_block) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(5, 5);
	cache.Age(sizeof(float) * (size_t(1) << 5));

	cache.SetResolution(9, 7);
	auto block = cache.Get(0);

	ASSERT_NE(nullptr, block);
	EXPECT_TRUE(std::isfinite(block[0]));
}

TEST(lagi_audio_display, spectrum_analysis_cache_prefetch_builds_and_visible_get_hits) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	cache.Prefetch(300, 301);
	ASSERT_TRUE(WaitForSpectrumPrefetchBuilds(cache, 2));
	ASSERT_TRUE(cache.AreBlocksReady(300, 301));
	auto const calls_after_prefetch = provider.fill_calls;
	auto block = cache.Get(300);
	ASSERT_NE(nullptr, block);
	EXPECT_EQ(calls_after_prefetch, provider.fill_calls);
	auto metrics = cache.GetMetricsSnapshot();
	EXPECT_GE(metrics.prefetch_requests, 1u);
	EXPECT_EQ(2u, metrics.prefetch_builds);
	EXPECT_EQ(0u, metrics.visible_builds);
	EXPECT_GE(metrics.cache_hits, 1u);
}

TEST(lagi_audio_display, spectrum_analysis_cache_prefetch_notifies_after_publish) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);
	std::atomic<int> ready_callbacks { 0 };
	cache.SetReadyCallback([&] { ++ready_callbacks; });

	EXPECT_EQ(nullptr, cache.GetIfReady(0));
	EXPECT_EQ(0, provider.fill_calls);
	cache.Prefetch(0, 0);
	ASSERT_TRUE(WaitForSpectrumPrefetchBuilds(cache, 1));
	auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (ready_callbacks.load() == 0 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	auto block = cache.GetIfReady(0);
	ASSERT_NE(nullptr, block);
	EXPECT_GT(provider.fill_calls, 0);
	EXPECT_EQ(1, ready_callbacks.load());
}

TEST(lagi_audio_display, spectrum_analysis_cache_get_if_ready_does_not_mutate_metrics) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	cache.Get(0);
	auto const before = cache.GetMetricsSnapshot();
	EXPECT_NE(nullptr, cache.GetIfReady(0));
	EXPECT_EQ(nullptr, cache.GetIfReady(300));
	auto const after = cache.GetMetricsSnapshot();

	EXPECT_EQ(before.cache_hits, after.cache_hits);
	EXPECT_EQ(before.cache_misses, after.cache_misses);
}

TEST(lagi_audio_display, spectrum_analysis_cache_stays_finite_with_prefetch_interleaving) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	for (size_t i = 0; i < 32; ++i) {
		cache.Prefetch(i + 1, i + 4);
		auto block = cache.Get(i);
		for (size_t b = 0; b < 32; ++b) {
			EXPECT_TRUE(std::isfinite(block[b]));
		}
	}
}

TEST(lagi_audio_display, spectrum_analysis_cache_handle_survives_lru_eviction) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(5, 5);
	cache.Age(sizeof(float) * (size_t(1) << 5));

	auto first = cache.Get(0);
	ASSERT_NE(nullptr, first);
	auto const first_value = first[0];
	ASSERT_NE(nullptr, cache.Get(1));
	EXPECT_EQ(nullptr, cache.GetIfReady(0));
	EXPECT_TRUE(std::isfinite(first[0]));
	EXPECT_EQ(first_value, first[0]);
}

TEST(lagi_audio_display, spectrum_analysis_cache_prefetch_caps_sparse_range_at_both_edges) {
	CountingStereoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);
	cache.SetPrefetchBuildMaxBlocks(4);

	cache.Prefetch(10, 300);
	ASSERT_TRUE(WaitForSpectrumPrefetchBuilds(cache, 4));
	EXPECT_NE(nullptr, cache.GetIfReady(10));
	EXPECT_NE(nullptr, cache.GetIfReady(11));
	EXPECT_EQ(nullptr, cache.GetIfReady(12));
	EXPECT_EQ(nullptr, cache.GetIfReady(298));
	EXPECT_NE(nullptr, cache.GetIfReady(299));
	EXPECT_NE(nullptr, cache.GetIfReady(300));
	EXPECT_EQ(4u, cache.GetMetricsSnapshot().prefetch_builds);
}

TEST(lagi_audio_display, spectrum_analysis_cache_recovers_failed_read_before_returning_power) {
	RecoverableReadProvider provider(1);
	RecoverableReadProvider reference;
	auto source = CreateAudioDisplaySource(&provider);
	auto reference_source = CreateAudioDisplaySource(&reference);
	AudioSpectrumAnalysisCache cache;
	AudioSpectrumAnalysisCache reference_cache;
	cache.SetSource(source.get());
	reference_cache.SetSource(reference_source.get());
	cache.SetResolution(9, 7);
	reference_cache.SetResolution(9, 7);

	auto recovered = cache.Get(5);
	auto expected = reference_cache.Get(5);
	ASSERT_NE(nullptr, recovered);
	ASSERT_NE(nullptr, expected);
	EXPECT_EQ(2, provider.fill_calls.load());
	for (size_t bin = 0; bin < (size_t{1} << 9); ++bin)
		EXPECT_EQ(expected[bin], recovered[bin]) << "bin " << bin;
	EXPECT_EQ(recovered, cache.GetIfReady(5));
	EXPECT_EQ(recovered, cache.Get(5));
	EXPECT_EQ(2, provider.fill_calls.load());
}

TEST(lagi_audio_display, spectrum_analysis_cache_failed_prefetch_does_not_publish_partial_audio) {
	RecoverableReadProvider provider(2);
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetResolution(9, 7);

	cache.Prefetch(5, 5);
	auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (cache.GetMetricsSnapshot().prefetch_busy_skips == 0 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	ASSERT_EQ(1u, cache.GetMetricsSnapshot().prefetch_busy_skips);
	EXPECT_EQ(2, provider.fill_calls.load());
	EXPECT_EQ(nullptr, cache.GetIfReady(5));
	EXPECT_EQ(0u, cache.GetMetricsSnapshot().prefetch_builds);

	RecoverableReadProvider reference;
	auto reference_source = CreateAudioDisplaySource(&reference);
	AudioSpectrumAnalysisCache reference_cache;
	reference_cache.SetSource(reference_source.get());
	reference_cache.SetResolution(9, 7);
	auto recovered = cache.Get(5);
	auto expected = reference_cache.Get(5);
	ASSERT_NE(nullptr, recovered);
	ASSERT_NE(nullptr, expected);
	EXPECT_EQ(3, provider.fill_calls.load());
	for (size_t bin = 0; bin < (size_t{1} << 9); ++bin)
		EXPECT_EQ(expected[bin], recovered[bin]) << "bin " << bin;
}

TEST(lagi_audio_display, spectrum_analysis_cache_caches_genuine_silence_on_first_read) {
	RecoverableReadProvider provider(0, true);
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetResolution(9, 7);

	auto first = cache.Get(5);
	ASSERT_NE(nullptr, first);
	EXPECT_EQ(1, provider.fill_calls.load());
	for (size_t bin = 0; bin < (size_t{1} << 9); ++bin)
		EXPECT_EQ(0.f, first[bin]);
	EXPECT_EQ(first, cache.GetIfReady(5));
	EXPECT_EQ(first, cache.Get(5));
	EXPECT_EQ(1, provider.fill_calls.load());
}

TEST(lagi_audio_display, spectrum_analysis_cache_failed_rolling_tail_reloads_entire_window) {
	RecoverableReadProvider provider;
	RecoverableReadProvider reference;
	auto source = CreateAudioDisplaySource(&provider);
	auto reference_source = CreateAudioDisplaySource(&reference);
	AudioSpectrumAnalysisCache cache;
	AudioSpectrumAnalysisCache reference_cache;
	cache.SetSource(source.get());
	reference_cache.SetSource(reference_source.get());
	cache.SetResolution(9, 7);
	reference_cache.SetResolution(9, 7);
	auto previous = cache.Get(5);
	ASSERT_NE(nullptr, previous);

	provider.failures_remaining = 2;
	EXPECT_THROW(cache.Get(6), agi::AudioDecodeError);
	EXPECT_EQ(128, provider.last_read_count.load());
	EXPECT_EQ(nullptr, cache.GetIfReady(6));
	EXPECT_EQ(previous, cache.GetIfReady(5));

	auto recovered = cache.Get(6);
	auto expected = reference_cache.Get(6);
	ASSERT_NE(nullptr, recovered);
	ASSERT_NE(nullptr, expected);
	EXPECT_EQ(1024, provider.last_read_count.load());
	EXPECT_EQ(4, provider.fill_calls.load());
	for (size_t bin = 0; bin < (size_t{1} << 9); ++bin)
		EXPECT_EQ(expected[bin], recovered[bin]) << "bin " << bin;
}
TEST(lagi_audio_display, spectrum_analysis_cache_visible_get_shares_inflight_prefetch_build) {
	ScopedTestDeadline deadline("spectrum visible/prefetch shared build");
	BlockingSpectrumProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	cache.Prefetch(0, 0);
	ASSERT_TRUE(WaitForBlockingProvider(provider));
	auto visible = std::async(std::launch::async, [&] { return cache.Get(0); });
	if (!WaitForVisibleLockContention(cache)) {
		provider.Release();
		EXPECT_EQ(std::future_status::ready, visible.wait_for(std::chrono::seconds(2)));
		FAIL() << "visible spectrum miss did not register build-lock contention";
	}
	EXPECT_EQ(std::future_status::timeout, visible.wait_for(std::chrono::milliseconds(0)));
	provider.Release();
	ASSERT_EQ(std::future_status::ready, visible.wait_for(std::chrono::seconds(2)));
	EXPECT_NE(nullptr, visible.get());
	EXPECT_EQ(1, provider.fill_calls.load());
	auto const metrics = cache.GetMetricsSnapshot();
	EXPECT_EQ(1u, metrics.prefetch_builds);
	EXPECT_GE(metrics.visible_lock_contention, 1u);
	EXPECT_GE(metrics.cache_hits, 1u);
}

TEST(lagi_audio_display, spectrum_analysis_cache_visible_miss_preempts_remaining_prefetch_batch) {
	ScopedTestDeadline deadline("spectrum visible cross-block preemption");
	SequencedBlockingMonoProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	std::promise<void> prefetch_finished;
	auto finished = prefetch_finished.get_future();
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);
	cache.SetReadyCallback([&] { prefetch_finished.set_value(); });
	std::future<AudioSpectrumAnalysisCache::BlockHandle> visible;
	auto release = agi::make_scope_exit([&] { provider.ReleaseAll(); });

	cache.Prefetch(10, 13);
	ASSERT_TRUE(provider.WaitForCalls(1));
	visible = std::async(std::launch::async, [&] { return cache.Get(13); });
	ASSERT_TRUE(WaitForVisibleLockContention(cache));
	EXPECT_EQ(std::future_status::timeout, visible.wait_for(std::chrono::milliseconds(0)));
	provider.ReleaseThrough(1);
	ASSERT_TRUE(provider.WaitForCalls(2));
	EXPECT_EQ((std::vector<int64_t>{768, 1152}), provider.ReadStarts());
	ASSERT_EQ(std::future_status::ready, finished.wait_for(std::chrono::seconds(2)));
	EXPECT_EQ(std::future_status::timeout, visible.wait_for(std::chrono::milliseconds(0)));
	EXPECT_NE(nullptr, cache.GetIfReady(10));
	EXPECT_EQ(nullptr, cache.GetIfReady(11));
	EXPECT_EQ(nullptr, cache.GetIfReady(12));
	EXPECT_EQ(nullptr, cache.GetIfReady(13));
	auto const pending = cache.GetMetricsSnapshot();
	EXPECT_EQ(1u, pending.prefetch_builds);
	EXPECT_EQ(0u, pending.visible_builds);
	EXPECT_EQ(1u, pending.prefetch_busy_skips);

	provider.ReleaseAll();
	ASSERT_EQ(std::future_status::ready, visible.wait_for(std::chrono::seconds(2)));
	EXPECT_NE(nullptr, visible.get());
	EXPECT_EQ((std::vector<int64_t>{768, 1152}), provider.ReadStarts());
	EXPECT_NE(nullptr, cache.GetIfReady(10));
	EXPECT_NE(nullptr, cache.GetIfReady(13));
	EXPECT_EQ(nullptr, cache.GetIfReady(11));
	EXPECT_EQ(nullptr, cache.GetIfReady(12));
	auto const metrics = cache.GetMetricsSnapshot();
	EXPECT_EQ(1u, metrics.prefetch_builds);
	EXPECT_EQ(1u, metrics.visible_builds);
	EXPECT_EQ(1u, metrics.prefetch_busy_skips);
}
TEST(lagi_audio_display, spectrum_analysis_cache_latest_range_drops_obsolete_result) {
	ScopedTestDeadline deadline("spectrum latest-range cancellation");
	BlockingSpectrumProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	cache.Prefetch(0, 0);
	ASSERT_TRUE(WaitForBlockingProvider(provider));
	cache.Prefetch(100, 100);
	provider.Release();
	ASSERT_TRUE(WaitForSpectrumPrefetchBuilds(cache, 1));
	EXPECT_EQ(nullptr, cache.GetIfReady(0));
	EXPECT_NE(nullptr, cache.GetIfReady(100));
	EXPECT_EQ(2, provider.fill_calls.load());
	auto const metrics = cache.GetMetricsSnapshot();
	EXPECT_EQ(1u, metrics.prefetch_builds);
	EXPECT_GE(metrics.stale_drops, 1u);
}

TEST(lagi_audio_display, spectrum_analysis_cache_nonzero_age_does_not_wait_for_inflight_prefetch) {
	ScopedTestDeadline deadline("spectrum nonzero cache aging");
	BlockingSpectrumProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetResolution(9, 7);

	cache.Prefetch(0, 0);
	ASSERT_TRUE(WaitForBlockingProvider(provider));
	auto age = std::async(std::launch::async, [&] { cache.Age(1); });
	if (age.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
		provider.Release();
		FAIL() << "nonzero spectrum cache aging waited for the in-flight provider read";
	}
	age.get();
	auto const block_bytes = sizeof(float) * (size_t{1} << 9);
	EXPECT_EQ(block_bytes, cache.GetMetricsSnapshot().cache_budget_bytes);
	EXPECT_EQ(0u, cache.GetMetricsSnapshot().prefetch_builds);
	provider.Release();
	ASSERT_TRUE(WaitForSpectrumPrefetchBuilds(cache, 1));
	EXPECT_NE(nullptr, cache.GetIfReady(0));
	EXPECT_EQ(block_bytes, cache.GetMetricsSnapshot().cache_bytes);
}

TEST(lagi_audio_display, spectrum_analysis_cache_provider_detach_drops_inflight_prefetch) {
	ScopedTestDeadline deadline("spectrum provider detach");
	BlockingSpectrumProvider provider;
	auto source = CreateAudioDisplaySource(&provider);
	AudioSpectrumAnalysisCache cache;
	cache.SetSource(source.get());
	cache.SetMixPolicy(AudioMixPolicy::MonoAverage);
	cache.SetResolution(9, 7);

	cache.Prefetch(0, 0);
	ASSERT_TRUE(WaitForBlockingProvider(provider));
	auto detach = std::async(std::launch::async, [&] {
		cache.SetSource(nullptr);
	});
	EXPECT_EQ(std::future_status::timeout, detach.wait_for(std::chrono::milliseconds(50)));
	provider.Release();
	ASSERT_EQ(std::future_status::ready, detach.wait_for(std::chrono::seconds(2)));
	detach.get();
	source.reset();
	EXPECT_EQ(nullptr, cache.GetIfReady(0));
	EXPECT_EQ(0u, cache.GetMetricsSnapshot().prefetch_builds);
	EXPECT_GE(cache.GetMetricsSnapshot().stale_drops, 1u);
}

TEST(lagi_audio_display, track_cursor_overlay_refresh_policy_handles_same_pixel_updates) {
	EXPECT_FALSE(AudioDisplayInvalidationPlanner::ShouldRefreshTrackCursor(-1, -1));
	EXPECT_TRUE(AudioDisplayInvalidationPlanner::ShouldRefreshTrackCursor(-1, 120));
	EXPECT_TRUE(AudioDisplayInvalidationPlanner::ShouldRefreshTrackCursor(120, -1));
	EXPECT_FALSE(AudioDisplayInvalidationPlanner::ShouldRefreshTrackCursor(120, 120));
	EXPECT_TRUE(AudioDisplayInvalidationPlanner::ShouldRefreshTrackCursor(120, 121));
}

TEST(lagi_audio_display, invalidation_planner_unions_track_cursor_and_label_rects) {
	using Planner = AudioDisplayInvalidationPlanner;

	Planner::Rect const old_label = { 0, 5, 10, 5 };
	Planner::Rect const new_label = { 30, 5, 10, 5 };

	auto const dirty = Planner::PlanTrackCursorDirtyRect(
		120,
		121,
		old_label,
		new_label,
		100,
		10,
		50);

	EXPECT_EQ((Planner::Rect{ 0, 5, 40, 55 }), dirty);
}

TEST(lagi_audio_display, invalidation_planner_unions_marker_move_rects) {
	using Planner = AudioDisplayInvalidationPlanner;

	Planner::Rect const old_marker = { 10, 10, 5, 50 };
	Planner::Rect const new_marker = { 20, 10, 5, 50 };

	EXPECT_EQ((Planner::Rect{ 10, 10, 15, 50 }), Planner::PlanMarkerMoveDirtyRect(old_marker, new_marker));
}

TEST(lagi_audio_display, invalidation_planner_unions_selection_edge_rects) {
	using Planner = AudioDisplayInvalidationPlanner;

	auto const dirty = Planner::PlanSelectionEdgeDirtyRect(1000, 995, 900, 10, 50);
	EXPECT_EQ((Planner::Rect{ 94, 10, 8, 50 }), dirty);
}

TEST(lagi_audio_display, marker_drag_dead_zone_ignores_horizontal_jitter) {
	AudioMarkerDragDeadZone dead_zone(100, 5);

	EXPECT_FALSE(dead_zone.ShouldDrag(95));
	EXPECT_FALSE(dead_zone.ShouldDrag(105));
	EXPECT_TRUE(dead_zone.ShouldDrag(106));
}

TEST(lagi_audio_display, marker_drag_dead_zone_stays_active_after_threshold) {
	AudioMarkerDragDeadZone dead_zone(100, 5);

	EXPECT_TRUE(dead_zone.ShouldDrag(94));
	EXPECT_TRUE(dead_zone.ShouldDrag(100));
}

TEST(lagi_audio_display, marker_drag_dead_zone_can_be_disabled) {
	AudioMarkerDragDeadZone dead_zone(100, 0);

	EXPECT_FALSE(dead_zone.ShouldDrag(100));
	EXPECT_TRUE(dead_zone.ShouldDrag(101));
}
