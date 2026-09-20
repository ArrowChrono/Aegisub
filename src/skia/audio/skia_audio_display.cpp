#include "skia_audio_display.h"

#include "skia_audio_content_worker.h"
#include "skia_audio_presenter.h"
#include "skia_audio_tile_diagnostics.h"

#include "../../include/aegisub/context.h"
#include "../../project.h"
#include "../../audio_controller.h"
#include "../../audio_colorscheme.h"
#include "../../audio_marker_drag_dead_zone.h"
#include "../../audio_marker_pixel_aggregation.h"
#include "../../audio_renderer_spectrum.h"
#include "../../audio_scroll_position.h"
#include "../../audio_timing.h"
#include "../../include/aegisub/hotkey.h"
#include "../../navigation_preview_policy.h"
#include "../../options.h"
#include "../../perf_trace.h"
#include "../../video_controller.h"

#include <libaegisub/signal.h>
#include <libaegisub/audio/provider.h>
#include <libaegisub/ass/time.h>
#include <libaegisub/color.h>
#include <libaegisub/log.h>
#include <libaegisub/fs.h>
#include <libaegisub/path.h>
#include <libaegisub/scope_exit.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <wx/dcclient.h>
#include <wx/display.h>
#include <wx/mousestate.h>
#include <wx/thread.h>
#include <wx/timer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <utility>

namespace aegisub::skia::audio {
namespace {

wxDEFINE_EVENT(EVT_SKIA_AUDIO_CONTENT_READY, wxThreadEvent);
wxDEFINE_EVENT(EVT_SKIA_AUDIO_CONTENT_FAILURE, wxThreadEvent);

// Mirror this window's wxFont into the frame so the presenter's Skia text has
// the same face and size as legacy's wxDC text. Legacy simply draws with
// dc.GetFont(), which is the window font; the Skia presenter has no wxDC, so the
// font has to travel through the frame.
//
// The size is an em size in device pixels. On MSW GetContentScaleFactor() is 1.0
// and wx coordinates already are device pixels, so wxFont::GetPixelSize() is
// directly usable. On Apple/GTK3 the factor is the backing-store ratio and wx
// coordinates are logical, so the em size scales up with it to match the
// device-pixel frame geometry.
TextStyleFrame BuildTextStyle(wxWindow const& window, double content_scale) {
	TextStyleFrame style;
	auto const font = window.GetFont();
	if (!font.IsOk())
		return style;

	style.face = font.GetFaceName().utf8_string();
	auto size = static_cast<double>(font.GetPixelSize().GetHeight());
	if (!(size > 0.0)) {
		auto const points = font.GetFractionalPointSize();
		auto const dpi = window.GetDPI().GetHeight();
		if (points > 0.0 && dpi > 0)
			size = points * dpi / 72.0;
	}
	if (!(size > 0.0))
		size = 11.0;
	style.size = static_cast<float>(size * std::max(1.0, content_scale));
	return style;
}

class DeferredAudioUiDuration final {
	char const *phase;
	std::chrono::steady_clock::time_point started;
	int detail_a;
	int detail_b;
	bool active;

public:
	DeferredAudioUiDuration(char const *phase, int detail_a = -1, int detail_b = -1) noexcept
	: phase(phase)
	, detail_a(detail_a)
	, detail_b(detail_b)
	, active(perf_trace::IsEnabled())
	{
		if (active)
			started = std::chrono::steady_clock::now();
	}

	~DeferredAudioUiDuration() noexcept {
		Publish();
	}

	void Publish() noexcept {
		if (!active)
			return;
		active = false;
		auto const duration_ms = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - started).count();
		perf_trace::ObserveAudioUiDuration(
			phase, duration_ms, detail_a, detail_b, duration_ms >= 8.0);
	}
};

int RequestedAudioSwapInterval() noexcept {
	auto const *value = std::getenv("AEGISUB_SKIA_AUDIO_SWAP_INTERVAL");
	if (!value || !*value)
		return 0;
	char *end = nullptr;
	auto const parsed = std::strtol(value, &end, 10);
	return end != value && *end == '\0'
		? static_cast<int>(std::clamp<long>(parsed, 0, 4))
		: 0;
}

bool ConfigureCurrentSwapInterval(int interval) noexcept {
#ifdef _WIN32
	auto const address = wglGetProcAddress("wglSwapIntervalEXT");
	auto const raw = reinterpret_cast<std::intptr_t>(address);
	if (!address || raw == 1 || raw == 2 || raw == 3 || raw == -1)
		return false;
	using SwapIntervalProc = BOOL (WINAPI *)(int);
	return reinterpret_cast<SwapIntervalProc>(address)(interval) == TRUE;
#else
	(void)interval;
	return false;
#endif
}

std::uint32_t ToArgb(wxColour const& color) {
	return 0xFF000000u
		| (static_cast<std::uint32_t>(color.Red()) << 16)
		| (static_cast<std::uint32_t>(color.Green()) << 8)
		| static_cast<std::uint32_t>(color.Blue());
}

std::uint32_t ToArgb(agi::Color const& color) {
	return 0xFF000000u
		| (static_cast<std::uint32_t>(color.r) << 16)
		| (static_cast<std::uint32_t>(color.g) << 8)
		| static_cast<std::uint32_t>(color.b);
}

MarkerFrame BuildMarkerFrame(AudioMarkerPixel const& pixel, double content_scale) {
	auto const pen = pixel.marker->GetStyle();
	auto const color = ToArgb(pen.GetColour());
	auto const foot_color = [color](AudioMarker const *marker) {
		return marker ? ToArgb(marker->GetStyle().GetColour()) : color;
	};
	return {
		static_cast<float>(pixel.x),
		color,
		std::max(1, static_cast<int>(std::lround(pen.GetWidth() * content_scale))),
		static_cast<std::uint8_t>(pixel.feet),
		foot_color(pixel.left_foot_marker),
		foot_color(pixel.right_foot_marker),
		pen.GetStyle() == wxPENSTYLE_DOT
			? MarkerLineStyle::Dotted
			: MarkerLineStyle::Solid,
	};
}

struct UiChromeColors {
	std::uint32_t dark = 0;
	std::uint32_t light = 0;
	std::uint32_t selection = 0;
};

class StyleRangeCollector final : public AudioRenderingStyleRanges {
public:
	std::vector<TimeStyleRange> ranges;

	void AddRange(int start, int end, AudioRenderingStyle style) override {
		if (end <= start || style < AudioStyle_Normal || style >= AudioStyle_MAX)
			return;
		ranges.push_back({ std::max(0, start), std::max(0, end), static_cast<FrameStyle>(style) });
	}
};

int ProviderDurationMs(agi::AudioProvider const *provider) {
	return provider
		? AudioDurationMsFromSamples(provider->GetNumSamples(), provider->GetSampleRate())
		: 0;
}

int RelativeLogicalXFromTime(int time_ms, int scroll_left, double milliseconds_per_pixel) {
	auto const absolute = LegacyLogicalPixelFromTime(time_ms, milliseconds_per_pixel);
	auto const relative = static_cast<std::int64_t>(absolute) - scroll_left;
	return static_cast<int>(std::clamp<std::int64_t>(
		relative,
		std::numeric_limits<int>::min(),
		std::numeric_limits<int>::max()));
}

int SpectrumQuality() {
	int quality = OPT_GET("Audio/Renderer/Spectrum/Quality")->GetInt();
#if defined(WITH_PFFFT) || defined(WITH_FFTW3)
	quality += 2;
#endif
	return std::clamp(quality, 0, 5);
}

std::pair<std::size_t, std::size_t> SpectrumResolution() {
	constexpr std::size_t widths[] = { 8, 9, 9, 9, 10, 11 };
	constexpr std::size_t distances[] = { 8, 8, 7, 6, 6, 5 };
	auto const quality = SpectrumQuality();
	return { widths[quality], distances[quality] };
}

std::size_t ConfiguredSpectrumMemoryBudgetBytes() noexcept {
	constexpr std::size_t bytes_per_mebibyte = 1024 * 1024;
	auto const configured_mebibytes =
		OPT_GET("Audio/Renderer/Spectrum/Memory Max")->GetInt();
	if (configured_mebibytes <= 0)
		return 0;
	auto const maximum_mebibytes =
		std::numeric_limits<std::size_t>::max() / bytes_per_mebibyte;
	if (static_cast<std::uint64_t>(configured_mebibytes) > maximum_mebibytes)
		return std::numeric_limits<std::size_t>::max();
	return static_cast<std::size_t>(configured_mebibytes) * bytes_per_mebibyte;
}

constexpr auto PlaybackOverlayRefreshInterval = std::chrono::milliseconds(50);

#if wxCHECK_VERSION(3, 1, 1)
int gl_attributes[] = {
	WX_GL_RGBA,
	WX_GL_DOUBLEBUFFER,
	WX_GL_STENCIL_SIZE, 8,
	WX_GL_BUFFER_SIZE, 24,
	WX_GL_MIN_ALPHA, 8,
	0,
};
#else
int gl_attributes[] = { WX_GL_RGBA, WX_GL_DOUBLEBUFFER, WX_GL_STENCIL_SIZE, 8, 0 };
#endif

}

struct SkiaAudioDisplay::Impl {
	Impl(
		SkiaAudioDisplay *owner,
		AudioController *audio_controller,
		agi::Context *project_context,
		FailureInjection failure_injection,
		std::uint64_t failure_injection_after_content_frames,
		FailureCallback failure_callback)
	: presenter(std::make_unique<Presenter>(
		failure_injection_after_content_frames
			? FailureInjection::None
			: failure_injection))
	, content_worker([this, owner](ContentGeneration) {
		if (!content_ready_event_pending.exchange(true, std::memory_order_acq_rel))
			wxQueueEvent(owner, new wxThreadEvent(EVT_SKIA_AUDIO_CONTENT_READY));
	}, [owner](ContentWorkerFailure const& failure) {
		auto *event = new wxThreadEvent(EVT_SKIA_AUDIO_CONTENT_FAILURE);
		event->SetPayload(failure);
		wxQueueEvent(owner, event);
	})
	, failure_callback(std::move(failure_callback)) {
		this->audio_controller = audio_controller;
		this->project_context = project_context;
		presentation_timer.SetOwner(owner);
		load_timer.SetOwner(owner);
		middle_seek_timer.SetOwner(owner);
		if (failure_injection_after_content_frames) {
			deferred_failure_injection = failure_injection;
			this->failure_injection_after_content_frames =
				failure_injection_after_content_frames;
		}
	}

	std::atomic<bool> content_ready_event_pending { false };
	std::unique_ptr<wxGLContext> context;
	std::unique_ptr<Presenter> presenter;
	ContentWorker content_worker;
	agi::signal::Connection audio_open_connection;
	agi::signal::Connection playback_position_connection;
	agi::signal::Connection playback_stop_connection;
	agi::signal::Connection timing_controller_connection;
	std::vector<agi::signal::Connection> timing_connections;
	std::vector<agi::signal::Connection> option_connections;
	agi::Context *project_context = nullptr;
	agi::AudioProvider *provider = nullptr;
	AudioController *audio_controller = nullptr;
	ContentAnalysisConfig content_analysis;
	ContentGeneration content_generation;
	ContentViewportRequest last_content_request;
	std::uint64_t last_content_payload_revision = 0;
	ContentCacheBudgetPlan content_budget_plan;
	bool has_last_content_request = false;
	bool content_budget_soft_limit_active = false;
	FrameViewport viewport;
	FrameTarget diagnostic_frame_target;
	int zoom_level = 0;
	int scroll_left = 0;
	int content_scroll_left = 0;
	float amplitude_scale = 1.f;
	std::uint64_t presentation_revision = 1;
	std::array<std::array<std::uint32_t, 4>, AudioStyle_MAX> waveform_style_colors {};
	std::array<UiChromeColors, 2> ui_chrome_colors {};
	bool presentation_colors_ready = false;
	std::array<std::shared_ptr<SpectrumPalette const>, AudioStyle_MAX> spectrum_style_palettes;
	std::shared_ptr<SpectrumBandPlan const> spectrum_band_plan;
	std::shared_ptr<ContentFrame const> last_complete_content_frame;
	std::shared_ptr<ContentFrame const> last_presented_content_frame;
	std::size_t last_presented_visible_tile_count = 0;
	std::size_t last_presented_ready_tile_count = 0;
	bool last_presented_complete_content_viewport = false;
	bool last_presented_retained_content_frame = false;
	std::uint64_t static_frame_revision = 0;
	Revisions revisions;
	Layer dirty_layers = Layer::All;
	bool retained_overlay_pending = false;
	FailureCallback failure_callback;
	wxTimer load_timer;
	wxTimer middle_seek_timer;
	wxTimer presentation_timer;
	bool repaint_pending = false;
	bool paint_queued = false;
	bool swap_interval_attempted = false;
	bool swap_interval_configured = false;
	int swap_interval = 0;
	NavigationPreviewPolicy middle_seek_policy;
	std::int64_t last_decoded_samples = 0;
	std::uint64_t context_generation = 0;
	std::uint64_t trace_frame_sequence = 0;
	bool fallback_requested = false;
	bool content_frame_presented = false;
	FailureInjection deferred_failure_injection = FailureInjection::None;
	std::uint64_t failure_injection_after_content_frames = 0;
	std::uint64_t successful_content_frames = 0;
	bool deferred_failure_injection_armed = false;
	bool has_present_timestamp = false;
	std::chrono::steady_clock::time_point last_present;
	bool has_playback_overlay_refresh = false;
	std::chrono::steady_clock::time_point last_playback_overlay_refresh;
	int presentation_display = wxNOT_FOUND;
	int presentation_refresh_rate = 60;
	int playback_position_ms = -1;
	int mouse_position_ms = -1;
	int mouse_position_x = -1;
	std::vector<AudioMarker *> dragged_markers;
	wxMouseButton dragged_button = wxMOUSE_BTN_NONE;
	AudioMarkerDragDeadZone marker_drag_dead_zone;
	bool timeline_dragging = false;
	bool scrollbar_dragging = false;
	bool middle_seek_active = false;
	int drag_last_x = 0;

	SkiaGlContextToken ContextToken() const noexcept {
		return { context.get(), context_generation };
	}

	bool IsVideoPlaybackActive() const {
		if (!project_context)
			return false;
		auto const core = project_context->GetCore();
		return core.videoController && core.videoController->IsPlaying();
	}

	bool ShouldRefreshPlaybackOverlay(bool force) {
		if (!IsVideoPlaybackActive()) {
			has_playback_overlay_refresh = false;
			return true;
		}

		auto const now = std::chrono::steady_clock::now();
		if (!force
			&& has_playback_overlay_refresh
			&& now - last_playback_overlay_refresh < PlaybackOverlayRefreshInterval)
			return false;

		last_playback_overlay_refresh = now;
		has_playback_overlay_refresh = true;
		return true;
	}

	void ResetPlaybackOverlayRefresh() noexcept {
		has_playback_overlay_refresh = false;
	}

	void InvalidatePresentation() {
		++presentation_revision;
		if (!presentation_revision)
			++presentation_revision;
		for (auto& palette : spectrum_style_palettes)
			palette.reset();
		spectrum_band_plan.reset();
		last_presented_content_frame.reset();
		last_presented_visible_tile_count = 0;
		last_presented_ready_tile_count = 0;
		last_presented_complete_content_viewport = false;
		last_presented_retained_content_frame = false;
		retained_overlay_pending = false;
		presentation_colors_ready = false;
	}
};

SkiaAudioDisplay::SkiaAudioDisplay(
	wxWindow *parent,
	AudioController *controller,
	agi::Context *context,
	FailureInjection failure_injection,
	std::uint64_t failure_injection_after_content_frames,
	FailureCallback failure_callback)
: wxGLCanvas(
	parent,
	wxID_ANY,
	gl_attributes,
	wxDefaultPosition,
	wxDefaultSize,
	wxFULL_REPAINT_ON_RESIZE | wxWANTS_CHARS | wxBORDER_SIMPLE)
, impl(std::make_unique<Impl>(
	this,
	controller,
	context,
	failure_injection,
	failure_injection_after_content_frames,
	std::move(failure_callback)))
{
	impl->project_context = context;
	impl->audio_open_connection = context->GetCore().project->AddAudioProviderListener(
		&SkiaAudioDisplay::OnAudioOpen,
		this);
	impl->playback_position_connection = controller->AddPlaybackPositionListener(
		&SkiaAudioDisplay::OnPlaybackPosition,
		this);
	impl->playback_stop_connection = controller->AddPlaybackStopListener(
		&SkiaAudioDisplay::OnPlaybackStop,
		this);
	impl->timing_controller_connection = controller->AddTimingControllerListener(
		&SkiaAudioDisplay::OnTimingControllerChanged,
		this);
	impl->option_connections = agi::signal::make_vector({
		OPT_SUB("Audio/Spectrum", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
		OPT_SUB("Audio/Renderer/Spectrum/Quality", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
		OPT_SUB("Audio/Renderer/Spectrum/Input Format", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
		OPT_SUB("Audio/Renderer/Spectrum/Computation Mode", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
		OPT_SUB("Audio/Renderer/Spectrum/FreqCurve", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
		OPT_SUB("Audio/Renderer/Spectrum/Mono Mix Mode", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
		OPT_SUB("Audio/Renderer/Spectrum/Memory Max", &SkiaAudioDisplay::OnCacheBudgetChanged, this),
		OPT_SUB("Audio/Display/Waveform Style", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
		OPT_SUB("Colour/Audio Display/Spectrum", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
		OPT_SUB("Colour/Audio Display/Waveform", &SkiaAudioDisplay::OnRenderingSettingsChanged, this),
	});
	SetMinClientSize(wxSize(-1, 70));
	SetBackgroundStyle(wxBG_STYLE_PAINT);
	SetThemeEnabled(false);
	Bind(wxEVT_PAINT, &SkiaAudioDisplay::OnPaint, this);
	Bind(wxEVT_ERASE_BACKGROUND, &SkiaAudioDisplay::OnEraseBackground, this);
	Bind(wxEVT_SIZE, &SkiaAudioDisplay::OnSize, this);
	Bind(wxEVT_DPI_CHANGED, &SkiaAudioDisplay::OnDPIChanged, this);
	Bind(EVT_SKIA_AUDIO_CONTENT_READY, &SkiaAudioDisplay::OnContentReady, this);
	Bind(EVT_SKIA_AUDIO_CONTENT_FAILURE, &SkiaAudioDisplay::OnContentFailure, this);
	Bind(wxEVT_TIMER, &SkiaAudioDisplay::OnPresentationTimer, this, impl->presentation_timer.GetId());
	Bind(wxEVT_TIMER, &SkiaAudioDisplay::OnLoadTimer, this, impl->load_timer.GetId());
	Bind(wxEVT_TIMER, &SkiaAudioDisplay::OnMiddleSeekTimer, this, impl->middle_seek_timer.GetId());
	Bind(wxEVT_LEFT_DOWN, &SkiaAudioDisplay::OnMouseEvent, this);
	Bind(wxEVT_LEFT_UP, &SkiaAudioDisplay::OnMouseEvent, this);
	Bind(wxEVT_RIGHT_DOWN, &SkiaAudioDisplay::OnMouseEvent, this);
	Bind(wxEVT_RIGHT_UP, &SkiaAudioDisplay::OnMouseEvent, this);
	Bind(wxEVT_MIDDLE_DOWN, &SkiaAudioDisplay::OnMouseEvent, this);
	Bind(wxEVT_MIDDLE_UP, &SkiaAudioDisplay::OnMouseEvent, this);
	Bind(wxEVT_AUX1_DOWN, &SkiaAudioDisplay::OnMouseEvent, this);
	Bind(wxEVT_AUX2_DOWN, &SkiaAudioDisplay::OnMouseEvent, this);
	Bind(wxEVT_MOTION, &SkiaAudioDisplay::OnMouseEvent, this);
	Bind(wxEVT_ENTER_WINDOW, &SkiaAudioDisplay::OnMouseEnter, this);
	Bind(wxEVT_LEAVE_WINDOW, &SkiaAudioDisplay::OnMouseLeave, this);
	Bind(wxEVT_MOUSE_CAPTURE_LOST, &SkiaAudioDisplay::OnMouseCaptureLost, this);
	Bind(wxEVT_SET_FOCUS, &SkiaAudioDisplay::OnFocus, this);
	Bind(wxEVT_KILL_FOCUS, &SkiaAudioDisplay::OnFocus, this);
	Bind(wxEVT_CHAR_HOOK, &SkiaAudioDisplay::OnKeyDown, this);
	Bind(wxEVT_KEY_DOWN, &SkiaAudioDisplay::OnKeyDown, this);
	OnTimingControllerChanged();
}

SkiaAudioDisplay::~SkiaAudioDisplay() {
	if (!impl)
		return;
	CancelMiddleSeek();
	if (!impl->presenter)
		return;
	if (impl->context && impl->context->IsOK() && SetCurrent(*impl->context))
		impl->presenter->Release(impl->ContextToken());
	else
		impl->presenter->Abandon();
	impl->presenter.reset();
	impl->context.reset();
}

void SkiaAudioDisplay::RebuildViewport() {
	if (!impl)
		return;
	auto const size = GetClientSize();
	int text_width = 0;
	int text_height = 0;
	GetTextExtent(wxS("0123456789:."), &text_width, &text_height);
	FrameViewportRequest request;
	request.logical_width = size.GetWidth();
	request.logical_height = size.GetHeight();
	request.content_scale = std::max(1.0, static_cast<double>(GetContentScaleFactor()));
	request.timeline_height = text_height + 4;
	request.scrollbar_height = 15;
	request.scroll_left = impl->content_scroll_left;
	request.duration_ms = ProviderDurationMs(impl->provider);
	request.milliseconds_per_logical_pixel =
		AudioMillisecondsPerLogicalPixel(impl->zoom_level);
	impl->viewport = BuildFrameViewport(request);
	if (impl->viewport.IsValid()) {
		impl->content_scroll_left = impl->viewport.scroll_left;
		auto const maximum_scroll = std::max(0,
			impl->viewport.logical_audio_width - request.logical_width);
		impl->scroll_left = std::clamp(impl->scroll_left, 0, maximum_scroll);
	}
}

void SkiaAudioDisplay::ReconfigureAnalysis() {
	if (!impl)
		return;
	ContentAnalysisConfig config;
	if (OPT_GET("Audio/Spectrum")->GetBool()) {
		config.kind = ContentKind::Spectrum;
		auto const input_format = std::clamp<int>(
			static_cast<int>(OPT_GET("Audio/Renderer/Spectrum/Input Format")->GetInt()),
			0,
			1);
		config.source_mode = input_format == 0
			? ContentSourceMode::Int16Mono
			: ContentSourceMode::FloatInterleaved;
		auto const [derivation_size, derivation_distance] = SpectrumResolution();
		config.spectrum_derivation_size = derivation_size;
		config.spectrum_derivation_distance = derivation_distance;
		config.spectrum_channel_mode = SpectrumChannelMode::MixedMono;
		if (input_format == 1) {
			auto const mono_mode = std::clamp<int>(
				static_cast<int>(OPT_GET("Audio/Renderer/Spectrum/Mono Mix Mode")->GetInt()),
				0,
				2);
			config.spectrum_channel_mode = static_cast<SpectrumChannelMode>(mono_mode);
		}
	}
	else {
		config.kind = ContentKind::Waveform;
		config.source_mode = ContentSourceMode::Int16Mono;
	}
	auto const scale = std::max(1.0, static_cast<double>(GetContentScaleFactor()));
	config.milliseconds_per_pixel = AudioMillisecondsPerLogicalPixel(impl->zoom_level) / scale;
	config.mix_policy = AudioMixPolicy::MonoAverage;
	impl->content_analysis = config;
	auto const old_generation = impl->content_generation;
	impl->content_generation = impl->content_worker.SetAnalysis(config);
	if (impl->content_generation != old_generation) {
		impl->has_last_content_request = false;
		impl->last_complete_content_frame.reset();
	}
	impl->spectrum_band_plan.reset();
	RebuildViewport();
	RequestVisibleContent();
}

bool SkiaAudioDisplay::EnsureSpectrumBandPlan() {
	if (!impl
		|| impl->content_analysis.kind != ContentKind::Spectrum)
		return true;
	if (!impl->provider || !impl->viewport.IsValid())
		return false;

	auto const bin_count = static_cast<std::uint32_t>(
		std::size_t { 1 } << impl->content_analysis.spectrum_derivation_size);
	if (impl->spectrum_band_plan
		&& impl->spectrum_band_plan->bin_count == bin_count
		&& impl->spectrum_band_plan->output_height == impl->viewport.content.height) {
		return true;
	}

	SpectrumBandPlanRequest request;
	request.bin_count = bin_count;
	request.output_height = impl->viewport.content.height;
	request.sample_rate = impl->provider->GetSampleRate();
	request.mode = OPT_GET("Audio/Renderer/Spectrum/Computation Mode")->GetInt() == 0
		? SpectrumScaleMode::LegacyLinear
		: SpectrumScaleMode::FrequencyCurve;
	request.frequency_reference_position = SpectrumFrequencyReferenceForPreset(
		OPT_GET("Audio/Renderer/Spectrum/FreqCurve")->GetInt());
	auto plan = std::make_shared<SpectrumBandPlan>(BuildSpectrumBandPlan(request));
	if (!plan->IsValid()) {
		RequestFallback("Skia Audio Display could not build a valid spectrum band plan");
		return false;
	}
	impl->spectrum_band_plan = std::move(plan);
	return true;
}

void SkiaAudioDisplay::RequestVisibleContent() {
	if (!impl || !impl->provider || !impl->viewport.IsValid() || !impl->content_analysis.IsValid())
		return;
	if (!EnsureSpectrumBandPlan())
		return;
	ContentViewportRequest request;
	request.generation = impl->content_generation;
	request.kind = impl->content_analysis.kind;
	request.first_column = impl->viewport.first_column;
	request.column_count = impl->viewport.visible_column_count;
	request.tile_column_count = 256;
	request.prefetch_tile_count = 1;
	if (request.kind == ContentKind::Spectrum)
		request.spectrum_bin_count = static_cast<std::uint32_t>(
			std::size_t { 1 } << impl->content_analysis.spectrum_derivation_size);
	auto const payload_revision = request.kind == ContentKind::Waveform
		? kWaveformUploadPayloadRevision : impl->spectrum_band_plan->revision;
	if (impl->has_last_content_request
		&& impl->last_content_request == request
		&& impl->last_content_payload_revision == payload_revision) {
		return;
	}
	auto const budget_plan = PlanContentCacheBudget(
		request,
		ConfiguredSpectrumMemoryBudgetBytes(),
		request.kind == ContentKind::Spectrum
			? static_cast<std::uint32_t>(impl->spectrum_band_plan->output_height) : 0);
	if (!budget_plan.valid) {
		RequestFallback("Skia Audio content cache budget planning overflowed or rejected the viewport");
		return;
	}
	impl->content_worker.SetCacheBudgets({
		budget_plan.content_budget_bytes,
		budget_plan.payload_budget_bytes,
		budget_plan.spectrum_analysis_budget_bytes,
	});
	impl->content_budget_plan = budget_plan;
	if (budget_plan.soft_limit_exceeded && !impl->content_budget_soft_limit_active) {
		LOG_W("audio/display/skia")
			<< "Skia Audio spectrum cache raised the configured soft limit from "
			<< budget_plan.configured_total_bytes << " to "
			<< budget_plan.effective_total_bytes
			<< " bytes to retain the visible working set";
	}
	impl->content_budget_soft_limit_active = budget_plan.soft_limit_exceeded;
	impl->last_content_request = request;
	impl->last_content_payload_revision = payload_revision;
	impl->has_last_content_request = true;
	impl->content_worker.Request(request, impl->spectrum_band_plan);
}

bool SkiaAudioDisplay::HasCompleteVisibleContent() const {
	if (!impl || !impl->provider || !impl->viewport.IsValid() || !impl->content_analysis.IsValid())
		return false;
	ContentViewportRequest request;
	request.generation = impl->content_generation;
	request.kind = impl->content_analysis.kind;
	request.first_column = impl->viewport.first_column;
	request.column_count = impl->viewport.visible_column_count;
	request.tile_column_count = 256;
	if (request.kind == ContentKind::Spectrum)
		request.spectrum_bin_count = static_cast<std::uint32_t>(
			std::size_t { 1 } << impl->content_analysis.spectrum_derivation_size);
	auto const visible_tiles = PlanVisibleContentTiles(request);
	return !visible_tiles.empty()
		&& std::all_of(visible_tiles.begin(), visible_tiles.end(), [this](ContentTileKey const& key) {
			return static_cast<bool>(impl->content_worker.FindPayload(
				MakeContentUploadPayloadKey(key, impl->spectrum_band_plan.get())));
		});
}

void SkiaAudioDisplay::CommitScrollbarContentViewport() {
	if (!impl)
		return;
	perf_trace::AudioUiDurationScope trace("audio_display.scrollbar_content_commit");
	auto const old_content_scroll_left = impl->content_scroll_left;
	impl->content_scroll_left = impl->scroll_left;
	RebuildViewport();
	RequestVisibleContent();
	if (impl->content_scroll_left != old_content_scroll_left)
		Invalidate(Change::Scroll);
	trace.SetDetails(
		std::abs(impl->content_scroll_left - old_content_scroll_left),
		impl->content_analysis.kind == ContentKind::Spectrum ? 1 : 0);
}

void SkiaAudioDisplay::OnRenderingSettingsChanged() {
	impl->InvalidatePresentation();
	Invalidate(Change::Palette);
	auto const old_generation = impl->content_generation;
	ReconfigureAnalysis();
	if (impl->content_generation != old_generation)
		Invalidate(Change::AnalysisSettings);
	RequestRepaint(true);
}

void SkiaAudioDisplay::OnCacheBudgetChanged() {
	if (!impl)
		return;
	impl->has_last_content_request = false;
	Invalidate(Change::AnalysisSettings);
	RequestVisibleContent();
	RequestRepaint();
}

void SkiaAudioDisplay::OnPaint(wxPaintEvent&) try {
	wxPaintDC paint_dc(this);
	impl->repaint_pending = false;
	impl->paint_queued = false;
	if (impl->presentation_timer.IsRunning())
		impl->presentation_timer.Stop();
	if (impl->fallback_requested)
		return;

	auto const logical_size = GetClientSize();
	if (logical_size.GetWidth() <= 0 || logical_size.GetHeight() <= 0)
		return;
	if (impl->presentation_display == wxNOT_FOUND)
		UpdatePresentationTiming();
	DeferredAudioUiDuration paint_trace("audio_display.paint", 1, impl->provider ? 1 : 0);
	if (!impl->retained_overlay_pending)
		RebuildViewport();
	if (!impl->viewport.IsValid())
		return;
	auto make_cursor_frame = [this]() -> std::shared_ptr<CursorFrame const> {
		auto const placement = BuildCursorPlacement(
			impl->viewport,
			impl->mouse_position_ms,
			impl->mouse_position_x,
			impl->playback_position_ms);
		if (!placement.IsActive())
			return {};

		auto cursor = std::make_shared<CursorFrame>();
		cursor->x = placement.device_x;
		cursor->position_ms = placement.position_ms;
		cursor->playback = placement.source == CursorSource::Playback;
		// Legacy PaintTrackCursor always uses wxWHITE for both mouse and
		// playback cursors; the configured play-cursor colour is not consulted.
		cursor->color = 0xFFFFFFFFu;
		if (!cursor->playback
			&& OPT_GET("Audio/Display/Draw/Cursor Time")->GetBool()) {
			cursor->label = agi::Time(cursor->position_ms).GetAssFormatted();
			// Mirror the legacy "Audio/Track Cursor/Font Face" option (read via
			// FontFace("Audio/Track Cursor") in legacy PaintTrackCursor). Empty
			// keeps the default face.
			cursor->font_face = OPT_GET("Audio/Track Cursor/Font Face")->GetString();
		}
		return cursor;
	};
	auto const trace_audio = perf_trace::IsCategoryEnabled(perf_trace::Category::Audio);
	std::uint64_t trace_frame_id = 0;
	if (trace_audio) {
		trace_frame_id = ++impl->trace_frame_sequence;
		if (!trace_frame_id)
			trace_frame_id = ++impl->trace_frame_sequence;
	}

	if (!impl->context) {
		impl->context = std::make_unique<wxGLContext>(this);
		++impl->context_generation;
		if (!impl->context_generation)
			++impl->context_generation;
	}

	auto const context = impl->ContextToken();
	bool context_active = false;
	{
		perf_trace::AudioUiDurationScope context_trace("audio_display.context_activate");
		context_active = impl->context->IsOK() && SetCurrent(*impl->context);
		context_trace.SetDetails(context_active ? 1 : 0, impl->swap_interval_configured ? 1 : 0);
	}
	if (!context_active) {
		impl->presenter->Fail(
			context,
			SkiaGlDeviceFailure::ContextActivationFailed,
			"wxGLCanvas could not activate the Audio Display context");
		RequestFallback(impl->presenter->TakeFailureLogMessage());
		return;
	}
	if (!impl->swap_interval_attempted) {
		impl->swap_interval_attempted = true;
		impl->swap_interval = RequestedAudioSwapInterval();
		impl->swap_interval_configured = ConfigureCurrentSwapInterval(impl->swap_interval);
	}

	FrameTarget target;
	target.context_generation = context.generation;
	target.width = impl->viewport.target_width;
	target.height = impl->viewport.target_height;
	target.sample_count = 0;
	GLint stencil_bits = 0;
	glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);
	target.stencil_bits = std::max(0, static_cast<int>(stencil_bits));
	target.framebuffer_id = 0;
	target.bottom_left_origin = true;

	bool content_frame_rendered = false;
	bool complete_content_viewport = false;
	bool retained_content_frame = false;
	std::size_t visible_tile_count = 0;
	std::size_t ready_tile_count = 0;
	PresenterFrameTrace pending_frame_trace;
	bool has_pending_frame_trace = false;
	PresenterMetrics rendered_presenter_metrics;
	bool has_rendered_presenter_metrics = false;
	bool visible_content_request_called = false;
	bool content_lookup_performed = false;
	std::uint64_t content_tiles_drawn_this_frame = 0;
	std::uint64_t gpu_tile_uploads_this_frame = 0;
	perf_trace::AudioDisplaySnapshot pending_snapshot;
	bool has_pending_snapshot = false;
	CursorSource rendered_cursor_source = CursorSource::None;
	int rendered_cursor_position_ms = -1;
	double rendered_cursor_device_x = -1.0;
	bool rendered_cursor_label_visible = false;
	auto capture_rendered_cursor = [&](ContentFrame const& frame) {
		if (!frame.cursor) {
			rendered_cursor_source = CursorSource::None;
			rendered_cursor_position_ms = -1;
			rendered_cursor_device_x = -1.0;
			rendered_cursor_label_visible = false;
			return;
		}
		rendered_cursor_source = frame.cursor->playback
			? CursorSource::Playback : CursorSource::Mouse;
		rendered_cursor_position_ms = frame.cursor->position_ms;
		rendered_cursor_device_x = frame.cursor->x;
		rendered_cursor_label_visible = !frame.cursor->label.empty();
	};
	auto populate_marker_frames = [&](ContentFrame& frame) {
		frame.markers.clear();
		auto *timing = impl->audio_controller
			? impl->audio_controller->GetTimingController() : nullptr;
		if (!timing)
			return;
		auto const first_visible_ms = std::max(0, static_cast<int>(std::floor(
			impl->viewport.first_column_exact * impl->viewport.milliseconds_per_column)));
		auto const last_visible_ms = std::max(first_visible_ms, static_cast<int>(std::ceil(
			(impl->viewport.first_column_exact + impl->viewport.content.width)
				* impl->viewport.milliseconds_per_column)));
		AudioMarkerVector markers;
		timing->GetMarkers(TimeRange(first_visible_ms, last_visible_ms), markers);
		auto const x_from_marker = [this](AudioMarker const& marker) {
			return static_cast<int>(LegacyDeviceXFromTime(
				impl->viewport, marker.GetPosition()));
		};
		auto const pixels = AggregateAudioMarkersByPixel(
			markers,
			static_cast<int>(std::floor(impl->viewport.content.x)) - 8,
			static_cast<int>(std::ceil(impl->viewport.content.x + impl->viewport.content.width)) + 8,
			x_from_marker);
		frame.markers.reserve(pixels.size());
		for (auto const& pixel : pixels)
			frame.markers.push_back(BuildMarkerFrame(pixel, GetContentScaleFactor()));
	};
	if (!impl->provider) {
		if (!impl->presenter->RenderDiagnosticFrame(context, target)) {
			RequestFallback(impl->presenter->TakeFailureLogMessage());
			return;
		}
	}
	else {
		bool retained_overlay_rendered = false;
		if (impl->retained_overlay_pending && impl->last_presented_content_frame) {
			auto frame = *impl->last_presented_content_frame;
			auto const updated_layers = impl->dirty_layers;
			if (frame.generation == impl->content_generation
				&& frame.kind == impl->content_analysis.kind
				&& frame.first_column == impl->viewport.first_column
				&& frame.x == impl->viewport.content.x
				&& frame.y == impl->viewport.content.y
				&& frame.width == impl->viewport.content.width
				&& frame.height == impl->viewport.content.height
				&& frame.first_column_offset == impl->viewport.first_column_offset) {
				if (HasLayer(updated_layers, Layer::Marker))
					populate_marker_frames(frame);
				frame.cursor = make_cursor_frame();
				capture_rendered_cursor(frame);
				perf_trace::AudioUiDurationScope content_trace("audio_display.paint_audio");
				auto const before = content_trace.IsActive() ? impl->presenter->Metrics() : PresenterMetrics{};
				retained_overlay_rendered = impl->presenter->RenderRetainedOverlayFrame(
					context,
					target,
					frame,
					updated_layers);
				if (content_trace.IsActive()) {
					rendered_presenter_metrics = impl->presenter->Metrics();
					has_rendered_presenter_metrics = true;
					content_tiles_drawn_this_frame =
						rendered_presenter_metrics.content_tiles_drawn - before.content_tiles_drawn;
					gpu_tile_uploads_this_frame =
						rendered_presenter_metrics.content_uploads - before.content_uploads;
					content_trace.SetDetails(
						static_cast<int>(content_tiles_drawn_this_frame),
						static_cast<int>(gpu_tile_uploads_this_frame));
				}
				if (retained_overlay_rendered) {
					if (trace_audio && has_rendered_presenter_metrics
						&& rendered_presenter_metrics.last_frame_trace.valid) {
						pending_frame_trace = rendered_presenter_metrics.last_frame_trace;
						has_pending_frame_trace = true;
					}
					visible_tile_count = impl->last_presented_visible_tile_count;
					ready_tile_count = impl->last_presented_ready_tile_count;
					complete_content_viewport = impl->last_presented_complete_content_viewport;
					retained_content_frame = impl->last_presented_retained_content_frame;
					content_frame_rendered = true;
					if (HasLayer(updated_layers, Layer::Marker))
						impl->last_presented_content_frame = std::make_shared<ContentFrame>(frame);
					impl->dirty_layers = Layer::None;
					impl->retained_overlay_pending = false;
				}
			}
		}
		if (!retained_overlay_rendered) {
			visible_content_request_called = true;
			RequestVisibleContent();
			if (impl->content_analysis.kind == ContentKind::Spectrum
				&& !impl->spectrum_band_plan)
				return;
		ContentFrame frame;
		frame.generation = impl->content_generation;
		++impl->static_frame_revision;
		if (!impl->static_frame_revision)
			++impl->static_frame_revision;
		frame.static_revision = impl->static_frame_revision;
		frame.kind = impl->content_analysis.kind;
		frame.first_column = impl->viewport.first_column;
		frame.x = static_cast<float>(impl->viewport.content.x);
		frame.y = static_cast<float>(impl->viewport.content.y);
		frame.width = static_cast<float>(impl->viewport.content.width);
		frame.height = static_cast<float>(impl->viewport.content.height);
		frame.first_column_offset = static_cast<float>(impl->viewport.first_column_offset);
		frame.amplitude = impl->amplitude_scale;
		auto const content_scale = std::max(1.0, static_cast<double>(GetContentScaleFactor()));
		frame.content_scale = static_cast<float>(content_scale);
		frame.text_style = BuildTextStyle(*this, content_scale);
		if (!impl->presentation_colors_ready) {
			auto const waveform_scheme_name =
				OPT_GET("Colour/Audio Display/Waveform")->GetString();
			auto const spectrum_scheme_name = frame.kind == ContentKind::Spectrum
				? OPT_GET("Colour/Audio Display/Spectrum")->GetString()
				: std::string {};
			auto const& ui_scheme_name = frame.kind == ContentKind::Spectrum
				? spectrum_scheme_name : waveform_scheme_name;
			for (std::size_t focused = 0; focused < impl->ui_chrome_colors.size(); ++focused) {
				auto const ui_prefix = std::string("Colour/Schemes/") + ui_scheme_name
					+ (focused ? "/UI Focused/" : "/UI/");
				impl->ui_chrome_colors[focused] = {
					ToArgb(OPT_GET(ui_prefix + "Dark")->GetColor()),
					ToArgb(OPT_GET(ui_prefix + "Light")->GetColor()),
					ToArgb(OPT_GET(ui_prefix + "Selection")->GetColor()),
				};
			}
			for (int style = AudioStyle_Normal; style < AudioStyle_MAX; ++style) {
				AudioColorScheme waveform_scheme(
					6,
					waveform_scheme_name,
					static_cast<AudioRenderingStyle>(style));
				impl->waveform_style_colors[style] = {
					ToArgb(waveform_scheme.get(0.f)),
					ToArgb(waveform_scheme.get(0.4f)),
					ToArgb(waveform_scheme.get(0.7f)),
					ToArgb(waveform_scheme.get(1.f)),
				};
				if (frame.kind == ContentKind::Spectrum) {
					auto palette = std::make_shared<SpectrumPalette>();
					palette->revision = (impl->presentation_revision << 3)
						| static_cast<std::uint64_t>(style + 1);
					AudioColorScheme spectrum_scheme(
						12,
						spectrum_scheme_name,
						static_cast<AudioRenderingStyle>(style));
					for (std::size_t i = 0; i < palette->colors.size(); ++i)
						palette->colors[i] = ToArgb(
							spectrum_scheme.get(static_cast<float>(i)
								/ static_cast<float>(kSpectrumPaletteFactor)));
					impl->spectrum_style_palettes[style] = std::move(palette);
				}
			}
			impl->presentation_colors_ready = true;
		}
		auto const& ui_colors = impl->ui_chrome_colors[HasFocus() ? 1 : 0];
		auto timeline = std::make_shared<TimelineFrame>();
		timeline->y = impl->viewport.timeline.y;
		timeline->height = impl->viewport.timeline.height;
		timeline->scroll_left = impl->viewport.scroll_left;
		timeline->scroll_left_exact = impl->viewport.first_column_exact;
		timeline->duration_ms = ProviderDurationMs(impl->provider);
		timeline->milliseconds_per_pixel = impl->viewport.milliseconds_per_column;
		timeline->background_color = ui_colors.dark;
		timeline->foreground_color = ui_colors.light;
		frame.timeline = std::move(timeline);
		auto scrollbar_frame = std::make_shared<ScrollbarFrame>();
		scrollbar_frame->y = impl->viewport.scrollbar.y;
		scrollbar_frame->height = impl->viewport.scrollbar.height;
		scrollbar_frame->content_scale = static_cast<float>(
			std::max(1.0, static_cast<double>(GetContentScaleFactor())));
		scrollbar_frame->total = static_cast<int>(std::lround(
		impl->viewport.logical_audio_width * std::max(1.0, static_cast<double>(GetContentScaleFactor()))));
		scrollbar_frame->page = impl->viewport.target_width;
		scrollbar_frame->position = static_cast<int>(std::lround(
			impl->scroll_left * std::max(1.0, static_cast<double>(GetContentScaleFactor()))));
		if (impl->provider && impl->provider->GetNumSamples() > 0) {
			auto const decoded = impl->provider->GetDecodedSamples();
			if (decoded >= impl->provider->GetNumSamples())
				scrollbar_frame->load_position = -1;
			else
				scrollbar_frame->load_position = static_cast<int>(std::clamp<std::int64_t>(
					decoded * scrollbar_frame->total / impl->provider->GetNumSamples(), 0, scrollbar_frame->total));
		}
		scrollbar_frame->background_color = ui_colors.dark;
		scrollbar_frame->thumb_color = ui_colors.light;
		scrollbar_frame->selection_color = ui_colors.selection;
		frame.scrollbar = scrollbar_frame;

		frame.draw_waveform_average =
			OPT_GET("Audio/Display/Waveform Style")->GetInt() != 0;
		auto const waveform_zero_color_index = frame.draw_waveform_average ? 3 : 1;
		frame.background_color = impl->waveform_style_colors[AudioStyle_Normal][0];
		frame.waveform_peak_color = impl->waveform_style_colors[AudioStyle_Normal][1];
		frame.waveform_average_color = impl->waveform_style_colors[AudioStyle_Normal][2];
		frame.waveform_zero_color =
			impl->waveform_style_colors[AudioStyle_Normal][waveform_zero_color_index];

		if (frame.kind == ContentKind::Spectrum) {
			frame.spectrum_palette = impl->spectrum_style_palettes[AudioStyle_Normal];
			frame.background_color = frame.spectrum_palette->colors.front();
			frame.spectrum_band_plan = impl->spectrum_band_plan;
		}

		StyleRangeCollector style_collector;
		auto *timing = impl->audio_controller ? impl->audio_controller->GetTimingController() : nullptr;
		if (timing)
			timing->GetRenderingStyles(style_collector);
		for (auto const& span : BuildDeviceStyleSpans(style_collector.ranges, impl->viewport)) {
			auto const style_index = std::clamp(
				static_cast<int>(span.style),
				static_cast<int>(AudioStyle_Normal),
				static_cast<int>(AudioStyle_MAX - 1));
			StyleFrame style;
			style.x = span.x;
			style.width = span.width;
			style.background_color = impl->waveform_style_colors[style_index][0];
			style.waveform_peak_color = impl->waveform_style_colors[style_index][1];
			style.waveform_average_color = impl->waveform_style_colors[style_index][2];
			style.waveform_zero_color =
				impl->waveform_style_colors[style_index][waveform_zero_color_index];
			if (frame.kind == ContentKind::Spectrum) {
				style.spectrum_palette = impl->spectrum_style_palettes[style_index];
				style.background_color = style.spectrum_palette->colors.front();
			}
			frame.styles.push_back(std::move(style));
		}

		if (timing) {
			auto const first_visible_ms = std::max(0, static_cast<int>(std::floor(
				impl->viewport.first_column_exact * impl->viewport.milliseconds_per_column)));
			auto const last_visible_ms = std::max(first_visible_ms, static_cast<int>(std::ceil(
				(impl->viewport.first_column_exact + impl->viewport.content.width)
					* impl->viewport.milliseconds_per_column)));
			TimeRange const visible_range(first_visible_ms, last_visible_ms);

			AudioMarkerVector markers;
			timing->GetMarkers(visible_range, markers);
			auto const x_from_marker = [this](AudioMarker const& marker) {
				return static_cast<int>(LegacyDeviceXFromTime(
					impl->viewport, marker.GetPosition()));
			};
			auto const pixels = AggregateAudioMarkersByPixel(
				markers,
				static_cast<int>(std::floor(impl->viewport.content.x)) - 8,
				static_cast<int>(std::ceil(impl->viewport.content.x + impl->viewport.content.width)) + 8,
				x_from_marker);
			frame.markers.reserve(pixels.size());
			for (auto const& pixel : pixels)
				frame.markers.push_back(BuildMarkerFrame(pixel, GetContentScaleFactor()));

			std::vector<AudioLabelProvider::AudioLabel> labels;
			timing->GetLabels(visible_range, labels);
			frame.labels.reserve(labels.size());
			for (auto const& label : labels) {
				frame.labels.push_back({
					LegacyDeviceXFromTime(impl->viewport, label.range.begin()),
					LegacyDeviceWidthFromDuration(impl->viewport, label.range.length()),
					label.text.utf8_string(),
				});
			}

			auto const selection = timing->GetPrimaryPlaybackRange();
			scrollbar_frame->selection_start = std::max(0, static_cast<int>(std::floor(
				selection.begin() / impl->viewport.milliseconds_per_column)));
			scrollbar_frame->selection_length = std::max(0, static_cast<int>(
				selection.length() / impl->viewport.milliseconds_per_column));
		}
		frame.cursor = make_cursor_frame();

		ContentViewportRequest request;
		request.generation = impl->content_generation;
		request.kind = impl->content_analysis.kind;
		request.first_column = impl->viewport.first_column;
		request.column_count = impl->viewport.visible_column_count;
		request.tile_column_count = 256;
		if (request.kind == ContentKind::Spectrum)
			request.spectrum_bin_count = static_cast<std::uint32_t>(
				std::size_t { 1 } << impl->content_analysis.spectrum_derivation_size);
		{
			content_lookup_performed = true;
			perf_trace::AudioUiDurationScope lookup_trace("audio_display.content_lookup");
			auto const visible_tiles = PlanVisibleContentTiles(request);
			visible_tile_count = visible_tiles.size();
			for (auto const& key : visible_tiles)
				if (auto tile = impl->content_worker.FindPayload(
					MakeContentUploadPayloadKey(key, frame.spectrum_band_plan.get()))) {
					frame.tiles.push_back(std::move(tile));
				}
			lookup_trace.SetDetails(
				static_cast<int>(frame.tiles.size()),
				static_cast<int>(visible_tiles.size() - frame.tiles.size()));
		}
		ready_tile_count = frame.tiles.size();

		complete_content_viewport = visible_tile_count > 0
			&& frame.tiles.size() == visible_tile_count;
		if (complete_content_viewport) {
			impl->last_complete_content_frame = std::make_shared<ContentFrame>(frame);
		}
		else if (auto const& retained = impl->last_complete_content_frame;
			ShouldRetainLastCompleteContentFrame(
				complete_content_viewport,
				impl->scrollbar_dragging)
			&& retained
			&& retained->generation == frame.generation
			&& retained->kind == frame.kind
			&& retained->x == frame.x
			&& retained->y == frame.y
			&& retained->width == frame.width
			&& retained->height == frame.height) {
			// While the scrollbar owns the interaction, a large jump can outrun
			// waveform/FFT analysis. Keep the last complete viewport visible and
			// update only the live scrollbar. Once the drag ends, current overlays
			// must be rendered even when some content tiles are still missing so
			// visible timing coordinates continue to match mouse hit testing.
			auto retained_frame = *retained;
			retained_frame.scrollbar = frame.scrollbar;
			frame = std::move(retained_frame);
			retained_content_frame = true;
		}
		capture_rendered_cursor(frame);

		bool rendered = false;
		{
			perf_trace::AudioUiDurationScope content_trace("audio_display.paint_audio");
			auto const before = content_trace.IsActive() ? impl->presenter->Metrics() : PresenterMetrics{};
			rendered = impl->presenter->RenderContentFrame(context, target, frame);
			if (content_trace.IsActive()) {
				rendered_presenter_metrics = impl->presenter->Metrics();
				has_rendered_presenter_metrics = true;
				content_tiles_drawn_this_frame =
					rendered_presenter_metrics.content_tiles_drawn - before.content_tiles_drawn;
				gpu_tile_uploads_this_frame =
					rendered_presenter_metrics.content_uploads - before.content_uploads;
				content_trace.SetDetails(
					static_cast<int>(content_tiles_drawn_this_frame),
					static_cast<int>(gpu_tile_uploads_this_frame));
			}
		}
		if (!rendered) {
			RequestFallback(impl->presenter->TakeFailureLogMessage());
			return;
		}
		if (trace_audio
			&& has_rendered_presenter_metrics
			&& rendered_presenter_metrics.last_frame_trace.valid) {
			pending_frame_trace = rendered_presenter_metrics.last_frame_trace;
			has_pending_frame_trace = true;
		}
		content_frame_rendered = true;
		impl->last_presented_content_frame = std::make_shared<ContentFrame>(frame);
		impl->last_presented_visible_tile_count = visible_tile_count;
		impl->last_presented_ready_tile_count = ready_tile_count;
		impl->last_presented_complete_content_viewport = complete_content_viewport;
		impl->last_presented_retained_content_frame = retained_content_frame;
		impl->dirty_layers = Layer::None;
		impl->retained_overlay_pending = false;
		}
	}
	bool swapped = false;
	{
		perf_trace::AudioUiDurationScope swap_trace("audio_display.swap");
		swapped = SwapBuffers();
		swap_trace.SetDetails(swapped ? 1 : 0, impl->swap_interval_configured ? impl->swap_interval + 1 : 0);
	}
	if (!swapped) {
		impl->presenter->Fail(context, SkiaGlDeviceFailure::SwapBuffersFailed, "wxGLCanvas::SwapBuffers failed");
		RequestFallback(impl->presenter->TakeFailureLogMessage());
	}
	else if (content_frame_rendered) {
		if (TileDiagnosticsEnabled())
			impl->diagnostic_frame_target = target;
		impl->content_frame_presented = true;
		++impl->successful_content_frames;
		if (!impl->deferred_failure_injection_armed
			&& impl->deferred_failure_injection != FailureInjection::None
			&& impl->successful_content_frames
				>= impl->failure_injection_after_content_frames) {
			impl->presenter->SetFailureInjection(impl->deferred_failure_injection);
			impl->deferred_failure_injection_armed = true;
			LOG_I("audio/display/skia")
				<< "Armed deferred Audio Display failure injection after "
				<< impl->successful_content_frames << " successful content frames";
		}
	}
	if (swapped) {
		impl->last_present = std::chrono::steady_clock::now();
		impl->has_present_timestamp = true;
	}
	if (content_frame_rendered && trace_audio) {
		pending_snapshot.renderer_name = "skia";
		pending_snapshot.content_kind = impl->content_analysis.kind == ContentKind::Spectrum
			? "spectrum" : "waveform";
		pending_snapshot.frame_id = trace_frame_id;
		pending_snapshot.provider_generation = impl->content_generation.provider;
		pending_snapshot.analysis_generation = impl->content_generation.analysis;
		pending_snapshot.marker_revision = impl->revisions.marker;
		pending_snapshot.chrome_revision = impl->revisions.chrome;
		pending_snapshot.presentation_revision = impl->presentation_revision;
		pending_snapshot.content_scale = std::max(
			1.0,
			static_cast<double>(GetContentScaleFactor()));
		pending_snapshot.viewport_first_column = impl->viewport.first_column;
		pending_snapshot.viewport_column_count = impl->viewport.visible_column_count;
		pending_snapshot.target_width = static_cast<std::uint64_t>(impl->viewport.target_width);
		pending_snapshot.target_height = static_cast<std::uint64_t>(impl->viewport.target_height);
		pending_snapshot.visible_tile_count = visible_tile_count;
		pending_snapshot.ready_tile_count = ready_tile_count;
		pending_snapshot.complete_content_viewport = complete_content_viewport;
		pending_snapshot.retained_content_frame = retained_content_frame;
		pending_snapshot.cursor_only = has_pending_frame_trace && pending_frame_trace.cursor_only;
		pending_snapshot.retained_layers_reused = has_pending_frame_trace
			&& pending_frame_trace.retained_layers_reused;
		pending_snapshot.focused = HasFocus();
		pending_snapshot.middle_seek_active = impl->middle_seek_active;
		pending_snapshot.cursor_source = CursorSourceName(rendered_cursor_source);
		pending_snapshot.cursor_position_ms = rendered_cursor_position_ms;
		pending_snapshot.cursor_device_x = rendered_cursor_device_x;
		pending_snapshot.cursor_label_visible = rendered_cursor_label_visible;
		pending_snapshot.visible_content_request_called = visible_content_request_called;
		pending_snapshot.content_lookup_performed = content_lookup_performed;
		if (has_rendered_presenter_metrics) {
			pending_snapshot.content_tiles_drawn_this_frame = content_tiles_drawn_this_frame;
			pending_snapshot.gpu_tile_uploads_this_frame = gpu_tile_uploads_this_frame;
		}
		pending_snapshot.swapped = swapped;
		has_pending_snapshot = true;
	}
	if (swapped) {
		if (complete_content_viewport
			&& impl->scrollbar_dragging
			&& impl->content_scroll_left != impl->scroll_left) {
			CommitScrollbarContentViewport();
		}
	}
	paint_trace.Publish();
	if (has_pending_snapshot) {
		auto const cpu = impl->content_worker.StoreMetrics();
		auto const payload = impl->content_worker.PayloadMetrics();
		auto const fft = impl->content_worker.AnalysisMetrics();
		auto const worker = impl->content_worker.Metrics();
		auto const gpu = has_rendered_presenter_metrics
			? rendered_presenter_metrics
			: impl->presenter->Metrics();
		pending_snapshot.cpu_tile_budget_bytes = cpu.budget_bytes;
		pending_snapshot.cpu_tile_bytes = cpu.bytes;
		pending_snapshot.cpu_tile_entries = cpu.entries;
		pending_snapshot.cpu_tile_hits = cpu.hits;
		pending_snapshot.cpu_tile_misses = cpu.misses;
		pending_snapshot.cpu_tile_evictions = cpu.evictions;
		pending_snapshot.cpu_payload_budget_bytes = payload.budget_bytes;
		pending_snapshot.cpu_payload_bytes = payload.bytes;
		pending_snapshot.cpu_payload_entries = payload.entries;
		pending_snapshot.cpu_payload_hits = payload.hits;
		pending_snapshot.cpu_payload_misses = payload.misses;
		pending_snapshot.cpu_payload_evictions = payload.evictions;
		pending_snapshot.fft_budget_bytes = fft.configured_spectrum_budget_bytes;
		pending_snapshot.fft_active_cache_budget_bytes = fft.spectrum_cache_budget_bytes;
		pending_snapshot.fft_bytes = fft.spectrum_cache_bytes;
		pending_snapshot.fft_entries = fft.spectrum_cache_entries;
		pending_snapshot.fft_hits = fft.spectrum_cache_hits;
		pending_snapshot.fft_misses = fft.spectrum_cache_misses;
		pending_snapshot.fft_visible_builds = fft.spectrum_visible_builds;
		pending_snapshot.fft_evictions = fft.spectrum_cache_evictions;
		pending_snapshot.gpu_tile_budget_bytes = gpu.content_cache_budget_bytes;
		pending_snapshot.gpu_tile_bytes = gpu.content_cache_bytes;
		pending_snapshot.gpu_tile_entries = gpu.content_cache_entries;
		pending_snapshot.gpu_tile_hits = gpu.content_cache_hits;
		pending_snapshot.gpu_tile_misses = gpu.content_cache_misses;
		pending_snapshot.gpu_tile_uploads = gpu.content_uploads;
		pending_snapshot.gpu_tile_upload_bytes = gpu.content_upload_bytes;
		pending_snapshot.gpu_tile_evictions = gpu.content_evictions;
		pending_snapshot.gpu_palette_uploads = gpu.palette_uploads;
		pending_snapshot.worker_builds_started = worker.builds_started;
		pending_snapshot.worker_builds_ready = worker.builds_ready;
		pending_snapshot.worker_builds_cancelled = worker.builds_cancelled;
		pending_snapshot.worker_payload_builds_started = worker.payload_builds_started;
		pending_snapshot.worker_payload_builds_ready = worker.payload_builds_ready;
		pending_snapshot.worker_payload_builds_cancelled = worker.payload_builds_cancelled;
		pending_snapshot.worker_superseded_requests = worker.superseded_requests;
	}
	if (has_pending_frame_trace) {
		auto const& trace = pending_frame_trace;
		if (trace.base_layer_rebuild_ms >= 0.0) {
			perf_trace::ObserveAudioUiDuration(
				"audio_display.base_layer_rebuild",
				trace.base_layer_rebuild_ms,
				trace.tile_count,
				trace.style_count);
		}
		if (trace.marker_layer_rebuild_ms >= 0.0) {
			perf_trace::ObserveAudioUiDuration(
				"audio_display.marker_layer_rebuild",
				trace.marker_layer_rebuild_ms,
				trace.marker_count,
				trace.markers_drawn);
		}
		if (trace.label_layer_rebuild_ms >= 0.0) {
			perf_trace::ObserveAudioUiDuration(
				"audio_display.label_layer_rebuild",
				trace.label_layer_rebuild_ms,
				trace.label_count,
				trace.labels_drawn);
		}
		if (trace.scrollbar_layer_rebuild_ms >= 0.0) {
			perf_trace::ObserveAudioUiDuration(
				"audio_display.scrollbar_layer_rebuild",
				trace.scrollbar_layer_rebuild_ms,
				trace.scrollbar_selection_visible ? 1 : 0,
				trace.scrollbar_load_visible ? 1 : 0);
		}
		perf_trace::ObserveAudioUiDuration(
			"audio_display.frame_compose",
			trace.frame_compose_ms,
			trace.marker_count,
			trace.label_count);
	}
	if (has_pending_snapshot)
		perf_trace::ObserveAudioDisplaySnapshot(pending_snapshot);
}
catch (std::exception const& err) {
	if (impl->presenter && impl->context) {
		impl->presenter->Fail(
			impl->ContextToken(),
			SkiaGlDeviceFailure::SurfaceAcquisitionFailed,
			err.what());
		RequestFallback(impl->presenter->TakeFailureLogMessage());
	}
	else {
		RequestFallback(std::string("Skia Audio Display initialization failed: ") + err.what());
	}
}
catch (...) {
	RequestFallback("an unknown exception escaped Skia Audio Display paint");
}

void SkiaAudioDisplay::OnEraseBackground(wxEraseEvent&) {
}

void SkiaAudioDisplay::OnSize(wxSizeEvent& event) {
	UpdatePresentationTiming();
	Invalidate(Change::Resize);
	ReconfigureAnalysis();
	RequestRepaint();
	event.Skip();
}

void SkiaAudioDisplay::OnDPIChanged(wxDPIChangedEvent& event) {
	if (!impl) {
		event.Skip();
		return;
	}

	UpdatePresentationTiming();
	impl->InvalidatePresentation();
	Invalidate(Change::Dpi);
	ReconfigureAnalysis();
	RequestRepaint(true);
	event.Skip();
}

void SkiaAudioDisplay::OnContentReady(wxThreadEvent&) {
	if (impl)
		impl->content_ready_event_pending.store(false, std::memory_order_release);
	Invalidate(Change::ContentReady);
	if (impl && impl->scrollbar_dragging) {
		if (HasCompleteVisibleContent())
			RequestRepaint(true);
		return;
	}
	RequestRepaint();
}

void SkiaAudioDisplay::UpdatePresentationTiming() {
	if (!impl)
		return;
	auto const display_index = wxDisplay::GetFromWindow(this);
	if (display_index == wxNOT_FOUND)
		return;
	wxDisplay display(static_cast<unsigned int>(display_index));
	if (!display.IsOk())
		return;
	auto const refresh_rate = display.GetCurrentMode().GetRefresh();
	impl->presentation_display = display_index;
	impl->presentation_refresh_rate = refresh_rate >= 24 && refresh_rate <= 1000
		? refresh_rate : 60;
}

void SkiaAudioDisplay::Invalidate(Change change) {
	if (!impl)
		return;
	auto const plan = PlanTransition(impl->revisions, change);
	impl->revisions = plan.next;
	impl->dirty_layers = impl->dirty_layers | plan.dirty_layers;
	if (CanRenderRetainedOverlay(impl->dirty_layers)
		&& impl->last_presented_content_frame
		&& impl->last_presented_content_frame->static_revision) {
		impl->retained_overlay_pending = true;
		return;
	}
	impl->retained_overlay_pending = false;
	impl->last_presented_content_frame.reset();
}

void SkiaAudioDisplay::RequestRepaint(bool interactive) {
	if (!impl || impl->fallback_requested)
		return;
	impl->repaint_pending = true;
	if (impl->paint_queued)
		return;
	if (interactive) {
		if (impl->presentation_timer.IsRunning())
			impl->presentation_timer.Stop();
		impl->paint_queued = true;
		Refresh(false);
		return;
	}
	if (!impl->has_present_timestamp) {
		impl->paint_queued = true;
		Refresh(false);
		return;
	}

	auto const now = std::chrono::steady_clock::now();
	auto const deadline = impl->last_present
		+ PresentationFrameInterval(impl->presentation_refresh_rate);
	if (now >= deadline) {
		if (impl->presentation_timer.IsRunning())
			impl->presentation_timer.Stop();
		impl->paint_queued = true;
		Refresh(false);
		return;
	}
	if (impl->presentation_timer.IsRunning())
		return;
	auto const remaining = deadline - now;
	auto const delay = std::max<std::int64_t>(
		1,
		std::chrono::duration_cast<std::chrono::milliseconds>(
			remaining + std::chrono::milliseconds(1) - std::chrono::nanoseconds(1)).count());
	impl->presentation_timer.StartOnce(static_cast<int>(std::min<std::int64_t>(delay, 1000)));
}

void SkiaAudioDisplay::OnPresentationTimer(wxTimerEvent&) {
	if (!impl || !impl->repaint_pending || impl->fallback_requested)
		return;
	impl->paint_queued = true;
	Refresh(false);
}

void SkiaAudioDisplay::OnContentFailure(wxThreadEvent& event) {
	auto const failure = event.GetPayload<ContentWorkerFailure>();
	if (impl->content_worker.IsFailureCurrent(failure))
		RequestFallback(failure.message);
}

void SkiaAudioDisplay::OnLoadTimer(wxTimerEvent&) {
	if (!impl || !impl->provider)
		return;
	auto const decoded = impl->provider->GetDecodedSamples();
	if (decoded != impl->last_decoded_samples) {
		impl->last_decoded_samples = decoded;
		impl->has_last_content_request = false;
		RequestVisibleContent();
		Invalidate(Change::Chrome);
		RequestRepaint();
	}
	if (decoded >= impl->provider->GetNumSamples())
		impl->load_timer.Stop();
}

void SkiaAudioDisplay::EmitMiddleSeekOutput(int time_ms, bool commit) {
	if (!impl || !impl->project_context)
		return;
	auto core = impl->project_context->GetCore();
	if (!core.videoController || !core.project->VideoProvider())
		return;
	auto const frame = core.videoController->FrameAtTime(time_ms, agi::vfr::EXACT);
	perf_trace::TraceAudioMiddleSeek(commit ? "commit" : "preview", time_ms, frame);
	if (commit)
		core.videoController->CommitInteractiveSeekPreviewToTime(time_ms, agi::vfr::EXACT);
	else {
		core.videoController->PreviewToFrameLatest(frame);
	}
}

void SkiaAudioDisplay::ScheduleMiddleSeekTimer() {
	if (!impl || !impl->middle_seek_active) {
		if (impl)
			impl->middle_seek_timer.Stop();
		return;
	}
	if (!wxGetMouseState().MiddleIsDown()) {
		FinishMiddleSeek(impl->mouse_position_ms >= 0 ? impl->mouse_position_ms : 0);
		return;
	}
	if (impl->viewport.IsValid()) {
		auto const point = ScreenToClient(wxGetMousePosition());
		impl->mouse_position_ms = std::max(0, static_cast<int>(
			(impl->scroll_left + point.x) * AudioMillisecondsPerLogicalPixel(impl->zoom_level)));
		impl->mouse_position_x = RelativeLogicalXFromTime(
			impl->mouse_position_ms,
			impl->scroll_left,
			AudioMillisecondsPerLogicalPixel(impl->zoom_level));
	}
	auto next = impl->middle_seek_policy.NextPreviewTime();
	if (!next) {
		impl->middle_seek_timer.Start(33, true);
		return;
	}
	auto const now = NavigationPreviewPolicy::Clock::now();
	auto const delay = *next > now
		? std::chrono::duration_cast<std::chrono::milliseconds>(*next - now).count() : 1;
	impl->middle_seek_timer.Start(std::max(1, static_cast<int>(delay)), true);
}

void SkiaAudioDisplay::FinishMiddleSeek(int time_ms) {
	if (!impl || !impl->middle_seek_active)
		return;
	impl->middle_seek_timer.Stop();
	impl->middle_seek_active = false;
	impl->middle_seek_policy.OnRelease(time_ms, NavigationPreviewPolicy::Clock::now());
	EmitMiddleSeekOutput(time_ms, true);
	auto const point = ScreenToClient(wxGetMousePosition());
	auto const scale = std::max(1.0, static_cast<double>(GetContentScaleFactor()));
	impl->mouse_position_ms = MousePositionMsForClientPoint(
		impl->viewport,
		point.x,
		point.y,
		scale,
		AudioMillisecondsPerLogicalPixel(impl->zoom_level));
	impl->mouse_position_x = impl->mouse_position_ms >= 0 ? point.x : -1;
	Invalidate(Change::Cursor);
	RequestRepaint(true);
}

void SkiaAudioDisplay::CancelMiddleSeek() {
	if (!impl || !impl->middle_seek_active)
		return;
	impl->middle_seek_timer.Stop();
	impl->middle_seek_active = false;
	impl->middle_seek_policy.Cancel();
	if (impl->project_context) {
		auto core = impl->project_context->GetCore();
		if (core.videoController)
			core.videoController->CancelInteractiveSeekPreview();
	}
	impl->mouse_position_ms = -1;
	impl->mouse_position_x = -1;
}

void SkiaAudioDisplay::OnMiddleSeekTimer(wxTimerEvent&) {
	if (!impl || !impl->middle_seek_active)
		return;
	if (!wxGetMouseState().MiddleIsDown()) {
		FinishMiddleSeek(impl->mouse_position_ms >= 0 ? impl->mouse_position_ms : 0);
		return;
	}
	if (impl->viewport.IsValid()) {
		auto const point = ScreenToClient(wxGetMousePosition());
		impl->mouse_position_ms = std::max(0, static_cast<int>(
			(impl->scroll_left + point.x) * AudioMillisecondsPerLogicalPixel(impl->zoom_level)));
		impl->mouse_position_x = RelativeLogicalXFromTime(
			impl->mouse_position_ms,
			impl->scroll_left,
			AudioMillisecondsPerLogicalPixel(impl->zoom_level));
	}
	if (auto output = impl->middle_seek_policy.OnTimer(NavigationPreviewPolicy::Clock::now()))
		EmitMiddleSeekOutput(output->target, false);
	ScheduleMiddleSeekTimer();
}

void SkiaAudioDisplay::OnPlaybackPosition(int position_ms) {
	if (!impl)
		return;
	auto const playback_position_ms = std::max(0, position_ms);
	bool viewport_changed = false;
	if (!impl->middle_seek_active && OPT_GET("Audio/Lock Scroll on Cursor")->GetBool() && impl->viewport.IsValid()) {
		perf_trace::AudioUiDurationScope scroll_trace("audio_display.scroll");
		auto const old_scroll_left = impl->scroll_left;
		auto const logical_ms_per_pixel = AudioMillisecondsPerLogicalPixel(impl->zoom_level);
		auto const pixel_position = static_cast<int>(std::floor(playback_position_ms / logical_ms_per_pixel));
		auto const client_width = std::max(1, GetClientSize().GetWidth());
		auto const edge = std::max(1, client_width / 20);
		if (impl->scroll_left > 0 && pixel_position < impl->scroll_left + edge)
			impl->scroll_left = std::max(0, pixel_position - edge);
		else if (pixel_position >= impl->scroll_left + client_width - edge)
			impl->scroll_left = pixel_position - client_width + edge;
		viewport_changed = impl->scroll_left != old_scroll_left;
		if (viewport_changed) {
			impl->content_scroll_left = impl->scroll_left;
			RebuildViewport();
			RequestVisibleContent();
		}
		scroll_trace.SetDetails(
			std::abs(impl->scroll_left - old_scroll_left),
			viewport_changed ? 1 : 0);
	}
	perf_trace::AudioUiDurationScope trace("audio_display.cursor_update", 1, 0);
	impl->playback_position_ms = playback_position_ms;
	Invalidate(viewport_changed ? Change::Scroll : Change::Cursor);
	RequestRepaint();
}

void SkiaAudioDisplay::OnPlaybackStop() {
	if (!impl)
		return;
	perf_trace::AudioUiDurationScope trace("audio_display.cursor_update", 1, 0);
	impl->playback_position_ms = -1;
	impl->ResetPlaybackOverlayRefresh();
	impl->mouse_position_ms = -1;
	impl->mouse_position_x = -1;
	Invalidate(Change::Cursor);
	RequestRepaint();
}

void SkiaAudioDisplay::OnTimingControllerChanged() {
	if (!impl || !impl->audio_controller)
		return;
	impl->timing_connections.clear();
	if (auto *timing = impl->audio_controller->GetTimingController()) {
		impl->timing_connections = agi::signal::make_vector({
			timing->AddMarkerMovedListener(&SkiaAudioDisplay::OnMarkerMoved, this),
			timing->AddLabelChangedListener(&SkiaAudioDisplay::OnTimingDataChanged, this),
			timing->AddUpdatedPrimaryRangeListener(&SkiaAudioDisplay::OnSelectionChanged, this),
			timing->AddUpdatedStyleRangesListener(&SkiaAudioDisplay::OnStyleRangesChanged, this),
		});
	}
	OnTimingDataChanged();
}

void SkiaAudioDisplay::OnTimingDataChanged() {
	if (impl) {
		Invalidate(Change::Style);
		Invalidate(Change::Marker);
		Invalidate(Change::Chrome);
		RequestRepaint();
	}
}

void SkiaAudioDisplay::OnMarkerMoved() {
	if (!impl)
		return;
	bool const force_refresh = impl->timeline_dragging
		|| impl->scrollbar_dragging
		|| !impl->dragged_markers.empty()
		|| impl->middle_seek_active;
	bool const refresh_due = impl->ShouldRefreshPlaybackOverlay(force_refresh);
	// Keep the latest marker state dirty while only the GL repaint is throttled.
	Invalidate(Change::Marker);
	if (refresh_due)
		RequestRepaint(true);
}

void SkiaAudioDisplay::OnSelectionChanged() {
	if (!impl)
		return;
	// Mirror the legacy AudioDisplay::OnSelectionChanged auto-scroll: when no
	// marker is being dragged and Audio/Auto/Scroll is on, bring the active
	// line's primary range into view (including scrolling backward). Skia
	// handles marker-drag out-of-view via OnMouseEvent's ScrollBy, so only the
	// non-drag branch is ported here.
	if (impl->dragged_markers.empty() && OPT_GET("Audio/Auto/Scroll")->GetBool()) {
		auto *timing = impl->audio_controller ? impl->audio_controller->GetTimingController() : nullptr;
		if (timing) {
			auto const sel = timing->GetPrimaryPlaybackRange();
			if (sel.end() != 0) {
				ScrollTimeRangeInView(sel);
				// The scroll moved the viewport under the (stationary) mouse, so
				// the mouse-position track cursor must be recomputed against the
				// current screen mouse or its time label drifts off-cursor.
				// Mirrors legacy UpdateTrackCursorFromCurrentMouse().
				if (!impl->audio_controller->IsPlaying() && impl->viewport.IsValid()) {
					auto const point = ScreenToClient(wxGetMousePosition());
					auto const scale = std::max(1.0, static_cast<double>(GetContentScaleFactor()));
					impl->mouse_position_ms = MousePositionMsForClientPoint(
						impl->viewport,
						point.x,
						point.y,
						scale,
						AudioMillisecondsPerLogicalPixel(impl->zoom_level));
					impl->mouse_position_x = impl->mouse_position_ms >= 0 ? point.x : -1;
					Invalidate(Change::Cursor);
				}
			}
		}
	}
	Invalidate(Change::Chrome);
	RequestRepaint(true);
}

void SkiaAudioDisplay::OnStyleRangesChanged() {
	if (impl) {
		Invalidate(Change::Style);
		RequestRepaint(true);
	}
}

void SkiaAudioDisplay::OnMouseEvent(wxMouseEvent& event) {
	if (!impl || impl->fallback_requested)
		return;
	if (hotkey::check("Audio", impl->project_context, event))
		return;

	// On platforms that report mouse move events while the cursor is outside
	// the client rectangle (notably macOS when the window has focus), drop the
	// spurious motion so the track cursor does not follow it. Matches legacy
	// AudioDisplay::OnMouseEvent.
	if (event.Moving() && !GetClientRect().Contains(event.GetPosition())) {
		event.Skip();
		return;
	}

	RebuildViewport();
	if (!impl->viewport.IsValid())
		return;
	auto const mouse = event.GetPosition();
	auto const client_width = std::max(1, GetClientSize().GetWidth());
	auto const scale = std::max(1.0, static_cast<double>(GetContentScaleFactor()));
	auto const timeline_bottom = static_cast<int>(std::ceil(
		(impl->viewport.timeline.y + impl->viewport.timeline.height) / scale));
	auto const scrollbar_top = static_cast<int>(std::floor(impl->viewport.scrollbar.y / scale));
	auto const time_from_x = [this](int x) {
		auto const value = (impl->scroll_left + x) * AudioMillisecondsPerLogicalPixel(impl->zoom_level);
		return static_cast<int>(std::clamp<double>(value, 0.0, std::numeric_limits<int>::max()));
	};
	auto *timing = impl->audio_controller ? impl->audio_controller->GetTimingController() : nullptr;
	auto const set_middle_cursor = [&](int time_ms) {
		impl->mouse_position_ms = time_ms;
		impl->mouse_position_x = RelativeLogicalXFromTime(
			time_ms,
			impl->scroll_left,
			AudioMillisecondsPerLogicalPixel(impl->zoom_level));
	};

	if (impl->middle_seek_active) {
		if (event.MiddleUp() || !event.MiddleIsDown()) {
			FinishMiddleSeek(time_from_x(mouse.x));
		}
		else {
			auto const time_ms = time_from_x(mouse.x);
			set_middle_cursor(time_ms);
			if (auto output = impl->middle_seek_policy.OnMotion(
				time_ms, NavigationPreviewPolicy::Clock::now(), event.MiddleDown())) {
				EmitMiddleSeekOutput(output->target, false);
			}
			ScheduleMiddleSeekTimer();
			Invalidate(Change::Cursor);
			RequestRepaint(true);
		}
		return;
	}

	// Scroll the display after a mouse-up near one of the edges, mirroring the
	// legacy AudioDisplay::OnMouseEvent edge-scroll, using the raw mouse x.
	if ((event.LeftUp() || event.RightUp()) && OPT_GET("Audio/Auto/Scroll")->GetBool()) {
		if (mouse.x < client_width / 20)
			ScrollBy(-client_width / 3);
		else if (client_width - mouse.x < client_width / 20)
			ScrollBy(client_width / 3);
	}

	if (impl->timeline_dragging) {
		if (event.LeftIsDown()) {
			ScrollBy(impl->drag_last_x - mouse.x);
			impl->drag_last_x = mouse.x;
		}
		else {
			impl->timeline_dragging = false;
			if (HasCapture()) ReleaseMouse();
		}
		return;
	}
	if (impl->scrollbar_dragging) {
		if (event.LeftIsDown()) {
			auto const old_scroll_left = impl->scroll_left;
			auto const total = std::max(1, impl->viewport.logical_audio_width);
			auto const page = std::clamp(GetClientSize().GetWidth(), 1, total);
			auto const geometry = BuildScrollbarGeometry(
				static_cast<float>(client_width),
				10.f,
				25.f,
				total,
				page,
				impl->scroll_left,
				-1,
				-1,
				0);
			auto const shaft = std::max(1.f,
				static_cast<float>(client_width) - geometry.nominal_thumb_width);
			auto const maximum = std::max(0, total - page);
			impl->scroll_left = static_cast<int>(maximum
				* std::clamp(
					static_cast<float>(mouse.x) - geometry.nominal_thumb_width * 0.5f,
					0.f,
					shaft)
				/ shaft);
			if (HasCompleteVisibleContent())
				CommitScrollbarContentViewport();
			else if (impl->scroll_left != old_scroll_left)
				Invalidate(Change::Chrome);
			RequestRepaint(true);
		}
		else {
			impl->scrollbar_dragging = false;
			if (impl->content_scroll_left != impl->scroll_left)
				CommitScrollbarContentViewport();
			RequestRepaint(true);
			if (HasCapture()) ReleaseMouse();
		}
		return;
	}
	if (!impl->dragged_markers.empty()) {
		bool const button_down = impl->dragged_button == wxMOUSE_BTN_LEFT
			? event.LeftIsDown() : event.RightIsDown();
		if (button_down && timing) {
			if (!impl->marker_drag_dead_zone.ShouldDrag(mouse.x))
				return;
			if (mouse.x < 0)
				ScrollBy(mouse.x - client_width / 20);
			else if (mouse.x >= client_width)
				ScrollBy(mouse.x - client_width + client_width / 20);
			auto const snap = OPT_GET("Audio/Snap/Enable")->GetBool() != event.ShiftDown()
				? static_cast<int>(OPT_GET("Audio/Snap/Distance")->GetInt()
					* AudioMillisecondsPerLogicalPixel(impl->zoom_level))
				: 0;
			timing->OnMarkerDrag(impl->dragged_markers, time_from_x(mouse.x), snap);
			Invalidate(Change::Marker);
			RequestRepaint(true);
		}
		else {
			impl->dragged_markers.clear();
			impl->dragged_button = wxMOUSE_BTN_NONE;
			impl->marker_drag_dead_zone.Reset(0, 0);
			SetCursor(wxNullCursor);
			if (HasCapture()) ReleaseMouse();
		}
		return;
	}

	if (event.IsButton())
		SetFocus();
	auto const over_timeline = mouse.y < timeline_bottom;
	auto const over_scrollbar = mouse.y >= scrollbar_top;
	if (over_timeline || over_scrollbar) {
		if (!impl->audio_controller->IsPlaying()
			&& (impl->mouse_position_ms >= 0 || impl->mouse_position_x >= 0)) {
			impl->mouse_position_ms = -1;
			impl->mouse_position_x = -1;
			Invalidate(Change::Cursor);
			RequestRepaint(true);
		}
		if (over_timeline)
			SetCursor(wxCursor(wxCURSOR_SIZEWE));
		else if (event.Moving())
			SetCursor(wxNullCursor);

		if (event.LeftDown() && over_timeline) {
			impl->timeline_dragging = true;
			impl->drag_last_x = mouse.x;
			if (!HasCapture()) CaptureMouse();
		}
		else if (event.LeftDown() && over_scrollbar) {
			impl->scrollbar_dragging = true;
			if (!HasCapture()) CaptureMouse();
			wxMouseEvent motion(event);
			motion.SetEventType(wxEVT_MOTION);
			OnMouseEvent(motion);
		}
		// Legacy ForwardMouseEvent consumes every event in the timeline and
		// scrollbar bands, including middle-button events.
		return;
	}

	if (event.MiddleIsDown()) {
		auto core = impl->project_context->GetCore();
		if (core.videoController && core.project->VideoProvider()) {
			auto const time_ms = time_from_x(mouse.x);
			set_middle_cursor(time_ms);
			core.videoController->BeginInteractiveSeekPreview();
			bool const was_playing = core.videoController->IsPlaying();
			impl->middle_seek_policy.BeginGesture(
				was_playing,
				static_cast<int>(std::ceil(3.0 * AudioMillisecondsPerLogicalPixel(impl->zoom_level))));
			if (was_playing)
				core.videoController->PrefetchFrame(core.videoController->FrameAtTime(time_ms));
			impl->middle_seek_active = true;
			if (auto output = impl->middle_seek_policy.OnMotion(
				time_ms, NavigationPreviewPolicy::Clock::now(), event.MiddleDown())) {
				EmitMiddleSeekOutput(output->target, false);
			}
			ScheduleMiddleSeekTimer();
			Invalidate(Change::Cursor);
			RequestRepaint(true);
		}
		return;
	}

	if (event.Moving()) {
		impl->mouse_position_ms = MousePositionMsForClientPoint(
			impl->viewport,
			mouse.x,
			mouse.y,
			scale,
			AudioMillisecondsPerLogicalPixel(impl->zoom_level));
		impl->mouse_position_x = impl->mouse_position_ms >= 0 ? mouse.x : -1;
		auto const over_audio = impl->mouse_position_ms >= 0;
		if (!impl->audio_controller->IsPlaying()) {
			auto const cursor_time_detail = perf_trace::IsEnabled()
				&& OPT_GET("Audio/Display/Draw/Cursor Time")->GetBool() ? 1 : 0;
			perf_trace::AudioUiDurationScope trace(
				"audio_display.cursor_update", 1, cursor_time_detail);
			Invalidate(Change::Cursor);
			RequestRepaint(true);
		}
		if (mouse.y < timeline_bottom) {
			// Hovering the timeline strip: it is draggable, so show the resize
			// cursor unconditionally (mirrors legacy ForwardMouseEvent).
			SetCursor(wxCursor(wxCURSOR_SIZEWE));
		}
		else if (timing && over_audio) {
			auto const sensitivity = static_cast<int>(
				OPT_GET("Audio/Start Drag Sensitivity")->GetInt()
				* AudioMillisecondsPerLogicalPixel(impl->zoom_level));
			SetCursor(timing->IsNearbyMarker(impl->mouse_position_ms, sensitivity, event.AltDown())
				? wxCursor(wxCURSOR_SIZEWE) : wxNullCursor);
		}
		return;
	}

	if (timing && (event.LeftDown() || event.RightDown())
		&& mouse.y >= timeline_bottom && mouse.y < scrollbar_top) {
		auto const sensitivity = static_cast<int>(
			OPT_GET("Audio/Start Drag Sensitivity")->GetInt()
			* AudioMillisecondsPerLogicalPixel(impl->zoom_level));
		auto const snap = OPT_GET("Audio/Snap/Enable")->GetBool() != event.ShiftDown()
			? static_cast<int>(OPT_GET("Audio/Snap/Distance")->GetInt()
				* AudioMillisecondsPerLogicalPixel(impl->zoom_level))
			: 0;
		// OnLeftClick/OnRightClick synchronously announce an updated primary
		// range, which OnSelectionChanged may react to by auto-scrolling under
		// Audio/Auto/Scroll. Clicking should never move the viewport, so capture
		// the scroll position (and the mouse-position cursor derived from it)
		// before the click and restore it after, mirroring the legacy
		// AudioDisplay::OnMouseEvent click guard.
		auto const saved_scroll_left = impl->scroll_left;
		auto const saved_content_scroll_left = impl->content_scroll_left;
		auto const saved_mouse_position_ms = impl->mouse_position_ms;
		auto const saved_mouse_position_x = impl->mouse_position_x;
		impl->dragged_markers = event.LeftDown()
			? timing->OnLeftClick(time_from_x(mouse.x), event.CmdDown(), event.AltDown(), sensitivity, snap)
			: timing->OnRightClick(time_from_x(mouse.x), event.CmdDown(), sensitivity, snap);
		if (impl->scroll_left != saved_scroll_left) {
			impl->scroll_left = saved_scroll_left;
			impl->content_scroll_left = saved_content_scroll_left;
			// OnSelectionChanged recomputed the cursor against the temporary
			// scrolled viewport; the restored viewport matches the pre-click
			// state, so the saved cursor time is correct again.
			impl->mouse_position_ms = saved_mouse_position_ms;
			impl->mouse_position_x = saved_mouse_position_x;
			RebuildViewport();
			Invalidate(Change::Scroll);
			Invalidate(Change::Cursor);
		}
		if (!impl->dragged_markers.empty()) {
			impl->dragged_button = event.LeftDown() ? wxMOUSE_BTN_LEFT : wxMOUSE_BTN_RIGHT;
			impl->marker_drag_dead_zone.Reset(
				mouse.x, OPT_GET("Audio/Drag Dead Zone")->GetInt());
			impl->mouse_position_ms = -1;
			impl->mouse_position_x = -1;
			if (!HasCapture()) CaptureMouse();
		}
		Invalidate(Change::Marker);
		Invalidate(Change::Chrome);
		RequestRepaint(true);
	}
}

void SkiaAudioDisplay::OnMouseEnter(wxMouseEvent& event) {
	if (impl
		&& !impl->middle_seek_active
		&& impl->audio_controller
		&& !impl->audio_controller->IsPlaying()) {
		RebuildViewport();
		auto const point = event.GetPosition();
		auto const scale = std::max(1.0, static_cast<double>(GetContentScaleFactor()));
		impl->mouse_position_ms = MousePositionMsForClientPoint(
			impl->viewport,
			point.x,
			point.y,
			scale,
			AudioMillisecondsPerLogicalPixel(impl->zoom_level));
		impl->mouse_position_x = impl->mouse_position_ms >= 0 ? point.x : -1;
		perf_trace::AudioUiDurationScope trace("audio_display.cursor_update", 1, 0);
		Invalidate(Change::Cursor);
		RequestRepaint(true);
	}
	if (OPT_GET("Audio/Auto/Focus")->GetBool())
		SetFocus();
	event.Skip();
}

void SkiaAudioDisplay::OnMouseLeave(wxMouseEvent& event) {
	if (impl && !impl->middle_seek_active && impl->audio_controller) {
		auto const playing = impl->audio_controller->IsPlaying();
		impl->mouse_position_ms = -1;
		impl->mouse_position_x = -1;
		if (!playing) {
			perf_trace::AudioUiDurationScope trace("audio_display.cursor_update", 1, 0);
			Invalidate(Change::Cursor);
			RequestRepaint(true);
		}
	}
	event.Skip();
}

void SkiaAudioDisplay::OnMouseCaptureLost(wxMouseCaptureLostEvent&) {
	if (!impl)
		return;
	auto const commit_scrollbar_target = impl->scrollbar_dragging
		&& impl->content_scroll_left != impl->scroll_left;
	impl->dragged_markers.clear();
	impl->dragged_button = wxMOUSE_BTN_NONE;
	impl->marker_drag_dead_zone.Reset(0, 0);
	impl->timeline_dragging = false;
	impl->scrollbar_dragging = false;
	Invalidate(Change::Chrome);
	if (commit_scrollbar_target)
		CommitScrollbarContentViewport();
	CancelMiddleSeek();
	SetCursor(wxNullCursor);
	RequestRepaint(true);
}

void SkiaAudioDisplay::OnFocus(wxFocusEvent& event) {
	Invalidate(Change::Chrome);
	RequestRepaint(true);
	event.Skip();
}

void SkiaAudioDisplay::OnKeyDown(wxKeyEvent& event) {
	if (TileDiagnosticsEnabled() && event.GetKeyCode() == WXK_F12 && event.ControlDown() && event.ShiftDown() && !event.AltDown()) {
		CaptureTileDiagnostics();
		return;
	}
	// Mirrors the legacy AudioDisplay and VideoDisplay contract: hotkey::check
	// dispatches the binding and calls evt.Skip() itself when nothing matches,
	// so unmatched keys propagate without an extra Skip wrapper here. The ctor
	// sets wxWANTS_CHARS and binds wxEVT_CHAR_HOOK so navigation keys (arrows,
	// space, tab) reach this handler before default wx handling can swallow them.
	hotkey::check("Audio", impl->project_context, event);
}

void SkiaAudioDisplay::CaptureTileDiagnostics() try {
	if (!impl || !TileDiagnosticsEnabled())
		return;
	auto const frame = impl->last_presented_content_frame;
	if (!frame || !impl->context || !impl->context->IsOK()) {
		LOG_W("audio/display/tile-diagnostics") << "No presented content frame is available for capture";
		return;
	}
	auto const session = perf_trace::GetSessionDirectory();
	if (session.empty() || !perf_trace::IsEnabled()) {
		LOG_W("audio/display/tile-diagnostics") << "Capture requires AEGISUB_PERF_TRACE=audio,log at startup";
		return;
	}

	// This is a readback of the existing frame, not a paint or a content request.
	// Preserve the previously current native context if another widget owned it.
#ifdef _WIN32
	auto const previous_dc = wglGetCurrentDC();
	auto const previous_context = wglGetCurrentContext();
	auto restore_context = agi::make_scope_exit([&] {
		wglMakeCurrent(previous_dc, previous_context);
	});
#endif
	if (!SetCurrent(*impl->context)) {
		LOG_E("audio/display/tile-diagnostics") << "Could not activate the audio context for capture";
		return;
	}
	auto const directory = agi::fs::UniquePath(session / "tile-capture-%%%%%%%%");
	agi::fs::CreateDirectory(directory);
	LOG_I("audio/display/tile-diagnostics") << "Begin " << agi::fs::PathToString(directory.filename())
											<< ", trace frame " << impl->trace_frame_sequence;
	std::string error;
	auto const captured = impl->presenter->CaptureTileDiagnostics(
		impl->ContextToken(), impl->diagnostic_frame_target, *frame, directory, error);

	std::ofstream metadata(directory / "display.txt", std::ios::binary);
	metadata.imbue(std::locale::classic());
	metadata << std::setprecision(17)
			 << "capture_succeeded=" << captured << '\n'
			 << "trace_frame_id=" << impl->trace_frame_sequence << '\n'
			 << "current_provider_generation=" << impl->content_generation.provider << '\n'
			 << "current_analysis_generation=" << impl->content_generation.analysis << '\n'
			 << "last_presented_complete=" << impl->last_presented_complete_content_viewport << '\n'
			 << "last_presented_retained=" << impl->last_presented_retained_content_frame << '\n'
			 << "last_presented_ready_tiles=" << impl->last_presented_ready_tile_count << '\n'
			 << "last_presented_visible_tiles=" << impl->last_presented_visible_tile_count << '\n'
			 << "zoom_level=" << impl->zoom_level << '\n'
			 << "milliseconds_per_pixel=" << impl->content_analysis.milliseconds_per_pixel << '\n'
			 << "source_mode=" << (impl->content_analysis.source_mode == ContentSourceMode::Int16Mono ? "int16_mono" : "float_interleaved") << '\n'
			 << "spectrum_channel_mode=" << static_cast<int>(impl->content_analysis.spectrum_channel_mode) << '\n'
			 << "spectrum_derivation_size=" << impl->content_analysis.spectrum_derivation_size << '\n'
			 << "spectrum_derivation_distance=" << impl->content_analysis.spectrum_derivation_distance << '\n';
	if (impl->provider) {
		metadata << "sample_rate=" << impl->provider->GetSampleRate() << '\n'
				 << "channels=" << impl->provider->GetChannels() << '\n'
				 << "bytes_per_sample=" << impl->provider->GetBytesPerSample() << '\n'
				 << "float_samples=" << impl->provider->AreSamplesFloat() << '\n'
				 << "num_samples=" << impl->provider->GetNumSamples() << '\n'
				 << "decoded_samples=" << impl->provider->GetDecodedSamples() << '\n';
	}
	metadata.close();
	if (!metadata)
		LOG_E("audio/display/tile-diagnostics") << "Could not write capture display metadata";
	if (captured)
		LOG_I("audio/display/tile-diagnostics") << "Saved " << agi::fs::PathToString(directory.filename());
	else
		LOG_E("audio/display/tile-diagnostics") << "Capture failed: " << error;
}
catch (std::exception const& error) {
	LOG_E("audio/display/tile-diagnostics") << "Capture failed: " << error.what();
}
catch (...) {
	LOG_E("audio/display/tile-diagnostics") << "Capture failed with an unknown diagnostic error";
}

void SkiaAudioDisplay::OnAudioOpen(agi::AudioProvider *provider) {
	try {
		Invalidate(Change::Provider);
		impl->provider = provider;
		impl->last_complete_content_frame.reset();
		impl->content_scroll_left = impl->scroll_left;
		impl->last_decoded_samples = provider ? provider->GetDecodedSamples() : 0;
		if (provider && provider->GetDecodedSamples() < provider->GetNumSamples())
			impl->load_timer.Start(100);
		else
			impl->load_timer.Stop();
		auto const generation = impl->content_worker.SetProvider(provider);
		if (generation != impl->content_generation)
			impl->has_last_content_request = false;
		impl->content_generation = generation;
		ReconfigureAnalysis();
		RequestRepaint();
	}
	catch (std::exception const& err) {
		RequestFallback(std::string("Skia Audio content worker initialization failed: ") + err.what());
	}
	catch (...) {
		RequestFallback("an unknown exception escaped Skia Audio content worker initialization");
	}
}

void SkiaAudioDisplay::RequestFallback(std::string message) {
	if (impl->fallback_requested)
		return;
	CancelMiddleSeek();
	impl->fallback_requested = true;
	if (message.empty())
		message = "Skia Audio Display failed without a device diagnostic";
	if (impl->failure_callback)
		impl->failure_callback(std::move(message));
}

void SkiaAudioDisplay::ClearFailureCallback() {
	impl->failure_callback = {};
}

bool SkiaAudioDisplay::HasPresentedContentFrame() const noexcept {
	return impl && impl->content_frame_presented;
}

SkiaAudioDisplayViewState SkiaAudioDisplay::GetViewState() const noexcept {
	if (!impl)
		return {};
	return {
		impl->zoom_level,
		impl->amplitude_scale,
		impl->scroll_left,
	};
}

void SkiaAudioDisplay::SyncToCurrentAudioProvider() {
	OnAudioOpen(impl->project_context->GetCore().project->AudioProvider());
}

void SkiaAudioDisplay::ScrollBy(int pixel_amount) {
	perf_trace::AudioUiDurationScope trace("audio_display.scroll");
	auto const old_scroll_left = impl->scroll_left;
	impl->scroll_left += pixel_amount;
	impl->content_scroll_left = impl->scroll_left;
	RebuildViewport();
	RequestVisibleContent();
	if (impl->scroll_left != old_scroll_left)
		Invalidate(Change::Scroll);
	RequestRepaint(true);
	trace.SetDetails(std::abs(impl->scroll_left - old_scroll_left), impl->viewport.IsValid() ? 1 : 0);
}

void SkiaAudioDisplay::ScrollBy(int pixel_amount, int mouse_x) {
	ScrollBy(pixel_amount);
	if (impl->viewport.IsValid()
		&& impl->audio_controller
		&& !impl->audio_controller->IsPlaying()) {
		auto const value = (impl->scroll_left + mouse_x)
			* AudioMillisecondsPerLogicalPixel(impl->zoom_level);
		impl->mouse_position_ms = static_cast<int>(std::clamp<double>(
			value, 0.0, std::numeric_limits<int>::max()));
		impl->mouse_position_x = mouse_x;
		RequestRepaint(true);
	}
}

void SkiaAudioDisplay::ScrollToTime(int time_ms) {
	RebuildViewport();
	if (!impl->viewport.IsValid())
		return;

	auto const old_scroll_left = impl->scroll_left;
	impl->scroll_left = aegisub::audio::CenteredScrollLeft(
		time_ms,
		GetClientSize().GetWidth(),
		AudioMillisecondsPerLogicalPixel(impl->zoom_level));
	impl->content_scroll_left = impl->scroll_left;
	RebuildViewport();
	RequestVisibleContent();
	if (impl->scroll_left != old_scroll_left)
		Invalidate(Change::Scroll);
	RequestRepaint(true);
}

void SkiaAudioDisplay::ScrollTimeRangeInView(TimeRange const& range) {
	RebuildViewport();
	if (!impl->viewport.IsValid())
		return;
	auto const ms_per_pixel = AudioMillisecondsPerLogicalPixel(impl->zoom_level);
	auto const begin = static_cast<int>(range.begin() / ms_per_pixel);
	auto const end = static_cast<int>(range.end() / ms_per_pixel);
	auto const margin = GetClientSize().GetWidth() / 20;
	auto const page = GetClientSize().GetWidth() - 2 * margin;
	if (begin >= impl->scroll_left + margin && end <= impl->scroll_left + margin + page)
		return;
	if (end - begin < page)
		impl->scroll_left = begin - (page - (end - begin)) / 2 - margin;
	// Range larger than the viewport and the viewport is on a middle slice of
	// it: leave the scroll alone (mirrors legacy AudioDisplay::ScrollTimeRangeInView).
	else if (begin < impl->scroll_left + margin && end > impl->scroll_left + margin + page)
		return;
	else if (end >= impl->scroll_left + margin && end < impl->scroll_left + margin + page)
		impl->scroll_left = end - page - margin;
	else
		impl->scroll_left = begin - margin;
	impl->content_scroll_left = impl->scroll_left;
	RebuildViewport();
	RequestVisibleContent();
	Invalidate(Change::Scroll);
	RequestRepaint(true);
}

void SkiaAudioDisplay::SetZoomLevel(int zoom_level) {
	auto const zoom_changed = impl->zoom_level != zoom_level;
	auto const old_milliseconds_per_pixel = AudioMillisecondsPerLogicalPixel(impl->zoom_level);
	auto const new_milliseconds_per_pixel = AudioMillisecondsPerLogicalPixel(zoom_level);
	if (old_milliseconds_per_pixel != new_milliseconds_per_pixel) {
		auto const anchor_time_ms = impl->playback_position_ms >= 0
			? impl->playback_position_ms : impl->mouse_position_ms;
		impl->scroll_left = AudioScrollLeftAfterZoom(
			impl->scroll_left,
			GetClientSize().GetWidth(),
			old_milliseconds_per_pixel,
			new_milliseconds_per_pixel,
			anchor_time_ms);
		impl->content_scroll_left = impl->scroll_left;
	}
	impl->zoom_level = zoom_level;
	if (zoom_changed)
		Invalidate(Change::Zoom);
	ReconfigureAnalysis();
	RequestRepaint(true);
}

int SkiaAudioDisplay::GetZoomLevel() const {
	return impl->zoom_level;
}

void SkiaAudioDisplay::SetAmplitudeScale(float scale) {
	auto const amplitude_scale = std::max(0.f, scale);
	if (impl->amplitude_scale != amplitude_scale)
		Invalidate(Change::Amplitude);
	impl->amplitude_scale = amplitude_scale;
	RequestRepaint(true);
}

}
