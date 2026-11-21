#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/ass_info.h"
#include "../../src/ass_io_core.h"
#include "../../src/ass_style.h"
#include "../../src/font_face_selection.h"
#include "../../src/libass_runtime.h"
#include "../../src/motion_track/apply_source.h"
#include "../../src/perspective_apply_plan.h"
#include "../../src/perspective_ass_state.h"
#include "../../src/perspective_quad_edit_state.h"
#include "../../src/perspective_tag_solver.h"

#include <gtest/gtest.h>

#include <ass/ass.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <tuple>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using perspective::BoundsKind;
using perspective::ForwardInput;
using perspective::MakeHomography;
using perspective::Quad;
using perspective::Rect;
using perspective::Resolution;
using perspective::SolvePerspectiveTags;
using perspective::Vec2;

constexpr int RenderWidth = 640;
constexpr int RenderHeight = 360;

struct RenderedMask {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> alpha;

    unsigned char At(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height)
            return 0;
        return alpha[static_cast<std::size_t>(y) * width + x];
    }
};

std::string StyleLine(int scale_x = 100, int scale_y = 100) {
    return
        "Style: Default,Arial,40,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
        "0,0,0,0," + std::to_string(scale_x) + "," + std::to_string(scale_y)
        + ",0,0,1,0,0,7,0,0,0,1";
}

std::string AssHeader(int scale_x = 100, int scale_y = 100) {
    return std::string(
        "[Script Info]\n"
        "ScriptType: v4.00+\n"
        "PlayResX: 640\n"
        "PlayResY: 360\n"
        "WrapStyle: 2\n"
        "ScaledBorderAndShadow: yes\n"
        "\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
            "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
            "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
            "Alignment, MarginL, MarginR, MarginV, Encoding\n")
        + StyleLine(scale_x, scale_y) + "\n"
        "\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
            "Effect, Text\n";
}

std::string MakeScript(
    std::string const& event_text,
    int scale_x = 100,
    int scale_y = 100) {
    return AssHeader(scale_x, scale_y)
        + "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,"
        + event_text + "\n";
}

std::string MakeTransformTags(
    perspective::SerializedTransformState const& serialized,
    std::string_view geometry_mode) {
    std::string result = "{\\an7\\pos" + serialized.position;
    if (serialized.origin)
        result += "\\org" + *serialized.origin;
    result += "\\fscx" + serialized.scale_x;
    result += "\\fscy" + serialized.scale_y;
    result += "\\fax" + serialized.shear_x;
    result += "\\fay" + serialized.shear_y;
    result += "\\frx" + serialized.rotation_x;
    result += "\\fry" + serialized.rotation_y;
    result += "\\frz" + serialized.rotation_z;
    result += geometry_mode;
    result += "}";
    return result;
}

RenderedMask Render(
    ASS_Library* library, ASS_Renderer* renderer, std::string script,
    long long now_ms = 0) {
    auto const& api = libass::runtime::GetApi();
    auto *mutable_data = script.data();
    auto *track = api.ass_read_memory(
        library, mutable_data, script.size(), nullptr);
    EXPECT_NE(nullptr, track);
    if (!track)
        return {};

    api.ass_set_frame_size(renderer, RenderWidth, RenderHeight);
    api.ass_set_storage_size(renderer, RenderWidth, RenderHeight);
    auto *images = api.ass_render_frame(renderer, track, now_ms, nullptr);

    RenderedMask result;
    result.width = RenderWidth;
    result.height = RenderHeight;
    result.alpha.assign(static_cast<std::size_t>(RenderWidth) * RenderHeight, 0);
    for (auto *image = images; image; image = image->next) {
        for (int y = 0; y < image->h; ++y) {
            for (int x = 0; x < image->w; ++x) {
                int const dst_x = image->dst_x + x;
                int const dst_y = image->dst_y + y;
                if (dst_x < 0 || dst_y < 0
                    || dst_x >= RenderWidth || dst_y >= RenderHeight)
                    continue;
                auto const source_alpha = image->bitmap[
                    static_cast<std::ptrdiff_t>(y) * image->stride + x];
                auto& destination = result.alpha[
                    static_cast<std::size_t>(dst_y) * RenderWidth + dst_x];
                destination = std::max(destination, source_alpha);
            }
        }
    }
    api.ass_free_track(track);
    return result;
}

struct MaskStats {
    std::size_t opaque_pixels = 0;
    double alpha_area = 0.0;
    Vec2 centroid;
};

struct MaskBounds {
    int left = RenderWidth;
    int top = RenderHeight;
    int right = 0;
    int bottom = 0;

    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
    bool Empty() const { return left >= right || top >= bottom; }
};

MaskBounds GetMaskBounds(RenderedMask const& mask, unsigned char threshold = 16) {
    MaskBounds result;
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) {
            if (mask.At(x, y) < threshold)
                continue;
            result.left = std::min(result.left, x);
            result.top = std::min(result.top, y);
            result.right = std::max(result.right, x + 1);
            result.bottom = std::max(result.bottom, y + 1);
        }
    }
    return result;
}

#ifdef _WIN32
std::pair<double, double> GetGdiExtent(
    std::string_view font, std::wstring_view text = L"\u4e2d") {
    auto *dc = CreateCompatibleDC(nullptr);
    EXPECT_NE(nullptr, dc);
    if (!dc)
        return {};
    LOGFONTW descriptor {};
    descriptor.lfHeight = 40 * 64;
    descriptor.lfWeight = FW_NORMAL;
    descriptor.lfOutPrecision = OUT_TT_PRECIS;
    descriptor.lfQuality = ANTIALIASED_QUALITY;
    auto const wide_font = std::wstring(font.begin(), font.end());
    wcsncpy_s(descriptor.lfFaceName, wide_font.c_str(), 31);
    auto *selected_font = CreateFontIndirectW(&descriptor);
    EXPECT_NE(nullptr, selected_font);
    if (!selected_font) {
        DeleteDC(dc);
        return {};
    }
    auto *old_font = SelectObject(dc, selected_font);
    SIZE size {};
    EXPECT_TRUE(GetTextExtentPoint32W(
        dc, text.data(), static_cast<int>(text.size()), &size));
    SelectObject(dc, old_font);
    DeleteObject(selected_font);
    DeleteDC(dc);
    return {size.cx / 64.0, size.cy / 64.0};
}

bool VerticalCjkTextExtents(
    AssStyle*, std::string const&,
    double& width, double& height,
    double& descent, double& external_leading) {
    auto const extent = GetGdiExtent(
        "@Microsoft YaHei",
        L"\u5929\u5730\u3001\u3002\uff01\uff1f\uff08\uff09\u300c\u300d");
    width = extent.first;
    height = extent.second;
    descent = 0.0;
    external_leading = 0.0;
    return width > 0.0 && height > 0.0;
}

bool ArialTextExtents(
    AssStyle*, std::string const& text,
    double& width, double& height,
    double& descent, double& external_leading) {
    auto const extent = GetGdiExtent(
        "Arial", std::wstring(text.begin(), text.end()));
    width = extent.first;
    height = extent.second;
    descent = 0.0;
    external_leading = 0.0;
    return width > 0.0 && height > 0.0;
}
#endif

MaskStats GetStats(RenderedMask const& mask, unsigned char threshold = 16) {
    MaskStats result;
    double weighted_x = 0.0;
    double weighted_y = 0.0;
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) {
            auto const alpha = mask.At(x, y);
            if (alpha >= threshold)
                ++result.opaque_pixels;
            double const weight = static_cast<double>(alpha) / 255.0;
            result.alpha_area += weight;
            weighted_x += (x + 0.5) * weight;
            weighted_y += (y + 0.5) * weight;
        }
    }
    if (result.alpha_area > 0.0)
        result.centroid = {
            weighted_x / result.alpha_area,
            weighted_y / result.alpha_area};
    return result;
}

double SignedDistanceOutside(Vec2 point, Quad const& quad) {
    double minimum = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < quad.size(); ++index) {
        auto const start = quad[index];
        auto const end = quad[(index + 1) % quad.size()];
        auto const edge = end - start;
        auto const length = std::hypot(edge.x, edge.y);
        if (length <= 0.0)
            return std::numeric_limits<double>::infinity();
        minimum = std::min(
            minimum,
            edge.Cross(point - start) / length);
    }
    return std::max(0.0, -minimum);
}

double MaxMaskDistanceOutside(RenderedMask const& mask, Quad const& target) {
    double result = 0.0;
    for (int y = 0; y < mask.height; ++y) {
        for (int x = 0; x < mask.width; ++x) {
            if (mask.At(x, y) < 16)
                continue;
            result = std::max(
                result,
                SignedDistanceOutside({x + 0.5, y + 0.5}, target));
        }
    }
    return result;
}

double PolygonArea(Quad const& quad) {
    double area = 0.0;
    for (std::size_t index = 0; index < quad.size(); ++index)
        area += quad[index].Cross(quad[(index + 1) % quad.size()]);
    return std::abs(area) / 2.0;
}

ForwardInput MakeSourceInput() {
    ForwardInput source;
    source.play_resolution = {640.0, 360.0};
    source.bounds = {
        Rect {0.0, 0.0, 400.0, 60.0},
        BoundsKind::Drawing,
        Resolution {400.0, 60.0},
        {0.0, 0.0},
        {}};
    source.state.alignment = 7;
    source.state.position = {80.0, 80.0};
    return source;
}

struct LibassSession {
    ASS_Library* library = nullptr;
    ASS_Renderer* renderer = nullptr;

    ~LibassSession() {
        if (!library && !renderer)
            return;
        auto const& api = libass::runtime::GetApi();
        if (renderer)
            api.ass_renderer_done(renderer);
        if (library)
            api.ass_library_done(library);
    }
};

LibassSession StartLibass() {
    LibassSession result;
    auto const& api = libass::runtime::GetApi();
    result.library = api.ass_library_init();
    if (!result.library)
        return result;
    api.ass_set_extract_fonts(result.library, 0);
    result.renderer = api.ass_renderer_init(result.library);
    if (!result.renderer)
        return result;
    api.ass_set_font_scale(result.renderer, 1.0);
    api.ass_set_fonts(result.renderer, nullptr, "Sans", 1, nullptr, true);
    return result;
}

}

TEST(font_face_selection, scale_writer_preserves_the_libass_animation_target) {
	if (!libass::runtime::IsAvailable())
		GTEST_SKIP() << libass::runtime::GetLoadError();
	auto const session = StartLibass();
	ASSERT_NE(nullptr, session.renderer);
	AssDialogue line;
	line.Text = R"({\t(0,1000,\fscx200)}TEST)";
	AssStyle const style;
	auto const edit = CaptureFontOverrideEdit(aegisub::subtitle_edit_ops::ParsedLine(&line), 0, 2, style, {});
	FontOverrideValues values;
	values[FontOverrideIndex(FontOverrideField::ScaleX)] = "110";
	auto const actual = edit.Apply(line.Text.get(), values, 2, 2).text;
	std::string const expected = R"({\fscx110\t(0,1000,\fscx200)}TEST)";
	int last_width = 0;
	for (int const time : {0, 500, 1200}) {
		SCOPED_TRACE(time);
		auto const observed = Render(session.library, session.renderer, MakeScript(actual), time);
		auto const reference = Render(session.library, session.renderer, MakeScript(expected), time);
		EXPECT_TRUE(observed.alpha == reference.alpha);
		auto const bounds = GetMaskBounds(observed);
		int const width = bounds.right - bounds.left;
		EXPECT_GT(width, last_width);
		last_width = width;
	}
	auto const final = Render(session.library, session.renderer, MakeScript(actual), 1200);
	auto const target = Render(session.library, session.renderer, MakeScript(R"({\fscx200}TEST)"), 1200);
	EXPECT_TRUE(final.alpha == target.alpha);
}

TEST(font_face_selection, libass_distinguishes_empty_invalid_and_prefixed_geometry_arguments) {
	if (!libass::runtime::IsAvailable())
		GTEST_SKIP() << libass::runtime::GetLoadError();
	auto const session = StartLibass();
	ASSERT_NE(nullptr, session.renderer);
	for (auto const& [input, expected] : {
			 std::pair{R"({\fscx80\fscx\fsp20\fsp}TEST)", "TEST"},
			 std::pair{R"({\fscxabc}TEST)", R"({\fscx0}TEST)"},
			 std::pair{R"({\fspabc}TEST)", R"({\fsp0}TEST)"},
			 std::pair{R"({\fscx110.25tail\fsp-1.5px}TEST)", R"({\fscx110.25\fsp-1.5}TEST)"},
		 }) {
		SCOPED_TRACE(input);
		auto const observed = Render(session.library, session.renderer, MakeScript(input, 120));
		auto const reference = Render(session.library, session.renderer, MakeScript(expected, 120));
		EXPECT_TRUE(observed.alpha == reference.alpha);
	}
}

TEST(perspective_libass_render, generated_tags_match_hand_drawn_quad_for_vector_and_text) {
    if (!libass::runtime::IsAvailable())
        GTEST_SKIP() << "libass runtime is unavailable: "
            << libass::runtime::GetLoadError();

    auto session = StartLibass();
    ASSERT_NE(nullptr, session.library);
    ASSERT_NE(nullptr, session.renderer);

    auto const source = MakeSourceInput();
    Quad const target {{{140.0, 110.0}, {500.0, 90.0},
        {470.0, 250.0}, {120.0, 235.0}}};
    auto const solved = SolvePerspectiveTags({
        source, target, {1.0, 1.0}, 0.1});
    ASSERT_TRUE(solved) << perspective::DescribeSolverError(solved.error);

    auto const marker_tags = MakeTransformTags(
        solved.candidate->serialized, "\\p1");
    auto const marker = Render(
        session.library,
        session.renderer,
        MakeScript(marker_tags + "m 0 0 l 400 0 l 400 60 l 0 60"));
    auto const marker_stats = GetStats(marker);
    ASSERT_GT(marker_stats.alpha_area, 1000.0);
    EXPECT_LT(MaxMaskDistanceOutside(marker, target), 3.0);
    EXPECT_NEAR(
        marker_stats.alpha_area,
        PolygonArea(target),
        1500.0);

    auto const source_text = Render(
        session.library,
        session.renderer,
        MakeScript("{\\an7\\pos(80,80)}Perspective"));
    auto const rewritten = perspective::RewritePerspectiveTags(
        "{\\an7\\pos(80,80)}Perspective",
        source.state,
        source.state,
        *solved.candidate);
    ASSERT_TRUE(rewritten) << perspective::DescribeRewriteError(rewritten.error);
    ASSERT_TRUE(rewritten.changed);
    auto const transformed_text = Render(
        session.library,
        session.renderer,
        MakeScript(rewritten.text));
    auto const source_stats = GetStats(source_text);
    auto const transformed_stats = GetStats(transformed_text);
    ASSERT_GT(source_stats.alpha_area, 10.0);
    ASSERT_GT(transformed_stats.alpha_area, 10.0);
    EXPECT_LT(MaxMaskDistanceOutside(transformed_text, target), 5.0);

    auto const expected_homography = MakeHomography(source.bounds.rectangle, target);
    ASSERT_TRUE(expected_homography);
    auto const source_local_centroid = source_stats.centroid - source.state.position;
    auto const expected_centroid = expected_homography.value.Map(source_local_centroid);
    ASSERT_TRUE(expected_centroid);
    // A projective warp changes the alpha weighting across a glyph, so its
    // raster centroid is not exactly the homography of the source centroid.
    EXPECT_NEAR(transformed_stats.centroid.x, expected_centroid->x, 8.0);
    EXPECT_NEAR(transformed_stats.centroid.y, expected_centroid->y, 8.0);
}

// The review's repro, rendered: a nested tag written with a space after its
// backslash animates for libass exactly like the canonical spelling (the
// review measured identical masks and a 11967 px gap to the static drawing).
// The evaluator used to parse the nested body space-blind, so it saw a static
// line where the renderer animated one.
TEST(perspective_libass_render, spaced_nested_animation_renders_like_the_canonical_form) {
    if (!libass::runtime::IsAvailable())
        GTEST_SKIP() << "libass runtime is unavailable: "
            << libass::runtime::GetLoadError();

    auto session = StartLibass();
    ASSERT_NE(nullptr, session.library);
    ASSERT_NE(nullptr, session.renderer);

    std::string const prefix = "\\an7\\pos(200,200)";
    std::string const drawing = "m 0 0 l 200 0 200 80 0 80";
    std::string const spaced =
        "{" + prefix + "\\t(0,1000,\\ frz30)\\p1}" + drawing;
    std::string const canonical =
        "{" + prefix + "\\t(0,1000,\\frz30)\\p1}" + drawing;
    // A space before the argument region rides along with the tag name in
    // the scanner; libass matches the tag by prefix and renders it exactly
    // like the canonical spelling.
    std::string const spaced_paren =
        "{" + prefix + "\\t (0,1000,\\ frz30)\\p1}" + drawing;
    auto const still = Render(
        session.library, session.renderer,
        MakeScript("{" + prefix + "\\p1}" + drawing));
    auto const canonical_mask = Render(
        session.library, session.renderer, MakeScript(canonical), 2000);
    auto const spaced_mask = Render(
        session.library, session.renderer, MakeScript(spaced), 2000);
    auto const spaced_paren_mask = Render(
        session.library, session.renderer, MakeScript(spaced_paren), 2000);

    ASSERT_GT(GetStats(still).alpha_area, 100.0);
    ASSERT_GT(GetStats(canonical_mask).alpha_area, 100.0);
    ASSERT_EQ(spaced_mask.alpha.size(), canonical_mask.alpha.size());
    ASSERT_GT(GetStats(spaced_mask).alpha_area, 100.0);
    ASSERT_GT(GetStats(spaced_paren_mask).alpha_area, 100.0);
    std::size_t spaced_vs_canonical = 0;
    std::size_t spaced_paren_vs_canonical = 0;
    std::size_t spaced_vs_still = 0;
    for (std::size_t index = 0; index < spaced_mask.alpha.size(); ++index) {
        if (spaced_mask.alpha[index] != canonical_mask.alpha[index])
            ++spaced_vs_canonical;
        if (spaced_paren_mask.alpha[index] != canonical_mask.alpha[index])
            ++spaced_paren_vs_canonical;
        if (std::abs(static_cast<int>(spaced_mask.alpha[index])
                - static_cast<int>(still.alpha[index])) > 16)
            ++spaced_vs_still;
    }
    EXPECT_EQ(0u, spaced_vs_canonical);
    EXPECT_EQ(0u, spaced_paren_vs_canonical);
    EXPECT_GT(spaced_vs_still, 1000u);
}

TEST(perspective_libass_render, ordinary_text_minimal_style_tags_render_target) {
#ifndef _WIN32
    GTEST_SKIP() << "the renderer-compatible Arial extent provider is Windows-specific";
#else
    if (!libass::runtime::IsAvailable())
        GTEST_SKIP() << "libass runtime is unavailable: "
            << libass::runtime::GetLoadError();

    auto session = StartLibass();
    ASSERT_NE(nullptr, session.library);
    ASSERT_NE(nullptr, session.renderer);

    constexpr int StyleScaleX = 125;
    constexpr int StyleScaleY = 90;
    auto const original = std::string(
        "{\\an7\\pos(90,80)\\fscx100\\fscy100}Perspective");
    AssFile file;
    file.Info.emplace_back("PlayResX", "640");
    file.Info.emplace_back("PlayResY", "360");
    file.Info.emplace_back("WrapStyle", "2");
    file.Styles.push_back(*new AssStyle(StyleLine(StyleScaleX, StyleScaleY)));
    auto *line = new AssDialogue;
    line->Start = 0;
    line->End = 10000;
    line->Text = original;
    file.Events.push_back(*line);

    perspective::PerspectiveApplyContext context;
    context.frame_number = 0;
    context.capture_time_ms = 0;
    context.play_resolution = {640.0, 360.0};
    context.video_storage_resolution = Resolution {640.0, 360.0};
    context.output_mapping = {1.0, 1.0};
    auto const capture = perspective::CapturePerspectiveSource(
        file, *line, context, ArialTextExtents);
    ASSERT_TRUE(capture)
        << perspective::DescribePerspectivePlanError(capture.error);
    ASSERT_TRUE(capture.source->current_quad);
    EXPECT_DOUBLE_EQ(100.0, capture.source->state.transform.scale_x);
    EXPECT_DOUBLE_EQ(100.0, capture.source->state.transform.scale_y);
    EXPECT_DOUBLE_EQ(
        static_cast<double>(StyleScaleX),
        capture.source->state.event_style_transform.scale_x);
    EXPECT_DOUBLE_EQ(
        static_cast<double>(StyleScaleY),
        capture.source->state.event_style_transform.scale_y);

    auto target_input = capture.source->forward_input;
    target_input.state.position = {185.0, 105.0};
    target_input.state.origin.reset();
    target_input.state.scale_x = StyleScaleX;
    target_input.state.scale_y = StyleScaleY;
    target_input.state.shear_x = 0.16;
    target_input.state.shear_y = 0.0;
    target_input.state.rotation_x = 12.0;
    target_input.state.rotation_y = -18.0;
    target_input.state.rotation_z = 7.0;
    auto const target_forward = perspective::ForwardQuad(target_input);
    ASSERT_TRUE(target_forward)
        << perspective::DescribeForwardError(target_forward.error);
    auto const target = target_forward.quad;
    auto const top_edge = target[1] - target[0];
    auto const bottom_edge = target[2] - target[3];
    EXPECT_GT(std::abs(top_edge.Cross(bottom_edge)), 1.0);

    auto const planned = perspective::BuildPerspectiveMutationPlan(
        file, context, *capture.source, target, 0.1, ArialTextExtents);
    ASSERT_TRUE(planned)
        << perspective::DescribePerspectivePlanError(planned.error);
    EXPECT_FALSE(planned.plan->Candidate().state.origin);
    EXPECT_FALSE(planned.plan->Candidate().serialized.origin);
    EXPECT_NE(
        perspective::CandidateFamily::ProjectiveExplicitOrigin,
        planned.plan->Family());
    EXPECT_LE(planned.plan->MaxError(), 0.1);

    auto const& rewritten = planned.plan->ReplacementText();
    SCOPED_TRACE("rewritten ASS event: " + rewritten);
    EXPECT_EQ(std::string::npos, rewritten.find("\\org"));
    EXPECT_EQ(std::string::npos, rewritten.find("\\fscx"));
    EXPECT_EQ(std::string::npos, rewritten.find("\\fscy"));
    EXPECT_NE(std::string::npos, rewritten.find("\\frx"));
    EXPECT_NE(std::string::npos, rewritten.find("\\fry"));
    EXPECT_NE(std::string::npos, rewritten.find("Perspective"));

    auto const source_rendered = Render(
        session.library,
        session.renderer,
        MakeScript(original, StyleScaleX, StyleScaleY));
    auto const transformed_rendered = Render(
        session.library,
        session.renderer,
        MakeScript(rewritten, StyleScaleX, StyleScaleY));
    auto const source_stats = GetStats(source_rendered);
    auto const transformed_stats = GetStats(transformed_rendered);
    ASSERT_GT(source_stats.alpha_area, 100.0);
    ASSERT_GT(transformed_stats.alpha_area, 100.0);
    EXPECT_LT(MaxMaskDistanceOutside(transformed_rendered, target), 5.0);
    EXPECT_GT(std::hypot(
        transformed_stats.centroid.x - source_stats.centroid.x,
        transformed_stats.centroid.y - source_stats.centroid.y), 50.0);
#endif
}

TEST(perspective_libass_render, vertical_cjk_perspective_tags_render_inside_target) {
#ifndef _WIN32
    GTEST_SKIP() << "the @ CJK vertical-face contract is Windows-specific";
#else
    if (!libass::runtime::IsAvailable())
        GTEST_SKIP() << "libass runtime is unavailable: "
            << libass::runtime::GetLoadError();

    auto session = StartLibass();
    ASSERT_NE(nullptr, session.library);
    ASSERT_NE(nullptr, session.renderer);

    auto const text = "\xE5\xA4\xA9\xE5\x9C\xB0\xE3\x80\x81\xE3\x80\x82"
        "\xEF\xBC\x81\xEF\xBC\x9F\xEF\xBC\x88\xEF\xBC\x89"
        "\xE3\x80\x8C\xE3\x80\x8D";
    auto const original = std::string(
        "{\\an7\\pos(100,30)\\frz270\\fn@Microsoft YaHei\\fs40}") + text;
    AssFile file;
    file.Info.emplace_back("PlayResX", "640");
    file.Info.emplace_back("PlayResY", "360");
    file.Info.emplace_back("WrapStyle", "2");
    file.Styles.push_back(*new AssStyle(
        "Style: Default,Arial,40,&H00FFFFFF,&H00FFFFFF,&H00000000,"
        "&H00000000,0,0,0,0,100,100,0,0,1,0,0,7,0,0,0,1"));
    auto *line = new AssDialogue;
    line->Start = 0;
    line->End = 10000;
    line->Text = original;
    file.Events.push_back(*line);

    perspective::PerspectiveApplyContext context;
    context.frame_number = 0;
    context.capture_time_ms = 0;
    context.play_resolution = {640.0, 360.0};
    context.video_storage_resolution = Resolution {640.0, 360.0};
    context.output_mapping = {1.0, 1.0};
    auto const capture = perspective::CapturePerspectiveSource(
        file, *line, context, VerticalCjkTextExtents);
    ASSERT_TRUE(capture)
        << perspective::DescribePerspectivePlanError(capture.error);
    ASSERT_TRUE(capture.source->current_quad);

    auto target = perspective::PerspectiveQuadFromOppositeCorners(
        {165.0, 25.0}, {270.0, 325.0},
        perspective::PerspectiveFirstEdgeDirection(*capture.source));
    target[1].x -= 20.0;
    target[2].y -= 25.0;
    target[3] = target[3] + Vec2 {15.0, 20.0};
    auto const planned = perspective::BuildPerspectiveMutationPlan(
        file, context, *capture.source, target, 0.1, VerticalCjkTextExtents);
    ASSERT_TRUE(planned)
        << perspective::DescribePerspectivePlanError(planned.error);
    EXPECT_EQ("270", planned.plan->Candidate().serialized.rotation_z);

    auto const source_rendered = Render(
        session.library, session.renderer, MakeScript(original));
    auto const source_bounds = GetMaskBounds(source_rendered);
    ASSERT_GT(GetStats(source_rendered).alpha_area, 100.0);
    EXPECT_LT(source_bounds.Width(), source_bounds.Height());

    auto const& rewritten = planned.plan->ReplacementText();
    EXPECT_NE(std::string::npos, rewritten.find("\\frz270"));

    auto const rendered = Render(
        session.library, session.renderer, MakeScript(rewritten));
    auto const stats = GetStats(rendered);
    ASSERT_GT(stats.alpha_area, 100.0);
    EXPECT_LT(MaxMaskDistanceOutside(rendered, target), 10.0);
    auto const rendered_bounds = GetMaskBounds(rendered);
    EXPECT_LT(rendered_bounds.Width(), rendered_bounds.Height());
#endif
}

namespace {
using namespace aegisub::motion_track;
using RasterMotionCase = std::tuple<TrackModel, ApplyMode, bool>;

constexpr unsigned char RasterForeground = 16;
constexpr int RasterRadius = 2;
constexpr double RasterInteriorCoverage = 0.995;

std::string RasterCaseName(RasterMotionCase const& value) {
	return std::string(std::get<0>(value) == TrackModel::Affine ? "Affine" : "Homography") + (std::get<1>(value) == ApplyMode::Exact ? "Exact" : "Compact") + (std::get<2>(value) ? "InverseClip" : "NormalClip");
}

std::string RasterPolygon(std::vector<Vec2> const& points,
						  std::array<double, 9> const& matrix) {
	std::ostringstream drawing;
	drawing << std::setprecision(17);
	for (size_t i = 0; i < points.size(); ++i) {
		auto const [x, y] = points[i];
		double const w = matrix[6] * x + matrix[7] * y + matrix[8];
		drawing << (i == 0 ? "m " : i == 1 ? " l "
										   : " ")
				<< (matrix[0] * x + matrix[1] * y + matrix[2]) / w << ' '
				<< (matrix[3] * x + matrix[4] * y + matrix[5]) / w;
	}
	return drawing.str();
}

std::string RasterEvent(std::vector<Vec2> const& shape,
						std::vector<Vec2> const& clip,
						std::array<double, 9> const& matrix, bool inverse) {
	return std::string(R"({\an7\pos(0,0)\bord0\shad0\)") + (inverse ? "iclip(" : "clip(") + RasterPolygon(clip, matrix) + ")\\p1}" + RasterPolygon(shape, matrix);
}

void WriteRasterText(std::filesystem::path const& path, std::string const& value) {
	std::ofstream stream(path, std::ios::binary);
	ASSERT_TRUE(stream.is_open()) << path.generic_string();
	stream << value;
	stream.close();
	ASSERT_FALSE(stream.fail()) << path.generic_string();
}

std::string ReadRasterText(std::filesystem::path const& path) {
	std::ifstream stream(path, std::ios::binary);
	EXPECT_TRUE(stream.is_open()) << path.generic_string();
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void WriteRasterMask(std::filesystem::path const& path, RenderedMask const& mask) {
	std::ofstream stream(path, std::ios::binary);
	ASSERT_TRUE(stream.is_open()) << path.generic_string();
	stream << "P5\n"
		   << mask.width << ' ' << mask.height << "\n255\n";
	stream.write(reinterpret_cast<char const *>(mask.alpha.data()), mask.alpha.size());
	stream.close();
	ASSERT_FALSE(stream.fail()) << path.generic_string();
}

bool RasterContains(RenderedMask const& mask, int x, int y) {
	return mask.At(x, y) >= RasterForeground;
}

std::vector<std::pair<int, int>> RasterBoundary(RenderedMask const& mask) {
	std::vector<std::pair<int, int>> result;
	for (int y = 0; y < mask.height; ++y)
		for (int x = 0; x < mask.width; ++x)
			if (RasterContains(mask, x, y) && (!RasterContains(mask, x - 1, y) || !RasterContains(mask, x + 1, y) || !RasterContains(mask, x, y - 1) || !RasterContains(mask, x, y + 1)))
				result.emplace_back(x, y);
	return result;
}

double RasterBoundaryDistance(std::vector<std::pair<int, int>> const& from,
							  std::vector<std::pair<int, int>> const& to) {
	if (from.empty() || to.empty())
		return std::numeric_limits<double>::infinity();
	double maximum_squared = 0;
	for (auto const& [x, y] : from) {
		double nearest_squared = std::numeric_limits<double>::infinity();
		for (auto const& [other_x, other_y] : to) {
			double const dx = x - other_x, dy = y - other_y;
			nearest_squared = std::min(nearest_squared, dx * dx + dy * dy);
			if (nearest_squared == 0)
				break;
		}
		maximum_squared = std::max(maximum_squared, nearest_squared);
	}
	return std::sqrt(maximum_squared);
}

struct RasterMetrics {
	double actual_to_reference = 0;
	double reference_to_actual = 0;
	size_t actual_pixels = 0;
	size_t reference_pixels = 0;
	size_t eroded_pixels = 0;
	size_t covered_eroded_pixels = 0;
	size_t outside_dilated_pixels = 0;
};

RasterMetrics MeasureRaster(RenderedMask const& actual, RenderedMask const& reference) {
	RasterMetrics result;
	auto const actual_boundary = RasterBoundary(actual);
	auto const reference_boundary = RasterBoundary(reference);
	result.actual_to_reference = RasterBoundaryDistance(actual_boundary, reference_boundary);
	result.reference_to_actual = RasterBoundaryDistance(reference_boundary, actual_boundary);
	for (int y = 0; y < reference.height; ++y) {
		for (int x = 0; x < reference.width; ++x) {
			bool const occupied = RasterContains(actual, x, y);
			bool const expected = RasterContains(reference, x, y);
			result.actual_pixels += occupied;
			result.reference_pixels += expected;
			bool eroded = true, dilated = false;
			for (int dy = -RasterRadius; dy <= RasterRadius; ++dy) {
				for (int dx = -RasterRadius; dx <= RasterRadius; ++dx) {
					if (dx * dx + dy * dy > RasterRadius * RasterRadius)
						continue;
					bool const neighbor = RasterContains(reference, x + dx, y + dy);
					eroded &= neighbor;
					dilated |= neighbor;
				}
			}
			result.eroded_pixels += eroded;
			result.covered_eroded_pixels += eroded && occupied;
			result.outside_dilated_pixels += occupied && !dilated;
		}
	}
	return result;
}

class motion_track_libass_render : public testing::TestWithParam<RasterMotionCase> {};
using TextMotionCase = std::tuple<TrackModel, ApplyMode>;
class motion_track_text_libass_render : public testing::TestWithParam<TextMotionCase> {};

std::string TextCaseName(TextMotionCase const& value) {
	return std::string(std::get<0>(value) == TrackModel::Affine ? "Affine" : "Homography") + (std::get<1>(value) == ApplyMode::Exact ? "Exact" : "Compact");
}
}

TEST_P(motion_track_libass_render, saved_motion_matches_raw_matrix_polygon_reference) {
	if (!libass::runtime::IsAvailable())
		GTEST_SKIP() << libass::runtime::GetLoadError();
	auto session = StartLibass();
	ASSERT_NE(nullptr, session.library);
	ASSERT_NE(nullptr, session.renderer);
	auto const [model, mode, inverse] = GetParam();
	auto const directory = std::filesystem::path("artifacts") / "motion-render-precision" / RasterCaseName(GetParam());
	std::filesystem::create_directories(directory);
	std::vector<Vec2> const shape{{.x = 100, .y = 75}, {.x = 390, .y = 75}, {.x = 390, .y = 225}, {.x = 280, .y = 225}, {.x = 280, .y = 185}, {.x = 100, .y = 185}};
	std::vector<Vec2> const clip = inverse
									   ? std::vector<Vec2>{{.x = 210, .y = 105}, {.x = 310, .y = 115}, {.x = 305, .y = 170}, {.x = 215, .y = 165}}
									   : std::vector<Vec2>{{.x = 140, .y = 60}, {.x = 365, .y = 90}, {.x = 340, .y = 215}, {.x = 120, .y = 200}};
	std::array<double, 9> const identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
	auto const source_script = MakeScript(RasterEvent(shape, clip, identity, inverse));
	auto const source_path = directory / "source.ass";
	WriteRasterText(source_path, source_script);
	AssFile file = ReadAssFileForCore(source_path, "UTF-8");
	ASSERT_EQ(1u, file.Events.size());
	file.Events.front().End = 400;
	WriteAssFileForCore(&file, source_path, agi::vfr::Framerate(10.0), "UTF-8", AssWriteOptions{});
	ApplyPlanInput input;
	input.model = model;
	input.options.mode = mode;
	input.options.position_decimals = 4;
	input.options.compact_epsilon = 0.05;
	input.storage_width = input.script_width = RenderWidth;
	input.storage_height = input.script_height = RenderHeight;
	input.timecodes = agi::vfr::Framerate(10.0);
	input.video_frame_count = 4;
	input.decode_interval = input.direction_domain = {.first = 0, .last = 3};
	for (int frame = 0; frame < 4; ++frame) {
		TrackSample sample;
		sample.frame = frame;
		sample.model = model;
		sample.status = TrackStatus::Ok;
		double const p = model == TrackModel::Homography ? 0.00012 + 0.00003 * frame : 0;
		double const q = model == TrackModel::Homography ? -0.00009 - 0.00002 * frame : 0;
		sample.transform.matrix = {1.03 + 0.015 * frame, 0.09 + 0.015 * frame, 12.0 + 4 * frame,
								   -0.06 - 0.01 * frame, 0.96 + 0.012 * frame, 22.0 + 3 * frame,
								   p, q, 1};
		sample.center_x = sample.transform.matrix[2];
		sample.center_y = sample.transform.matrix[5];
		input.samples.push_back(sample);
	}
	MotionTrackApplySource source;
	source.Capture(file, {&file.Events.front()}, input.timecodes, input.video_frame_count);
	auto const plan = BuildApplyPlan(file, source.Targets(), input);
	ASSERT_TRUE(plan.has_mutations()) << plan.message;
	int commits = 0;
	std::vector<AssDialogue *> selection = source.Targets();
	std::string message;
	ASSERT_TRUE(source.Apply(file, plan, input.timecodes, [&](std::vector<AssDialogue *> const& lines) {
            file.Commit("motion render precision", AssFile::COMMIT_DIAG_ADDREM
                | AssFile::COMMIT_DIAG_TIME | AssFile::COMMIT_DIAG_TEXT);
            selection = lines;
            ++commits; }, message)) << message;
	ASSERT_EQ(1, commits);
	ASSERT_FALSE(selection.empty());
	auto const applied_path = directory / "applied.ass";
	WriteAssFileForCore(&file, applied_path, input.timecodes, "UTF-8", AssWriteOptions{});
	AssFile reopened = ReadAssFileForCore(applied_path, "UTF-8");
	auto const reloaded_path = directory / "reloaded.ass";
	WriteAssFileForCore(&reopened, reloaded_path, input.timecodes, "UTF-8", AssWriteOptions{});
	auto const reloaded_script = ReadRasterText(reloaded_path);
	std::ofstream metrics(directory / "metrics.tsv", std::ios::binary);
	ASSERT_TRUE(metrics.is_open());
	metrics << "frame\ttime_ms\tactive_events\tactual_pixels\treference_pixels"
			   "\tactual_to_reference_px\treference_to_actual_px\teroded_pixels"
			   "\tcovered_eroded_pixels\teroded_coverage\toutside_dilated2_pixels\n";
	for (int frame = 0; frame < 4; ++frame) {
		SCOPED_TRACE(frame);
		int const time = input.timecodes.TimeAtFrame(frame);
		int active = 0;
		for (auto const& line : reopened.Events)
			active += line.Start.GetMillisecond() <= time && time < line.End.GetMillisecond();
		EXPECT_EQ(1, active);
		auto const reference_script = MakeScript(
			RasterEvent(shape, clip, input.samples[frame].transform.matrix, inverse));
		auto const prefix = "frame-" + std::to_string(frame);
		WriteRasterText(directory / (prefix + "-reference.ass"), reference_script);
		auto const actual = Render(session.library, session.renderer, reloaded_script, time);
		auto const reference = Render(session.library, session.renderer, reference_script, time);
		ASSERT_EQ(reference.width, actual.width);
		ASSERT_EQ(reference.height, actual.height);
		auto diff = actual;
		for (size_t i = 0; i < diff.alpha.size(); ++i)
			diff.alpha[i] = static_cast<unsigned char>(std::abs(static_cast<int>(actual.alpha[i]) - static_cast<int>(reference.alpha[i])));
		WriteRasterMask(directory / (prefix + "-actual.pgm"), actual);
		WriteRasterMask(directory / (prefix + "-reference.pgm"), reference);
		WriteRasterMask(directory / (prefix + "-diff.pgm"), diff);
		auto const measured = MeasureRaster(actual, reference);
		double const coverage = measured.eroded_pixels
									? static_cast<double>(measured.covered_eroded_pixels) / measured.eroded_pixels
									: 0;
		metrics << frame << '\t' << time << '\t' << active << '\t' << measured.actual_pixels
				<< '\t' << measured.reference_pixels << '\t' << measured.actual_to_reference
				<< '\t' << measured.reference_to_actual << '\t' << measured.eroded_pixels
				<< '\t' << measured.covered_eroded_pixels << '\t' << std::setprecision(17)
				<< coverage << '\t' << measured.outside_dilated_pixels << '\n';
		metrics.flush();
		ASSERT_TRUE(metrics.good());
		EXPECT_GT(measured.actual_pixels, 100u);
		EXPECT_GT(measured.reference_pixels, 100u);
		EXPECT_GT(measured.eroded_pixels, 100u);
		EXPECT_LE(measured.actual_to_reference, RasterRadius);
		EXPECT_LE(measured.reference_to_actual, RasterRadius);
		EXPECT_GE(coverage, RasterInteriorCoverage);
		EXPECT_EQ(0u, measured.outside_dilated_pixels);
	}
	metrics.close();
	EXPECT_FALSE(metrics.fail());
}


TEST_P(motion_track_text_libass_render, saved_ordinary_text_matches_analytic_ass_reference) {
#ifndef _WIN32
	GTEST_SKIP() << "the renderer-compatible Arial extent provider is Windows-specific";
#else
	if (!libass::runtime::IsAvailable())
		GTEST_SKIP() << libass::runtime::GetLoadError();
	auto session = StartLibass();
	ASSERT_NE(nullptr, session.library);
	ASSERT_NE(nullptr, session.renderer);
	auto const [model, mode] = GetParam();
	auto const directory = std::filesystem::path("artifacts") / "motion-render-precision" / (TextCaseName(GetParam()) + "Text");
	std::filesystem::create_directories(directory);
	auto const source_path = directory / "source.ass";
	WriteRasterText(source_path, MakeScript("{\\an7\\pos(120,90)}Motion"));
	AssFile file = ReadAssFileForCore(source_path, "UTF-8");
	ASSERT_EQ(1u, file.Events.size());
	file.Events.front().End = 300;
	ApplyPlanInput input;
	input.model = model;
	input.options.mode = mode;
	input.options.position_decimals = 4;
	input.options.compact_epsilon = 0.05;
	input.origin_center_x = 120;
	input.origin_center_y = 90;
	input.storage_width = input.script_width = RenderWidth;
	input.storage_height = input.script_height = RenderHeight;
	input.timecodes = agi::vfr::Framerate(10.0);
	input.video_frame_count = 3;
	input.decode_interval = input.direction_domain = {.first = 0, .last = 2};
	input.text_extents = ArialTextExtents;
	WriteAssFileForCore(&file, source_path, input.timecodes, "UTF-8", AssWriteOptions{});
	std::array<std::string, 3> reference_scripts;
	for (int frame = 0; frame < 3; ++frame) {
		double const angle_degrees = 10 + 5 * frame;
		double const tilt_degrees = model == TrackModel::Homography ? 12 + 2 * frame : 0;
		double const scale_x_percent = 110 + 5 * frame;
		double const scale_y_percent = 90 + 2 * frame;
		double const shear = 0.1 + 0.025 * frame;
		double const radians = std::acos(-1.0) / 180;
		double const c = std::cos(angle_degrees * radians), sn = std::sin(angle_degrees * radians);
		double const a = scale_x_percent / 100 * c;
		double const b = scale_x_percent / 100 * shear * c - scale_y_percent / 100 * sn;
		double const d = scale_x_percent / 100 * sn;
		double const e = scale_x_percent / 100 * shear * sn + scale_y_percent / 100 * c;
		double const ct = std::cos(tilt_degrees * radians), st = std::sin(tilt_degrees * radians);
		TrackSample sample;
		sample.frame = frame;
		sample.model = model;
		sample.status = TrackStatus::Ok;
		sample.center_x = 120;
		sample.center_y = 90;
		sample.transform.matrix = {a, b, 0, ct * d, ct * e, 0, st * d / 312.5, st * e / 312.5, 1};
		input.samples.push_back(sample);
		std::ostringstream tags;
		tags << std::setprecision(17) << R"({\an7\pos(120,90)\org(120,90)\fscx)"
			 << scale_x_percent << "\\fscy" << scale_y_percent << "\\fax" << shear
			 << "\\frz" << -angle_degrees << "\\frx" << -tilt_degrees << "}Motion";
		reference_scripts[frame] = MakeScript(tags.str());
	}
	MotionTrackApplySource source;
	source.Capture(file, {&file.Events.front()}, input.timecodes, input.video_frame_count);
	auto const plan = BuildApplyPlan(file, source.Targets(), input);
	ASSERT_TRUE(plan.has_mutations()) << plan.message;
	int commits = 0;
	std::vector<AssDialogue *> selection = source.Targets();
	std::string message;
	ASSERT_TRUE(source.Apply(file, plan, input.timecodes, [&](std::vector<AssDialogue *> const& lines) {
            file.Commit("motion text render precision", AssFile::COMMIT_DIAG_ADDREM
                | AssFile::COMMIT_DIAG_TIME | AssFile::COMMIT_DIAG_TEXT);
            selection = lines;
            ++commits; }, message)) << message;
	ASSERT_EQ(1, commits);
	ASSERT_FALSE(selection.empty());
	auto const applied_path = directory / "applied.ass";
	WriteAssFileForCore(&file, applied_path, input.timecodes, "UTF-8", AssWriteOptions{});
	AssFile reopened = ReadAssFileForCore(applied_path, "UTF-8");
	auto const reloaded_path = directory / "reloaded.ass";
	WriteAssFileForCore(&reopened, reloaded_path, input.timecodes, "UTF-8", AssWriteOptions{});
	auto const reloaded_script = ReadRasterText(reloaded_path);
	std::ofstream metrics(directory / "metrics.tsv", std::ios::binary);
	ASSERT_TRUE(metrics.is_open());
	metrics << "frame\ttime_ms\tactive_events\tactual_pixels\treference_pixels"
			   "\tactual_to_reference_px\treference_to_actual_px\tnormalized_alpha_l1\n";
	for (int frame = 0; frame < 3; ++frame) {
		SCOPED_TRACE(frame);
		int const time = input.timecodes.TimeAtFrame(frame);
		int active = 0;
		for (auto const& line : reopened.Events)
			active += line.Start.GetMillisecond() <= time && time < line.End.GetMillisecond();
		EXPECT_EQ(1, active);
		auto const prefix = "frame-" + std::to_string(frame);
		WriteRasterText(directory / (prefix + "-reference.ass"), reference_scripts[frame]);
		auto const actual = Render(session.library, session.renderer, reloaded_script, time);
		auto const reference = Render(session.library, session.renderer, reference_scripts[frame], time);
		ASSERT_EQ(reference.width, actual.width);
		ASSERT_EQ(reference.height, actual.height);
		auto diff = actual;
		double absolute_difference = 0, reference_alpha = 0;
		for (size_t i = 0; i < diff.alpha.size(); ++i) {
			diff.alpha[i] = static_cast<unsigned char>(std::abs(static_cast<int>(actual.alpha[i]) - static_cast<int>(reference.alpha[i])));
			absolute_difference += diff.alpha[i];
			reference_alpha += reference.alpha[i];
		}
		WriteRasterMask(directory / (prefix + "-actual.pgm"), actual);
		WriteRasterMask(directory / (prefix + "-reference.pgm"), reference);
		WriteRasterMask(directory / (prefix + "-diff.pgm"), diff);
		auto const measured = MeasureRaster(actual, reference);
		double const normalized_alpha_l1 = reference_alpha > 0
											   ? absolute_difference / reference_alpha
											   : std::numeric_limits<double>::infinity();
		metrics << frame << '\t' << time << '\t' << active << '\t' << measured.actual_pixels
				<< '\t' << measured.reference_pixels << '\t' << measured.actual_to_reference
				<< '\t' << measured.reference_to_actual << '\t' << std::setprecision(17)
				<< normalized_alpha_l1 << '\n';
		metrics.flush();
		ASSERT_TRUE(metrics.good());
		EXPECT_GT(measured.actual_pixels, 100u);
		EXPECT_GT(measured.reference_pixels, 100u);
		EXPECT_LE(measured.actual_to_reference, RasterRadius);
		EXPECT_LE(measured.reference_to_actual, RasterRadius);
		EXPECT_LE(normalized_alpha_l1, 0.02);
	}
	metrics.close();
	EXPECT_FALSE(metrics.fail());
#endif
}

INSTANTIATE_TEST_SUITE_P(
	NonTranslation, motion_track_libass_render,
	testing::Combine(testing::Values(TrackModel::Affine, TrackModel::Homography),
					 testing::Values(ApplyMode::Exact, ApplyMode::Compact), testing::Bool()),
	[](testing::TestParamInfo<RasterMotionCase> const& info) { return RasterCaseName(info.param); });

INSTANTIATE_TEST_SUITE_P(
	NonTranslation, motion_track_text_libass_render,
	testing::Combine(testing::Values(TrackModel::Affine, TrackModel::Homography),
					 testing::Values(ApplyMode::Exact, ApplyMode::Compact)),
	[](testing::TestParamInfo<TextMotionCase> const& info) { return TextCaseName(info.param); });
