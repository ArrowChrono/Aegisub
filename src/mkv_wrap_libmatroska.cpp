#include "mkv_wrap.h"

#include "ass_file.h"
#include "ass_file_app.h"
#include "ass_parser.h"
#include "mkv_wrap_common.h"
#include "options.h"
#include "track_choice.h"
#include "translation_service.h"
#include "transient_font_set.h"
#include "ui_services.h"

#include <libaegisub/exception.h>
#include <libaegisub/format.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>
#include <libaegisub/string_utils.h>

#include <ebml/EbmlHead.h>
#include <ebml/EbmlStream.h>
#include <ebml/IOCallback.h>

#include <matroska/KaxAttached.h>
#include <matroska/KaxAttachments.h>
#include <matroska/KaxBlock.h>
#include <matroska/KaxCluster.h>
#include <matroska/KaxSemantic.h>
#include <matroska/KaxSegment.h>
#include <matroska/KaxTracks.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <ios>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {
using libebml::EbmlElement;
using libebml::EbmlStream;

char constexpr kMkvLogSection[] = "subtitle/mkv";
std::atomic<uint64_t> next_transient_font_generation{1};

void LogMkvParserBackendOnce() {
	static std::once_flag once;
	std::call_once(once, [] {
		LOG_I(kMkvLogSection) << "Using MKV parser backend: libmatroska/libebml";
	});
}

struct Cursor {
	std::unique_ptr<EbmlElement> element;
	int upper = 0;
};

struct RawBlockPayload {
	uint64_t start_ns = 0;
	std::vector<std::string> packets;
};

Cursor next_child(EbmlStream &stream, libebml::EbmlSemanticContext const& context, bool allow_dummy = true) {
	int upper = 0;
	return { std::unique_ptr<EbmlElement>(stream.FindNextElement(context, upper, std::numeric_limits<uint64_t>::max(), allow_dummy)), upper };
}

Cursor continue_after_nested(EbmlStream &stream, libebml::EbmlSemanticContext const& parent_context, Cursor cursor) {
	if (cursor.upper > 0)
		--cursor.upper;
	if (cursor.upper < 0)
		cursor.upper = 0;
	if (!cursor.element && cursor.upper == 0)
		return next_child(stream, parent_context);
	return cursor;
}

class PathIOCallback final : public libebml::IOCallback {
	FILE *file = nullptr;
	uint64_t current_position = 0;

#ifdef _WIN32
	static int seek_file(FILE *file, int64_t offset, libebml::seek_mode mode) {
		return _fseeki64(file, offset, mode);
	}

	static int64_t tell_file(FILE *file) {
		return _ftelli64(file);
	}
#else
	static int seek_file(FILE *file, int64_t offset, libebml::seek_mode mode) {
		return fseeko(file, static_cast<off_t>(offset), mode);
	}

	static int64_t tell_file(FILE *file) {
		return ftello(file);
	}
#endif

public:
	explicit PathIOCallback(agi::fs::path const& filename) {
		auto const *mode_text = "rb";
#ifdef _WIN32
		file = _wfopen(filename.c_str(), L"rb");
#else
		auto const filename_text = agi::fs::PathToString(filename);
		file = fopen(filename_text.c_str(), mode_text);
#endif
		if (!file) {
			std::stringstream msg;
			msg << "Can't open MKV file \"" << agi::fs::PathToString(filename) << "\" in mode \"" << mode_text << "\"";
			throw std::ios_base::failure(msg.str(), std::error_code(errno, std::system_category()));
		}
	}

	~PathIOCallback() noexcept override {
		if (file && fclose(file) == 0)
			file = nullptr;
	}

	uint32 read(void *buffer, size_t size) override {
		auto const result = fread(buffer, 1, size, file);
		current_position += result;
		return static_cast<uint32>(result);
	}

	void setFilePointer(int64 offset, libebml::seek_mode mode = libebml::seek_beginning) override {
		if (seek_file(file, offset, mode) != 0) {
			std::ostringstream msg;
			msg << "Failed to seek MKV file handle to offset " << offset << " in mode " << mode;
			throw std::ios_base::failure(msg.str(), std::error_code(errno, std::system_category()));
		}

		auto const position = tell_file(file);
		if (position < 0)
			throw std::ios_base::failure("Failed to query MKV file position.", std::error_code(errno, std::system_category()));
		current_position = static_cast<uint64_t>(position);
	}

	size_t write(void const *buffer, size_t size) override {
		auto const result = fwrite(buffer, 1, size, file);
		current_position += result;
		return result;
	}

	uint64 getFilePointer() override {
		return current_position;
	}

	void close() override {
		if (!file)
			return;
		if (fclose(file) != 0)
			throw std::ios_base::failure("Can't close MKV file handle.", std::error_code(errno, std::system_category()));
		file = nullptr;
	}
};

void skip_ebml_head(EbmlStream &stream) {
	std::unique_ptr<EbmlElement> head(stream.FindNextID(EBML_INFO(libebml::EbmlHead), std::numeric_limits<uint64_t>::max()));
	if (head)
		head->SkipData(stream, EBML_CLASS_CONTEXT(libebml::EbmlHead));
}

std::unique_ptr<EbmlElement> open_segment(EbmlStream &stream) {
	skip_ebml_head(stream);
	return std::unique_ptr<EbmlElement>(stream.FindNextID(EBML_INFO(libmatroska::KaxSegment), std::numeric_limits<uint64_t>::max()));
}

uint64_t scale_track_time_ns(uint64_t value, double track_scale) {
	long double const scaled = static_cast<long double>(value) * static_cast<long double>(track_scale);
	if (scaled <= 0)
		return 0;
	if (scaled >= static_cast<long double>(std::numeric_limits<uint64_t>::max()))
		return std::numeric_limits<uint64_t>::max();
	return static_cast<uint64_t>(scaled + 0.5L);
}

uint64_t scale_block_duration_ns(uint64_t value, uint64_t segment_scale, double track_scale) {
	long double const scaled = static_cast<long double>(value) * static_cast<long double>(segment_scale) * static_cast<long double>(track_scale);
	if (scaled <= 0)
		return 0;
	if (scaled >= static_cast<long double>(std::numeric_limits<uint64_t>::max()))
		return std::numeric_limits<uint64_t>::max();
	return static_cast<uint64_t>(scaled + 0.5L);
}

int ns_to_ass_ms(uint64_t value) {
	auto const limit = static_cast<uint64_t>(std::numeric_limits<int>::max()) * 1000000ULL;
	if (value >= limit)
		return std::numeric_limits<int>::max();
	return static_cast<int>(value / 1000000ULL);
}

std::string to_lower_copy(std::string value) {
	agi::util::strings::to_lower_inplace(value);
	return value;
}

bool looks_like_font_mime_type(std::string const& mime_type) {
	auto const lowered = to_lower_copy(mime_type);
	return agi::util::strings::starts_with(lowered, "font/")
		|| agi::util::strings::contains(lowered, "truetype")
		|| agi::util::strings::contains(lowered, "opentype")
		|| agi::util::strings::contains(lowered, "sfnt");
}

bool is_supported_font_attachment(std::string const& file_name, std::string const& mime_type) {
	auto const ext = to_lower_copy(agi::fs::PathToString(agi::fs::PathFromString(file_name).extension()));
	if (ext == ".ttf" || ext == ".ttc" || ext == ".otf" || ext == ".otc" || ext == ".pfb")
		return true;
	return !mime_type.empty() && looks_like_font_mime_type(mime_type);
}

std::shared_ptr<TransientFontSet> ensure_transient_font_set(std::shared_ptr<TransientFontSet>& fonts) {
	if (!fonts) {
		fonts = std::make_shared<TransientFontSet>();
		fonts->generation = next_transient_font_generation.fetch_add(1, std::memory_order_relaxed);
	}
	return fonts;
}

void throw_if_cancelled(agi::ProgressSink *ps) {
	if (ps && ps->IsCancelled())
		throw agi::UserCancelException("Cancelled by user");
}

char const* describe_content_encoding_algorithm(MkvContentEncodingAlgorithm algorithm) {
	switch (algorithm) {
	case MkvContentEncodingAlgorithm::Zlib:
		return "zlib";
	case MkvContentEncodingAlgorithm::HeaderStripping:
		return "header-stripping";
	case MkvContentEncodingAlgorithm::Unsupported:
		break;
	}
	return "unsupported";
}

std::string describe_content_encoding_target(uint64_t scope) {
	std::vector<std::string> parts;
	if (scope & kMkvContentEncodingScopeBlock)
		parts.emplace_back("block");
	if (scope & kMkvContentEncodingScopePrivate)
		parts.emplace_back("private");
	if (scope & kMkvContentEncodingScopeNext)
		parts.emplace_back("next");
	if (parts.empty())
		return "none";

	std::string joined = parts.front();
	for (size_t i = 1; i < parts.size(); ++i) {
		joined += "+";
		joined += parts[i];
	}
	return joined;
}

std::string describe_content_encodings(std::vector<MkvContentEncoding> const& encodings) {
	if (encodings.empty())
		return "none";

	std::string joined;
	for (size_t i = 0; i < encodings.size(); ++i) {
		if (i)
			joined += ", ";
		joined += agi::format("#%u %s[%s]",
			static_cast<unsigned>(encodings[i].order),
			describe_content_encoding_algorithm(encodings[i].algorithm),
			describe_content_encoding_target(encodings[i].scope));
	}
	return joined;
}

void append_block_lines(MkvTrackInfo const& track, RawBlockPayload const& raw_block, bool have_duration, uint64_t duration_units, std::vector<std::pair<int, std::string>> &lines, int &fallback_sort_key, uint64_t segment_timecode_scale) {
	if (raw_block.packets.empty())
		return;

	auto start_ns = raw_block.start_ns;
	auto const frame_count = raw_block.packets.size();
	auto const scaled_duration = have_duration ? scale_block_duration_ns(duration_units, segment_timecode_scale, track.timecode_scale) : 0;

	for (size_t i = 0; i < frame_count; ++i) {
		uint64_t end_ns = start_ns;
		if (frame_count == 1) {
			if (have_duration)
				end_ns = start_ns + scaled_duration;
			else if (track.default_duration)
				end_ns = start_ns + track.default_duration;
		}
		else if (have_duration) {
			if (i + 1 < frame_count) {
				end_ns = start_ns + track.default_duration;
			}
			else {
				end_ns = raw_block.start_ns + scaled_duration;
			}
		}
		else if (track.default_duration) {
			end_ns = start_ns + track.default_duration;
		}

		auto line = ParseMkvTextSubtitlePacket(track.subtitle_codec, raw_block.packets[i], ns_to_ass_ms(start_ns), ns_to_ass_ms(end_ns), fallback_sort_key++);
		if (line)
			lines.emplace_back(line->sort_key, std::move(line->line));

		if (frame_count > 1 && track.default_duration)
			start_ns += track.default_duration;
	}
}

void append_block_packets(MkvTrackInfo const& track, RawBlockPayload const& raw_block, bool have_duration, uint64_t duration_units, uint64_t segment_timecode_scale, std::vector<SecondarySubtitlePacket>& packets) {
	if (raw_block.packets.empty())
		return;

	auto pts_ns = raw_block.start_ns;
	auto const frame_count = raw_block.packets.size();
	auto const block_duration = have_duration
		? scale_block_duration_ns(duration_units, segment_timecode_scale, track.timecode_scale)
		: 0;
	uint64_t per_frame_duration = 0;
	if (block_duration && frame_count == 1)
		per_frame_duration = block_duration;
	else if (track.default_duration)
		per_frame_duration = track.default_duration;
	else if (block_duration && frame_count > 0)
		per_frame_duration = block_duration / frame_count;

	for (size_t index = 0; index < frame_count; ++index) {
		SecondarySubtitlePacket packet;
		packet.pts_ns = pts_ns > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
			? std::numeric_limits<int64_t>::max()
			: static_cast<int64_t>(pts_ns);
		packet.duration_ns = per_frame_duration > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
			? std::numeric_limits<int64_t>::max()
			: static_cast<int64_t>(per_frame_duration);
		packet.payload = raw_block.packets[index];
		packets.emplace_back(std::move(packet));
		pts_ns = per_frame_duration > std::numeric_limits<uint64_t>::max() - pts_ns
			? std::numeric_limits<uint64_t>::max()
			: pts_ns + per_frame_duration;
	}
}

std::optional<RawBlockPayload> read_selected_block(EbmlStream &stream, libmatroska::KaxInternalBlock &block, libmatroska::KaxCluster &cluster, MkvTrackInfo const& track) {
	auto const data_start = block.GetElementPosition() + block.HeadSize();

	block.SetParent(cluster);
	block.ReadInternalHead(stream.I_O());

	auto const selected = block.TrackNum() == track.track_number;
	stream.I_O().setFilePointer(data_start, libebml::seek_beginning);

	if (!selected) {
		block.SkipData(stream, EBML_CONTEXT(&block));
		return std::nullopt;
	}

	block.ReadData(stream.I_O(), libebml::SCOPE_ALL_DATA);
	block.SetParent(cluster);

	RawBlockPayload payload;
	payload.start_ns = scale_track_time_ns(block.GlobalTimecode(), track.timecode_scale);
	payload.packets.reserve(block.NumberFrames());
	for (unsigned int i = 0; i < block.NumberFrames(); ++i) {
		auto &buffer = block.GetBuffer(i);
		std::string decoded_error;
		auto decoded = DecodeMkvContentEncodedData(
			std::string_view(reinterpret_cast<char const*>(buffer.Buffer()), buffer.Size()),
			track.content_encodings,
			MkvContentEncodingTarget::Block,
			&decoded_error);
		if (!decoded) {
			throw MatroskaException(agi::format(
				"Failed to decode MKV subtitle block for track %u: %s",
				static_cast<unsigned>(track.track_number),
				decoded_error));
		}
		payload.packets.emplace_back(std::move(*decoded));
	}
	return payload;
}

Cursor parse_content_compression(EbmlStream &stream, EbmlElement &compression_element, MkvContentEncoding &encoding, std::string &error) {
	auto const& compression_context = EBML_CONTEXT(&compression_element);
	auto cursor = next_child(stream, compression_context);

	while (cursor.element && cursor.upper <= 0) {
		auto &child = *cursor.element;

		if (EbmlId(child) == EBML_ID(libmatroska::KaxContentCompAlgo)) {
			auto &value = *static_cast<libmatroska::KaxContentCompAlgo*>(cursor.element.get());
			value.ReadData(stream.I_O());
			switch (value.GetValue()) {
			case libmatroska::MATROSKA_TRACK_ENCODING_COMP_ZLIB:
				encoding.algorithm = MkvContentEncodingAlgorithm::Zlib;
				break;
			case libmatroska::MATROSKA_TRACK_ENCODING_COMP_HEADERSTRIP:
				encoding.algorithm = MkvContentEncodingAlgorithm::HeaderStripping;
				break;
			default:
				error = agi::format("unsupported compression algorithm %u", static_cast<unsigned>(value.GetValue()));
				break;
			}
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxContentCompSettings)) {
			auto &value = *static_cast<libmatroska::KaxContentCompSettings*>(cursor.element.get());
			value.ReadData(stream.I_O());
			encoding.settings.assign(reinterpret_cast<char const*>(value.GetBuffer()), value.GetSize());
		}
		else {
			child.SkipData(stream, EBML_CONTEXT(&child));
		}

		cursor = next_child(stream, compression_context);
	}

	return cursor;
}

Cursor parse_content_encoding(EbmlStream &stream, EbmlElement &encoding_element, MkvContentEncoding &encoding, std::string &error) {
	auto const& encoding_context = EBML_CONTEXT(&encoding_element);
	auto cursor = next_child(stream, encoding_context);

	while (cursor.element && cursor.upper <= 0) {
		auto &child = *cursor.element;

		if (EbmlId(child) == EBML_ID(libmatroska::KaxContentEncodingOrder)) {
			auto &value = *static_cast<libmatroska::KaxContentEncodingOrder*>(cursor.element.get());
			value.ReadData(stream.I_O());
			encoding.order = value.GetValue();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxContentEncodingScope)) {
			auto &value = *static_cast<libmatroska::KaxContentEncodingScope*>(cursor.element.get());
			value.ReadData(stream.I_O());
			encoding.scope = value.GetValue();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxContentEncodingType)) {
			auto &value = *static_cast<libmatroska::KaxContentEncodingType*>(cursor.element.get());
			value.ReadData(stream.I_O());
			if (value.GetValue() != libmatroska::MATROSKA_CONTENTENCODINGTYPE_COMPRESSION)
				error = "encryption content encodings are not supported";
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxContentCompression)) {
			auto nested = parse_content_compression(stream, *cursor.element, encoding, error);
			cursor = continue_after_nested(stream, encoding_context, std::move(nested));
			continue;
		}
		else {
			child.SkipData(stream, EBML_CONTEXT(&child));
		}

		cursor = next_child(stream, encoding_context);
	}

	return cursor;
}

Cursor parse_content_encodings(EbmlStream &stream, EbmlElement &encodings_element, MkvTrackInfo &track) {
	auto const& encodings_context = EBML_CONTEXT(&encodings_element);
	auto cursor = next_child(stream, encodings_context);

	while (cursor.element && cursor.upper <= 0) {
		if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxContentEncoding)) {
			MkvContentEncoding encoding;
			std::string error;
			auto nested = parse_content_encoding(stream, *cursor.element, encoding, error);
			if (!error.empty() && track.unsupported_content_encoding_reason.empty())
				track.unsupported_content_encoding_reason = error;
			else if (track.unsupported_content_encoding_reason.empty())
				track.content_encodings.emplace_back(std::move(encoding));
			cursor = continue_after_nested(stream, encodings_context, std::move(nested));
		}
		else {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
			cursor = next_child(stream, encodings_context);
		}
	}

	std::stable_sort(track.content_encodings.begin(), track.content_encodings.end(), [](auto const& left, auto const& right) {
		return left.order > right.order;
	});

	return cursor;
}

Cursor parse_attached_file(EbmlStream &stream, EbmlElement &attached_element, std::shared_ptr<TransientFontSet>& fonts) {
	auto const& attached_context = EBML_CONTEXT(&attached_element);
	auto cursor = next_child(stream, attached_context);

	std::string file_name;
	std::string mime_type;
	std::vector<char> data;
	std::optional<bool> supported_font;

	auto update_supported_font = [&] {
		if (file_name.empty() && mime_type.empty())
			return;
		supported_font = is_supported_font_attachment(file_name, mime_type);
	};

	while (cursor.element && cursor.upper <= 0) {
		auto &child = *cursor.element;

		if (EbmlId(child) == EBML_ID(libmatroska::KaxFileName)) {
			auto &value = *static_cast<libmatroska::KaxFileName*>(cursor.element.get());
			value.ReadData(stream.I_O());
			file_name = value.GetValueUTF8();
			update_supported_font();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxMimeType)) {
			auto &value = *static_cast<libmatroska::KaxMimeType*>(cursor.element.get());
			value.ReadData(stream.I_O());
			mime_type = value.GetValue();
			update_supported_font();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxFileData)) {
			if (supported_font && !*supported_font) {
				child.SkipData(stream, EBML_CONTEXT(&child));
			}
			else {
				auto &value = *static_cast<libmatroska::KaxFileData*>(cursor.element.get());
				value.ReadData(stream.I_O());
				data.assign(reinterpret_cast<char const*>(value.GetBuffer()), reinterpret_cast<char const*>(value.GetBuffer()) + value.GetSize());
			}
		}
		else {
			child.SkipData(stream, EBML_CONTEXT(&child));
		}

		cursor = next_child(stream, attached_context);
	}

	auto const attachment_is_supported = supported_font.value_or(is_supported_font_attachment(file_name, mime_type));
	if (!file_name.empty() && !data.empty()) {
		if (attachment_is_supported) {
			auto font_set = ensure_transient_font_set(fonts);
			font_set->fonts.push_back({ file_name, mime_type, std::move(data) });
			LOG_I(kMkvLogSection) << "Collected MKV font attachment: " << file_name
				<< (mime_type.empty() ? "" : agi::format(" (%s)", mime_type));
		}
		else {
			LOG_D(kMkvLogSection) << "Ignoring non-font MKV attachment: " << file_name;
		}
	}
	else if (!file_name.empty() && !attachment_is_supported) {
		LOG_D(kMkvLogSection) << "Ignoring non-font MKV attachment: " << file_name;
	}

	return cursor;
}

Cursor parse_attachments(EbmlStream &stream, EbmlElement &attachments_element, std::shared_ptr<TransientFontSet>& fonts) {
	auto const& attachments_context = EBML_CONTEXT(&attachments_element);
	auto cursor = next_child(stream, attachments_context);

	while (cursor.element && cursor.upper <= 0) {
		if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxAttached)) {
			auto nested = parse_attached_file(stream, *cursor.element, fonts);
			cursor = continue_after_nested(stream, attachments_context, std::move(nested));
		}
		else {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
			cursor = next_child(stream, attachments_context);
		}
	}

	return cursor;
}

Cursor parse_track_audio(EbmlStream &stream, EbmlElement &audio_element, MkvTrackInfo &track) {
	auto const& audio_context = EBML_CONTEXT(&audio_element);
	auto cursor = next_child(stream, audio_context);

	while (cursor.element && cursor.upper <= 0) {
		auto &child = *cursor.element;

		if (EbmlId(child) == EBML_ID(libmatroska::KaxAudioChannels)) {
			auto &value = *static_cast<libmatroska::KaxAudioChannels*>(cursor.element.get());
			value.ReadData(stream.I_O());
			if (value.GetValue() <= static_cast<uint64_t>(std::numeric_limits<int>::max()))
				track.audio_channels = static_cast<int>(value.GetValue());
		}
		else {
			child.SkipData(stream, EBML_CONTEXT(&child));
		}

		cursor = next_child(stream, audio_context);
	}

	return cursor;
}

Cursor parse_track_entry(EbmlStream &stream, EbmlElement &entry_element, MkvTrackInfo &track) {
	auto const& entry_context = EBML_CONTEXT(&entry_element);
	auto cursor = next_child(stream, entry_context);

	while (cursor.element && cursor.upper <= 0) {
		auto &child = *cursor.element;

		if (EbmlId(child) == EBML_ID(libmatroska::KaxTrackNumber)) {
			auto &value = *static_cast<libmatroska::KaxTrackNumber*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.track_number = value.GetValue();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxTrackType)) {
			auto &value = *static_cast<libmatroska::KaxTrackType*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.type = ClassifyMkvTrackType(value.GetValue());
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxCodecID)) {
			auto &value = *static_cast<libmatroska::KaxCodecID*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.codec_id = value.GetValue();
			track.subtitle_codec = ClassifyMkvTextSubtitleCodec(track.codec_id);
			track.bitmap_subtitle_codec = ClassifyMkvBitmapSubtitleCodec(track.codec_id);
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxTrackLanguage)) {
			auto &value = *static_cast<libmatroska::KaxTrackLanguage*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.language = value.GetValue();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxLanguageIETF)) {
			auto &value = *static_cast<libmatroska::KaxLanguageIETF*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.language_ietf = value.GetValue();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxTrackName)) {
			auto &value = *static_cast<libmatroska::KaxTrackName*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.name = value.GetValueUTF8();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxTrackTimecodeScale)) {
			auto &value = *static_cast<libmatroska::KaxTrackTimecodeScale*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.timecode_scale = value.GetValue();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxTrackDefaultDuration)) {
			auto &value = *static_cast<libmatroska::KaxTrackDefaultDuration*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.default_duration = value.GetValue();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxCodecDelay)) {
			auto& value = *static_cast<libmatroska::KaxCodecDelay *>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.codec_delay_ns = value.GetValue();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxSeekPreRoll)) {
			auto& value = *static_cast<libmatroska::KaxSeekPreRoll *>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.seek_pre_roll_ns = value.GetValue();
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxCodecPrivate)) {
			auto &value = *static_cast<libmatroska::KaxCodecPrivate*>(cursor.element.get());
			value.ReadData(stream.I_O());
			track.codec_private.assign(reinterpret_cast<char const*>(value.GetBuffer()), value.GetSize());
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxContentEncodings)) {
			auto nested = parse_content_encodings(stream, child, track);
			cursor = continue_after_nested(stream, entry_context, std::move(nested));
			continue;
		}
		else if (EbmlId(child) == EBML_ID(libmatroska::KaxTrackAudio)) {
			auto nested = parse_track_audio(stream, child, track);
			cursor = continue_after_nested(stream, entry_context, std::move(nested));
			continue;
		}
		else {
			child.SkipData(stream, EBML_CONTEXT(&child));
		}

		cursor = next_child(stream, entry_context);
	}

	return cursor;
}

Cursor parse_tracks(EbmlStream &stream, EbmlElement &tracks_element, MkvTrackScanResult &result) {
	auto const& tracks_context = EBML_CONTEXT(&tracks_element);
	auto cursor = next_child(stream, tracks_context);
	int video_count = 0;
	int audio_count = 0;
	int subtitle_count = 0;
	int other_count = 0;

	while (cursor.element && cursor.upper <= 0) {
		if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxTrackEntry)) {
			MkvTrackInfo track;
			auto nested = parse_track_entry(stream, *cursor.element, track);
			track.global_ordinal = static_cast<int>(result.tracks.size());
			switch (track.type) {
			case MkvTrackType::Video:
				track.type_ordinal = video_count++;
				break;
			case MkvTrackType::Audio:
				track.type_ordinal = audio_count++;
				break;
			case MkvTrackType::Subtitle:
				track.type_ordinal = subtitle_count++;
				break;
			case MkvTrackType::Other:
				track.type_ordinal = other_count++;
				break;
			}
			result.tracks.emplace_back(std::move(track));

			cursor = continue_after_nested(stream, tracks_context, std::move(nested));
		}
		else {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
			cursor = next_child(stream, tracks_context);
		}
	}

	return cursor;
}

Cursor parse_info(EbmlStream &stream, EbmlElement &info_element, MkvTrackScanResult &result) {
	auto const& info_context = EBML_CONTEXT(&info_element);
	auto cursor = next_child(stream, info_context);

	while (cursor.element && cursor.upper <= 0) {
		if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxTimecodeScale)) {
			auto &value = *static_cast<libmatroska::KaxTimecodeScale*>(cursor.element.get());
			value.ReadData(stream.I_O());
			result.segment_timecode_scale = value.GetValue();
		}
		else {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
		}

		cursor = next_child(stream, info_context);
	}

	return cursor;
}

MkvTrackScanResult scan_tracks(agi::fs::path const& filename) {
	PathIOCallback input(filename);
	EbmlStream stream(input);
	auto segment = open_segment(stream);
	if (!segment)
		throw MatroskaException("File is not a Matroska file.");

	MkvTrackScanResult result;
	auto const& segment_context = EBML_CONTEXT(segment.get());
	auto cursor = next_child(stream, segment_context);

	while (cursor.element && cursor.upper <= 0) {
		if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxInfo)) {
			cursor = continue_after_nested(stream, segment_context, parse_info(stream, *cursor.element, result));
		}
		else if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxTracks)) {
			cursor = continue_after_nested(stream, segment_context, parse_tracks(stream, *cursor.element, result));
		}
		else if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxCluster)) {
			break;
		}
		else {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
			cursor = next_child(stream, segment_context);
		}
	}

	return result;
}

Cursor next_timeline_child(EbmlStream& stream, EbmlElement const& parent) {
	if (parent.IsFiniteSize() && stream.I_O().getFilePointer() >= parent.GetEndPosition())
		return {};
	return next_child(stream, EBML_CONTEXT(&parent));
}

std::pair<uint64_t, int16_t> read_timeline_block(EbmlStream& stream, EbmlElement& block) {
	std::array<unsigned char, 11> header{};
	if (!block.IsFiniteSize() || block.GetSize() < 4 || stream.I_O().read(header.data(), 1) != 1 || !header[0])
		throw MatroskaException("Invalid Matroska block header in Opus timeline.");
	unsigned width = 1;
	unsigned marker = 0x80;
	while (!(header[0] & marker)) {
		marker >>= 1;
		++width;
	}
	if (block.GetSize() < width + 3 || stream.I_O().read(header.data() + 1, width + 2) != width + 2)
		throw MatroskaException("Truncated Matroska block header in Opus timeline.");
	uint64_t track_number = header[0] & (marker - 1);
	for (unsigned i = 1; i < width; ++i)
		track_number = (track_number << 8) | header[i];
	int const timestamp = (header[width] << 8) | header[width + 1];
	block.SkipData(stream, EBML_CONTEXT(&block));
	return {track_number, static_cast<int16_t>(timestamp < 0x8000 ? timestamp : timestamp - 0x10000)};
}

int64_t timeline_timestamp_ns(uint64_t cluster_time, int16_t relative_time, uint64_t segment_scale, double track_scale) {
	if (!segment_scale || !std::isfinite(track_scale) || track_scale <= 0 || cluster_time > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / segment_scale)
		throw MatroskaException("Invalid or unrepresentable Matroska Opus timestamp scale.");
	auto const origin = static_cast<int64_t>(cluster_time * segment_scale);
	auto const relative = std::round(static_cast<long double>(relative_time) * segment_scale * track_scale);
	auto const limit = std::ldexp(1.0L, 63);
	if (!std::isfinite(relative) || relative >= limit || relative < -limit)
		throw MatroskaException("Matroska Opus block timestamp is out of range.");
	auto const offset = static_cast<int64_t>(relative);
	if (offset > 0 && origin > std::numeric_limits<int64_t>::max() - offset)
		throw MatroskaException("Matroska Opus block timestamp is out of range.");
	return origin + offset;
}

std::optional<MkvAudioTimeline> scan_opus_audio_timeline(agi::fs::path const& filename, int audio_ordinal, int audio_count) {
	PathIOCallback input(filename);
	std::array<unsigned char, 4> signature{};
	if (input.read(signature.data(), signature.size()) != signature.size() || signature != std::array<unsigned char, 4>{0x1a, 0x45, 0xdf, 0xa3})
		return std::nullopt;
	input.setFilePointer(0);
	EbmlStream stream(input);
	auto segment = open_segment(stream);
	if (!segment)
		return std::nullopt;

	auto const scan = scan_tracks(filename);
	MkvTrackInfo const *audio = nullptr;
	MkvTrackInfo const *video = nullptr;
	for (auto const& track : scan.tracks) {
		if (track.type == MkvTrackType::Audio && track.type_ordinal == audio_ordinal)
			audio = &track;
		if (track.type == MkvTrackType::Video && !video)
			video = &track;
	}
	if (!video)
		return std::nullopt;
	if (audio_count >= 0 && std::ranges::count_if(scan.tracks, [](auto const& track) { return track.type == MkvTrackType::Audio; }) != audio_count)
		throw MatroskaException("Cannot map decoder audio streams to Matroska tracks.");
	if (!audio || audio->codec_id != "A_OPUS")
		return std::nullopt;
	std::string error;
	auto const head = DecodeMkvContentEncodedData(audio->codec_private, audio->content_encodings, MkvContentEncodingTarget::Private, &error);
	if (!head)
		throw MatroskaException("Failed to decode Matroska Opus header: " + error);
	if (head->size() < 19 || !head->starts_with("OpusHead") || static_cast<unsigned char>((*head)[8]) > 15 || (*head)[9] == 0)
		throw MatroskaException("Invalid Matroska Opus identification header.");
	auto const channels = static_cast<unsigned char>((*head)[9]);
	auto const family = static_cast<unsigned char>((*head)[18]);
	if ((!family && channels > 2) || (family && head->size() < 21u + channels))
		throw MatroskaException("Invalid Matroska Opus channel mapping header.");
	MkvAudioTimeline result;
	result.codec_delay_ns = audio->codec_delay_ns;
	result.seek_pre_roll_ns = audio->seek_pre_roll_ns;
	result.pre_skip = static_cast<unsigned char>((*head)[10]) | (static_cast<unsigned char>((*head)[11]) << 8);
	std::optional<int64_t> first_audio;
	std::optional<int64_t> first_video;
	auto const& segment_context = EBML_CONTEXT(segment.get());
	auto cursor = next_timeline_child(stream, *segment);
	while (cursor.element && cursor.upper <= 0) {
		if (EbmlId(*cursor.element) != EBML_ID(libmatroska::KaxCluster)) {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
			cursor = next_timeline_child(stream, *segment);
			continue;
		}
		auto& cluster = *cursor.element;
		auto const& cluster_context = EBML_CONTEXT(&cluster);
		std::optional<uint64_t> cluster_time;
		std::optional<int16_t> audio_time;
		std::optional<int16_t> video_time;
		auto read_block = [&](EbmlElement& block) {
			auto const [track_number, timestamp] = read_timeline_block(stream, block);
			if (track_number == audio->track_number && !first_audio && !audio_time)
				audio_time = timestamp;
			if (track_number == video->track_number && !first_video)
				video_time = video_time ? std::min(*video_time, timestamp) : timestamp;
		};
		auto child = next_timeline_child(stream, cluster);
		while (child.element && child.upper <= 0) {
			if (EbmlId(*child.element) == EBML_ID(libmatroska::KaxClusterTimecode)) {
				auto& value = *static_cast<libmatroska::KaxClusterTimecode *>(child.element.get());
				value.ReadData(stream.I_O());
				cluster_time = value.GetValue();
			}
			else if (EbmlId(*child.element) == EBML_ID(libmatroska::KaxSimpleBlock))
				read_block(*child.element);
			else if (EbmlId(*child.element) == EBML_ID(libmatroska::KaxBlockGroup)) {
				auto& group = *child.element;
				auto nested = next_timeline_child(stream, group);
				while (nested.element && nested.upper <= 0) {
					if (EbmlId(*nested.element) == EBML_ID(libmatroska::KaxBlock))
						read_block(*nested.element);
					else
						nested.element->SkipData(stream, EBML_CONTEXT(nested.element.get()));
					nested = next_timeline_child(stream, group);
				}
				child = continue_after_nested(stream, cluster_context, std::move(nested));
				continue;
			}
			else
				child.element->SkipData(stream, EBML_CONTEXT(child.element.get()));
			child = next_timeline_child(stream, cluster);
		}
		if ((audio_time || video_time) && !cluster_time)
			throw MatroskaException("Matroska Opus timeline cluster has no timestamp.");
		if (audio_time)
			first_audio = timeline_timestamp_ns(*cluster_time, *audio_time, scan.segment_timecode_scale, audio->timecode_scale);
		// The first video cluster may store B frames after later presentation times.
		if (video_time)
			first_video = timeline_timestamp_ns(*cluster_time, *video_time, scan.segment_timecode_scale, video->timecode_scale);
		if (first_audio && first_video) {
			result.first_audio_ns = *first_audio;
			result.first_video_ns = *first_video;
			return result;
		}
		cursor = continue_after_nested(stream, segment_context, std::move(child));
	}
	throw MatroskaException("Matroska Opus timeline has no audio or video block origin.");
}

std::vector<MkvTrackInfo const*> collect_importable_subtitle_tracks(MkvTrackScanResult const& scan, bool log_tracks) {
	std::vector<MkvTrackInfo const*> tracks;
	tracks.reserve(scan.tracks.size());

	for (auto const& track : scan.tracks) {
		if (track.type != MkvTrackType::Subtitle)
			continue;

		if (!track.codec_id.empty() && track.subtitle_codec == MkvTextSubtitleCodec::Unsupported) {
			if (log_tracks)
				LOG_I(kMkvLogSection) << "Skipping MKV subtitle track " << track.track_number << " with unsupported codec " << track.codec_id;
		}
		else if (!track.unsupported_content_encoding_reason.empty()) {
			if (log_tracks) {
				LOG_I(kMkvLogSection) << "Skipping MKV subtitle track " << track.track_number << " (" << track.codec_id
					<< ") because content encodings are unsupported: " << track.unsupported_content_encoding_reason;
			}
		}
		else if (IsImportableMkvSubtitleTrack(track)) {
			if (log_tracks) {
				LOG_I(kMkvLogSection) << "Found importable MKV subtitle track " << track.track_number << " (" << track.codec_id
					<< "), content encodings: " << describe_content_encodings(track.content_encodings);
			}
			tracks.push_back(&track);
		}
	}

	return tracks;
}

std::vector<MkvTrackInfo const*> collect_bitmap_subtitle_tracks(MkvTrackScanResult const& scan, bool log_tracks) {
	std::vector<MkvTrackInfo const*> tracks;
	for (auto const& track : scan.tracks) {
		if (!IsDecodableMkvBitmapSubtitleTrack(track))
			continue;
		if (log_tracks)
			LOG_I(kMkvLogSection) << "Found packet-backed MKV subtitle track " << track.track_number << " (" << track.codec_id << ")";
		tracks.push_back(&track);
	}
	return tracks;
}

Cursor parse_block_group(EbmlStream &stream, EbmlElement &group_element, libmatroska::KaxCluster &cluster, MkvTrackInfo const& track, std::vector<std::pair<int, std::string>> &lines, int &fallback_sort_key, uint64_t segment_timecode_scale) {
	auto const& group_context = EBML_CONTEXT(&group_element);
	auto cursor = next_child(stream, group_context);

	std::optional<RawBlockPayload> raw_block;
	uint64_t duration_units = 0;
	bool have_duration = false;

	while (cursor.element && cursor.upper <= 0) {
		if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxBlockDuration)) {
			auto &duration = *static_cast<libmatroska::KaxBlockDuration*>(cursor.element.get());
			duration.ReadData(stream.I_O());
			duration_units = duration.GetValue();
			have_duration = true;
		}
		else if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxBlock)) {
			auto &block = *static_cast<libmatroska::KaxBlock*>(cursor.element.get());
			raw_block = read_selected_block(stream, block, cluster, track);
		}
		else {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
		}

		cursor = next_child(stream, group_context);
	}

	if (raw_block)
		append_block_lines(track, *raw_block, have_duration, duration_units, lines, fallback_sort_key, segment_timecode_scale);

	return cursor;
}

Cursor parse_packet_block_group(EbmlStream &stream, EbmlElement &group_element, libmatroska::KaxCluster &cluster, MkvTrackInfo const& track, uint64_t segment_timecode_scale, std::vector<SecondarySubtitlePacket>& packets) {
	auto const& group_context = EBML_CONTEXT(&group_element);
	auto cursor = next_child(stream, group_context);
	std::optional<RawBlockPayload> raw_block;
	uint64_t duration_units = 0;
	bool have_duration = false;

	while (cursor.element && cursor.upper <= 0) {
		if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxBlockDuration)) {
			auto &duration = *static_cast<libmatroska::KaxBlockDuration*>(cursor.element.get());
			duration.ReadData(stream.I_O());
			duration_units = duration.GetValue();
			have_duration = true;
		}
		else if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxBlock)) {
			auto &block = *static_cast<libmatroska::KaxBlock*>(cursor.element.get());
			raw_block = read_selected_block(stream, block, cluster, track);
		}
		else {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
		}
		cursor = next_child(stream, group_context);
	}

	if (raw_block)
		append_block_packets(track, *raw_block, have_duration, duration_units, segment_timecode_scale, packets);
	return cursor;
}

void import_track(agi::fs::path const& filename, MkvTrackInfo const& track, uint64_t segment_timecode_scale, agi::ProgressSink *ps, std::vector<std::pair<int, std::string>> &lines, std::shared_ptr<TransientFontSet>& transient_fonts) {
	PathIOCallback input(filename);
	EbmlStream stream(input);
	auto segment = open_segment(stream);
	if (!segment)
		throw MatroskaException("File is not a Matroska file.");

	auto const file_size = std::max<uint64_t>(agi::fs::Size(filename), 1);
	auto const& segment_context = EBML_CONTEXT(segment.get());
	auto cursor = next_child(stream, segment_context);
	int fallback_sort_key = 0;

	while (cursor.element && cursor.upper <= 0) {
		throw_if_cancelled(ps);

		if (track.subtitle_codec != MkvTextSubtitleCodec::Utf8 && EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxAttachments)) {
			cursor = continue_after_nested(stream, segment_context, parse_attachments(stream, *cursor.element, transient_fonts));
		}
		else if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxCluster)) {
			auto &cluster = *static_cast<libmatroska::KaxCluster*>(cursor.element.get());
			auto const& cluster_context = EBML_CONTEXT(cursor.element.get());
			auto child = next_child(stream, cluster_context);
			bool cluster_initialized = false;

			while (child.element && child.upper <= 0) {
				throw_if_cancelled(ps);

				if (EbmlId(*child.element) == EBML_ID(libmatroska::KaxClusterTimecode)) {
					auto &timestamp = *static_cast<libmatroska::KaxClusterTimecode*>(child.element.get());
					timestamp.ReadData(stream.I_O());
					cluster.InitTimecode(timestamp.GetValue(), segment_timecode_scale);
					cluster_initialized = true;
					child = next_child(stream, cluster_context);
				}
				else if (EbmlId(*child.element) == EBML_ID(libmatroska::KaxBlockGroup)) {
					if (cluster_initialized)
						child = continue_after_nested(stream, cluster_context, parse_block_group(stream, *child.element, cluster, track, lines, fallback_sort_key, segment_timecode_scale));
					else {
						child.element->SkipData(stream, EBML_CONTEXT(child.element.get()));
						child = next_child(stream, cluster_context);
					}
				}
				else if (EbmlId(*child.element) == EBML_ID(libmatroska::KaxSimpleBlock)) {
					if (cluster_initialized) {
						auto &block = *static_cast<libmatroska::KaxSimpleBlock*>(child.element.get());
						auto raw_block = read_selected_block(stream, block, cluster, track);
						if (raw_block)
							append_block_lines(track, *raw_block, false, 0, lines, fallback_sort_key, segment_timecode_scale);
						child = next_child(stream, cluster_context);
					}
					else {
						child.element->SkipData(stream, EBML_CONTEXT(child.element.get()));
						child = next_child(stream, cluster_context);
					}
				}
				else {
					child.element->SkipData(stream, EBML_CONTEXT(child.element.get()));
					child = next_child(stream, cluster_context);
				}

				if (ps)
					ps->SetProgress(stream.I_O().getFilePointer(), file_size);
			}

			cursor = continue_after_nested(stream, segment_context, std::move(child));
		}
		else {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
			cursor = next_child(stream, segment_context);
		}
	}
}

void import_packet_track(agi::fs::path const& filename, MkvTrackInfo const& track, uint64_t segment_timecode_scale, agi::ProgressSink *ps, std::vector<SecondarySubtitlePacket>& packets) {
	PathIOCallback input(filename);
	EbmlStream stream(input);
	auto segment = open_segment(stream);
	if (!segment)
		throw MatroskaException("File is not a Matroska file.");

	auto const file_size = std::max<uint64_t>(agi::fs::Size(filename), 1);
	auto const& segment_context = EBML_CONTEXT(segment.get());
	auto cursor = next_child(stream, segment_context);

	while (cursor.element && cursor.upper <= 0) {
		throw_if_cancelled(ps);
		if (EbmlId(*cursor.element) == EBML_ID(libmatroska::KaxCluster)) {
			auto &cluster = *static_cast<libmatroska::KaxCluster*>(cursor.element.get());
			auto const& cluster_context = EBML_CONTEXT(cursor.element.get());
			auto child = next_child(stream, cluster_context);
			bool cluster_initialized = false;

			while (child.element && child.upper <= 0) {
				throw_if_cancelled(ps);
				if (EbmlId(*child.element) == EBML_ID(libmatroska::KaxClusterTimecode)) {
					auto &timestamp = *static_cast<libmatroska::KaxClusterTimecode*>(child.element.get());
					timestamp.ReadData(stream.I_O());
					cluster.InitTimecode(timestamp.GetValue(), segment_timecode_scale);
					cluster_initialized = true;
					child = next_child(stream, cluster_context);
				}
				else if (EbmlId(*child.element) == EBML_ID(libmatroska::KaxBlockGroup)) {
					if (cluster_initialized)
						child = continue_after_nested(stream, cluster_context, parse_packet_block_group(stream, *child.element, cluster, track, segment_timecode_scale, packets));
					else {
						child.element->SkipData(stream, EBML_CONTEXT(child.element.get()));
						child = next_child(stream, cluster_context);
					}
				}
				else if (EbmlId(*child.element) == EBML_ID(libmatroska::KaxSimpleBlock)) {
					if (cluster_initialized) {
						auto &block = *static_cast<libmatroska::KaxSimpleBlock*>(child.element.get());
						auto raw_block = read_selected_block(stream, block, cluster, track);
						if (raw_block)
							append_block_packets(track, *raw_block, false, 0, segment_timecode_scale, packets);
						child = next_child(stream, cluster_context);
					}
					else {
						child.element->SkipData(stream, EBML_CONTEXT(child.element.get()));
						child = next_child(stream, cluster_context);
					}
				}
				else {
					child.element->SkipData(stream, EBML_CONTEXT(child.element.get()));
					child = next_child(stream, cluster_context);
				}

				if (ps)
					ps->SetProgress(stream.I_O().getFilePointer(), file_size);
			}
			cursor = continue_after_nested(stream, segment_context, std::move(child));
		}
		else {
			cursor.element->SkipData(stream, EBML_CONTEXT(cursor.element.get()));
			cursor = next_child(stream, segment_context);
		}
	}
}

void load_text_track(agi::fs::path const& filename, MkvTrackInfo const& track, uint64_t segment_timecode_scale, AssFile *target, std::shared_ptr<agi::BackgroundRunnerFactory> background_runner_factory) {
	target->SetTransientFonts({});
	LOG_I(kMkvLogSection) << "Importing MKV subtitle track " << track.track_number << " (" << track.codec_id << ") from " << agi::fs::PathToString(filename);
	if (!track.content_encodings.empty())
		LOG_I(kMkvLogSection) << "Decoding MKV content encodings for track " << track.track_number << ": " << describe_content_encodings(track.content_encodings);

	AssParser parser(target, track.subtitle_codec != MkvTextSubtitleCodec::Ssa);
	if (track.subtitle_codec == MkvTextSubtitleCodec::Utf8) {
		LoadDefaultAssFileWithAppOptions(*target, false, GetSubtitleFormatDefaultStyleCatalog("SRT"));
	}
	else {
		std::string decoded_error;
		auto decoded_codec_private = DecodeMkvContentEncodedData(track.codec_private, track.content_encodings, MkvContentEncodingTarget::Private, &decoded_error);
		if (!decoded_codec_private) {
			throw MatroskaException(agi::format(
				"Failed to decode MKV CodecPrivate for track %u: %s",
				static_cast<unsigned>(track.track_number), decoded_error));
		}
		for (auto const& line : SplitMkvCodecPrivateLines(*decoded_codec_private))
			parser.AddLine(line);
	}
	parser.AddLine("[Events]");

	std::vector<std::pair<int, std::string>> lines;
	std::shared_ptr<TransientFontSet> transient_fonts;
	std::string error;
	auto runner_factory = background_runner_factory ? std::move(background_runner_factory) : std::make_shared<agi::InlineBackgroundRunnerFactory>();
	auto runner = runner_factory->Create(_("Parsing Matroska"), _("Reading subtitles from Matroska file."));
	runner->Run([&](agi::ProgressSink *ps) {
		try {
			import_track(filename, track, segment_timecode_scale, ps, lines, transient_fonts);
		}
		catch (agi::UserCancelException const&) { throw; }
		catch (agi::Exception const& e) { error = e.GetMessage(); ps->Log(error); }
		catch (std::exception const& e) { error = e.what(); ps->Log(error); }
		catch (...) { error = "Unknown Matroska parsing error."; ps->Log(error); }
	});
	if (!error.empty())
		throw MatroskaException(error);

	std::stable_sort(lines.begin(), lines.end(), [](auto const& left, auto const& right) { return left.first < right.first; });
	for (auto &line : lines)
		parser.AddLine(line.second);
	target->SetTransientFonts(transient_fonts);
	LOG_I(kMkvLogSection) << "Imported " << lines.size() << " subtitle lines from MKV track " << track.track_number;
}
}

MkvTrackScanResult MatroskaWrapper::ScanTracks(agi::fs::path const& filename) {
	LogMkvParserBackendOnce();
	return scan_tracks(filename);
}

std::optional<MkvAudioTimeline> MatroskaWrapper::GetOpusAudioTimeline(agi::fs::path const& filename, int audio_ordinal, int audio_count) {
	return scan_opus_audio_timeline(filename, audio_ordinal, audio_count);
}

MkvSubtitleAvailability MatroskaWrapper::GetSubtitleAvailability(agi::fs::path const& filename) {
	LogMkvParserBackendOnce();
	try {
		auto scan = scan_tracks(filename);
		MkvSubtitleAvailability availability;
		availability.text = !collect_importable_subtitle_tracks(scan, false).empty();
		auto const bitmap_tracks = collect_bitmap_subtitle_tracks(scan, false);
		availability.bitmap = !bitmap_tracks.empty();
		for (auto const *track : bitmap_tracks) {
			availability.hdmv_pgs = availability.hdmv_pgs
				|| track->bitmap_subtitle_codec == MkvBitmapSubtitleCodec::HdmvPgs;
			availability.vobsub = availability.vobsub
				|| track->bitmap_subtitle_codec == MkvBitmapSubtitleCodec::VobSub;
		}
		return availability;
	}
	catch (...) {
		return {};
	}
}

void MatroskaWrapper::GetTextSubtitlesForTrack(agi::fs::path const& filename, uint64_t track_number, AssFile *target, std::shared_ptr<agi::BackgroundRunnerFactory> background_runner_factory) {
	LogMkvParserBackendOnce();
	auto scan = scan_tracks(filename);
	auto track = std::find_if(scan.tracks.begin(), scan.tracks.end(), [&](MkvTrackInfo const& value) {
		return value.track_number == track_number && IsImportableMkvSubtitleTrack(value);
	});
	if (track == scan.tracks.end())
		throw MatroskaException("Selected Matroska text subtitle track is unavailable.");
	load_text_track(filename, *track, scan.segment_timecode_scale, target, std::move(background_runner_factory));
}

SecondarySubtitlePacketStream MatroskaWrapper::GetBitmapSubtitlePacketsForTrack(agi::fs::path const& filename, uint64_t track_number, std::shared_ptr<agi::BackgroundRunnerFactory> background_runner_factory) {
	LogMkvParserBackendOnce();
	auto scan = scan_tracks(filename);
	auto track = std::find_if(scan.tracks.begin(), scan.tracks.end(), [&](MkvTrackInfo const& value) {
		return value.track_number == track_number && IsDecodableMkvBitmapSubtitleTrack(value);
	});
	if (track == scan.tracks.end())
		throw MatroskaException("Selected Matroska bitmap subtitle track is unavailable.");
	auto const codec_id = GetSecondarySubtitleCodecId(track->bitmap_subtitle_codec);
	if (codec_id.empty())
		throw MatroskaException("Selected Matroska bitmap subtitle track is unavailable.");

	SecondarySubtitlePacketStream stream;
	stream.codec_id = codec_id;
	std::string decoded_error;
	auto codec_private = DecodeMkvContentEncodedData(track->codec_private, track->content_encodings, MkvContentEncodingTarget::Private, &decoded_error);
	if (!codec_private)
		throw MatroskaException("Failed to decode Matroska subtitle CodecPrivate: " + decoded_error);
	stream.codec_private = std::move(*codec_private);

	std::string error;
	auto runner_factory = background_runner_factory ? std::move(background_runner_factory) : std::make_shared<agi::InlineBackgroundRunnerFactory>();
	auto runner = runner_factory->Create(_("Parsing Matroska"), _("Reading bitmap subtitles from Matroska file."));
	runner->Run([&](agi::ProgressSink *ps) {
		try {
			import_packet_track(filename, *track, scan.segment_timecode_scale, ps, stream.packets);
		}
		catch (agi::UserCancelException const&) { throw; }
		catch (agi::Exception const& e) { error = e.GetMessage(); ps->Log(error); }
		catch (std::exception const& e) { error = e.what(); ps->Log(error); }
		catch (...) { error = "Unknown Matroska packet extraction error."; ps->Log(error); }
	});
	if (!error.empty())
		throw MatroskaException(error);
	if (stream.packets.empty())
		throw MatroskaException("Selected Matroska bitmap subtitle track contains no packets.");
	return stream;
}

void MatroskaWrapper::GetSubtitles(agi::fs::path const& filename, AssFile *target, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink, std::shared_ptr<agi::BackgroundRunnerFactory> background_runner_factory, bool secondary_track_choice, std::string *selected_track_label) {
	LogMkvParserBackendOnce();
	target->SetTransientFonts({});

	auto scan = scan_tracks(filename);
	auto subtitle_tracks = collect_importable_subtitle_tracks(scan, true);
	if (subtitle_tracks.empty())
		throw MatroskaException("File has no recognised subtitle tracks.");

	MkvTrackInfo const* selected_track = subtitle_tracks.front();
	if (subtitle_tracks.size() > 1) {
		std::vector<std::string> choices;
		choices.reserve(subtitle_tracks.size());
		for (auto const* track : subtitle_tracks)
			choices.emplace_back(DescribeMkvTrack(*track));

		if (!choice_sink)
			throw agi::UserCancelException("canceled");
		auto choice = choice_sink->RequestSingleChoice(
				aegisub::track_choice::BuildRequest(aegisub::track_choice::DialogKind::Subtitle, choices, secondary_track_choice));
		auto resolved = aegisub::track_choice::ResolveSelection(subtitle_tracks.size(), choice);
		if (!resolved)
			throw agi::UserCancelException("canceled");

		selected_track = subtitle_tracks[*resolved];
	}

	if (selected_track_label)
		*selected_track_label = DescribeMkvTrack(*selected_track);

	load_text_track(filename, *selected_track, scan.segment_timecode_scale, target, std::move(background_runner_factory));
}

bool MatroskaWrapper::HasSubtitles(agi::fs::path const& filename) {
	return GetSubtitleAvailability(filename).text;
}
