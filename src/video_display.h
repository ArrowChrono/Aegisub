// Copyright (c) 2005-2010, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file video_display.h
/// @see video_display.cpp
/// @ingroup video main_ui
///

#include <libaegisub/signal.h>

#include "ivideo_renderer.h"
#include "video_display_layout.h"
#include "video_memory_stats.h"
#include "video_render_packet.h"
#include "video_subtitle_scene_cache.h"
#include "video_tool_release_feedback.h"
#include "visual_tool_presentation.h"

#include "vector2d.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>
#include <wx/cursor.h>
#include <wx/glcanvas.h>

#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
#include <include/core/SkRefCnt.h>  // for sk_sp<SkSurface> member
#endif

// Prototypes
class AssDialogue;
class RetinaHelper;
class AsyncVideoProvider;
class VideoColorZoomPreview;
class OpenGLText;
class OpenGLTextTextureDeleter;
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
class SkiaSurfaceProvider;
class SkiaTextLayoutCache;
class SkiaVideoCompositor;
class SkiaVideoOverlayCommandBuffer;
class SkSurface;
struct SkiaGlContextToken;
struct SkiaVideoFrameTarget;
enum class SkiaVideoFailureInjection;
#endif
class VideoController;
class VideoOverlayDrawContext;
struct VisualGuideViewport;
class VisualToolBase;
class UiDeadlineTimer;
enum class VisualScaleAxis;
enum class VisualNudgeMagnitude;
class wxComboBox;
class wxTextCtrl;
class wxToolBar;
class wxImage;

namespace agi {
	struct Context;
	class OptionValue;
}

class VideoDisplay final : public wxGLCanvas {
	/// Signals the display is connected to
	std::vector<agi::signal::Connection> connections;
	agi::signal::Signal<int> FramePresented;
	/// Signals when the untransformed video viewport changes (for presenters).
	agi::signal::Signal<> BaseViewportChanged;

	const agi::OptionValue* autohideTools;
	const agi::OptionValue* scrollAction;
	const agi::OptionValue* ctrlScrollAction;
	const agi::OptionValue* shiftScrollAction;

	agi::Context *con;

	std::unique_ptr<wxMenu> context_menu;

	/// The size of the video in screen at the current zoom level, which may not
	/// be the same as the actual client size of the display
	wxSize videoSize;

	Vector2D last_mouse_pos, mouse_pos;

	struct PointSelectionSession {
		std::string owner;
		int point_count = 0;
		bool script_coordinates = true;
		std::vector<std::pair<double, double>> points;
		std::function<void(
			std::vector<std::pair<double, double>>, int, bool)> completed;
		/// Cursor shown over the canvas for the duration of the session, so the
		/// mode is visible rather than inferred from a status line that times out.
		wxCursor cursor;
		/// Localized "what to do now, how to get out" hint, re-shown whenever the
		/// pointer re-enters the canvas.
		wxString hint;
		/// Show the colour-pick magnifier following the pointer for the
		/// duration of the session. Click handling is unchanged; the magnifier
		/// is a passive preview of the frame a click would sample.
		bool live_zoom = false;
	};
	std::optional<PointSelectionSession> point_selection;
	/// Magnifier for an armed colour-pick session (point_selection->live_zoom);
	/// created by BeginPointSelection, dropped by FinishPointSelection so every
	/// exit path closes it.
	std::unique_ptr<VideoColorZoomPreview> zoom_preview;

	/// Base viewport before attached-mode content pan/zoom is applied
	VideoDisplayViewportLayout baseViewport;
	wxSize last_size_event_client_size = wxDefaultSize;

	/// Screen pixels between the left of the canvas and the left of the video
	int viewport_left = 0;
	/// The width of the video in screen pixels
	int viewport_width = 0;
	/// Screen pixels between the bottom of the canvas and the bottom of the video; used for glViewport
	int viewport_bottom = 0;
	/// Screen pixels between the bottom of the canvas and the top of the video; used for coordinate space conversion
	int viewport_top = 0;
	/// The height of the video in screen pixels
	int viewport_height = 0;
	int baseViewportScaleFactor = 1;

	/// The current zoom level, where 1.0 = 100%
	double zoomValue;
	/// The current content zoom level inside the base viewport
	double contentZoomValue = 1.0;
	/// Current content pan in units relative to the base viewport height
	double pan_x = 0.0;
	double pan_y = 0.0;
	/// Host-owned sizer changes must not be mistaken for a user resize.
	int internalLayoutResizeDepth = 0;
	std::uint64_t internalLayoutResizeGeneration = 0;
	bool internalLayoutResizePending = false;

	/// The video renderer
	std::unique_ptr<IVideoRenderer> videoRenderer;
	/// Optional secondary renderer used to keep direct subtitle overlays on a separate GL pass
	std::unique_ptr<IVideoRenderer> subtitleOverlayRenderer;

	/// The active visual typesetting tool
	std::unique_ptr<VisualToolBase> tool;
	/// Text cache shared by persistent visual-guide labels on the legacy path.
	std::unique_ptr<OpenGLText> visualGuideText;
	/// Only this display releases text textures, with its own GL context current.
	std::shared_ptr<OpenGLTextTextureDeleter> text_texture_deleter;
	/// The toolbar used by individual typesetting tools
	wxToolBar* toolBar;

	/// The OpenGL context for this display
	std::unique_ptr<wxGLContext> glContext;
	/// Trace-only presentation configuration, sampled once per activated context.
	bool presentation_configuration_traced = false;

	/// The dropdown box for selecting zoom levels
	wxComboBox *zoomBox;

	/// Whether the display can be freely resized by the user
	bool freeSize;

	/// Render packet which will replace the currently visible frame on the next render
	VideoRenderPacket pending_packet;
	bool has_pending_packet = false;
	bool pending_packet_deferred_for_visual_interaction = false;
	/// Last packet successfully uploaded to the current renderer set; reused across backend reloads.
	VideoRenderPacket displayed_packet;
	bool has_displayed_packet = false;
	bool last_render_succeeded = false;
	VisualToolPresentation tool_presentation;
	void PrepareToolPresentation(VideoSubtitleUpdateOptions& options);

	std::unique_ptr<RetinaHelper> retina_helper;
	int scale_factor;
	agi::signal::Connection scale_factor_connection;
	agi::signal::Connection dpi_scale_option_connection;
	agi::signal::Connection renderer_backend_option_connection;

	bool render_requested = false;
	bool render_in_progress = false;
	bool render_scheduled = false;
	/// Trace-only, display-local sequence numbers; never consulted by scheduling.
	int render_request_trace_sequence = 0;
	int render_queue_trace_sequence = 0;
	/// Pure tool feedback pending one render attempt, separate from ordinary repaints.
	bool tool_feedback_dirty = false;
	VideoToolReleaseFeedback release_tool_feedback;
	std::unique_ptr<UiDeadlineTimer> release_tool_feedback_timer;
	bool scene_cache_enabled = true;
	bool scene_cache_retry_blocked = false;
	bool scene_cache_valid = false;
	bool scene_cache_dirty = true;
	bool scene_cache_waiting_for_subtitle_packet = false;
	bool last_frame_had_separate_overlay = false;
	int scene_cache_retry_canvas_width = 0;
	int scene_cache_retry_canvas_height = 0;
	int scene_cache_width = 0;
	int scene_cache_height = 0;
	unsigned int scene_cache_framebuffer = 0;
	unsigned int scene_cache_texture = 0;
	video_subtitle_scene_cache::SubtitleSceneSnapshot displayed_subtitle_scene;
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	bool use_skia_video_tools = false;
	bool use_skia_video_compositor_probe = false;
	SkiaVideoFailureInjection skia_video_failure_injection;
	std::uint64_t gl_context_generation = 0;
	std::uint64_t skia_present_generation = 0;
	std::unique_ptr<SkiaVideoCompositor> skia_video_compositor;
	std::unique_ptr<SkiaSurfaceProvider> skia_overlay_surface_provider;
	std::unique_ptr<SkiaTextLayoutCache> skia_overlay_text_cache;
	unsigned int skia_overlay_framebuffer = 0;
	unsigned int skia_overlay_texture = 0;
	unsigned int skia_overlay_stencil_renderbuffer = 0;
	unsigned int skia_overlay_invert_framebuffer = 0;
	unsigned int skia_overlay_invert_texture = 0;
	unsigned int skia_overlay_invert_stencil_renderbuffer = 0;
	std::unique_ptr<SkiaVideoOverlayCommandBuffer> skia_overlay_cached_commands;
	bool skia_overlay_cache_valid = false;
	std::uint64_t skia_overlay_cache_context_generation = 0;
	int skia_overlay_cache_canvas_width = 0;
	int skia_overlay_cache_canvas_height = 0;
	int skia_overlay_cache_scale_factor = 0;
	int skia_overlay_origin_x = 0;
	int skia_overlay_origin_y = 0;
	int skia_overlay_width = 0;
	int skia_overlay_height = 0;
	int skia_overlay_invert_origin_x = 0;
	int skia_overlay_invert_origin_y = 0;
	int skia_overlay_invert_width = 0;
	int skia_overlay_invert_height = 0;
	bool skia_overlay_backing_release_requested = false;
	std::uint64_t skia_overlay_backing_context_generation = 0;
	// Cached Ganesh surfaces wrapping the overlay FBOs. Re-acquired only when the
	// backing FBO id / size / GL context generation changes (e.g. on a static
	// overlay the same surface is reused across frames instead of re-wrapping
	// the FBO every paint). Destroyed together with the GL backing in
	// DestroySkiaOverlayBacking.
	sk_sp<SkSurface> skia_overlay_surface;
	sk_sp<SkSurface> skia_overlay_invert_surface;
	std::uint64_t skia_overlay_surface_context_generation = 0;
	unsigned int skia_overlay_surface_framebuffer = 0;
	int skia_overlay_surface_width = 0;
	int skia_overlay_surface_height = 0;
	std::uint64_t skia_overlay_invert_surface_context_generation = 0;
	unsigned int skia_overlay_invert_surface_framebuffer = 0;
	int skia_overlay_invert_surface_width = 0;
	int skia_overlay_invert_surface_height = 0;
#endif

	double GetVideoScaleFactor() const;
	void InvalidateSceneCache();
	bool IsSceneCacheUsableForCurrentPlayback() const noexcept;
	bool ShouldUseSceneCacheForCurrentFrame() const noexcept;
	bool ShouldDeferIncomingSubtitlePacket(VideoRenderPacket const& packet) const noexcept;
	void ResetSceneCacheRetryBlock() noexcept;
	void BlockSceneCacheUntilRetry(int canvas_width, int canvas_height) noexcept;
	bool ShouldAttemptSceneCache(int canvas_width, int canvas_height) noexcept;
	void DestroySceneCache() noexcept;
	bool EnsureSceneCache(int canvas_width, int canvas_height);
	void RenderBackendScene(int canvas_width, int canvas_height);
	bool RenderSceneToCache(wxSize const& client_size, int canvas_width, int canvas_height);
	void DrawSceneCache(wxSize const& client_size, int canvas_width, int canvas_height);
	void DrawLegacyOverlayPass(wxSize const& client_size);
	void DrawOverlayPass(wxSize const& client_size);
	void DrawVisualGuides(VideoOverlayDrawContext &draw_context);
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	bool TryDrawSkiaOverlayPass(wxSize const& client_size);
	bool IsSkiaVideoRuntimeRequested() const noexcept;
	SkiaVideoCompositor *EnsureSkiaVideoCompositor();
	SkiaGlContextToken CurrentSkiaGlContextToken() const noexcept;
	SkiaVideoFrameTarget BuildSkiaVideoFrameTarget(wxSize const& client_size);
	void ProbeSkiaVideoCompositor(wxSize const& client_size);
	void LogSkiaVideoFailureOnce();
#endif
	void ResetDisplayedSubtitleScene() noexcept;
	void RefreshDisplayedSubtitleSceneSnapshot();
	void OnSubtitlesCommit(int type, AssDialogue const* changed);

	/// @brief Draw an overscan mask
	/// @param horizontal_percent The percent of the video reserved horizontally
	/// @param vertical_percent The percent of the video reserved vertically
	void DrawOverscanMask(float horizontal_percent, float vertical_percent) const;
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	void DrawOverscanMaskSkia(VideoOverlayDrawContext &draw_context, float horizontal_percent, float vertical_percent) const;
	bool EnsureSkiaOverlayBacking(
		int normal_width,
		int normal_height,
		int invert_width,
		int invert_height,
		bool need_invert,
		int& allocated_targets);
	void DestroySkiaOverlayBacking() noexcept;
#endif

	/// Upload the image for the current frame to the video card
	void UploadFrameData(VideoRenderPacket const&, double);

	/// @brief Initialize the gl context and set the active context to this one
	/// @return Could the context be set?
	bool InitContext();
	void ResetRenderers();
	bool ApplyRendererSourceModePreference();
	void OnRendererBackendChanged(agi::OptionValue const&);
	void ApplyVideoProvider(AsyncVideoProvider *provider);
	void OnVideoProviderChanged(AsyncVideoProvider *provider);
	wxImage CaptureCurrentRenderersImage();
	wxImage CapturePacketImage(VideoRenderPacket const& packet);

	/// @brief Set the size of the display based on the current zoom and video resolution
	void UpdateSize();
	void PositionVideo();
	void RefreshVideoScale();
	/// Set the zoom level to that indicated by the dropdown
	void SetZoomFromBox(wxCommandEvent&);
	/// Set the zoom level to that indicated by the text
	void SetZoomFromBoxText(wxCommandEvent&);
	void Pan(Vector2D delta);
	Vector2D GetZoomAnchorPoint(wxPoint position) const;
	void ZoomAndPan(double newZoomValue, Vector2D anchorPoint, wxPoint newPosition);

	/// @brief Key event handler
	void OnKeyDown(wxKeyEvent &event);
	/// @brief Mouse event handler
	void OnMouseEvent(wxMouseEvent& event);
	void OnMouseWheel(wxMouseEvent& event);
	void OnMouseLeave(wxMouseEvent& event);
	void OnEraseBackground(wxEraseEvent &event);
	void OnPaint(wxPaintEvent &event);
	/// @brief Recalculate video positioning and scaling when the available area or zoom changes
	void OnSizeEvent(wxSizeEvent &event);
	void OnContextMenu(wxContextMenuEvent&);
	void OnIdle(wxIdleEvent&);
	[[nodiscard]] bool IsToolFeedbackReady() const;
	void OnReleaseToolFeedbackTimer();
	void TraceRenderRequest(int kind);
	void TraceRenderState(char const *phase) const;
	void ScheduleRender();
	void DoRender();
	void LayoutContainingSizers();
	void FinishPointSelection(bool cancelled, bool notify);
	/// Apply the cursor the current state asks for: the point-selection session's
	/// while one is active, otherwise the active tool's idle cursor. Single owner
	/// of the canvas cursor, so ending a session cannot strand the tool's.
	void RefreshCursor();
	void FlushVisualToolEditBoxSync();

public:
	/// @brief Constructor
	VideoDisplay(
		wxToolBar *visualSubToolBar,
		bool isDetached,
		wxComboBox *zoomBox,
		wxWindow* parent,
		agi::Context *context);
	~VideoDisplay();

	/// @brief Render the currently visible frame
	void Render();
	/// @brief Render immediately on the UI thread; used for high-frequency tool feedback
	void RenderNow();
	void BeginToolPresentation(std::uint64_t interaction_id);
	void ResetToolPresentation();
	std::shared_ptr<const VisualToolRenderSnapshot> GetToolPresentationSnapshot() const;
	bool NeedsInteractionRender() const;
	/// @brief Request a repaint for per-mouse-event visual tool state changes
	void RenderToolFeedback();
	/// Let a submitted Final share the release/hover feedback draw for one interval.
	void RenderFinalToolFeedback(std::uint64_t final_interaction_id);
	/// Invalidate only the release window; ordinary repaint requests remain intact.
	void CancelReleaseToolFeedback();
	wxImage GetFrameImage(bool raw);
	VideoDisplayMemoryStats CollectMemoryStats() const;
	DEFINE_SIGNAL_ADDERS(FramePresented, AddFramePresentedListener)

	/// @brief Set the zoom level
	/// @param value The new zoom level
	void SetZoom(double value);
	/// @brief Get the current zoom level
	double GetZoom() const { return zoomValue; }
	double GetWindowZoom() const { return zoomValue; }
	void SetWindowZoom(double value) { SetZoom(value); }
	void SyncToCurrentVideoProvider();
	void ResetContentZoom();
	/// Preserve the content transform across host-owned layout resizes. Calls
	/// may be nested; the final End keeps queued size events covered until idle.
	void BeginInternalLayoutResize();
	void EndInternalLayoutResize();
	/// Get the video viewport before attached-mode pan/zoom, in logical pixels.
	/// This includes detached-mode letterboxing and is stable across content transforms.
	wxRect GetBaseViewportRect() const;
	DEFINE_SIGNAL_ADDERS(BaseViewportChanged, AddBaseViewportChangedListener)

	/// Get the last seen position of the mouse in script coordinates
	Vector2D GetMousePosition() const;
	/// Get the current logical video-content viewport for visual-guide mapping.
	VisualGuideViewport GetVisualGuideViewport() const;

	/// Begin a host-owned video point-selection session. Coordinates are
	/// returned in script resolution when script_coordinates is true, otherwise
	/// in source-frame pixels. Starting a new session cancels the old one.
	///
	/// `cursor` is shown over the canvas while the session is armed and should
	/// say what the mode does (an eyedropper for colour sampling); an invalid
	/// cursor falls back to a crosshair. `hint` overrides the generic
	/// "click n points" status hint, which is re-shown on every canvas re-entry
	/// because the status bar clears itself on a timer.
	void BeginPointSelection(
		std::string owner,
		int point_count,
		bool script_coordinates,
		std::function<void(
			std::vector<std::pair<double, double>>, int, bool)> completed,
		wxCursor cursor = wxNullCursor,
		wxString hint = wxString(),
		bool live_zoom = false);
	/// Cancel the active selection only when its opaque owner matches.
	void CancelPointSelection(std::string const& owner, bool notify = true);

	/// Map a display-client position onto a virtual canvas of the given size
	/// (script resolution or source frame), handling letterbox, zoom, pan and
	/// DPI scale. Returns nullopt when outside the video viewport.
	std::optional<std::pair<double, double>> MapClientToVideoPoint(
		wxPoint client_pos, double target_width, double target_height) const;
	/// Map a display-client position onto the nearest raw-storage pixel,
	/// including the provider's clean-aperture crop. Returns nullopt when
	/// outside the video viewport or the geometry is degenerate.
	std::optional<wxPoint> MapClientToStoragePixel(wxPoint client_pos) const;
	/// Map a global screen position to the nearest raw video pixel.
	std::optional<wxPoint> MapScreenToVideoPixel(wxPoint screen_pos) const;

	void SetTool(std::unique_ptr<VisualToolBase> new_tool);
	/// Bind legacy text resource lifetime to this display's GL context.
	std::unique_ptr<OpenGLText> CreateTextRenderer();
	bool IsVisualToolInteracting() const noexcept;

	bool ToolIsType(std::type_info const& type) const;

	/// Whether the current tool can receive keyboard nudges on an active line.
	bool CanNudgeTool() const;
	/// Forward a keyboard nudge to the current visual tool.
	bool NudgeTool(Vector2D direction, VisualNudgeMagnitude magnitude);
	/// Whether the Scale tool can normalize every selected line on this axis.
	bool CanNormalizeScaleTool(VisualScaleAxis axis) const;
	/// Normalize every selected line on one axis using the current Scale tool.
	bool NormalizeScaleTool(VisualScaleAxis axis);
	/// Whether the Measure tool's Perspective submode can Apply right now.
	bool CanApplyMeasurePerspective() const;
	/// Apply the Measure tool's Perspective target to the active line.
	void ApplyMeasurePerspective();
	/// Whether the Measure tool has a selected guide it can remove.
	bool CanRemoveMeasureGuide() const;
	/// Remove the Measure tool's selected measurement guide.
	void RemoveMeasureGuide();
	/// Select a tool-specific sub-mode on the current visual tool.
	bool SetToolSubMode(int mode);
	/// Current tool sub-mode, or -1 when none.
	int GetToolSubMode() const;

	/// Discard all OpenGL state
	void Unload();
};
