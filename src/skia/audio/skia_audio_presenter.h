#pragma once

#include "skia_audio_content.h"
#include "skia_audio_display_contract.h"
#include "skia_audio_frame_model.h"
#include "skia_audio_upload_payload.h"
#include "../skia_gl_device.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace aegisub::skia::audio {

inline constexpr std::size_t kSpectrumPaletteFactor = 1u << 12;
inline constexpr std::size_t kSpectrumPaletteColorCount = kSpectrumPaletteFactor + 1;

struct PresenterFrameTrace {
	bool valid = false;
	bool retained_layers_reused = false;
	bool cursor_only = false;
	double frame_compose_ms = 0.0;
	double base_layer_rebuild_ms = 0.0;
	double marker_layer_rebuild_ms = -1.0;
	double label_layer_rebuild_ms = -1.0;
	double scrollbar_layer_rebuild_ms = -1.0;
	int tile_count = 0;
	int style_count = 0;
	int marker_count = 0;
	int markers_drawn = 0;
	int label_count = 0;
	int labels_drawn = 0;
	bool scrollbar_selection_visible = false;
	bool scrollbar_load_visible = false;
};

struct PresenterMetrics {
	std::uint64_t frame_attempts = 0;
	std::uint64_t surface_acquisitions = 0;
	std::uint64_t submits = 0;
	std::uint64_t content_tiles_drawn = 0;
	std::uint64_t content_tiles_skipped = 0;
	std::uint64_t content_cache_hits = 0;
	std::uint64_t content_cache_misses = 0;
	std::uint64_t content_uploads = 0;
	std::uint64_t content_upload_bytes = 0;
	std::uint64_t content_evictions = 0;
	std::uint64_t palette_uploads = 0;
	std::size_t content_cache_entries = 0;
	std::size_t content_cache_bytes = 0;
	std::size_t content_cache_budget_bytes = 0;
	PresenterFrameTrace last_frame_trace;
};

struct SpectrumPalette {
	std::uint64_t revision = 0;
	// SkColor-compatible AARRGGBB entries. Legacy spectrum rendering uses a
	// 12-bit lookup table, including the saturated endpoint.
	std::array<std::uint32_t, kSpectrumPaletteColorCount> colors {};
};

struct StyleFrame {
	float x = 0.f;
	float width = 0.f;
	std::uint32_t background_color = 0xFF182230;
	std::uint32_t waveform_peak_color = 0xFF2A9D8F;
	std::uint32_t waveform_average_color = 0xFFE9C46A;
	std::uint32_t waveform_zero_color = 0xFF8CA0B3;
	std::shared_ptr<SpectrumPalette const> spectrum_palette;
};

enum class MarkerLineStyle : std::uint8_t {
	Solid,
	Dotted,
};

struct MarkerFrame {
	float x = 0.f;
	std::uint32_t color = 0xFFFFFFFF;
	int width = 1;
	std::uint8_t feet = 0;
	std::uint32_t left_foot_color = color;
	std::uint32_t right_foot_color = color;
	MarkerLineStyle line_style = MarkerLineStyle::Solid;
};

struct LabelFrame {
	float x = 0.f;
	float width = 0.f;
	std::string text;
};

/// Base text style for every string the audio display draws (timeline scale
/// labels, timing labels, cursor time). Legacy pulls these from the window's
/// wxFont via wxDC, so the display side mirrors that font here to keep the two
/// renderers metrically identical. Bold is applied per draw site, not here.
struct TextStyleFrame {
	/// Family name of the window UI font. Empty resolves the platform default.
	std::string face;
	/// Em size in device pixels (already multiplied by the content scale).
	float size = 11.f;
};

struct CursorFrame {
	float x = 0.f;
	std::uint32_t color = 0xFFFFFFFF;
	std::string label;
	int position_ms = -1;
	bool playback = false;
	/// Optional font face override for the cursor label, mirroring the legacy
	/// "Audio/Track Cursor/Font Face" option. Empty means use the default face.
	std::string font_face;
};

struct TimelineFrame {
	int y = 0;
	int height = 0;
	int scroll_left = 0;
	double scroll_left_exact = 0.0;
	int duration_ms = 0;
	double milliseconds_per_pixel = 0.0;
	std::uint32_t background_color = 0xFF202020;
	std::uint32_t foreground_color = 0xFFB0B0B0;
};

struct ScrollbarFrame {
	int y = 0;
	int height = 0;
	float content_scale = 1.f;
	int total = 1;
	int page = 1;
	int position = 0;
	int load_position = -1;
	int selection_start = -1;
	int selection_length = 0;
	std::uint32_t background_color = 0xFF202020;
	std::uint32_t thumb_color = 0xFF808080;
	std::uint32_t selection_color = 0xFFB0B0B0;
};

struct ContentFrame {
	ContentGeneration generation;
	// Non-zero values identify a display-side static frame model. Cursor-only
	// frames keep this revision unchanged so the presenter can reuse its
	// retained pre/post-cursor pictures safely.
	std::uint64_t static_revision = 0;
	ContentKind kind = ContentKind::Waveform;
	std::uint64_t first_column = 0;
	float x = 0.f;
	float y = 0.f;
	float width = 0.f;
	float height = 0.f;
	float first_column_offset = 0.f;
	float amplitude = 1.f;
	/// Device pixels per logical pixel, used to scale the fixed pixel offsets
	/// legacy hardcodes (tick heights, label insets, cursor label border).
	float content_scale = 1.f;
	TextStyleFrame text_style;
	std::uint32_t background_color = 0xFF182230;
	std::uint32_t waveform_peak_color = 0xFF2A9D8F;
	std::uint32_t waveform_average_color = 0xFFE9C46A;
	std::uint32_t waveform_zero_color = 0xFF8CA0B3;
	bool draw_waveform_average = true;
	std::shared_ptr<SpectrumPalette const> spectrum_palette;
	std::shared_ptr<SpectrumBandPlan const> spectrum_band_plan;
	std::vector<std::shared_ptr<ContentUploadPayload const>> tiles;
	std::vector<StyleFrame> styles;
	std::vector<MarkerFrame> markers;
	std::vector<LabelFrame> labels;
	std::shared_ptr<CursorFrame const> cursor;
	std::shared_ptr<TimelineFrame const> timeline;
	std::shared_ptr<ScrollbarFrame const> scrollbar;
};

// Backend-only fixed-frame presenter used by P3.3. It owns a Ganesh device for
// one externally current GL context and retains the wrapped back-buffer surface
// while its exact context/size/FBO key remains unchanged.
class Presenter final {
	struct Impl;
	std::unique_ptr<Impl> impl;

public:
	explicit Presenter(FailureInjection failure_injection);
	~Presenter();

	Presenter(Presenter const&) = delete;
	Presenter& operator=(Presenter const&) = delete;

	bool RenderDiagnosticFrame(SkiaGlContextToken context, FrameTarget const& target);
	bool RenderContentFrame(SkiaGlContextToken context, FrameTarget const& target, ContentFrame const& frame);
	bool RenderCursorFrame(SkiaGlContextToken context, FrameTarget const& target, ContentFrame const& frame);
	bool RenderRetainedOverlayFrame(
		SkiaGlContextToken context,
		FrameTarget const& target,
		ContentFrame const& frame,
		Layer updated_layers);
	// Explicit fault capture only: never renders, uploads, swaps, or touches LRU state.
	bool CaptureTileDiagnostics(
		SkiaGlContextToken context,
		FrameTarget const& target,
		ContentFrame const& frame,
		std::filesystem::path const& capture_directory,
		std::string& error) noexcept;
	void SetFailureInjection(FailureInjection failure_injection) noexcept;
	void SetContentCacheBudget(std::size_t budget_bytes);
	void Fail(SkiaGlContextToken context, SkiaGlDeviceFailure failure, std::string detail) noexcept;
	void Release(SkiaGlContextToken context) noexcept;
	void Abandon() noexcept;

	SkiaGlDeviceHealth Health() const noexcept;
	SkiaGlDeviceFailure LastFailure() const noexcept;
	PresenterMetrics Metrics() const noexcept;
	std::string TakeFailureLogMessage();
};

}
