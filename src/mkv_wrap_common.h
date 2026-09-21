// Copyright (c) 2026, Aegisub Project
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

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class MkvTextSubtitleCodec {
	Unsupported,
	Ass,
	Ssa,
	Utf8,
};

enum class MkvBitmapSubtitleCodec {
	Unsupported,
	HdmvPgs,
	VobSub,
};

enum class MkvTrackType {
	Other,
	Video,
	Audio,
	Subtitle,
};

struct MkvTextSubtitleLine {
	int sort_key = 0;
	std::string line;
};

inline constexpr uint64_t kMkvContentEncodingScopeBlock = 0x1;
inline constexpr uint64_t kMkvContentEncodingScopePrivate = 0x2;
inline constexpr uint64_t kMkvContentEncodingScopeNext = 0x4;

enum class MkvContentEncodingAlgorithm {
	Zlib,
	HeaderStripping,
	Unsupported,
};

enum class MkvContentEncodingTarget {
	Block,
	Private,
};

struct MkvContentEncoding {
	uint64_t order = 0;
	uint64_t scope = kMkvContentEncodingScopeBlock;
	MkvContentEncodingAlgorithm algorithm = MkvContentEncodingAlgorithm::Zlib;
	std::string settings;
};

inline constexpr uint64_t kDefaultSegmentTimecodeScale = 1000000;

struct MkvTrackInfo {
	int global_ordinal = -1;
	int type_ordinal = -1;
	MkvTrackType type = MkvTrackType::Other;
	uint64_t track_number = 0;
	uint64_t default_duration = 0;
	uint64_t codec_delay_ns = 0;
	uint64_t seek_pre_roll_ns = 0;
	double timecode_scale = 1.0;
	std::string codec_id;
	std::string language = "eng";
	std::string language_ietf;
	std::string name;
	std::string codec_private;
	std::vector<MkvContentEncoding> content_encodings;
	MkvTextSubtitleCodec subtitle_codec = MkvTextSubtitleCodec::Unsupported;
	MkvBitmapSubtitleCodec bitmap_subtitle_codec = MkvBitmapSubtitleCodec::Unsupported;
	std::optional<int> audio_channels;
	std::string unsupported_content_encoding_reason;
};

struct MkvTrackScanResult {
	uint64_t segment_timecode_scale = kDefaultSegmentTimecodeScale;
	std::vector<MkvTrackInfo> tracks;
};

struct MkvAudioTimeline {
	int64_t first_audio_ns = 0;
	int64_t first_video_ns = 0;
	uint64_t codec_delay_ns = 0;
	uint16_t pre_skip = 0;
	uint64_t seek_pre_roll_ns = 0;
};

struct MkvSubtitleAvailability {
	bool text = false;
	bool bitmap = false;
	bool hdmv_pgs = false;
	bool vobsub = false;
};

MkvTrackType ClassifyMkvTrackType(uint64_t track_type);
MkvTextSubtitleCodec ClassifyMkvTextSubtitleCodec(std::string_view codec_id);
MkvBitmapSubtitleCodec ClassifyMkvBitmapSubtitleCodec(std::string_view codec_id);
std::string_view GetSecondarySubtitleCodecId(MkvBitmapSubtitleCodec codec);
bool IsSupportedMkvTextSubtitleCodec(std::string_view codec_id);
bool IsSupportedMkvBitmapSubtitleCodec(std::string_view codec_id);
bool IsImportableMkvSubtitleTrack(MkvTrackInfo const& track);
bool IsDecodableMkvBitmapSubtitleTrack(MkvTrackInfo const& track);
std::string GetPreferredMkvTrackLanguage(MkvTrackInfo const& track);
std::string DescribeMkvTrack(MkvTrackInfo const& track);
std::string FormatMkvAudioChannelCount(std::optional<int> channels);
std::vector<std::string> SplitMkvCodecPrivateLines(std::string_view codec_private);
std::optional<MkvTextSubtitleLine> ParseMkvTextSubtitlePacket(MkvTextSubtitleCodec codec, std::string_view packet, int start_ms, int end_ms, int fallback_sort_key);
std::optional<std::string> DecodeMkvContentEncodedData(std::string_view data, std::vector<MkvContentEncoding> const& encodings, MkvContentEncodingTarget target, std::string *error = nullptr);
