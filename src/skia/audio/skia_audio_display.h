#pragma once

#include "skia_audio_display_contract.h"
#include "skia_audio_frame_model.h"

#include <wx/glcanvas.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class wxEraseEvent;
class wxPaintEvent;
class wxMouseEvent;
class wxMouseCaptureLostEvent;
class wxFocusEvent;
class wxKeyEvent;
class wxSizeEvent;
class wxDPIChangedEvent;
class wxThreadEvent;
class wxTimerEvent;
class TimeRange;
class AudioController;

namespace agi { class AudioProvider; struct Context; }

namespace aegisub::skia::audio {

struct SkiaAudioDisplayViewState {
	int zoom_level = 0;
	float amplitude_scale = 1.f;
	int scroll_left = 0;

	friend bool operator==(SkiaAudioDisplayViewState const&, SkiaAudioDisplayViewState const&) = default;
};

// Opt-in retained Audio Display. Audio analysis runs on a coalescing worker;
// the UI thread composes cached content tiles with live timing overlays and
// requests a wx AudioDisplay fallback if the Skia/GL boundary fails.
class SkiaAudioDisplay final : public wxGLCanvas {
	struct Impl;
	std::unique_ptr<Impl> impl;

	void OnPaint(wxPaintEvent& event);
	void OnEraseBackground(wxEraseEvent& event);
	void OnSize(wxSizeEvent& event);
	void OnDPIChanged(wxDPIChangedEvent& event);
	void OnContentReady(wxThreadEvent& event);
	void OnContentFailure(wxThreadEvent& event);
	void OnPresentationTimer(wxTimerEvent& event);
	void OnLoadTimer(wxTimerEvent& event);
	void OnMiddleSeekTimer(wxTimerEvent& event);
	void OnAudioOpen(agi::AudioProvider *provider);
	void OnPlaybackPosition(int position_ms);
	void OnPlaybackStop();
	void OnTimingControllerChanged();
	void OnTimingDataChanged();
	void OnMarkerMoved();
	void OnSelectionChanged();
	void OnStyleRangesChanged();
	void OnMouseEvent(wxMouseEvent& event);
	void OnMouseEnter(wxMouseEvent& event);
	void OnMouseLeave(wxMouseEvent& event);
	void OnMouseCaptureLost(wxMouseCaptureLostEvent& event);
	void OnFocus(wxFocusEvent& event);
	void OnKeyDown(wxKeyEvent& event);
	void CaptureTileDiagnostics();
	void EmitMiddleSeekOutput(int time_ms, bool commit);
	void ScheduleMiddleSeekTimer();
	void FinishMiddleSeek(int time_ms);
	void CancelMiddleSeek();
	void OnRenderingSettingsChanged();
	void OnCacheBudgetChanged();
	void ReconfigureAnalysis();
	void RebuildViewport();
	bool EnsureSpectrumBandPlan();
	void RequestVisibleContent();
	bool HasCompleteVisibleContent() const;
	void CommitScrollbarContentViewport();
	void UpdatePresentationTiming();
	void Invalidate(Change change);
	void RequestRepaint(bool interactive = false);
	void RequestFallback(std::string message);

public:
	using FailureCallback = std::function<void(std::string)>;

	SkiaAudioDisplay(
		wxWindow *parent,
		AudioController *controller,
		agi::Context *context,
		FailureInjection failure_injection,
		std::uint64_t failure_injection_after_content_frames,
		FailureCallback failure_callback);
	~SkiaAudioDisplay();

	SkiaAudioDisplay(SkiaAudioDisplay const&) = delete;
	SkiaAudioDisplay& operator=(SkiaAudioDisplay const&) = delete;

	void ClearFailureCallback();
	bool HasPresentedContentFrame() const noexcept;
	SkiaAudioDisplayViewState GetViewState() const noexcept;
	void SyncToCurrentAudioProvider();
	void ScrollBy(int pixel_amount);
	void ScrollBy(int pixel_amount, int mouse_x);
	void ScrollToTime(int time_ms);
	void ScrollTimeRangeInView(TimeRange const& range);
	void SetZoomLevel(int zoom_level);
	int GetZoomLevel() const;
	void SetAmplitudeScale(float scale);
};

}
