#include "skia_audio_presenter.h"
#include "skia_audio_tile_diagnostics.h"

#include "../../perf_trace.h"
#include "../../skia_runtime/platform_font_runtime.h"
#include "../../skia_runtime/skia_surface_provider.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkData.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMetrics.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPicture.h>
#include <include/core/SkPictureRecorder.h>
#include <include/core/SkRect.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkString.h>
#include <include/core/SkSurface.h>
#include <include/core/SkTypeface.h>
#include <include/effects/SkDashPathEffect.h>
#include <include/effects/SkGradient.h>
#include <include/effects/SkRuntimeEffect.h>
#include <include/gpu/GpuTypes.h>
#include <include/gpu/ganesh/GrDirectContext.h>
#include <include/gpu/ganesh/SkImageGanesh.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <include/gpu/ganesh/gl/GrGLInterface.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <queue>
#include <span>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegisub::skia::audio {
namespace {

constexpr std::size_t kDefaultContentCacheBudget = 32 * 1024 * 1024;
constexpr std::size_t kGaneshResourceCacheBudget = 64 * 1024 * 1024;
constexpr int kSpectrumPaletteTextureWidth = 2048;
constexpr int kSpectrumPaletteTextureHeight = static_cast<int>(
	(kSpectrumPaletteColorCount + kSpectrumPaletteTextureWidth - 1)
	/ kSpectrumPaletteTextureWidth);
// Matches AudioDisplay::foot_size in the legacy wx renderer.
constexpr float kMarkerFootSize = 6.f;

static_assert(kSpectrumPaletteTextureHeight == 3);

using FrameTraceClock = std::chrono::steady_clock;

// Resolve a typeface for the given family name, cached because every repaint
// asks for the same two or three faces. An empty family means "the platform
// default UI face", which is what legacy gets from the window's wxFont.
sk_sp<SkTypeface> ResolveAudioTypeface(std::string const& family, bool bold) {
	return PlatformFontRuntime::Get().ResolveTypeface({
		family,
		bold ? SkFontStyle::kBold_Weight : SkFontStyle::kNormal_Weight,
		SkFontStyle::kNormal_Width,
		SkFontStyle::kUpright_Slant,
	});
}

// Build the SkFont for one draw site from the display's base text style. Returns
// nullopt when no typeface resolves, so callers skip the draw instead of
// emitting invisible glyphs from an empty typeface.
//
// face_override mirrors legacy's per-site wxFont::SetFaceName (only the track
// cursor uses it); bold mirrors its wxFONTWEIGHT_BOLD.
std::optional<SkFont> MakeAudioFont(
	TextStyleFrame const& style,
	bool bold,
	std::string const& face_override = {}) {
	auto const& family = face_override.empty() ? style.face : face_override;
	auto typeface = ResolveAudioTypeface(family, bold);
	if (!typeface && !face_override.empty()) {
		// A configured face that does not exist should not cost us the label.
		typeface = ResolveAudioTypeface(style.face, bold);
	}
	if (!typeface)
		return std::nullopt;

	auto const size = std::isfinite(style.size) && style.size > 0.f
		? std::clamp(style.size, 4.f, 256.f)
		: 11.f;
	SkFont font(std::move(typeface), size);
	font.setEdging(SkFont::Edging::kAntiAlias);
	// Legacy renders through wxDC, which snaps glyph origins to whole pixels.
	font.setSubpixel(false);
	// Synthesise bold when the resolved face has no real bold variant, which is
	// what GDI/DirectWrite do for legacy.
	if (bold && font.getTypeface() && !font.getTypeface()->isBold())
		font.setEmbolden(true);
	return font;
}

// wxDC draws a 1px line on one exact pixel column. Skia's drawLine centres a
// 1px stroke on the coordinate, so it straddles two columns and lands a half
// pixel left of where legacy puts it. Thin timeline rules and ticks are drawn as
// pixel-snapped rects instead so both renderers light the same columns.
void FillDeviceRect(
	SkCanvas *canvas, float x, float y, float width, float height, SkPaint const& paint) {
	canvas->drawRect(
		SkRect::MakeXYWH(
			std::floor(x),
			std::floor(y),
			std::max(1.f, std::round(width)),
			std::max(1.f, std::round(height))),
		paint);
}

float DeviceStrokeWidth(float width) noexcept {
	return std::max(1.f, std::round(width));
}

float DeviceStrokeLeft(float x, float width) noexcept {
	width = DeviceStrokeWidth(width);
	return std::floor(x - width * 0.5f + 0.5f);
}

void FillDeviceVerticalStroke(
	SkCanvas *canvas,
	float x,
	float y,
	float height,
	float width,
	SkPaint const& paint) {
	width = DeviceStrokeWidth(width);
	FillDeviceRect(canvas, DeviceStrokeLeft(x, width), y, width, height, paint);
}

// wxDC::DrawText anchors text by its top-left corner; Skia anchors it by the
// baseline. fAscent is negative, hence the subtraction.
float TextBaselineForTop(SkFont const& font, float top) {
	SkFontMetrics metrics;
	font.getMetrics(&metrics);
	return top - metrics.fAscent;
}

float TextLineHeight(SkFont const& font) {
	SkFontMetrics metrics;
	font.getMetrics(&metrics);
	return metrics.fDescent - metrics.fAscent;
}

// Device pixels per logical pixel, used to scale the pixel offsets legacy
// hardcodes for a 1x display.
float FrameContentScale(ContentFrame const& frame) noexcept {
	return std::isfinite(frame.content_scale)
		? std::clamp(frame.content_scale, 1.f, 8.f)
		: 1.f;
}

void DrawMarkerFoot(
	SkCanvas *canvas,
	float marker_x,
	float marker_top,
	float marker_bottom,
	float direction,
	float size,
	SkPaint const& paint) {
	SkPathBuilder feet;
	feet.moveTo(marker_x + direction * size, marker_top);
	feet.lineTo(marker_x, marker_top);
	feet.lineTo(marker_x, marker_top + size);
	feet.close();
	feet.moveTo(marker_x + direction * size, marker_bottom);
	feet.lineTo(marker_x, marker_bottom - size);
	feet.lineTo(marker_x, marker_bottom);
	feet.close();
	SkPaint fill(paint);
	fill.setStyle(SkPaint::kFill_Style);
	canvas->drawPath(feet.detach(), fill);
}

FrameTraceClock::time_point BeginFrameTrace(bool enabled) noexcept {
	return enabled ? FrameTraceClock::now() : FrameTraceClock::time_point {};
}

double EndFrameTrace(FrameTraceClock::time_point started) noexcept {
	if (started == FrameTraceClock::time_point {})
		return -1.0;
	return std::chrono::duration<double, std::milli>(FrameTraceClock::now() - started).count();
}

perf_trace::AudioContentTileEvent MakeTileEvent(
	char const *stage,
	ContentTileKey const& key) noexcept {
	perf_trace::AudioContentTileEvent event;
	event.stage = stage;
	event.spectrum = key.kind == ContentKind::Spectrum;
	event.provider_generation = key.generation.provider;
	event.analysis_generation = key.generation.analysis;
	event.tile_index = key.tile_index;
	event.column_count = key.column_count;
	event.spectrum_bin_count = key.spectrum_bin_count;
	return event;
}

perf_trace::AudioContentTileEvent MakePayloadEvent(
	char const *stage,
	ContentUploadPayloadKey const& key) noexcept {
	auto event = MakeTileEvent(stage, key.tile);
	event.variant_revision = key.variant_revision;
	return event;
}

SkiaGlFailureInjection DeviceFailureInjection(FailureInjection injection) noexcept {
	switch (injection) {
		case FailureInjection::ContextInitialization:
			return SkiaGlFailureInjection::ContextInitialization;
		case FailureInjection::FlushSubmit:
			return SkiaGlFailureInjection::FlushSubmit;
		case FailureInjection::None:
		case FailureInjection::FrameBegin:
		case FailureInjection::Unsupported:
			return SkiaGlFailureInjection::None;
	}
	return SkiaGlFailureInjection::None;
}

std::string ReadGlString(GLenum name) {
	auto const *value = glGetString(name);
	return value ? reinterpret_cast<char const *>(value) : std::string{};
}

sk_sp<SkImage> UploadTexture(
	GrDirectContext *context,
	SkImageInfo const& info,
	void const *pixels,
	std::size_t byte_count,
	std::size_t row_bytes) {
	if (!context || !pixels || byte_count == 0)
		return nullptr;
	auto data = SkData::MakeWithCopy(pixels, byte_count);
	if (!data)
		return nullptr;
	auto raster = SkImages::RasterFromData(info, std::move(data), row_bytes);
	if (!raster)
		return nullptr;
	return SkImages::TextureFromImage(context, raster);
}

std::vector<std::uint8_t> EncodePalette(SpectrumPalette const& palette) {
	std::vector<std::uint8_t> pixels(
		static_cast<std::size_t>(kSpectrumPaletteTextureWidth)
			* kSpectrumPaletteTextureHeight * 4);
	auto const write_color = [&pixels](std::size_t index, std::uint32_t color) {
		pixels[index * 4 + 0] = static_cast<std::uint8_t>((color >> 16) & 0xFF);
		pixels[index * 4 + 1] = static_cast<std::uint8_t>((color >> 8) & 0xFF);
		pixels[index * 4 + 2] = static_cast<std::uint8_t>(color & 0xFF);
		pixels[index * 4 + 3] = static_cast<std::uint8_t>((color >> 24) & 0xFF);
	};
	for (std::size_t i = 0; i < palette.colors.size(); ++i) {
		write_color(i, palette.colors[i]);
	}
	for (std::size_t i = palette.colors.size(); i < pixels.size() / 4; ++i)
		write_color(i, palette.colors.back());
	return pixels;
}

std::string ValidateContentFrame(FrameTarget const& target, ContentFrame const& frame) {
	if (!frame.generation.provider || !frame.generation.analysis)
		return "the Audio content generation is zero";
	if (!std::isfinite(frame.x)
		|| !std::isfinite(frame.y)
		|| !std::isfinite(frame.width)
		|| !std::isfinite(frame.height)
		|| !std::isfinite(frame.first_column_offset)
		|| frame.x < 0.f
		|| frame.y < 0.f
		|| frame.width <= 0.f
		|| frame.height <= 0.f
		|| frame.first_column_offset > 0.f
		|| frame.first_column_offset <= -1.f)
		return "the Audio content bounds are invalid";
	if (frame.x > target.width - frame.width || frame.y > target.height - frame.height)
		return "the Audio content bounds exceed the frame target";
	if (!std::isfinite(frame.amplitude) || frame.amplitude < 0.f)
		return "the Audio content amplitude is invalid";
	if (frame.kind == ContentKind::Spectrum
		&& (!frame.spectrum_palette || !frame.spectrum_palette->revision
			|| !frame.spectrum_band_plan || !frame.spectrum_band_plan->IsValid()
			|| frame.spectrum_band_plan->output_height != static_cast<int>(std::lround(frame.height)))) {
		return "the spectrum palette, band plan, or its revision is missing";
	}
	for (auto const& style : frame.styles) {
		if (!IsValidDeviceStyleSpan(frame.x, frame.width, style.x, style.width))
			return "an Audio rendering style span is invalid";
		if (frame.kind == ContentKind::Spectrum
			&& (!style.spectrum_palette || !style.spectrum_palette->revision))
			return "an Audio spectrum rendering style palette is missing";
	}
	return {};
}

enum class FrameLayerPart : std::uint32_t {
	None = 0,
	Timeline = 1u << 0,
	Marker = 1u << 1,
	CursorLine = 1u << 2,
	TimingLabel = 1u << 3,
	CursorLabel = 1u << 4,
	Scrollbar = 1u << 5,
	All = (1u << 6) - 1,
};

constexpr FrameLayerPart operator|(FrameLayerPart lhs, FrameLayerPart rhs) noexcept {
	return static_cast<FrameLayerPart>(
		static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

constexpr bool HasFrameLayerPart(FrameLayerPart mask, FrameLayerPart part) noexcept {
	return (static_cast<std::uint32_t>(mask) & static_cast<std::uint32_t>(part)) != 0;
}

void DrawAudioFrameLayers(
	SkCanvas *canvas,
	FrameTarget const& target,
	ContentFrame const& frame,
	PresenterFrameTrace *frame_trace,
	FrameLayerPart parts = FrameLayerPart::All) {
	if (!canvas)
		return;

	SkPaint paint;
	paint.setAntiAlias(false);

	// Timeline is deliberately drawn after content in the same canvas submit.
	// Its scroll origin is expressed in device pixels, matching FrameViewport.
	if (HasFrameLayerPart(parts, FrameLayerPart::Timeline)
		&& frame.timeline && frame.timeline->height > 0) {
		auto const timeline_y = static_cast<float>(frame.timeline->y);
		auto const timeline_height = static_cast<float>(frame.timeline->height);
		auto const timeline_bottom = timeline_y + timeline_height;
		auto const scale = FrameContentScale(frame);
		paint.setColor(static_cast<SkColor>(frame.timeline->background_color));
		canvas->drawRect(SkRect::MakeXYWH(frame.x, timeline_y, frame.width, timeline_height), paint);

		paint.setColor(static_cast<SkColor>(frame.timeline->foreground_color));

		// Legacy's "Top line": a full-width rule along the bottom of the timeline
		// band, separating it from the waveform.
		FillDeviceRect(canvas, frame.x, timeline_bottom - scale, frame.width, scale, paint);

		auto const device_ms_per_pixel = frame.timeline->milliseconds_per_pixel;
		auto const logical_ms_per_pixel = device_ms_per_pixel * scale;
		auto const plan = BuildTimelineScalePlan(logical_ms_per_pixel);
		if (plan.valid) {
			auto const logical_scroll_left = std::isfinite(frame.timeline->scroll_left_exact)
				&& (frame.timeline->scroll_left_exact != 0.0 || frame.timeline->scroll_left == 0)
				? frame.timeline->scroll_left_exact / scale
				: static_cast<double>(frame.timeline->scroll_left);
			auto const marks = BuildTimelineMarks(
				plan,
				logical_scroll_left,
				frame.width / scale,
				logical_ms_per_pixel);
			auto const font = MakeAudioFont(frame.text_style, false);
			TimelineLabelFormatter formatter(plan.scale, frame.timeline->duration_ms);
			// Clip to the timeline band so an edge label is truncated rather than
			// dropped, which is what wxDC does for the legacy timeline.
			canvas->save();
			canvas->clipRect(SkRect::MakeXYWH(frame.x, timeline_y, frame.width, timeline_height));
			SkPaint text_paint(paint);
			text_paint.setAntiAlias(true);
			double last_text_right = -1.0;
			for (auto const& mark : marks) {
				auto const device_mark_x = static_cast<float>(mark.x * scale);
				// Legacy tick heights: major bottom-6..bottom-1, minor
				// bottom-4..bottom-1, i.e. 5px and 3px stopping one pixel short of
				// the rule. Scaled so they keep their proportions on HiDPI.
				auto const tick_height = (mark.major ? 5.f : 3.f) * scale;
				FillDeviceRect(
					canvas,
					frame.x + device_mark_x,
					timeline_bottom - scale - tick_height,
					scale,
					tick_height,
					paint);
				// Legacy only formats a label when it will actually be drawn, so
				// the hour/minute suppression state advances on drawn labels only.
				if (!mark.major || !font || device_mark_x <= last_text_right)
					continue;
				auto const label = formatter.Format(mark.time_ms);
				if (label.empty())
					continue;
				last_text_right = device_mark_x + font->measureText(
					label.data(), label.size(), SkTextEncoding::kUTF8);
				canvas->drawSimpleText(
					label.data(), label.size(), SkTextEncoding::kUTF8,
					std::floor(frame.x + device_mark_x),
					TextBaselineForTop(*font, timeline_y),
					*font,
					text_paint);
			}
			canvas->restore();
		}
	}

	// Marker lines, feet and labels are all batched into this frame. The frame
	// builder supplies device-space x coordinates, so no extra time conversion
	// or intermediate surface is needed here.
	if (HasFrameLayerPart(parts, FrameLayerPart::Marker)
		|| HasFrameLayerPart(parts, FrameLayerPart::CursorLine)) {
		canvas->save();
		canvas->clipRect(SkRect::MakeXYWH(frame.x, frame.y, frame.width, frame.height));
		if (HasFrameLayerPart(parts, FrameLayerPart::Marker)) {
			auto const marker_trace_started = BeginFrameTrace(frame_trace != nullptr);
			auto const scale = FrameContentScale(frame);
			auto const marker_foot_size = kMarkerFootSize * scale;
			SkScalar const dotted_intervals[] { scale, scale };
			auto const dotted_effect = SkDashPathEffect::Make(
				SkSpan<const SkScalar>(dotted_intervals, 2), 0.f);
			int markers_drawn = 0;
			for (auto const& marker : frame.markers) {
				if (!std::isfinite(marker.x)
					|| marker.x < frame.x - marker_foot_size
					|| marker.x > frame.x + frame.width + marker_foot_size)
					continue;
				paint.reset();
				paint.setAntiAlias(false);
				paint.setColor(static_cast<SkColor>(marker.color));
				auto const marker_width = DeviceStrokeWidth(
					static_cast<float>(std::max(1, marker.width)));
				if (marker.line_style == MarkerLineStyle::Dotted && dotted_effect) {
					paint.setStyle(SkPaint::kStroke_Style);
					paint.setStrokeWidth(marker_width);
					paint.setStrokeCap(SkPaint::kButt_Cap);
					paint.setPathEffect(dotted_effect);
					auto const marker_center = DeviceStrokeLeft(marker.x, marker_width)
						+ marker_width * 0.5f;
					canvas->drawLine(
						marker_center,
						frame.y,
						marker_center,
						frame.y + frame.height,
						paint);
					paint.setPathEffect(nullptr);
				}
				else {
					FillDeviceVerticalStroke(
						canvas, marker.x, frame.y, frame.height, marker_width, paint);
				}
				if (marker.feet & 1u) {
					paint.setColor(static_cast<SkColor>(marker.left_foot_color));
					DrawMarkerFoot(canvas, marker.x, frame.y, frame.y + frame.height,
						-1.f, marker_foot_size, paint);
				}
				if (marker.feet & 2u) {
					paint.setColor(static_cast<SkColor>(marker.right_foot_color));
					DrawMarkerFoot(canvas, marker.x, frame.y, frame.y + frame.height,
						1.f, marker_foot_size, paint);
				}
				++markers_drawn;
			}
			if (frame_trace) {
				frame_trace->marker_layer_rebuild_ms = EndFrameTrace(marker_trace_started);
				frame_trace->marker_count = static_cast<int>(frame.markers.size());
				frame_trace->markers_drawn = markers_drawn;
			}
		}
		if (HasFrameLayerPart(parts, FrameLayerPart::CursorLine)
			&& frame.cursor && std::isfinite(frame.cursor->x)) {
			paint.setColor(static_cast<SkColor>(frame.cursor->color));
			FillDeviceVerticalStroke(
				canvas, frame.cursor->x, frame.y, frame.height, 1.f, paint);
		}
		canvas->restore();
	}

	if ((HasFrameLayerPart(parts, FrameLayerPart::TimingLabel) && !frame.labels.empty())
		|| (HasFrameLayerPart(parts, FrameLayerPart::CursorLabel)
			&& frame.cursor && !frame.cursor->label.empty())) {
		auto const label_trace_started = BeginFrameTrace(
			frame_trace && HasFrameLayerPart(parts, FrameLayerPart::TimingLabel)
			&& !frame.labels.empty());
		// Legacy PaintLabels bolds the window font; PaintTrackCursor does the same
		// on top of its optional face override.
		auto const scale = FrameContentScale(frame);
		auto const bold_font = MakeAudioFont(frame.text_style, true);
		paint.setAntiAlias(true);
		paint.setColor(SK_ColorWHITE);
		int labels_drawn = 0;
		if (HasFrameLayerPart(parts, FrameLayerPart::TimingLabel) && bold_font) {
			// Legacy anchors timing labels 4px below the top of the waveform.
			auto const label_top = frame.y + 4.f * scale;
			auto const baseline = TextBaselineForTop(*bold_font, label_top);
			auto const line_height = TextLineHeight(*bold_font);
			for (auto const& label : frame.labels) {
				if (label.text.empty() || !std::isfinite(label.x)
					|| !std::isfinite(label.width) || label.width <= 0.f)
					continue;
				auto const text_width = bold_font->measureText(
					label.text.data(), label.text.size(), SkTextEncoding::kUTF8);
				if (label.width < text_width) {
					// Too narrow for the text: truncate it, as legacy does.
					canvas->save();
					canvas->clipRect(SkRect::MakeXYWH(label.x, label_top, label.width, line_height));
					canvas->drawSimpleText(label.text.data(), label.text.size(), SkTextEncoding::kUTF8,
						label.x, baseline, *bold_font, paint);
					canvas->restore();
				}
				else {
					// Otherwise centre it in the range.
					canvas->drawSimpleText(label.text.data(), label.text.size(), SkTextEncoding::kUTF8,
						std::floor(label.x + (label.width - text_width) * 0.5f),
						baseline, *bold_font, paint);
				}
				++labels_drawn;
			}
		}
		if (frame_trace && HasFrameLayerPart(parts, FrameLayerPart::TimingLabel)
			&& !frame.labels.empty()) {
			frame_trace->label_layer_rebuild_ms = EndFrameTrace(label_trace_started);
			frame_trace->label_count = static_cast<int>(frame.labels.size());
			frame_trace->labels_drawn = labels_drawn;
		}
		if (HasFrameLayerPart(parts, FrameLayerPart::CursorLabel)
			&& frame.cursor && !frame.cursor->label.empty() && std::isfinite(frame.cursor->x)) {
			// The cursor label is always bold (legacy PaintTrackCursor sets
			// wxFONTWEIGHT_BOLD unconditionally), with an optional face override
			// from "Audio/Track Cursor/Font Face".
			auto const cursor_font = MakeAudioFont(frame.text_style, true, frame.cursor->font_face);
			if (cursor_font) {
				auto const& text = frame.cursor->label;
				auto const text_width = cursor_font->measureText(
					text.data(), text.size(), SkTextEncoding::kUTF8);
				// Legacy centres the label on the cursor, then keeps it inside the
				// client area with a 2px margin on both sides.
				auto const margin = 2.f * scale;
				auto const limit = std::max(
					margin, static_cast<float>(target.width) - text_width - margin);
				auto const label_x = std::clamp(frame.cursor->x - text_width * 0.5f, margin, limit);
				auto const label_top = frame.y + margin;
				auto const baseline = TextBaselineForTop(*cursor_font, label_top);
				auto const draw = [&](float dx, float dy) {
					canvas->drawSimpleText(
						text.data(), text.size(), SkTextEncoding::kUTF8,
						std::floor(label_x) + dx, baseline + dy, *cursor_font, paint);
				};
				// Legacy draws the label four times offset by one pixel in a dark
				// grey to outline it, then once in white on top, so it stays
				// readable over a bright waveform.
				paint.setColor(SkColorSetRGB(64, 64, 64));
				draw(-scale, -scale);
				draw(-scale, scale);
				draw(scale, -scale);
				draw(scale, scale);
				paint.setColor(SK_ColorWHITE);
				draw(0.f, 0.f);
			}
		}
	}

	if (HasFrameLayerPart(parts, FrameLayerPart::Scrollbar)
		&& frame.scrollbar && frame.scrollbar->height > 0) {
		auto const scrollbar_trace_started = BeginFrameTrace(frame_trace != nullptr);
		auto const scrollbar = *frame.scrollbar;
		auto const y = static_cast<float>(std::clamp(scrollbar.y, 0, target.height - scrollbar.height));
		auto const h = static_cast<float>(scrollbar.height);
		auto const scale = std::isfinite(scrollbar.content_scale)
			? std::clamp(scrollbar.content_scale, 1.f, 8.f) : 1.f;
		auto const track = std::max(1.f, static_cast<float>(target.width));
		auto const geometry = BuildScrollbarGeometry(
			track,
			10.f * scale,
			25.f * scale,
			scrollbar.total,
			scrollbar.page,
			scrollbar.position,
			scrollbar.load_position,
			scrollbar.selection_start,
			scrollbar.selection_length);
		if (!geometry.valid) {
			if (frame_trace)
				frame_trace->scrollbar_layer_rebuild_ms = EndFrameTrace(scrollbar_trace_started);
			return;
		}

		canvas->save();
		canvas->clipRect(SkRect::MakeXYWH(0.f, y, track, h));
		paint.reset();
		paint.setAntiAlias(false);
		paint.setColor(static_cast<SkColor>(scrollbar.background_color));
		canvas->drawRect(SkRect::MakeXYWH(0.f, y, track, h), paint);

		// Match wx AudioDisplayScrollbar z-order: selection is part of the
		// track and must never obscure the load marker or draggable thumb.
		if (geometry.selection_visible) {
			paint.setColor(static_cast<SkColor>(scrollbar.selection_color));
			canvas->drawRect(SkRect::MakeXYWH(
				geometry.selection_x, y, geometry.selection_width, h), paint);
		}

		auto const border_width = std::max(1.f, std::round(scale));
		paint.setColor(static_cast<SkColor>(scrollbar.thumb_color));
		FillDeviceRect(canvas, 0.f, y, track, border_width, paint);
		FillDeviceRect(canvas, 0.f, y + h - border_width, track, border_width, paint);
		FillDeviceRect(canvas, 0.f, y, border_width, h, paint);
		FillDeviceRect(canvas, track - border_width, y, border_width, h, paint);

		if (geometry.load_visible) {
			std::array<SkColor4f, 2> colors {
				SkColor4f::FromColor(static_cast<SkColor>(scrollbar.background_color)),
				SkColor4f::FromColor(static_cast<SkColor>(scrollbar.thumb_color)),
			};
			SkPoint points[] {
				{ geometry.load_x, y },
				{ geometry.load_x + geometry.load_width, y },
			};
			SkGradient gradient(
				SkGradient::Colors(SkSpan<const SkColor4f>(colors), SkTileMode::kClamp),
				{});
			paint.reset();
			paint.setAntiAlias(false);
			paint.setShader(SkShaders::LinearGradient(points, gradient));
			canvas->drawRect(SkRect::MakeXYWH(
				geometry.load_x,
				y + scale,
				geometry.load_width,
				std::max(1.f, h - 2.f * scale)), paint);
		}

		paint.reset();
		paint.setAntiAlias(false);
		paint.setColor(static_cast<SkColor>(scrollbar.thumb_color));
		canvas->drawRect(SkRect::MakeXYWH(
			geometry.thumb_x, y, geometry.thumb_width, h), paint);
		canvas->restore();
		if (frame_trace) {
			frame_trace->scrollbar_layer_rebuild_ms = EndFrameTrace(scrollbar_trace_started);
			frame_trace->scrollbar_selection_visible = geometry.selection_visible;
			frame_trace->scrollbar_load_visible = geometry.load_visible;
		}
	}
}

void DrawTimelineLayer(SkCanvas *canvas, ContentFrame const& frame) {
	DrawAudioFrameLayers(canvas, {}, frame, nullptr, FrameLayerPart::Timeline);
}

void DrawMarkerLayer(SkCanvas *canvas, ContentFrame const& frame, PresenterFrameTrace *trace) {
	DrawAudioFrameLayers(canvas, {}, frame, trace, FrameLayerPart::Marker);
}

void DrawTimingLabelLayer(SkCanvas *canvas, ContentFrame const& frame, PresenterFrameTrace *trace) {
	DrawAudioFrameLayers(canvas, {}, frame, trace, FrameLayerPart::TimingLabel);
}

// The target is needed here: legacy clamps the cursor time label to the client
// width so it stays fully on screen when the cursor is near either edge.
void DrawCursorLayer(
	SkCanvas *canvas, FrameTarget const& target, ContentFrame const& frame, bool label) {
	DrawAudioFrameLayers(
		canvas,
		target,
		frame,
		nullptr,
		label ? FrameLayerPart::CursorLabel : FrameLayerPart::CursorLine);
}

void DrawScrollbarLayer(
	SkCanvas *canvas,
	FrameTarget const& target,
	ContentFrame const& frame,
	PresenterFrameTrace *trace) {
	DrawAudioFrameLayers(canvas, target, frame, trace, FrameLayerPart::Scrollbar);
}

sk_sp<SkPicture> RecordLayerPicture(
	FrameTarget const& target,
	std::function<void(SkCanvas *)> const& draw) {
	SkPictureRecorder recorder;
	auto *canvas = recorder.beginRecording(
		static_cast<SkScalar>(target.width),
		static_cast<SkScalar>(target.height));
	if (!canvas)
		return nullptr;
	draw(canvas);
	return recorder.finishRecordingAsPicture();
}

sk_sp<SkImage> RasterizeMarkerLayer(
	GrDirectContext *context,
	FrameTarget const& target,
	ContentFrame const& frame,
	PresenterFrameTrace *trace) {
	if (!context || target.width <= 0 || target.height <= 0)
		return nullptr;
	auto surface = SkSurfaces::RenderTarget(
		context,
		skgpu::Budgeted::kYes,
		SkImageInfo::Make(
			target.width,
			target.height,
			kRGBA_8888_SkColorType,
			kPremul_SkAlphaType,
			SkColorSpace::MakeSRGB()),
		0,
		nullptr);
	if (!surface)
		return nullptr;
	auto *canvas = surface->getCanvas();
	canvas->clear(SK_ColorTRANSPARENT);
	DrawMarkerLayer(canvas, frame, trace);
	return surface->makeImageSnapshot();
}

// readPixels may change these bindings too; keep the guard alive through both
// raw front-buffer capture and Skia texture readback. No GL context is switched.
class DiagnosticReadbackState final {
	GrGLInterface const& gl;
	GrDirectContext& context;
	bool separate_framebuffers = false;
	bool pack_buffer_supported = false;
	GLint read_framebuffer = 0;
	GLint draw_framebuffer = 0;
	GLint read_buffer = 0;
	GLint default_read_buffer = 0;
	GLint pack_buffer = 0;
	static constexpr GLenum framebuffer = 0x8D40;
	static constexpr GLenum read_framebuffer_target = 0x8CA8;
	static constexpr GLenum draw_framebuffer_target = 0x8CA9;
	static constexpr GLenum pixel_pack_buffer = 0x88EB;
	static constexpr std::array<GLenum, 6> pack_names{
		GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_ROWS,
		GL_PACK_SKIP_PIXELS, GL_PACK_SWAP_BYTES, GL_PACK_LSB_FIRST};
	std::array<GLint, pack_names.size()> pack_values{};

	public:
	explicit DiagnosticReadbackState(GrGLInterface const& gl, GrDirectContext& context, std::string const& version)
		: gl(gl), context(context) {
		int major = 0;
		int minor = 0;
		char dot = 0;
		std::istringstream version_stream(version);
		version_stream.imbue(std::locale::classic());
		version_stream >> major >> dot >> minor;
		separate_framebuffers = major >= 3 || gl.fExtensions.has("GL_ARB_framebuffer_object") || gl.fExtensions.has("GL_EXT_framebuffer_blit");
		pack_buffer_supported = major > 2 || (major == 2 && minor >= 1) || gl.fExtensions.has("GL_ARB_pixel_buffer_object") || gl.fExtensions.has("GL_EXT_pixel_buffer_object");
		glGetIntegerv(separate_framebuffers ? 0x8CAA : 0x8CA6, &read_framebuffer);
		glGetIntegerv(0x8CA6, &draw_framebuffer);
		glGetIntegerv(GL_READ_BUFFER, &read_buffer);
		for (std::size_t i = 0; i < pack_names.size(); ++i)
			glGetIntegerv(pack_names[i], &pack_values[i]);
		if (pack_buffer_supported)
			glGetIntegerv(0x88ED, &pack_buffer);
		gl.fFunctions.fBindFramebuffer(separate_framebuffers ? read_framebuffer_target : framebuffer, 0);
		glGetIntegerv(GL_READ_BUFFER, &default_read_buffer);
	}

	~DiagnosticReadbackState() {
		gl.fFunctions.fBindFramebuffer(separate_framebuffers ? read_framebuffer_target : framebuffer, 0);
		glReadBuffer(static_cast<GLenum>(default_read_buffer));
		gl.fFunctions.fBindFramebuffer(
			separate_framebuffers ? read_framebuffer_target : framebuffer,
			static_cast<GLuint>(read_framebuffer));
		glReadBuffer(static_cast<GLenum>(read_buffer));
		if (separate_framebuffers)
			gl.fFunctions.fBindFramebuffer(draw_framebuffer_target, static_cast<GLuint>(draw_framebuffer));
		for (std::size_t i = 0; i < pack_names.size(); ++i)
			glPixelStorei(pack_names[i], pack_values[i]);
		if (pack_buffer_supported)
			gl.fFunctions.fBindBuffer(pixel_pack_buffer, static_cast<GLuint>(pack_buffer));
		// Restoration bypasses Ganesh too. Invalidate its binding knowledge on
		// every exit, including exceptions, after all raw GL state is restored.
		context.resetContext();
	}

	void PrepareFrontRead() const {
		if (pack_buffer_supported)
			gl.fFunctions.fBindBuffer(pixel_pack_buffer, 0);
		for (auto name : pack_names)
			glPixelStorei(name, name == GL_PACK_ALIGNMENT ? 1 : 0);
		glReadBuffer(GL_FRONT);
	}
};

void WriteDiagnosticBytes(std::filesystem::path const& path, std::span<std::uint8_t const> bytes) {
	std::ofstream file(path, std::ios::binary);
	file.exceptions(std::ios::badbit | std::ios::failbit);
	file.write(reinterpret_cast<char const *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	file.close();
}

void WriteDiagnosticPpm(
	std::filesystem::path const& path,
	int width,
	int height,
	std::span<std::uint8_t const> rgba,
	bool bottom_up) {
	std::ofstream file(path, std::ios::binary);
	file.exceptions(std::ios::badbit | std::ios::failbit);
	file.imbue(std::locale::classic());
	file << "P6\n"
		 << width << ' ' << height << "\n255\n";
	std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 3);
	for (int y = 0; y < height; ++y) {
		auto const source_y = bottom_up ? height - y - 1 : y;
		auto const source_row = static_cast<std::size_t>(source_y) * width * 4;
		for (int x = 0; x < width; ++x)
			std::copy_n(rgba.data() + source_row + static_cast<std::size_t>(x) * 4, 3, row.data() + static_cast<std::size_t>(x) * 3);
		file.write(reinterpret_cast<char const *>(row.data()), static_cast<std::streamsize>(row.size()));
	}
	file.close();
}

void WriteDiagnosticSummary(std::ostream& stream, TileDataSummary const& summary) {
	stream << FormatDiagnosticHash(summary.hash) << ',' << summary.element_count << ','
		   << summary.nonzero_columns << ',' << summary.minimum << ',' << summary.maximum;
}
}

struct Presenter::Impl {
	struct GpuContentEntry {
		sk_sp<SkImage> primary;
		std::size_t bytes = 0;
		std::uint64_t touch = 0;
		TileDataSummary upload_summary;
		void const *upload_native_context = nullptr;
	};
	struct GpuContentTouch {
		std::uint64_t touch = 0;
		ContentUploadPayloadKey key;
		bool operator>(GpuContentTouch const& other) const noexcept { return touch > other.touch; }
	};

	explicit Impl(FailureInjection failure_injection)
	: failure_injection(failure_injection)
	, device(DeviceFailureInjection(failure_injection)) {
		{
			auto const sksl = SkString(R"(
				uniform shader endpoint_texture;
				uniform float height;
				uniform float amplitude;
				uniform float draw_average;
				layout(color) uniform half4 peak_color;
				layout(color) uniform half4 average_color;

				float truncate_to_zero(float value) {
					return value < 0.0 ? ceil(value) : floor(value);
				}

				half4 main(float2 p) {
					half4 encoded = endpoint_texture.eval(float2(p.x, 0.5));
					float4 endpoints = float4(encoded) * 2.0 - 1.0;
					float midpoint = floor(height * 0.5);
					float4 scaled = float4(
						truncate_to_zero(endpoints.r * amplitude * midpoint),
						truncate_to_zero(endpoints.g * amplitude * midpoint),
						truncate_to_zero(endpoints.b * amplitude * midpoint),
						truncate_to_zero(endpoints.a * amplitude * midpoint));
					scaled = clamp(scaled, -midpoint, midpoint);
					float row = floor(p.y);
					bool in_peak = row >= midpoint - scaled.g
						&& row <= midpoint - scaled.r;
					bool in_average = row >= midpoint - scaled.a
						&& row <= midpoint - scaled.b;
					if (draw_average > 0.5 && in_average)
						return average_color;
					return in_peak ? peak_color : half4(0.0);
				}
			)");
			auto result = SkRuntimeEffect::MakeForShader(sksl);
			waveform_effect = std::move(result.effect);
			if (!waveform_effect)
				waveform_effect_error = result.errorText.c_str();
		}
		{
			auto const sksl = SkString(R"(
			uniform shader power_texture;
			uniform shader palette_texture;
			uniform float amplitude;
			half4 main(float2 p) {
				half4 encoded = power_texture.eval(p);
				float normalized = encoded.r * (16711680.0 / 16777215.0)
				                 + encoded.g * (65280.0 / 16777215.0)
				                 + encoded.b * (255.0 / 16777215.0);
				float power = normalized * 8.0;
				float palette_index = floor(
					clamp(power * amplitude, 0.0, 1.0) * 4096.0);
				float palette_y = floor(palette_index / 2048.0);
				float palette_x = palette_index - palette_y * 2048.0;
				return palette_texture.eval(float2(palette_x + 0.5, palette_y + 0.5));
			}
		)");
			auto result = SkRuntimeEffect::MakeForShader(sksl);
			spectrum_effect = std::move(result.effect);
			if (!spectrum_effect)
				spectrum_effect_error = result.errorText.c_str();
		}
	}

	FailureInjection failure_injection = FailureInjection::None;
	SkiaGlDevice device;
	SkiaSurfaceProvider surface_provider;
	sk_sp<SkSurface> surface;
	std::optional<SurfaceKey> surface_key;
	std::unordered_map<ContentUploadPayloadKey, GpuContentEntry, ContentUploadPayloadKeyHash> content_cache;
	std::priority_queue<GpuContentTouch, std::vector<GpuContentTouch>, std::greater<GpuContentTouch>> content_touches;
	std::size_t content_cache_budget = kDefaultContentCacheBudget;
	std::size_t content_cache_bytes = 0;
	std::uint64_t content_touch_counter = 0;
	sk_sp<SkRuntimeEffect> waveform_effect;
	std::string waveform_effect_error;
	sk_sp<SkRuntimeEffect> spectrum_effect;
	std::string spectrum_effect_error;
	std::unordered_map<std::uint64_t, sk_sp<SkImage>> palette_images;
	std::uint64_t retained_static_revision = 0;
	SurfaceKey retained_surface_key;
	bool retained_layers_ready = false;
	sk_sp<SkImage> retained_base_image;
	sk_sp<SkPicture> retained_timeline;
	sk_sp<SkImage> retained_markers;
	sk_sp<SkPicture> retained_timing_labels;
	sk_sp<SkPicture> retained_post_cursor;
	PresenterMetrics metrics;
	SkiaGlContextToken last_context;
	void const *last_native_context = nullptr;
	bool failure_logged = false;
	bool gl_probed = false;
	bool resource_budget_set = false;
	std::string gl_vendor;
	std::string gl_renderer;
	std::string gl_version;

	void UpdateContentMetrics() noexcept {
		metrics.content_cache_entries = content_cache.size();
		metrics.content_cache_bytes = content_cache_bytes;
		metrics.content_cache_budget_bytes = content_cache_budget;
	}

	void ResetContentCache() noexcept {
		content_cache.clear();
		content_touches = {};
		content_cache_bytes = 0;
		content_touch_counter = 0;
		palette_images.clear();
		UpdateContentMetrics();
	}

	void ResetRetainedLayers() noexcept {
		retained_static_revision = 0;
		retained_surface_key = {};
		retained_layers_ready = false;
		retained_base_image.reset();
		retained_timeline.reset();
		retained_markers.reset();
		retained_timing_labels.reset();
		retained_post_cursor.reset();
	}

	void Fail(SkiaGlContextToken context, SkiaGlDeviceFailure failure, std::string detail) noexcept {
		last_context = context;
		surface.reset();
		surface_key.reset();
		ResetContentCache();
		ResetRetainedLayers();
		device.Fail(context, failure, std::move(detail));
	}

	void TouchContent(ContentUploadPayloadKey const& key, GpuContentEntry& entry) {
		entry.touch = ++content_touch_counter;
		content_touches.push({ entry.touch, key });
		if (content_touches.size() > content_cache.size() * 4 + 64) {
			decltype(content_touches) compacted;
			for (auto const& [current_key, current_entry] : content_cache)
				compacted.push({ current_entry.touch, current_key });
			content_touches.swap(compacted);
		}
	}

	void TrimContentCache() {
		while (content_cache_bytes > content_cache_budget && !content_cache.empty()) {
			bool evicted = false;
			while (!content_touches.empty()) {
				auto const candidate = content_touches.top();
				content_touches.pop();
				auto entry = content_cache.find(candidate.key);
				if (entry == content_cache.end() || entry->second.touch != candidate.touch)
					continue;
				auto event = MakePayloadEvent("gpu_evict", entry->first);
				event.outcome = "budget";
				event.bytes = entry->second.bytes;
				content_cache_bytes -= entry->second.bytes;
				content_cache.erase(entry);
				++metrics.content_evictions;
				perf_trace::ObserveAudioContentTileEvent(event);
				evicted = true;
				break;
			}
			if (!evicted)
				break;
		}
		UpdateContentMetrics();
	}

	bool PrepareFrame(SkiaGlContextToken context, FrameTarget const& target) {
		last_context = context;
		auto validation = ValidateFrameTarget(target, context.generation);
		if (!validation.valid) {
			Fail(context, SkiaGlDeviceFailure::InvalidFrameTarget, std::move(validation.detail));
			return false;
		}
		if (failure_injection == FailureInjection::Unsupported) {
			Fail(
				context,
				SkiaGlDeviceFailure::UnsupportedFailureInjection,
				"AEGISUB_SKIA_AUDIO_FAILURE_INJECTION contains an unsupported value");
			return false;
		}

		if (!gl_probed) {
			gl_vendor = ReadGlString(GL_VENDOR);
			gl_renderer = ReadGlString(GL_RENDERER);
			gl_version = ReadGlString(GL_VERSION);
			gl_probed = true;
		}
		if (!SupportsSkiaGaneshDesktopGl(gl_version)) {
			Fail(
				context,
				SkiaGlDeviceFailure::GlVersionUnsupported,
				"Skia Audio Display requires desktop OpenGL 2.0 or newer; GL_VERSION=" + gl_version);
			return false;
		}
		if (IsSoftwareLikeGlRenderer(gl_vendor, gl_renderer)) {
			Fail(
				context,
				SkiaGlDeviceFailure::SoftwareRendererUnsupported,
				"software-like OpenGL renderer is not enabled for Audio Display; GL_RENDERER=" + gl_renderer);
			return false;
		}
		if (failure_injection == FailureInjection::FrameBegin) {
			Fail(
				context,
				SkiaGlDeviceFailure::FrameBeginInjected,
				"AEGISUB_SKIA_AUDIO_FAILURE_INJECTION requested frame-begin");
			return false;
		}
		if (!device.BeginExternalFrame(context))
			return false;
#ifdef _WIN32
		if (TileDiagnosticsEnabled())
			last_native_context = wglGetCurrentContext();
#endif
		if (!resource_budget_set) {
			device.Get()->setResourceCacheLimit(kGaneshResourceCacheBudget);
			resource_budget_set = true;
		}

		auto const key = MakeSurfaceKey(target);
		if (!surface || !surface_key || *surface_key != key) {
			surface.reset();
			surface_key.reset();

			SkiaFramebufferSurfaceDescriptor descriptor;
			descriptor.width = target.width;
			descriptor.height = target.height;
			descriptor.sample_count = target.sample_count;
			descriptor.stencil_bits = target.stencil_bits;
			descriptor.framebuffer_id = target.framebuffer_id;
			descriptor.bottom_left_origin = target.bottom_left_origin;
			surface = surface_provider.AcquireFramebufferSurface(device.Get(), descriptor);
			if (!surface) {
				Fail(
					context,
					SkiaGlDeviceFailure::SurfaceAcquisitionFailed,
					"failed to wrap the Audio Display back buffer");
				return false;
			}
			surface_key = key;
			++metrics.surface_acquisitions;
		}
		if (!surface->getCanvas()) {
			Fail(context, SkiaGlDeviceFailure::SurfaceAcquisitionFailed, "the wrapped Audio Display surface has no canvas");
			return false;
		}
		return true;
	}

	bool FinishFrame(SkiaGlContextToken context) {
		perf_trace::AudioUiDurationScope trace("audio_display.submit");
		if (!device.FlushAndSubmit(context)) {
			trace.SetDetails(0);
			device.ResetTextureBindingsForExternalUse(context);
			return false;
		}
		++metrics.submits;
		device.ResetTextureBindingsForExternalUse(context);
		trace.SetDetails(1);
		return true;
	}

	std::optional<GpuContentEntry> UploadContentTile(
		ContentUploadPayload const& payload) {
		auto *context = device.Get();
		if (!context || !payload.IsValid())
			return std::nullopt;

		GpuContentEntry uploaded;
		if (payload.key.tile.kind == ContentKind::Waveform) {
			auto const info = SkImageInfo::Make(
				static_cast<int>(payload.width),
				static_cast<int>(payload.height),
				kR16G16B16A16_unorm_SkColorType,
				kUnpremul_SkAlphaType,
				nullptr);
			uploaded.primary = UploadTexture(
				context,
				info,
				payload.primary.data(),
				payload.primary.size(),
				static_cast<std::size_t>(payload.width) * kWaveformUploadBytesPerColumn);
			if (!uploaded.primary)
				return std::nullopt;
			uploaded.bytes = uploaded.primary->textureSize();
			if (!uploaded.bytes)
				uploaded.bytes = payload.primary.size();
		}
		else {
			auto const info = SkImageInfo::Make(
				static_cast<int>(payload.width),
				static_cast<int>(payload.height),
				kRGBA_8888_SkColorType,
				kOpaque_SkAlphaType,
				nullptr);
			uploaded.primary = UploadTexture(
				context,
				info,
				payload.primary.data(),
				payload.primary.size(),
				static_cast<std::size_t>(payload.width) * 4);
			if (!uploaded.primary)
				return std::nullopt;
			uploaded.bytes = uploaded.primary->textureSize();
			if (!uploaded.bytes)
				uploaded.bytes = payload.primary.size();
		}
		return uploaded;
	}

	std::optional<GpuContentEntry> AcquireContentTile(
		ContentUploadPayload const& payload) {
		auto found = content_cache.find(payload.key);
		if (found != content_cache.end()) {
			TouchContent(found->first, found->second);
			++metrics.content_cache_hits;
			return found->second;
		}

		++metrics.content_cache_misses;
		if (!payload.IsValid())
			return std::nullopt;
		auto uploaded = UploadContentTile(payload);
		if (!uploaded)
			return std::nullopt;
		if (uploaded->bytes > content_cache_budget)
			return std::nullopt;

		auto const key = payload.key;
		auto [entry, inserted] = content_cache.emplace(key, std::move(*uploaded));
		if (!inserted)
			return entry->second;
		content_cache_bytes += entry->second.bytes;
		metrics.content_upload_bytes += entry->second.bytes;
		++metrics.content_uploads;
		auto event = MakePayloadEvent("gpu_upload", entry->first);
		event.outcome = "uploaded";
		event.bytes = entry->second.bytes;
		perf_trace::ObserveAudioContentTileEvent(event);
		if (TileDiagnosticsEnabled() && payload.key.tile.kind == ContentKind::Spectrum) {
			entry->second.upload_summary = SummarizeUploadPayload(payload);
#ifdef _WIN32
			entry->second.upload_native_context = wglGetCurrentContext();
#endif
			auto const& summary = entry->second.upload_summary;
			event.stage = "gpu_upload_summary";
			event.include_diagnostics = true;
			event.diagnostic_hash = summary.hash;
			event.diagnostic_elements = summary.element_count;
			event.diagnostic_nonfinite = summary.nonfinite_count;
			event.diagnostic_nonzero_columns = summary.nonzero_columns;
			event.diagnostic_minimum = summary.minimum;
			event.diagnostic_maximum = summary.maximum;
			perf_trace::ObserveAudioContentTileEvent(event);
		}
		TouchContent(entry->first, entry->second);
		TrimContentCache();
		return entry->second;
	}

	sk_sp<SkImage> AcquirePalette(SpectrumPalette const& palette) {
		if (auto found = palette_images.find(palette.revision); found != palette_images.end())
			return found->second;

		auto pixels = EncodePalette(palette);
		auto const info = SkImageInfo::Make(
			kSpectrumPaletteTextureWidth,
			kSpectrumPaletteTextureHeight,
			kRGBA_8888_SkColorType,
			kUnpremul_SkAlphaType,
			SkColorSpace::MakeSRGB());
		auto uploaded = UploadTexture(
			device.Get(),
			info,
			pixels.data(),
			pixels.size(),
			static_cast<std::size_t>(kSpectrumPaletteTextureWidth) * 4);
		if (!uploaded)
			return nullptr;
		if (palette_images.size() >= 8)
			palette_images.erase(palette_images.begin());
		auto [entry, inserted] = palette_images.emplace(palette.revision, std::move(uploaded));
		if (!inserted)
			return entry->second;
		++metrics.palette_uploads;
		return entry->second;
	}
};

Presenter::Presenter(FailureInjection failure_injection)
: impl(std::make_unique<Impl>(failure_injection)) {
	impl->UpdateContentMetrics();
}

Presenter::~Presenter() = default;

bool Presenter::CaptureTileDiagnostics(
	SkiaGlContextToken context,
	FrameTarget const& target,
	ContentFrame const& frame,
	std::filesystem::path const& capture_directory,
	std::string& error) noexcept try {
	error.clear();
	if (!TileDiagnosticsEnabled()) {
		error = "audio tile diagnostics are disabled";
		return false;
	}
	auto const validation = ValidateFrameTarget(target, context.generation);
	if (!validation.valid || target.framebuffer_id != 0) {
		error = "capture requires a valid default-framebuffer target";
		return false;
	}
	auto *gpu_context = impl->device.Get();
	if (!gpu_context || gpu_context->abandoned() || context.identity != impl->last_context.identity || context.generation != impl->last_context.generation) {
		error = "capture does not match the presenter's live context";
		return false;
	}
#ifdef _WIN32
	if (!wglGetCurrentContext() || wglGetCurrentContext() != impl->last_native_context) {
		error = "the current native WGL context differs from the last rendered audio context";
		return false;
	}
#endif
	// This limit applies only to explicit diagnostic allocations, never rendering.
	constexpr std::size_t max_capture_bytes = 256 * 1024 * 1024;
	auto const frame_bytes = static_cast<std::uint64_t>(target.width) * target.height * 4;
	if (frame_bytes > max_capture_bytes) {
		error = "the diagnostic framebuffer exceeds the 256 MiB capture limit";
		return false;
	}
	auto gl = GrGLMakeNativeInterface();
	if (!gl || !gl->fFunctions.fBindFramebuffer || !gl->fFunctions.fBindBuffer) {
		error = "the native GL readback interface is unavailable";
		return false;
	}
	std::filesystem::create_directories(capture_directory);
	std::ofstream metadata(capture_directory / "frame.txt", std::ios::binary);
	metadata.exceptions(std::ios::badbit | std::ios::failbit);
	metadata.imbue(std::locale::classic());
	metadata << "format_version=1\n"
			 << "capture_order=front_framebuffer,content_textures,retained_base\n"
			 << "capture_synchronizes_gpu=true\n"
			 << "context_generation=" << context.generation << '\n'
			 << "context_logical_identity=" << context.identity << '\n'
			 << "context_last_native_identity=" << impl->last_native_context << '\n'
#ifdef _WIN32
			 << "context_current_native_identity=" << static_cast<void const *>(wglGetCurrentContext()) << '\n'
#endif
			 << "gl_vendor=" << ReadGlString(GL_VENDOR) << '\n'
			 << "gl_renderer=" << ReadGlString(GL_RENDERER) << '\n'
			 << "gl_version=" << ReadGlString(GL_VERSION) << '\n'
			 << "width=" << target.width << "\nheight=" << target.height << '\n'
			 << "framebuffer_row_bytes=" << static_cast<std::size_t>(target.width) * 4 << '\n'
			 << "framebuffer_rgba_orientation=bottom_up\nppm_orientation=top_down\n"
			 << "tile_rgba_orientation=top_down\ntile_rgba_layout=R,G,B,A unsigned bytes\n"
			 << "tile_power_encoding=RGB24 unsigned integer; power=RGB24*8/16777215; alpha=255\n"
			 << "hash_algorithm=FNV-1a64 over all raw bytes\n"
			 << "provider_generation=" << frame.generation.provider << '\n'
			 << "analysis_generation=" << frame.generation.analysis << '\n'
			 << "static_revision=" << frame.static_revision << '\n'
			 << "retained_static_revision=" << impl->retained_static_revision << '\n'
			 << "first_column=" << frame.first_column << '\n'
			 << "first_column_offset=" << frame.first_column_offset << '\n'
			 << "content_x=" << frame.x << "\ncontent_y=" << frame.y << '\n'
			 << "content_width=" << frame.width << "\ncontent_height=" << frame.height << '\n'
			 << "amplitude=" << frame.amplitude << '\n'
			 << "frame_tile_count=" << frame.tiles.size() << '\n'
			 << "gpu_cache_entries=" << impl->content_cache.size() << '\n'
			 << "gpu_cache_bytes=" << impl->content_cache_bytes << '\n';
	// Preserve pre-existing GL errors as evidence instead of confusing them with
	// the following read. Reading GL errors is destructive and capture-only.
	for (int i = 0; i < 16; ++i) {
		auto const previous_error = glGetError();
		if (previous_error == GL_NO_ERROR)
			break;
		metadata << "pre_capture_gl_error=" << previous_error << '\n';
	}
	DiagnosticReadbackState restore_state(*gl, *gpu_context, ReadGlString(GL_VERSION));
	restore_state.PrepareFrontRead();
	std::vector<std::uint8_t> front_pixels(static_cast<std::size_t>(frame_bytes));
	glReadPixels(0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, front_pixels.data());
	auto const front_error = glGetError();
	metadata << "front_read_gl_error=" << front_error << '\n';
	if (front_error == GL_NO_ERROR) {
		WriteDiagnosticBytes(capture_directory / "framebuffer-front.rgba", front_pixels);
		WriteDiagnosticPpm(capture_directory / "framebuffer-front.ppm", target.width, target.height, front_pixels, true);
		metadata << "front_hash=" << FormatDiagnosticHash(HashDiagnosticBytes(front_pixels)) << '\n';
	}
	else
		error = "front framebuffer readback failed; remaining evidence was captured";
	// The raw GL read changed bindings outside Ganesh; invalidate its state
	// knowledge before readPixels, without rebuilding or drawing any image.
	gpu_context->resetContext();

	std::ofstream tiles(capture_directory / "tiles.csv");
	tiles.exceptions(std::ios::badbit | std::ios::failbit);
	tiles.imbue(std::locale::classic());
	tiles << "tile_index,provider_generation,analysis_generation,variant_revision,width,height,row_bytes,"
		  << "cpu_hash,cpu_elements,cpu_nonzero_columns,cpu_min,cpu_max,gpu_cache_present,"
		  << "upload_hash,upload_elements,upload_nonzero_columns,upload_min,upload_max,"
		  << "upload_native_context,gpu_read_ok,gpu_hash,gpu_elements,gpu_nonzero_columns,gpu_min,gpu_max,"
		  << "cpu_equals_gpu\n";
	for (auto const& tile : frame.tiles) {
		if (!tile || tile->key.tile.kind != ContentKind::Spectrum)
			continue;
		auto const& key = tile->key;
		auto const stem = "tile-" + std::to_string(key.tile.tile_index);
		auto const current_summary = SummarizeUploadPayload(*tile);
		tiles << key.tile.tile_index << ',' << key.tile.generation.provider << ','
			  << key.tile.generation.analysis << ',' << key.variant_revision << ','
			  << tile->width << ',' << tile->height << ',' << static_cast<std::size_t>(tile->width) * 4 << ',';
		WriteDiagnosticSummary(tiles, current_summary);
		WriteDiagnosticBytes(capture_directory / (stem + "-cpu.rgba"), tile->primary);
		// Do not use AcquireContentTile or TouchContent: absent stays absent, and
		// a capture must not change eviction order or replace suspect uploads.
		auto const cached = impl->content_cache.find(key);
		if (cached == impl->content_cache.end()) {
			tiles << ",0,,,,,,,0,,,,,,\n";
			continue;
		}
		auto const& entry = cached->second;
		tiles << ",1,";
		WriteDiagnosticSummary(tiles, entry.upload_summary);
		tiles << ',' << entry.upload_native_context << ',';
		if (!entry.primary || !tile->HasValidShape() || tile->primary.size() > max_capture_bytes || entry.primary->width() != static_cast<int>(tile->width) || entry.primary->height() != static_cast<int>(tile->height)) {
			tiles << "0,,,,,,\n";
			error = "one or more cached textures could not be captured";
			continue;
		}
		ContentUploadPayload readback;
		readback.key = key;
		readback.width = tile->width;
		readback.height = tile->height;
		readback.primary.resize(tile->primary.size());
		auto const info = SkImageInfo::Make(
			static_cast<int>(tile->width), static_cast<int>(tile->height),
			kRGBA_8888_SkColorType, kOpaque_SkAlphaType, nullptr);
		bool const read_ok = entry.primary->readPixels(gpu_context, info, readback.primary.data(),
													   static_cast<std::size_t>(tile->width) * 4, 0, 0, SkImage::kDisallow_CachingHint);
		tiles << read_ok << ',';
		if (!read_ok) {
			tiles << ",,,,,\n";
			error = "one or more cached textures could not be captured";
			continue;
		}
		WriteDiagnosticBytes(capture_directory / (stem + "-gpu.rgba"), readback.primary);
		WriteDiagnosticSummary(tiles, SummarizeUploadPayload(readback));
		tiles << ',' << (tile->primary == readback.primary) << '\n';
	}
	tiles.close();
	if (impl->retained_base_image) {
		auto const& base = impl->retained_base_image;
		auto const base_bytes = static_cast<std::uint64_t>(base->width()) * base->height() * 4;
		if (base_bytes <= max_capture_bytes) {
			std::vector<std::uint8_t> pixels(static_cast<std::size_t>(base_bytes));
			auto const info = SkImageInfo::Make(base->width(), base->height(),
												kRGBA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
			bool const read_ok = base->readPixels(gpu_context, info, pixels.data(),
												  static_cast<std::size_t>(base->width()) * 4, 0, 0, SkImage::kDisallow_CachingHint);
			metadata << "retained_base_read_ok=" << read_ok << '\n'
					 << "retained_base_width=" << base->width() << "\nretained_base_height=" << base->height() << '\n';
			if (read_ok) {
				WriteDiagnosticPpm(capture_directory / "retained-base.ppm", base->width(), base->height(), pixels, false);
				metadata << "retained_base_hash=" << FormatDiagnosticHash(HashDiagnosticBytes(pixels)) << '\n';
			}
		}
	}
	metadata << "capture_complete=" << error.empty() << '\n';
	metadata.close();
	return error.empty();
}
catch (std::exception const& exception) {
	error = std::string("audio tile capture failed: ") + exception.what();
	return false;
}
catch (...) {
	error = "audio tile capture failed with an unknown exception";
	return false;
}

bool Presenter::RenderDiagnosticFrame(
	SkiaGlContextToken context,
	FrameTarget const& target) try {
	++impl->metrics.frame_attempts;
	if (!impl->PrepareFrame(context, target))
		return false;

	auto *canvas = impl->surface->getCanvas();
	canvas->clear(SkColorSetRGB(24, 34, 48));
	SkPaint paint;
	paint.setAntiAlias(false);
	paint.setColor(SkColorSetRGB(42, 157, 143));
	canvas->drawRect(SkRect::MakeXYWH(0.f, 0.f, target.width * 0.32f, static_cast<float>(target.height)), paint);
	paint.setColor(SkColorSetRGB(233, 196, 106));
	canvas->drawRect(SkRect::MakeXYWH(
		target.width * 0.32f,
		target.height * 0.58f,
		target.width * 0.68f,
		target.height * 0.42f), paint);
	return impl->FinishFrame(context);
}
catch (std::exception const& err) {
	impl->Fail(context, SkiaGlDeviceFailure::SurfaceAcquisitionFailed, err.what());
	return false;
}
catch (...) {
	impl->Fail(context, SkiaGlDeviceFailure::SurfaceAcquisitionFailed, "an unknown exception escaped the Audio Display presenter");
	return false;
}

bool Presenter::RenderContentFrame(
	SkiaGlContextToken context,
	FrameTarget const& target,
	ContentFrame const& frame) try {
	++impl->metrics.frame_attempts;
	impl->metrics.last_frame_trace = {};
	if (!impl->PrepareFrame(context, target))
		return false;
	if (auto const error = ValidateContentFrame(target, frame); !error.empty()) {
		impl->Fail(context, SkiaGlDeviceFailure::InvalidFrameTarget, error);
		return false;
	}

	auto const trace_enabled = perf_trace::IsCategoryEnabled(perf_trace::Category::Audio);
	PresenterFrameTrace frame_trace;
	auto *trace = trace_enabled ? &frame_trace : nullptr;
	auto const compose_trace_started = BeginFrameTrace(trace_enabled);
	auto const base_trace_started = BeginFrameTrace(trace_enabled);
	auto *canvas = impl->surface->getCanvas();
	canvas->clear(static_cast<SkColor>(frame.background_color));
	SkRect const content_bounds = SkRect::MakeXYWH(
		static_cast<float>(frame.x),
		static_cast<float>(frame.y),
		static_cast<float>(frame.width),
		static_cast<float>(frame.height));
	SkPaint paint;
	paint.setAntiAlias(false);
	StyleFrame default_style;
	std::span<StyleFrame const> styles = frame.styles;
	if (styles.empty()) {
		default_style.x = frame.x;
		default_style.width = frame.width;
		default_style.background_color = frame.background_color;
		default_style.waveform_peak_color = frame.waveform_peak_color;
		default_style.waveform_average_color = frame.waveform_average_color;
		default_style.waveform_zero_color = frame.waveform_zero_color;
		default_style.spectrum_palette = frame.spectrum_palette;
		styles = std::span<StyleFrame const>(&default_style, 1);
	}
	for (auto const& style : styles) {
		paint.setColor(static_cast<SkColor>(style.background_color));
		canvas->drawRect(SkRect::MakeXYWH(style.x, frame.y, style.width, frame.height), paint);
	}

	if (frame.kind == ContentKind::Waveform && !impl->waveform_effect) {
		impl->Fail(
			context,
			SkiaGlDeviceFailure::ContentShaderUnavailable,
			"Skia waveform runtime effect failed to compile: " + impl->waveform_effect_error);
		return false;
	}
	if (frame.kind == ContentKind::Spectrum) {
		if (!impl->spectrum_effect) {
			impl->Fail(
				context,
				SkiaGlDeviceFailure::ContentShaderUnavailable,
				"Skia spectrum runtime effect failed to compile: " + impl->spectrum_effect_error);
			return false;
		}
		for (auto const& style : styles) {
			if (!impl->AcquirePalette(*style.spectrum_palette)) {
				impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, "failed to upload an Audio spectrum style palette");
				return false;
			}
		}
	}

	canvas->save();
	canvas->clipRect(content_bounds);
	for (auto const& tile : frame.tiles) {
		if (!tile
			|| !tile->IsValid()
			|| tile->key.tile.generation != frame.generation
			|| tile->key.tile.kind != frame.kind
			|| (frame.kind == ContentKind::Waveform
				&& tile->key.variant_revision != kWaveformUploadPayloadRevision)
			|| (frame.kind == ContentKind::Spectrum
				&& (tile->key.tile.spectrum_bin_count != frame.spectrum_band_plan->bin_count
					|| tile->key.variant_revision != frame.spectrum_band_plan->revision
					|| tile->height != static_cast<std::uint32_t>(frame.spectrum_band_plan->output_height)))
			|| tile->key.tile.tile_index
				> std::numeric_limits<std::uint64_t>::max() / tile->key.tile.column_count) {
			++impl->metrics.content_tiles_skipped;
			continue;
		}

		auto const& tile_key = tile->key.tile;
		auto const tile_first = tile_key.tile_index * tile_key.column_count;
		double relative_x = tile_first >= frame.first_column
			? static_cast<double>(tile_first - frame.first_column)
			: -static_cast<double>(frame.first_column - tile_first);
		if (relative_x >= frame.width
			|| relative_x + tile_key.column_count <= 0.0) {
			continue;
		}

		auto gpu_tile = impl->AcquireContentTile(*tile);
		if (!gpu_tile) {
			impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, "failed to upload or retain an Audio content tile");
			return false;
		}
		auto const destination_x = frame.x + frame.first_column_offset + static_cast<float>(relative_x);

		if (frame.kind == ContentKind::Waveform) {
			SkRect const destination = SkRect::MakeXYWH(
				destination_x,
				frame.y,
				static_cast<float>(tile_key.column_count),
				frame.height);
			auto endpoint_shader = gpu_tile->primary->makeRawShader(
				SkSamplingOptions(SkFilterMode::kNearest),
				nullptr);
			if (!endpoint_shader) {
				impl->Fail(context, SkiaGlDeviceFailure::ContentShaderUnavailable,
					"failed to create a waveform endpoint child shader");
				return false;
			}
			for (auto const& style : styles) {
				auto const style_bounds = SkRect::MakeXYWH(style.x, frame.y, style.width, frame.height);
				if (!SkRect::Intersects(style_bounds, destination))
					continue;
				SkRuntimeShaderBuilder builder(impl->waveform_effect);
				builder.child("endpoint_texture") = endpoint_shader;
				builder.uniform("height") = frame.height;
				builder.uniform("amplitude") = std::clamp(frame.amplitude, 0.f, 64.f);
				builder.uniform("draw_average") = frame.draw_waveform_average ? 1.f : 0.f;
				builder.uniform("peak_color") = SkColor4f::FromColor(
					static_cast<SkColor>(style.waveform_peak_color));
				builder.uniform("average_color") = SkColor4f::FromColor(
					static_cast<SkColor>(style.waveform_average_color));
				auto shader = builder.makeShader();
				if (!shader) {
					impl->Fail(context, SkiaGlDeviceFailure::ContentShaderUnavailable,
						"failed to instantiate the waveform runtime shader");
					return false;
				}

				paint.reset();
				paint.setAntiAlias(false);
				paint.setShader(std::move(shader));
				canvas->save();
				canvas->clipRect(style_bounds);
				canvas->translate(destination_x, frame.y);
				canvas->drawRect(SkRect::MakeWH(
					static_cast<float>(tile_key.column_count),
					frame.height), paint);
				canvas->restore();
			}
		}
		else {
			SkRect const destination = SkRect::MakeXYWH(
				destination_x,
				frame.y,
				static_cast<float>(tile_key.column_count),
				frame.height);
			for (auto const& style : styles) {
				auto const style_bounds = SkRect::MakeXYWH(style.x, frame.y, style.width, frame.height);
				if (!SkRect::Intersects(style_bounds, destination))
					continue;
				auto power_shader = gpu_tile->primary->makeRawShader(
					SkSamplingOptions(SkFilterMode::kLinear),
					nullptr);
				auto palette = impl->AcquirePalette(*style.spectrum_palette);
				if (!palette) {
					impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, "failed to retain an Audio spectrum style palette");
					return false;
				}
				auto palette_shader = palette->makeShader(
					SkSamplingOptions(SkFilterMode::kNearest),
					nullptr);
				if (!power_shader || !palette_shader) {
					impl->Fail(context, SkiaGlDeviceFailure::ContentShaderUnavailable, "failed to create a spectrum child shader");
					return false;
				}
				SkRuntimeShaderBuilder builder(impl->spectrum_effect);
				builder.child("power_texture") = std::move(power_shader);
				builder.child("palette_texture") = std::move(palette_shader);
				builder.uniform("amplitude") = std::clamp(frame.amplitude, 0.f, 64.f);
				auto shader = builder.makeShader();
				if (!shader) {
					impl->Fail(context, SkiaGlDeviceFailure::ContentShaderUnavailable, "failed to instantiate the spectrum runtime shader");
					return false;
				}

				paint.reset();
				paint.setShader(std::move(shader));
				canvas->save();
				canvas->clipRect(style_bounds);
				canvas->translate(destination_x, frame.y);
				canvas->drawRect(SkRect::MakeWH(
					static_cast<float>(tile_key.column_count),
					frame.height), paint);
				canvas->restore();
			}
		}
		++impl->metrics.content_tiles_drawn;
	}

	if (frame.kind == ContentKind::Waveform) {
		for (auto const& style : styles) {
			paint.reset();
			paint.setAntiAlias(false);
			paint.setColor(static_cast<SkColor>(style.waveform_zero_color));
			FillDeviceRect(
				canvas,
				style.x,
				frame.y + std::floor(frame.height * 0.5f),
				style.width,
				1.f,
				paint);
		}
	}
	canvas->restore();
	auto retained_base_image = frame.static_revision
		? impl->surface->makeImageSnapshot()
		: sk_sp<SkImage>{};
	if (trace) {
		trace->base_layer_rebuild_ms = EndFrameTrace(base_trace_started);
		trace->tile_count = static_cast<int>(frame.tiles.size());
		trace->style_count = static_cast<int>(styles.size());
	}
	DrawTimelineLayer(canvas, frame);
	auto marker_image = frame.static_revision
		? RasterizeMarkerLayer(impl->device.Get(), target, frame, trace)
		: sk_sp<SkImage>{};
	if (marker_image)
		canvas->drawImage(marker_image, 0.f, 0.f);
	else
		DrawMarkerLayer(canvas, frame, trace);
	DrawTimingLabelLayer(canvas, frame, trace);
	DrawCursorLayer(canvas, target, frame, false);
	DrawCursorLayer(canvas, target, frame, true);
	DrawScrollbarLayer(canvas, target, frame, trace);

	if (frame.static_revision) {
		auto timeline = RecordLayerPicture(target, [&frame](SkCanvas *recording) {
			DrawTimelineLayer(recording, frame);
		});
		auto timing_labels = RecordLayerPicture(target, [&frame](SkCanvas *recording) {
			DrawTimingLabelLayer(recording, frame, nullptr);
		});
		auto post_cursor = RecordLayerPicture(target, [&target, &frame](SkCanvas *recording) {
			DrawScrollbarLayer(recording, target, frame, nullptr);
		});
		if (!retained_base_image || !timeline || !marker_image || !timing_labels || !post_cursor) {
			impl->ResetRetainedLayers();
		}
		else {
			impl->retained_static_revision = frame.static_revision;
			impl->retained_surface_key = MakeSurfaceKey(target);
			impl->retained_base_image = std::move(retained_base_image);
			impl->retained_timeline = std::move(timeline);
			impl->retained_markers = std::move(marker_image);
			impl->retained_timing_labels = std::move(timing_labels);
			impl->retained_post_cursor = std::move(post_cursor);
			impl->retained_layers_ready = true;
		}
	}
	else {
		impl->ResetRetainedLayers();
	}
	if (trace) {
		trace->frame_compose_ms = EndFrameTrace(compose_trace_started);
		trace->valid = true;
	}
	auto const finished = impl->FinishFrame(context);
	if (finished && trace)
		impl->metrics.last_frame_trace = frame_trace;
	return finished;
}
catch (std::exception const& err) {
	impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, err.what());
	return false;
}
catch (...) {
	impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, "an unknown exception escaped Audio retained content rendering");
	return false;
}

bool Presenter::RenderCursorFrame(
	SkiaGlContextToken context,
	FrameTarget const& target,
	ContentFrame const& frame) {
	return RenderRetainedOverlayFrame(context, target, frame, Layer::Cursor);
}

bool Presenter::RenderRetainedOverlayFrame(
	SkiaGlContextToken context,
	FrameTarget const& target,
	ContentFrame const& frame,
	Layer updated_layers) try {
	++impl->metrics.frame_attempts;
	impl->metrics.last_frame_trace = {};
	if (!CanRenderRetainedOverlay(updated_layers)
		|| !frame.static_revision
		|| !impl->retained_layers_ready
		|| impl->retained_static_revision != frame.static_revision
		|| impl->retained_surface_key != MakeSurfaceKey(target)
		|| !impl->retained_base_image
		|| !impl->retained_timeline
		|| !impl->retained_markers
		|| !impl->retained_timing_labels
		|| !impl->retained_post_cursor) {
		return false;
	}
	sk_sp<SkImage> updated_markers;
	auto const trace_enabled = perf_trace::IsCategoryEnabled(perf_trace::Category::Audio);
	PresenterFrameTrace frame_trace;
	frame_trace.retained_layers_reused = true;
	frame_trace.cursor_only = updated_layers == Layer::Cursor;
	frame_trace.base_layer_rebuild_ms = -1.0;
	if (!impl->PrepareFrame(context, target))
		return false;
	if (HasLayer(updated_layers, Layer::Marker)) {
		updated_markers = RasterizeMarkerLayer(
			impl->device.Get(),
			target,
			frame,
			trace_enabled ? &frame_trace : nullptr);
		if (!updated_markers)
			return false;
	}

	auto const compose_trace_started = BeginFrameTrace(trace_enabled);
	auto *canvas = impl->surface->getCanvas();
	canvas->clear(SK_ColorTRANSPARENT);
	canvas->drawImage(impl->retained_base_image, 0.f, 0.f);
	canvas->drawPicture(impl->retained_timeline);
	if (updated_markers)
		impl->retained_markers = std::move(updated_markers);
	canvas->drawImage(impl->retained_markers, 0.f, 0.f);
	canvas->drawPicture(impl->retained_timing_labels);
	DrawCursorLayer(canvas, target, frame, false);
	DrawCursorLayer(canvas, target, frame, true);
	canvas->drawPicture(impl->retained_post_cursor);
	if (trace_enabled) {
		frame_trace.frame_compose_ms = EndFrameTrace(compose_trace_started);
		frame_trace.valid = true;
	}
	auto const finished = impl->FinishFrame(context);
	if (finished && trace_enabled)
		impl->metrics.last_frame_trace = frame_trace;
	return finished;
}
catch (std::exception const& err) {
	impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, err.what());
	return false;
}
catch (...) {
	impl->Fail(context, SkiaGlDeviceFailure::ContentUploadFailed, "an unknown exception escaped Audio retained overlay rendering");
	return false;
}

void Presenter::SetContentCacheBudget(std::size_t budget_bytes) {
	impl->content_cache_budget = std::max<std::size_t>(1, budget_bytes);
	impl->TrimContentCache();
}

void Presenter::SetFailureInjection(FailureInjection failure_injection) noexcept {
	impl->failure_injection = failure_injection;
	impl->device.SetFailureInjection(DeviceFailureInjection(failure_injection));
}

void Presenter::Fail(
	SkiaGlContextToken context,
	SkiaGlDeviceFailure failure,
	std::string detail) noexcept {
	impl->Fail(context, failure, std::move(detail));
}

void Presenter::Release(SkiaGlContextToken context) noexcept {
	impl->last_context = context;
	impl->surface.reset();
	impl->surface_key.reset();
	impl->ResetContentCache();
	impl->ResetRetainedLayers();
	impl->device.ReleaseResourcesAndAbandon(context);
}

void Presenter::Abandon() noexcept {
	impl->device.Abandon();
	impl->surface.reset();
	impl->surface_key.reset();
	impl->ResetContentCache();
	impl->ResetRetainedLayers();
}

SkiaGlDeviceHealth Presenter::Health() const noexcept {
	return impl->device.Health();
}

SkiaGlDeviceFailure Presenter::LastFailure() const noexcept {
	return impl->device.LastFailure();
}

PresenterMetrics Presenter::Metrics() const noexcept {
	impl->UpdateContentMetrics();
	return impl->metrics;
}

std::string Presenter::TakeFailureLogMessage() {
	if (impl->failure_logged || impl->device.LastFailure() == SkiaGlDeviceFailure::None)
		return {};
	impl->failure_logged = true;

	std::ostringstream message;
	message
		<< "Skia Audio Display is disabled for this widget: context="
		<< impl->last_context.identity
		<< ", generation=" << impl->last_context.generation
		<< ", health=" << ToString(impl->device.Health())
		<< ", failure=" << ToString(impl->device.LastFailure());
	if (!impl->device.LastFailureDetail().empty())
		message << ", detail=" << impl->device.LastFailureDetail();
	if (!impl->gl_vendor.empty())
		message << ", GL_VENDOR=" << impl->gl_vendor;
	if (!impl->gl_renderer.empty())
		message << ", GL_RENDERER=" << impl->gl_renderer;
	if (!impl->gl_version.empty())
		message << ", GL_VERSION=" << impl->gl_version;
	return message.str();
}

}
