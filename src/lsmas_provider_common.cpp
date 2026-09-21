#include "lsmas_provider_common.h"

#include "mkv_wrap.h"
#include "options.h"
#include "provider_index_cache.h"
#include "track_choice.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <limits>
#include <sstream>

namespace lsmas_provider {

ErrorString::~ErrorString() {
    if (value)
        lsmas::GetApi().free(value);
}

void ErrorString::Reset() {
    if (value)
        lsmas::GetApi().free(value);
    value = nullptr;
}

std::string ErrorString::Message(std::string const& fallback) const {
    if (value && *value)
        return value;
    return fallback;
}

namespace {
int GetConfiguredLsmasNativeVideoThreads() {
    return config::GetIntOptionOrDefault("Provider/Video/LsmasNative/Decoding Threads", 0);
}

std::string TrackTypeName(TrackType type) {
    return type == TrackType::Video ? "video" : "audio";
}

MkvTrackType ToMkvTrackType(TrackType type) {
    return type == TrackType::Video ? MkvTrackType::Video : MkvTrackType::Audio;
}

bool IsMatroskaLikePath(agi::fs::path const& filename) {
    return agi::fs::HasExtension(filename, "mkv")
        || agi::fs::HasExtension(filename, "mka")
        || agi::fs::HasExtension(filename, "mks")
        || agi::fs::HasExtension(filename, "mk3d")
        || agi::fs::HasExtension(filename, "webm");
}

std::string GetString(json::Object const& object, char const *key) {
    auto it = object.find(key);
    if (it == object.end())
        return {};
    try {
        return static_cast<json::String const&>(it->second);
    }
    catch (...) {
        return {};
    }
}

int64_t GetInteger(json::Object const& object, char const *key, int64_t fallback = 0) {
    auto it = object.find(key);
    if (it == object.end())
        return fallback;
    try {
        return static_cast<json::Integer const&>(it->second);
    }
    catch (...) {
        return fallback;
    }
}

int GetPositiveInt(json::Object const& object, char const *key) {
    auto const value = GetInteger(object, key);
    if (value <= 0 || value > std::numeric_limits<int>::max())
        return 0;
    return static_cast<int>(value);
}

TrackRational GetRational(json::Object const& object, char const *key) {
    auto it = object.find(key);
    if (it == object.end())
        return {};

    try {
        auto const& rational = static_cast<json::Object const&>(it->second);
        auto const numerator = GetInteger(rational, "num");
        auto const denominator = GetInteger(rational, "den");
        if (numerator <= 0 || denominator <= 0)
            return {};
        return { numerator, denominator };
    }
    catch (...) {
        return {};
    }
}

std::string FormatFrameRate(TrackRational const& frame_rate) {
    if (frame_rate.numerator <= 0 || frame_rate.denominator <= 0)
        return {};
    return std::to_string(frame_rate.numerator) + "/"
        + std::to_string(frame_rate.denominator) + " fps";
}

std::string FormatDimensions(int width, int height) {
    if (width <= 0 || height <= 0)
        return {};
    return std::to_string(width) + "x" + std::to_string(height);
}

std::string FormatChannels(int channels) {
    if (channels <= 0)
        return {};
    return std::to_string(channels) + " ch";
}

void UpdateDisplayName(TrackChoice& choice, TrackType type) {
    aegisub::track_choice::TrackLabel label;
    label.index = choice.stream_index;
    label.codec = choice.codec_name;
    if (type == TrackType::Video) {
        label.details = {
            FormatDimensions(choice.width, choice.height),
            FormatFrameRate(choice.frame_rate),
            choice.language
        };
    }
    else {
        label.details = {
            FormatChannels(choice.channels),
            choice.language
        };
    }
    label.title = choice.title;
    choice.display_name = aegisub::track_choice::FormatTrackLabel(label);
}

void TryEnrichTracksFromMkv(agi::fs::path const& filename, TrackType type, std::vector<TrackChoice>& tracks) {
#if AEGISUB_MATROSKA_PARSING
    if (tracks.size() <= 1 || !IsMatroskaLikePath(filename))
        return;

    try {
        auto const scan = MatroskaWrapper::ScanTracks(filename);
        auto const wanted_type = ToMkvTrackType(type);
        for (auto& choice : tracks) {
            auto const match = std::find_if(scan.tracks.begin(), scan.tracks.end(), [&](MkvTrackInfo const& track) {
                return track.global_ordinal == choice.stream_index && track.type == wanted_type;
            });
            if (match == scan.tracks.end())
                continue;

            if (choice.language.empty())
                choice.language = GetPreferredMkvTrackLanguage(*match);
            if (choice.title.empty())
                choice.title = match->name;
            if (type == TrackType::Audio && choice.channels <= 0 && match->audio_channels)
                choice.channels = *match->audio_channels;
            UpdateDisplayName(choice, type);
        }
    }
    catch (agi::Exception const& e) {
        LOG_D("provider/lsmasnative/mkv") << "Failed to enrich MKV track metadata for "
            << agi::fs::PathToString(filename) << ": " << e.GetMessage();
    }
    catch (std::exception const& e) {
        LOG_D("provider/lsmasnative/mkv") << "Failed to enrich MKV track metadata for "
            << agi::fs::PathToString(filename) << ": " << e.what();
    }
#else
    (void)filename;
    (void)type;
    (void)tracks;
#endif
}
}

std::vector<TrackChoice> ParseTrackChoicesJson(std::string_view json_text, TrackType type) {
    std::istringstream stream{std::string(json_text)};
    json::UnknownElement root;
    json::Reader::Read(root, stream);
    auto const& root_object = static_cast<json::Object const&>(root);
    auto streams_it = root_object.find("streams");
    if (streams_it == root_object.end())
        return {};

    std::vector<TrackChoice> tracks;
    for (auto const& item : static_cast<json::Array const&>(streams_it->second)) {
        auto const& object = static_cast<json::Object const&>(item);
        if (GetString(object, "type") != TrackTypeName(type))
            continue;

        auto const stream_index = GetInteger(object, "index", -1);
        if (stream_index < 0 || stream_index > std::numeric_limits<int>::max())
            continue;

        TrackChoice choice;
        choice.stream_index = static_cast<int>(stream_index);
        choice.codec_name = GetString(object, "codec");
        choice.language = GetString(object, "language");
        choice.title = GetString(object, "title");
        choice.channels = GetPositiveInt(object, "channels");
        choice.width = GetPositiveInt(object, "width");
        choice.height = GetPositiveInt(object, "height");
        choice.frame_rate = GetRational(object, "avg_frame_rate");
        UpdateDisplayName(choice, type);
        tracks.push_back(std::move(choice));
    }
    return tracks;
}

std::vector<TrackChoice> ProbeTracks(agi::fs::path const& filename, TrackType type) {
    auto const& api = lsmas::GetApi();
    auto const filename_utf8 = agi::fs::PathToString(filename);

    ErrorString error;
    char *json_text = api.probe_streams_json_utf8(filename_utf8.c_str(), error.Out());
    if (!json_text)
        throw agi::EnvironmentError(error.Message("failed to probe media streams"));
    std::unique_ptr<char, decltype(api.free)> json_holder(json_text, api.free);

    auto tracks = ParseTrackChoicesJson(json_text, type);
    TryEnrichTracksFromMkv(filename, type, tracks);
    return tracks;
}

int SelectTrack(agi::fs::path const& filename,
				TrackType type,
				std::shared_ptr<agi::SingleChoiceInteractionSink> const& choice_sink,
				int *type_ordinal, int *type_count) {
	if (type_ordinal)
		*type_ordinal = -1;
	auto tracks = ProbeTracks(filename, type);
	if (type_count)
		*type_count = static_cast<int>(tracks.size());
	if (tracks.empty())
        return -1;
	if (tracks.size() == 1 || !choice_sink) {
		if (type_ordinal)
			*type_ordinal = 0;
		return tracks.front().stream_index;
	}

	std::vector<std::string> choices;
	choices.reserve(tracks.size());
    for (auto const& track : tracks)
        choices.push_back(track.display_name);

    auto choice = choice_sink->RequestSingleChoice(aegisub::track_choice::BuildRequest(
        type == TrackType::Video ? aegisub::track_choice::DialogKind::Video : aegisub::track_choice::DialogKind::Audio,
        choices));
    auto resolved = aegisub::track_choice::ResolveSelection(tracks.size(), choice);
    if (!resolved)
        throw agi::UserCancelException(type == TrackType::Video ? "video loading canceled by user" : "audio loading canceled by user");
	if (type_ordinal)
		*type_ordinal = static_cast<int>(*resolved);
	return tracks[*resolved].stream_index;
}

agi::fs::path GetIndexCacheFilename(agi::fs::path const& filename) {
    return aegisub::provider_index_cache::BuildFilename(filename, "?local/lsmasnativecache/", ".lwi");
}

void CleanIndexCache() {
    aegisub::provider_index_cache::Clean("?local/lsmasnativecache/",
        "*.lwi",
        "Provider/LsmasNative/Cache/Size",
        "Provider/LsmasNative/Cache/Files");
}

lsmas_video_open_options_t MakeVideoOpenOptions(int stream_index) {
    lsmas_video_open_options_t options = {};
    options.stream_index = stream_index;
    options.threads = GetConfiguredLsmasNativeVideoThreads();
    options.seek_mode = LSMAS_SEEK_NORMAL;
    options.seek_threshold = 10;
    options.fpsden = 1;
    options.prefer_hw = LSMAS_HW_NONE;
    options.cache_index = 1;
    options.soft_reset = 1;
    options.repeat = 1;
    options.dominance = LSMAS_DOMINANCE_OBEY;
    return options;
}

lsmas_audio_open_options_t MakeAudioOpenOptions(int stream_index, bool downmix) {
    lsmas_audio_open_options_t options = {};
    options.stream_index = stream_index;
    options.threads = 0;
    options.av_sync = 1;
    options.cache_index = 1;
    options.sample_format = LSMAS_AUDIO_S16;
    if (downmix)
        options.channel_layout = 0x4; // AV_CH_FRONT_CENTER
    return options;
}

int ProgressCallback(void *userdata, const char *message_utf8, int32_t percent) {
    auto *ps = static_cast<agi::ProgressSink *>(userdata);
    if (!ps)
        return 0;
    if (message_utf8 && *message_utf8)
        ps->SetMessage(message_utf8);
    ps->SetProgress(percent, 100);
    return ps->IsCancelled() ? 1 : 0;
}

}
