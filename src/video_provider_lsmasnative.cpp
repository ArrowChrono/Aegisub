#ifdef WITH_LSMASNATIVE

#include "include/aegisub/video_provider.h"

#include "lsmas_native_api.h"
#include "lsmas_provider_common.h"
#include "options.h"
#ifdef WITH_SCENECHANGE
#include "scenechange_native_api.h"
#endif
#include "source_frame.h"
#include "video_frame.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/exception.h>
#include <libaegisub/fs.h>
#ifdef WITH_SCENECHANGE
#include <libaegisub/keyframe.h>
#endif
#include <libaegisub/make_unique.h>
#include <libaegisub/log.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
bool GetConfiguredLsmasNativeAudioDownmix() {
    return config::GetBoolOptionOrDefault("Provider/Audio/LsmasNative/Downmix", false);
}

enum AvColorSpace {
    AVCOL_SPC_RGB = 0,
    AVCOL_SPC_BT709 = 1,
    AVCOL_SPC_UNSPECIFIED = 2,
    AVCOL_SPC_FCC = 4,
    AVCOL_SPC_BT470BG = 5,
    AVCOL_SPC_SMPTE170M = 6,
    AVCOL_SPC_SMPTE240M = 7,
    AVCOL_SPC_YCOCG = 8,
    AVCOL_SPC_BT2020_NCL = 9,
    AVCOL_SPC_BT2020_CL = 10,
    AVCOL_SPC_ICTCP = 14
};

enum AvColorRange {
    AVCOL_RANGE_UNSPECIFIED = 0,
    AVCOL_RANGE_MPEG = 1,
    AVCOL_RANGE_JPEG = 2
};

enum AvColorPrimaries {
    AVCOL_PRI_BT709 = 1,
    AVCOL_PRI_BT470M = 4,
    AVCOL_PRI_BT470BG = 5,
    AVCOL_PRI_SMPTE170M = 6,
    AVCOL_PRI_SMPTE240M = 7,
    AVCOL_PRI_FILM = 8,
    AVCOL_PRI_BT2020 = 9,
    AVCOL_PRI_SMPTE431 = 11,
    AVCOL_PRI_SMPTE432 = 12
};

enum AvTransferCharacteristics {
    AVCOL_TRC_BT709 = 1,
    AVCOL_TRC_GAMMA22 = 4,
    AVCOL_TRC_GAMMA28 = 5,
    AVCOL_TRC_SMPTE170M = 6,
    AVCOL_TRC_SMPTE240M = 7,
    AVCOL_TRC_LINEAR = 8,
    AVCOL_TRC_IEC61966_2_1 = 13,
    AVCOL_TRC_BT2020_10 = 14,
    AVCOL_TRC_BT2020_12 = 15,
    AVCOL_TRC_SMPTE2084 = 16,
    AVCOL_TRC_ARIB_STD_B67 = 18
};

enum AvChromaLocation {
    AVCHROMA_LOC_LEFT = 1,
    AVCHROMA_LOC_CENTER = 2,
    AVCHROMA_LOC_TOPLEFT = 3,
    AVCHROMA_LOC_TOP = 4,
    AVCHROMA_LOC_BOTTOMLEFT = 5,
    AVCHROMA_LOC_BOTTOM = 6
};

SourceFrameDolbyVisionMetadata BuildDolbyVisionMetadata(
    lsmas::Api const& api,
    lsmas_video_frame_t const *frame) {
    SourceFrameDolbyVisionMetadata out;
    if (!api.video_frame_get_dovi_metadata)
        return out;

    lsmas_provider::ErrorString error;
    lsmas_dovi_metadata_t metadata = {};
    int metadata_rc = api.video_frame_get_dovi_metadata(frame, &metadata, error.Out());
    if (metadata_rc != 0 || !metadata.valid)
        return out;

    out.bl_bit_depth = metadata.bl_bit_depth;
    out.coefficient_log2_denom = metadata.coefficient_log2_denom;
    out.has_l1 = metadata.has_l1 != 0;
    for (int i = 0; i < 3; ++i)
        out.nonlinear_offset[static_cast<size_t>(i)] = metadata.nonlinear_offset[i];
    for (int i = 0; i < 9; ++i) {
        out.nonlinear[static_cast<size_t>(i)] = metadata.nonlinear[i];
        out.linear[static_cast<size_t>(i)] = metadata.linear[i];
    }

    for (int c = 0; c < 3; ++c) {
        auto const& src = metadata.comp[c];
        auto& dst = out.comp[static_cast<size_t>(c)];
        dst.num_pivots = src.num_pivots;
        for (int i = 0; i < 9; ++i)
            dst.pivots[static_cast<size_t>(i)] = src.pivots[i];
        int const pieces = std::max(0, std::min<int>(dst.num_pivots, 9) - 1);
        for (int i = 0; i < pieces; ++i) {
            dst.method[static_cast<size_t>(i)] = src.method[i];
            for (int k = 0; k < 3; ++k)
                dst.poly_coeffs[static_cast<size_t>(i)][static_cast<size_t>(k)] = src.poly_coeffs[i][k];
            dst.mmr_order[static_cast<size_t>(i)] = src.mmr_order[i];
            dst.mmr_constant[static_cast<size_t>(i)] = src.mmr_constant[i];
            for (int j = 0; j < 3; ++j) {
                for (int k = 0; k < 7; ++k)
                    dst.mmr_coeffs[static_cast<size_t>(i)][static_cast<size_t>(j)][static_cast<size_t>(k)] =
                        src.mmr_coeffs[i][j][k];
            }
        }
    }

    out.source_min_pq = metadata.source_min_pq;
    out.source_max_pq = metadata.source_max_pq;
    out.max_pq_y = metadata.max_pq_y;
    out.avg_pq_y = metadata.avg_pq_y;

    const uint8_t *rpu_data = nullptr;
    int32_t rpu_size = 0;
    error.Reset();
    if (api.video_frame_get_side_data) {
        int rpu_rc = api.video_frame_get_side_data(
            frame,
            LSMAS_FRAME_SIDE_DATA_DOVI_RPU,
            &rpu_data,
            &rpu_size,
            error.Out());
        if (rpu_rc > 0 && rpu_data && rpu_size > 0)
            out.rpu.assign(rpu_data, rpu_data + rpu_size);
    }

    out.valid = true;
    return out;
}

std::string MatrixName(int cs, int cr, int width, int height) {
    if (cs == AVCOL_SPC_RGB)
        return "None";
    std::string range = cr == AVCOL_RANGE_JPEG ? "PC" : "TV";
    if (cs == AVCOL_SPC_UNSPECIFIED)
        cs = width > 1024 || height >= 600 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
    switch (cs) {
        case AVCOL_SPC_BT709: return range + ".709";
        case AVCOL_SPC_FCC: return range + ".FCC";
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: return range + ".601";
        case AVCOL_SPC_SMPTE240M: return range + ".240M";
        case AVCOL_SPC_BT2020_NCL: return range + ".2020";
        case AVCOL_SPC_BT2020_CL: return range + ".2020CL";
        case AVCOL_SPC_ICTCP: return range + ".ICTCP";
        default: return range + ".709";
    }
}

std::string PrimariesName(int primaries, int cs) {
    switch (primaries) {
        case AVCOL_PRI_BT709: return "BT.709";
        case AVCOL_PRI_BT470M: return "BT.470M";
        case AVCOL_PRI_BT470BG: return "BT.601-625";
        case AVCOL_PRI_SMPTE170M: return "BT.601-525";
        case AVCOL_PRI_SMPTE240M: return "SMPTE-240M";
        case AVCOL_PRI_FILM: return "Film C";
        case AVCOL_PRI_BT2020: return "BT.2020";
        case AVCOL_PRI_SMPTE431: return "DCI-P3";
        case AVCOL_PRI_SMPTE432: return "Display-P3";
        default: break;
    }
    switch (cs) {
        case AVCOL_SPC_BT709: return "BT.709";
        case AVCOL_SPC_BT470BG: return "BT.601-625";
        case AVCOL_SPC_SMPTE170M: return "BT.601-525";
        case AVCOL_SPC_SMPTE240M: return "SMPTE-240M";
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
        case AVCOL_SPC_ICTCP: return "BT.2020";
        default: return {};
    }
}

std::string TransferName(int transfer) {
    switch (transfer) {
        case AVCOL_TRC_GAMMA22: return "Gamma 2.2";
        case AVCOL_TRC_GAMMA28: return "Gamma 2.8";
        case AVCOL_TRC_LINEAR: return "Linear";
        case AVCOL_TRC_IEC61966_2_1: return "sRGB";
        case AVCOL_TRC_SMPTE2084: return "PQ";
        case AVCOL_TRC_ARIB_STD_B67: return "HLG";
        case AVCOL_TRC_BT709:
        case AVCOL_TRC_SMPTE170M:
        case AVCOL_TRC_SMPTE240M:
        case AVCOL_TRC_BT2020_10:
        case AVCOL_TRC_BT2020_12: return "BT.1886";
        default: return {};
    }
}

SourceFrameColorMetadata ColorMetadata(lsmas_video_frame_props_t const& props) {
    SourceFrameColorMetadata color;
    color.matrix = MatrixName(props.colorspace, props.color_range, props.width, props.height);
    color.primaries = PrimariesName(props.color_primaries, props.colorspace);
    color.transfer = TransferName(props.color_trc);
    color.range = props.color_range == AVCOL_RANGE_JPEG ? SourceFrameColorRange::Full : SourceFrameColorRange::Limited;
    if (props.colorspace == AVCOL_SPC_RGB)
        color.matrix = "None";
    return color;
}

SourceFrameChromaLocation ChromaLocation(int value) {
    switch (value) {
        case AVCHROMA_LOC_LEFT: return SourceFrameChromaLocation::Left;
        case AVCHROMA_LOC_CENTER: return SourceFrameChromaLocation::Center;
        case AVCHROMA_LOC_TOPLEFT: return SourceFrameChromaLocation::TopLeft;
        case AVCHROMA_LOC_TOP: return SourceFrameChromaLocation::TopCenter;
        case AVCHROMA_LOC_BOTTOMLEFT: return SourceFrameChromaLocation::BottomLeft;
        case AVCHROMA_LOC_BOTTOM: return SourceFrameChromaLocation::BottomCenter;
        default: return SourceFrameChromaLocation::Unknown;
    }
}

SourceFrameFormatInfo ConvertFormatInfo(lsmas_video_format_info_t const& info) {
    SourceFrameFormatInfo out;
    out.color_family = info.color_family == LSMAS_COLOR_FAMILY_RGB ? SourceFrameColorFamily::Rgb
        : info.color_family == LSMAS_COLOR_FAMILY_YCBCR ? SourceFrameColorFamily::YCbCr
        : SourceFrameColorFamily::Unknown;
    out.plane_count = info.plane_count;
    for (int i = 0; i < info.plane_count && i < 4; ++i) {
        auto const& src = info.planes[i];
        auto& dst = out.planes[static_cast<size_t>(i)];
        dst.width_divisor = std::max(1, src.width_divisor);
        dst.height_divisor = std::max(1, src.height_divisor);
        dst.components_per_sample = std::max(1, src.components_per_sample);
        dst.bytes_per_sample = std::max(1, src.bytes_per_sample);
        dst.bits_per_component = std::max(1, src.bits_per_component);
        for (int c = 0; c < 4; ++c)
            dst.component_shift[static_cast<size_t>(c)] = src.component_shift[c];
    }
    return out;
}

SourceFrameGeometry BuildGeometry(lsmas_video_frame_props_t const& props) {
    auto geometry = MakeDefaultSourceFrameGeometry(props.width, props.height);
    int visible_x = std::max(0, props.crop_left);
    int visible_y = std::max(0, props.crop_top);
    int visible_w = props.width - std::max(0, props.crop_left) - std::max(0, props.crop_right);
    int visible_h = props.height - std::max(0, props.crop_top) - std::max(0, props.crop_bottom);
    if (visible_w > 0 && visible_h > 0)
        geometry.visible_rect = { visible_x, visible_y, visible_w, visible_h };
    geometry.rotation = props.display_rotation_degrees;
    geometry.display_vflip = props.display_vflip != 0;
    if (props.sar_num > 0 && props.sar_den > 0)
        geometry.pixel_aspect_ratio = static_cast<double>(props.sar_num) / props.sar_den;
    return geometry;
}

class LsmasVideoProvider final : public VideoProvider {
    lsmas_handle_t *handle = nullptr;
    lsmas_video_info_t info = {};
    lsmas_video_frame_props_t first_props = {};
    SourceFrameColorMetadata color;
    SourceFrameGeometry geometry;
    SourceFrameFormatInfo native_format_info;
    int native_pix_fmt = -1;
    std::string native_format_name;
    std::string cache_filename_utf8;
    std::vector<int> keyframes;
    agi::vfr::Framerate timecodes;
    bool has_audio = false;
    SourceFrameOutputMode output_mode = SourceFrameOutputMode::Bgra8;

public:
    LsmasVideoProvider(agi::fs::path const& filename, std::string const&, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink);
    ~LsmasVideoProvider() override {
        if (handle)
            lsmas::GetApi().av_close(handle);
    }

    void GetFrame(int n, VideoFrame &frame) override;
    bool GetNativeFrame(int n, SourceFrame& frame, std::shared_ptr<void>& owner) override;
    void SetColorSpace(std::string const&) override { }

    int GetFrameCount() const override { return info.num_frames; }
    int GetWidth() const override { return GetSourceFrameDisplayOutputRect(geometry).width; }
    int GetHeight() const override { return GetSourceFrameDisplayOutputRect(geometry).height; }
    double GetDAR() const override { return GetSourceFrameDisplayAspectRatio(geometry); }
    agi::vfr::Framerate GetFPS() const override { return timecodes; }
    std::vector<int> GetKeyFrames() const override { return keyframes; }
    std::string GetColorSpace() const override { return color.matrix.empty() ? "None" : color.matrix; }
    SourceFrameColorMetadata GetColorMetadata() const override { return color; }
    SourceFrameColorMetadata GetRealColorMetadata() const override { return color; }
    SourceFrameGeometry GetFrameGeometry() const override { return output_mode == SourceFrameOutputMode::Native ? geometry : BakeSourceFrameGeometry(geometry); }
    SourceFrameNativeFormatIdentity GetNativeFormatIdentity() const override { return { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, native_pix_fmt }; }
    std::string GetNativeFormatDescription() const override { return native_format_name; }
    std::vector<SourceFrameOutputMode> GetAvailableSourceModes() const override { return { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }; }
    bool SetOutputMode(SourceFrameOutputMode mode) override;
    std::string GetDecoderName() const override { return "LsmasNative"; }
    bool WantsCaching() const override { return false; }
    bool HasAudio() const override { return has_audio; }
#ifdef WITH_SCENECHANGE
	[[nodiscard]] std::optional<scenechange::KeyframeRequest> PrepareSceneChangeKeyframes() const override;
	void GenerateSceneChangeKeyframes(scenechange::KeyframeRequest const& request,
									  agi::fs::path const& output_path, agi::ProgressSink *ps) override;
#endif
};

class LsmasFrameOwner {
    lsmas_video_frame_t *frame = nullptr;
public:
    explicit LsmasFrameOwner(lsmas_video_frame_t *frame) : frame(frame) { }
    ~LsmasFrameOwner() { if (frame) lsmas::GetApi().video_release_frame(frame); }
    LsmasFrameOwner(LsmasFrameOwner const&) = delete;
    LsmasFrameOwner& operator=(LsmasFrameOwner const&) = delete;
};

std::vector<int> BuildKeyframes(lsmas_handle_t *handle) {
    auto const& api = lsmas::GetApi();
    lsmas_provider::ErrorString error;
    int32_t source_count = 0;
    if (api.video_get_source_frame_count(handle, &source_count, error.Out()) < 0 || source_count <= 0)
        source_count = 0;
    if (source_count <= 0)
        return {};

    std::vector<uint8_t> flags(static_cast<size_t>(source_count));
    error.Reset();
    int32_t got = api.video_get_source_keyframe_flags(handle, flags.data(), source_count, error.Out());
    if (got < 0)
        return {};

    std::vector<int> result;
    for (int32_t i = 0; i < got; ++i) {
        if (flags[static_cast<size_t>(i)])
            result.push_back(i);
    }
    return result;
}

agi::vfr::Framerate BuildTimecodes(lsmas_handle_t *handle, lsmas_video_info_t const& info) {
    auto const& api = lsmas::GetApi();
    if (info.num_frames < 2)
        return info.fps_num > 0 && info.fps_den > 0 ? agi::vfr::Framerate(info.fps_num, info.fps_den) : agi::vfr::Framerate(25.0);

    int32_t tb_num = 0;
    int32_t tb_den = 0;
    lsmas_provider::ErrorString error;
    if (api.video_get_time_base(handle, &tb_num, &tb_den, error.Out()) < 0 || tb_num <= 0 || tb_den <= 0)
        return info.fps_num > 0 && info.fps_den > 0 ? agi::vfr::Framerate(info.fps_num, info.fps_den) : agi::vfr::Framerate(25.0);

    std::vector<int64_t> pts(static_cast<size_t>(info.num_frames));
    error.Reset();
    int32_t got = api.video_get_pts_list(handle, pts.data(), info.num_frames, error.Out());
    if (got < info.num_frames)
        return info.fps_num > 0 && info.fps_den > 0 ? agi::vfr::Framerate(info.fps_num, info.fps_den) : agi::vfr::Framerate(25.0);

    std::vector<int> ms;
    ms.reserve(pts.size());
    for (auto value : pts) {
        long double t = static_cast<long double>(value) * static_cast<long double>(tb_num) * 1000.0L / static_cast<long double>(tb_den);
        ms.push_back(static_cast<int>(t >= 0 ? t + 0.5L : t - 0.5L));
    }
    return agi::vfr::Framerate(ms);
}

LsmasVideoProvider::LsmasVideoProvider(agi::fs::path const& filename, std::string const&, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) {
    auto const& api = lsmas::GetApi();
    auto const filename_utf8 = agi::fs::PathToString(filename);

    int stream_index = lsmas_provider::SelectTrack(filename, lsmas_provider::TrackType::Video, choice_sink);
    if (stream_index < 0)
        throw VideoNotSupported("no video tracks found");

    auto cache_name = lsmas_provider::GetIndexCacheFilename(filename);
    cache_filename_utf8 = agi::fs::PathToString(cache_name);

    auto video_options = lsmas_provider::MakeVideoOpenOptions(stream_index);
    video_options.cachefile = cache_filename_utf8.c_str();

    auto audio_options = lsmas_provider::MakeAudioOpenOptions(-1, GetConfiguredLsmasNativeAudioDownmix());
    audio_options.cachefile = cache_filename_utf8.c_str();

    lsmas_provider::ErrorString error;
    if (br) {
        br->Run([&](agi::ProgressSink *ps) {
            ps->SetTitle("Indexing");
            ps->SetMessage("Reading audio, timecodes and frame data");
            handle = api.av_open_with_progress_utf8(filename_utf8.c_str(), &video_options, &audio_options, lsmas_provider::ProgressCallback, ps, error.Out());
        });
    }
    else {
        handle = api.av_open_with_progress_utf8(filename_utf8.c_str(), &video_options, &audio_options, nullptr, nullptr, error.Out());
    }
    if (!handle)
        throw VideoOpenError(error.Message("failed to open video"));
    auto close_handle_on_error = agi::make_scope_exit([&] {
        if (handle) {
            api.av_close(handle);
            handle = nullptr;
        }
    });
    agi::fs::Touch(cache_name);
    lsmas_provider::CleanIndexCache();

    lsmas_audio_info_t audio_info = {};
    error.Reset();
    has_audio = api.audio_get_info(handle, &audio_info, error.Out()) >= 0;

    error.Reset();
    if (api.video_get_info(handle, &info, error.Out()) < 0 || info.width <= 0 || info.height <= 0 || info.num_frames <= 0)
        throw VideoOpenError(error.Message("failed to query video info"));

    lsmas_video_frame_t *first = nullptr;
    error.Reset();
    if (api.video_acquire_avframe(handle, 0, &first, error.Out()) < 0)
        throw VideoOpenError(error.Message("failed to decode first frame"));
    std::unique_ptr<LsmasFrameOwner> first_owner = std::make_unique<LsmasFrameOwner>(first);

    if (api.video_frame_get_props(first, &first_props, error.Out()) < 0)
        throw VideoOpenError(error.Message("failed to query first frame metadata"));
    lsmas_video_format_info_t fmt = {};
    if (api.video_frame_get_format_info(first, &fmt, error.Out()) < 0)
        throw VideoOpenError(error.Message("failed to query first frame format"));

    auto format_info = ConvertFormatInfo(fmt);

    color = ColorMetadata(first_props);
    geometry = BuildGeometry(first_props);
    native_format_info = format_info;
    native_pix_fmt = first_props.pix_fmt;
    if (auto *name = api.video_frame_get_pix_fmt_name(first))
        native_format_name = std::string(name) + " (ffmpeg:" + std::to_string(native_pix_fmt) + ")";
    else
        native_format_name = "ffmpeg:" + std::to_string(native_pix_fmt);

    keyframes = BuildKeyframes(handle);
    timecodes = BuildTimecodes(handle, info);
    close_handle_on_error.release();
}

#ifdef WITH_SCENECHANGE
std::string SceneChangeError(char const *error, std::string const& fallback) {
    if (error && *error)
        return error;
    return fallback;
}

void ThrowSceneChangeError(char const *error, std::string const& fallback) {
    throw VideoProviderError(SceneChangeError(error, fallback));
}

constexpr int32_t kSceneChangeCanceled = 1;

struct SceneChangeProgressState {
    agi::ProgressSink *sink = nullptr;
    int total_frames = 0;
};

int32_t SCENECHANGE_NATIVE_CALL SceneChangeProgressCallback(void *user_data, int32_t processed_frames, int32_t) {
    auto *state = static_cast<SceneChangeProgressState *>(user_data);
    if (!state || !state->sink)
        return 0;

    try {
        if (state->sink->IsCancelled())
            return kSceneChangeCanceled;

        state->sink->SetProgress(std::min(processed_frames, state->total_frames), state->total_frames);

        return state->sink->IsCancelled() ? kSceneChangeCanceled : 0;
    }
    catch (...) {
        return kSceneChangeCanceled;
    }
}

struct I420Layout {
    size_t y_size = 0;
    size_t chroma_size = 0;
    size_t total_size = 0;
};

I420Layout GetI420Layout(lsmas_video_info_t const& info) {
    if ((info.width & 1) != 0 || (info.height & 1) != 0)
        throw VideoProviderError("YUV420P8 SceneChange input requires even video dimensions.");

    auto const width = static_cast<size_t>(info.width);
    auto const height = static_cast<size_t>(info.height);
    auto const chroma_width = width / 2;
    auto const chroma_height = height / 2;
    auto const max_size = std::numeric_limits<size_t>::max();

    if (height > max_size / width || chroma_height > max_size / chroma_width)
        throw VideoProviderError("Video dimensions are too large for YUV420P8 SceneChange input.");

    I420Layout layout;
    layout.y_size = width * height;
    layout.chroma_size = chroma_width * chroma_height;
    if (layout.chroma_size > (max_size - layout.y_size) / 2)
        throw VideoProviderError("Video dimensions are too large for YUV420P8 SceneChange input.");
    layout.total_size = layout.y_size + layout.chroma_size * 2;
    return layout;
}

I420Layout ValidateI420WriteFrame(scenechange_provider_frame_buffer const& frame,
                                  lsmas_video_info_t const& info) {
    auto const layout = GetI420Layout(info);
    if (frame.struct_size != SCENECHANGE_PROVIDER_FRAME_BUFFER_V1_SIZE ||
        frame.pixel_format != SCENECHANGE_PROVIDER_PIXEL_FORMAT_YUV420P8 ||
        frame.plane_count != 3 ||
        !frame.data ||
        frame.data_size != layout.total_size ||
        !frame.planes[0].data ||
        !frame.planes[1].data ||
        !frame.planes[2].data ||
        frame.planes[0].data != frame.data ||
        frame.planes[1].data != frame.data + layout.y_size ||
        frame.planes[2].data != frame.data + layout.y_size + layout.chroma_size ||
        frame.planes[0].stride != info.width ||
        frame.planes[0].width != info.width ||
        frame.planes[0].height != info.height ||
        frame.planes[1].stride != info.width / 2 ||
        frame.planes[1].width != info.width / 2 ||
        frame.planes[1].height != info.height / 2 ||
        frame.planes[2].stride != info.width / 2 ||
        frame.planes[2].width != info.width / 2 ||
        frame.planes[2].height != info.height / 2)
        throw VideoProviderError("SceneChange provider returned an invalid YUV420P8 write buffer.");
    return layout;
}

void ValidateGray8WriteFrame(scenechange_provider_frame_buffer const& frame,
                             lsmas_video_info_t const& info) {
    int const rounded_width = (info.width + 15) & ~15;
    int const rounded_height = (info.height + 15) & ~15;
    if (frame.struct_size != SCENECHANGE_PROVIDER_FRAME_BUFFER_V1_SIZE ||
        frame.pixel_format != SCENECHANGE_PROVIDER_PIXEL_FORMAT_GRAY8_PADDED16 ||
        frame.plane_count != 1 ||
        !frame.data ||
        !frame.planes[0].data ||
        frame.planes[0].data != frame.data ||
        frame.planes[0].stride < rounded_width ||
        frame.planes[0].width != rounded_width ||
        frame.planes[0].height != rounded_height ||
        frame.data_size != static_cast<size_t>(frame.planes[0].stride) * static_cast<size_t>(rounded_height))
        throw VideoProviderError("SceneChange provider returned an invalid Gray8Padded16 write buffer.");
}

void ValidateLsmasI420Layout(lsmas_video_frame_buffer_layout_t const& layout,
                             int64_t required_bytes,
                             scenechange_provider_frame_buffer const& frame,
                             I420Layout const& expected,
                             lsmas_video_info_t const& info) {
    if (required_bytes <= 0 ||
        layout.required_bytes != required_bytes ||
        static_cast<uint64_t>(required_bytes) != static_cast<uint64_t>(frame.data_size) ||
        layout.output_format != LSMAS_VIDEO_FRAME_OUTPUT_YUV420P8 ||
        layout.width != info.width ||
        layout.height != info.height ||
        layout.plane_count != 3 ||
        layout.plane_width[0] != frame.planes[0].width ||
        layout.plane_height[0] != frame.planes[0].height ||
        layout.plane_stride[0] != frame.planes[0].stride ||
        layout.plane_offset[0] != 0 ||
        layout.plane_width[1] != frame.planes[1].width ||
        layout.plane_height[1] != frame.planes[1].height ||
        layout.plane_stride[1] != frame.planes[1].stride ||
        layout.plane_offset[1] != static_cast<int64_t>(expected.y_size) ||
        layout.plane_width[2] != frame.planes[2].width ||
        layout.plane_height[2] != frame.planes[2].height ||
        layout.plane_stride[2] != frame.planes[2].stride ||
        layout.plane_offset[2] != static_cast<int64_t>(expected.y_size + expected.chroma_size))
        throw VideoDecodeError("LsmasNative returned an incompatible YUV420P8 frame layout for SceneChange.");
}

scenechange::InputPixelFormatMask GetSupportedSceneChangeInputFormats(lsmas::Api const& api) noexcept {
    auto formats = scenechange::kInputPixelFormatGray8Padded16;
    if (lsmas::SupportsVideoFrameOutput(api.api_version, LSMAS_VIDEO_FRAME_OUTPUT_YUV420P8))
        formats |= scenechange::kInputPixelFormatYuv420p8;
    return formats;
}

scenechange::BackendSelection SelectSceneChangeBackend(lsmas_video_info_t const& info) noexcept {
    try {
        auto const& lsm = lsmas::GetApi();
		return scenechange::SelectBackendForDimensions(
			info.width, info.height, GetSupportedSceneChangeInputFormats(lsm), true);
	}
    catch (...) {
        // LsmasNative runtime missing or broken — not a SceneChange DLL issue.
        return {};
    }
}

// Scans with a pre-selected backend snapshot (no second SelectBackend call).
std::vector<int> ScanSceneChangeKeyframes(lsmas_handle_t *handle,
                                          lsmas_video_info_t const& info,
                                          scenechange::Api const& sc,
                                          agi::ProgressSink *ps) {
    if (!handle)
        throw VideoProviderError("LsmasNative handle is not open.");
    if (info.width <= 0 || info.height <= 0 || info.num_frames <= 0)
        throw VideoProviderError("LsmasNative returned invalid video dimensions for SceneChange.");

    auto const& lsm = lsmas::GetApi();

    SceneChangeProgressState progress_state { ps, info.num_frames };
    auto flush_decoder = agi::make_scope_exit([&] {
        lsmas_provider::ErrorString flush_error;
        if (lsm.video_flush)
            lsm.video_flush(handle, flush_error.Out());
    });

    std::vector<int> result;
    result.reserve(std::min(std::max(info.num_frames / 20, 16), 4096));

    if (ps) {
        ps->SetTitle("Generating keyframes");
        ps->SetMessage("Scanning scene changes");
        ps->SetProgress(0, info.num_frames);
    }

    char sc_error[4096] = {};
    {
        std::string const backend_name = scenechange::BackendName(sc.backend);
		LOG_I("provider/lsmasnative/scenechange")
			<< "Scanning scene changes with backend: " << backend_name;
		void *ctx = sc.provider.create(info.width, info.height, nullptr, 0, sc_error, sizeof(sc_error));
        if (!ctx)
            ThrowSceneChangeError(sc_error, "Failed to create " + backend_name + ".");
        auto destroy_ctx = agi::make_scope_exit([&] { sc.provider.destroy(ctx); });

        if (ps) {
            int32_t const rc = sc.provider.set_progress_callback(
                ctx, SceneChangeProgressCallback, &progress_state, sc_error, sizeof(sc_error));
            if (rc != 0)
                ThrowSceneChangeError(sc_error, "Failed to set SceneChange provider progress callback.");
        }

        for (int frame_index = 0; frame_index < info.num_frames; ++frame_index) {
            if (ps && ps->IsCancelled())
                throw agi::UserCancelException("SceneChange keyframe generation canceled by user.");

            scenechange_provider_frame_buffer frame_buffer {};
            frame_buffer.struct_size = SCENECHANGE_PROVIDER_FRAME_BUFFER_V1_SIZE;
            int32_t rc = sc.provider.get_write_frame(ctx, &frame_buffer, sc_error, sizeof(sc_error));
            if (rc != 0)
                ThrowSceneChangeError(sc_error, "Failed to get SceneChange provider write buffer.");

            lsmas_provider::ErrorString error;
            lsmas_video_frame_buffer_layout_t layout = {};
            if (sc.provider.input_pixel_format == SCENECHANGE_PROVIDER_PIXEL_FORMAT_YUV420P8) {
                auto const expected_layout = ValidateI420WriteFrame(frame_buffer, info);
                int64_t bytes = lsm.video_get_frame(
                    handle,
                    frame_index,
                    LSMAS_VIDEO_FRAME_OUTPUT_YUV420P8,
                    nullptr,
                    frame_buffer.planes[0].stride,
                    &layout,
                    error.Out());
                if (bytes <= 0)
                    throw VideoDecodeError(error.Message("failed to query YUV420P8 frame layout for SceneChange"));
                ValidateLsmasI420Layout(layout, bytes, frame_buffer, expected_layout, info);

                error.Reset();
                layout = {};
                bytes = lsm.video_get_frame(
                    handle,
                    frame_index,
                    LSMAS_VIDEO_FRAME_OUTPUT_YUV420P8,
                    frame_buffer.data,
                    frame_buffer.planes[0].stride,
                    &layout,
                    error.Out());
                if (bytes <= 0)
                    throw VideoDecodeError(error.Message("failed to decode YUV420P8 frame for SceneChange"));
                ValidateLsmasI420Layout(layout, bytes, frame_buffer, expected_layout, info);
            }
            else if (sc.provider.input_pixel_format == SCENECHANGE_PROVIDER_PIXEL_FORMAT_GRAY8_PADDED16) {
                ValidateGray8WriteFrame(frame_buffer, info);
                int64_t const bytes = lsm.video_get_frame(
                    handle,
                    frame_index,
                    LSMAS_VIDEO_FRAME_OUTPUT_GRAY8_PADDED16,
                    frame_buffer.data,
                    frame_buffer.planes[0].stride,
                    &layout,
                    error.Out());
                if (bytes <= 0)
                    throw VideoDecodeError(error.Message("failed to decode Gray8Padded16 frame for SceneChange"));
                if (layout.output_format != LSMAS_VIDEO_FRAME_OUTPUT_GRAY8_PADDED16 ||
                    layout.width != info.width ||
                    layout.height != info.height ||
                    layout.plane_count != 1 ||
                    layout.plane_stride[0] != frame_buffer.planes[0].stride ||
                    layout.plane_width[0] != frame_buffer.planes[0].width ||
                    layout.plane_height[0] != frame_buffer.planes[0].height ||
                    layout.plane_offset[0] != 0 ||
                    layout.required_bytes != bytes ||
                    static_cast<uint64_t>(bytes) != static_cast<uint64_t>(frame_buffer.data_size))
                    throw VideoDecodeError("LsmasNative returned an incompatible Gray8Padded16 frame layout for SceneChange.");
            }
            else {
                throw VideoProviderError("SceneChange provider selected an unsupported input pixel format.");
            }

            scenechange_provider_result frame_result {};
            frame_result.struct_size = SCENECHANGE_PROVIDER_RESULT_V1_SIZE;
            rc = sc.provider.commit_written_frame(ctx, &frame_result, sc_error, sizeof(sc_error));
            if (rc == kSceneChangeCanceled)
                throw agi::UserCancelException("SceneChange keyframe generation canceled by user.");
            if (rc != 0)
                ThrowSceneChangeError(sc_error, "SceneChange provider failed while analyzing a frame.");
            if (frame_result.struct_size != SCENECHANGE_PROVIDER_RESULT_V1_SIZE ||
                frame_result.frame_index != frame_index ||
                (frame_result.is_scene_change != 0 && frame_result.is_scene_change != 1))
                throw VideoProviderError("SceneChange provider returned an invalid frame result.");
            if (frame_result.is_scene_change)
                result.push_back(frame_index);
        }
    }
    return result;
}
#endif

bool LsmasVideoProvider::SetOutputMode(SourceFrameOutputMode mode) {
    if (mode != SourceFrameOutputMode::Native && mode != SourceFrameOutputMode::Bgra8)
        return false;
    output_mode = mode;
    return true;
}

#ifdef WITH_SCENECHANGE
std::optional<scenechange::KeyframeRequest> LsmasVideoProvider::PrepareSceneChangeKeyframes() const {
	auto const selection = SelectSceneChangeBackend(info);
    if (selection.status != scenechange::BackendSelectionStatus::Selected)
        return {};
	return scenechange::KeyframeRequest(selection.selected);
}

void LsmasVideoProvider::GenerateSceneChangeKeyframes(scenechange::KeyframeRequest const& request,
													  agi::fs::path const& output_path, agi::ProgressSink *ps) {
	if (output_path.empty())
        throw VideoProviderError("SceneChange keyframe output path is empty.");
    if (!handle)
        throw VideoProviderError("LsmasNative handle is not open.");
    if (info.width <= 0 || info.height <= 0 || info.num_frames <= 0)
        throw VideoProviderError("LsmasNative returned invalid video dimensions for SceneChange.");

	auto generated = ScanSceneChangeKeyframes(handle, info, request.GetApi(), ps);
	agi::keyframe::Save(output_path, generated);
    LOG_I("provider/lsmasnative/scenechange")
        << "Generated " << generated.size() << " SceneChange keyframes: "
        << agi::fs::PathToString(output_path);
}
#endif

void LsmasVideoProvider::GetFrame(int n, VideoFrame &frame) {
    auto const& api = lsmas::GetApi();
    n = std::clamp(n, 0, info.num_frames - 1);

    lsmas_provider::ErrorString error;
    lsmas_video_frame_buffer_layout_t layout = {};
    int64_t required = api.video_get_frame(handle, n, LSMAS_VIDEO_FRAME_OUTPUT_BGRA, nullptr, 0, &layout, error.Out());
    if (required < 0)
        throw VideoDecodeError(error.Message("failed to decode BGRA frame"));

    if (layout.width <= 0 || layout.height <= 0 || layout.plane_count != 1 || layout.plane_stride[0] <= 0 || required <= 0)
        throw VideoDecodeError("invalid BGRA frame layout returned by LsmasNative");
    if (static_cast<uint64_t>(required) > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        throw VideoDecodeError("BGRA frame is too large");

    auto const pitch = static_cast<size_t>(layout.plane_stride[0]);
    auto const height = static_cast<size_t>(layout.height);
    if (height && pitch > std::numeric_limits<size_t>::max() / height)
        throw VideoDecodeError("BGRA frame dimensions are too large");
    if (static_cast<size_t>(required) < pitch * height)
        throw VideoDecodeError("invalid BGRA frame buffer size returned by LsmasNative");

    frame.width = static_cast<size_t>(layout.width);
    frame.height = height;
    frame.pitch = pitch;
    frame.flipped = false;
    frame.data.resize(static_cast<size_t>(required));

    error.Reset();
    required = api.video_get_frame(handle, n, LSMAS_VIDEO_FRAME_OUTPUT_BGRA, frame.data.data(), layout.plane_stride[0], &layout, error.Out());
    if (required < 0)
        throw VideoDecodeError(error.Message("failed to decode BGRA frame"));
    if (static_cast<uint64_t>(required) > static_cast<uint64_t>(frame.data.size()))
        throw VideoDecodeError("BGRA frame grew during decode");
}

bool LsmasVideoProvider::GetNativeFrame(int n, SourceFrame& out, std::shared_ptr<void>& owner) {
    auto const& api = lsmas::GetApi();
    n = std::clamp(n, 0, info.num_frames - 1);

    lsmas_provider::ErrorString error;
    lsmas_video_frame_t *native = nullptr;
    if (api.video_acquire_avframe(handle, n, &native, error.Out()) < 0)
        throw VideoDecodeError(error.Message("failed to acquire native frame"));

    auto native_owner = std::make_shared<LsmasFrameOwner>(native);
    lsmas_video_frame_props_t props = {};
    if (api.video_frame_get_props(native, &props, error.Out()) < 0)
        throw VideoDecodeError(error.Message("failed to query native frame metadata"));
    lsmas_video_format_info_t fmt = {};
    if (api.video_frame_get_format_info(native, &fmt, error.Out()) < 0)
        throw VideoDecodeError(error.Message("failed to query native frame format"));

    auto format_info = ConvertFormatInfo(fmt);
    out = {};
    out.output_mode = SourceFrameOutputMode::Native;
    out.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, props.pix_fmt };
    out.native_payload_kind = SourceFrameNativePayloadKind::FFmpegAVFrame;
    out.native_payload = api.video_frame_get_avframe(native);
    out.format_info = format_info;
    out.width = props.width;
    out.height = props.height;
    out.flipped = false;
    out.plane_count = format_info.plane_count;
    out.geometry = BuildGeometry(props);
    out.color = ColorMetadata(props);
    out.dolby_vision = BuildDolbyVisionMetadata(api, native);
    out.chroma_location = ChromaLocation(props.chroma_location);

    for (int i = 0; i < out.plane_count; ++i) {
        const uint8_t *data = nullptr;
        int32_t stride = 0;
        int32_t width = 0;
        int32_t height = 0;
        if (api.video_frame_get_plane(native, i, &data, &stride, &width, &height, error.Out()) < 0)
            throw VideoDecodeError(error.Message("failed to query native frame plane"));
        out.planes[static_cast<size_t>(i)] = { data, stride, width, height };
    }

    owner = native_owner;
    return true;
}
}

std::unique_ptr<VideoProvider> CreateLsmasNativeVideoProvider(agi::fs::path const& path, std::string const& colormatrix, agi::BackgroundRunner *br, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) {
    return agi::make_unique<LsmasVideoProvider>(path, colormatrix, br, std::move(choice_sink));
}

#endif
