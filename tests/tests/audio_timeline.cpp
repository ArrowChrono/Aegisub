#include <main.h>

#include "../../src/audio_provider_timeline.h"
#include "../../src/mkv_wrap.h"

#include <libaegisub/fs.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {
template <class Sample>
class TimelineSamples final : public agi::AudioProvider {
	std::vector<Sample> samples;

	void FillBuffer(void *buffer, int64_t start, int64_t count) const override {
		if (fail_reads)
			throw agi::AudioDecodeError("timeline test decode failure");
		memcpy(buffer, samples.data() + start * channels, static_cast<size_t>(count) * channels * sizeof(Sample));
	}

	public:
	bool fail_reads = false;

	explicit TimelineSamples(std::vector<Sample> values, int channel_count = 1)
		: samples(std::move(values)) {
		channels = channel_count;
		num_samples = static_cast<int64_t>(samples.size()) / channels;
		decoded_samples = num_samples;
		sample_rate = 48000;
		bytes_per_sample = sizeof(Sample);
		float_samples = std::is_floating_point_v<Sample>;
	}

	bool NeedsCache() const override { return true; }
};

std::unique_ptr<agi::AudioProvider> Sequence() {
	return std::make_unique<TimelineSamples<int16_t>>(std::vector<int16_t>{100, 200, 300, 400});
}

#if AEGISUB_MATROSKA_PARSING
std::string UnsignedBytes(uint64_t value) {
	std::string bytes(1, static_cast<char>(value & 0xFF));
	while ((value >>= 8) != 0)
		bytes.insert(bytes.begin(), static_cast<char>(value & 0xFF));
	return bytes;
}

std::string Element(uint32_t id, std::string const& payload) {
	if (payload.size() >= 16383)
		throw std::runtime_error("test EBML element is too large");
	auto result = UnsignedBytes(id);
	if (payload.size() < 127)
		result += static_cast<char>(0x80 | payload.size());
	else {
		result += static_cast<char>(0x40 | (payload.size() >> 8));
		result += static_cast<char>(payload.size() & 0xFF);
	}
	return result + payload;
}

std::string UnsignedElement(uint32_t id, uint64_t value) {
	return Element(id, UnsignedBytes(value));
}

std::string Track(uint8_t number, bool audio, uint16_t pre_skip = 312, uint64_t codec_delay = 6500000,
				  std::string const& extra_fields = {}) {
	auto fields = UnsignedElement(0xD7, number) + UnsignedElement(0x73C5, number) + UnsignedElement(0x83, audio ? 2 : 1) + Element(0x86, audio ? "A_OPUS" : "V_VP9");
	if (audio) {
		std::string header = "OpusHead";
		header += static_cast<char>(1);
		header += static_cast<char>(1);
		header += static_cast<char>(pre_skip & 0xFF);
		header += static_cast<char>(pre_skip >> 8);
		header.append(7, '\0');
		fields += Element(0x63A2, header) + UnsignedElement(0x56AA, codec_delay) + UnsignedElement(0x56BB, 80000000);
	}
	return Element(0xAE, fields + extra_fields);
}

std::string Block(uint8_t track, int16_t timestamp, bool group = false) {
	std::string bytes;
	bytes += static_cast<char>(0x80 | track);
	bytes += static_cast<char>(static_cast<uint16_t>(timestamp) >> 8);
	bytes += static_cast<char>(static_cast<uint16_t>(timestamp) & 0xFF);
	bytes += static_cast<char>(0x80);
	bytes += '\0';
	return group ? Element(0xA0, Element(0xA1, bytes)) : Element(0xA3, bytes);
}

std::string Cluster(uint64_t timestamp, std::string const& blocks, bool unknown_size = false) {
	auto payload = UnsignedElement(0xE7, timestamp) + blocks;
	return unknown_size ? UnsignedBytes(0x1F43B675) + std::string(1, static_cast<char>(0xFF)) + payload
						: Element(0x1F43B675, payload);
}

std::string Matroska(std::string const& tracks, std::string const& clusters) {
	auto header = UnsignedElement(0x4286, 1) + UnsignedElement(0x42F7, 1) + UnsignedElement(0x42F2, 4) + UnsignedElement(0x42F3, 8) + Element(0x4282, "matroska") + UnsignedElement(0x4287, 4) + UnsignedElement(0x4285, 2);
	return Element(0x1A45DFA3, header) + Element(0x18538067, Element(0x1549A966, UnsignedElement(0x2AD7B1, 1000000)) + Element(0x1654AE6B, tracks) + clusters);
}

class TimelineContainer final {
	public:
	agi::fs::path path = agi::fs::UniquePath(std::filesystem::temp_directory_path() / "aegisub-audio-timeline-%%%%%%%%.bin");

	explicit TimelineContainer(std::string const& data) {
		std::ofstream output(path, std::ios::binary);
		output.write(data.data(), static_cast<std::streamsize>(data.size()));
		output.close();
		if (!output)
			throw std::runtime_error("failed to write timeline test container");
	}

	~TimelineContainer() {
		std::error_code error;
		std::filesystem::remove(path, error);
	}
};
#endif
}

TEST(audio_timeline, retains_opus_container_precision_instead_of_demuxed_milliseconds) {
	MkvAudioTimeline timeline{.first_audio_ns = 234000000, .first_video_ns = 0, .codec_delay_ns = 6500000, .pre_skip = 312};
	EXPECT_EQ(11232, GetMatroskaAudioOffset(timeline, 48000));
	EXPECT_EQ(5616, GetMatroskaAudioOffset(timeline, 24000));
	timeline.seek_pre_roll_ns = 80000000;
	EXPECT_EQ(11232, GetMatroskaAudioOffset(timeline, 48000));
	timeline.seek_pre_roll_ns = 160000000;
	EXPECT_EQ(11232, GetMatroskaAudioOffset(timeline, 48000));
	timeline.first_video_ns = 500000000;
	EXPECT_EQ(-12768, GetMatroskaAudioOffset(timeline, 48000));
}

TEST(audio_timeline, accounts_for_codec_delay_and_preskip_independently) {
	EXPECT_EQ(11496, GetMatroskaAudioOffset(MkvAudioTimeline{.first_audio_ns = 234000000, .first_video_ns = 0, .codec_delay_ns = 1000000, .pre_skip = 312}, 48000));
	EXPECT_EQ(6432, GetMatroskaAudioOffset(MkvAudioTimeline{.first_audio_ns = 234000000, .first_video_ns = 100000000, .codec_delay_ns = 2500000, .pre_skip = 120}, 48000));
	EXPECT_EQ(10319, GetMatroskaAudioOffset(MkvAudioTimeline{.first_audio_ns = 234000000, .first_video_ns = 0, .codec_delay_ns = 6500000, .pre_skip = 312}, 44100));
}

TEST(audio_timeline, positive_offset_pads_start_and_preserves_last_sample) {
	AudioTimelineProvider provider(Sequence(), 2);
	EXPECT_EQ(6, provider.GetNumSamples());
	EXPECT_EQ(6, provider.GetDecodedSamples());
	EXPECT_EQ(48000, provider.GetSampleRate());
	EXPECT_TRUE(provider.NeedsCache());
	std::array<int16_t, 8> actual{};
	provider.GetAudioChecked(actual.data(), -1, actual.size());
	EXPECT_EQ((std::array<int16_t, 8>{0, 0, 0, 100, 200, 300, 400, 0}), actual);
	std::array<int16_t, 3> tail{};
	provider.GetAudioChecked(tail.data(), 5, tail.size());
	EXPECT_EQ((std::array<int16_t, 3>{400, 0, 0}), tail);
}

TEST(audio_timeline, negative_offset_trims_decoded_prefix_without_changing_eof) {
	AudioTimelineProvider provider(Sequence(), -2);
	EXPECT_EQ(2, provider.GetNumSamples());
	EXPECT_EQ(2, provider.GetDecodedSamples());
	std::array<int16_t, 4> actual{};
	provider.GetInt16MonoAudioChecked(actual.data(), -1, actual.size());
	EXPECT_EQ((std::array<int16_t, 4>{0, 300, 400, 0}), actual);
}

TEST(audio_timeline, rejects_empty_or_overflowing_shifted_timeline) {
	EXPECT_THROW(AudioTimelineProvider(Sequence(), -4), agi::AudioProviderError);
	EXPECT_THROW(AudioTimelineProvider(Sequence(), -5), agi::AudioProviderError);
	EXPECT_THROW(AudioTimelineProvider(Sequence(), std::numeric_limits<int64_t>::max()), agi::AudioProviderError);
}

TEST(audio_timeline, preserves_unsigned_silence_and_stereo_mono_conversion) {
	auto source = std::make_unique<TimelineSamples<uint8_t>>(std::vector<uint8_t>{0, 255, 128, 255}, 2);
	AudioTimelineProvider provider(std::move(source), 1);
	EXPECT_EQ(2, provider.GetChannels());
	EXPECT_EQ(1, provider.GetBytesPerSample());
	EXPECT_FALSE(provider.AreSamplesFloat());
	std::array<uint8_t, 8> raw{};
	provider.GetAudioChecked(raw.data(), 0, 4);
	EXPECT_EQ((std::array<uint8_t, 8>{128, 128, 0, 255, 128, 255, 128, 128}), raw);
	std::array<int16_t, 4> mono{};
	provider.GetInt16MonoAudioChecked(mono.data(), 0, mono.size());
	EXPECT_EQ((std::array<int16_t, 4>{0, -128, 16256, 0}), mono);
}

TEST(audio_timeline, preserves_float_format_and_applies_offset_before_mono_conversion) {
	auto source = std::make_unique<TimelineSamples<float>>(std::vector<float>{-0.5f, 0.25f, 0.25f, 0.5f}, 2);
	AudioTimelineProvider provider(std::move(source), 1);
	EXPECT_TRUE(provider.AreSamplesFloat());
	EXPECT_EQ(4, provider.GetBytesPerSample());
	std::array<float, 8> raw{};
	provider.GetAudioChecked(raw.data(), 0, 4);
	EXPECT_EQ((std::array<float, 8>{0.f, 0.f, -0.5f, 0.25f, 0.25f, 0.5f, 0.f, 0.f}), raw);
	std::array<int16_t, 4> mono{};
	provider.GetInt16MonoAudioChecked(mono.data(), 0, mono.size());
	EXPECT_EQ((std::array<int16_t, 4>{0, -4096, 12288, 0}), mono);
}

TEST(audio_timeline, checked_reads_propagate_source_failure_without_affecting_padding) {
	auto source = std::make_unique<TimelineSamples<int16_t>>(std::vector<int16_t>{100, 200});
	source->fail_reads = true;
	AudioTimelineProvider provider(std::move(source), 2);
	std::array<int16_t, 2> samples{7, 7};
	EXPECT_NO_THROW(provider.GetAudioChecked(samples.data(), 0, samples.size()));
	EXPECT_EQ((std::array<int16_t, 2>{0, 0}), samples);
	EXPECT_THROW(provider.GetAudioChecked(samples.data(), 2, samples.size()), agi::AudioDecodeError);
	EXPECT_THROW(provider.GetInt16MonoAudioChecked(samples.data(), 2, samples.size()), agi::AudioDecodeError);
	EXPECT_NO_THROW(provider.GetAudio(samples.data(), 2, samples.size()));
	EXPECT_EQ((std::array<int16_t, 2>{0, 0}), samples);
}

TEST(audio_timeline, absent_container_timeline_leaves_provider_unwrapped) {
	auto source = Sequence();
	auto *original = source.get();
	auto provider = ApplyMatroskaAudioTimeline(std::move(source), std::nullopt);
	EXPECT_EQ(original, provider.get());
	EXPECT_EQ(4, provider->GetNumSamples());
}

#if AEGISUB_MATROSKA_PARSING
TEST(audio_timeline, scans_selected_audio_ordinal_and_raw_simpleblock_timestamp) {
	TimelineContainer file(Matroska(Track(7, false) + Track(19, true) + Track(41, true, 120, 2500000),
									Cluster(0, Block(7, 0) + Block(19, 234) + Block(41, 500))));
	auto first = MatroskaWrapper::GetOpusAudioTimeline(file.path, 0);
	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(234000000, first->first_audio_ns);
	EXPECT_EQ(0, first->first_video_ns);
	EXPECT_EQ(6500000, first->codec_delay_ns);
	EXPECT_EQ(312, first->pre_skip);
	EXPECT_EQ(80000000, first->seek_pre_roll_ns);
	EXPECT_EQ(11232, GetMatroskaAudioOffset(*first, 48000));
	auto second = MatroskaWrapper::GetOpusAudioTimeline(file.path, 1);
	ASSERT_TRUE(second.has_value());
	EXPECT_EQ(500000000, second->first_audio_ns);
	EXPECT_EQ(2500000, second->codec_delay_ns);
	EXPECT_EQ(120, second->pre_skip);
	EXPECT_EQ(80000000, second->seek_pre_roll_ns);
	EXPECT_FALSE(MatroskaWrapper::GetOpusAudioTimeline(file.path, 2).has_value());
	EXPECT_THROW(MatroskaWrapper::GetOpusAudioTimeline(file.path, 1, 1), MatroskaException);
}

TEST(audio_timeline, scans_negative_blockgroup_timestamp_and_earliest_video_presentation) {
	TimelineContainer file(Matroska(Track(7, false) + Track(19, true),
									Cluster(100, Block(7, 30) + Block(19, -20, true) + Block(7, 0))));
	auto timeline = MatroskaWrapper::GetOpusAudioTimeline(file.path, 0);
	ASSERT_TRUE(timeline.has_value());
	EXPECT_EQ(80000000, timeline->first_audio_ns);
	EXPECT_EQ(100000000, timeline->first_video_ns);
	EXPECT_EQ(-960, GetMatroskaAudioOffset(*timeline, 48000));
}

TEST(audio_timeline, traverses_unknown_size_cluster_to_later_audio_origin) {
	TimelineContainer file(Matroska(Track(7, false) + Track(19, true),
									Cluster(0, Block(7, 0), true) + Cluster(200, Block(19, 34))));
	auto timeline = MatroskaWrapper::GetOpusAudioTimeline(file.path, 0);
	ASSERT_TRUE(timeline.has_value());
	EXPECT_EQ(234000000, timeline->first_audio_ns);
	EXPECT_EQ(0, timeline->first_video_ns);
	EXPECT_EQ(11232, GetMatroskaAudioOffset(*timeline, 48000));
}

TEST(audio_timeline, leaves_audio_only_and_nonmatroska_sources_unchanged) {
	TimelineContainer audio_only(Matroska(Track(19, true), Cluster(0, Block(19, 234))));
	EXPECT_FALSE(MatroskaWrapper::GetOpusAudioTimeline(audio_only.path, 0).has_value());
	TimelineContainer other("OggS test audio");
	EXPECT_FALSE(MatroskaWrapper::GetOpusAudioTimeline(other.path, 0).has_value());
}

TEST(audio_timeline, leaves_nonopus_audio_with_video_on_existing_provider_policy) {
	auto aac = Element(0xAE, UnsignedElement(0xD7, 19) + UnsignedElement(0x83, 2) + Element(0x86, "A_AAC"));
	TimelineContainer file(Matroska(Track(7, false) + aac, Cluster(0, Block(7, 0) + Block(19, 234))));
	EXPECT_FALSE(MatroskaWrapper::GetOpusAudioTimeline(file.path, 0).has_value());
}

TEST(audio_timeline, applies_track_timestamp_scale_to_block_offset_not_cluster_origin) {
	// A four-byte big-endian IEEE float encoding TrackTimestampScale = 0.5.
	auto const half_scale = Element(0x23314F, std::string("\x3F\0\0\0", 4));
	TimelineContainer file(Matroska(Track(7, false) + Track(19, true, 312, 6500000, half_scale),
									Cluster(100, Block(7, 0) + Block(19, -20))));
	auto timeline = MatroskaWrapper::GetOpusAudioTimeline(file.path, 0);
	ASSERT_TRUE(timeline.has_value());
	EXPECT_EQ(90000000, timeline->first_audio_ns);
	EXPECT_EQ(100000000, timeline->first_video_ns);
	EXPECT_EQ(-480, GetMatroskaAudioOffset(*timeline, 48000));
}

TEST(audio_timeline, rejects_malformed_selected_opus_identification_header) {
	auto malformed = Element(0xAE, UnsignedElement(0xD7, 19) + UnsignedElement(0x83, 2) + Element(0x86, "A_OPUS") + Element(0x63A2, "OpusHead"));
	TimelineContainer file(Matroska(Track(7, false) + malformed, Cluster(0, Block(7, 0) + Block(19, 234))));
	EXPECT_THROW(MatroskaWrapper::GetOpusAudioTimeline(file.path, 0), MatroskaException);
}

TEST(audio_timeline, rejects_selected_opus_without_a_packet_origin) {
	TimelineContainer file(Matroska(Track(7, false) + Track(19, true), Cluster(0, Block(7, 0))));
	EXPECT_THROW(MatroskaWrapper::GetOpusAudioTimeline(file.path, 0), MatroskaException);
}
#endif
