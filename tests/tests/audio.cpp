// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
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
//
// Aegisub Project http://www.aegisub.org/

#include <main.h>

#include "../../src/audio_spectrum_analysis_cache.h"
#include "../../src/audio_waveform_summary_cache.h"
#include "../../src/provider_catalog.h"
#include "../../src/provider_catalog_builder.h"
#include "../../src/provider_factory_entry.h"
#include "../../src/provider_open_policy.h"
#include "../../src/provider_selection_diagnostics.h"
#include <libaegisub/audio/provider.h>
#include <libaegisub/fs.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/path.h>
#include <libaegisub/scope_exit.h>
#include <libaegisub/util.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

template<typename Predicate>
bool WaitUntil(Predicate&& predicate, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
	auto const deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline) {
		if (predicate())
			return true;
		agi::util::sleep_for(10);
	}
	return predicate();
}

TEST(lagi_audio, dummy_blank) {
	auto provider = agi::CreateDummyAudioProvider("dummy-audio:", nullptr);

	char buff[1024];
	memset(buff, 1, sizeof(buff));
	provider->GetAudio(buff, 12356, 512);
	for (size_t i = 0; i < sizeof(buff); ++i) ASSERT_EQ(0, buff[i]);
}

TEST(lagi_audio, dummy_noise) {
	auto provider = agi::CreateDummyAudioProvider("dummy-audio:noise?", nullptr);

	char buff[1024];
	memset(buff, 0, sizeof(buff));
	provider->GetAudio(buff, 12356, 512);
	for (size_t i = 0; i < sizeof(buff); ++i) {
		if (buff[i] != 0)
			return;
	}
	bool all_zero = true;
	ASSERT_FALSE(all_zero);
}

TEST(lagi_audio, dummy_rejects_non_dummy_url) {
	auto provider = agi::CreateDummyAudioProvider("/tmp", nullptr);
	ASSERT_EQ(nullptr, provider.get());
}

template<typename Sample=uint16_t>
struct TestAudioProvider : agi::AudioProvider {
	int bias = 0;

	TestAudioProvider(int64_t duration = 90, int rate=48000) {
		channels = 1;
		num_samples = duration * 48000;
		decoded_samples = num_samples;
		sample_rate = rate;
		bytes_per_sample = sizeof(Sample);
		float_samples = false;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		auto out = static_cast<Sample *>(buf);
		for (int64_t end = start + count; start < end; ++start)
			*out++ = (Sample)(start + bias);
	}
};

template<typename Sample=uint16_t>
struct NamedTestAudioProvider : TestAudioProvider<Sample> {
	using TestAudioProvider<Sample>::TestAudioProvider;

	agi::AudioProviderMemoryStats GetMemoryStats() const override {
		return this->BuildMemoryStats("TestSource");
	}
};

TEST(provider_selection_diagnostics, reports_fallback_reason_from_preferred_attempt) {
	aegisub::provider_selection_diagnostics::SelectionReport report;
	report.preferred_provider = "Avisynth";
	report.selected_provider = "FFmpegSource";
	report.attempts = {
		{"Avisynth", "error", "Avisynth error:\nmissing plugin"},
		{"FFmpegSource", "opened", ""}
	};

	EXPECT_TRUE(aegisub::provider_selection_diagnostics::UsedFallback(report));
	EXPECT_EQ("error: Avisynth error: missing plugin", aegisub::provider_selection_diagnostics::DescribeFallbackReason(report));
	EXPECT_EQ(
		"Avisynth:error (Avisynth error: missing plugin) | FFmpegSource:opened",
		aegisub::provider_selection_diagnostics::FormatAttempts(report));
}

TEST(provider_selection_diagnostics, does_not_report_fallback_when_preferred_opens) {
	aegisub::provider_selection_diagnostics::SelectionReport report;
	report.preferred_provider = "FFmpegSource";
	report.selected_provider = "FFmpegSource";
	report.attempts = {
		{"FFmpegSource", "opened", ""}
	};

	EXPECT_FALSE(aegisub::provider_selection_diagnostics::UsedFallback(report));
	EXPECT_TRUE(aegisub::provider_selection_diagnostics::DescribeFallbackReason(report).empty());
	EXPECT_EQ("FFmpegSource:opened", aegisub::provider_selection_diagnostics::FormatAttempts(report));
}

TEST(provider_selection_diagnostics, canonicalizes_common_aliases) {
	EXPECT_EQ("FFmpegSource", aegisub::provider_selection_diagnostics::CanonicalizeProviderName("ffms2"));
	EXPECT_EQ("FFmpegSource", aegisub::provider_selection_diagnostics::CanonicalizeProviderName("ffmpegsource"));
	EXPECT_EQ("Avisynth", aegisub::provider_selection_diagnostics::CanonicalizeProviderName("avs"));
	EXPECT_EQ("YUV4MPEG", aegisub::provider_selection_diagnostics::CanonicalizeProviderName("y4m"));
}

TEST(provider_selection_diagnostics, does_not_report_fallback_for_equivalent_aliases) {
	aegisub::provider_selection_diagnostics::SelectionReport report;
	report.preferred_provider = "ffms2";
	report.selected_provider = "FFmpegSource";
	report.attempts = {
		{"FFmpegSource", "opened", ""}
	};

	EXPECT_FALSE(aegisub::provider_selection_diagnostics::UsedFallback(report));
	EXPECT_TRUE(aegisub::provider_selection_diagnostics::DescribeFallbackReason(report).empty());
}

TEST(provider_catalog, exposes_visible_choices_and_availability_without_gui_types) {
	aegisub::provider_catalog::ProviderCatalog catalog;
	catalog.kind = aegisub::provider_catalog::ProviderKind::Audio;
	catalog.preferred_provider = "FFmpegSource";
	catalog.providers = {
		{catalog.kind, "Dummy", "Dummy", true, true, ""},
		{catalog.kind, "FFmpegSource", "FFmpegSource", false, false, "missing ffms2.dll"},
		{catalog.kind, "PCM", "PCM", false, true, ""}
	};

	auto visible = aegisub::provider_catalog::VisibleProviders(catalog);
	ASSERT_EQ(2u, visible.size());
	EXPECT_EQ("FFmpegSource", visible[0].name);
	EXPECT_FALSE(visible[0].available);
	EXPECT_EQ("missing ffms2.dll", visible[0].unavailable_reason);
	EXPECT_TRUE(aegisub::provider_catalog::IsPreferred(visible[0], "ffms2"));

	auto names = aegisub::provider_catalog::VisibleProviderNames(catalog);
	EXPECT_EQ((std::vector<std::string>{"FFmpegSource", "PCM"}), names);

	auto choices = aegisub::provider_catalog::VisibleProviderChoices(catalog);
	ASSERT_EQ(2u, choices.size());
	EXPECT_EQ("FFmpegSource", choices[0].first);
	EXPECT_EQ("FFmpegSource", choices[0].second);
}

TEST(provider_catalog_builder, reports_unavailable_runtime_without_gui_types) {
	auto unavailable = [] { return false; };
	auto load_error = []() -> std::string { return "missing runtime"; };
	aegisub::provider_catalog::ProviderFactoryDescriptor provider {
		"FFmpegSource",
		false,
		unavailable,
		load_error
	};

	std::string availability_error;
	EXPECT_FALSE(aegisub::provider_catalog::IsProviderAvailable(provider, availability_error));
	EXPECT_EQ("missing runtime", availability_error);

	auto descriptor = aegisub::provider_catalog::BuildProviderDescriptor(
		aegisub::provider_catalog::ProviderKind::Audio,
		provider,
		false,
		availability_error);
	EXPECT_EQ("FFmpegSource (Unavailable)", descriptor.display_name);
	EXPECT_EQ("missing runtime", descriptor.unavailable_reason);
}

TEST(provider_catalog_builder, captures_availability_exceptions_as_errors) {
	auto throws = []() -> bool { throw std::runtime_error("probe failed"); };
	aegisub::provider_catalog::ProviderFactoryDescriptor provider {
		"LsmasNative",
		false,
		throws,
		nullptr
	};

	std::string availability_error;
	EXPECT_FALSE(aegisub::provider_catalog::IsProviderAvailable(provider, availability_error));
	EXPECT_EQ("probe failed", availability_error);
}

TEST(provider_catalog_builder, returns_optional_unavailable_reason_with_default_fallback) {
	auto unavailable = [] { return false; };
	auto empty_load_error = []() -> std::string { return ""; };
	aegisub::provider_catalog::ProviderFactoryDescriptor provider {
		"FFmpegSource",
		false,
		unavailable,
		empty_load_error
	};

	auto reason = aegisub::provider_catalog::ProviderUnavailableReason(provider);

	ASSERT_TRUE(reason);
	EXPECT_EQ("runtime library is unavailable.", *reason);
}

TEST(provider_catalog_builder, returns_no_unavailable_reason_when_provider_is_available) {
	aegisub::provider_catalog::ProviderFactoryDescriptor provider {
		"PCM",
		false,
		nullptr,
		nullptr
	};

	EXPECT_FALSE(aegisub::provider_catalog::ProviderUnavailableReason(provider));
}

TEST(provider_catalog_builder, describes_typed_factory_entries_without_host_types) {
	using CreateFn = void (*)();
	aegisub::provider_catalog::ProviderFactoryEntry<CreateFn> provider {
		"FFmpegSource",
		nullptr,
		nullptr,
		nullptr,
		false
	};

	auto descriptor = aegisub::provider_catalog::DescribeProviderFactoryEntry(provider);

	EXPECT_EQ("FFmpegSource", aegisub::provider_catalog::ProviderName(descriptor));
	EXPECT_FALSE(descriptor.hidden);
	EXPECT_EQ(nullptr, descriptor.is_available);
	EXPECT_EQ(nullptr, descriptor.availability_error);
}

TEST(provider_open_policy, records_attempts_with_empty_safe_defaults) {
	aegisub::provider_selection_diagnostics::SelectionReport report;

	aegisub::provider_catalog::RecordAttempt(report, nullptr, nullptr);

	ASSERT_EQ(1u, report.attempts.size());
	EXPECT_TRUE(report.attempts[0].provider_name.empty());
	EXPECT_TRUE(report.attempts[0].outcome.empty());
	EXPECT_TRUE(report.attempts[0].detail.empty());
}

TEST(provider_open_policy, records_returned_null_and_open_success_with_shared_helpers) {
	aegisub::provider_selection_diagnostics::SelectionReport report;

	aegisub::provider_catalog::RecordProviderReturnedNull(report, "FFmpegSource");
	aegisub::provider_catalog::RecordProviderOpenSuccess(report, "LsmasNative");

	EXPECT_EQ("LsmasNative", report.selected_provider);
	ASSERT_EQ(2u, report.attempts.size());
	EXPECT_EQ("FFmpegSource", report.attempts[0].provider_name);
	EXPECT_EQ("returned_null", report.attempts[0].outcome);
	EXPECT_EQ("provider factory returned null", report.attempts[0].detail);
	EXPECT_EQ("LsmasNative", report.attempts[1].provider_name);
	EXPECT_EQ("opened", report.attempts[1].outcome);
	EXPECT_TRUE(report.attempts[1].detail.empty());
}

TEST(provider_open_policy, try_open_provider_factory_handles_unavailable_null_and_success) {
	auto unavailable = [] { return false; };
	auto load_error = []() -> std::string { return "missing runtime"; };
	auto describe = [](aegisub::provider_catalog::ProviderFactoryDescriptor const& provider) {
		return provider;
	};

	aegisub::provider_selection_diagnostics::SelectionReport report;
	aegisub::provider_catalog::ProviderFactoryDescriptor unavailable_provider {
		"FFmpegSource",
		false,
		unavailable,
		load_error
	};
	auto unavailable_result = aegisub::provider_catalog::TryOpenProviderFactory(
		unavailable_provider,
		describe,
		report,
		[](auto const&) {
			return std::unique_ptr<int>(new int(1));
		});
	EXPECT_EQ(aegisub::provider_catalog::ProviderOpenAttemptState::Unavailable, unavailable_result.state);
	EXPECT_EQ("missing runtime", unavailable_result.unavailable_reason);
	EXPECT_TRUE(report.attempts.empty());

	aegisub::provider_catalog::ProviderFactoryDescriptor null_provider {
		"PCM",
		false,
		nullptr,
		nullptr
	};
	auto null_result = aegisub::provider_catalog::TryOpenProviderFactory(
		null_provider,
		describe,
		report,
		[](auto const&) {
			return std::unique_ptr<int>();
		});
	EXPECT_EQ(aegisub::provider_catalog::ProviderOpenAttemptState::ReturnedNull, null_result.state);
	ASSERT_EQ(1u, report.attempts.size());
	EXPECT_EQ("PCM", report.attempts[0].provider_name);
	EXPECT_EQ("returned_null", report.attempts[0].outcome);

	aegisub::provider_catalog::ProviderFactoryDescriptor opened_provider {
		"LsmasNative",
		false,
		nullptr,
		nullptr
	};
	auto opened_result = aegisub::provider_catalog::TryOpenProviderFactory(
		opened_provider,
		describe,
		report,
		[](auto const&) {
			return std::unique_ptr<int>(new int(42));
		});
	EXPECT_EQ(aegisub::provider_catalog::ProviderOpenAttemptState::Opened, opened_result.state);
	ASSERT_TRUE(opened_result.provider);
	EXPECT_EQ(42, *opened_result.provider);
	EXPECT_EQ("LsmasNative", report.selected_provider);
	ASSERT_EQ(2u, report.attempts.size());
	EXPECT_EQ("LsmasNative", report.attempts[1].provider_name);
	EXPECT_EQ("opened", report.attempts[1].outcome);
}

TEST(provider_open_policy, formats_attempt_error_lines_for_shared_open_reports) {
	EXPECT_EQ("FFmpegSource: missing runtime\n",
		aegisub::provider_catalog::FormatAttemptErrorLine("FFmpegSource", "missing runtime"));
	EXPECT_EQ(": missing provider name\n",
		aegisub::provider_catalog::FormatAttemptErrorLine(nullptr, "missing provider name"));

	std::string errors;
	aegisub::provider_catalog::AppendAttemptErrorLine(errors, "LsmasNative", "not supported");
	aegisub::provider_catalog::AppendAttemptErrorLine(errors, "Avisynth", "plugin failed");
	EXPECT_EQ("LsmasNative: not supported\nAvisynth: plugin failed\n", errors);
}

TEST(provider_open_policy, selects_audio_open_failure_from_recorded_attempts) {
	aegisub::provider_catalog::AudioProviderOpenFailureReport report;

	aegisub::provider_catalog::RecordAudioProviderOpenFailure(
		report,
		"FFmpegSource",
		"missing runtime",
		aegisub::provider_catalog::AudioProviderOpenAttemptFailure::Unavailable);
	EXPECT_EQ(aegisub::provider_catalog::AudioProviderOpenFailure::FileNotFound,
		aegisub::provider_catalog::FinalAudioProviderOpenFailure(report));
	EXPECT_EQ("FFmpegSource: missing runtime\n", report.all_errors);

	aegisub::provider_catalog::RecordAudioProviderOpenFailure(
		report,
		"PCM",
		"no audio track",
		aegisub::provider_catalog::AudioProviderOpenAttemptFailure::AudioNotFound);
	EXPECT_EQ(aegisub::provider_catalog::AudioProviderOpenFailure::AudioNotFound,
		aegisub::provider_catalog::FinalAudioProviderOpenFailure(report));
	EXPECT_EQ("FFmpegSource: missing runtime\nPCM: no audio track\n",
		aegisub::provider_catalog::FinalAudioProviderOpenErrorDetail(report));

	aegisub::provider_catalog::RecordAudioProviderOpenFailure(
		report,
		"LsmasNative",
		"codec failed",
		aegisub::provider_catalog::AudioProviderOpenAttemptFailure::ProviderError);
	EXPECT_EQ(aegisub::provider_catalog::AudioProviderOpenFailure::ProviderError,
		aegisub::provider_catalog::FinalAudioProviderOpenFailure(report));
	EXPECT_EQ("LsmasNative: codec failed\n",
		aegisub::provider_catalog::FinalAudioProviderOpenErrorDetail(report));
	EXPECT_EQ("FFmpegSource: missing runtime\nPCM: no audio track\nLsmasNative: codec failed\n",
		report.all_errors);
}

TEST(provider_open_policy, records_audio_open_failure_attempts_with_diagnostics) {
	aegisub::provider_catalog::AudioProviderOpenFailureReport report;
	aegisub::provider_selection_diagnostics::SelectionReport diagnostics;

	aegisub::provider_catalog::RecordAudioProviderOpenFailureAttempt(
		report,
		diagnostics,
		"PCM",
		"missing.wav not found.",
		aegisub::provider_catalog::AudioProviderOpenAttemptFailure::FileNotFound,
		nullptr,
		"missing.wav");
	aegisub::provider_catalog::RecordAudioProviderOpenFailureAttempt(
		report,
		diagnostics,
		"FFmpegSource",
		"decoder crashed",
		aegisub::provider_catalog::AudioProviderOpenAttemptFailure::ProviderError,
		"std_exception");

	EXPECT_EQ("PCM: missing.wav not found.\nFFmpegSource: decoder crashed\n", report.all_errors);
	EXPECT_EQ("FFmpegSource: decoder crashed\n", report.partial_errors);
	ASSERT_EQ(2u, diagnostics.attempts.size());
	EXPECT_EQ("PCM", diagnostics.attempts[0].provider_name);
	EXPECT_EQ("file_not_found", diagnostics.attempts[0].outcome);
	EXPECT_EQ("missing.wav", diagnostics.attempts[0].detail);
	EXPECT_EQ("FFmpegSource", diagnostics.attempts[1].provider_name);
	EXPECT_EQ("std_exception", diagnostics.attempts[1].outcome);
	EXPECT_EQ("decoder crashed", diagnostics.attempts[1].detail);
}

TEST(provider_open_policy, selects_video_open_failure_from_recorded_attempts) {
	aegisub::provider_catalog::VideoProviderOpenFailureReport report;

	aegisub::provider_catalog::RecordVideoProviderOpenFailure(
		report,
		"FFmpegSource",
		"missing runtime",
		aegisub::provider_catalog::VideoProviderOpenAttemptFailure::Unavailable);
	EXPECT_EQ(aegisub::provider_catalog::VideoProviderOpenFailure::FileNotFound,
		aegisub::provider_catalog::FinalVideoProviderOpenFailure(report));

	aegisub::provider_catalog::RecordVideoProviderOpenFailure(
		report,
		"YUV4MPEG",
		"video is not in a supported format.",
		aegisub::provider_catalog::VideoProviderOpenAttemptFailure::NotSupported);
	EXPECT_EQ(aegisub::provider_catalog::VideoProviderOpenFailure::NotSupported,
		aegisub::provider_catalog::FinalVideoProviderOpenFailure(report));

	aegisub::provider_catalog::RecordVideoProviderOpenFailure(
		report,
		"LsmasNative",
		"index failed",
		aegisub::provider_catalog::VideoProviderOpenAttemptFailure::OpenError);
	EXPECT_EQ(aegisub::provider_catalog::VideoProviderOpenFailure::OpenError,
		aegisub::provider_catalog::FinalVideoProviderOpenFailure(report));
	EXPECT_EQ("FFmpegSource: missing runtime\nYUV4MPEG: video is not in a supported format.\nLsmasNative: index failed\n",
		report.errors);
}

TEST(provider_open_policy, records_video_open_failure_attempts_with_diagnostics) {
	aegisub::provider_catalog::VideoProviderOpenFailureReport report;
	aegisub::provider_selection_diagnostics::SelectionReport diagnostics;

	aegisub::provider_catalog::RecordVideoProviderOpenFailureAttempt(
		report,
		diagnostics,
		"YUV4MPEG",
		"video is not in a supported format.",
		aegisub::provider_catalog::VideoProviderOpenAttemptFailure::NotSupported);
	aegisub::provider_catalog::RecordVideoProviderOpenFailureAttempt(
		report,
		diagnostics,
		"LsmasNative",
		"index failed",
		aegisub::provider_catalog::VideoProviderOpenAttemptFailure::OpenError);

	EXPECT_EQ(aegisub::provider_catalog::VideoProviderOpenFailure::OpenError,
		aegisub::provider_catalog::FinalVideoProviderOpenFailure(report));
	EXPECT_EQ("YUV4MPEG: video is not in a supported format.\nLsmasNative: index failed\n", report.errors);
	ASSERT_EQ(2u, diagnostics.attempts.size());
	EXPECT_EQ("YUV4MPEG", diagnostics.attempts[0].provider_name);
	EXPECT_EQ("not_supported", diagnostics.attempts[0].outcome);
	EXPECT_EQ("video is not in a supported format.", diagnostics.attempts[0].detail);
	EXPECT_EQ("LsmasNative", diagnostics.attempts[1].provider_name);
	EXPECT_EQ("error", diagnostics.attempts[1].outcome);
	EXPECT_EQ("index failed", diagnostics.attempts[1].detail);
}

TEST(provider_catalog_builder, sorts_factories_with_hidden_first_then_preferred) {
	std::vector<aegisub::provider_catalog::ProviderFactoryDescriptor> providers {
		{"Dummy", true, nullptr, nullptr},
		{"FFmpegSource", false, nullptr, nullptr},
		{"LsmasNative", false, nullptr, nullptr}
	};
	auto describe = [](auto const& provider) { return provider; };

	auto names = aegisub::provider_catalog::VisibleFactoryNames(providers, describe);
	EXPECT_EQ((std::vector<std::string>{"FFmpegSource", "LsmasNative"}), names);

	auto sorted = aegisub::provider_catalog::SortFactories(providers, "LsmasNative", describe);
	ASSERT_EQ(3u, sorted.size());
	EXPECT_EQ("Dummy", sorted[0]->name);
	EXPECT_EQ("LsmasNative", sorted[1]->name);
	EXPECT_EQ("FFmpegSource", sorted[2]->name);
}

TEST(provider_catalog_builder, builds_catalog_with_canonical_preferred_and_availability) {
	auto unavailable = [] { return false; };
	auto load_error = []() -> std::string { return "missing ffms2.dll"; };
	std::vector<aegisub::provider_catalog::ProviderFactoryDescriptor> providers {
		{"Dummy", true, nullptr, nullptr},
		{"FFmpegSource", false, unavailable, load_error},
		{"LsmasNative", false, nullptr, nullptr}
	};
	auto describe = [](auto const& provider) { return provider; };

	auto catalog = aegisub::provider_catalog::BuildCatalog(
		aegisub::provider_catalog::ProviderKind::Video,
		providers,
		"lsmas",
		describe);

	EXPECT_EQ(aegisub::provider_catalog::ProviderKind::Video, catalog.kind);
	EXPECT_EQ("LsmasNative", catalog.preferred_provider);
	ASSERT_EQ(3u, catalog.providers.size());
	EXPECT_EQ("Dummy", catalog.providers[0].name);
	EXPECT_TRUE(catalog.providers[0].hidden);
	EXPECT_EQ("LsmasNative", catalog.providers[1].name);
	EXPECT_TRUE(catalog.providers[1].available);
	EXPECT_EQ("FFmpegSource", catalog.providers[2].name);
	EXPECT_FALSE(catalog.providers[2].available);
	EXPECT_EQ("FFmpegSource (Unavailable)", catalog.providers[2].display_name);
	EXPECT_EQ("missing ffms2.dll", catalog.providers[2].unavailable_reason);
}

struct BlockingSequenceAudioProvider : agi::AudioProvider {
	mutable std::mutex mutex;
	mutable std::condition_variable cv;
	mutable bool block_reads = true;
	mutable bool entered = false;

	BlockingSequenceAudioProvider(int64_t duration = 90) {
		channels = 1;
		num_samples = duration * 48000;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(uint16_t);
		float_samples = false;
	}

	void Release() const {
		{
			std::lock_guard<std::mutex> lock(mutex);
			block_reads = false;
		}
		cv.notify_all();
	}

	bool WaitUntilEntered() const {
		std::unique_lock<std::mutex> lock(mutex);
		return cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		std::unique_lock<std::mutex> lock(mutex);
		entered = true;
		cv.notify_all();
		cv.wait(lock, [&] { return !block_reads; });
		lock.unlock();

		auto out = static_cast<uint16_t *>(buf);
		for (int64_t end = start + count; start < end; ++start)
			*out++ = static_cast<uint16_t>(start);
	}
};

struct SlowStopAudioProvider : agi::AudioProvider {
	int sleep_ms;

	SlowStopAudioProvider(int sleep_ms, int64_t duration = 5000)
	: sleep_ms(sleep_ms) {
		channels = 1;
		num_samples = duration * 48000;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(uint16_t);
		float_samples = false;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		agi::util::sleep_for(sleep_ms);
		auto out = static_cast<uint16_t *>(buf);
		for (int64_t end = start + count; start < end; ++start)
			*out++ = static_cast<uint16_t>(start);
	}
};

struct CountingSequenceAudioProvider : agi::AudioProvider {
	mutable std::atomic<int> fill_calls{0};

	CountingSequenceAudioProvider(int64_t duration = 90) {
		channels = 1;
		num_samples = duration * 48000;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(uint16_t);
		float_samples = false;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		fill_calls.fetch_add(1, std::memory_order_relaxed);
		auto out = static_cast<uint16_t *>(buf);
		for (int64_t end = start + count; start < end; ++start)
			*out++ = static_cast<uint16_t>(start);
	}
};

struct FailingRamCacheAudioProvider : agi::AudioProvider {
	static constexpr int64_t BlockSamples = (1 << 22) / sizeof(int16_t);
	int failed_reads;
	bool silence;
	mutable std::atomic<int> first_block_reads{0};
	mutable std::atomic<int> second_block_reads{0};
	mutable std::atomic<int64_t> last_first_block_start{0};
	mutable std::atomic<int64_t> last_first_block_count{0};

	FailingRamCacheAudioProvider(int failed_reads, bool silence = false)
		: failed_reads(failed_reads), silence(silence) {
		channels = 1;
		num_samples = BlockSamples + 64;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(int16_t);
		float_samples = false;
	}

	static int16_t SampleAt(int64_t position) {
		return static_cast<int16_t>(1 + position % 30000);
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		auto *out = static_cast<int16_t *>(buf);
		if (start < BlockSamples) {
			last_first_block_start = start;
			last_first_block_count = count;
			if (++first_block_reads <= failed_reads) {
				std::fill_n(out, std::min<int64_t>(count, 32), int16_t{-23456});
				throw agi::AudioDecodeError("injected partial RAM cache read");
			}
		}
		else {
			++second_block_reads;
		}
		for (int64_t i = 0; i < count; ++i)
			out[i] = silence ? int16_t{0} : SampleAt(start + i);
	}
};

struct FrontierRamCacheAudioProvider : FailingRamCacheAudioProvider {
	mutable std::mutex mutex;
	mutable std::condition_variable cv;
	mutable bool entered = false;
	mutable bool released = false;
	mutable std::atomic<bool> timed_out{false};

	FrontierRamCacheAudioProvider() : FailingRamCacheAudioProvider(0) {}

	bool WaitUntilEntered() const {
		std::unique_lock<std::mutex> lock(mutex);
		return cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
	}

	void Release() const {
		{
			std::scoped_lock lock(mutex);
			released = true;
		}
		cv.notify_all();
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		if (start >= BlockSamples) {
			std::unique_lock<std::mutex> lock(mutex);
			entered = true;
			cv.notify_all();
			if (!cv.wait_for(lock, std::chrono::seconds(2), [&] { return released; })) {
				timed_out = true;
				throw agi::AudioDecodeError("RAM cache frontier test timed out");
			}
		}
		FailingRamCacheAudioProvider::FillBuffer(buf, start, count);
	}
};

struct FailingStereoFloatAudioProvider : agi::AudioProvider {
	bool fail_reads = true;
	mutable int fill_calls = 0;

	explicit FailingStereoFloatAudioProvider(int rate = 48000) {
		channels = 2;
		num_samples = 16;
		decoded_samples = num_samples;
		sample_rate = rate;
		bytes_per_sample = sizeof(float);
		float_samples = true;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		++fill_calls;
		auto *out = static_cast<float *>(buf);
		out[0] = 0.25F;
		out[1] = 0.5F;
		if (fail_reads)
			throw agi::AudioDecodeError("injected partial stereo read");
		for (int64_t i = 1; i < count; ++i) {
			out[i * 2] = 0.25F;
			out[i * 2 + 1] = 0.5F;
		}
	}
};

TEST(lagi_audio, checked_reads_propagate_partial_decode_failures_through_lock) {
	auto source = agi::make_unique<FailingStereoFloatAudioProvider>();
	auto *raw = source.get();
	auto provider = agi::CreateLockAudioProvider(std::move(source));
	std::array<float, 8> stereo;
	stereo.fill(-1.0F);

	EXPECT_THROW(provider->GetAudioChecked(stereo.data(), 0, 4), agi::AudioDecodeError);
	EXPECT_EQ(1, raw->fill_calls);
	EXPECT_EQ((std::array<float, 8>{0.25F, 0.5F, -1.0F, -1.0F, -1.0F, -1.0F, -1.0F, -1.0F}), stereo);

	std::array<int16_t, 4> mono;
	mono.fill(-1234);
	EXPECT_THROW(provider->GetInt16MonoAudioChecked(mono.data(), 0, mono.size()), agi::AudioDecodeError);
	EXPECT_EQ(2, raw->fill_calls);

	stereo.fill(-1.0F);
	provider->GetAudio(stereo.data(), 0, 4);
	EXPECT_EQ((std::array<float, 8>{}), stereo);
	provider->GetInt16MonoAudio(mono.data(), 0, mono.size());
	EXPECT_EQ((std::array<int16_t, 4>{}), mono);
	EXPECT_EQ(4, raw->fill_calls);
}

TEST(lagi_audio, checked_reads_propagate_failures_through_conversion_and_sample_doubling) {
	auto source = agi::make_unique<FailingStereoFloatAudioProvider>(16000);
	auto *raw = source.get();
	auto provider = agi::CreateConvertAudioProvider(agi::CreateLockAudioProvider(std::move(source)));
	ASSERT_EQ(32000, provider->GetSampleRate());
	ASSERT_EQ(1, provider->GetChannels());
	ASSERT_EQ(sizeof(int16_t), provider->GetBytesPerSample());
	ASSERT_FALSE(provider->AreSamplesFloat());
	std::array<int16_t, 4> samples;
	samples.fill(-1234);

	EXPECT_THROW(provider->GetAudioChecked(samples.data(), 3, samples.size()), agi::AudioDecodeError);
	EXPECT_EQ(1, raw->fill_calls);
	EXPECT_THROW(provider->GetInt16MonoAudioChecked(samples.data(), 3, 1), agi::AudioDecodeError);
	EXPECT_EQ(2, raw->fill_calls);
	provider->GetAudio(samples.data(), 3, samples.size());
	EXPECT_EQ((std::array<int16_t, 4>{}), samples);
	samples.fill(-1234);
	provider->GetInt16MonoAudio(samples.data(), 3, samples.size());
	EXPECT_EQ((std::array<int16_t, 4>{}), samples);
	EXPECT_EQ(4, raw->fill_calls);

	raw->fail_reads = false;
	provider->GetAudioChecked(samples.data(), 3, samples.size());
	EXPECT_EQ((std::array<int16_t, 4>{12288, 12288, 12288, 12288}), samples);
	samples.fill(-1234);
	provider->GetInt16MonoAudioChecked(samples.data(), 3, 1);
	EXPECT_EQ((std::array<int16_t, 4>{12288, -1234, -1234, -1234}), samples);
	EXPECT_EQ(6, raw->fill_calls);
}

TEST(lagi_audio, checked_reads_pad_only_samples_outside_audio) {
	for (bool mono : {false, true}) {
		SCOPED_TRACE(mono);
		CountingSequenceAudioProvider provider(1);
		std::array<int16_t, 8> samples;
		auto read = [&](int64_t start) {
			samples.fill(-1);
			if (mono)
				provider.GetInt16MonoAudioChecked(samples.data(), start, samples.size());
			else
				provider.GetAudioChecked(samples.data(), start, samples.size());
		};

		read(-2);
		EXPECT_EQ((std::array<int16_t, 8>{0, 0, 0, 1, 2, 3, 4, 5}), samples);
		EXPECT_EQ(1, provider.fill_calls);
		read(provider.GetNumSamples() - 3);
		for (size_t i = 0; i < 3; ++i)
			EXPECT_EQ(static_cast<int16_t>(provider.GetNumSamples() - 3 + i), samples[i]);
		for (size_t i = 3; i < samples.size(); ++i)
			EXPECT_EQ(0, samples[i]);
		EXPECT_EQ(2, provider.fill_calls);
	}
}

TEST(lagi_audio, checked_reads_pad_fully_outside_audio_without_decoding) {
	CountingSequenceAudioProvider provider(1);
	for (auto const start : {int64_t{-8}, int64_t{-9}, std::numeric_limits<int64_t>::min(),
							 provider.GetNumSamples(), provider.GetNumSamples() + 1, std::numeric_limits<int64_t>::max()}) {
		SCOPED_TRACE(start);
		std::array<int16_t, 8> samples;
		samples.fill(-1);
		provider.GetAudioChecked(samples.data(), start, samples.size());
		EXPECT_EQ((std::array<int16_t, 8>{}), samples);
		samples.fill(-1);
		provider.GetInt16MonoAudioChecked(samples.data(), start, samples.size());
		EXPECT_EQ((std::array<int16_t, 8>{}), samples);
	}
	EXPECT_EQ(0, provider.fill_calls);
}

TEST(lagi_audio, before_sample_zero) {
	TestAudioProvider<> provider;

	uint16_t buff[16];
	memset(buff, 1, sizeof(buff));
	provider.GetAudio(buff, -8, 16);

	for (int i = 0; i < 8; ++i)
		ASSERT_EQ(0, buff[i]);
	for (int i = 8; i < 16; ++i)
		ASSERT_EQ(i - 8, buff[i]);
}

TEST(lagi_audio, before_sample_zero_8bit) {
	TestAudioProvider<uint8_t> provider;
	provider.bias = 128;

	uint8_t buff[16];
	memset(buff, 1, sizeof(buff));
	provider.GetAudio(buff, -8, 16);

	for (int i = 0; i < 8; ++i)
		ASSERT_EQ(128, buff[i]);
	for (int i = 8; i < 16; ++i)
		ASSERT_EQ(128 + i - 8, buff[i]);
}

TEST(lagi_audio, after_end) {
	TestAudioProvider<> provider(1);

	uint16_t buff[16];
	memset(buff, 1, sizeof(buff));
	provider.GetAudio(buff, provider.GetNumSamples() - 8, 16);

	for (int i = 0; i < 8; ++i)
		ASSERT_NE(0, buff[i]);
	for (int i = 8; i < 16; ++i)
		ASSERT_EQ(0, buff[i]);
}

TEST(lagi_audio, save_audio_clip) {
	auto path = agi::Path().Decode("?temp/save_clip");
	agi::fs::Remove(path);

	auto provider = agi::CreateDummyAudioProvider("dummy-audio:noise?", nullptr);
	agi::SaveAudioClip(*provider, path, 60 * 60 * 1000, (60 * 60 + 10) * 1000);

	{
		std::ifstream s(path, std::ios_base::binary);
		ASSERT_TRUE(s.good());
		s.seekg(0, std::ios::end);
		// 10 seconds of 44.1 kHz samples per second of 16-bit mono, plus 44 bytes of header
		EXPECT_EQ(10 * 44100 * 2 + 44, s.tellg());
	}
	agi::fs::Remove(path);
}

TEST(lagi_audio, save_audio_clip_out_of_audio_range) {
	const auto path = agi::Path().Decode("?temp/save_clip");
	agi::fs::Remove(path);

	const auto provider = agi::CreateDummyAudioProvider("dummy-audio:noise?", nullptr);
	const auto end_time = 150 * 60 * 1000;

	// Start time after end of clip: empty file
	agi::SaveAudioClip(*provider, path, end_time, end_time + 1);
	{
		std::ifstream s(path, std::ios_base::binary);
		ASSERT_TRUE(s.good());
		s.seekg(0, std::ios::end);
		EXPECT_EQ(44, s.tellg());
	}
	agi::fs::Remove(path);

	// Start time >= end time: empty file
	agi::SaveAudioClip(*provider, path, end_time - 1, end_time - 1);
	{
		std::ifstream s(path, std::ios_base::binary);
		ASSERT_TRUE(s.good());
		s.seekg(0, std::ios::end);
		EXPECT_EQ(44, s.tellg());
	}
	agi::fs::Remove(path);

	// Start time during clip, end time after end of clip: save only the part that exists
	agi::SaveAudioClip(*provider, path, end_time - 1000, end_time + 1000);
	{
		std::ifstream s(path, std::ios_base::binary);
		ASSERT_TRUE(s.good());
		s.seekg(0, std::ios::end);
		// 1 second of 44.1 kHz samples per second of 16-bit mono, plus 44 bytes of header
		EXPECT_EQ(44100 * 2 + 44, s.tellg());
	}
	agi::fs::Remove(path);
}

TEST(lagi_audio, get_with_volume) {
	TestAudioProvider<> provider;
	int16_t buff[4];

	provider.GetInt16MonoAudioWithVolume(buff, 0, 4, 1.0);
	EXPECT_EQ(0, buff[0]);
	EXPECT_EQ(1, buff[1]);
	EXPECT_EQ(2, buff[2]);
	EXPECT_EQ(3, buff[3]);

	provider.GetInt16MonoAudioWithVolume(buff, 0, 4, 0.0);
	EXPECT_EQ(0, buff[0]);
	EXPECT_EQ(0, buff[1]);
	EXPECT_EQ(0, buff[2]);
	EXPECT_EQ(0, buff[3]);

	provider.GetInt16MonoAudioWithVolume(buff, 0, 4, 2.0);
	EXPECT_EQ(0, buff[0]);
	EXPECT_EQ(2, buff[1]);
	EXPECT_EQ(4, buff[2]);
	EXPECT_EQ(6, buff[3]);
}

TEST(lagi_audio, volume_should_clamp_rather_than_wrap) {
	TestAudioProvider<> provider;
	int16_t buff[1];
	provider.GetInt16MonoAudioWithVolume(buff, 30000, 1, 2.0);
	EXPECT_EQ(SHRT_MAX, buff[0]);
}

TEST(lagi_audio, ram_cache) {
	auto provider = agi::CreateRAMAudioProvider(agi::make_unique<TestAudioProvider<>>());
	EXPECT_EQ(1, provider->GetChannels());
	EXPECT_EQ(90 * 48000, provider->GetNumSamples());
	EXPECT_EQ(48000, provider->GetSampleRate());
	EXPECT_EQ(2, provider->GetBytesPerSample());
	EXPECT_EQ(false, provider->AreSamplesFloat());
	EXPECT_EQ(false, provider->NeedsCache());
	ASSERT_TRUE(WaitUntil([&] {
		return provider->GetDecodedSamples() == provider->GetNumSamples();
	}));

	uint16_t buff[512];
	provider->GetAudio(buff, (1 << 22) - 256, 512); // Stride two cache blocks

	for (size_t i = 0; i < 512; ++i)
		ASSERT_EQ(static_cast<uint16_t>((1 << 22) - 256 + i), buff[i]);
}

TEST(lagi_audio, ram_cache_reports_memory_stats) {
	auto provider = agi::CreateRAMAudioProvider(agi::make_unique<TestAudioProvider<>>());
	ASSERT_TRUE(WaitUntil([&] {
		return provider->GetDecodedSamples() == provider->GetNumSamples();
	}));

	auto const stats = provider->GetMemoryStats();
	EXPECT_EQ("RAM", stats.provider_name);
	EXPECT_EQ("memory", stats.storage_kind);
	EXPECT_GE(stats.storage_bytes, stats.logical_bytes);
	EXPECT_LT(stats.storage_bytes, stats.logical_bytes + (1u << 22));
	EXPECT_EQ(stats.logical_bytes, stats.decoded_bytes);
}

TEST(lagi_audio, ram_cache_preserves_wrapped_provider_name) {
	auto provider = agi::CreateRAMAudioProvider(agi::make_unique<NamedTestAudioProvider<>>());

	auto const stats = provider->GetMemoryStats();
	EXPECT_EQ("RAM (TestSource)", stats.provider_name);
}

TEST(lagi_audio, ram_cache_recovers_entire_failed_block_before_publishing_samples) {
	auto source = agi::make_unique<FailingRamCacheAudioProvider>(1);
	auto *raw = source.get();
	auto provider = agi::CreateRAMAudioProvider(std::move(source));
	ASSERT_TRUE(WaitUntil([&] {
		return provider->GetDecodedSamples() == provider->GetNumSamples();
	}));
	ASSERT_EQ(1, raw->first_block_reads);
	ASSERT_EQ(1, raw->second_block_reads);

	std::array<int16_t, 32> samples;
	provider->GetAudioChecked(samples.data(), 100, samples.size());
	for (size_t i = 0; i < samples.size(); ++i)
		EXPECT_EQ(FailingRamCacheAudioProvider::SampleAt(100 + i), samples[i]);
	EXPECT_EQ(2, raw->first_block_reads);
	EXPECT_EQ(0, raw->last_first_block_start);
	EXPECT_EQ(FailingRamCacheAudioProvider::BlockSamples, raw->last_first_block_count);

	// Recovery of one small requested range must also replace the partial
	// prefix and the rest of that block, without rereading its healthy neighbour.
	provider->GetAudioChecked(samples.data(), 0, samples.size());
	for (size_t i = 0; i < samples.size(); ++i)
		EXPECT_EQ(FailingRamCacheAudioProvider::SampleAt(i), samples[i]);
	auto const boundary_start = FailingRamCacheAudioProvider::BlockSamples - 16;
	provider->GetAudioChecked(samples.data(), boundary_start, samples.size());
	for (size_t i = 0; i < samples.size(); ++i)
		EXPECT_EQ(FailingRamCacheAudioProvider::SampleAt(boundary_start + i), samples[i]);
	EXPECT_EQ(2, raw->first_block_reads);
	EXPECT_EQ(1, raw->second_block_reads);
}

TEST(lagi_audio, ram_cache_failed_recovery_stays_retryable_and_does_not_poison_other_blocks) {
	auto source = agi::make_unique<FailingRamCacheAudioProvider>(3);
	auto *raw = source.get();
	auto provider = agi::CreateRAMAudioProvider(std::move(source));
	ASSERT_TRUE(WaitUntil([&] {
		return provider->GetDecodedSamples() == provider->GetNumSamples();
	}));
	ASSERT_EQ(1, raw->first_block_reads);

	std::array<int16_t, 32> samples;
	EXPECT_THROW(provider->GetAudioChecked(samples.data(), 0, samples.size()), agi::AudioDecodeError);
	EXPECT_EQ(2, raw->first_block_reads);
	provider->GetAudio(samples.data(), 0, samples.size());
	for (auto const sample : samples)
		EXPECT_EQ(0, sample);
	EXPECT_EQ(3, raw->first_block_reads);

	provider->GetAudioChecked(samples.data(), FailingRamCacheAudioProvider::BlockSamples, samples.size());
	for (size_t i = 0; i < samples.size(); ++i)
		EXPECT_EQ(FailingRamCacheAudioProvider::SampleAt(FailingRamCacheAudioProvider::BlockSamples + i), samples[i]);
	EXPECT_EQ(3, raw->first_block_reads);
	EXPECT_EQ(1, raw->second_block_reads);

	provider->GetAudioChecked(samples.data(), 0, samples.size());
	for (size_t i = 0; i < samples.size(); ++i)
		EXPECT_EQ(FailingRamCacheAudioProvider::SampleAt(i), samples[i]);
	EXPECT_EQ(4, raw->first_block_reads);
	provider->GetAudioChecked(samples.data(), 200, samples.size());
	for (size_t i = 0; i < samples.size(); ++i)
		EXPECT_EQ(FailingRamCacheAudioProvider::SampleAt(200 + i), samples[i]);
	EXPECT_EQ(4, raw->first_block_reads);
}

TEST(lagi_audio, ram_cache_retains_genuine_silence_without_redecoding) {
	auto source = agi::make_unique<FailingRamCacheAudioProvider>(0, true);
	auto *raw = source.get();
	auto provider = agi::CreateRAMAudioProvider(std::move(source));
	ASSERT_TRUE(WaitUntil([&] {
		return provider->GetDecodedSamples() == provider->GetNumSamples();
	}));

	std::array<int16_t, 32> samples;
	for (auto const start : {int64_t{0}, int64_t{100}, FailingRamCacheAudioProvider::BlockSamples - 16}) {
		samples.fill(-1);
		provider->GetAudioChecked(samples.data(), start, samples.size());
		for (auto const sample : samples)
			EXPECT_EQ(0, sample);
	}
	EXPECT_EQ(1, raw->first_block_reads);
	EXPECT_EQ(1, raw->second_block_reads);
}

TEST(lagi_audio, ram_cache_checked_reads_reject_unprocessed_samples_until_decode_completes) {
	auto source = agi::make_unique<FrontierRamCacheAudioProvider>();
	auto *raw = source.get();
	auto provider = agi::CreateRAMAudioProvider(std::move(source));
	ASSERT_TRUE(raw->WaitUntilEntered());
	EXPECT_EQ(FailingRamCacheAudioProvider::BlockSamples, provider->GetDecodedSamples());

	std::array<int16_t, 32> samples;
	auto const boundary_start = FailingRamCacheAudioProvider::BlockSamples - 16;
	EXPECT_THROW(provider->GetAudioChecked(samples.data(), boundary_start, samples.size()), agi::AudioDecodeError);
	provider->GetAudio(samples.data(), boundary_start, samples.size());
	EXPECT_EQ((std::array<int16_t, 32>{}), samples);

	// A healthy block must remain readable while the next source read is gated.
	provider->GetAudioChecked(samples.data(), 100, samples.size());
	for (size_t i = 0; i < samples.size(); ++i)
		EXPECT_EQ(FailingRamCacheAudioProvider::SampleAt(100 + i), samples[i]);
	raw->Release();
	ASSERT_TRUE(WaitUntil([&] {
		return provider->GetDecodedSamples() == provider->GetNumSamples();
	}));
	EXPECT_FALSE(raw->timed_out.load());
	provider->GetAudioChecked(samples.data(), boundary_start, samples.size());
	for (size_t i = 0; i < samples.size(); ++i)
		EXPECT_EQ(FailingRamCacheAudioProvider::SampleAt(boundary_start + i), samples[i]);
	EXPECT_EQ(1, raw->first_block_reads);
	EXPECT_EQ(1, raw->second_block_reads);
}

TEST(lagi_audio, hd_cache) {
	auto provider = agi::CreateHDAudioProvider(agi::make_unique<TestAudioProvider<>>(), agi::Path().Decode("?temp"));
	ASSERT_TRUE(WaitUntil([&] {
		return provider->GetDecodedSamples() == provider->GetNumSamples();
	}));

	uint16_t buff[512];
	provider->GetAudio(buff, (1 << 22) - 256, 512);

	for (size_t i = 0; i < 512; ++i)
		ASSERT_EQ(static_cast<uint16_t>((1 << 22) - 256 + i), buff[i]);
}

TEST(lagi_audio, hd_cache_reports_memory_stats) {
	auto provider = agi::CreateHDAudioProvider(agi::make_unique<TestAudioProvider<>>(), agi::Path().Decode("?temp"));
	ASSERT_TRUE(WaitUntil([&] {
		return provider->GetDecodedSamples() == provider->GetNumSamples();
	}));

	auto const stats = provider->GetMemoryStats();
	EXPECT_EQ("HD", stats.provider_name);
	EXPECT_EQ("disk", stats.storage_kind);
	EXPECT_EQ(static_cast<size_t>(90) * 48000 * sizeof(uint16_t), stats.storage_bytes);
	EXPECT_EQ(stats.storage_bytes, stats.logical_bytes);
	EXPECT_EQ(stats.logical_bytes, stats.decoded_bytes);
}

TEST(lagi_audio, hd_cache_preserves_wrapped_provider_name) {
	auto provider = agi::CreateHDAudioProvider(agi::make_unique<NamedTestAudioProvider<>>(), agi::Path().Decode("?temp"));

	auto const stats = provider->GetMemoryStats();
	EXPECT_EQ("HD (TestSource)", stats.provider_name);
}

TEST(lagi_audio, hd_cache_zero_fills_undecoded_tail) {
	auto source = agi::make_unique<BlockingSequenceAudioProvider>();
	auto *raw = source.get();
	auto provider = agi::CreateHDAudioProvider(std::move(source), agi::Path().Decode("?temp"));

	ASSERT_TRUE(raw->WaitUntilEntered());

	uint16_t buff[32];
	memset(buff, 0xFF, sizeof(buff));
	provider->GetAudio(buff, provider->GetNumSamples() - 32, 32);

	for (auto sample : buff)
		EXPECT_EQ(0, sample);

	raw->Release();
}

TEST(lagi_audio, hd_cache_destructor_stops_background_decode_promptly) {
	auto start = std::chrono::steady_clock::now();
	{
		auto provider = agi::CreateHDAudioProvider(agi::make_unique<SlowStopAudioProvider>(30), agi::Path().Decode("?temp"));
		agi::util::sleep_for(5);
	}
	auto elapsed = std::chrono::steady_clock::now() - start;
	EXPECT_LT(elapsed, std::chrono::milliseconds(500));
}

TEST(lagi_audio, convert_8bit) {
	auto provider = agi::CreateConvertAudioProvider(agi::make_unique<TestAudioProvider<uint8_t>>());

	int16_t data[256];
	provider->GetInt16MonoAudio(data, 0, 256);
	for (int i = 0; i < 256; ++i)
		ASSERT_EQ((i - 128) * 256, data[i]);
}

TEST(lagi_audio, sample_doubling_reports_output_memory_shape) {
	struct AudioProvider : agi::AudioProvider {
		AudioProvider() {
			channels = 1;
			num_samples = 90 * 20000;
			decoded_samples = num_samples;
			sample_rate = 20000;
			bytes_per_sample = 2;
			float_samples = false;
		}

		void FillBuffer(void *buf, int64_t start, int64_t count) const override {
			auto out = static_cast<int16_t *>(buf);
			for (int64_t end = start + count; start < end; ++start)
				*out++ = static_cast<int16_t>(start * 2);
		}
	};

	auto provider = agi::CreateConvertAudioProvider(agi::make_unique<AudioProvider>());

	auto const stats = provider->GetMemoryStats();
	EXPECT_EQ(static_cast<size_t>(90) * 40000 * sizeof(int16_t), stats.logical_bytes);
	EXPECT_EQ(stats.logical_bytes, stats.decoded_bytes);
	EXPECT_EQ(40000, stats.sample_rate);
	EXPECT_EQ(2, stats.bytes_per_sample);
	EXPECT_EQ(1, stats.channels);
	EXPECT_FALSE(stats.float_samples);
}

TEST(lagi_audio, convert_32bit) {
	auto src = agi::make_unique<TestAudioProvider<uint32_t>>(100000);
	src->bias = INT_MIN;
	auto provider = agi::CreateConvertAudioProvider(std::move(src));

	int16_t sample;
	provider->GetInt16MonoAudio(&sample, 0, 1);
	EXPECT_EQ(SHRT_MIN, sample);

	provider->GetInt16MonoAudio(&sample, 1LL << 31, 1);
	EXPECT_EQ(0, sample);

	provider->GetInt16MonoAudio(&sample, (1LL << 32) - 1, 1);
	EXPECT_EQ(SHRT_MAX, sample);
}

TEST(lagi_audio, sample_doubling) {
	struct AudioProvider : agi::AudioProvider {
		AudioProvider() {
			channels = 1;
			num_samples = 90 * 20000;
			decoded_samples = num_samples;
			sample_rate = 20000;
			bytes_per_sample = 2;
			float_samples = false;
		}

		void FillBuffer(void *buf, int64_t start, int64_t count) const override {
			auto out = static_cast<int16_t *>(buf);
			for (int64_t end = start + count; start < end; ++start)
				*out++ = (int16_t)(start * 2);
		}
	};

	auto provider = agi::CreateConvertAudioProvider(agi::make_unique<AudioProvider>());
	EXPECT_EQ(40000, provider->GetSampleRate());

	int16_t samples[6];
	for (int k = 0; k < 6; ++k) {
		SCOPED_TRACE(k);
		for (int i = k; i < 6; ++i) {
			SCOPED_TRACE(i);
			memset(samples, 0, sizeof(samples));
			provider->GetAudio(samples, k, i - k);
			for (int j = 0; j < i - k; ++j)
				EXPECT_EQ(j + k, samples[j]);
			for (int j = i - k; j < 6 - k; ++j)
				EXPECT_EQ(0, samples[j]);
		}
	}
}

TEST(lagi_audio, stereo_downmix) {
	struct AudioProvider : agi::AudioProvider {
		AudioProvider() {
			channels = 2;
			num_samples = 90 * 480000;
			decoded_samples = num_samples;
			sample_rate = 480000;
			bytes_per_sample = 2;
			float_samples = false;
		}

		void FillBuffer(void *buf, int64_t start, int64_t count) const override {
			auto out = static_cast<int16_t *>(buf);
			for (int64_t end = start + count; start < end; ++start) {
				*out++ = (int16_t)(start * 2);
				*out++ = 0;
			}
		}
	};

	auto provider = agi::CreateConvertAudioProvider(agi::make_unique<AudioProvider>());
	EXPECT_EQ(2, provider->GetChannels());

	int16_t samples[100];
	provider->GetInt16MonoAudio(samples, 0, 100);
	for (int i = 0; i < 100; ++i)
		EXPECT_EQ(i, samples[i]);
}

template<typename Float>
struct FloatAudioProvider : agi::AudioProvider {
	FloatAudioProvider() {
		channels = 1;
		num_samples = 90 * 480000;
		decoded_samples = num_samples;
		sample_rate = 480000;
		bytes_per_sample = sizeof(Float);
		float_samples = true;
	}

	void FillBuffer(void *buf, int64_t start, int64_t count) const override {
		auto out = static_cast<Float *>(buf);
		for (int64_t end = start + count; start < end; ++start) {
			auto shifted = start + SHRT_MIN;
			*out++ = (Float)(shifted) / (-SHRT_MIN);
		}
	}
};

TEST(lagi_audio, float_conversion) {
	auto provider = agi::CreateConvertAudioProvider(agi::make_unique<FloatAudioProvider<float>>());
	EXPECT_TRUE(provider->AreSamplesFloat());

	int16_t samples[1 << 16];
	provider->GetInt16MonoAudio(samples, 0, 1 << 16);
	for (int i = 0; i < (1 << 16); ++i)
		ASSERT_EQ(i + SHRT_MIN, samples[i]);
}

TEST(lagi_audio, double_conversion) {
	auto provider = agi::CreateConvertAudioProvider(agi::make_unique<FloatAudioProvider<double>>());
	EXPECT_TRUE(provider->AreSamplesFloat());

	int16_t samples[1 << 16];
	provider->GetInt16MonoAudio(samples, 0, 1 << 16);
	for (int i = 0; i < (1 << 16); ++i)
		ASSERT_EQ(i + SHRT_MIN, samples[i]);
}

TEST(lagi_audio, pcm_simple) {
	auto path = agi::Path().Decode("?temp/pcm_simple");
	{
		TestAudioProvider<> provider;
		agi::SaveAudioClip(provider, path, 0, 1000);
	}

	{
		auto provider = agi::CreatePCMAudioProvider(path, nullptr);
		EXPECT_EQ(1, provider->GetChannels());
		EXPECT_EQ(48000, provider->GetNumSamples());
		EXPECT_EQ(48000, provider->GetSampleRate());
		EXPECT_EQ(2, provider->GetBytesPerSample());
		EXPECT_EQ(false, provider->AreSamplesFloat());
		EXPECT_EQ(false, provider->NeedsCache());

		for (int i = 0; i < 100; ++i) {
			uint16_t sample;
			provider->GetAudio(&sample, i, 1);
			ASSERT_EQ(i, sample);
		}
	}

	agi::fs::Remove(path);
}

TEST(lagi_audio, pcm_reports_mapped_memory_stats) {
	auto path = agi::Path().Decode("?temp/pcm_memory_stats");
	{
		TestAudioProvider<> provider;
		agi::SaveAudioClip(provider, path, 0, 1000);
	}

	{
		auto provider = agi::CreatePCMAudioProvider(path, nullptr);
		auto const stats = provider->GetMemoryStats();
		EXPECT_EQ("PCM", stats.provider_name);
		EXPECT_EQ("mapped", stats.storage_kind);
		EXPECT_EQ(static_cast<size_t>(44) + static_cast<size_t>(48000) * sizeof(uint16_t), stats.storage_bytes);
		EXPECT_EQ(static_cast<size_t>(48000) * sizeof(uint16_t), stats.logical_bytes);
		EXPECT_EQ(stats.logical_bytes, stats.decoded_bytes);
	}

	agi::fs::Remove(path);
}

TEST(lagi_audio, pcm_truncated) {
	auto path = agi::Path().Decode("?temp/pcm_truncated");
	{
		TestAudioProvider<> provider;
		agi::SaveAudioClip(provider, path, 0, 1000);
	}

	char file[1000];

	{ std::ifstream s(path, std::ios_base::binary); s.read(file, sizeof file); }
	{ std::ofstream s(path, std::ios_base::binary); s.write(file, sizeof file); }

	{
		auto provider = agi::CreatePCMAudioProvider(path, nullptr);

		// Should still report full duration
		EXPECT_EQ(48000, provider->GetNumSamples());

		// And should zero-pad past the end
		int64_t sample_count = (1000 - 44) / 2;
		uint16_t sample;

		provider->GetAudio(&sample, sample_count - 1, 1);
		EXPECT_EQ(sample_count - 1, sample);

		provider->GetAudio(&sample, sample_count, 1);
		EXPECT_EQ(0, sample);
	}

	agi::fs::Remove(path);
}

#define RIFF "RIFF\0\0\0\x60WAVE"
#define FMT_VALID "fmt \x10\0\0\0\1\0\1\0\x10\0\0\0\x20\0\0\0\2\0\x10\0"
#define DATA_VALID "data\1\0\0\0\0\0"
#define WRITE(str) do { std::ofstream s(path, std::ios_base::binary); s.write(str, sizeof(str) - 1); } while (false)

TEST(lagi_audio, pcm_incomplete) {
	auto path = agi::Path().Decode("?temp/pcm_incomplete");

	agi::fs::Remove(path);
	ASSERT_THROW(agi::CreatePCMAudioProvider(path, nullptr), agi::fs::FileNotFound);

	{std::ofstream(path, std::ios_base::binary); }
	ASSERT_THROW(agi::CreatePCMAudioProvider(path, nullptr), agi::AudioDataNotFound);

	// Invalid tags
	WRITE("ASDF");
	ASSERT_THROW(agi::CreatePCMAudioProvider(path, nullptr), agi::AudioDataNotFound);

	WRITE("RIFF\0\0\0\x60" "ASDF");
	ASSERT_THROW(agi::CreatePCMAudioProvider(path, nullptr), agi::AudioDataNotFound);

	// Incomplete files
	const char valid_file[] = RIFF FMT_VALID DATA_VALID;

	// -1 for nul term, -3 so that longest file is still invalid
	for (size_t i = 0; i < sizeof(valid_file) - 4; ++i) {
		{
			std::ofstream s(path, std::ios_base::binary);
			s.write(valid_file, i);
		}
		ASSERT_THROW(agi::CreatePCMAudioProvider(path, nullptr), agi::AudioDataNotFound);
	}

	// fmt must come before data
	WRITE(RIFF "data\0\0\0\x60");
	ASSERT_THROW(agi::CreatePCMAudioProvider(path, nullptr), agi::AudioProviderError);

	// Bad compression format
	WRITE(RIFF "fmt \x60\0\0\0\2\0");
	ASSERT_THROW(agi::CreatePCMAudioProvider(path, nullptr), agi::AudioProviderError);

	// Multiple fmt chunks not supported
	WRITE(RIFF FMT_VALID FMT_VALID);
	ASSERT_THROW(agi::CreatePCMAudioProvider(path, nullptr), agi::AudioProviderError);

	agi::fs::Remove(path);
}

TEST(lagi_audio, multiple_data_chunks) {
	auto path = agi::Path().Decode("?temp/multiple_data");

	WRITE(RIFF FMT_VALID "data\2\0\0\0\1\0" "data\2\0\0\0\2\0" "data\2\0\0\0\3\0");

	{
		auto provider = agi::CreatePCMAudioProvider(path, nullptr);
		ASSERT_EQ(3, provider->GetNumSamples());

		uint16_t samples[3];

		provider->GetAudio(samples, 0, 3);
		EXPECT_EQ(1, samples[0]);
		EXPECT_EQ(2, samples[1]);
		EXPECT_EQ(3, samples[2]);

		samples[1] = 5;
		provider->GetAudio(samples, 2, 1);
		EXPECT_EQ(3, samples[0]);
		EXPECT_EQ(5, samples[1]);

		provider->GetAudio(samples, 1, 1);
		EXPECT_EQ(2, samples[0]);
		EXPECT_EQ(5, samples[1]);

		provider->GetAudio(samples, 0, 1);
		EXPECT_EQ(1, samples[0]);
		EXPECT_EQ(5, samples[1]);
	}

	agi::fs::Remove(path);
}

#define WAVE64_FILE \
	"riff\x2e\x91\xcf\x11\xa5\xd6\x28\xdb\x04\xc1\x00\x00"   /* RIFF GUID */          \
	"\x74\x00\0\0\0\0\0\0"                                   /* file size */          \
	"wave\xf3\xac\xd3\x11\x8c\xd1\x00\xc0\x4f\x8e\xdb\x8a"   /* WAVE GUID */          \
	"fmt \xf3\xac\xd3\x11\x8c\xd1\x00\xc0\x4f\x8e\xdb\x8a"   /* fmt GUID */           \
	"\x30\x00\0\0\0\0\0\0"                                   /* fmt chunk size */     \
	"\1\0\1\0\x10\0\0\0\x20\0\0\0\2\0\x10\0\0\0\0\0\0\0\0\0" /* fmt chunk */          \
	"data\xf3\xac\xd3\x11\x8c\xd1\x00\xc0\x4f\x8e\xdb\x8a"   /* data GUID */          \
	"\x1c\0\0\0\0\0\0\0"                                     /* data chunk size */    \
	"\1\0\2\0"                                               /* actual sample data */ \

TEST(lagi_audio, wave64_simple) {
	auto path = agi::Path().Decode("?temp/w64_valid");
	WRITE(WAVE64_FILE);

	{
		auto provider = agi::CreatePCMAudioProvider(path, nullptr);
		ASSERT_EQ(2, provider->GetNumSamples());

		uint16_t samples[2];
		provider->GetAudio(samples, 0, 2);
		EXPECT_EQ(1, samples[0]);
		EXPECT_EQ(2, samples[1]);
	}

	agi::fs::Remove(path);
}

TEST(lagi_audio, wave64_truncated) {
	auto path = agi::Path().Decode("?temp/w64_truncated");

	// Should be invalid until there's an entire sample
	for (size_t i = 0; i < sizeof(WAVE64_FILE) - 4; ++i) {
		{
			std::ofstream s(path, std::ios_base::binary);
			s.write(WAVE64_FILE, i);
		}
		ASSERT_THROW(agi::CreatePCMAudioProvider(path, nullptr), agi::AudioDataNotFound);
	}

	{
		std::ofstream s(path, std::ios_base::binary);
		s.write(WAVE64_FILE, sizeof(WAVE64_FILE) - 3);
	}
	ASSERT_NO_THROW(agi::CreatePCMAudioProvider(path, nullptr));

	{
		auto provider = agi::CreatePCMAudioProvider(path, nullptr);
		uint16_t sample;
		provider->GetAudio(&sample, 0, 1);
		EXPECT_EQ(1, sample);
	}

	agi::fs::Remove(path);
}

namespace {
struct FailingHDAudioProvider : agi::AudioProvider {
	static constexpr int64_t BlockSamples = 65536;
	int failed_reads;
	bool silence;
	mutable std::atomic<int> first_reads{0};
	mutable std::atomic<int> second_reads{0};
	mutable std::atomic<int64_t> last_start{0};
	mutable std::atomic<int64_t> last_count{0};

	explicit FailingHDAudioProvider(int failed_reads, bool silence = false)
		: failed_reads(failed_reads), silence(silence) {
		channels = 1;
		sample_rate = 48000;
		bytes_per_sample = sizeof(int16_t);
		num_samples = BlockSamples + 64;
		decoded_samples = num_samples;
	}
	static int16_t SampleAt(int64_t frame) { return static_cast<int16_t>(1 + frame % 30000); }
	void FillBuffer(void *buffer, int64_t start, int64_t count) const override {
		auto *samples = static_cast<int16_t *>(buffer);
		if (start < BlockSamples) {
			last_start = start;
			last_count = count;
			if (++first_reads <= failed_reads) {
				std::fill_n(samples, std::min<int64_t>(count, 32), int16_t{-23456});
				throw agi::AudioDecodeError("injected partial HD cache read");
			}
		}
		else
			++second_reads;
		for (int64_t i = 0; i < count; ++i)
			samples[i] = silence ? 0 : SampleAt(start + i);
	}
};

struct FrontierHDAudioProvider : agi::AudioProvider {
	mutable std::mutex mutex;
	mutable std::condition_variable condition;
	mutable bool entered = false;
	mutable bool released = false;
	mutable std::atomic<bool> timed_out{false};

	FrontierHDAudioProvider(int channel_count, int sample_bytes) {
		channels = channel_count;
		bytes_per_sample = sample_bytes;
		sample_rate = 48000;
		num_samples = 65540;
		decoded_samples = num_samples;
	}
	static unsigned char ByteAt(int64_t frame, int byte) {
		return static_cast<unsigned char>(1 + (frame + byte) % 126);
	}
	bool WaitForSecondRead() const {
		std::unique_lock lock(mutex);
		return condition.wait_for(lock, std::chrono::seconds(2), [&] { return entered; });
	}
	void Release() const {
		{
			std::scoped_lock lock(mutex);
			released = true;
		}
		condition.notify_all();
	}
	void FillBuffer(void *buffer, int64_t start, int64_t count) const override {
		if (start >= 65536) {
			std::unique_lock lock(mutex);
			entered = true;
			condition.notify_all();
			if (!condition.wait_for(lock, std::chrono::seconds(2), [&] { return released; })) {
				timed_out = true;
				throw agi::AudioDecodeError("HD cache frontier test timed out");
			}
		}
		auto *bytes = static_cast<unsigned char *>(buffer);
		int const frame_bytes = channels * bytes_per_sample;
		for (int64_t frame = 0; frame < count; ++frame)
			for (int byte = 0; byte < frame_bytes; ++byte)
				bytes[frame * frame_bytes + byte] = ByteAt(start + frame, byte);
	}
};
}

TEST(lagi_audio, hd_cache_recovers_complete_failed_block_before_returning_samples) {
	auto source = std::make_unique<FailingHDAudioProvider>(1);
	auto *raw = source.get();
	auto cache = agi::CreateHDAudioProvider(std::move(source), agi::Path().Decode("?temp"));
	ASSERT_TRUE(WaitUntil([&] { return cache->GetDecodedSamples() == cache->GetNumSamples(); }));
	ASSERT_EQ(1, raw->first_reads);
	std::array<int16_t, 32> samples;
	cache->GetAudioChecked(samples.data(), 100, samples.size());
	for (size_t i = 0; i < samples.size(); ++i)
		EXPECT_EQ(FailingHDAudioProvider::SampleAt(100 + i), samples[i]);
	EXPECT_EQ(2, raw->first_reads);
	EXPECT_EQ(0, raw->last_start);
	EXPECT_EQ(FailingHDAudioProvider::BlockSamples, raw->last_count);
	for (int64_t start : {int64_t{0}, FailingHDAudioProvider::BlockSamples - 16}) {
		cache->GetAudioChecked(samples.data(), start, samples.size());
		for (size_t i = 0; i < samples.size(); ++i)
			EXPECT_EQ(FailingHDAudioProvider::SampleAt(start + i), samples[i]);
	}
	EXPECT_EQ(2, raw->first_reads);
	EXPECT_EQ(1, raw->second_reads);
}

TEST(lagi_audio, hd_cache_recovery_failure_stays_retryable_and_preserves_healthy_blocks) {
	auto source = std::make_unique<FailingHDAudioProvider>(3);
	auto *raw = source.get();
	auto cache = agi::CreateHDAudioProvider(std::move(source), agi::Path().Decode("?temp"));
	ASSERT_TRUE(WaitUntil([&] { return cache->GetDecodedSamples() == cache->GetNumSamples(); }));
	std::array<int16_t, 32> samples;
	EXPECT_THROW(cache->GetAudioChecked(samples.data(), 0, samples.size()), agi::AudioDecodeError);
	EXPECT_EQ(2, raw->first_reads);
	cache->GetAudio(samples.data(), 0, samples.size());
	EXPECT_EQ((std::array<int16_t, 32>{}), samples);
	EXPECT_EQ(3, raw->first_reads);
	cache->GetAudioChecked(samples.data(), FailingHDAudioProvider::BlockSamples, samples.size());
	for (size_t i = 0; i < samples.size(); ++i)
		EXPECT_EQ(FailingHDAudioProvider::SampleAt(FailingHDAudioProvider::BlockSamples + i), samples[i]);
	EXPECT_EQ(3, raw->first_reads);
	EXPECT_EQ(1, raw->second_reads);
	cache->GetAudioChecked(samples.data(), 0, samples.size());
	for (size_t i = 0; i < samples.size(); ++i)
		EXPECT_EQ(FailingHDAudioProvider::SampleAt(i), samples[i]);
	EXPECT_EQ(4, raw->first_reads);
	cache->GetAudioChecked(samples.data(), 100, samples.size());
	EXPECT_EQ(4, raw->first_reads);
}

TEST(lagi_audio, hd_cache_keeps_successful_silence_without_redecoding) {
	auto source = std::make_unique<FailingHDAudioProvider>(0, true);
	auto *raw = source.get();
	auto cache = agi::CreateHDAudioProvider(std::move(source), agi::Path().Decode("?temp"));
	ASSERT_TRUE(WaitUntil([&] { return cache->GetDecodedSamples() == cache->GetNumSamples(); }));
	std::array<int16_t, 32> samples;
	for (int64_t start : {int64_t{0}, int64_t{100}, FailingHDAudioProvider::BlockSamples - 16}) {
		samples.fill(-1);
		cache->GetAudioChecked(samples.data(), start, samples.size());
		EXPECT_EQ((std::array<int16_t, 32>{}), samples);
	}
	EXPECT_EQ(1, raw->first_reads);
	EXPECT_EQ(1, raw->second_reads);
}

TEST(lagi_audio, hd_cache_rejects_unprocessed_frames_without_corrupting_any_sample_format) {
	for (auto const [channels, sample_bytes] : {std::pair{1, 1}, {2, 2}, {6, 2}, {2, 3}, {2, 4}}) {
		SCOPED_TRACE(channels);
		SCOPED_TRACE(sample_bytes);
		auto source = std::make_unique<FrontierHDAudioProvider>(channels, sample_bytes);
		auto *raw = source.get();
		auto cache = agi::CreateHDAudioProvider(std::move(source), agi::Path().Decode("?temp"));
		ASSERT_TRUE(raw->WaitForSecondRead());
		ASSERT_EQ(65536, cache->GetDecodedSamples());
		int const frame_bytes = channels * sample_bytes;
		std::vector<unsigned char> guarded(frame_bytes * 2 + 2, 0xCD);
		EXPECT_THROW(cache->GetAudioChecked(guarded.data() + 1, 65535, 2), agi::AudioDecodeError);
		EXPECT_TRUE(std::ranges::all_of(guarded, [](auto value) { return value == 0xCD; }));
		cache->GetAudio(guarded.data() + 1, 65535, 2);
		EXPECT_EQ(0xCD, guarded.front());
		EXPECT_EQ(0xCD, guarded.back());
		for (int byte = 0; byte < frame_bytes * 2; ++byte)
			EXPECT_EQ(sample_bytes == 1 ? 128 : 0, guarded[byte + 1]);
		// The background decoder owns source I/O here. A healthy cache read
		// must not wait for that read, including while acquiring a mapping view.
		cache->GetAudioChecked(guarded.data() + 1, 0, 1);
		for (int byte = 0; byte < frame_bytes; ++byte)
			EXPECT_EQ(FrontierHDAudioProvider::ByteAt(0, byte), guarded[byte + 1]);
		raw->Release();
		ASSERT_TRUE(WaitUntil([&] { return cache->GetDecodedSamples() == cache->GetNumSamples(); }));
		EXPECT_FALSE(raw->timed_out);
		cache->GetAudioChecked(guarded.data() + 1, 65535, 2);
		for (int frame = 0; frame < 2; ++frame)
			for (int byte = 0; byte < frame_bytes; ++byte)
				EXPECT_EQ(FrontierHDAudioProvider::ByteAt(65535 + frame, byte), guarded[1 + frame * frame_bytes + byte]);
		EXPECT_EQ(0xCD, guarded.front());
		EXPECT_EQ(0xCD, guarded.back());
	}
}

TEST(lagi_audio, hd_cache_waveform_recovery_matches_uncached_source) {
	auto source = std::make_unique<FailingHDAudioProvider>(1);
	auto *raw = source.get();
	auto cache = agi::CreateHDAudioProvider(std::move(source), agi::Path().Decode("?temp"));
	ASSERT_TRUE(WaitUntil([&] { return cache->GetDecodedSamples() == cache->GetNumSamples(); }));
	ASSERT_EQ(1, raw->first_reads);
	FailingHDAudioProvider reference_provider(0);
	auto display = CreateInt16MonoAudioDisplaySource(cache.get());
	auto reference_display = CreateInt16MonoAudioDisplaySource(&reference_provider);
	AudioWaveformSummaryCache waveform;
	AudioWaveformSummaryCache reference;
	waveform.SetSource(display.get());
	reference.SetSource(reference_display.get());
	waveform.SetMillisecondsPerPixel(1.0);
	reference.SetMillisecondsPerPixel(1.0);

	// Start inside the failed block, revisit its prefix and a later uncached
	// summary, then check the original cached summary.
	for (size_t block_index : {size_t{1}, size_t{0}, size_t{40}, size_t{1}}) {
		SCOPED_TRACE(block_index);
		auto const actual = waveform.Get(block_index);
		auto const expected = reference.Get(block_index);
		ASSERT_NE(nullptr, actual);
		ASSERT_NE(nullptr, expected);
		ASSERT_TRUE(actual->has_exact_pcm16);
		ASSERT_TRUE(expected->has_exact_pcm16);
		for (size_t column = 0; column < AudioWaveformSummaryBlock::width; ++column) {
			SCOPED_TRACE(column);
			EXPECT_FLOAT_EQ(expected->summaries[column].peak_min, actual->summaries[column].peak_min);
			EXPECT_FLOAT_EQ(expected->summaries[column].peak_max, actual->summaries[column].peak_max);
			EXPECT_FLOAT_EQ(expected->summaries[column].avg_min, actual->summaries[column].avg_min);
			EXPECT_FLOAT_EQ(expected->summaries[column].avg_max, actual->summaries[column].avg_max);
			EXPECT_EQ(expected->pcm16_summaries[column].peak_min, actual->pcm16_summaries[column].peak_min);
			EXPECT_EQ(expected->pcm16_summaries[column].peak_max, actual->pcm16_summaries[column].peak_max);
			EXPECT_EQ(expected->pcm16_summaries[column].avg_min_accum, actual->pcm16_summaries[column].avg_min_accum);
			EXPECT_EQ(expected->pcm16_summaries[column].avg_max_accum, actual->pcm16_summaries[column].avg_max_accum);
		}
		EXPECT_EQ(2, raw->first_reads);
		EXPECT_EQ(1, raw->second_reads);
		EXPECT_EQ(0, raw->last_start);
		EXPECT_EQ(FailingHDAudioProvider::BlockSamples, raw->last_count);
	}
}

TEST(lagi_audio, hd_cache_spectrum_recovery_matches_uncached_source) {
	auto source = std::make_unique<FailingHDAudioProvider>(1);
	auto *raw = source.get();
	auto cache = agi::CreateHDAudioProvider(std::move(source), agi::Path().Decode("?temp"));
	ASSERT_TRUE(WaitUntil([&] { return cache->GetDecodedSamples() == cache->GetNumSamples(); }));
	ASSERT_EQ(1, raw->first_reads);
	FailingHDAudioProvider reference_provider(0);
	auto display = CreateAudioDisplaySource(cache.get());
	auto reference_display = CreateAudioDisplaySource(&reference_provider);
	AudioSpectrumAnalysisCache spectrum;
	AudioSpectrumAnalysisCache reference;
	spectrum.SetSource(display.get());
	reference.SetSource(reference_display.get());
	spectrum.SetResolution(9, 7);
	reference.SetResolution(9, 7);

	// Exercise initial recovery, a rolling FFT read, leading padding, the HD
	// block boundary with EOF padding, and a previously computed FFT.
	for (size_t block_index : {size_t{8}, size_t{9}, size_t{0}, size_t{511}, size_t{8}}) {
		SCOPED_TRACE(block_index);
		auto const actual = spectrum.Get(block_index);
		auto const expected = reference.Get(block_index);
		ASSERT_NE(nullptr, actual);
		ASSERT_NE(nullptr, expected);
		for (size_t bin = 0; bin < 512; ++bin)
			EXPECT_FLOAT_EQ(expected[bin], actual[bin]) << "bin " << bin;
		EXPECT_EQ(2, raw->first_reads);
		EXPECT_EQ(1, raw->second_reads);
		EXPECT_EQ(0, raw->last_start);
		EXPECT_EQ(FailingHDAudioProvider::BlockSamples, raw->last_count);
	}
}

namespace {
class MultichannelCacheAudioProvider final : public agi::AudioProvider {
	public:
	mutable std::atomic<int> reads{0};
	MultichannelCacheAudioProvider(int channel_count, int sample_bytes, int64_t frames) {
		channels = channel_count;
		bytes_per_sample = sample_bytes;
		sample_rate = 48000;
		num_samples = frames;
		decoded_samples = num_samples;
	}
	static unsigned char ByteAt(int64_t frame, int byte) {
		return static_cast<unsigned char>(1 + (frame * 7 + byte) % 126);
	}
	void FillBuffer(void *buffer, int64_t start, int64_t count) const override {
		++reads;
		auto *bytes = static_cast<unsigned char *>(buffer);
		int const frame_bytes = channels * bytes_per_sample;
		for (int64_t frame = 0; frame < count; ++frame)
			for (int byte = 0; byte < frame_bytes; ++byte)
				bytes[frame * frame_bytes + byte] = ByteAt(start + frame, byte);
	}
};
}

TEST(lagi_audio, ram_cache_uses_whole_frames_for_multichannel_capacity) {
	for (auto const [channels, sample_bytes] : {std::pair{6, 2}, {3, 2}, {2, 3}}) {
		int const frame_bytes = channels * sample_bytes;
		int64_t const frames_per_block = (1 << 22) / frame_bytes;
		for (int64_t frames : {frames_per_block - 1, frames_per_block, frames_per_block + 1, frames_per_block * 3 + 1}) {
			SCOPED_TRACE(channels);
			SCOPED_TRACE(sample_bytes);
			SCOPED_TRACE(frames);
			auto source = std::make_unique<MultichannelCacheAudioProvider>(channels, sample_bytes, frames);
			auto *raw = source.get();
			auto cache = agi::CreateRAMAudioProvider(agi::CreateConvertAudioProvider(std::move(source)));
			ASSERT_EQ(channels, cache->GetChannels());
			ASSERT_EQ(sample_bytes, cache->GetBytesPerSample());
			ASSERT_TRUE(WaitUntil([&] { return cache->GetDecodedSamples() == cache->GetNumSamples(); }));
			EXPECT_EQ(frames, cache->GetDecodedSamples());
			EXPECT_EQ(frames / frames_per_block + (frames % frames_per_block != 0), raw->reads);
			std::vector<unsigned char> tail(frame_bytes * 4, 0xCD);
			cache->GetAudioChecked(tail.data(), frames - 2, 4);
			for (int frame = 0; frame < 4; ++frame)
				for (int byte = 0; byte < frame_bytes; ++byte)
					EXPECT_EQ(frame < 2 ? MultichannelCacheAudioProvider::ByteAt(frames - 2 + frame, byte) : 0,
							  tail[frame * frame_bytes + byte]);
		}
	}
}

TEST(lagi_audio, hd_cache_instances_keep_independent_samples_when_opened_together) {
	struct CacheDirectory {
		agi::fs::path path = agi::fs::UniquePath(agi::Path().Decode("?temp") / "audio-cache-isolation-%%%%%%%%%%%%%%%%");
		~CacheDirectory() {
			std::error_code error;
			std::filesystem::remove(path, error);
		}
	} directory;
	ASSERT_TRUE(std::filesystem::create_directory(directory.path));
	std::vector<std::unique_ptr<agi::AudioProvider>> caches;
	const auto started = std::chrono::steady_clock::now();
	for (int index = 0; index < 4; ++index) {
		auto source = std::make_unique<TestAudioProvider<int16_t>>(1);
		source->bias = (index + 1) * 1000;
		caches.push_back(agi::CreateHDAudioProvider(std::move(source), directory.path));
	}
	// Four live providers opened within two seconds must share at least one
	// second-based filename under the old scheme, even across a clock tick.
	ASSERT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
				  std::chrono::steady_clock::now() - started)
				  .count(),
			  2000);
	ASSERT_TRUE(WaitUntil([&] {
		return std::ranges::all_of(caches, [](auto const& cache) {
			return cache->GetDecodedSamples() == cache->GetNumSamples();
		});
	}));
#ifdef _WIN32
	EXPECT_EQ(4, std::distance(std::filesystem::directory_iterator(directory.path), std::filesystem::directory_iterator()));
#endif
	auto check_cache = [&](size_t index) {
		const int bias = static_cast<int>(index + 1) * 1000;
		std::vector<int16_t> samples(static_cast<size_t>(caches[index]->GetNumSamples()));
		std::vector<int16_t> expected(samples.size());
		for (size_t frame = 0; frame < samples.size(); ++frame)
			expected[frame] = static_cast<int16_t>(frame + bias);
		caches[index]->GetAudioChecked(samples.data(), 0, samples.size());
		EXPECT_EQ(expected, samples) << "cache=" << index;
	};
	for (size_t index = 0; index < caches.size(); ++index)
		check_cache(index);
	caches.front().reset();
#ifdef _WIN32
	EXPECT_EQ(3, std::distance(std::filesystem::directory_iterator(directory.path), std::filesystem::directory_iterator()));
#endif
	for (size_t index = 1; index < caches.size(); ++index)
		check_cache(index);
}

namespace {
struct AudioExportDirectory {
	std::filesystem::path path = agi::fs::UniquePath(std::filesystem::temp_directory_path() / "aegisub-audio-export-%%%%%%%%");
	AudioExportDirectory() { std::filesystem::create_directory(path); }
	~AudioExportDirectory() {
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}
	[[nodiscard]] std::vector<std::string> TempFiles() const {
		std::vector<std::string> files;
		agi::fs::DirectoryIterator(path, "clip_tmp_*.wav").GetAll(files);
		return files;
	}
};

std::string ReadExportFile(std::filesystem::path const& path) {
	std::ifstream input(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class ExportAudioProvider final : public TestAudioProvider<int16_t> {
	int fail_on_read;

	public:
	mutable int reads = 0;
	explicit ExportAudioProvider(int failure = 0) : TestAudioProvider<int16_t>(1), fail_on_read(failure) {}
	void FillBuffer(void *buffer, int64_t start, int64_t count) const override {
		++reads;
		TestAudioProvider<int16_t>::FillBuffer(buffer, start, count);
		if (reads == fail_on_read) {
			throw agi::AudioDecodeError("injected audio export failure");
		}
	}
};
}

TEST(lagi_audio, save_audio_clip_commits_complete_multichunk_pcm_over_existing_file) {
	AudioExportDirectory directory;
	auto const target = directory.path / "clip.wav";
	{
		std::ofstream(target, std::ios::binary) << "old destination";
	}
	ExportAudioProvider source;
	source.bias = 123;
	ASSERT_NO_THROW(agi::SaveAudioClip(source, target, 0, 1000));
	EXPECT_EQ(2, source.reads);
	auto const bytes = ReadExportFile(target);
	ASSERT_EQ(44U + 48000U * 2U, bytes.size());
	EXPECT_EQ("RIFF", bytes.substr(0, 4));
	EXPECT_EQ("WAVEfmt ", bytes.substr(8, 8));
	EXPECT_EQ("data", bytes.substr(36, 4));
	for (size_t i = 0; i < 48000; ++i) {
		auto const expected = static_cast<uint16_t>(i + 123);
		auto const actual = static_cast<unsigned char>(bytes[44 + i * 2]) | (static_cast<unsigned char>(bytes[45 + i * 2]) << 8);
		ASSERT_EQ(expected, actual) << "sample " << i;
	}
	EXPECT_TRUE(directory.TempFiles().empty());
}

class AudioExportDecodeFailureTest : public ::testing::TestWithParam<std::pair<int, bool>> {};

TEST_P(AudioExportDecodeFailureTest, abort_preserves_destination_and_removes_partial_wave) {
	auto const [failed_read, destination_exists] = GetParam();
	AudioExportDirectory directory;
	auto const target = directory.path / "clip.wav";
	if (destination_exists) {
		std::ofstream(target, std::ios::binary) << "original destination bytes";
	}
	ExportAudioProvider source(failed_read);
	EXPECT_THROW(agi::SaveAudioClip(source, target, 0, 1000), agi::AudioDecodeError);
	EXPECT_EQ(failed_read, source.reads);
	if (destination_exists) {
		EXPECT_EQ("original destination bytes", ReadExportFile(target));
	}
	else {
		EXPECT_FALSE(std::filesystem::exists(target));
	}
	EXPECT_TRUE(directory.TempFiles().empty());
}

INSTANTIATE_TEST_SUITE_P(FirstAndLateRead, AudioExportDecodeFailureTest,
						 ::testing::Values(std::pair{1, false}, std::pair{1, true}, std::pair{2, false}, std::pair{2, true}),
						 [](auto const& info) {
							 return std::string(info.param.first == 1 ? "First" : "Late") + (info.param.second ? "Existing" : "Absent");
						 });

TEST(lagi_audio, save_audio_clip_reports_commit_failure_and_cleans_temporary_wave) {
	AudioExportDirectory directory;
	auto const target = directory.path / "clip.wav";
	ASSERT_TRUE(std::filesystem::create_directory(target));
	{
		std::ofstream(target / "marker", std::ios::binary) << "preserved destination";
	}
	ExportAudioProvider source;
	EXPECT_THROW(agi::SaveAudioClip(source, target, 0, 1000), agi::fs::FileSystemError);
	EXPECT_EQ(2, source.reads);
	EXPECT_TRUE(std::filesystem::is_directory(target));
	EXPECT_EQ("preserved destination", ReadExportFile(target / "marker"));
	EXPECT_TRUE(directory.TempFiles().empty());
}

TEST(lagi_audio, save_audio_clip_rejects_unready_ram_cache_without_replacing_destination) {
	AudioExportDirectory directory;
	auto const target = directory.path / "clip.wav";
	{
		std::ofstream(target, std::ios::binary) << "original destination";
	}
	auto source = std::make_unique<FrontierRamCacheAudioProvider>();
	auto *raw = source.get();
	auto cache = agi::CreateRAMAudioProvider(std::move(source));
	auto release = agi::make_scope_exit([&] { raw->Release(); });
	ASSERT_TRUE(raw->WaitUntilEntered());
	ASSERT_EQ(FrontierRamCacheAudioProvider::BlockSamples, cache->GetDecodedSamples());
	// This range begins in the ready first block and crosses the blocked second block.
	int const start_ms = static_cast<int>(FrontierRamCacheAudioProvider::BlockSamples * 1000 / 48000);
	EXPECT_THROW(agi::SaveAudioClip(*cache, target, start_ms, start_ms + 2), agi::AudioDecodeError);
	EXPECT_EQ("original destination", ReadExportFile(target));
	EXPECT_TRUE(directory.TempFiles().empty());
	EXPECT_FALSE(raw->timed_out.load());
}
