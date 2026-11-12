#pragma once

#include <wx/event.h>

#include <cstdint>
#include <functional>
#include <string>

namespace agi { struct Context; }

class AudioController;
class AudioDisplay;
class TimeRange;
class wxWindow;

namespace aegisub::skia::audio {

class SkiaAudioDisplay;

// AudioBox-private ownership seam. Build-time inclusion and runtime opt-in may
// select the Skia sibling. Failures before the first content frame replace it
// with the unchanged wx AudioDisplay; later failures require user confirmation
// so a working Skia session never silently changes renderer.
class AudioDisplaySlot final : public wxEvtHandler {
	bool runtime_requested = false;
	bool create_skia_widget = false;
	wxWindow *parent = nullptr;
	AudioController *controller = nullptr;
	agi::Context *context = nullptr;
	AudioDisplay *display = nullptr;
	SkiaAudioDisplay *skia_display = nullptr;
	wxWindow *active_window = nullptr;
	std::function<void(wxWindow *)> replacement_callback;
	bool fallback_pending = false;
	bool sync_requested = false;
	bool zoom_set = false;
	bool amplitude_set = false;
	bool visible_range_set = false;
	int zoom_level = 0;
	float amplitude_scale = 1.f;
	int visible_range_begin = 0;
	int visible_range_end = 0;
	std::int64_t pending_scroll_pixels = 0;
	bool exact_scroll_left_set = false;
	int exact_scroll_left = 0;

	void CreateWxDisplay(wxWindow *replaced_window = nullptr);
	void RequestWxFallback(std::string message);
	bool ConfirmRuntimeFallback(std::string const& message);
	void ApplyStateToWxDisplay();

public:
	AudioDisplaySlot(
		wxWindow *parent,
		AudioController *controller,
		agi::Context *context,
		std::function<void(wxWindow *)> replacement_callback);
	~AudioDisplaySlot() override;

	AudioDisplaySlot(AudioDisplaySlot const&) = delete;
	AudioDisplaySlot& operator=(AudioDisplaySlot const&) = delete;

	wxWindow *Window() const { return active_window; }
	bool RuntimeRequested() const { return runtime_requested; }
	bool SkiaWidgetRequested() const { return create_skia_widget; }

	void RetireSubtitleRowBorrows() noexcept;
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
