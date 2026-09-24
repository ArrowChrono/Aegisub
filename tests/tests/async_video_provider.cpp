#include <main.h>

#include "../../src/ass_dialogue.h"
#include "../../src/ass_file.h"
#include "../../src/async_video_provider.h"
#include "../../src/async_video_trace.h"
#include "../../src/export_fixstyle.h"
#include "../../src/include/aegisub/subtitles_provider.h"
#include "../../src/subtitle_overlay_blend.h"
#include "../../src/ui_services.h"
#include "../../src/transient_font_set.h"
#include "../../src/video_render_geometry.h"
#include "../../src/include/aegisub/video_provider.h"
#include "../../src/video_frame.h"
#include "../../src/video_provider_manager.h"
#include "../../src/visual_tool_render_snapshot.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/scope_exit.h>
#include <libaegisub/vfr.h>

#include <chrono>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace {
struct VideoProviderState {
	std::mutex mutex;
	std::condition_variable cv;
	std::vector<int> requested_frames;
	bool block_next = false;
	bool entered = false;
	bool released = false;
};

class ScopedVideoProviderBlock {
	std::shared_ptr<VideoProviderState> state;
	bool released = false;

public:
	explicit ScopedVideoProviderBlock(std::shared_ptr<VideoProviderState> state)
	: state(std::move(state)) {
		std::lock_guard<std::mutex> lock(this->state->mutex);
		this->state->block_next = true;
		this->state->entered = false;
		this->state->released = false;
	}

	~ScopedVideoProviderBlock() {
		Release();
	}

	bool WaitUntilBlocked() {
		bool entered = false;
		{
			std::unique_lock<std::mutex> lock(state->mutex);
			entered = state->cv.wait_for(lock, std::chrono::seconds(2), [&] { return state->entered; });
		}
		if (!entered)
			Release();
		return entered;
	}

	void Release() {
		if (released)
			return;
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			state->released = true;
			released = true;
		}
		state->cv.notify_all();
	}
};

struct FakeNativeFrameStorage {
	std::vector<unsigned char> plane0;
	std::vector<unsigned char> plane1;
};

class FakeVideoProvider final : public VideoProvider {
	std::shared_ptr<VideoProviderState> state;

public:
	int frame_width = 2;
	int frame_height = 2;
	bool bgra_flipped = false;
	std::string color_space = "BT.709";
	std::string real_color_space = "BT.709";
	SourceFrameNativeFormatIdentity native_format = { };
	SourceFrameChromaLocation native_chroma_location = SourceFrameChromaLocation::Unknown;
	SourceFrameGeometry bgra_geometry = { };
	SourceFrameGeometry native_geometry = MakeDefaultSourceFrameGeometry(2, 2);
	std::vector<SourceFrameOutputMode> available_modes = { SourceFrameOutputMode::Bgra8 };
	SourceFrameOutputMode output_mode = SourceFrameOutputMode::Bgra8;
	std::function<void(int, VideoFrame&)> fill_frame;

	explicit FakeVideoProvider(std::shared_ptr<VideoProviderState> state)
	: state(std::move(state)) {
	}

	void GetFrame(int n, VideoFrame &frame) override {
		{
			std::lock_guard<std::mutex> lock(state->mutex);
			state->requested_frames.push_back(n);
			if (state->block_next) {
				state->entered = true;
				state->cv.notify_all();
			}
		}

		std::unique_lock<std::mutex> lock(state->mutex);
		if (state->block_next) {
			state->cv.wait(lock, [&] { return state->released; });
			state->block_next = false;
			state->released = false;
		}
		lock.unlock();

		frame.width = frame_width;
		frame.height = frame_height;
		frame.pitch = static_cast<size_t>(frame_width) * 4;
		frame.flipped = bgra_flipped;
		frame.data.assign(frame.pitch * frame.height, 0);
		frame.data[0] = static_cast<unsigned char>(n);
		if (fill_frame)
			fill_frame(n, frame);
	}

	void SetColorSpace(std::string const& matrix) override { color_space = matrix; }
	int GetFrameCount() const override { return 100; }
	int GetWidth() const override { return frame_width; }
	int GetHeight() const override { return frame_height; }
	double GetDAR() const override { return 1.0; }
	agi::vfr::Framerate GetFPS() const override { return agi::vfr::Framerate(24.0); }
	std::vector<int> GetKeyFrames() const override { return {}; }
	std::string GetColorSpace() const override { return color_space; }
	std::string GetRealColorSpace() const override { return real_color_space; }
	SourceFrameGeometry GetFrameGeometry() const override {
		if (output_mode == SourceFrameOutputMode::Native)
			return native_geometry;
		if (bgra_geometry.storage_width > 0 && bgra_geometry.storage_height > 0)
			return bgra_geometry;
		return MakeDefaultSourceFrameGeometry(frame_width, frame_height);
	}
	SourceFrameNativeFormatIdentity GetNativeFormatIdentity() const override { return native_format; }
	bool GetNativeFrame(int n, SourceFrame& frame, std::shared_ptr<void>& owner) override {
		if (output_mode != SourceFrameOutputMode::Native)
			return false;

		int native_width = frame_width;
		int native_height = frame_height;
		auto format_info = MakeSemiplanar420SourceFrameFormatInfo(8, 1, 2);
		auto storage = std::make_shared<FakeNativeFrameStorage>();
		storage->plane0.resize(static_cast<size_t>(native_width) * native_height, 0);
		storage->plane1.resize(
			static_cast<size_t>(GetSourceFramePlaneWidth(format_info, native_width, 1))
			* GetSourceFramePlaneHeight(format_info, native_height, 1)
			* format_info.planes[1].bytes_per_sample,
			0);
		storage->plane0[0] = static_cast<unsigned char>(n);
		storage->plane1[0] = static_cast<unsigned char>(n + 1);

		frame = { };
		frame.output_mode = SourceFrameOutputMode::Native;
		frame.native_format = native_format.IsValid()
			? native_format
			: SourceFrameNativeFormatIdentity{ SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 7 };
		frame.format_info = format_info;
		frame.width = native_width;
		frame.height = native_height;
		frame.flipped = false;
		frame.plane_count = frame.format_info.plane_count;
		frame.geometry = native_geometry;
		frame.color = SourceFrameColorMetadataFromLegacyColorSpace(color_space);
		frame.chroma_location = native_chroma_location;
		frame.planes[0] = {
			storage->plane0.data(),
			native_width,
			native_width,
			native_height
		};
		frame.planes[1] = {
			storage->plane1.data(),
			GetSourceFramePlaneWidth(format_info, native_width, 1) * format_info.planes[1].bytes_per_sample,
			GetSourceFramePlaneWidth(format_info, native_width, 1),
			GetSourceFramePlaneHeight(format_info, native_height, 1)
		};
		owner = storage;
		return true;
	}
	std::vector<SourceFrameOutputMode> GetAvailableSourceModes() const override { return available_modes; }
	bool SetOutputMode(SourceFrameOutputMode mode) override {
		if (std::find(available_modes.begin(), available_modes.end(), mode) == available_modes.end())
			return false;
		output_mode = mode;
		return true;
	}
	std::string GetDecoderName() const override { return "fake"; }
};

void SetBgraPixel(VideoFrame& frame, int x, int y, unsigned char b, unsigned char g, unsigned char r) {
	size_t const base = static_cast<size_t>(y) * frame.pitch + static_cast<size_t>(x) * 4;
	frame.data[base + 0] = b;
	frame.data[base + 1] = g;
	frame.data[base + 2] = r;
	frame.data[base + 3] = 255;
}

void FillSyntheticVisibilityFrame(
	int n,
	VideoFrame& frame,
	int visibility,
	bool noisy_background) {
	constexpr int background_b = 16;
	constexpr int background_g = 24;
	constexpr int background_r = 32;
	constexpr int foreground_b = 196;
	constexpr int foreground_g = 148;
	constexpr int foreground_r = 100;

	for (int y = 0; y < static_cast<int>(frame.height); ++y) {
		for (int x = 0; x < static_cast<int>(frame.width); ++x) {
			int noise = noisy_background ? ((x * 13 + y * 7 + n * 5) % 9) - 4 : 0;
			int b = background_b + noise;
			int g = background_g + noise;
			int r = background_r + noise;
			if (x >= 8 && x < 15 && y >= 8 && y < 15) {
				b = (foreground_b * visibility + background_b * (100 - visibility)) / 100;
				g = (foreground_g * visibility + background_g * (100 - visibility)) / 100;
				r = (foreground_r * visibility + background_r * (100 - visibility)) / 100;
			}
			SetBgraPixel(frame, x, y, static_cast<unsigned char>(b), static_cast<unsigned char>(g), static_cast<unsigned char>(r));
		}
	}
}

void FillSyntheticFadeFrame(
	int n,
	VideoFrame& frame,
	bool hard_cut = false,
	bool noisy_background = false) {
	int visibility = 100;
	if (hard_cut)
		visibility = n >= 7 && n <= 17 ? 100 : 0;
	else if (n < 2)
		visibility = 0;
	else if (n < 7)
		visibility = (n - 2) * 20;
	else if (n <= 17)
		visibility = 100;
	else if (n < 23)
		visibility = (22 - n) * 20;
	else
		visibility = 0;
	FillSyntheticVisibilityFrame(n, frame, visibility, noisy_background);
}

void FillSyntheticLongFadeFrame(int n, VideoFrame& frame) {
	int visibility = 100;
	if (n < 5)
		visibility = 0;
	else if (n < 25)
		visibility = (n - 5) * 5;
	else if (n <= 45)
		visibility = 100;
	else if (n < 66)
		visibility = (65 - n) * 5;
	else
		visibility = 0;
	FillSyntheticVisibilityFrame(n, frame, visibility, false);
}

void FillSyntheticWhiteTextSceneFrame(int n, VideoFrame& frame, bool white_background_after_text = false) {
	// The text first becomes visible at frame 6, reaches full opacity at 15,
	// starts fading after 65, and is completely absent from frame 75 onward.
	int const visibility = std::clamp(std::min(n - 5, 75 - n) * 10, 0, 100);
	int const width = static_cast<int>(frame.width);
	int const height = static_cast<int>(frame.height);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			int background = 32;
			if (n >= 30 && n < 50)
				background = 208;
			else if (n >= 50)
				background = (x / 12 + y / 10) % 2 ? 224 : 48;
			if (white_background_after_text && n >= 75)
				background = 236;

			// A box with a middle stroke and three separate horizontal strokes
			// provide glyph edges in both directions without relying on a font.
			bool const horizontal = (y >= 24 && y < 26) || (y >= 31 && y < 33) || (y >= 39 && y < 41);
			bool const box = x >= 34 && x < 48 && y >= 24 && y < 41 && (x < 36 || x >= 46 || horizontal);
			bool const bars = x >= 55 && x < 71 && horizontal;
			int value = box || bars
							? (255 * visibility + background * (100 - visibility)) / 100
							: background;
			// The clicked pixel stays white after the text vanishes, but the
			// small replacement block does not have the selected glyph's shape.
			if (white_background_after_text && n >= 75 && x >= 39 && x < 42 && y >= 23 && y < 26)
				value = 255;
			auto const channel = static_cast<unsigned char>(value);
			SetBgraPixel(frame, x, y, channel, channel, channel);
		}
	}
}

void FillSyntheticWhiteGlyphFrame(VideoFrame& frame, int visibility, int glyph_x, int glyph_y, int background = 32) {
	int const width = static_cast<int>(frame.width);
	int const height = static_cast<int>(frame.height);
	for (int y = 0; y < height; ++y) {
		int const storage_y = frame.flipped ? height - 1 - y : y;
		for (int x = 0; x < width; ++x) {
			int const dx = x - glyph_x;
			int const dy = y - glyph_y;
			bool const horizontal = dy < 2 || (dy >= 7 && dy < 9) || dy >= 15;
			bool const glyph = dx >= 0 && dx < 14 && dy >= 0 && dy < 17 && (dx < 2 || dx >= 12 || horizontal);
			// Composite a white box glyph over a uniform background using the
			// supplied opacity, independently of the scan's visibility model.
			auto const channel = static_cast<unsigned char>(glyph
																? (255 * visibility + background * (100 - visibility)) / 100
																: background);
			SetBgraPixel(frame, x, storage_y, channel, channel, channel);
		}
	}
}

class FakeSubtitlesProvider final : public SubtitlesProvider {
public:
	int load_calls = 0;

private:
	void LoadSubtitles(const char *, size_t) override {
		++load_calls;
	}

public:
	void DrawSubtitles(VideoFrame &dst, double) override {
		if (dst.data.size() < 2)
			dst.data.resize(2);
		dst.data[1] = static_cast<unsigned char>(load_calls);
	}
};

class FakeOverlaySubtitlesProvider final : public SubtitlesProvider {
public:
	int load_calls = 0;

private:
	void LoadSubtitles(const char *, size_t) override {
		++load_calls;
	}

public:
	SubtitleRenderMode GetRenderMode() const override {
		return SubtitleRenderMode::PremultipliedOverlay;
	}

	bool RenderOverlayClearsTarget() const override {
		return true;
	}

	bool RenderOverlay(SourceFrame const&, SubtitleOverlay& overlay, double) override {
		overlay.premultiplied_alpha = true;
		overlay.has_visible_content = true;
		for (int y = 0; y < overlay.height; ++y)
			std::memset(overlay.planes[0].data + static_cast<std::ptrdiff_t>(y) * overlay.planes[0].stride, 0, static_cast<size_t>(overlay.width) * 4);
		auto *pixel = overlay.planes[0].data;
		pixel[0] = 10;
		pixel[1] = 20;
		pixel[2] = 30;
		pixel[3] = 128;
		return true;
	}

	void DrawSubtitles(VideoFrame &, double) override {
		FAIL() << "legacy subtitle path should not be used";
	}
};

class ThrowingOverlaySubtitlesProvider final : public SubtitlesProvider {
	void LoadSubtitles(const char *, size_t) override { }

public:
	SubtitleRenderMode GetRenderMode() const override {
		return SubtitleRenderMode::PremultipliedOverlay;
	}

	bool RenderOverlay(SourceFrame const&, SubtitleOverlay&, double) override {
		throw agi::InternalError("synthetic overlay render failure");
	}

	void DrawSubtitles(VideoFrame &, double) override {
		FAIL() << "legacy subtitle path should not be used";
	}
};

class FakeGeometryAwareOverlaySubtitlesProvider final : public SubtitlesProvider {
public:
	int load_calls = 0;
	int render_overlay_calls = 0;
	SourceFrameGeometry last_source_geometry = { };
	int last_overlay_target_x = 0;
	int last_overlay_target_y = 0;
	int last_overlay_width = 0;
	int last_overlay_height = 0;

private:
	void LoadSubtitles(const char *, size_t) override {
		++load_calls;
	}

public:
	SubtitleRenderMode GetRenderMode() const override {
		return SubtitleRenderMode::PremultipliedOverlay;
	}

	bool RenderOverlayClearsTarget() const override {
		return true;
	}

	bool RenderOverlay(SourceFrame const& source, SubtitleOverlay& overlay, double) override {
		++render_overlay_calls;
		last_source_geometry = source.geometry;

		overlay.premultiplied_alpha = true;
		overlay.has_visible_content = true;
		overlay.canvas_width = source.geometry.storage_width;
		overlay.canvas_height = source.geometry.storage_height;
		overlay.target_x = 1;
		overlay.target_y = 2;
		overlay.width = 4;
		overlay.height = 2;
		last_overlay_target_x = overlay.target_x;
		last_overlay_target_y = overlay.target_y;
		last_overlay_width = overlay.width;
		last_overlay_height = overlay.height;
		overlay.planes[0].data +=
			static_cast<std::ptrdiff_t>(overlay.target_y) * overlay.planes[0].stride +
			static_cast<std::ptrdiff_t>(overlay.target_x) * 4;
		overlay.planes[0].width = overlay.width;
		overlay.planes[0].height = overlay.height;

		for (int y = 0; y < overlay.height; ++y) {
			auto* row = overlay.planes[0].data + static_cast<std::ptrdiff_t>(y) * overlay.planes[0].stride;
			for (int x = 0; x < overlay.width; ++x) {
				auto* pixel = row + static_cast<std::ptrdiff_t>(x) * 4;
				pixel[0] = 10;
				pixel[1] = 20;
				pixel[2] = 30;
				pixel[3] = 200;
			}
		}
		return true;
	}

	void DrawSubtitles(VideoFrame &, double) override {
		FAIL() << "legacy subtitle path should not be used";
	}
};

class FakeDirtyRectOverlaySubtitlesProvider final : public SubtitlesProvider {
	std::vector<SubtitleOverlayDirtyRect> dirty_rects;
	int render_calls = 0;

private:
	void LoadSubtitles(const char *, size_t) override {
	}

public:
	SubtitleRenderMode GetRenderMode() const override {
		return SubtitleRenderMode::PremultipliedOverlay;
	}

	bool SupportsOverlayDirtyRects() const override {
		return true;
	}

	bool RenderOverlay(SourceFrame const&, SubtitleOverlay& overlay, double) override {
		++render_calls;
		overlay.premultiplied_alpha = true;
		overlay.has_visible_content = true;
		for (int y = 0; y < overlay.height; ++y)
			std::memset(overlay.planes[0].data + static_cast<std::ptrdiff_t>(y) * overlay.planes[0].stride, 0, static_cast<size_t>(overlay.width) * 4);

		auto* pixel = overlay.planes[0].data + 4;
		pixel[0] = 5;
		pixel[1] = 6;
		pixel[2] = 7;
		pixel[3] = 255;

		dirty_rects.clear();
		if (render_calls == 1)
			dirty_rects.push_back({ 1, 0, 1, 1 });

		overlay.dirty_rects = dirty_rects.empty() ? nullptr : dirty_rects.data();
		overlay.dirty_rect_count = static_cast<int>(dirty_rects.size());
		return true;
	}

	void DrawSubtitles(VideoFrame &, double) override {
		FAIL() << "legacy subtitle path should not be used";
	}
};

class FakeDropSensitiveOverlaySubtitlesProvider final : public SubtitlesProvider {
	std::vector<SubtitleOverlayDirtyRect> dirty_rects;
	int render_calls = 0;

private:
	void LoadSubtitles(const char *, size_t) override {
	}

public:
	SubtitleRenderMode GetRenderMode() const override {
		return SubtitleRenderMode::PremultipliedOverlay;
	}

	bool SupportsOverlayDirtyRects() const override {
		return true;
	}

	bool RenderOverlay(SourceFrame const&, SubtitleOverlay& overlay, double) override {
		++render_calls;
		overlay.premultiplied_alpha = true;
		overlay.has_visible_content = true;
		for (int y = 0; y < overlay.height; ++y)
			std::memset(overlay.planes[0].data + static_cast<std::ptrdiff_t>(y) * overlay.planes[0].stride, 0, static_cast<size_t>(overlay.width) * 4);

		auto* pixel = overlay.planes[0].data + 4;
		pixel[0] = static_cast<unsigned char>(10 + render_calls);
		pixel[1] = 20;
		pixel[2] = 30;
		pixel[3] = 255;

		dirty_rects.clear();
		if (render_calls == 1)
			dirty_rects.push_back({ 0, 0, overlay.width, overlay.height });

		overlay.dirty_rects = dirty_rects.empty() ? nullptr : dirty_rects.data();
		overlay.dirty_rect_count = static_cast<int>(dirty_rects.size());
		return true;
	}

	void DrawSubtitles(VideoFrame &, double) override {
		FAIL() << "legacy subtitle path should not be used";
	}
};

class FakeInvisibleOverlaySubtitlesProvider final : public SubtitlesProvider {
private:
	void LoadSubtitles(const char *, size_t) override {
	}

public:
	SubtitleRenderMode GetRenderMode() const override {
		return SubtitleRenderMode::PremultipliedOverlay;
	}

	bool RenderOverlayClearsTarget() const override {
		return true;
	}

	bool SupportsOverlayDirtyRects() const override {
		return true;
	}

	bool RenderOverlay(SourceFrame const&, SubtitleOverlay& overlay, double) override {
		overlay.premultiplied_alpha = true;
		overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;
		overlay.has_visible_content = false;
		overlay.dirty_rects = nullptr;
		overlay.dirty_rect_count = 0;
		return true;
	}

	void DrawSubtitles(VideoFrame &, double) override {
		FAIL() << "legacy subtitle path should not be used";
	}
};

class FakeCompatibilityOnlySubtitlesProvider final : public SubtitlesProvider {
public:
	int load_calls = 0;
	int render_overlay_calls = 0;
	int draw_calls = 0;
	std::mutex mutex;
	std::condition_variable cv;
	bool block_next_draw = false;
	bool draw_entered = false;
	bool draw_released = false;

	bool WaitForDrawEntered() {
		std::unique_lock<std::mutex> lock(mutex);
		return cv.wait_for(lock, std::chrono::seconds(2), [&] { return draw_entered; });
	}

	void ReleaseDraw() {
		{
			std::lock_guard<std::mutex> lock(mutex);
			draw_released = true;
		}
		cv.notify_all();
	}

private:
	void LoadSubtitles(const char *, size_t) override {
		++load_calls;
	}

public:
	SubtitleRenderMode GetRenderMode() const override {
		return SubtitleRenderMode::CompatibilityFrameOnly;
	}

	bool RenderOverlay(SourceFrame const&, SubtitleOverlay&, double) override {
		++render_overlay_calls;
		return true;
	}

	void DrawSubtitles(VideoFrame &dst, double) override {
		{
			std::unique_lock<std::mutex> lock(mutex);
			if (block_next_draw) {
				draw_entered = true;
				cv.notify_all();
				cv.wait(lock, [&] { return draw_released; });
				block_next_draw = false;
				draw_released = false;
			}
		}

		++draw_calls;
		if (dst.data.size() < 2)
			dst.data.resize(2);
		dst.data[1] = static_cast<unsigned char>(10 + load_calls);
	}
};

class FakeActivationAwareSubtitlesProvider final : public SubtitlesProvider {
public:
	int activation_calls = 0;

private:
	void LoadSubtitles(const char *, size_t) override {
	}

public:
	void OnActivated() override {
		++activation_calls;
	}

	void DrawSubtitles(VideoFrame &, double) override {
	}
};

class FakeActivationOrderSubtitlesProvider final : public SubtitlesProvider {
	int *destruction_count = nullptr;

private:
	void LoadSubtitles(const char *, size_t) override {
	}

public:
	int destroyed_before_activation = -1;

	explicit FakeActivationOrderSubtitlesProvider(int *destruction_count)
	: destruction_count(destruction_count) {
	}

	~FakeActivationOrderSubtitlesProvider() override {
		if (destruction_count)
			++*destruction_count;
	}

	void OnActivated() override {
		destroyed_before_activation = destruction_count ? *destruction_count : -1;
	}

	void DrawSubtitles(VideoFrame &, double) override {
	}
};

class FakeVisualToolRenderSnapshot final : public VisualToolRenderSnapshot {
	public:
	FakeVisualToolRenderSnapshot()
		: VisualToolRenderSnapshot(std::make_shared<VisualToolRenderContext>()) {}

	void Draw(VideoOverlayDrawContext&) const override {
		ADD_FAILURE() << "the subtitle worker must not draw visual-tool snapshots";
	}
};

struct RecordedFrame {
	int frame_number = -1;
	int subtitle_generation = -1;
	VideoRenderDeliveryVersion delivery_version;
	double time = 0.0;
	bool has_overlay = false;
	int overlay_dirty_rect_count = 0;
	uint64_t overlay_continuity_generation = 0;
	SourceFrameRect source_visible_rect = { };
	VideoRenderDeliveryClass delivery_class = VideoRenderDeliveryClass::EveryFrame;
	uint64_t visual_interaction_id = 0;
	std::shared_ptr<const VisualToolRenderSnapshot> visual_tool_snapshot;
};

class EventRecorder {
	std::mutex mutex;
	std::condition_variable cv;
	std::vector<RecordedFrame> frames;
	std::vector<std::string> subtitle_errors;

public:
	void operator()(VideoRenderPacket packet, double time) {
		auto display_frame = packet.DisplayFrame();
		RecordedFrame frame;
		frame.frame_number = display_frame && !display_frame->data.empty() ? display_frame->data[0] : -1;
		frame.subtitle_generation = display_frame && display_frame->data.size() > 1 ? display_frame->data[1] : -1;
		frame.delivery_version = packet.delivery_version;
		frame.time = time;
		frame.has_overlay = packet.has_subtitle_overlay;
		frame.overlay_dirty_rect_count = packet.subtitle_overlay.dirty_rect_count;
		frame.overlay_continuity_generation = packet.subtitle_overlay.continuity_generation;
		frame.source_visible_rect = GetSourceFrameVisibleRect(packet.source_frame);
		frame.delivery_class = packet.delivery_class;
		frame.visual_interaction_id = packet.visual_interaction_id;
		frame.visual_tool_snapshot = std::move(packet.visual_tool_snapshot);

		{
			std::lock_guard<std::mutex> lock(mutex);
			frames.push_back(frame);
		}
		cv.notify_all();
	}

	operator AsyncVideoProviderEventSink() {
		AsyncVideoProviderEventSink sink;
		sink.on_frame_ready = [this](VideoRenderPacket packet, double time) {
			(*this)(std::move(packet), time);
		};
		sink.on_subtitles_error = [this](std::string const& message) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				subtitle_errors.push_back(message);
			}
			cv.notify_all();
		};
		return sink;
	}

	bool WaitForCount(size_t count) {
		std::unique_lock<std::mutex> lock(mutex);
		return cv.wait_for(lock, std::chrono::seconds(2), [&] { return frames.size() >= count; });
	}

	std::vector<RecordedFrame> Snapshot() {
		std::lock_guard<std::mutex> lock(mutex);
		return frames;
	}

	bool WaitForSubtitleErrorCount(size_t count) {
		std::unique_lock<std::mutex> lock(mutex);
		return cv.wait_for(lock, std::chrono::seconds(2), [&] { return subtitle_errors.size() >= count; });
	}

	std::vector<std::string> SubtitleErrors() {
		std::lock_guard<std::mutex> lock(mutex);
		return subtitle_errors;
	}
};

class MainThreadDeliveryFixture : public ::testing::Test {
protected:
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<agi::dispatch::Thunk> main_queue;
	std::thread::id main_thread_id = std::this_thread::get_id();

	void SetUp() override {
		agi::dispatch::Init([this](agi::dispatch::Thunk thunk) {
			{
				std::lock_guard<std::mutex> lock(mutex);
				main_queue.emplace_back(std::move(thunk));
			}
			cv.notify_all();
		}, [this] {
			return std::this_thread::get_id() == main_thread_id;
		}, [this] {
			return PumpMainTasks();
		});
	}

	void TearDown() override {
		PumpMainTasks();
		agi::dispatch::Init([](agi::dispatch::Thunk) { }, [] {
			return false;
		}, [] {
			return std::size_t{ 0 };
		});
	}

	bool WaitForMainTasks(size_t count) {
		std::unique_lock<std::mutex> lock(mutex);
		return cv.wait_for(lock, std::chrono::seconds(2), [&] { return main_queue.size() >= count; });
	}

	std::size_t MainTaskCount() {
		std::lock_guard<std::mutex> lock(mutex);
		return main_queue.size();
	}

	std::size_t PumpOneMainTask() {
		agi::dispatch::Thunk thunk;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (main_queue.empty())
				return 0;
			thunk = std::move(main_queue.front());
			main_queue.pop_front();
		}
		thunk();
		return 1;
	}

	std::size_t PumpMainTasks() {
		std::size_t executed = 0;
		while (PumpOneMainTask())
			++executed;
		return executed;
	}
};

class SubtitleLoadRecorder {
	std::mutex mutex;
	std::condition_variable cv;
	std::vector<std::vector<std::string>> snapshots;

public:
	void Record(AssFile const& file) {
		std::vector<std::string> texts;
		texts.reserve(file.Events.size());
		for (auto const& line : file.Events)
			texts.push_back(line.Text.get());

		{
			std::lock_guard<std::mutex> lock(mutex);
			snapshots.push_back(std::move(texts));
		}
		cv.notify_all();
	}

	bool WaitForCount(size_t count) {
		std::unique_lock<std::mutex> lock(mutex);
		return cv.wait_for(lock, std::chrono::seconds(2), [&] { return snapshots.size() >= count; });
	}

	bool WaitForSnapshot(std::vector<std::string> const& expected) {
		std::unique_lock<std::mutex> lock(mutex);
		return cv.wait_for(lock, std::chrono::seconds(2), [&] {
			return std::find(snapshots.begin(), snapshots.end(), expected) != snapshots.end();
		});
	}

	std::vector<std::vector<std::string>> Snapshot() {
		std::lock_guard<std::mutex> lock(mutex);
		return snapshots;
	}
};

SubtitleLoadRecorder *g_subtitle_load_recorder = nullptr;

class ScopedSubtitleLoadRecorder {
	SubtitleLoadRecorder *previous = nullptr;

public:
	explicit ScopedSubtitleLoadRecorder(SubtitleLoadRecorder& recorder)
	: previous(g_subtitle_load_recorder) {
		g_subtitle_load_recorder = &recorder;
	}

	~ScopedSubtitleLoadRecorder() {
		g_subtitle_load_recorder = previous;
	}
};

AssFile MakeSubtitleFile(std::string const& text) {
	AssFile file;
	auto *line = new AssDialogue;
	line->Start = 0;
	line->End = 5000;
	line->Row = 0;
	line->Text = text;
	file.Events.push_back(*line);
	return file;
}

AssFile MakeSubtitleFile(std::string const& first, std::string const& second) {
	auto file = MakeSubtitleFile(first);
	auto *line = new AssDialogue;
	line->Start = 0;
	line->End = 5000;
	line->Row = 1;
	line->Text = second;
	file.Events.push_back(*line);
	return file;
}

AssFile MakeSubtitleFile(std::string const& first, std::string const& second, std::string const& third) {
	auto file = MakeSubtitleFile(first, second);
	auto *line = new AssDialogue;
	line->Start = 0;
	line->End = 5000;
	line->Row = 2;
	line->Text = third;
	file.Events.push_back(*line);
	return file;
}

std::function<std::unique_ptr<VideoProvider>()> g_video_provider_factory;
std::function<std::unique_ptr<SubtitlesProvider>(SubtitleRenderEnvironment const&)> g_subtitles_provider_factory;
std::shared_ptr<const TransientFontSet> g_last_factory_transient_fonts;
std::shared_ptr<agi::SingleChoiceInteractionSink> g_last_factory_choice_sink;
agi::BackgroundRunner *g_last_factory_background_runner = nullptr;

struct ScopedFactoryOverride final {
	~ScopedFactoryOverride() {
		g_video_provider_factory = nullptr;
		g_subtitles_provider_factory = nullptr;
		g_last_factory_transient_fonts.reset();
		g_last_factory_choice_sink.reset();
		g_last_factory_background_runner = nullptr;
	}
};
}

std::vector<std::string> VideoProviderFactory::GetClasses() { return {}; }
std::vector<std::pair<std::string, std::string>> VideoProviderFactory::GetChoices() { return {}; }
std::unique_ptr<VideoProvider> VideoProviderFactory::GetProvider(agi::fs::path const&, std::string const&, agi::BackgroundRunner *, std::shared_ptr<agi::SingleChoiceInteractionSink> choice_sink) {
	g_last_factory_choice_sink = std::move(choice_sink);
	if (g_video_provider_factory)
		return g_video_provider_factory();
	return nullptr;
}
std::vector<std::string> SubtitlesProviderFactory::GetClasses() { return {}; }
std::unique_ptr<SubtitlesProvider> SubtitlesProviderFactory::GetProvider(SubtitleRenderEnvironment const& env) {
	g_last_factory_background_runner = env.background_runner;
	g_last_factory_transient_fonts = env.transient_fonts;
	if (g_subtitles_provider_factory)
		return g_subtitles_provider_factory(env);
	return nullptr;
}
void SubtitlesProvider::LoadSubtitles(AssFile *subs, int, agi::vfr::Framerate const*) {
	if (g_subtitle_load_recorder)
		g_subtitle_load_recorder->Record(*subs);
	static const char payload[] = "test";
	LoadSubtitles(payload, sizeof(payload) - 1);
}
void AssFixStylesFilter::ProcessSubs(AssFile *) { }

TEST(ass_file_transient_fonts, copy_assignment_and_swap_preserve_shared_state) {
	auto primary_fonts = std::make_shared<TransientFontSet>();
	primary_fonts->generation = 17;
	primary_fonts->fonts.push_back({ "primary.ttf", "font/ttf", { 'a', 'b', 'c' } });

	auto secondary_fonts = std::make_shared<TransientFontSet>();
	secondary_fonts->generation = 23;
	secondary_fonts->fonts.push_back({ "secondary.otf", "font/otf", { 'x', 'y' } });

	AssFile original;
	original.SetTransientFonts(primary_fonts);

	AssFile copied(original);
	ASSERT_TRUE(copied.GetTransientFonts());
	EXPECT_EQ(primary_fonts, copied.GetTransientFonts());
	EXPECT_EQ(17u, copied.GetTransientFonts()->generation);
	ASSERT_EQ(1u, copied.GetTransientFonts()->fonts.size());
	EXPECT_EQ("primary.ttf", copied.GetTransientFonts()->fonts.front().original_name);

	AssFile assigned;
	assigned = original;
	ASSERT_TRUE(assigned.GetTransientFonts());
	EXPECT_EQ(primary_fonts, assigned.GetTransientFonts());

	AssFile other;
	other.SetTransientFonts(secondary_fonts);
	original.swap(other);
	EXPECT_EQ(secondary_fonts, original.GetTransientFonts());
	EXPECT_EQ(primary_fonts, other.GetTransientFonts());
}

TEST(ass_file_transient_fonts, explicit_reset_clears_transient_fonts) {
	auto fonts = std::make_shared<TransientFontSet>();
	fonts->generation = 5;
	fonts->fonts.push_back({ "font.ttf", "font/ttf", { '1' } });

	AssFile file;
	file.SetTransientFonts(fonts);
	ASSERT_EQ(fonts, file.GetTransientFonts());

	file.SetTransientFonts({});
	EXPECT_FALSE(file.GetTransientFonts());
}

TEST(async_video_provider, request_frame_keeps_only_latest_pending_render) {
	auto state = std::make_shared<VideoProviderState>();
	state->block_next = true;
	auto *subs = new FakeSubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	provider.RequestFrame(1, 1000);

	{
		std::unique_lock<std::mutex> lock(state->mutex);
		ASSERT_TRUE(state->cv.wait_for(lock, std::chrono::seconds(2), [&] { return state->entered; }));
	}

	provider.RequestFrame(2, 2000);
	provider.RequestFrame(3, 3000);
	provider.RequestFrame(4, 4000);

	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->released = true;
	}
	state->cv.notify_all();

	ASSERT_TRUE(recorder.WaitForCount(1));
	auto frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_EQ(4, frames.back().frame_number);

	std::lock_guard<std::mutex> lock(state->mutex);
	EXPECT_EQ((std::vector<int>{1, 4}), state->requested_frames);
}

TEST(async_video_provider, cancel_pending_frame_requests_drops_in_flight_render) {
	auto state = std::make_shared<VideoProviderState>();
	state->block_next = true;
	auto *subs = new FakeSubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	provider.RequestFrame(1, 1000);

	{
		std::unique_lock<std::mutex> lock(state->mutex);
		ASSERT_TRUE(state->cv.wait_for(lock, std::chrono::seconds(2), [&] { return state->entered; }));
	}

	provider.CancelPendingFrameRequests();

	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->released = true;
	}
	state->cv.notify_all();

	provider.GetRenderPacket(2, 2000);
	EXPECT_TRUE(recorder.Snapshot().empty());

	std::lock_guard<std::mutex> lock(state->mutex);
	EXPECT_EQ((std::vector<int>{1, 2}), state->requested_frames);
}

TEST(async_video_provider, cancelling_prefetch_stops_after_running_decode) {
	auto state = std::make_shared<VideoProviderState>();
	EventRecorder recorder;
	AsyncVideoProvider provider(
		CreateCacheVideoProvider(agi::make_unique<FakeVideoProvider>(state), 1024),
		agi::make_unique<FakeSubtitlesProvider>(), recorder);
	ScopedVideoProviderBlock block(state);

	provider.PrefetchFrames(10, 4);
	ASSERT_TRUE(block.WaitUntilBlocked());
	provider.CancelFramePrefetch();
	block.Release();
	// The running prefetch posts its successor behind this worker barrier.
	provider.CollectMemoryStats();
	provider.CollectMemoryStats();

	EXPECT_TRUE(recorder.Snapshot().empty());
	{
		std::scoped_lock lock(state->mutex);
		EXPECT_EQ((std::vector<int>{10}), state->requested_frames);
	}
	provider.RequestFrame(10, 1000);
	ASSERT_TRUE(recorder.WaitForCount(1));
	EXPECT_EQ(10, recorder.Snapshot().front().frame_number);
	std::scoped_lock lock(state->mutex);
	EXPECT_EQ((std::vector<int>{10}), state->requested_frames);
}

TEST(async_video_provider, cancelling_prefetch_preserves_in_flight_preview_delivery) {
	auto state = std::make_shared<VideoProviderState>();
	EventRecorder recorder;
	AsyncVideoProvider provider(
		CreateCacheVideoProvider(agi::make_unique<FakeVideoProvider>(state), 1024),
		agi::make_unique<FakeSubtitlesProvider>(), recorder);
	ScopedVideoProviderBlock block(state);

	provider.RequestFrame(20, 2000);
	ASSERT_TRUE(block.WaitUntilBlocked());
	provider.PrefetchFrames(21, 4);
	provider.CancelFramePrefetch();
	block.Release();
	ASSERT_TRUE(recorder.WaitForCount(1));
	provider.CollectMemoryStats();

	auto const frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_EQ(20, frames.front().frame_number);
	EXPECT_EQ(2000, frames.front().time);
	std::scoped_lock lock(state->mutex);
	EXPECT_EQ((std::vector<int>{20}), state->requested_frames);
}

TEST(async_video_provider, prefetch_does_not_repost_after_interactive_request_arrives) {
	auto state = std::make_shared<VideoProviderState>();
	EventRecorder recorder;
	AsyncVideoProvider provider(
		CreateCacheVideoProvider(agi::make_unique<FakeVideoProvider>(state), 1024),
		agi::make_unique<FakeSubtitlesProvider>(), recorder);
	ScopedVideoProviderBlock block(state);

	provider.PrefetchFrames(10, 4);
	ASSERT_TRUE(block.WaitUntilBlocked());
	provider.RequestFrame(20, 2000);
	block.Release();

	ASSERT_TRUE(recorder.WaitForCount(1));
	provider.CollectMemoryStats();
	std::scoped_lock lock(state->mutex);
	EXPECT_EQ((std::vector<int>{10, 20}), state->requested_frames);
}

TEST(async_video_provider, prefetch_does_not_repost_after_request_version_changes) {
	auto state = std::make_shared<VideoProviderState>();
	EventRecorder recorder;
	AsyncVideoProvider provider(
		CreateCacheVideoProvider(agi::make_unique<FakeVideoProvider>(state), 1024),
		agi::make_unique<FakeSubtitlesProvider>(), recorder);
	ScopedVideoProviderBlock block(state);

	provider.PrefetchFrames(10, 4);
	ASSERT_TRUE(block.WaitUntilBlocked());
	provider.CancelPendingFrameRequests();
	block.Release();

	provider.CollectMemoryStats();
	std::scoped_lock lock(state->mutex);
	EXPECT_EQ((std::vector<int>{10}), state->requested_frames);
}

TEST(async_video_provider, load_subtitles_invalidates_stale_render_result) {
	auto state = std::make_shared<VideoProviderState>();
	state->block_next = true;
	auto *subs = new FakeSubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto first = MakeSubtitleFile("old");
	auto second = MakeSubtitleFile("new");

	provider.LoadSubtitles(&first);
	provider.RequestFrame(1, 1000);

	{
		std::unique_lock<std::mutex> lock(state->mutex);
		ASSERT_TRUE(state->cv.wait_for(lock, std::chrono::seconds(2), [&] { return state->entered; }));
	}

	provider.LoadSubtitles(&second);

	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->released = true;
	}
	state->cv.notify_all();

	ASSERT_TRUE(recorder.WaitForCount(1));
	auto frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_EQ(1, frames.back().frame_number);
	EXPECT_EQ(2, frames.back().subtitle_generation);
}

TEST(async_video_provider, pipeline_events_correlate_superseded_render_and_latest_delivery) {
	using aegisub::async_video_trace::PipelineEvent;
	class PipelineRecorder final : public aegisub::async_video_trace::Sink {
		std::mutex mutex;
		std::vector<PipelineEvent> events;

		public:
		void ObserveFrameResult(int, double, bool, bool) override {}
		void ObserveVideoFrameRenderDuration(int, double, bool, bool, double) override {}
		void ObservePipelineEvent(PipelineEvent const& event) override {
			std::scoped_lock lock(mutex);
			events.push_back(event);
		}
		std::vector<PipelineEvent> Snapshot() {
			std::scoped_lock lock(mutex);
			return events;
		}
	} pipeline;
	// The provider's inner scope drains its worker before this disconnects the
	// sink, including when an ASSERT returns early from the test.
	auto disconnect = agi::make_scope_exit([] { aegisub::async_video_trace::SetSink(nullptr); });
	auto state = std::make_shared<VideoProviderState>();
	EventRecorder recorder;
	auto subtitles = MakeSubtitleFile("before");
	auto stale_snapshot = std::make_shared<FakeVisualToolRenderSnapshot>();
	std::weak_ptr<const VisualToolRenderSnapshot> stale_lifetime = stale_snapshot;
	auto final_snapshot = std::make_shared<FakeVisualToolRenderSnapshot>();
	std::uint64_t provider_id = 0;
	{
		AsyncVideoProvider provider(
			agi::make_unique<FakeVideoProvider>(state),
			agi::make_unique<FakeSubtitlesProvider>(), recorder);
		provider_id = provider.GetRawVideoIdentity().generation;
		provider.SetCurrentFrameContext(7, 1000);
		provider.CollectMemoryStats();
		aegisub::async_video_trace::SetSink(&pipeline);
		ScopedVideoProviderBlock block(state);

		provider.LoadSubtitles(&subtitles, {.delivery_class = VideoRenderDeliveryClass::VisualSubtitleIntermediate,
											.visual_interaction_id = 41,
											.visual_tool_snapshot = stale_snapshot});
		stale_snapshot.reset();
		// Source acquisition is inside ProcRenderPacket, after worker_render_begin
		// and the pre-render version check. Supersede that exact in-flight pass.
		ASSERT_TRUE(block.WaitUntilBlocked());
		EXPECT_FALSE(stale_lifetime.expired());
		subtitles.Events.front().Text = "after";
		provider.UpdateSubtitles(&subtitles, &subtitles.Events.front(), {.delivery_class = VideoRenderDeliveryClass::VisualSubtitleFinal, .visual_interaction_id = 42, .force_current_frame_render = true, .visual_tool_snapshot = final_snapshot});
		block.Release();
		ASSERT_TRUE(recorder.WaitForCount(1));
	}

	auto const events = pipeline.Snapshot();
	ASSERT_EQ(8u, events.size());
	constexpr std::array<char const *, 8> stages = {
		"submit_load", "worker_take", "worker_render_begin", "submit_update",
		"worker_render_drop", "worker_take", "worker_render_begin", "worker_render_deliver"};
	constexpr std::array<std::uint64_t, 8> contents = {1, 1, 1, 2, 1, 2, 2, 2};
	for (size_t index = 0; index < events.size(); ++index) {
		SCOPED_TRACE(index);
		auto const& event = events[index];
		bool const latest = contents[index] == 2;
		EXPECT_STREQ(stages[index], event.stage);
		EXPECT_EQ(provider_id, event.version.provider);
		EXPECT_EQ(contents[index], event.version.content);
		EXPECT_EQ(0u, event.version.request);
		EXPECT_EQ(latest ? VideoRenderDeliveryClass::VisualSubtitleFinal : VideoRenderDeliveryClass::VisualSubtitleIntermediate,
				  event.delivery_class);
		EXPECT_EQ(latest ? 42u : 41u, event.visual_interaction_id);
		EXPECT_EQ(index == 0 || index == 3 ? -1 : 7, event.frame);
		EXPECT_GT(event.timestamp_ns, 0);
		if (index == 4 || index == 7)
			EXPECT_GE(event.duration_ms, 0.0);
		else
			EXPECT_EQ(-1.0, event.duration_ms);
	}

	auto const frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	VideoRenderDeliveryVersion const latest_version{.provider = provider_id, .content = 2, .request = 0};
	EXPECT_EQ(latest_version, frames.front().delivery_version);
	EXPECT_EQ(7, frames.front().frame_number);
	EXPECT_EQ(1000, frames.front().time);
	EXPECT_EQ(2, frames.front().subtitle_generation);
	EXPECT_EQ(VideoRenderDeliveryClass::VisualSubtitleFinal, frames.front().delivery_class);
	EXPECT_EQ(42u, frames.front().visual_interaction_id);
	EXPECT_EQ(final_snapshot, frames.front().visual_tool_snapshot);
	EXPECT_TRUE(stale_lifetime.expired());
}

TEST_F(MainThreadDeliveryFixture, queued_same_frame_packet_is_rejected_after_subtitle_reload) {
	std::unique_ptr<AsyncVideoProvider> provider;
	auto event_lifetime = agi::ui::MakeLifetime();
	int expected_frame = 7;
	int callback_count = 0;
	bool version_current = true;
	bool frame_matches = false;
	bool accepted = true;

	auto sink = CreateAsyncVideoProviderMainThreadSink(
		event_lifetime,
		{
			[&](VideoRenderPacket packet, double) {
				++callback_count;
				version_current = provider->IsCurrent(packet.delivery_version);
				frame_matches = packet.frame_number == expected_frame;
				accepted = provider->IsCurrent(packet, expected_frame);
			},
			{},
			{}
		});
	provider = agi::make_unique<AsyncVideoProvider>(
		agi::make_unique<FakeVideoProvider>(std::make_shared<VideoProviderState>()),
		agi::make_unique<FakeSubtitlesProvider>(),
		std::move(sink));

	auto old_subtitles = MakeSubtitleFile("old");
	auto new_subtitles = MakeSubtitleFile("new");
	provider->LoadSubtitles(&old_subtitles);
	provider->RequestFrame(expected_frame, 7000);
	ASSERT_TRUE(WaitForMainTasks(1));

	// The old result is already in the UI queue when the same-frame style data changes.
	provider->LoadSubtitles(&new_subtitles);
	ASSERT_EQ(1u, PumpOneMainTask());

	EXPECT_EQ(1, callback_count);
	EXPECT_TRUE(frame_matches);
	EXPECT_FALSE(version_current);
	EXPECT_FALSE(accepted);
}

TEST_F(MainThreadDeliveryFixture, queued_packet_is_rejected_when_expected_frame_changes) {
	std::unique_ptr<AsyncVideoProvider> provider;
	auto event_lifetime = agi::ui::MakeLifetime();
	int expected_frame = 3;
	int callback_count = 0;
	bool version_current = false;
	bool frame_matches = true;
	bool accepted = true;

	auto sink = CreateAsyncVideoProviderMainThreadSink(
		event_lifetime,
		{
			[&](VideoRenderPacket packet, double) {
				++callback_count;
				version_current = provider->IsCurrent(packet.delivery_version);
				frame_matches = packet.frame_number == expected_frame;
				accepted = provider->IsCurrent(packet, expected_frame);
			},
			{},
			{}
		});
	provider = agi::make_unique<AsyncVideoProvider>(
		agi::make_unique<FakeVideoProvider>(std::make_shared<VideoProviderState>()),
		agi::make_unique<FakeSubtitlesProvider>(),
		std::move(sink));

	provider->RequestFrame(expected_frame, 3000);
	ASSERT_TRUE(WaitForMainTasks(1));
	expected_frame = 4;
	ASSERT_EQ(1u, PumpOneMainTask());

	EXPECT_EQ(1, callback_count);
	EXPECT_TRUE(version_current);
	EXPECT_FALSE(frame_matches);
	EXPECT_FALSE(accepted);
}

TEST_F(MainThreadDeliveryFixture, queued_packet_is_rejected_after_provider_replacement) {
	std::unique_ptr<AsyncVideoProvider> provider;
	auto event_lifetime = agi::ui::MakeLifetime();
	int callback_count = 0;
	bool version_current = true;
	auto sink = CreateAsyncVideoProviderMainThreadSink(
		event_lifetime,
		{
			[&](VideoRenderPacket packet, double) {
				++callback_count;
				version_current = provider->IsCurrent(packet.delivery_version);
			},
			{},
			{}
		});
	provider = agi::make_unique<AsyncVideoProvider>(
		agi::make_unique<FakeVideoProvider>(std::make_shared<VideoProviderState>()),
		agi::make_unique<FakeSubtitlesProvider>(),
		std::move(sink));

	provider->RequestFrame(3, 3000);
	ASSERT_TRUE(WaitForMainTasks(1));
	provider = agi::make_unique<AsyncVideoProvider>(
		agi::make_unique<FakeVideoProvider>(std::make_shared<VideoProviderState>()),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});
	ASSERT_EQ(1u, PumpOneMainTask());

	EXPECT_EQ(1, callback_count);
	EXPECT_FALSE(version_current);
}

TEST_F(MainThreadDeliveryFixture, visual_subtitle_batches_stop_when_ui_lifetime_expires) {
	auto event_lifetime = agi::ui::MakeLifetime();
	int callback_count = 0;
	auto sink = CreateAsyncVideoProviderMainThreadSink(
		event_lifetime,
		{
			[&](VideoRenderPacket, double) {
				++callback_count;
			},
			{},
		{}},
		AsyncVideoFrameDeliveryMode::VisualSubtitleBatches);

	VideoRenderPacket packet;
	packet.delivery_class = VideoRenderDeliveryClass::VisualSubtitleIntermediate;
	packet.visual_interaction_id = 11;
	sink.on_frame_ready(std::move(packet), 0.0);
	ASSERT_TRUE(WaitForMainTasks(1));
	event_lifetime.reset();

	EXPECT_EQ(1u, PumpOneMainTask());
	EXPECT_EQ(0, callback_count);
	EXPECT_EQ(0u, MainTaskCount());
}

TEST_F(MainThreadDeliveryFixture, default_delivery_preserves_every_frame) {
	auto event_lifetime = agi::ui::MakeLifetime();
	std::vector<int> delivered_frames;
	auto sink = CreateAsyncVideoProviderMainThreadSink(
		event_lifetime,
		{
			[&](VideoRenderPacket packet, double) {
				delivered_frames.push_back(packet.frame_number);
			},
			{},
			{}
		});

	for (int frame = 0; frame < 3; ++frame) {
		VideoRenderPacket packet;
		packet.frame_number = frame;
		sink.on_frame_ready(std::move(packet), 0.0);
	}

	ASSERT_TRUE(WaitForMainTasks(3));
	EXPECT_EQ(3u, MainTaskCount());
	EXPECT_EQ(3u, PumpMainTasks());
	EXPECT_EQ((std::vector<int>{0, 1, 2}), delivered_frames);
}

TEST_F(MainThreadDeliveryFixture, visual_subtitle_batches_coalesce_only_visual_packets) {
	auto event_lifetime = agi::ui::MakeLifetime();
	std::vector<int> delivered_frames;
	auto sink = CreateAsyncVideoProviderMainThreadSink(
		event_lifetime,
		{
			[&](VideoRenderPacket packet, double) {
				delivered_frames.push_back(packet.frame_number);
			},
			{},
			{}},
		AsyncVideoFrameDeliveryMode::VisualSubtitleBatches);

	auto visual_packet = [](int frame, VideoRenderDeliveryClass delivery_class, uint64_t interaction_id) {
		VideoRenderPacket packet;
		packet.frame_number = frame;
		packet.delivery_class = delivery_class;
		packet.visual_interaction_id = interaction_id;
		return packet;
	};

	sink.on_frame_ready(visual_packet(1, VideoRenderDeliveryClass::VisualSubtitleIntermediate, 7), 0.0);
	sink.on_frame_ready(visual_packet(2, VideoRenderDeliveryClass::VisualSubtitleIntermediate, 7), 1.0);
	sink.on_frame_ready(visual_packet(3, VideoRenderDeliveryClass::VisualSubtitleIntermediate, 7), 2.0);
	sink.on_frame_ready(visual_packet(4, VideoRenderDeliveryClass::EveryFrame, 0), 3.0);
	sink.on_frame_ready(visual_packet(5, VideoRenderDeliveryClass::VisualSubtitleIntermediate, 7), 4.0);
	sink.on_frame_ready(visual_packet(6, VideoRenderDeliveryClass::EveryFrame, 0), 5.0);
	sink.on_frame_ready(visual_packet(7, VideoRenderDeliveryClass::EveryFrame, 0), 6.0);

	ASSERT_TRUE(WaitForMainTasks(5));
	EXPECT_EQ(5u, MainTaskCount());
	EXPECT_EQ(5u, PumpMainTasks());
	EXPECT_EQ((std::vector<int>{3, 4, 5, 6, 7}), delivered_frames);
}

TEST_F(MainThreadDeliveryFixture, visual_subtitle_batch_after_normal_packet_does_not_overtake_it) {
	auto event_lifetime = agi::ui::MakeLifetime();
	std::vector<int> delivered_frames;
	auto sink = CreateAsyncVideoProviderMainThreadSink(
		event_lifetime,
		{
			[&](VideoRenderPacket packet, double) {
				delivered_frames.push_back(packet.frame_number);
			},
			{},
		{}},
		AsyncVideoFrameDeliveryMode::VisualSubtitleBatches);

	VideoRenderPacket first_visual;
	first_visual.frame_number = 1;
	first_visual.delivery_class = VideoRenderDeliveryClass::VisualSubtitleIntermediate;
	first_visual.visual_interaction_id = 12;
	sink.on_frame_ready(std::move(first_visual), 0.0);

	VideoRenderPacket normal;
	normal.frame_number = 2;
	normal.delivery_class = VideoRenderDeliveryClass::EveryFrame;
	sink.on_frame_ready(std::move(normal), 1.0);

	VideoRenderPacket later_visual;
	later_visual.frame_number = 3;
	later_visual.delivery_class = VideoRenderDeliveryClass::VisualSubtitleIntermediate;
	later_visual.visual_interaction_id = 12;
	sink.on_frame_ready(std::move(later_visual), 2.0);

	ASSERT_TRUE(WaitForMainTasks(3));
	EXPECT_EQ(3u, MainTaskCount());
	EXPECT_EQ(3u, PumpMainTasks());
	EXPECT_EQ((std::vector<int>{1, 2, 3}), delivered_frames);
}

TEST_F(MainThreadDeliveryFixture, visual_subtitle_batches_keep_visual_queue_bounded_while_every_frame_remains_exact) {
	auto event_lifetime = agi::ui::MakeLifetime();
	std::vector<int> delivered_frames;
	auto sink = CreateAsyncVideoProviderMainThreadSink(
		event_lifetime,
		{
			[&](VideoRenderPacket packet, double) {
				delivered_frames.push_back(packet.frame_number);
			},
			{},
		{}},
		AsyncVideoFrameDeliveryMode::VisualSubtitleBatches);

	for (int frame = 0; frame < 100; ++frame) {
		VideoRenderPacket packet;
		packet.frame_number = frame;
		packet.delivery_class = VideoRenderDeliveryClass::VisualSubtitleIntermediate;
		packet.visual_interaction_id = 9;
		sink.on_frame_ready(std::move(packet), frame);
	}

	ASSERT_TRUE(WaitForMainTasks(1));
	EXPECT_EQ(1u, MainTaskCount());
	ASSERT_EQ(1u, PumpMainTasks());
	ASSERT_EQ(1u, delivered_frames.size());
	EXPECT_EQ(99, delivered_frames.front());
}

TEST_F(MainThreadDeliveryFixture, visual_subtitle_final_packet_is_not_replaced_by_late_intermediate) {
	auto event_lifetime = agi::ui::MakeLifetime();
	std::vector<VideoRenderDeliveryClass> delivered_classes;
	auto sink = CreateAsyncVideoProviderMainThreadSink(
		event_lifetime,
		{
			[&](VideoRenderPacket packet, double) {
				delivered_classes.push_back(packet.delivery_class);
			},
			{},
			{}},
		AsyncVideoFrameDeliveryMode::VisualSubtitleBatches);

	VideoRenderPacket intermediate;
	intermediate.frame_number = 1;
	intermediate.delivery_class = VideoRenderDeliveryClass::VisualSubtitleIntermediate;
	intermediate.visual_interaction_id = 8;
	sink.on_frame_ready(std::move(intermediate), 0.0);

	VideoRenderPacket final;
	final.frame_number = 2;
	final.delivery_class = VideoRenderDeliveryClass::VisualSubtitleFinal;
	final.visual_interaction_id = 8;
	sink.on_frame_ready(std::move(final), 1.0);

	VideoRenderPacket late_intermediate;
	late_intermediate.frame_number = 3;
	late_intermediate.delivery_class = VideoRenderDeliveryClass::VisualSubtitleIntermediate;
	late_intermediate.visual_interaction_id = 8;
	sink.on_frame_ready(std::move(late_intermediate), 2.0);

	ASSERT_TRUE(WaitForMainTasks(1));
	EXPECT_EQ(1u, MainTaskCount());
	ASSERT_EQ(1u, PumpMainTasks());
	ASSERT_EQ(1u, delivered_classes.size());
	EXPECT_EQ(VideoRenderDeliveryClass::VisualSubtitleFinal, delivered_classes.front());

	late_intermediate = {};
	late_intermediate.frame_number = 4;
	late_intermediate.delivery_class = VideoRenderDeliveryClass::VisualSubtitleIntermediate;
	late_intermediate.visual_interaction_id = 8;
	sink.on_frame_ready(std::move(late_intermediate), 3.0);
	EXPECT_EQ(0u, MainTaskCount());
}

TEST(async_video_provider, pending_full_subtitles_coalesce_same_line_updates) {
	auto state = std::make_shared<VideoProviderState>();
	SubtitleLoadRecorder load_recorder;
	ScopedSubtitleLoadRecorder scoped_load_recorder(load_recorder);

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	ScopedVideoProviderBlock block(state);
	provider.RequestFrame(1, 1000);
	ASSERT_TRUE(block.WaitUntilBlocked());

	auto subtitles = MakeSubtitleFile("before", "unchanged");
	provider.LoadSubtitles(&subtitles);
	subtitles.Events.front().Text = "during";
	provider.UpdateSubtitles(&subtitles, &subtitles.Events.front());
	subtitles.Events.front().Text = "latest";
	provider.UpdateSubtitles(&subtitles, &subtitles.Events.front());

	block.Release();

	ASSERT_TRUE(load_recorder.WaitForSnapshot({ "latest", "unchanged" }));
}

TEST(async_video_provider, pending_visual_snapshot_tracks_latest_subtitle_content_and_version) {
	auto state = std::make_shared<VideoProviderState>();
	SubtitleLoadRecorder load_recorder;
	ScopedSubtitleLoadRecorder scoped_load_recorder(load_recorder);
	EventRecorder recorder;
	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		agi::make_unique<FakeSubtitlesProvider>(), recorder);
	ScopedVideoProviderBlock block(state);
	provider.RequestFrame(1, 1000);
	ASSERT_TRUE(block.WaitUntilBlocked());

	auto first_snapshot = std::make_shared<FakeVisualToolRenderSnapshot>();
	std::weak_ptr<const VisualToolRenderSnapshot> first_lifetime = first_snapshot;
	auto latest_snapshot = std::make_shared<FakeVisualToolRenderSnapshot>();
	auto subtitles = MakeSubtitleFile("before", "unchanged");
	provider.LoadSubtitles(&subtitles, {.delivery_class = VideoRenderDeliveryClass::VisualSubtitleIntermediate,
										.visual_interaction_id = 71,
										.visual_tool_snapshot = first_snapshot});
	first_snapshot.reset();
	EXPECT_FALSE(first_lifetime.expired());
	subtitles.Events.front().Text = "latest";
	provider.UpdateSubtitles(&subtitles, &subtitles.Events.front(), {.delivery_class = VideoRenderDeliveryClass::VisualSubtitleIntermediate, .visual_interaction_id = 72, .visual_tool_snapshot = latest_snapshot});
	EXPECT_TRUE(first_lifetime.expired());
	block.Release();
	provider.CollectMemoryStats();

	auto const frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_EQ(latest_snapshot, frames.front().visual_tool_snapshot);
	EXPECT_EQ((std::vector<std::vector<std::string>>{{"latest", "unchanged"}}), load_recorder.Snapshot());
	EXPECT_EQ((VideoRenderDeliveryVersion{.provider = provider.GetRawVideoIdentity().generation,
										  .content = 2,
										  .request = 1}),
			  frames.front().delivery_version);
	EXPECT_EQ(VideoRenderDeliveryClass::VisualSubtitleIntermediate, frames.front().delivery_class);
	EXPECT_EQ(72u, frames.front().visual_interaction_id);
	EXPECT_EQ(1, frames.front().frame_number);
	EXPECT_EQ(1000, frames.front().time);
}

TEST(async_video_provider, newest_null_visual_snapshot_clears_pending_overlay) {
	auto state = std::make_shared<VideoProviderState>();
	SubtitleLoadRecorder load_recorder;
	ScopedSubtitleLoadRecorder scoped_load_recorder(load_recorder);
	EventRecorder recorder;
	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		agi::make_unique<FakeSubtitlesProvider>(), recorder);
	ScopedVideoProviderBlock block(state);
	provider.RequestFrame(1, 1000);
	ASSERT_TRUE(block.WaitUntilBlocked());

	auto snapshot = std::make_shared<FakeVisualToolRenderSnapshot>();
	std::weak_ptr<const VisualToolRenderSnapshot> lifetime = snapshot;
	auto subtitles = MakeSubtitleFile("with tool");
	provider.LoadSubtitles(&subtitles, {.delivery_class = VideoRenderDeliveryClass::VisualSubtitleIntermediate,
										.visual_interaction_id = 71,
										.visual_tool_snapshot = snapshot});
	snapshot.reset();
	subtitles.Events.front().Text = "without tool";
	provider.UpdateSubtitles(&subtitles, &subtitles.Events.front(), {.delivery_class = VideoRenderDeliveryClass::VisualSubtitleIntermediate, .visual_interaction_id = 72});
	EXPECT_TRUE(lifetime.expired());
	block.Release();
	provider.CollectMemoryStats();

	auto const frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_FALSE(frames.front().visual_tool_snapshot);
	EXPECT_EQ((std::vector<std::vector<std::string>>{{"without tool"}}), load_recorder.Snapshot());
	EXPECT_EQ(2u, frames.front().delivery_version.content);
	EXPECT_EQ(VideoRenderDeliveryClass::VisualSubtitleIntermediate, frames.front().delivery_class);
	EXPECT_EQ(72u, frames.front().visual_interaction_id);
}

TEST(async_video_provider, explicit_frame_request_strips_pending_visual_snapshot) {
	auto state = std::make_shared<VideoProviderState>();
	EventRecorder recorder;
	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		agi::make_unique<FakeSubtitlesProvider>(), recorder);
	ScopedVideoProviderBlock block(state);
	provider.RequestFrame(1, 1000);
	ASSERT_TRUE(block.WaitUntilBlocked());

	auto snapshot = std::make_shared<FakeVisualToolRenderSnapshot>();
	std::weak_ptr<const VisualToolRenderSnapshot> lifetime = snapshot;
	auto subtitles = MakeSubtitleFile("seek");
	provider.LoadSubtitles(&subtitles, {.delivery_class = VideoRenderDeliveryClass::VisualSubtitleIntermediate,
										.visual_interaction_id = 71,
										.visual_tool_snapshot = snapshot});
	snapshot.reset();
	EXPECT_FALSE(lifetime.expired());
	provider.RequestFrame(2, 2000);
	block.Release();
	provider.CollectMemoryStats();

	auto const frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_EQ(2, frames.front().frame_number);
	EXPECT_EQ(2000, frames.front().time);
	EXPECT_EQ(VideoRenderDeliveryClass::EveryFrame, frames.front().delivery_class);
	EXPECT_EQ(0u, frames.front().visual_interaction_id);
	EXPECT_FALSE(frames.front().visual_tool_snapshot);
	EXPECT_TRUE(lifetime.expired());
	EXPECT_EQ((VideoRenderDeliveryVersion{.provider = provider.GetRawVideoIdentity().generation,
										  .content = 1,
										  .request = 2}),
			  frames.front().delivery_version);
}

TEST(async_video_provider, visual_snapshots_release_without_frame_or_visible_change) {
	auto state = std::make_shared<VideoProviderState>();
	EventRecorder recorder;
	auto subtitles_provider = agi::make_unique<FakeSubtitlesProvider>();
	auto *subtitles_provider_ptr = subtitles_provider.get();
	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state), std::move(subtitles_provider), recorder);
	auto subtitles = MakeSubtitleFile("unchanged");
	auto no_frame_snapshot = std::make_shared<FakeVisualToolRenderSnapshot>();
	std::weak_ptr<const VisualToolRenderSnapshot> no_frame_lifetime = no_frame_snapshot;
	provider.LoadSubtitles(&subtitles, {.delivery_class = VideoRenderDeliveryClass::VisualSubtitleIntermediate,
										.visual_tool_snapshot = no_frame_snapshot});
	no_frame_snapshot.reset();
	provider.CollectMemoryStats();
	EXPECT_TRUE(no_frame_lifetime.expired());
	EXPECT_TRUE(recorder.Snapshot().empty());
	EXPECT_EQ(0, subtitles_provider_ptr->load_calls);

	provider.RequestFrame(1, 1000);
	provider.CollectMemoryStats();
	ASSERT_EQ(1u, recorder.Snapshot().size());
	ASSERT_EQ(1, subtitles_provider_ptr->load_calls);
	auto no_change_snapshot = std::make_shared<FakeVisualToolRenderSnapshot>();
	std::weak_ptr<const VisualToolRenderSnapshot> no_change_lifetime = no_change_snapshot;
	provider.UpdateSubtitles(&subtitles, &subtitles.Events.front(), {.delivery_class = VideoRenderDeliveryClass::VisualSubtitleIntermediate, .visual_tool_snapshot = no_change_snapshot});
	no_change_snapshot.reset();
	provider.CollectMemoryStats();
	EXPECT_TRUE(no_change_lifetime.expired());
	EXPECT_EQ(1u, recorder.Snapshot().size());
	EXPECT_EQ(1, subtitles_provider_ptr->load_calls);
}

TEST(async_video_provider, visual_snapshot_releases_after_subtitle_render_error) {
	auto state = std::make_shared<VideoProviderState>();
	EventRecorder recorder;
	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		agi::make_unique<ThrowingOverlaySubtitlesProvider>(), recorder);
	provider.SetCurrentFrameContext(1, 1000);
	provider.CollectMemoryStats();
	auto subtitles = MakeSubtitleFile("failure path");
	auto snapshot = std::make_shared<FakeVisualToolRenderSnapshot>();
	std::weak_ptr<const VisualToolRenderSnapshot> lifetime = snapshot;
	provider.LoadSubtitles(&subtitles, {.delivery_class = VideoRenderDeliveryClass::VisualSubtitleIntermediate,
										.visual_tool_snapshot = snapshot});
	snapshot.reset();
	provider.CollectMemoryStats();
	auto const errors = recorder.SubtitleErrors();
	ASSERT_EQ(1u, errors.size());
	EXPECT_NE(std::string::npos, errors.front().find("synthetic overlay render failure"));
	EXPECT_TRUE(recorder.Snapshot().empty());
	EXPECT_TRUE(lifetime.expired());
}

TEST(async_video_provider, pending_different_line_updates_preserve_both_lines) {
	auto state = std::make_shared<VideoProviderState>();
	SubtitleLoadRecorder load_recorder;
	ScopedSubtitleLoadRecorder scoped_load_recorder(load_recorder);

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	auto subtitles = MakeSubtitleFile("first-before", "second-before");
	provider.LoadSubtitles(&subtitles);
	provider.GetRenderPacket(0, 0);
	ASSERT_TRUE(load_recorder.WaitForCount(1));

	ScopedVideoProviderBlock block(state);
	provider.RequestFrame(1, 1000);
	ASSERT_TRUE(block.WaitUntilBlocked());

	auto first = subtitles.Events.begin();
	auto second = std::next(first);
	first->Text = "first-latest";
	provider.UpdateSubtitles(&subtitles, &*first);
	second->Text = "second-latest";
	provider.UpdateSubtitles(&subtitles, &*second);

	block.Release();

	ASSERT_TRUE(load_recorder.WaitForSnapshot({ "first-latest", "second-latest" }));
}

TEST(async_video_provider, pending_multi_line_update_applies_all_rows) {
	auto state = std::make_shared<VideoProviderState>();
	SubtitleLoadRecorder load_recorder;
	ScopedSubtitleLoadRecorder scoped_load_recorder(load_recorder);

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	ScopedVideoProviderBlock block(state);
	provider.RequestFrame(1, 1000);
	ASSERT_TRUE(block.WaitUntilBlocked());

	auto subtitles = MakeSubtitleFile("first-before", "second-before", "third-before");
	provider.LoadSubtitles(&subtitles);
	auto first = subtitles.Events.begin();
	auto third = std::next(first, 2);
	first->Text = "first-latest";
	third->Text = "third-latest";
	const AssDialogue *changed[] = { &*third, &*first };
	provider.UpdateSubtitles(&subtitles, changed);

	block.Release();

	ASSERT_TRUE(load_recorder.WaitForSnapshot({ "first-latest", "second-before", "third-latest" }));
}

TEST(async_video_provider, pending_multi_line_updates_merge_across_calls_latest_wins) {
	auto state = std::make_shared<VideoProviderState>();
	SubtitleLoadRecorder load_recorder;
	ScopedSubtitleLoadRecorder scoped_load_recorder(load_recorder);

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	auto subtitles = MakeSubtitleFile("first-before", "second-before", "third-before");
	provider.LoadSubtitles(&subtitles);
	provider.GetRenderPacket(0, 0);
	ASSERT_TRUE(load_recorder.WaitForSnapshot({ "first-before", "second-before", "third-before" }));

	ScopedVideoProviderBlock block(state);
	provider.RequestFrame(1, 1000);
	ASSERT_TRUE(block.WaitUntilBlocked());

	auto first = subtitles.Events.begin();
	auto second = std::next(first);
	auto third = std::next(second);
	first->Text = "first-update";
	second->Text = "second-outdated";
	const AssDialogue *first_batch[] = { &*first, &*second };
	provider.UpdateSubtitles(&subtitles, first_batch);

	second->Text = "second-latest";
	third->Text = "third-latest";
	const AssDialogue *second_batch[] = { &*third, &*second };
	provider.UpdateSubtitles(&subtitles, second_batch);

	block.Release();

	ASSERT_TRUE(load_recorder.WaitForSnapshot({ "first-update", "second-latest", "third-latest" }));
}

TEST(async_video_provider, same_size_reorder_falls_back_to_latest_full_snapshot) {
	auto state = std::make_shared<VideoProviderState>();
	SubtitleLoadRecorder load_recorder;
	ScopedSubtitleLoadRecorder scoped_load_recorder(load_recorder);

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	auto subtitles = MakeSubtitleFile("first-before", "second-before");
	provider.LoadSubtitles(&subtitles);
	provider.GetRenderPacket(0, 0);
	ASSERT_TRUE(load_recorder.WaitForSnapshot({ "first-before", "second-before" }));

	ScopedVideoProviderBlock block(state);
	provider.RequestFrame(1, 1000);
	ASSERT_TRUE(block.WaitUntilBlocked());

	subtitles.Events.reverse();
	int row = 0;
	for (auto& line : subtitles.Events)
		line.Row = row++;
	subtitles.Events.front().Text = "second-latest";
	provider.UpdateSubtitles(&subtitles, &subtitles.Events.front());

	block.Release();

	ASSERT_TRUE(load_recorder.WaitForSnapshot({ "second-latest", "first-before" }));
}

TEST(async_video_provider, multi_line_update_rejects_any_pointer_outside_source_rows) {
	auto state = std::make_shared<VideoProviderState>();
	SubtitleLoadRecorder load_recorder;
	ScopedSubtitleLoadRecorder scoped_load_recorder(load_recorder);

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	ScopedVideoProviderBlock block(state);
	provider.RequestFrame(1, 1000);
	ASSERT_TRUE(block.WaitUntilBlocked());

	auto subtitles = MakeSubtitleFile("first-before", "second-before");
	provider.LoadSubtitles(&subtitles);
	auto second = std::next(subtitles.Events.begin());
	subtitles.Events.front().Text = "first-latest";
	second->Text = "second-latest";
	AssDialogue external;
	external.Row = 0;
	external.Text = "external-invalid";
	const AssDialogue *changed[] = { &external, &*second };
	provider.UpdateSubtitles(&subtitles, changed);

	block.Release();

	ASSERT_TRUE(load_recorder.WaitForSnapshot({ "first-latest", "second-latest" }));
}

TEST(async_video_provider, source_id_change_at_same_address_falls_back_to_latest_full_snapshot) {
	auto state = std::make_shared<VideoProviderState>();
	SubtitleLoadRecorder load_recorder;
	ScopedSubtitleLoadRecorder scoped_load_recorder(load_recorder);

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	ScopedVideoProviderBlock block(state);
	provider.RequestFrame(1, 1000);
	ASSERT_TRUE(block.WaitUntilBlocked());

	auto subtitles = MakeSubtitleFile("first-before", "second-before");
	provider.LoadSubtitles(&subtitles);
	auto& first = subtitles.Events.front();
	first.Id += 1000000;
	first.Text = "first-latest";
	std::next(subtitles.Events.begin())->Text = "second-latest";
	provider.UpdateSubtitles(&subtitles, &first);

	block.Release();

	ASSERT_TRUE(load_recorder.WaitForSnapshot({ "first-latest", "second-latest" }));
}

TEST(async_video_provider, get_frame_flushes_pending_subtitle_state) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeSubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("sync");
	provider.LoadSubtitles(&subtitle_file);

	auto frame = provider.GetFrame(7, 7000);
	ASSERT_TRUE(frame);
	ASSERT_GE(frame->data.size(), 2u);
	EXPECT_EQ(7, frame->data[0]);
	EXPECT_EQ(1, frame->data[1]);
}

TEST(async_video_provider, get_frame_bgra_returns_cpu_frame_when_native_mode_selected) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		recorder);

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	ASSERT_EQ(SourceFrameOutputMode::Native, provider.GetSelectedSourceMode());
	ASSERT_EQ(SourceFrameOutputMode::Native, video->output_mode);

	auto frame = provider.GetFrameBgra(7, 7000, true);
	ASSERT_TRUE(frame);
	ASSERT_GE(frame->data.size(), 1u);
	EXPECT_EQ(7, frame->data[0]);
	EXPECT_EQ(SourceFrameOutputMode::Native, provider.GetSelectedSourceMode());
	EXPECT_EQ(SourceFrameOutputMode::Native, video->output_mode);
}

TEST(async_video_provider, get_frame_bgra_bakes_direct_overlay_for_cpu_consumers) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	auto frame = provider.GetFrameBgra(9, 9000);
	ASSERT_TRUE(frame);
	ASSERT_GE(frame->data.size(), 4u);
	// We only care that the overlay path is baked into the CPU frame.
	// Exact blend math is validated in subtitle_overlay_blend tests.
	EXPECT_NE(9, frame->data[0]);
	EXPECT_GT(frame->data[1], 0);
	EXPECT_GT(frame->data[2], 0);

	std::lock_guard<std::mutex> lock(state->mutex);
	EXPECT_EQ((std::vector<int>{ 9 }), state->requested_frames);
}

TEST(async_video_provider, find_key_point_range_scans_frames_inside_worker) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 4;
	video->frame_height = 4;
	video->fill_frame = [](int n, VideoFrame& frame) {
		auto set_pixel = [&](int x, int y, unsigned char b, unsigned char g, unsigned char r) {
			size_t base = static_cast<size_t>(y) * frame.pitch + static_cast<size_t>(x) * 4;
			frame.data[base + 0] = b;
			frame.data[base + 1] = g;
			frame.data[base + 2] = r;
			frame.data[base + 3] = 255;
		};

		if (n >= 4 && n <= 8) {
			set_pixel(1, 1, 40, 80, 120);
			set_pixel(0, 1, 40, 80, 120);
			set_pixel(2, 1, 40, 80, 120);
			set_pixel(1, 0, 40, 80, 120);
			set_pixel(1, 2, 40, 80, 120);
		}
	};
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		recorder);

	auto result = provider.FindKeyPointRange({
		5,
		1,
		1,
		120,
		80,
		40,
		0,
		2,
		0
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_EQ(4, result.left);
	EXPECT_EQ(8, result.right);
	EXPECT_EQ((std::vector<int>{ 5, 3, 4, 7, 9, 8 }), state->requested_frames);
}

TEST(async_video_provider, find_key_point_range_respects_flipped_frame_coordinates) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 3;
	video->frame_height = 3;
	video->bgra_flipped = true;
	video->fill_frame = [](int n, VideoFrame& frame) {
		auto set_pixel = [&](int x, int y, unsigned char b, unsigned char g, unsigned char r) {
			size_t base = static_cast<size_t>(y) * frame.pitch + static_cast<size_t>(x) * 4;
			frame.data[base + 0] = b;
			frame.data[base + 1] = g;
			frame.data[base + 2] = r;
			frame.data[base + 3] = 255;
		};

		if (n >= 2 && n <= 4) {
			set_pixel(1, 2, 10, 30, 90);
			set_pixel(0, 2, 10, 30, 90);
			set_pixel(2, 2, 10, 30, 90);
		}
	};
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		recorder);

	auto result = provider.FindKeyPointRange({
		3,
		1,
		0,
		90,
		30,
		10,
		0,
		2,
		0
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_EQ(2, result.left);
	EXPECT_EQ(4, result.right);
}

TEST(async_video_provider, find_key_point_range_refines_coarse_scan_boundaries) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 4;
	video->frame_height = 4;
	video->fill_frame = [](int n, VideoFrame& frame) {
		auto set_pixel = [&](int x, int y, unsigned char b, unsigned char g, unsigned char r) {
			size_t base = static_cast<size_t>(y) * frame.pitch + static_cast<size_t>(x) * 4;
			frame.data[base + 0] = b;
			frame.data[base + 1] = g;
			frame.data[base + 2] = r;
			frame.data[base + 3] = 255;
		};

		if (n >= 4 && n <= 11) {
			set_pixel(2, 1, 12, 64, 128);
			set_pixel(1, 1, 12, 64, 128);
			set_pixel(3, 1, 12, 64, 128);
		}
	};
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		recorder);

	auto result = provider.FindKeyPointRange({
		7,
		2,
		1,
		128,
		64,
		12,
		0,
		4,
		0
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_EQ(4, result.left);
	EXPECT_EQ(11, result.right);
	EXPECT_EQ((std::vector<int>{ 7, 3, 6, 5, 4, 11, 15, 12 }), state->requested_frames);
}

TEST(async_video_provider, find_key_point_range_refines_to_boundary_when_coarse_scan_has_no_probe) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 3;
	video->frame_height = 3;
	video->fill_frame = [](int n, VideoFrame& frame) {
		auto set_pixel = [&](int x, int y, unsigned char b, unsigned char g, unsigned char r) {
			size_t base = static_cast<size_t>(y) * frame.pitch + static_cast<size_t>(x) * 4;
			frame.data[base + 0] = b;
			frame.data[base + 1] = g;
			frame.data[base + 2] = r;
			frame.data[base + 3] = 255;
		};

		if (n <= 6) {
			set_pixel(1, 1, 16, 48, 96);
			set_pixel(0, 1, 16, 48, 96);
			set_pixel(2, 1, 16, 48, 96);
		}
	};
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		recorder);

	auto result = provider.FindKeyPointRange({
		3,
		1,
		1,
		96,
		48,
		16,
		0,
		8,
		0
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_EQ(0, result.left);
	EXPECT_EQ(6, result.right);
	EXPECT_EQ((std::vector<int>{ 3, 2, 1, 0, 11, 4, 5, 6, 7 }), state->requested_frames);
}

TEST(async_video_provider, find_key_point_range_refines_to_boundary_after_coarse_hit) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 3;
	video->frame_height = 3;
	video->fill_frame = [](int n, VideoFrame& frame) {
		auto set_pixel = [&](int x, int y, unsigned char b, unsigned char g, unsigned char r) {
			size_t base = static_cast<size_t>(y) * frame.pitch + static_cast<size_t>(x) * 4;
			frame.data[base + 0] = b;
			frame.data[base + 1] = g;
			frame.data[base + 2] = r;
			frame.data[base + 3] = 255;
		};

		if (n <= 12) {
			set_pixel(1, 1, 24, 72, 144);
			set_pixel(0, 1, 24, 72, 144);
			set_pixel(2, 1, 24, 72, 144);
		}
	};
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		recorder);

	auto result = provider.FindKeyPointRange({
		12,
		1,
		1,
		144,
		72,
		24,
		0,
		8,
		0
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_EQ(0, result.left);
	EXPECT_EQ(12, result.right);
	EXPECT_EQ((std::vector<int>{ 12, 4, 3, 2, 1, 0, 20, 13 }), state->requested_frames);
}

TEST(async_video_provider, find_key_point_range_frame_by_frame_scan_does_not_cross_short_mismatch_gap) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 3;
	video->frame_height = 3;
	video->fill_frame = [](int n, VideoFrame& frame) {
		auto set_pixel = [&](int x, int y, unsigned char b, unsigned char g, unsigned char r) {
			size_t base = static_cast<size_t>(y) * frame.pitch + static_cast<size_t>(x) * 4;
			frame.data[base + 0] = b;
			frame.data[base + 1] = g;
			frame.data[base + 2] = r;
			frame.data[base + 3] = 255;
		};

		if ((n >= 8 && n <= 10) || (n >= 17 && n <= 20)) {
			set_pixel(1, 1, 32, 96, 160);
			set_pixel(0, 1, 32, 96, 160);
			set_pixel(2, 1, 32, 96, 160);
		}
	};
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		recorder);

	auto result = provider.FindKeyPointRange({
		10,
		1,
		1,
		160,
		96,
		32,
		0,
		1,
		0
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_EQ(8, result.left);
	EXPECT_EQ(10, result.right);
	EXPECT_EQ((std::vector<int>{ 10, 9, 8, 7, 11 }), state->requested_frames);
}

TEST(async_video_provider, find_key_point_range_keeps_strict_range_when_fade_detection_is_disabled) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 24;
	video->frame_height = 24;
	video->fill_frame = [](int n, VideoFrame& frame) {
		FillSyntheticFadeFrame(n, frame);
	};
	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	auto result = provider.FindKeyPointRange({
		12, 10, 10, 100, 148, 196, 0, 1, 5, false, 0
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_EQ(7, result.left);
	EXPECT_EQ(17, result.right);
	EXPECT_EQ(result.left, result.strict_left);
	EXPECT_EQ(result.right, result.strict_right);
	EXPECT_FALSE(result.fade_in_detected);
	EXPECT_FALSE(result.fade_out_detected);
}

TEST(async_video_provider, find_key_point_range_detects_bounded_fades) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 24;
	video->frame_height = 24;
	video->fill_frame = [](int n, VideoFrame& frame) {
		FillSyntheticFadeFrame(n, frame);
	};
	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	auto result = provider.FindKeyPointRange({
		12, 10, 10, 100, 148, 196, 0, 1, 5, true, 12
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_EQ(7, result.strict_left);
	EXPECT_EQ(17, result.strict_right);
	EXPECT_TRUE(result.fade_in_detected);
	EXPECT_TRUE(result.fade_out_detected);
	EXPECT_LT(result.left, result.strict_left);
	EXPECT_GT(result.right, result.strict_right);
	EXPECT_GE(result.fade_in_end, result.strict_left - 1);
	EXPECT_LE(result.fade_in_end, result.strict_left + 1);
	EXPECT_GE(result.fade_out_start, result.strict_right - 1);
	EXPECT_LE(result.fade_out_start, result.strict_right + 1);
	EXPECT_GT(result.fade_in_confidence, 0.2);
	EXPECT_GT(result.fade_out_confidence, 0.2);
}

TEST(async_video_provider, find_key_point_range_finishes_fade_inside_tolerant_strict_range) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 24;
	video->frame_height = 24;
	video->fill_frame = [](int n, VideoFrame& frame) {
		FillSyntheticLongFadeFrame(n, frame);
	};
	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	auto result = provider.FindKeyPointRange({
		35, 10, 10, 100, 148, 196, 20, 1, 5, true, 40
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_EQ(14, result.strict_left);
	EXPECT_EQ(56, result.strict_right);
	EXPECT_TRUE(result.fade_in_detected);
	EXPECT_TRUE(result.fade_out_detected);
	EXPECT_EQ(6, result.left);
	EXPECT_EQ(25, result.fade_in_end);
	EXPECT_GT(result.fade_in_end, result.strict_left);
	EXPECT_EQ(45, result.fade_out_start);
	EXPECT_EQ(64, result.right);
	EXPECT_LT(result.fade_out_start, result.strict_right);
}

TEST(async_video_provider, find_key_point_range_reanchors_a_partially_faded_selection_on_full_visibility) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 24;
	video->frame_height = 24;
	video->fill_frame = [](int n, VideoFrame& frame) {
		FillSyntheticLongFadeFrame(n, frame);
	};
	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	// Frame 20 is only 75% visible. The selected color therefore must not be
	// treated as the fully-visible reference when locating the fade plateau.
	auto result = provider.FindKeyPointRange({
		20, 10, 10, 83, 117, 151, 20, 1, 5, true, 40
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_TRUE(result.fade_in_detected);
	EXPECT_TRUE(result.fade_out_detected);
	EXPECT_EQ(6, result.left);
	EXPECT_EQ(25, result.fade_in_end);
	EXPECT_EQ(45, result.fade_out_start);
	EXPECT_EQ(64, result.right);
}

TEST(async_video_provider, find_key_point_range_tracks_white_text_across_three_scenes) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 96;
	video->frame_height = 64;
	video->fill_frame = [](int n, VideoFrame& frame) {
		FillSyntheticWhiteTextSceneFrame(n, frame);
	};
	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	// The middle scene's pale background lies within the default color
	// tolerance, so same-color horizontal and vertical runs expand at the cut.
	for (int anchor : {22, 40, 58}) {
		SCOPED_TRACE(anchor);
		auto result = provider.FindKeyPointRange({.frame = anchor,
												  .x = 40,
												  .y = 24,
												  .r = 255,
												  .g = 255,
												  .b = 255,
												  .tolerance = 20,
												  .scan_step = 1,
												  .bounds_tolerance = 5,
												  .detect_fade = true,
												  .max_fade_frames = 80});

		ASSERT_EQ(KeyPointRangeScanStatus::Success, result.status);
		EXPECT_TRUE(result.fade_in_detected);
		EXPECT_TRUE(result.fade_out_detected);
		EXPECT_EQ(6, result.left);
		EXPECT_EQ(15, result.fade_in_end);
		EXPECT_EQ(65, result.fade_out_start);
		EXPECT_EQ(74, result.right);
	}
}

TEST(async_video_provider, find_key_point_range_does_not_follow_white_background_after_text_disappears) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 96;
	video->frame_height = 64;
	video->fill_frame = [](int n, VideoFrame& frame) {
		FillSyntheticWhiteTextSceneFrame(n, frame, true);
	};
	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	// Include a 70%-opaque click: calibration must recover the text plateau,
	// even though a later unrelated white block also covers the clicked pixel.
	for (int anchor : {12, 22, 40, 58}) {
		SCOPED_TRACE(anchor);
		unsigned char const color = anchor == 12 ? 188 : 255;
		auto result = provider.FindKeyPointRange({.frame = anchor,
												  .x = 40,
												  .y = 24,
												  .r = color,
												  .g = color,
												  .b = color,
												  .tolerance = 20,
												  .scan_step = 1,
												  .bounds_tolerance = 5,
												  .detect_fade = true,
												  .max_fade_frames = 80});

		ASSERT_EQ(KeyPointRangeScanStatus::Success, result.status);
		EXPECT_TRUE(result.fade_in_detected);
		EXPECT_TRUE(result.fade_out_detected);
		EXPECT_EQ(6, result.left);
		EXPECT_EQ(15, result.fade_in_end);
		EXPECT_EQ(65, result.fade_out_start);
		EXPECT_EQ(74, result.right);
	}
}

TEST(async_video_provider, find_key_point_range_keeps_low_visibility_ends_of_thirty_frame_fades) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 24;
	video->frame_height = 24;
	video->fill_frame = [](int n, VideoFrame& frame) {
		int const visibility = std::clamp(std::min(n - 5, 95 - n) * 100 / 30, 0, 100);
		FillSyntheticVisibilityFrame(n, frame, visibility, false);
	};
	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	// Samples below 8% visibility still belong to the 30-frame fade; they
	// must not terminate the scan before its transparent plateau is found.
	auto const result = provider.FindKeyPointRange({.frame = 50,
													.x = 10,
													.y = 10,
													.r = 100,
													.g = 148,
													.b = 196,
													.tolerance = 20,
													.scan_step = 1,
													.bounds_tolerance = 5,
													.detect_fade = true,
													.max_fade_frames = 48});

	ASSERT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_TRUE(result.fade_in_detected);
	EXPECT_TRUE(result.fade_out_detected);
	EXPECT_EQ(6, result.left);
	EXPECT_EQ(35, result.fade_in_end);
	EXPECT_EQ(65, result.fade_out_start);
	EXPECT_EQ(94, result.right);
}

TEST(async_video_provider, find_key_point_range_reuses_decoded_pixels_only_within_one_request) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 240;
	video->frame_height = 180;
	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	for (int request_index : {0, 1}) {
		SCOPED_TRACE(request_index);
		int const time_shift = request_index * 8;
		int const glyph_x = request_index == 0 ? 150 : 10;
		int const glyph_y = request_index == 0 ? 100 : 10;
		video->fill_frame = [=](int n, VideoFrame& frame) {
			int const visibility = std::clamp(std::min(n - time_shift - 5, 75 + time_shift - n) * 10, 0, 100);
			FillSyntheticWhiteGlyphFrame(frame, visibility, glyph_x, glyph_y);
		};
		{
			std::scoped_lock lock(state->mutex);
			state->requested_frames.clear();
		}
		auto const result = provider.FindKeyPointRange({.frame = 40,
														.x = glyph_x + 6,
														.y = glyph_y,
														.r = 255,
														.g = 255,
														.b = 255,
														.tolerance = 20,
														.scan_step = 1,
														.bounds_tolerance = 5,
														.detect_fade = true,
														.max_fade_frames = 80});

		ASSERT_EQ(KeyPointRangeScanStatus::Success, result.status);
		EXPECT_TRUE(result.fade_in_detected);
		EXPECT_TRUE(result.fade_out_detected);
		EXPECT_EQ(6 + time_shift, result.left);
		EXPECT_EQ(15 + time_shift, result.fade_in_end);
		EXPECT_EQ(65 + time_shift, result.fade_out_start);
		EXPECT_EQ(74 + time_shift, result.right);

		std::scoped_lock lock(state->mutex);
		auto requested_frames = state->requested_frames;
		std::ranges::sort(requested_frames);
		// Recalibrating the template changes scores, but each frame's pixels
		// need decoding once. A subsequent click starts with fresh pixels.
		EXPECT_EQ(1, std::ranges::count(requested_frames, 40));
		EXPECT_EQ(requested_frames.end(), std::ranges::adjacent_find(requested_frames));
	}
}

TEST(async_video_provider, find_key_point_range_keeps_cached_region_coordinates_at_frame_edges) {
	for (bool flipped : {false, true}) {
		for (auto const& origin : {std::pair{150, 100}, std::pair{1, 2}, std::pair{224, 161}}) {
			SCOPED_TRACE(::testing::Message() << "flipped=" << flipped << ", origin=" << origin.first << ',' << origin.second);
			auto state = std::make_shared<VideoProviderState>();
			auto *video = new FakeVideoProvider(state);
			video->frame_width = 240;
			video->frame_height = 180;
			video->bgra_flipped = flipped;
			video->fill_frame = [origin](int n, VideoFrame& frame) {
				int const visibility = std::clamp(std::min(n - 5, 75 - n) * 10, 0, 100);
				FillSyntheticWhiteGlyphFrame(frame, visibility, origin.first, origin.second);
			};
			AsyncVideoProvider provider(
				std::unique_ptr<VideoProvider>(video),
				std::unique_ptr<SubtitlesProvider>(),
				AsyncVideoProviderEventSink{});
			auto const result = provider.FindKeyPointRange({.frame = 40,
															.x = origin.first + 6,
															.y = origin.second,
															.r = 255,
															.g = 255,
															.b = 255,
															.tolerance = 20,
															.scan_step = 1,
															.bounds_tolerance = 5,
															.detect_fade = true,
															.max_fade_frames = 80});

			ASSERT_EQ(KeyPointRangeScanStatus::Success, result.status);
			EXPECT_TRUE(result.fade_in_detected);
			EXPECT_TRUE(result.fade_out_detected);
			EXPECT_EQ(6, result.left);
			EXPECT_EQ(15, result.fade_in_end);
			EXPECT_EQ(65, result.fade_out_start);
			EXPECT_EQ(74, result.right);
		}
	}
}

TEST(async_video_provider, find_key_point_range_retains_long_anchor_runs_outside_cached_region) {
	for (bool horizontal : {false, true}) {
		SCOPED_TRACE(horizontal);
		auto state = std::make_shared<VideoProviderState>();
		auto *video = new FakeVideoProvider(state);
		video->frame_width = 320;
		video->frame_height = 240;
		video->fill_frame = [horizontal](int n, VideoFrame& frame) {
			bool const selected_shape = n >= 10 && n <= 20;
			int const left = !selected_shape && horizontal ? 60 : 80;
			int const top = !selected_shape && !horizontal ? 50 : 70;
			for (int y = top; y < 180; ++y) {
				for (int x = left; x < 240; ++x)
					SetBgraPixel(frame, x, y, 255, 255, 255);
			}
		};
		AsyncVideoProvider provider(
			std::unique_ptr<VideoProvider>(video),
			std::unique_ptr<SubtitlesProvider>(),
			AsyncVideoProviderEventSink{});
		// The complete local sample region is white in every frame. Only the
		// distant end of the anchor's horizontal or vertical run changes.
		auto const result = provider.FindKeyPointRange({.frame = 15,
														.x = 160,
														.y = 120,
														.r = 255,
														.g = 255,
														.b = 255,
														.tolerance = 20,
														.scan_step = 1,
														.bounds_tolerance = 5,
														.detect_fade = true,
														.max_fade_frames = 40});

		ASSERT_EQ(KeyPointRangeScanStatus::Success, result.status);
		EXPECT_EQ(10, result.strict_left);
		EXPECT_EQ(20, result.strict_right);
		EXPECT_EQ(10, result.left);
		EXPECT_EQ(20, result.right);
		EXPECT_FALSE(result.fade_in_detected);
		EXPECT_FALSE(result.fade_out_detected);
	}
}

TEST(async_video_provider, find_key_point_range_calibrates_within_the_clicked_text_event) {
	for (bool brighter_event_first : {false, true}) {
		SCOPED_TRACE(brighter_event_first);
		auto state = std::make_shared<VideoProviderState>();
		auto *video = new FakeVideoProvider(state);
		video->frame_width = 96;
		video->frame_height = 64;
		video->fill_frame = [brighter_event_first](int n, VideoFrame& frame) {
			int const first_visibility = std::clamp(std::min(n - 5, 35 - n) * 10, 0, brighter_event_first ? 100 : 70);
			int const second_visibility = std::clamp(std::min(n - 50, 90 - n) * 10, 0, brighter_event_first ? 70 : 100);
			FillSyntheticWhiteGlyphFrame(frame, std::max(first_visibility, second_visibility), 34, 24);
		};
		AsyncVideoProvider provider(
			std::unique_ptr<VideoProvider>(video),
			std::unique_ptr<SubtitlesProvider>(),
			AsyncVideoProviderEventSink{});
		int const clicked_frame = brighter_event_first ? 55 : 10;
		// The click is at 50% opacity, its own event peaks at 70%, and the
		// identical glyph elsewhere reaches 100%. Frames 35..50 are empty.
		auto const result = provider.FindKeyPointRange({.frame = clicked_frame,
														.x = 40,
														.y = 24,
														.r = 143,
														.g = 143,
														.b = 143,
														.tolerance = 20,
														.scan_step = 1,
														.bounds_tolerance = 5,
														.detect_fade = true,
														.max_fade_frames = 80});

		ASSERT_EQ(KeyPointRangeScanStatus::Success, result.status);
		EXPECT_TRUE(result.fade_in_detected);
		EXPECT_TRUE(result.fade_out_detected);
		EXPECT_EQ(brighter_event_first ? 51 : 6, result.left);
		EXPECT_EQ(brighter_event_first ? 57 : 12, result.fade_in_end);
		EXPECT_EQ(brighter_event_first ? 83 : 28, result.fade_out_start);
		EXPECT_EQ(brighter_event_first ? 89 : 34, result.right);
	}
}

TEST(async_video_provider, find_key_point_range_bridges_brief_unobservable_text_without_a_false_fade) {
	for (int anchor : {22, 58}) {
		SCOPED_TRACE(anchor);
		for (int scan_step : {1, 2, 6}) {
			SCOPED_TRACE(scan_step);
			auto state = std::make_shared<VideoProviderState>();
			auto *video = new FakeVideoProvider(state);
			video->frame_width = 96;
			video->frame_height = 64;
			video->fill_frame = [](int n, VideoFrame& frame) {
				int const visibility = std::clamp(std::min(n - 5, 75 - n) * 10, 0, 100);
				int const background = n >= 30 && n <= 31 ? 255 : 32;
				FillSyntheticWhiteGlyphFrame(frame, visibility, 34, 24, background);
			};
			AsyncVideoProvider provider(
				std::unique_ptr<VideoProvider>(video),
				std::unique_ptr<SubtitlesProvider>(),
				AsyncVideoProviderEventSink{});
			// The glyph is opaque throughout the two white frames, but no local
			// contrast can establish its presence until the dark scene resumes.
			auto const result = provider.FindKeyPointRange({.frame = anchor,
															.x = 40,
															.y = 24,
															.r = 255,
															.g = 255,
															.b = 255,
															.tolerance = 20,
															.scan_step = scan_step,
															.bounds_tolerance = 5,
															.detect_fade = true,
															.max_fade_frames = 80});

			ASSERT_EQ(KeyPointRangeScanStatus::Success, result.status);
			EXPECT_TRUE(result.fade_in_detected);
			EXPECT_TRUE(result.fade_out_detected);
			EXPECT_EQ(6, result.left);
			EXPECT_EQ(15, result.fade_in_end);
			EXPECT_EQ(65, result.fade_out_start);
			EXPECT_EQ(74, result.right);
		}
	}
}

TEST(async_video_provider, find_key_point_range_stops_at_sustained_unobservable_text_without_a_false_fade) {
	for (int anchor : {22, 58}) {
		SCOPED_TRACE(anchor);
		for (int scan_step : {1, 2, 6}) {
			SCOPED_TRACE(scan_step);
			auto state = std::make_shared<VideoProviderState>();
			auto *video = new FakeVideoProvider(state);
			video->frame_width = 96;
			video->frame_height = 64;
			video->fill_frame = [](int n, VideoFrame& frame) {
				int const visibility = std::clamp(std::min(n - 5, 75 - n) * 10, 0, 100);
				int const background = n >= 30 && n <= 34 ? 255 : 32;
				FillSyntheticWhiteGlyphFrame(frame, visibility, 34, 24, background);
			};
			AsyncVideoProvider provider(
				std::unique_ptr<VideoProvider>(video),
				std::unique_ptr<SubtitlesProvider>(),
				AsyncVideoProviderEventSink{});
			// Five frames without measurable contrast exceed the short gap budget.
			// Stop at the last confirmed frame; unknown opacity is not a fade.
			auto const result = provider.FindKeyPointRange({.frame = anchor,
															.x = 40,
															.y = 24,
															.r = 255,
															.g = 255,
															.b = 255,
															.tolerance = 20,
															.scan_step = scan_step,
															.bounds_tolerance = 5,
															.detect_fade = true,
															.max_fade_frames = 80});

			ASSERT_EQ(KeyPointRangeScanStatus::Success, result.status);
			if (anchor < 30) {
				EXPECT_EQ(6, result.left);
				EXPECT_EQ(15, result.fade_in_end);
				EXPECT_TRUE(result.fade_in_detected);
				EXPECT_EQ(29, result.strict_right);
				EXPECT_EQ(29, result.right);
				EXPECT_FALSE(result.fade_out_detected);
			}
			else {
				EXPECT_EQ(35, result.strict_left);
				EXPECT_EQ(35, result.left);
				EXPECT_FALSE(result.fade_in_detected);
				EXPECT_EQ(65, result.fade_out_start);
				EXPECT_EQ(74, result.right);
				EXPECT_TRUE(result.fade_out_detected);
			}
		}
	}
}

TEST(async_video_provider, find_key_point_range_preserves_fade_boundaries_after_pixel_cache_eviction) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	// A very wide anchor row makes a complete event exceed the cache budget
	// while keeping the decoded frame and the glyph template inexpensive.
	video->frame_width = 160000;
	video->frame_height = 1;
	video->fill_frame = [](int n, VideoFrame& frame) {
		int const visibility = std::clamp(std::min(n - 5, 75 - n) * 10, 0, 100);
		auto const channel = static_cast<unsigned char>(255 * visibility / 100);
		for (int x = 8; x < 22; ++x)
			SetBgraPixel(frame, x, 0, channel, channel, channel);
	};
	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		AsyncVideoProviderEventSink{});
	auto const result = provider.FindKeyPointRange({.frame = 40,
													.x = 10,
													.y = 0,
													.r = 255,
													.g = 255,
													.b = 255,
													.tolerance = 20,
													.scan_step = 1,
													.bounds_tolerance = 5,
													.detect_fade = true,
													.max_fade_frames = 80});

	ASSERT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_TRUE(result.fade_in_detected);
	EXPECT_TRUE(result.fade_out_detected);
	EXPECT_EQ(6, result.left);
	EXPECT_EQ(15, result.fade_in_end);
	EXPECT_EQ(65, result.fade_out_start);
	EXPECT_EQ(74, result.right);

	std::scoped_lock lock(state->mutex);
	auto requested_frames = state->requested_frames;
	std::ranges::sort(requested_frames);
	// A repeated decode proves the result survived cache eviction rather
	// than merely exercising a request whose pixels all stayed resident.
	EXPECT_NE(requested_frames.end(), std::ranges::adjacent_find(requested_frames));
}

TEST(async_video_provider, find_key_point_range_does_not_extend_beyond_fade_budget) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 24;
	video->frame_height = 24;
	video->fill_frame = [](int n, VideoFrame& frame) {
		FillSyntheticFadeFrame(n, frame);
	};
	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	auto result = provider.FindKeyPointRange({
		12, 10, 10, 100, 148, 196, 0, 1, 5, true, 2
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_EQ(7, result.strict_left);
	EXPECT_EQ(17, result.strict_right);
	EXPECT_GE(result.left, result.strict_left - 2);
	EXPECT_LE(result.right, result.strict_right + 2);
}

TEST(async_video_provider, find_key_point_range_rejects_hard_cut_as_fade) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 24;
	video->frame_height = 24;
	video->fill_frame = [](int n, VideoFrame& frame) {
		FillSyntheticFadeFrame(n, frame, true);
	};
	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	auto result = provider.FindKeyPointRange({
		12, 10, 10, 100, 148, 196, 0, 1, 5, true, 12
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_EQ(7, result.left);
	EXPECT_EQ(17, result.right);
	EXPECT_FALSE(result.fade_in_detected);
	EXPECT_FALSE(result.fade_out_detected);
}

TEST(async_video_provider, find_key_point_range_tolerates_local_background_noise) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 24;
	video->frame_height = 24;
	video->fill_frame = [](int n, VideoFrame& frame) {
		FillSyntheticFadeFrame(n, frame, false, true);
	};
	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	auto result = provider.FindKeyPointRange({
		12, 10, 10, 100, 148, 196, 0, 1, 5, true, 12
	});

	EXPECT_EQ(KeyPointRangeScanStatus::Success, result.status);
	EXPECT_TRUE(result.fade_in_detected);
	EXPECT_TRUE(result.fade_out_detected);
	EXPECT_EQ(3, result.left);
	EXPECT_EQ(7, result.fade_in_end);
	EXPECT_EQ(17, result.fade_out_start);
	EXPECT_EQ(21, result.right);
}

TEST(async_video_provider, get_render_packet_exposes_source_frame_and_overlay) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	auto packet = provider.GetRenderPacket(9, 9000);
	ASSERT_TRUE(packet.source_frame_storage);
	EXPECT_FALSE(packet.composited_frame_storage);
	ASSERT_TRUE(packet.has_subtitle_overlay);
	EXPECT_TRUE(packet.source_frame.IsValid());
	EXPECT_TRUE(packet.subtitle_overlay.IsValid());
	EXPECT_EQ("BT.709", packet.source_frame.color.matrix);
	EXPECT_EQ("BT.709", packet.source_frame.color.primaries);
	EXPECT_EQ(SourceFrameColorRange::Full, packet.source_frame.color.range);
	EXPECT_EQ(packet.source_frame.width, packet.source_frame.geometry.storage_width);
	EXPECT_EQ(packet.source_frame.height, packet.source_frame.geometry.storage_height);
	EXPECT_EQ(0, packet.source_frame.geometry.visible_rect.x);
	EXPECT_EQ(0, packet.source_frame.geometry.visible_rect.y);
	EXPECT_EQ(packet.source_frame.width, packet.source_frame.geometry.visible_rect.width);
	EXPECT_EQ(packet.source_frame.height, packet.source_frame.geometry.visible_rect.height);
	EXPECT_TRUE(packet.subtitle_overlay.premultiplied_alpha);
	EXPECT_EQ(SubtitleOverlayCompositionMode::PremultipliedAlpha, packet.subtitle_overlay.composition_mode);
	EXPECT_EQ(SubtitleOverlayCoordinateSpace::SourceStorage, packet.subtitle_overlay.coordinate_space);
	EXPECT_GT(packet.subtitle_overlay.continuity_generation, 0u);
	EXPECT_EQ(packet.source_frame_storage, packet.DisplayFrame());
	EXPECT_EQ(9, packet.source_frame_storage->data[0]);
	EXPECT_EQ(0, packet.source_frame_storage->data[1]);
	EXPECT_EQ(0, packet.source_frame_storage->data[2]);
	EXPECT_EQ(128, packet.subtitle_overlay.planes[0].data[3]);
	ASSERT_EQ(1, packet.subtitle_overlay.dirty_rect_count);
	EXPECT_EQ(0, packet.subtitle_overlay.dirty_rects[0].x);
	EXPECT_EQ(0, packet.subtitle_overlay.dirty_rects[0].y);
	EXPECT_EQ(2, packet.subtitle_overlay.dirty_rects[0].width);
	EXPECT_EQ(2, packet.subtitle_overlay.dirty_rects[0].height);
}

TEST(async_video_provider, update_subtitles_advances_overlay_continuity_generation_for_direct_overlay) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto initial = MakeSubtitleFile("overlay-1");
	provider.LoadSubtitles(&initial);

	auto first = provider.GetRenderPacket(5, 5000);
	ASSERT_TRUE(first.has_subtitle_overlay);
	auto const first_generation = first.subtitle_overlay.continuity_generation;

	auto updated = MakeSubtitleFile("overlay-2");
	provider.UpdateSubtitles(&updated, &updated.Events.front());

	auto second = provider.GetRenderPacket(5, 5000);
	ASSERT_TRUE(second.has_subtitle_overlay);
	EXPECT_GT(second.subtitle_overlay.continuity_generation, first_generation);
}

TEST(async_video_provider, update_subtitles_reuses_latest_synchronous_render_frame) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeSubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto initial = MakeSubtitleFile("before");
	provider.LoadSubtitles(&initial);
	provider.RequestFrame(1, 1000);

	ASSERT_TRUE(recorder.WaitForCount(1));
	auto frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_EQ(1, frames.back().frame_number);

	auto stepped = provider.GetRenderPacket(9, 9000);
	ASSERT_TRUE(stepped.source_frame_storage);
	EXPECT_EQ(9, stepped.source_frame_storage->data[0]);

	auto updated = MakeSubtitleFile("after");
	provider.UpdateSubtitles(&updated, &updated.Events.front());

	ASSERT_TRUE(recorder.WaitForCount(2));
	frames = recorder.Snapshot();
	ASSERT_EQ(2u, frames.size());
	EXPECT_EQ(9, frames.back().frame_number);
	EXPECT_GT(frames.back().subtitle_generation, frames.front().subtitle_generation);

	std::lock_guard<std::mutex> lock(state->mutex);
	EXPECT_EQ((std::vector<int>{ 1, 9 }), state->requested_frames);
}

TEST(async_video_provider, overlay_renderer_exceptions_use_subtitle_error_sink_and_queue_recovers) {
	auto state = std::make_shared<VideoProviderState>();
	EventRecorder recorder;
	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		agi::make_unique<ThrowingOverlaySubtitlesProvider>(),
		recorder);

	auto subtitles = MakeSubtitleFile("failure path");
	provider.LoadSubtitles(&subtitles);
	provider.RequestFrame(1, 1000);
	ASSERT_TRUE(recorder.WaitForSubtitleErrorCount(1));

	provider.RequestFrame(2, 2000);
	ASSERT_TRUE(recorder.WaitForSubtitleErrorCount(2));
	auto errors = recorder.SubtitleErrors();
	ASSERT_EQ(2u, errors.size());
	EXPECT_NE(std::string::npos, errors[0].find("synthetic overlay render failure"));
	EXPECT_NE(std::string::npos, errors[1].find("synthetic overlay render failure"));
}

TEST(async_video_provider, update_subtitles_uses_external_current_frame_context) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeSubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto initial = MakeSubtitleFile("before");
	provider.LoadSubtitles(&initial);
	provider.RequestFrame(1, 1000);

	ASSERT_TRUE(recorder.WaitForCount(1));
	auto frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_EQ(1, frames.back().frame_number);

	provider.SetCurrentFrameContext(9, 9000);

	auto updated = MakeSubtitleFile("after");
	provider.UpdateSubtitles(&updated, &updated.Events.front());

	ASSERT_TRUE(recorder.WaitForCount(2));
	frames = recorder.Snapshot();
	ASSERT_EQ(2u, frames.size());
	EXPECT_EQ(9, frames.back().frame_number);
	EXPECT_GT(frames.back().subtitle_generation, frames.front().subtitle_generation);

	std::lock_guard<std::mutex> lock(state->mutex);
	EXPECT_EQ((std::vector<int>{ 1, 9 }), state->requested_frames);
}

TEST(async_video_provider, current_frame_context_does_not_render_without_content_change) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeSubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	provider.SetCurrentFrameContext(9, 9000);
	provider.GetRenderPacket(2, 2000);

	auto frames = recorder.Snapshot();
	EXPECT_TRUE(frames.empty());

	std::lock_guard<std::mutex> lock(state->mutex);
	EXPECT_EQ((std::vector<int>{ 2 }), state->requested_frames);
}

TEST(async_video_provider, cached_seek_context_replaces_cancelled_preview_requests) {
	auto state = std::make_shared<VideoProviderState>();
	EventRecorder recorder;
	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		agi::make_unique<FakeSubtitlesProvider>(), recorder);
	auto subtitles = MakeSubtitleFile("before");
	provider.LoadSubtitles(&subtitles);
	ScopedVideoProviderBlock block(state);
	provider.RequestFrame(7, 7000);
	ASSERT_TRUE(block.WaitUntilBlocked());
	provider.RequestFrame(8, 8000);

	// The controller displays a cached packet for frame 3 immediately.
	provider.CancelPendingFrameRequests();
	provider.SetCurrentFrameContext(3, 3000);
	block.Release();
	provider.CollectMemoryStats();
	EXPECT_TRUE(recorder.Snapshot().empty());

	subtitles.Events.front().Text = "after";
	provider.UpdateSubtitles(&subtitles, &subtitles.Events.front());
	ASSERT_TRUE(recorder.WaitForCount(1));
	auto const frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_EQ(3, frames.front().frame_number);
	EXPECT_EQ(3000, frames.front().time);
	std::scoped_lock lock(state->mutex);
	EXPECT_EQ((std::vector<int>{7, 3}), state->requested_frames);
}

TEST(async_video_provider, request_frame_overrides_pending_current_frame_context) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeSubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("sync");
	provider.LoadSubtitles(&subtitle_file);
	provider.SetCurrentFrameContext(9, 9000);
	provider.RequestFrame(2, 2000);

	ASSERT_TRUE(recorder.WaitForCount(1));
	auto frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_EQ(2, frames.back().frame_number);

	std::lock_guard<std::mutex> lock(state->mutex);
	EXPECT_EQ((std::vector<int>{ 2 }), state->requested_frames);
}

TEST(async_video_provider, update_subtitles_reuses_latest_async_render_frame) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeSubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto initial = MakeSubtitleFile("before");
	provider.LoadSubtitles(&initial);
	provider.RequestFrame(2, 2000);

	ASSERT_TRUE(recorder.WaitForCount(1));
	auto frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_EQ(2, frames.back().frame_number);

	auto updated = MakeSubtitleFile("after");
	provider.UpdateSubtitles(&updated, &updated.Events.front());

	ASSERT_TRUE(recorder.WaitForCount(2));
	frames = recorder.Snapshot();
	ASSERT_EQ(2u, frames.size());
	EXPECT_EQ(2, frames.back().frame_number);
	EXPECT_GT(frames.back().subtitle_generation, frames.front().subtitle_generation);

	std::lock_guard<std::mutex> lock(state->mutex);
	EXPECT_EQ((std::vector<int>{ 2 }), state->requested_frames);
}

TEST(async_video_provider, color_space_override_updates_effective_source_frame_metadata) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	provider.SetColorSpace("TV.601");
	auto packet = provider.GetRenderPacket(3, 3000);

	EXPECT_EQ("TV.601", packet.source_frame.color.matrix);
	EXPECT_EQ("BT.601", packet.source_frame.color.primaries);
	EXPECT_EQ(SourceFrameColorRange::Full, packet.source_frame.color.range);
}

TEST(async_video_provider, direct_overlay_color_space_override_preserves_overlay_continuity_generation) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	auto first = provider.GetRenderPacket(3, 3000);
	ASSERT_TRUE(first.has_subtitle_overlay);
	auto const first_generation = first.subtitle_overlay.continuity_generation;

	provider.SetColorSpace("TV.601");
	auto second = provider.GetRenderPacket(3, 3000);
	ASSERT_TRUE(second.has_subtitle_overlay);
	EXPECT_EQ(first_generation, second.subtitle_overlay.continuity_generation);
}

TEST(async_video_provider, bgra_source_frame_preserves_upstream_native_format_identity) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 42 };
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto packet = provider.GetRenderPacket(3, 3000);
	EXPECT_EQ(SourceFrameOutputMode::Bgra8, packet.source_frame.output_mode);
	EXPECT_EQ(SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, packet.source_frame.native_format.format_namespace);
	EXPECT_EQ(42, packet.source_frame.native_format.format_id);
	EXPECT_EQ(packet.source_frame.width, packet.source_frame.geometry.storage_width);
	EXPECT_EQ(packet.source_frame.height, packet.source_frame.geometry.storage_height);
}

TEST(async_video_provider, bgra_source_mode_propagates_provider_geometry_to_overlay_contract) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 5;
	video->frame_height = 6;
	video->bgra_geometry = MakeDefaultSourceFrameGeometry(5, 6);
	video->bgra_geometry.visible_rect = { 1, 2, 3, 2 };
	video->bgra_geometry.pixel_aspect_ratio = 1.25;
	auto *subs = new FakeGeometryAwareOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	auto packet = provider.GetRenderPacket(5, 5000);
	ASSERT_TRUE(packet.has_subtitle_overlay);
	EXPECT_EQ(SourceFrameOutputMode::Bgra8, packet.source_frame.output_mode);
	EXPECT_EQ(1, packet.source_frame.geometry.visible_rect.x);
	EXPECT_EQ(2, packet.source_frame.geometry.visible_rect.y);
	EXPECT_EQ(3, packet.source_frame.geometry.visible_rect.width);
	EXPECT_EQ(2, packet.source_frame.geometry.visible_rect.height);
	EXPECT_DOUBLE_EQ(1.25, packet.source_frame.geometry.pixel_aspect_ratio);
	EXPECT_EQ(1, subs->last_source_geometry.visible_rect.x);
	EXPECT_EQ(2, subs->last_source_geometry.visible_rect.y);
	EXPECT_EQ(3, subs->last_source_geometry.visible_rect.width);
	EXPECT_EQ(2, subs->last_source_geometry.visible_rect.height);
	EXPECT_DOUBLE_EQ(1.25, subs->last_source_geometry.pixel_aspect_ratio);
}

TEST(async_video_provider, native_source_mode_returns_native_source_frame_packet) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 99 };
	video->native_chroma_location = SourceFrameChromaLocation::TopCenter;
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		recorder);

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	auto packet = provider.GetRenderPacket(5, 5000, true);
	EXPECT_EQ(SourceFrameOutputMode::Native, packet.source_frame.output_mode);
	EXPECT_EQ(SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, packet.source_frame.native_format.format_namespace);
	EXPECT_EQ(99, packet.source_frame.native_format.format_id);
	EXPECT_EQ(SourceFrameChromaLocation::TopCenter, packet.source_frame.chroma_location);
	EXPECT_TRUE(packet.source_frame.IsValid());
	EXPECT_EQ(packet.source_frame.width, packet.source_frame.geometry.storage_width);
	EXPECT_EQ(packet.source_frame.height, packet.source_frame.geometry.storage_height);
	EXPECT_EQ(packet.source_frame.width, packet.source_frame.geometry.visible_rect.width);
	EXPECT_EQ(packet.source_frame.height, packet.source_frame.geometry.visible_rect.height);
	EXPECT_TRUE(static_cast<bool>(packet.source_frame_owner));
	EXPECT_FALSE(static_cast<bool>(packet.source_frame_storage));
	EXPECT_FALSE(static_cast<bool>(packet.composited_frame_storage));
	EXPECT_FALSE(packet.has_subtitle_overlay);
}

TEST(async_video_provider, native_source_mode_keeps_native_frame_for_source_only_rotation_path) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	video->native_geometry = MakeDefaultSourceFrameGeometry(2, 2);
	video->native_geometry.rotation = 90;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		recorder);

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	auto packet = provider.GetRenderPacket(5, 5000, true);
	EXPECT_EQ(SourceFrameOutputMode::Native, packet.source_frame.output_mode);
	EXPECT_EQ(SourceFramePixelFormat::Unknown, packet.source_frame.pixel_format);
	EXPECT_FALSE(static_cast<bool>(packet.source_frame_storage));
	EXPECT_TRUE(static_cast<bool>(packet.source_frame_owner));
	EXPECT_EQ(90, packet.source_frame.geometry.rotation);
	EXPECT_EQ(packet.source_frame.width, packet.source_frame.geometry.storage_width);
	EXPECT_EQ(packet.source_frame.height, packet.source_frame.geometry.storage_height);
}

TEST(async_video_provider, native_source_mode_keeps_native_frame_for_source_only_display_vflip_path) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	video->native_geometry = MakeDefaultSourceFrameGeometry(2, 2);
	video->native_geometry.display_vflip = true;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		recorder);

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	auto packet = provider.GetRenderPacket(5, 5000, true);
	EXPECT_EQ(SourceFrameOutputMode::Native, packet.source_frame.output_mode);
	EXPECT_EQ(SourceFramePixelFormat::Unknown, packet.source_frame.pixel_format);
	EXPECT_FALSE(static_cast<bool>(packet.source_frame_storage));
	EXPECT_TRUE(static_cast<bool>(packet.source_frame_owner));
	EXPECT_TRUE(packet.source_frame.geometry.display_vflip);
}

TEST(async_video_provider, native_source_mode_keeps_native_frame_for_rotated_subtitle_overlay_path) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	video->native_geometry = MakeDefaultSourceFrameGeometry(2, 2);
	video->native_geometry.rotation = 90;
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	auto packet = provider.GetRenderPacket(5, 5000);
	EXPECT_EQ(SourceFrameOutputMode::Native, packet.source_frame.output_mode);
	EXPECT_EQ(SourceFramePixelFormat::Unknown, packet.source_frame.pixel_format);
	EXPECT_FALSE(static_cast<bool>(packet.source_frame_storage));
	EXPECT_TRUE(static_cast<bool>(packet.source_frame_owner));
	EXPECT_TRUE(packet.has_subtitle_overlay);
	EXPECT_EQ(90, packet.source_frame.geometry.rotation);
}

TEST(async_video_provider, native_source_mode_keeps_native_frame_for_display_vflip_overlay_path) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	video->native_geometry = MakeDefaultSourceFrameGeometry(2, 2);
	video->native_geometry.display_vflip = true;
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	auto packet = provider.GetRenderPacket(5, 5000);
	EXPECT_EQ(SourceFrameOutputMode::Native, packet.source_frame.output_mode);
	EXPECT_EQ(SourceFramePixelFormat::Unknown, packet.source_frame.pixel_format);
	EXPECT_FALSE(static_cast<bool>(packet.source_frame_storage));
	EXPECT_TRUE(static_cast<bool>(packet.source_frame_owner));
	EXPECT_TRUE(packet.has_subtitle_overlay);
	EXPECT_TRUE(packet.source_frame.geometry.display_vflip);
}

TEST(async_video_provider, native_source_mode_keeps_native_frame_for_rotation_plus_display_vflip_overlay_path) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	video->native_geometry = MakeDefaultSourceFrameGeometry(2, 2);
	video->native_geometry.rotation = 90;
	video->native_geometry.display_vflip = true;
	video->native_geometry.pixel_aspect_ratio = 1.25;
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	auto packet = provider.GetRenderPacket(5, 5000);
	EXPECT_EQ(SourceFrameOutputMode::Native, packet.source_frame.output_mode);
	EXPECT_EQ(SourceFramePixelFormat::Unknown, packet.source_frame.pixel_format);
	EXPECT_FALSE(static_cast<bool>(packet.source_frame_storage));
	EXPECT_TRUE(static_cast<bool>(packet.source_frame_owner));
	EXPECT_EQ(90, packet.source_frame.geometry.rotation);
	EXPECT_TRUE(packet.source_frame.geometry.display_vflip);
	EXPECT_DOUBLE_EQ(1.25, packet.source_frame.geometry.pixel_aspect_ratio);
	EXPECT_TRUE(packet.has_subtitle_overlay);
}

TEST(async_video_provider, native_source_mode_propagates_non_full_visible_rect_to_overlay_contract) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 8;
	video->frame_height = 6;
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	video->native_geometry = MakeDefaultSourceFrameGeometry(8, 6);
	video->native_geometry.visible_rect = { 2, 1, 4, 3 };
	auto *subs = new FakeGeometryAwareOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	auto packet = provider.GetRenderPacket(5, 5000);
	ASSERT_TRUE(packet.has_subtitle_overlay);
	EXPECT_EQ(SourceFrameOutputMode::Native, packet.source_frame.output_mode);
	EXPECT_EQ(2, packet.source_frame.geometry.visible_rect.x);
	EXPECT_EQ(1, packet.source_frame.geometry.visible_rect.y);
	EXPECT_EQ(4, packet.source_frame.geometry.visible_rect.width);
	EXPECT_EQ(3, packet.source_frame.geometry.visible_rect.height);
	EXPECT_EQ(2, subs->last_source_geometry.visible_rect.x);
	EXPECT_EQ(1, subs->last_source_geometry.visible_rect.y);
	EXPECT_EQ(4, subs->last_source_geometry.visible_rect.width);
	EXPECT_EQ(3, subs->last_source_geometry.visible_rect.height);
	EXPECT_EQ(1, subs->last_overlay_target_x);
	EXPECT_EQ(2, subs->last_overlay_target_y);
	EXPECT_EQ(4, subs->last_overlay_width);
	EXPECT_EQ(2, subs->last_overlay_height);
	EXPECT_EQ(1, packet.subtitle_overlay.target_x);
	EXPECT_EQ(2, packet.subtitle_overlay.target_y);
	EXPECT_EQ(4, packet.subtitle_overlay.width);
	EXPECT_EQ(2, packet.subtitle_overlay.height);
	EXPECT_EQ(8, packet.subtitle_overlay.canvas_width);
	EXPECT_EQ(6, packet.subtitle_overlay.canvas_height);
	ASSERT_EQ(1, packet.subtitle_overlay.dirty_rect_count);
	EXPECT_EQ(1, packet.subtitle_overlay.dirty_rects[0].x);
	EXPECT_EQ(2, packet.subtitle_overlay.dirty_rects[0].y);
	EXPECT_EQ(4, packet.subtitle_overlay.dirty_rects[0].width);
	EXPECT_EQ(2, packet.subtitle_overlay.dirty_rects[0].height);

	auto adjusted = AdjustSubtitleOverlayForSourceGeometry(
		packet.subtitle_overlay,
		packet.source_frame.geometry);
	EXPECT_EQ(4, adjusted.canvas_width);
	EXPECT_EQ(3, adjusted.canvas_height);
	EXPECT_EQ(0, adjusted.target_x);
	EXPECT_EQ(1, adjusted.target_y);
	EXPECT_EQ(3, adjusted.width);
	EXPECT_EQ(2, adjusted.height);
	EXPECT_EQ(packet.subtitle_overlay.planes[0].data + 4, adjusted.planes[0].data);
	EXPECT_TRUE(adjusted.force_full_upload);
	EXPECT_EQ(nullptr, adjusted.dirty_rects);
	EXPECT_EQ(0, adjusted.dirty_rect_count);
}

TEST(async_video_provider, request_frame_event_preserves_non_full_visible_rect_metadata) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->frame_width = 8;
	video->frame_height = 6;
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	video->native_geometry = MakeDefaultSourceFrameGeometry(8, 6);
	video->native_geometry.visible_rect = { 2, 1, 4, 3 };
	auto *subs = new FakeGeometryAwareOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);
	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));

	provider.RequestFrame(5, 5000);
	ASSERT_TRUE(recorder.WaitForCount(1));
	auto frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_TRUE(frames.back().has_overlay);
	EXPECT_EQ(2, frames.back().source_visible_rect.x);
	EXPECT_EQ(1, frames.back().source_visible_rect.y);
	EXPECT_EQ(4, frames.back().source_visible_rect.width);
	EXPECT_EQ(3, frames.back().source_visible_rect.height);
}

TEST(async_video_provider, native_source_mode_keeps_native_frame_for_display_vflip_source_only_display_path) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	video->native_geometry = MakeDefaultSourceFrameGeometry(2, 2);
	video->native_geometry.display_vflip = true;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		recorder);

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	auto packet = provider.GetRenderPacket(5, 5000);
	EXPECT_EQ(SourceFrameOutputMode::Native, packet.source_frame.output_mode);
	EXPECT_EQ(SourceFramePixelFormat::Unknown, packet.source_frame.pixel_format);
	EXPECT_FALSE(static_cast<bool>(packet.source_frame_storage));
	EXPECT_TRUE(static_cast<bool>(packet.source_frame_owner));
	EXPECT_TRUE(packet.source_frame.geometry.display_vflip);
}

TEST(async_video_provider, preferred_source_modes_choose_native_for_overlay_path) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	auto *subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	EXPECT_EQ(SourceFrameOutputMode::Native, provider.GetSelectedSourceMode());
	EXPECT_EQ(SourceFrameOutputMode::Native, video->output_mode);
}

TEST(async_video_provider, bgra_renderer_preference_chooses_bgra8_without_subtitles) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(),
		recorder);

	EXPECT_FALSE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Bgra8 }));
	EXPECT_EQ(SourceFrameOutputMode::Bgra8, provider.GetSelectedSourceMode());
	EXPECT_EQ(SourceFrameOutputMode::Bgra8, video->output_mode);
}

TEST(async_video_provider, compatibility_subtitle_mode_forces_bgra8_output_mode) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	auto *subs = new FakeCompatibilityOnlySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	EXPECT_FALSE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	EXPECT_EQ(SourceFrameOutputMode::Bgra8, provider.GetSelectedSourceMode());
	EXPECT_EQ(SourceFrameOutputMode::Bgra8, video->output_mode);
}

TEST(async_video_provider, replacing_overlay_provider_with_compatibility_provider_reselects_bgra8) {
	auto state = std::make_shared<VideoProviderState>();
	auto *video = new FakeVideoProvider(state);
	video->available_modes = { SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 };
	auto *overlay_subs = new FakeOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		std::unique_ptr<VideoProvider>(video),
		std::unique_ptr<SubtitlesProvider>(overlay_subs),
		recorder);

	EXPECT_TRUE(provider.SetPreferredSourceModes({ SourceFrameOutputMode::Native, SourceFrameOutputMode::Bgra8 }));
	EXPECT_EQ(SourceFrameOutputMode::Native, provider.GetSelectedSourceMode());
	EXPECT_EQ(SourceFrameOutputMode::Native, video->output_mode);

	provider.ReplaceSubtitlesProvider(agi::make_unique<FakeCompatibilityOnlySubtitlesProvider>());

	EXPECT_EQ(SourceFrameOutputMode::Bgra8, provider.GetSelectedSourceMode());
	EXPECT_EQ(SourceFrameOutputMode::Bgra8, video->output_mode);
}

TEST(async_video_provider, premultiplied_overlay_provider_can_supply_dirty_rects) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeDirtyRectOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	auto first = provider.GetRenderPacket(9, 9000);
	auto second = provider.GetRenderPacket(9, 9000);

	ASSERT_TRUE(first.has_subtitle_overlay);
	ASSERT_TRUE(second.has_subtitle_overlay);
	EXPECT_TRUE(first.allow_source_frame_upload_reuse);
	ASSERT_EQ(1, first.subtitle_overlay.dirty_rect_count);
	EXPECT_EQ(1, first.subtitle_overlay.dirty_rects[0].x);
	EXPECT_EQ(0, first.subtitle_overlay.dirty_rects[0].y);
	EXPECT_EQ(1, first.subtitle_overlay.dirty_rects[0].width);
	EXPECT_EQ(1, first.subtitle_overlay.dirty_rects[0].height);
	EXPECT_EQ(0, second.subtitle_overlay.dirty_rect_count);
}

TEST(async_video_provider, invisible_premultiplied_overlay_is_not_forwarded_as_visible_overlay_packet) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeInvisibleOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	auto packet = provider.GetRenderPacket(9, 9000);
	EXPECT_FALSE(packet.has_subtitle_overlay);
}

TEST(async_video_provider, compatibility_only_backend_emits_baked_source_frame) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeCompatibilityOnlySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("csri");
	provider.LoadSubtitles(&subtitle_file);

	auto packet = provider.GetRenderPacket(5, 5000);
	ASSERT_TRUE(packet.source_frame_storage);
	EXPECT_FALSE(packet.composited_frame_storage);
	EXPECT_EQ(packet.source_frame_storage, packet.DisplayFrame());
	EXPECT_EQ(0, subs->render_overlay_calls);
	EXPECT_EQ(1, subs->draw_calls);
	EXPECT_EQ(5, packet.source_frame_storage->data[0]);
	EXPECT_EQ(11, packet.source_frame_storage->data[1]);
	EXPECT_FALSE(packet.allow_source_frame_upload_reuse);
	EXPECT_FALSE(packet.HasDistinctCompositedFrame());

	EXPECT_FALSE(packet.has_subtitle_overlay);
	EXPECT_FALSE(packet.subtitle_overlay_storage);
}

TEST(async_video_provider, compatibility_backend_keeps_using_baked_source_frames_when_content_is_stable) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeCompatibilityOnlySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("csri");
	provider.LoadSubtitles(&subtitle_file);

	auto first = provider.GetRenderPacket(5, 5000);
	auto second = provider.GetRenderPacket(5, 5000);

	EXPECT_FALSE(first.has_subtitle_overlay);
	EXPECT_FALSE(second.has_subtitle_overlay);
	EXPECT_FALSE(first.subtitle_overlay_storage);
	EXPECT_FALSE(second.subtitle_overlay_storage);
	EXPECT_FALSE(first.composited_frame_storage);
	EXPECT_FALSE(second.composited_frame_storage);
	ASSERT_TRUE(first.source_frame_storage);
	ASSERT_TRUE(second.source_frame_storage);
	EXPECT_EQ(first.source_frame_storage->data, second.source_frame_storage->data);
}

TEST(async_video_provider, compatibility_backend_uses_only_baked_source_frames) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeCompatibilityOnlySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("csri");
	provider.LoadSubtitles(&subtitle_file);

	auto first = provider.GetRenderPacket(5, 5000);
	auto second = provider.GetRenderPacket(5, 5000);
	EXPECT_FALSE(first.has_subtitle_overlay);
	EXPECT_FALSE(second.has_subtitle_overlay);
	EXPECT_FALSE(static_cast<bool>(first.subtitle_overlay_storage));
	EXPECT_FALSE(static_cast<bool>(second.subtitle_overlay_storage));
	EXPECT_FALSE(static_cast<bool>(first.composited_frame_storage));
	EXPECT_FALSE(static_cast<bool>(second.composited_frame_storage));
	ASSERT_TRUE(first.source_frame_storage);
	ASSERT_TRUE(second.source_frame_storage);
	EXPECT_EQ(first.source_frame_storage->data[1], second.source_frame_storage->data[1]);
}

TEST(async_video_provider, dropped_compatibility_render_does_not_mark_visible_lines_current) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeCompatibilityOnlySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto initial = MakeSubtitleFile("before");
	provider.LoadSubtitles(&initial);
	provider.RequestFrame(4, 4000);
	ASSERT_TRUE(recorder.WaitForCount(1));

	{
		std::lock_guard<std::mutex> lock(subs->mutex);
		subs->block_next_draw = true;
	}

	auto updated = MakeSubtitleFile("after");
	provider.UpdateSubtitles(&updated, &updated.Events.front());
	bool const draw_entered = subs->WaitForDrawEntered();
	if (!draw_entered)
		subs->ReleaseDraw();
	ASSERT_TRUE(draw_entered);

	provider.UpdateSubtitles(&updated, &updated.Events.front());
	subs->ReleaseDraw();

	ASSERT_TRUE(recorder.WaitForCount(2));
	auto frames = recorder.Snapshot();
	ASSERT_EQ(2u, frames.size());
	EXPECT_EQ(4, frames.back().frame_number);
	EXPECT_GT(frames.back().subtitle_generation, frames.front().subtitle_generation);
	EXPECT_EQ(3, subs->draw_calls);
}

TEST(async_video_provider, visual_subtitle_final_update_keeps_incremental_packet_metadata) {
	auto state = std::make_shared<VideoProviderState>();
	EventRecorder recorder;
	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		agi::make_unique<FakeSubtitlesProvider>(),
		recorder);

	auto subtitles = MakeSubtitleFile("before");
	provider.LoadSubtitles(&subtitles);
	provider.RequestFrame(0, 0);
	ASSERT_TRUE(recorder.WaitForCount(1));

	auto snapshot = std::make_shared<FakeVisualToolRenderSnapshot>();
	provider.UpdateSubtitles(&subtitles, &subtitles.Events.front(), {.delivery_class = VideoRenderDeliveryClass::VisualSubtitleFinal, .visual_interaction_id = 42, .force_current_frame_render = true, .visual_tool_snapshot = snapshot});
	ASSERT_TRUE(recorder.WaitForCount(2));

	auto frames = recorder.Snapshot();
	ASSERT_EQ(2u, frames.size());
	EXPECT_EQ(VideoRenderDeliveryClass::EveryFrame, frames.front().delivery_class);
	EXPECT_EQ(VideoRenderDeliveryClass::VisualSubtitleFinal, frames.back().delivery_class);
	EXPECT_EQ(42u, frames.back().visual_interaction_id);
	EXPECT_EQ(snapshot, frames.back().visual_tool_snapshot);
	EXPECT_FALSE(frames.front().visual_tool_snapshot);
}

TEST(async_video_provider, dropped_packet_advances_overlay_continuity_generation_on_next_delivered_event) {
	auto state = std::make_shared<VideoProviderState>();

	auto *subs = new FakeDropSensitiveOverlaySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("overlay");
	provider.LoadSubtitles(&subtitle_file);

	auto baseline = provider.GetRenderPacket(0, 0);
	ASSERT_TRUE(baseline.has_subtitle_overlay);
	auto const baseline_generation = baseline.subtitle_overlay.continuity_generation;
	state->block_next = true;

	provider.RequestFrame(1, 1000);
	{
		std::unique_lock<std::mutex> lock(state->mutex);
		ASSERT_TRUE(state->cv.wait_for(lock, std::chrono::seconds(2), [&] { return state->entered; }));
	}

	provider.RequestFrame(2, 2000);

	{
		std::lock_guard<std::mutex> lock(state->mutex);
		state->released = true;
	}
	state->cv.notify_all();

	ASSERT_TRUE(recorder.WaitForCount(1));
	auto frames = recorder.Snapshot();
	ASSERT_EQ(1u, frames.size());
	EXPECT_EQ(2, frames.back().frame_number);
	EXPECT_TRUE(frames.back().has_overlay);
	EXPECT_GT(frames.back().overlay_continuity_generation, baseline_generation);
}

TEST(async_video_provider, replacing_subtitles_provider_reuses_video_provider_and_refreshes_overlay_mode) {
	auto state = std::make_shared<VideoProviderState>();
	auto *compat_subs = new FakeCompatibilityOnlySubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(compat_subs),
		recorder);

	auto subtitle_file = MakeSubtitleFile("swap");
	provider.LoadSubtitles(&subtitle_file);

	auto first = provider.GetRenderPacket(5, 5000);
	ASSERT_TRUE(first.source_frame_storage);
	EXPECT_EQ(5, first.source_frame_storage->data[0]);
	EXPECT_FALSE(first.has_subtitle_overlay);
	EXPECT_FALSE(first.allow_source_frame_upload_reuse);

	auto *overlay_subs = new FakeOverlaySubtitlesProvider;
	provider.ReplaceSubtitlesProvider(std::unique_ptr<SubtitlesProvider>(overlay_subs));
	provider.LoadSubtitles(&subtitle_file);

	auto second = provider.GetRenderPacket(5, 5000);
	ASSERT_TRUE(second.source_frame_storage);
	EXPECT_EQ(5, second.source_frame_storage->data[0]);
	ASSERT_TRUE(second.has_subtitle_overlay);
	EXPECT_TRUE(second.subtitle_overlay.premultiplied_alpha);
	EXPECT_EQ(SubtitleOverlayCompositionMode::PremultipliedAlpha, second.subtitle_overlay.composition_mode);
	EXPECT_EQ(128, second.subtitle_overlay.planes[0].data[3]);
}

TEST(async_video_provider, provider_activation_runs_on_initial_create_and_replace) {
	auto state = std::make_shared<VideoProviderState>();
	auto *first_subs = new FakeActivationAwareSubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(first_subs),
		recorder);

	EXPECT_EQ(1, first_subs->activation_calls);

	auto *second_subs = new FakeActivationAwareSubtitlesProvider;
	provider.ReplaceSubtitlesProvider(std::unique_ptr<SubtitlesProvider>(second_subs));
	EXPECT_EQ(1, second_subs->activation_calls);
}

TEST(async_video_provider, replacement_activates_new_provider_before_old_is_destroyed) {
	auto state = std::make_shared<VideoProviderState>();
	int destruction_count = 0;
	auto *first_subs = new FakeActivationOrderSubtitlesProvider(&destruction_count);
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(first_subs),
		recorder);

	EXPECT_EQ(0, first_subs->destroyed_before_activation);
	EXPECT_EQ(0, destruction_count);

	auto *second_subs = new FakeActivationOrderSubtitlesProvider(&destruction_count);
	provider.ReplaceSubtitlesProvider(std::unique_ptr<SubtitlesProvider>(second_subs));

	EXPECT_EQ(0, second_subs->destroyed_before_activation);
	EXPECT_EQ(1, destruction_count);
}

TEST(async_video_provider, filename_constructor_forwards_transient_fonts_to_factory) {
	ScopedFactoryOverride scope;
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeSubtitlesProvider;

	g_video_provider_factory = [state] {
		return agi::make_unique<FakeVideoProvider>(state);
	};
	g_subtitles_provider_factory = [subs](SubtitleRenderEnvironment const&) {
		return std::unique_ptr<SubtitlesProvider>(subs);
	};

	auto fonts = std::make_shared<TransientFontSet>();
	fonts->generation = 42;
	fonts->fonts.push_back({ "embedded.ttf", "font/ttf", { 'f', 'o', 'n', 't' } });

	AsyncVideoProvider provider(agi::fs::path("dummy.mkv"), "", AsyncVideoProviderEventSink{}, nullptr, fonts);

	ASSERT_TRUE(g_last_factory_transient_fonts);
	EXPECT_EQ(fonts, g_last_factory_transient_fonts);
	EXPECT_EQ(nullptr, g_last_factory_background_runner);
	EXPECT_EQ(42u, g_last_factory_transient_fonts->generation);
	ASSERT_EQ(1u, g_last_factory_transient_fonts->fonts.size());
	EXPECT_EQ("embedded.ttf", g_last_factory_transient_fonts->fonts.front().original_name);

	auto subtitle_file = MakeSubtitleFile("embedded");
	provider.LoadSubtitles(&subtitle_file);
	auto frame = provider.GetFrame(3, 3000);
	ASSERT_TRUE(frame);
	ASSERT_GE(frame->data.size(), 2u);
	EXPECT_EQ(3, frame->data[0]);
	EXPECT_EQ(1, frame->data[1]);
}

TEST(async_video_provider, filename_constructor_forwards_choice_sink_to_video_factory) {
	ScopedFactoryOverride scope;
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeSubtitlesProvider;

	g_video_provider_factory = [state] {
		return agi::make_unique<FakeVideoProvider>(state);
	};
	g_subtitles_provider_factory = [subs](SubtitleRenderEnvironment const&) {
		return std::unique_ptr<SubtitlesProvider>(subs);
	};

	auto choice_sink = std::make_shared<agi::NullSingleChoiceInteractionSink>();
	AsyncVideoProvider provider(agi::fs::path("dummy.mkv"), "", AsyncVideoProviderEventSink{}, nullptr, {}, choice_sink);

	ASSERT_TRUE(g_last_factory_choice_sink);
	EXPECT_EQ(choice_sink, g_last_factory_choice_sink);
}

TEST(async_video_provider, filename_constructor_does_not_create_default_choice_sink) {
	ScopedFactoryOverride scope;
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeSubtitlesProvider;

	g_video_provider_factory = [state] {
		return agi::make_unique<FakeVideoProvider>(state);
	};
	g_subtitles_provider_factory = [subs](SubtitleRenderEnvironment const&) {
		return std::unique_ptr<SubtitlesProvider>(subs);
	};

	AsyncVideoProvider provider(agi::fs::path("dummy.mkv"), "", AsyncVideoProviderEventSink{}, nullptr);

	EXPECT_EQ(nullptr, g_last_factory_choice_sink);
}

TEST(async_video_provider, subtitle_timecodes_override_source_fps_for_visibility_updates) {
	auto state = std::make_shared<VideoProviderState>();
	auto *subs = new FakeSubtitlesProvider;
	EventRecorder recorder;

	AsyncVideoProvider provider(
		agi::make_unique<FakeVideoProvider>(state),
		std::unique_ptr<SubtitlesProvider>(subs),
		recorder);

	AssFile subtitle_file;
	auto *line = new AssDialogue;
	line->Row = 0;
	line->Start = 0;
	line->End = 17;
	line->Text = "timecodes";
	subtitle_file.Events.push_back(*line);
	provider.LoadSubtitles(&subtitle_file);

	provider.SetSubtitlesTimecodes(agi::vfr::Framerate(100.0));
	provider.RequestFrame(0, 15);
	ASSERT_TRUE(recorder.WaitForCount(1));

	subtitle_file.Events.front().Text = "updated";
	provider.UpdateSubtitles(&subtitle_file, &subtitle_file.Events.front());
	ASSERT_TRUE(recorder.WaitForCount(2));

	auto frames = recorder.Snapshot();
	ASSERT_EQ(2u, frames.size());
	EXPECT_EQ(1, frames[0].subtitle_generation);
	EXPECT_EQ(2, frames[1].subtitle_generation);
}

// --- raw video batch API (motion track) ---

namespace {
auto MakeRawTestProvider() {
	return agi::make_unique<AsyncVideoProvider>(
		agi::make_unique<FakeVideoProvider>(std::make_shared<VideoProviderState>()),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});
}
}

TEST(async_video_provider_raw_batch, fetches_frames_and_reports_completed) {
	auto provider = MakeRawTestProvider();
	auto identity = provider->GetRawVideoIdentity();
	EXPECT_EQ(2, identity.width);
	EXPECT_EQ(100, identity.frame_count);
	EXPECT_NE(0u, identity.generation);

	std::vector<int> seen;
	RawVideoBatchResult result = provider->RunRawVideoBatch(
		identity,
		[&](RawFrameAccess& access) {
			for (int f : {3, 5, 9}) {
				aegisub::motion_track::RawBgraView view;
				auto read = access.FetchBgra(f, view);
				if (read.status != aegisub::motion_track::FrameReadStatus::Ok)
					return RawVideoBatchStatus::DecodeError;
				if (view.width != 2 || view.height != 2 || view.pitch < 8)
					return RawVideoBatchStatus::Error;
				seen.push_back(f);
			}
			return RawVideoBatchStatus::Completed;
		});
	EXPECT_EQ(RawVideoBatchStatus::Completed, result.status);
	ASSERT_EQ(3u, seen.size());
	EXPECT_EQ(3, seen[0]);
	EXPECT_EQ(9, seen[2]);
}

TEST(async_video_provider_raw_batch, stale_identity_is_rejected) {
	auto provider = MakeRawTestProvider();

	auto stale_generation = provider->GetRawVideoIdentity();
	stale_generation.generation += 1;
	auto result = provider->RunRawVideoBatch(
		stale_generation, [&](RawFrameAccess&) {
			return RawVideoBatchStatus::Completed;
		});
	EXPECT_EQ(RawVideoBatchStatus::ProviderChanged, result.status);

	auto stale_width = provider->GetRawVideoIdentity();
	stale_width.width += 1;
	result = provider->RunRawVideoBatch(
		stale_width, [&](RawFrameAccess&) {
			return RawVideoBatchStatus::Completed;
		});
	EXPECT_EQ(RawVideoBatchStatus::ProviderChanged, result.status);
}

TEST(async_video_provider_raw_batch, out_of_range_frame_maps_to_frame_unavailable) {
	auto provider = MakeRawTestProvider();
	RawVideoBatchResult result = provider->RunRawVideoBatch(
		provider->GetRawVideoIdentity(),
		[&](RawFrameAccess& access) {
			aegisub::motion_track::RawBgraView view;
			auto read = access.FetchBgra(-1, view);
			if (read.status != aegisub::motion_track::FrameReadStatus::FrameUnavailable)
				return RawVideoBatchStatus::Error;
			read = access.FetchBgra(100, view);
			if (read.status != aegisub::motion_track::FrameReadStatus::FrameUnavailable)
				return RawVideoBatchStatus::Error;
			return RawVideoBatchStatus::Completed;
		});
	EXPECT_EQ(RawVideoBatchStatus::Completed, result.status);
}

TEST(async_video_provider_raw_batch, decode_error_maps_to_decode_error) {
	auto fake = agi::make_unique<FakeVideoProvider>(std::make_shared<VideoProviderState>());
	fake->fill_frame = [](int, VideoFrame&) {
		throw VideoDecodeError("boom");
	};
	auto provider = agi::make_unique<AsyncVideoProvider>(
		std::move(fake),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	RawVideoBatchResult result = provider->RunRawVideoBatch(
		provider->GetRawVideoIdentity(),
		[&](RawFrameAccess& access) {
			aegisub::motion_track::RawBgraView view;
			auto read = access.FetchBgra(0, view);
			if (read.status != aegisub::motion_track::FrameReadStatus::DecodeError)
				return RawVideoBatchStatus::Error;
			if (read.message.find("boom") == std::string::npos)
				return RawVideoBatchStatus::Error;
			return RawVideoBatchStatus::Completed;
		});
	EXPECT_EQ(RawVideoBatchStatus::Completed, result.status);
}

TEST(async_video_provider_raw_batch, non_agi_exception_maps_to_error) {
	struct Mystery {};
	auto fake = agi::make_unique<FakeVideoProvider>(std::make_shared<VideoProviderState>());
	fake->fill_frame = [](int, VideoFrame&) { throw Mystery{}; };
	auto provider = agi::make_unique<AsyncVideoProvider>(
		std::move(fake),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	RawVideoBatchResult result = provider->RunRawVideoBatch(
		provider->GetRawVideoIdentity(),
		[&](RawFrameAccess& access) {
			aegisub::motion_track::RawBgraView view;
			auto read = access.FetchBgra(0, view);
			if (read.status != aegisub::motion_track::FrameReadStatus::Error)
				return RawVideoBatchStatus::Error;
			return RawVideoBatchStatus::Completed;
		});
	// The non-agi exception was mapped inside FetchBgra; the batch itself
	// completes so the caller can decide what to do with the failed frame.
	EXPECT_EQ(RawVideoBatchStatus::Completed, result.status);
}

TEST(async_video_provider_raw_batch, invalid_frame_buffer_maps_to_frame_unavailable) {
	auto fake = agi::make_unique<FakeVideoProvider>(std::make_shared<VideoProviderState>());
	fake->fill_frame = [](int, VideoFrame& frame) {
		frame.data.clear();
		frame.pitch = 0;
	};
	auto provider = agi::make_unique<AsyncVideoProvider>(
		std::move(fake),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	RawVideoBatchResult result = provider->RunRawVideoBatch(
		provider->GetRawVideoIdentity(),
		[&](RawFrameAccess& access) {
			aegisub::motion_track::RawBgraView view;
			auto read = access.FetchBgra(0, view);
			if (read.status != aegisub::motion_track::FrameReadStatus::FrameUnavailable)
				return RawVideoBatchStatus::Error;
			return RawVideoBatchStatus::Completed;
		});
	EXPECT_EQ(RawVideoBatchStatus::Completed, result.status);
}

TEST(async_video_provider_raw_batch, wrapping_pitch_product_maps_to_frame_unavailable) {
	// pitch * height wraps in size_t (2^63 * 2 == 2^64 == 0), so a naive
	// "data.size() < pitch * height" check passed this 16-byte buffer through;
	// the reader would then have indexed rows through a garbage int pitch.
	auto fake = agi::make_unique<FakeVideoProvider>(std::make_shared<VideoProviderState>());
	fake->fill_frame = [](int, VideoFrame& frame) {
		frame.pitch = (size_t(1) << 63);
	};
	auto provider = agi::make_unique<AsyncVideoProvider>(
		std::move(fake),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	RawVideoBatchResult result = provider->RunRawVideoBatch(
		provider->GetRawVideoIdentity(),
		[&](RawFrameAccess& access) {
			aegisub::motion_track::RawBgraView view;
			auto read = access.FetchBgra(0, view);
			if (read.status != aegisub::motion_track::FrameReadStatus::FrameUnavailable)
				return RawVideoBatchStatus::Error;
			return RawVideoBatchStatus::Completed;
		});
	EXPECT_EQ(RawVideoBatchStatus::Completed, result.status);
}

TEST(async_video_provider_raw_batch, wrapping_width_with_zero_pitch_maps_to_frame_unavailable) {
	// width * 4 wraps to 0 for width == 2^62, so a zero pitch used to slip
	// past the chained ordering check and the buffer-size division divided by
	// zero (UB). The geometry check now runs before any size arithmetic and
	// rejects the frame instead.
	auto fake = agi::make_unique<FakeVideoProvider>(std::make_shared<VideoProviderState>());
	fake->fill_frame = [](int, VideoFrame& frame) {
		frame.width = static_cast<size_t>(1) << 62;
		frame.height = 1;
		frame.pitch = 0;
		frame.data.assign(1, 0);
	};
	auto provider = agi::make_unique<AsyncVideoProvider>(
		std::move(fake),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	RawVideoBatchResult result = provider->RunRawVideoBatch(
		provider->GetRawVideoIdentity(),
		[&](RawFrameAccess& access) {
			aegisub::motion_track::RawBgraView view;
			auto read = access.FetchBgra(0, view);
			if (read.status != aegisub::motion_track::FrameReadStatus::FrameUnavailable)
				return RawVideoBatchStatus::Error;
			return RawVideoBatchStatus::Completed;
		});
	EXPECT_EQ(RawVideoBatchStatus::Completed, result.status);
}

TEST(async_video_provider_raw_batch, zero_pitch_with_matching_geometry_maps_to_frame_unavailable) {
	// A zero pitch on an otherwise well-formed frame must hit the explicit
	// pitch rejection rather than the data.size() / pitch division.
	auto fake = agi::make_unique<FakeVideoProvider>(std::make_shared<VideoProviderState>());
	fake->fill_frame = [](int, VideoFrame& frame) {
		frame.pitch = 0;
		frame.data.assign(4, 0);
	};
	auto provider = agi::make_unique<AsyncVideoProvider>(
		std::move(fake),
		agi::make_unique<FakeSubtitlesProvider>(),
		AsyncVideoProviderEventSink{});

	RawVideoBatchResult result = provider->RunRawVideoBatch(
		provider->GetRawVideoIdentity(),
		[&](RawFrameAccess& access) {
			aegisub::motion_track::RawBgraView view;
			auto read = access.FetchBgra(0, view);
			if (read.status != aegisub::motion_track::FrameReadStatus::FrameUnavailable)
				return RawVideoBatchStatus::Error;
			return RawVideoBatchStatus::Completed;
		});
	EXPECT_EQ(RawVideoBatchStatus::Completed, result.status);
}

TEST(async_video_provider_raw_batch, nested_sync_inside_callback_returns_null_without_deadlock) {
	auto provider = MakeRawTestProvider();
	RawVideoBatchResult result = provider->RunRawVideoBatch(
		provider->GetRawVideoIdentity(),
		[&](RawFrameAccess&) {
			// A nested synchronous worker entry must be rejected instead of
			// self-deadlocking on the queue.
			auto nested = provider->GetFrameBgra(1, 0.0);
			EXPECT_EQ(nullptr, nested);
			return RawVideoBatchStatus::Completed;
		});
	EXPECT_EQ(RawVideoBatchStatus::Completed, result.status);

	// The provider still serves normal requests afterwards.
	EXPECT_NE(nullptr, provider->GetFrameBgra(2, 0.0));
}
