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

/// @file video_display.cpp
/// @brief Control displaying a video frame obtained from the video context
/// @ingroup video main_ui
///

#include "video_display.h"

#include "ass_file.h"
#include "ass_time_projection.h"
#include "async_video_provider.h"
#include "async_video_trace.h"
#include "audio_tile_diagnostics_enabled.h"
#include "command/command.h"
#include "compat.h"
#include "format.h"
#include "frame_main.h"
#include "gl_text.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/hotkey.h"
#include "include/aegisub/menu.h"
#include "legacy_gl_draw.h"
#include "options.h"
#include "perf_trace.h"
#include "project.h"
#include "retina_helper.h"
#include "spline_curve.h"
#include "subs_edit_box.h"
#include "utils.h"
#include "ui_deadline_timer.h"
#include "video_render_opengl_proc_loader.h"
#include "video_renderer_factory.h"
#include "video_renderer_error.h"
#include "video_renderer_opengl.h"
#include "video_render_routing.h"
#include "video_display_layout.h"
#include "video_display_frame_policy.h"
#include "video_memory_stats.h"
#include "video_overlay_draw_context_legacy_gl.h"
#include "video_zoom.h"
#include "video_color_pick.h"
#include "video_color_zoom_preview.h"
#include "video_controller.h"
#include "video_frame_wx.h"
#include "visual_guide_overlay.h"
#include "visual_tool.h"
#include "visual_tool_measure.h"
#include "visual_tool_scale.h"

#include <libaegisub/color.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/log.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>
#include <wx/combobox.h>
#include <wx/dcclient.h>
#include <wx/display.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/textctrl.h>
#include <wx/toolbar.h>
#include <wx/weakref.h>

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
#include "skia/skia_video_compositor.h"
#include "skia/skia_video_overlay_command_buffer.h"
#include "skia/skia_video_overlay_gl.h"
#include "skia_runtime/skia_surface_provider.h"
#include "skia_runtime/skia_runtime_feature.h"
#include "skia_runtime/skia_text_layout_cache.h"
#include "video_overlay_draw_context_skia.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkSurface.h>
#endif

/// Attribute list for gl canvases; set the canvases to doublebuffered rgba with an 8 bit stencil buffer
#if wxCHECK_VERSION (3, 1, 1)
// Explicitly set buffer to 24-bit color + 8-bit alpha. See https://github.com/wangqr/Aegisub/issues/55
int attribList[] = { WX_GL_RGBA , WX_GL_DOUBLEBUFFER, WX_GL_STENCIL_SIZE, 8, WX_GL_BUFFER_SIZE, 24, WX_GL_MIN_ALPHA, 8, 0 };
#else
int attribList[] = { WX_GL_RGBA , WX_GL_DOUBLEBUFFER, WX_GL_STENCIL_SIZE, 8, 0 };
#endif

/// An OpenGL error occurred while uploading or displaying a frame
class OpenGlException final : public agi::Exception {
public:
	OpenGlException(const char *func, int err)
	: agi::Exception(agi::format("%s failed with error code %d", func, err))
	{ }
};

#define E(cmd) cmd; if (GLenum err = glGetError()) throw OpenGlException(#cmd, err)

namespace {
void TraceVideoGlLifecycle(char const *phase, VideoDisplay const *display, wxGLContext const *context,
						   std::uintptr_t old_tool, std::uintptr_t new_tool, bool activated = false) noexcept try {
	if (!aegisub::AudioTileDiagnosticsEnabled() || !agi::log::log)
		return;
	void const *current = nullptr;
	void const *owner = nullptr;
#ifdef _WIN32
	current = wglGetCurrentContext();
	if (context)
		owner = context->GetGLRC();
#endif
	std::ostringstream message;
	message.imbue(std::locale::classic());
	message << "phase=" << phase << " source=video_display display=" << display
			<< " context=" << context << " owner=" << owner << " current=" << current
			<< " mismatch=" << (owner && owner != current) << " activated=" << activated
			<< " old_tool=0x" << std::hex << old_tool << " new_tool=0x" << new_tool;
	LOG_I("audio/tile-diagnostics/video-context") << message.str();
}
catch (...) {
	// Diagnostic logging must not alter context activation or tool destruction.
}

template <typename Proc>
Proc LoadOptionalProc(char const *name, char const *fallback_name = nullptr) {
	if (auto *proc = opengl::GetProcAddress(name))
		return reinterpret_cast<Proc>(proc);
	if (fallback_name) {
		if (auto *proc = opengl::GetProcAddress(fallback_name))
			return reinterpret_cast<Proc>(proc);
	}
	return nullptr;
}

void TraceVideoPresentationConfiguration(wxGLCanvas *canvas) {
	// Status: 0=non-WGL, 1=no extension list, 2=extension absent,
	// 3=getter absent, 4=reported nonnegative, 5=reported negative.
	// Negative detail fields are omitted by the trace API, so the reported
	// interval is detail_b for status 4 and -1-detail_b for status 5.
	int interval_status = 0;
	int encoded_interval = -1;
#ifdef _WIN32
	using GetExtensionsArbProc = char const *(WINAPI *)(HDC);
	using GetExtensionsExtProc = char const *(WINAPI *)();
	using GetSwapIntervalProc = int(WINAPI *)();
	char const *extensions = nullptr;
	if (auto get_extensions = LoadOptionalProc<GetExtensionsArbProc>("wglGetExtensionsStringARB"))
		extensions = get_extensions(canvas->GetHDC());
	if (!extensions) {
		if (auto get_extensions = LoadOptionalProc<GetExtensionsExtProc>("wglGetExtensionsStringEXT"))
			extensions = get_extensions();
	}
	interval_status = 1;
	if (extensions) {
		bool supported = false;
		std::string_view names(extensions);
		while (!names.empty()) {
			auto const end = names.find(' ');
			if (names.substr(0, end) == "WGL_EXT_swap_control") {
				supported = true;
				break;
			}
			if (end == std::string_view::npos)
				break;
			names.remove_prefix(end + 1);
		}
		interval_status = supported ? 3 : 2;
		if (supported) {
			if (auto get_interval = LoadOptionalProc<GetSwapIntervalProc>("wglGetSwapIntervalEXT")) {
				int const interval = get_interval();
				interval_status = interval < 0 ? 5 : 4;
				encoded_interval = interval < 0 ? -(interval + 1) : interval;
			}
		}
	}
#endif
	// These observations neither change the interval nor report effective
	// vsync, compositor behavior, queue depth, or an exact refresh period.
	perf_trace::ObserveVideoUiDuration("video_display.swap_interval.reported", 0.0, interval_status, encoded_interval);
	int refresh_status = 0;
	int refresh_hz = -1;
	auto const display_index = wxDisplay::GetFromWindow(canvas);
	if (display_index != wxNOT_FOUND) {
		wxDisplay display(static_cast<unsigned int>(display_index));
		if (display.IsOk()) {
			int const reported_hz = display.GetCurrentMode().GetRefresh();
			refresh_status = reported_hz > 0 ? 2 : 1;
			if (reported_hz > 0)
				refresh_hz = reported_hz;
		}
	}
	// Status: 0=no valid display, 1=unknown refresh, 2=nominal Hz reported.
	perf_trace::ObserveVideoUiDuration("video_display.nominal_refresh_hz", 0.0, refresh_status, refresh_hz);
}

struct CaptureFramebufferFunctions {
	PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = nullptr;
	PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = nullptr;
	PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = nullptr;
	PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = nullptr;
	PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = nullptr;
	PFNGLBINDRENDERBUFFERPROC BindRenderbuffer = nullptr;
	PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers = nullptr;
	PFNGLGENRENDERBUFFERSPROC GenRenderbuffers = nullptr;
	PFNGLRENDERBUFFERSTORAGEPROC RenderbufferStorage = nullptr;
	PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer = nullptr;
};

struct ScopedFramebufferState {
	CaptureFramebufferFunctions const& gl;
	GLint framebuffer = 0;
	GLint draw_buffer = GL_BACK;
	GLint read_buffer = GL_BACK;
	GLint viewport[4] = { 0, 0, 0, 0 };

	explicit ScopedFramebufferState(CaptureFramebufferFunctions const& gl)
	: gl(gl) {
		glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &framebuffer);
		glGetIntegerv(GL_DRAW_BUFFER, &draw_buffer);
		glGetIntegerv(GL_READ_BUFFER, &read_buffer);
		glGetIntegerv(GL_VIEWPORT, viewport);
	}

	~ScopedFramebufferState() {
		if (gl.BindFramebuffer)
			gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, static_cast<GLuint>(framebuffer));
		glDrawBuffer(static_cast<GLenum>(draw_buffer));
		glReadBuffer(framebuffer == 0 ? GL_BACK : static_cast<GLenum>(read_buffer));
		glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
		legacy_gl::ResetCompatibilityState();
	}
};

CaptureFramebufferFunctions const& GetCaptureFramebufferFunctions() {
	static const CaptureFramebufferFunctions functions = {
		LoadOptionalProc<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer", "glBindFramebufferEXT"),
		LoadOptionalProc<PFNGLDELETEFRAMEBUFFERSPROC>("glDeleteFramebuffers", "glDeleteFramebuffersEXT"),
		LoadOptionalProc<PFNGLGENFRAMEBUFFERSPROC>("glGenFramebuffers", "glGenFramebuffersEXT"),
		LoadOptionalProc<PFNGLFRAMEBUFFERTEXTURE2DPROC>("glFramebufferTexture2D", "glFramebufferTexture2DEXT"),
		LoadOptionalProc<PFNGLCHECKFRAMEBUFFERSTATUSPROC>("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT"),
		LoadOptionalProc<PFNGLBINDRENDERBUFFERPROC>("glBindRenderbuffer", "glBindRenderbufferEXT"),
		LoadOptionalProc<PFNGLDELETERENDERBUFFERSPROC>("glDeleteRenderbuffers", "glDeleteRenderbuffersEXT"),
		LoadOptionalProc<PFNGLGENRENDERBUFFERSPROC>("glGenRenderbuffers", "glGenRenderbuffersEXT"),
		LoadOptionalProc<PFNGLRENDERBUFFERSTORAGEPROC>("glRenderbufferStorage", "glRenderbufferStorageEXT"),
		LoadOptionalProc<PFNGLFRAMEBUFFERRENDERBUFFERPROC>("glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT"),
	};
	return functions;
}

void BindWindowFramebufferForDisplayRender() {
	auto const& gl = GetCaptureFramebufferFunctions();
	if (gl.BindFramebuffer) {
		gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
		if (GLenum err = glGetError())
			throw OpenGlException("glBindFramebuffer", err);
		E(glDrawBuffer(GL_BACK));
		E(glReadBuffer(GL_BACK));
	}
	legacy_gl::ResetCompatibilityState();
}

wxImage GetBgraFallbackImage(agi::Context *context, int frame_number, double frame_time, bool raw) {
	auto frame = context->project->VideoProvider()->GetFrameBgra(frame_number, frame_time, raw);
	if (!frame || frame->data.empty())
		return {};
	return GetImage(*frame);
}


VideoMemorySnapshot BuildVideoMemorySnapshot(agi::Context *context, VideoDisplay const* display) {
	VideoMemorySnapshot snapshot;
	if (!context || !context->project)
		return snapshot;

	if (auto* provider = context->project->VideoProvider())
		snapshot.async = provider->CollectMemoryStats();
	if (auto* audio_provider = context->project->AudioProvider())
		snapshot.audio = audio_provider->GetMemoryStats();
	if (display)
		snapshot.display = display->CollectMemoryStats();
	return snapshot;
}

void ApplyViewportLayout(
	VideoDisplayViewportLayout const& layout,
	int& viewport_left,
	int& viewport_width,
	int& viewport_bottom,
	int& viewport_top,
	int& viewport_height) {
	viewport_left = layout.viewport_left;
	viewport_width = layout.viewport_width;
	viewport_bottom = layout.viewport_bottom;
	viewport_top = layout.viewport_top;
	viewport_height = layout.viewport_height;
}

bool SourceFrameColorMetadataEquals(
	SourceFrameColorMetadata const& lhs,
	SourceFrameColorMetadata const& rhs) {
	return lhs.matrix == rhs.matrix
		&& lhs.primaries == rhs.primaries
		&& lhs.transfer == rhs.transfer
		&& lhs.range == rhs.range;
}

bool SourceFrameGeometryEquals(
	SourceFrameGeometry const& lhs,
	SourceFrameGeometry const& rhs) {
	return lhs.storage_width == rhs.storage_width
		&& lhs.storage_height == rhs.storage_height
		&& lhs.visible_rect.x == rhs.visible_rect.x
		&& lhs.visible_rect.y == rhs.visible_rect.y
		&& lhs.visible_rect.width == rhs.visible_rect.width
		&& lhs.visible_rect.height == rhs.visible_rect.height
		&& lhs.rotation == rhs.rotation
		&& lhs.display_vflip == rhs.display_vflip
		&& lhs.pixel_aspect_ratio == rhs.pixel_aspect_ratio;
}

bool SourceFrameNativeFormatIdentityEquals(
	SourceFrameNativeFormatIdentity const& lhs,
	SourceFrameNativeFormatIdentity const& rhs) {
	return lhs.format_namespace == rhs.format_namespace
		&& lhs.format_id == rhs.format_id;
}

bool SourceFrameFloatEquals(float lhs, float rhs) {
	return std::fabs(lhs - rhs) <= 0.000001f;
}

inline bool SourceFrameValueEquals(float lhs, float rhs) {
	return SourceFrameFloatEquals(lhs, rhs);
}

template <typename T>
bool SourceFrameValueEquals(T const& lhs, T const& rhs);

template <typename T, size_t N>
bool SourceFrameValueEquals(std::array<T, N> const& lhs, std::array<T, N> const& rhs);

template <typename T, size_t N>
bool SourceFrameArrayEquals(std::array<T, N> const& lhs, std::array<T, N> const& rhs) {
	for (size_t i = 0; i < N; ++i) {
		if (!SourceFrameValueEquals(lhs[i], rhs[i]))
			return false;
	}
	return true;
}

template <typename T, size_t N>
bool SourceFrameValueEquals(std::array<T, N> const& lhs, std::array<T, N> const& rhs) {
	return SourceFrameArrayEquals(lhs, rhs);
}

template <typename T>
bool SourceFrameValueEquals(T const& lhs, T const& rhs) {
	return lhs == rhs;
}

bool SourceFrameDolbyVisionComponentEquals(
	SourceFrameDolbyVisionReshapeComponent const& lhs,
	SourceFrameDolbyVisionReshapeComponent const& rhs) {
	return lhs.num_pivots == rhs.num_pivots
		&& SourceFrameArrayEquals(lhs.pivots, rhs.pivots)
		&& SourceFrameArrayEquals(lhs.method, rhs.method)
		&& SourceFrameArrayEquals(lhs.poly_coeffs, rhs.poly_coeffs)
		&& SourceFrameArrayEquals(lhs.mmr_order, rhs.mmr_order)
		&& SourceFrameArrayEquals(lhs.mmr_constant, rhs.mmr_constant)
		&& SourceFrameArrayEquals(lhs.mmr_coeffs, rhs.mmr_coeffs);
}

bool SourceFrameDolbyVisionMetadataEquals(
	SourceFrameDolbyVisionMetadata const& lhs,
	SourceFrameDolbyVisionMetadata const& rhs) {
	if (lhs.valid != rhs.valid)
		return false;
	if (!lhs.valid)
		return true;
	if (lhs.bl_bit_depth != rhs.bl_bit_depth
		|| lhs.coefficient_log2_denom != rhs.coefficient_log2_denom
		|| lhs.has_l1 != rhs.has_l1
		|| !SourceFrameArrayEquals(lhs.nonlinear_offset, rhs.nonlinear_offset)
		|| !SourceFrameArrayEquals(lhs.nonlinear, rhs.nonlinear)
		|| !SourceFrameArrayEquals(lhs.linear, rhs.linear)
		|| !SourceFrameFloatEquals(lhs.source_min_pq, rhs.source_min_pq)
		|| !SourceFrameFloatEquals(lhs.source_max_pq, rhs.source_max_pq)
		|| !SourceFrameFloatEquals(lhs.max_pq_y, rhs.max_pq_y)
		|| !SourceFrameFloatEquals(lhs.avg_pq_y, rhs.avg_pq_y)
		|| lhs.rpu != rhs.rpu)
		return false;
	for (size_t i = 0; i < lhs.comp.size(); ++i) {
		if (!SourceFrameDolbyVisionComponentEquals(lhs.comp[i], rhs.comp[i]))
			return false;
	}
	return true;
}

bool SourceFrameEquivalentForUpload(
	SourceFrame const& lhs,
	SourceFrame const& rhs) {
	return lhs.output_mode == rhs.output_mode
		&& lhs.pixel_format == rhs.pixel_format
		&& SourceFrameNativeFormatIdentityEquals(lhs.native_format, rhs.native_format)
		&& SourceFrameFormatInfoEquals(lhs.format_info, rhs.format_info)
		&& lhs.width == rhs.width
		&& lhs.height == rhs.height
		&& lhs.flipped == rhs.flipped
		&& lhs.plane_count == rhs.plane_count
		&& SourceFrameColorMetadataEquals(lhs.color, rhs.color)
		&& SourceFrameDolbyVisionMetadataEquals(lhs.dolby_vision, rhs.dolby_vision)
		&& lhs.chroma_location == rhs.chroma_location
		&& SourceFrameGeometryEquals(lhs.geometry, rhs.geometry);
}

bool ReadEnvFlagDefaultOn(char const *name) {
	auto const* value = std::getenv(name);
	if (!value || !*value)
		return true;

	char const first = static_cast<char>(std::tolower(static_cast<unsigned char>(*value)));
	return first != '0' && first != 'f' && first != 'n';
}

#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
bool ReadEnvFlagDefaultOff(char const *name) {
	auto const* value = std::getenv(name);
	if (!value || !*value)
		return false;

	char const first = static_cast<char>(std::tolower(static_cast<unsigned char>(*value)));
	return first != '0' && first != 'f' && first != 'n';
}

bool IsSkiaVideoOverlayEnabled() {
	return aegisub::skia::ResolveRuntimeFeatureEnabled(
		std::getenv("AEGISUB_ENABLE_SKIA_VIDEO_TOOLS"),
		OPT_GET("Video/Skia Tools/Enabled")->GetBool());
}

bool IsSkiaVideoCompositorProbeEnabled() {
	return ReadEnvFlagDefaultOff("AEGISUB_ENABLE_SKIA_VIDEO_COMPOSITOR_PROBE");
}

SkiaVideoFailureInjection GetSkiaVideoFailureInjection() {
	auto const *value = std::getenv("AEGISUB_SKIA_VIDEO_FAILURE_INJECTION");
	return ParseSkiaVideoFailureInjection(value ? value : "");
}
#endif

}

VideoDisplay::VideoDisplay(wxToolBar *toolbar, bool freeSize, wxComboBox *zoomBox, wxWindow *parent, agi::Context *c)
: wxGLCanvas(parent, -1, attribList)
, autohideTools(OPT_GET("Tool/Visual/Autohide"))
, scrollAction(OPT_GET("Video/Scroll Action"))
, ctrlScrollAction(OPT_GET("Video/Ctrl Scroll Action"))
, shiftScrollAction(OPT_GET("Video/Shift Scroll Action"))
, con(c)
, zoomValue(OPT_GET("Video/Default Zoom")->GetInt() * .125 + .125)
, toolBar(toolbar)
, zoomBox(zoomBox)
, freeSize(freeSize)
, retina_helper(agi::make_unique<RetinaHelper>(this))
, scale_factor(retina_helper->GetScaleFactor())
, scene_cache_enabled(ReadEnvFlagDefaultOn("AEGISUB_ENABLE_VIDEO_SCENE_CACHE"))
, scale_factor_connection(retina_helper->AddScaleFactorListener([=](int new_scale_factor) {
	scale_factor = new_scale_factor;
	RefreshVideoScale();
}))
, dpi_scale_option_connection(OPT_SUB("Video/Scale with DPI", [=](agi::OptionValue const&) { RefreshVideoScale(); }))
, renderer_backend_option_connection(OPT_SUB("Video/Renderer/Backend", &VideoDisplay::OnRendererBackendChanged, this))
{
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	use_skia_video_tools = IsSkiaVideoOverlayEnabled();
	use_skia_video_compositor_probe = IsSkiaVideoCompositorProbeEnabled();
	skia_video_failure_injection = GetSkiaVideoFailureInjection();
#endif
	zoomBox->SetValue(fmt_wx("%g%%", zoomValue * 100.));
	zoomBox->Bind(wxEVT_COMBOBOX, &VideoDisplay::SetZoomFromBox, this);
	zoomBox->Bind(wxEVT_TEXT_ENTER, &VideoDisplay::SetZoomFromBoxText, this);

	connections = agi::signal::make_vector({
		con->videoController->AddFrameReadyListener(&VideoDisplay::UploadFrameData, this),
		con->videoController->AddPrepareVisualSubtitleUpdateListener(&VideoDisplay::PrepareToolPresentation, this),
		con->project->AddVideoProviderListener(&VideoDisplay::OnVideoProviderChanged, this),
		con->videoController->AddARChangeListener(&VideoDisplay::UpdateSize, this),
		con->ass->AddCommitListener(&VideoDisplay::OnSubtitlesCommit, this),
	});
	if (auto controller = c->GetUI().visualGuideController) {
		connections.push_back(controller->AddChangedListener([this] {
			// The attached display stays alive while a detached display owns the
			// shared context. Only the current display should render guide updates.
			if (con->GetUI().videoDisplay == this)
				RenderToolFeedback();
		}));
	}
	// Persistent guides read colours/font size each frame via OPT_GET, but a
	// preference change does not otherwise schedule a repaint.
	auto const render_if_active_display = [this](agi::OptionValue const&) {
		if (con->GetUI().videoDisplay == this)
			Render();
	};
	connections.push_back(OPT_SUB("Colour/Visual Tools/Lines Primary", render_if_active_display));
	connections.push_back(OPT_SUB("Colour/Visual Tools/Highlight Primary", render_if_active_display));
	connections.push_back(OPT_SUB("Tool/Visual/Coordinate Font Size", render_if_active_display));

	SetBackgroundStyle(wxBG_STYLE_PAINT);
	Bind(wxEVT_PAINT, &VideoDisplay::OnPaint, this);
	Bind(wxEVT_ERASE_BACKGROUND, &VideoDisplay::OnEraseBackground, this);
	Bind(wxEVT_IDLE, &VideoDisplay::OnIdle, this);
	Bind(wxEVT_SIZE, &VideoDisplay::OnSizeEvent, this);
	Bind(wxEVT_CONTEXT_MENU, &VideoDisplay::OnContextMenu, this);
	Bind(wxEVT_ENTER_WINDOW, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_CHAR_HOOK, &VideoDisplay::OnKeyDown, this);
	Bind(wxEVT_LEAVE_WINDOW, &VideoDisplay::OnMouseLeave, this);
	Bind(wxEVT_LEFT_DCLICK, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_LEFT_DOWN, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_LEFT_UP, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_MIDDLE_DOWN, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_MIDDLE_UP, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_AUX1_DOWN, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_AUX2_DOWN, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_MOTION, &VideoDisplay::OnMouseEvent, this);
	Bind(wxEVT_MOUSEWHEEL, &VideoDisplay::OnMouseWheel, this);

	SetCursor(wxNullCursor);

	c->GetUI().videoDisplay = this;

	con->videoController->JumpToFrame(con->videoController->GetFrameN());

	SetLayoutDirection(wxLayout_LeftToRight);
}

VideoDisplay::~VideoDisplay () {
	FinishPointSelection(true, false);
	Unload();
}

void VideoDisplay::SyncToCurrentVideoProvider() {
	ApplyVideoProvider(con->project->VideoProvider());
	if (con->project->VideoProvider())
		con->videoController->JumpToFrame(con->videoController->GetFrameN());
}

wxRect VideoDisplay::GetBaseViewportRect() const {
	int const factor = std::max(scale_factor, 1);
	int const left = static_cast<int>(std::lround(
		static_cast<double>(baseViewport.viewport_left) / factor));
	int const right = static_cast<int>(std::lround(
		static_cast<double>(baseViewport.viewport_left + baseViewport.viewport_width) / factor));
	int const top = static_cast<int>(std::lround(
		static_cast<double>(baseViewport.viewport_top) / factor));
	int const bottom = static_cast<int>(std::lround(
		static_cast<double>(baseViewport.viewport_top + baseViewport.viewport_height) / factor));
	return wxRect(left, top, std::max(right - left, 0), std::max(bottom - top, 0));
}

VisualGuideViewport VideoDisplay::GetVisualGuideViewport() const {
	VisualGuideViewport viewport;
	int const factor = std::max(scale_factor, 1);
	viewport.canvas_x = static_cast<double>(viewport_left) / factor;
	viewport.canvas_y = static_cast<double>(viewport_top) / factor;
	viewport.canvas_width = static_cast<double>(viewport_width) / factor;
	viewport.canvas_height = static_cast<double>(viewport_height) / factor;

	int script_width = 0;
	int script_height = 0;
	con->ass->GetResolution(ScriptResolutionType::PlayRes, script_width, script_height);
	viewport.script_width = script_width;
	viewport.script_height = script_height;
	return viewport;
}

void VideoDisplay::DrawVisualGuides(VideoOverlayDrawContext &draw_context) {
	auto controller = con->GetUI().visualGuideController;
	if (!controller)
		return;

	VisualGuideOverlayStyle style;
	style.line_colour = to_wx(OPT_GET("Colour/Visual Tools/Lines Primary")->GetColor());
	style.highlight_colour = to_wx(OPT_GET("Colour/Visual Tools/Highlight Primary")->GetColor());
	style.label_font_size = OPT_GET("Tool/Visual/Coordinate Font Size")->GetInt();

	VisualGuideOverlay overlay;
	overlay.Draw(draw_context, GetVisualGuideViewport(), controller->CaptureView(), style);
}

double VideoDisplay::GetVideoScaleFactor() const {
	if (!OPT_GET("Video/Scale with DPI")->GetBool())
		return 1.0;
	return GetWindowScaleFactor(const_cast<VideoDisplay *>(this));
}

bool VideoDisplay::InitContext() {
	if (!IsShownOnScreen())
		return false;

	// If this display is in a minimized detached dialog IsShownOnScreen will
	// return true, but the client size is guaranteed to be 0
	if (GetClientSize() == wxSize(0, 0))
		return false;

	bool const created_context = !glContext;
	if (created_context) {
		glContext = agi::make_unique<wxGLContext>(this);
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
		++gl_context_generation;
		if (!gl_context_generation)
			++gl_context_generation;
#endif
	}

	bool made_current = false;
#ifdef _WIN32
	int context_was_current = -1;
	int drawable_was_current = -1;
#endif
	{
		perf_trace::VideoUiDurationScope trace("video_display.context_activate");
#ifdef _WIN32
		bool const context_matches = glContext->GetGLRC() != nullptr && wglGetCurrentContext() == glContext->GetGLRC();
		bool const drawable_matches = GetHDC() != nullptr && wglGetCurrentDC() == GetHDC();
		if (trace.IsActive()) {
			context_was_current = context_matches ? 1 : 0;
			drawable_was_current = drawable_matches ? 1 : 0;
		}
		// Other canvases can change the thread's binding. Reuse it only when both
		// native identities currently match; rebinding even the same pair can stall.
		made_current = glContext->IsOK() && ((context_matches && drawable_matches) || SetCurrent(*glContext));
#else
		made_current = glContext->IsOK() && SetCurrent(*glContext);
#endif
		trace.SetDetails(made_current ? 1 : 0, created_context ? 1 : 0);
	}
#ifdef _WIN32
	// Sample before activation, but emit afterward so tracing does not delay the bind.
	if (context_was_current >= 0)
		perf_trace::ObserveVideoUiDuration("video_display.context_pre_current", 0.0, context_was_current, drawable_was_current);
#endif
	TraceVideoGlLifecycle("context_activate", this, glContext.get(),
						  reinterpret_cast<std::uintptr_t>(tool.get()), reinterpret_cast<std::uintptr_t>(tool.get()), made_current);
	if (!made_current) {
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
		if (IsSkiaVideoRuntimeRequested()) {
			if (auto *compositor = EnsureSkiaVideoCompositor())
				compositor->NotifyContextActivationFailure(CurrentSkiaGlContextToken());
			LogSkiaVideoFailureOnce();
		}
#endif
		return false;
	}
	if (!presentation_configuration_traced && perf_trace::IsCategoryEnabled(perf_trace::Category::Video)) {
		presentation_configuration_traced = true;
		TraceVideoPresentationConfiguration(this);
	}
	if (text_texture_deleter)
		text_texture_deleter->Drain();
	return true;
}

void VideoDisplay::InvalidateSceneCache() {
	scene_cache_dirty = true;
	scene_cache_valid = false;
}

bool VideoDisplay::IsSceneCacheUsableForCurrentPlayback() const noexcept {
	return !con->videoController->IsPlaying();
}

bool VideoDisplay::ShouldUseSceneCacheForCurrentFrame() const noexcept {
	if (!videoRenderer)
		return false;
	if (videoRenderer->PrefersSceneCacheForRepaint())
		return true;

	// Compatibility subtitle providers bake subtitles into the uploaded BGRA
	// frame; cache that composited scene so visual tool repaints do not have
	// to redraw the full video backend on every mouse event.
	if (has_displayed_packet && !displayed_packet.allow_source_frame_upload_reuse)
		return true;
	if (has_displayed_packet
		&& DecideVideoRenderRouting(
			displayed_packet,
			videoRenderer->SupportsDirectOverlay()) == VideoRenderRoutingMode::FallbackCompositedFrame)
		return true;

	// Native source frames may require an expensive display-transform pass
	// (e.g. libplacebo YUV→RGB conversion) even when the current backend
	// does not explicitly declare a preference for scene caching.  Caching
	// the composited result avoids repeating that transform on every visual
	// tool repaint.
	return has_displayed_packet
		&& displayed_packet.source_frame.output_mode == SourceFrameOutputMode::Native;
}

bool VideoDisplay::ShouldDeferIncomingSubtitlePacket(VideoRenderPacket const& packet) const noexcept {
	if (!tool || !tool->IsInteracting())
		return false;
	if (!videoRenderer || !has_displayed_packet)
		return false;
	if (last_frame_had_separate_overlay)
		return false;
	if (!IsSceneCacheUsableForCurrentPlayback())
		return false;
	if (packet.frame_number != displayed_packet.frame_number)
		return false;

	// Compatibility providers such as CSRI have already produced a fresh baked
	// frame on the worker. Deferring it keeps the visible subtitle frozen for
	// the whole drag, so present it as soon as it arrives and let the scene
	// cache cover repaint-only mouse events between packets.
	if (!packet.allow_source_frame_upload_reuse)
		return false;

	auto const routing = DecideVideoRenderRouting(packet, videoRenderer->SupportsDirectOverlay());
	bool const packet_uses_integrated_subtitles = routing == VideoRenderRoutingMode::FallbackCompositedFrame;
	return packet_uses_integrated_subtitles && ShouldUseSceneCacheForCurrentFrame();
}

void VideoDisplay::ResetDisplayedSubtitleScene() noexcept {
	displayed_subtitle_scene.clear();
	scene_cache_waiting_for_subtitle_packet = false;
}

void VideoDisplay::ResetSceneCacheRetryBlock() noexcept {
	scene_cache_retry_blocked = false;
	scene_cache_retry_canvas_width = 0;
	scene_cache_retry_canvas_height = 0;
}

void VideoDisplay::BlockSceneCacheUntilRetry(int canvas_width, int canvas_height) noexcept {
	scene_cache_retry_blocked = true;
	scene_cache_retry_canvas_width = canvas_width;
	scene_cache_retry_canvas_height = canvas_height;
	DestroySceneCache();
}

bool VideoDisplay::ShouldAttemptSceneCache(int canvas_width, int canvas_height) noexcept {
	if (!scene_cache_enabled || canvas_width <= 0 || canvas_height <= 0)
		return false;
	if (!ShouldUseSceneCacheForCurrentFrame()) {
		if (scene_cache_framebuffer || scene_cache_texture)
			DestroySceneCache();
		return false;
	}

	if (scene_cache_retry_canvas_width != canvas_width
		|| scene_cache_retry_canvas_height != canvas_height) {
		scene_cache_retry_blocked = false;
		scene_cache_retry_canvas_width = canvas_width;
		scene_cache_retry_canvas_height = canvas_height;
	}

	return !scene_cache_retry_blocked;
}

void VideoDisplay::DestroySceneCache() noexcept {
	if (scene_cache_texture) {
		auto texture = static_cast<GLuint>(scene_cache_texture);
		glDeleteTextures(1, &texture);
		scene_cache_texture = 0;
	}
	if (scene_cache_framebuffer) {
		auto const& gl = GetCaptureFramebufferFunctions();
		if (gl.DeleteFramebuffers) {
			auto framebuffer = static_cast<GLuint>(scene_cache_framebuffer);
			gl.DeleteFramebuffers(1, &framebuffer);
		}
		scene_cache_framebuffer = 0;
	}
	scene_cache_width = 0;
	scene_cache_height = 0;
	scene_cache_valid = false;
	scene_cache_dirty = true;
}

bool VideoDisplay::EnsureSceneCache(int canvas_width, int canvas_height) {
	if (!scene_cache_enabled)
		return false;
	if (canvas_width <= 0 || canvas_height <= 0)
		return false;

	auto const& gl = GetCaptureFramebufferFunctions();
	if (!gl.BindFramebuffer
		|| !gl.DeleteFramebuffers
		|| !gl.GenFramebuffers
		|| !gl.FramebufferTexture2D
		|| !gl.CheckFramebufferStatus) {
		DestroySceneCache();
		return false;
	}

	if (scene_cache_framebuffer
		&& scene_cache_texture
		&& scene_cache_width == canvas_width
		&& scene_cache_height == canvas_height) {
		legacy_gl::ResetCompatibilityState();
		return true;
	}

	DestroySceneCache();

	ScopedFramebufferState restore_state(gl);

	GLuint framebuffer = 0;
	GLuint texture = 0;
	gl.GenFramebuffers(1, &framebuffer);
	if (GLenum err = glGetError())
		throw OpenGlException("glGenFramebuffers", err);
	E(glGenTextures(1, &texture));

	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
	if (GLenum err = glGetError())
		throw OpenGlException("glBindFramebuffer", err);
	E(glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glReadBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glBindTexture(GL_TEXTURE_2D, texture));
	E(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
	E(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
	E(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP));
	E(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP));
	E(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, canvas_width, canvas_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
	gl.FramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texture, 0);
	if (GLenum err = glGetError())
		throw OpenGlException("glFramebufferTexture2D", err);
	if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) {
		glDeleteTextures(1, &texture);
		gl.DeleteFramebuffers(1, &framebuffer);
		return false;
	}
	E(glBindTexture(GL_TEXTURE_2D, 0));

	scene_cache_framebuffer = framebuffer;
	scene_cache_texture = texture;
	scene_cache_width = canvas_width;
	scene_cache_height = canvas_height;
	scene_cache_valid = false;
	scene_cache_dirty = true;
	return true;
}

void VideoDisplay::ResetRenderers() {
	ResetToolPresentation();
	if (glContext)
		SetCurrent(*glContext);

	if (videoRenderer)
		videoRenderer->Reset();
	videoRenderer.reset();

	if (subtitleOverlayRenderer)
		subtitleOverlayRenderer->Reset();
	subtitleOverlayRenderer.reset();
	ResetDisplayedSubtitleScene();
	ResetSceneCacheRetryBlock();
	InvalidateSceneCache();
}

bool VideoDisplay::ApplyRendererSourceModePreference() {
	auto* provider = con->project->VideoProvider();
	if (!provider || !videoRenderer)
		return false;

	bool const changed = provider->SetPreferredSourceModes(videoRenderer->GetPreferredSourceModes());
	if (changed)
		con->videoController->InvalidateRenderPacketCache();
	return changed;
}

void VideoDisplay::OnRendererBackendChanged(agi::OptionValue const&) {
	CancelReleaseToolFeedback();
	if (!con->project->VideoProvider())
		return;

	if (!has_pending_packet && has_displayed_packet) {
		pending_packet = displayed_packet;
		has_pending_packet = true;
		pending_packet_deferred_for_visual_interaction = false;
	}

	ResetRenderers();
	InvalidateSceneCache();

	if (has_pending_packet) {
		TraceRenderState("video_display.render_dispatch.backend_change");
		DoRender();
	}
}

void VideoDisplay::ApplyVideoProvider(AsyncVideoProvider *provider) {
	ResetToolPresentation();
	CancelReleaseToolFeedback();
	last_size_event_client_size = wxDefaultSize;
	pending_packet = { };
	has_pending_packet = false;
	pending_packet_deferred_for_visual_interaction = false;
	displayed_packet = { };
	has_displayed_packet = false;
	ResetDisplayedSubtitleScene();
	contentZoomValue = 1.0;
	pan_x = 0.0;
	pan_y = 0.0;
	ResetRenderers();
	if (glContext)
		SetCurrent(*glContext);
	DestroySceneCache();

	if (!provider)
		return;

	UpdateSize();
}

void VideoDisplay::OnVideoProviderChanged(AsyncVideoProvider *provider) {
	if (auto controller = con->GetUI().visualGuideController)
		controller->ResetForVideoChange();
	ApplyVideoProvider(provider);
}

void VideoDisplay::UploadFrameData(VideoRenderPacket const& packet, double) {
	aegisub::async_video_trace::ObservePipelineEvent({.stage = "display_receive", .version = packet.delivery_version, .delivery_class = packet.delivery_class, .visual_interaction_id = packet.visual_interaction_id, .frame = packet.frame_number});
	if (ShouldIgnoreVideoDisplayFrameReady(
		freeSize,
		con->GetUI().videoDisplay == this)) {
		perf_trace::ObserveVideoUiDuration(
			"video_display.frame_ready_ignored_hidden_attached",
			0.0,
			packet.frame_number);
		return;
	}

	if (con->videoController->IsPlaying())
		CancelReleaseToolFeedback();
	if (has_pending_packet) {
		aegisub::async_video_trace::ObservePipelineEvent({.stage = "display_replace", .version = pending_packet.delivery_version, .delivery_class = pending_packet.delivery_class, .visual_interaction_id = pending_packet.visual_interaction_id, .frame = pending_packet.frame_number});
	}
	bool const defer_interactive_subtitle_packet = ShouldDeferIncomingSubtitlePacket(packet);
	if (defer_interactive_subtitle_packet) {
		pending_packet = packet;
		has_pending_packet = true;
		pending_packet_deferred_for_visual_interaction = true;
		scene_cache_waiting_for_subtitle_packet = false;
		render_requested = true;
		TraceRenderRequest(3);
		if (con->videoController->IsPlaying())
			ScheduleRender();
		return;
	}

	bool const throttle_paused_visual_interaction =
		!con->videoController->IsPlaying()
		&& tool
		&& tool->IsInteracting()
		&& has_displayed_packet
		&& packet.frame_number == displayed_packet.frame_number;
	bool const defer_interactive_playback_same_frame_packet =
		con->videoController->IsPlaying()
		&& tool
		&& tool->IsInteracting()
		&& has_displayed_packet
		&& packet.frame_number == displayed_packet.frame_number;

	bool const can_reuse_video_only_scene_cache = scene_cache_valid
		&& !scene_cache_dirty
		&& has_displayed_packet
		&& videoRenderer
		&& last_frame_had_separate_overlay
		&& DecideVideoRenderRouting(packet, videoRenderer->SupportsDirectOverlay()) == VideoRenderRoutingMode::SecondaryRendererDirectOverlay
		&& packet.allow_source_frame_upload_reuse
		&& displayed_packet.allow_source_frame_upload_reuse
		&& packet.frame_number == displayed_packet.frame_number
		&& SourceFrameEquivalentForUpload(packet.source_frame, displayed_packet.source_frame);

	pending_packet = packet;
	has_pending_packet = true;
	pending_packet_deferred_for_visual_interaction = defer_interactive_playback_same_frame_packet;
	scene_cache_waiting_for_subtitle_packet = false;
	if (!can_reuse_video_only_scene_cache || !IsSceneCacheUsableForCurrentPlayback())
		InvalidateSceneCache();
	if (throttle_paused_visual_interaction) {
		if (con->GetUI().videoDisplay == this && packet.delivery_class == VideoRenderDeliveryClass::VisualSubtitleIntermediate && tool_presentation.MatchesActiveInteraction(packet.visual_interaction_id, packet.visual_tool_snapshot))
			tool->RenderReadyInteractionFrame();
		else
			tool->ScheduleInteractionRender();
	}
	if (defer_interactive_playback_same_frame_packet || throttle_paused_visual_interaction)
		return;

	// Instead of calling Render(), we force a render here to minimize delay
	TraceRenderState("video_display.render_dispatch.packet");
	DoRender();
}

VideoDisplayMemoryStats VideoDisplay::CollectMemoryStats() const {
	VideoDisplayMemoryStats stats;
	if (has_pending_packet)
		stats.pending_packet_ref_bytes = EstimateVideoRenderPacketReferencedBytes(pending_packet);
	if (has_displayed_packet)
		stats.displayed_packet_ref_bytes = EstimateVideoRenderPacketReferencedBytes(displayed_packet);
	if (videoRenderer) {
		stats.primary_renderer_name = videoRenderer->GetDebugName();
		stats.primary_renderer_texture_bytes = videoRenderer->EstimateTextureBytes();
	}
	if (subtitleOverlayRenderer) {
		stats.secondary_renderer_name = subtitleOverlayRenderer->GetDebugName();
		stats.secondary_renderer_texture_bytes = subtitleOverlayRenderer->EstimateTextureBytes();
	}
	if (scene_cache_texture && scene_cache_width > 0 && scene_cache_height > 0)
		stats.scene_cache_texture_bytes = static_cast<size_t>(scene_cache_width) * static_cast<size_t>(scene_cache_height) * 4;
	return stats;
}

void VideoDisplay::Render() {
	render_requested = true;
	TraceRenderRequest(1);
	ScheduleRender();
}

void VideoDisplay::ResetToolPresentation() {
	tool_presentation.Reset();
}

std::shared_ptr<const VisualToolRenderSnapshot> VideoDisplay::GetToolPresentationSnapshot() const {
	if (!has_displayed_packet || con->GetUI().videoDisplay != this || con->videoController->IsPlaying())
		return {};
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	if (IsSkiaVideoRuntimeRequested())
		return {};
#endif
	return tool_presentation.Select(displayed_packet.visual_tool_snapshot);
}

bool VideoDisplay::NeedsInteractionRender() const {
	bool const paired_snapshot = tool_presentation.IsActive() && con->GetUI().videoDisplay == this && !con->videoController->IsPlaying();
	bool const uploadable_packet = has_pending_packet && !(pending_packet_deferred_for_visual_interaction && tool && tool->IsInteracting());
	auto const drawn_snapshot = paired_snapshot && has_displayed_packet
		? tool_presentation.Select(displayed_packet.visual_tool_snapshot)
		: nullptr;
	return ShouldRenderVideoDisplayInteraction(
		paired_snapshot, last_render_succeeded, uploadable_packet, render_requested, IsToolFeedbackReady(),
		drawn_snapshot && drawn_snapshot->HasMouseDrivenFeedback());
}

void VideoDisplay::BeginToolPresentation(std::uint64_t interaction_id) {
	if (!tool || !has_displayed_packet || con->GetUI().videoDisplay != this || con->videoController->IsPlaying())
		return;
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	if (IsSkiaVideoRuntimeRequested())
		return;
#endif
	auto baseline = tool_presentation.Select(displayed_packet.visual_tool_snapshot);
	if (!baseline && tool_presentation.Matches(displayed_packet.visual_tool_snapshot))
		baseline = displayed_packet.visual_tool_snapshot;
	if (!baseline) {
		auto *provider = con->project->VideoProvider();
		if (!provider || !provider->IsCurrent(displayed_packet.delivery_version) || video_subtitle_scene_cache::CurrentFrameSubtitleSceneNeedsRefresh(con->ass->Events, con->project->Timecodes(), static_cast<int>(displayed_packet.time), displayed_subtitle_scene))
			return;
		baseline = tool->CaptureRenderSnapshot(tool_presentation.Context());
	}
	bool const paired = tool_presentation.Begin(interaction_id, std::move(baseline));
	perf_trace::ObserveVideoUiDuration("video_display.tool_snapshot.begin", 0.0, paired ? 1 : 0);
}

void VideoDisplay::PrepareToolPresentation(VideoSubtitleUpdateOptions& options) {
	if (con->GetUI().videoDisplay != this || !tool || con->videoController->IsPlaying())
		return;
	if (!tool_presentation.IsActive() || options.visual_interaction_id != tool_presentation.InteractionId())
		return;
	options.visual_tool_snapshot = tool->CaptureRenderSnapshot(tool_presentation.Context());
}

void VideoDisplay::RenderNow() {
	// Mouse-drag visual tool updates can keep the UI too busy for idle-driven
	// redraws; render synchronously while paused so tool feedback is not gated
	// on subtitle packet delivery cadence. During playback, video packet
	// presentation owns the full redraw cadence; mouse-motion events just update
	// tool state for the next frame instead of hammering the backend between
	// packets, which is especially fragile with libplacebo on some Win10 drivers.
	if (con->videoController->IsPlaying() && tool && tool->IsInteracting())
		return;

	render_requested = true;
	TraceRenderRequest(2);
	if (render_in_progress || con->videoController->IsPlaying()) {
		ScheduleRender();
		return;
	}
	TraceRenderState("video_display.render_dispatch.now");
	DoRender();
}

void VideoDisplay::RenderToolFeedback() {
	perf_trace::VideoUiDurationScope trace("video_display.tool_feedback_request");
	if (trace.IsActive()) {
		int const playing = con->videoController->IsPlaying() ? 1 : 0;
		int const interacting = IsVisualToolInteracting() ? 1 : 0;
		trace.SetDetails(playing, interacting);
		perf_trace::ObserveVideoUiDuration("video_display.tool_feedback_request.begin", 0.0, playing, interacting);
	}
	// Mouse motion arrives far faster than the video frame rate, and every
	// render ends in a SwapBuffers() that can block the UI thread until vsync.
	// Scheduling a render per motion event during playback therefore interleaves
	// extra vsync stalls between video packets, delaying the UploadFrameData
	// callbacks that present them: the picture visibly freezes while the mouse
	// moves. Packet presentation already redraws the overlay from live tool
	// state on every presented frame, so during playback only mark the frame
	// dirty and let that cadence own the repaint.
	if (con->videoController->IsPlaying()) {
		CancelReleaseToolFeedback();
		tool_feedback_dirty = true;
		return;
	}

	tool_feedback_dirty = true;
	release_tool_feedback.RequestFeedback();
	TraceRenderRequest(7);
	if (IsToolFeedbackReady())
		ScheduleRender();
}

bool VideoDisplay::IsToolFeedbackReady() const {
	return tool_feedback_dirty && !con->videoController->IsPlaying() && !release_tool_feedback.ShouldDefer(VideoToolReleaseFeedback::Clock::now());
}

void VideoDisplay::RenderFinalToolFeedback(std::uint64_t final_interaction_id) {
	CancelReleaseToolFeedback();
	if (!final_interaction_id && tool_presentation.IsActive() && tool && !tool->IsInteracting()) {
		auto* provider = con->project->VideoProvider();
		if (provider && provider->IsCurrent(displayed_packet.delivery_version))
			ResetToolPresentation();
	}
	if (con->videoController->IsPlaying() || !release_tool_feedback.Begin(final_interaction_id, VideoToolReleaseFeedback::Clock::now())) {
		perf_trace::ObserveVideoUiDuration("video_display.release_feedback.immediate", 0.0);
		RenderNow();
		return;
	}

	tool_feedback_dirty = true;
	TraceRenderRequest(8);
	if (!release_tool_feedback_timer)
		release_tool_feedback_timer = std::make_unique<UiDeadlineTimer>([this] { OnReleaseToolFeedbackTimer(); });
	release_tool_feedback_timer->StartAt(*release_tool_feedback.Deadline());
	perf_trace::ObserveVideoUiDuration("video_display.release_feedback.arm", 0.0,
									   static_cast<int>(VideoToolReleaseFeedback::MergeInterval.count()));
}

void VideoDisplay::CancelReleaseToolFeedback() {
	if (!release_tool_feedback.Deadline())
		return;
	perf_trace::ObserveVideoUiDuration("video_display.release_feedback.cancel", 0.0);
	release_tool_feedback.Cancel();
	if (release_tool_feedback_timer)
		release_tool_feedback_timer->Stop();
}

void VideoDisplay::OnReleaseToolFeedbackTimer() {
	if (con->videoController->IsPlaying()) {
		CancelReleaseToolFeedback();
		return;
	}
	auto const fallback = release_tool_feedback.Expire(VideoToolReleaseFeedback::Clock::now());
	if (!fallback) {
		if (auto const deadline = release_tool_feedback.Deadline())
			release_tool_feedback_timer->StartAt(*deadline);
		return;
	}
	perf_trace::ObserveVideoUiDuration("video_display.release_feedback.deadline", 0.0, *fallback ? 1 : 0);
	// The window is already ended: even a failed fallback is a single attempt.
	if (*fallback)
		RenderNow();
}

void VideoDisplay::OnEraseBackground(wxEraseEvent &) {
}

void VideoDisplay::OnPaint(wxPaintEvent&) {
	perf_trace::VideoUiDurationScope trace("video_display.paint");
	if (trace.IsActive()) {
		auto const dirty = GetUpdateRegion().GetBox();
		trace.SetDetails(dirty.GetWidth(), dirty.GetHeight());
		perf_trace::ObserveVideoUiDuration("video_display.paint.origin", 0.0, dirty.GetX(), dirty.GetY());
	}
	wxPaintDC dc(this);
	(void)dc;
	TraceRenderState("video_display.render_dispatch.paint");
	DoRender();
}

wxImage VideoDisplay::CapturePacketImage(VideoRenderPacket const& packet) {
	auto* provider = con->project->VideoProvider();
	if (!provider || !packet.source_frame.IsValid())
		return {};

	int const width = provider->GetWidth();
	int const height = provider->GetHeight();
	if (width <= 0 || height <= 0)
		return {};

	auto const& gl = GetCaptureFramebufferFunctions();
	if (!gl.BindFramebuffer
		|| !gl.DeleteFramebuffers
		|| !gl.GenFramebuffers
		|| !gl.FramebufferTexture2D
		|| !gl.CheckFramebufferStatus) {
		return {};
	}

	ScopedFramebufferState restore_state(gl);

	GLuint framebuffer = 0;
	GLuint texture = 0;
	auto cleanup = agi::make_scope_exit([&] {
		if (texture)
			glDeleteTextures(1, &texture);
		if (framebuffer)
			gl.DeleteFramebuffers(1, &framebuffer);
	});

	gl.GenFramebuffers(1, &framebuffer);
	if (GLenum err = glGetError())
		throw OpenGlException("glGenFramebuffers", err);
	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
	if (GLenum err = glGetError())
		throw OpenGlException("glBindFramebuffer", err);
	E(glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glReadBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glGenTextures(1, &texture));
	E(glBindTexture(GL_TEXTURE_2D, texture));
	E(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
	E(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
	E(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
	gl.FramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texture, 0);
	if (GLenum err = glGetError())
		throw OpenGlException("glFramebufferTexture2D", err);
	if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT)
		throw agi::InternalError("Failed to create an offscreen framebuffer for video capture.");

	auto renderer_result = CreateConfiguredVideoRenderer();
	auto capture_renderer = std::move(renderer_result.renderer);
	std::unique_ptr<IVideoRenderer> capture_overlay_renderer;

	auto const routing = DecideVideoRenderRouting(packet, capture_renderer->SupportsDirectOverlay());
	if (routing == VideoRenderRoutingMode::SourceFrameOnly) {
		capture_renderer->UploadFrame(packet.source_frame);
		capture_renderer->UploadOverlay(nullptr);
	}
	else if (routing == VideoRenderRoutingMode::PrimaryRendererDirectOverlay) {
		capture_renderer->UploadFrame(packet.source_frame);
		capture_renderer->UploadOverlay(&packet.subtitle_overlay);
	}
	else if (routing == VideoRenderRoutingMode::SecondaryRendererDirectOverlay) {
		capture_renderer->UploadFrame(packet.source_frame);
		capture_renderer->UploadOverlay(nullptr);
		capture_overlay_renderer = agi::make_unique<OpenGLVideoRenderer>(false, true, false);
		capture_overlay_renderer->UploadFrame(packet.source_frame);
		capture_overlay_renderer->UploadOverlay(&packet.subtitle_overlay);
	}
	else {
		auto display_frame = packet.DisplayFrame();
		if (!display_frame || display_frame->data.empty())
			throw agi::InternalError("Video capture needs a composited BGRA frame for fallback routing.");
		capture_renderer->UploadFrame(MakeBakedSourceFrameView(*display_frame, packet.source_frame));
		capture_renderer->UploadOverlay(nullptr);
	}

	capture_renderer->Render({ 0, 0, width, height }, width, height);
	if (capture_overlay_renderer)
		capture_overlay_renderer->Render({ 0, 0, width, height }, width, height);
	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
	if (GLenum err = glGetError())
		throw OpenGlException("glBindFramebuffer", err);
	E(glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glReadBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glViewport(0, 0, width, height));
	E(glFlush());

	VideoFrame frame;
	frame.width = width;
	frame.height = height;
	frame.pitch = static_cast<size_t>(width) * 4;
	frame.flipped = true;
	frame.data.resize(frame.pitch * frame.height);
	E(glReadPixels(0, 0, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, frame.data.data()));
	return GetImage(frame);
}

wxImage VideoDisplay::CaptureCurrentRenderersImage() {
	auto* provider = con->project->VideoProvider();
	if (!provider || !videoRenderer)
		return {};

	int const width = provider->GetWidth();
	int const height = provider->GetHeight();
	if (width <= 0 || height <= 0)
		return {};

	auto const& gl = GetCaptureFramebufferFunctions();
	if (!gl.BindFramebuffer
		|| !gl.DeleteFramebuffers
		|| !gl.GenFramebuffers
		|| !gl.FramebufferTexture2D
		|| !gl.CheckFramebufferStatus) {
		return {};
	}

	ScopedFramebufferState restore_state(gl);

	GLuint framebuffer = 0;
	GLuint texture = 0;
	auto cleanup = agi::make_scope_exit([&] {
		if (texture)
			glDeleteTextures(1, &texture);
		if (framebuffer)
			gl.DeleteFramebuffers(1, &framebuffer);
	});

	gl.GenFramebuffers(1, &framebuffer);
	if (GLenum err = glGetError())
		throw OpenGlException("glGenFramebuffers", err);
	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
	if (GLenum err = glGetError())
		throw OpenGlException("glBindFramebuffer", err);
	E(glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glReadBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glGenTextures(1, &texture));
	E(glBindTexture(GL_TEXTURE_2D, texture));
	E(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
	E(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
	E(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
	gl.FramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texture, 0);
	if (GLenum err = glGetError())
		throw OpenGlException("glFramebufferTexture2D", err);
	if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT)
		throw agi::InternalError("Failed to create an offscreen framebuffer for video capture.");

	videoRenderer->Render({ 0, 0, width, height }, width, height);
	if (subtitleOverlayRenderer)
		subtitleOverlayRenderer->Render({ 0, 0, width, height }, width, height);
	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
	if (GLenum err = glGetError())
		throw OpenGlException("glBindFramebuffer", err);
	E(glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glReadBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glViewport(0, 0, width, height));
	E(glFlush());

	VideoFrame frame;
	frame.width = width;
	frame.height = height;
	frame.pitch = static_cast<size_t>(width) * 4;
	frame.flipped = true;
	frame.data.resize(frame.pitch * frame.height);
	E(glReadPixels(0, 0, width, height, GL_BGRA_EXT, GL_UNSIGNED_BYTE, frame.data.data()));
	return GetImage(frame);
}

wxImage VideoDisplay::GetFrameImage(bool raw) {
	auto* provider = con->project->VideoProvider();
	if (!provider)
		return {};

	int const frame_number = con->videoController->GetFrameN();
	double const frame_time = con->project->Timecodes().TimeAtFrame(frame_number);
	if (!InitContext())
		return GetBgraFallbackImage(con, frame_number, frame_time, raw);

	VideoRenderPacket packet;
	if (!raw && has_displayed_packet) {
		packet = displayed_packet;
	}
	else {
		packet = provider->GetRenderPacket(frame_number, frame_time, raw);
	}

	try {
		auto image = CapturePacketImage(packet);
		if (image.IsOk())
			return image;
	}
	catch (agi::Exception const&) {
	}

	return GetBgraFallbackImage(con, frame_number, frame_time, raw);
}

void VideoDisplay::OnIdle(wxIdleEvent&) {
	if (con->videoController->IsPlaying())
		CancelReleaseToolFeedback();
	// Playback owns its feedback cadence. Paused feedback may wait for a Final,
	// but is promoted once that short release window ends.
	if (IsToolFeedbackReady()) {
		render_requested = true;
		TraceRenderRequest(6);
	}

	if (render_requested) {
		TraceRenderState("video_display.render_dispatch.idle");
		DoRender();
	}
}

void VideoDisplay::TraceRenderRequest(int kind) {
	if (!perf_trace::IsCategoryEnabled(perf_trace::Category::Video))
		return;

	// Kinds: Render, RenderNow, deferred packet, reentry, frame follow-up,
	// and idle promotion of feedback, respectively (one through six). Seven is
	// a pure feedback request; eight establishes a release-feedback window.
	// Bounded integer fields match the existing observer API. A wrap starts a
	// new trace sequence at one without affecting any render state.
	render_request_trace_sequence = render_request_trace_sequence == std::numeric_limits<int>::max()
										? 1
										: render_request_trace_sequence + 1;
	perf_trace::ObserveVideoUiDuration("video_display.render_request", 0.0, render_request_trace_sequence, kind);
}

void VideoDisplay::TraceRenderState(char const *phase) const {
	if (!perf_trace::IsCategoryEnabled(perf_trace::Category::Video))
		return;

	int const flags = (con->videoController->IsPlaying() ? 1 : 0) | (IsVisualToolInteracting() ? 2 : 0) | (has_pending_packet ? 4 : 0) | (pending_packet_deferred_for_visual_interaction ? 8 : 0) | (render_in_progress ? 16 : 0) | (render_requested ? 32 : 0) | (tool_feedback_dirty ? 64 : 0) | (render_scheduled ? 128 : 0);
	perf_trace::ObserveVideoUiDuration(phase, 0.0, render_request_trace_sequence, flags);
}

void VideoDisplay::ScheduleRender() {
	if (render_scheduled)
		return;

	render_scheduled = true;
	int trace_queue_sequence = 0;
	if (perf_trace::IsCategoryEnabled(perf_trace::Category::Video)) {
		render_queue_trace_sequence = render_queue_trace_sequence == std::numeric_limits<int>::max()
										  ? 1
										  : render_queue_trace_sequence + 1;
		trace_queue_sequence = render_queue_trace_sequence;
		perf_trace::ObserveVideoUiDuration("video_display.render_queue", 0.0, trace_queue_sequence, render_request_trace_sequence);
	}
	CallAfter([this, trace_queue_sequence] {
		render_scheduled = false;
		if (trace_queue_sequence)
			perf_trace::ObserveVideoUiDuration("video_display.render_callback", 0.0, trace_queue_sequence, render_request_trace_sequence);
		if (render_requested || IsToolFeedbackReady()) {
			perf_trace::ObserveVideoUiDuration("video_display.scheduled_render", 0.0);
			TraceRenderState("video_display.render_dispatch.scheduled");
			DoRender();
		}
	});
}

void VideoDisplay::RenderBackendScene(int canvas_width, int canvas_height) {
	videoRenderer->Render(
		{ viewport_left, viewport_bottom, viewport_width, viewport_height },
		canvas_width,
		canvas_height);
	if (subtitleOverlayRenderer) {
		subtitleOverlayRenderer->Render(
			{ viewport_left, viewport_bottom, viewport_width, viewport_height },
			canvas_width,
			canvas_height);
	}
}

bool VideoDisplay::RenderSceneToCache(wxSize const&, int canvas_width, int canvas_height) {
	if (!EnsureSceneCache(canvas_width, canvas_height))
		return false;

	auto const& gl = GetCaptureFramebufferFunctions();
	ScopedFramebufferState restore_state(gl);

	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, static_cast<GLuint>(scene_cache_framebuffer));
	if (GLenum err = glGetError())
		throw OpenGlException("glBindFramebuffer", err);
	E(glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT));
	E(glReadBuffer(GL_COLOR_ATTACHMENT0_EXT));
	// When the subtitle overlay is rendered by a separate pass (e.g. placebo
	// backend), only cache the video layer so that subtitle-only commits can
	// reuse the cache without a full video re-render.
	if (last_frame_had_separate_overlay) {
		videoRenderer->Render(
			{ viewport_left, viewport_bottom, viewport_width, viewport_height },
			canvas_width,
			canvas_height);
	} else {
		RenderBackendScene(canvas_width, canvas_height);
	}
	scene_cache_valid = true;
	scene_cache_dirty = false;
	return true;
}

void VideoDisplay::DrawSceneCache(wxSize const&, int canvas_width, int canvas_height) {
	if (!scene_cache_valid || !scene_cache_texture || canvas_width <= 0 || canvas_height <= 0)
		return;

	BindWindowFramebufferForDisplayRender();
	E(glDisable(GL_SCISSOR_TEST));
	E(glDisable(GL_STENCIL_TEST));
	E(glDisable(GL_CULL_FACE));
	E(glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
	E(glDisable(GL_BLEND));
	E(glViewport(0, 0, canvas_width, canvas_height));
	E(glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
	E(glClearStencil(0));
	E(glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));
	E(glMatrixMode(GL_PROJECTION));
	E(glLoadIdentity());
	E(glOrtho(0.0, canvas_width, 0.0, canvas_height, -1.0, 1.0));
	E(glMatrixMode(GL_MODELVIEW));
	E(glLoadIdentity());
	legacy_gl::DrawTexturedQuad(static_cast<GLuint>(scene_cache_texture), canvas_width, canvas_height);
	if (GLenum err = glGetError())
		throw OpenGlException("legacy_gl::DrawTexturedQuad", err);
}

void VideoDisplay::DrawLegacyOverlayPass(wxSize const& client_size) {
	// Restore legacy-compatible GL state before the overlay pass, since the
	// video backend (libplacebo or modern OpenGL pipeline) may have left VAOs,
	// shader programs, or other non-fixed-function state bound.
	legacy_gl::ResetCompatibilityState();

	// Overlay pass (overscan mask, visual tools) renders in client/window coordinates.
	// Always use a viewport anchored at (0,0) so that ortho coords == mouse coords.
	// The video_pos offset already positions tool features relative to the video.
	E(glViewport(0, 0, client_size.GetWidth() * scale_factor, client_size.GetHeight() * scale_factor));
	E(glMatrixMode(GL_PROJECTION));
	E(glLoadIdentity());
	E(glOrtho(0.0f, client_size.GetWidth(), client_size.GetHeight(), 0.0f, -1000.0f, 1000.0f));
	E(glMatrixMode(GL_MODELVIEW));
	E(glLoadIdentity());

	if (OPT_GET("Video/Overscan Mask")->GetBool()) {
		double ar = con->videoController->GetAspectRatioValue();

		// Based on BBC's guidelines: http://www.bbc.co.uk/guidelines/dq/pdf/tv/tv_standards_london.pdf
		// 16:9 or wider
		if (ar > 1.75) {
			DrawOverscanMask(.1f, .05f);
			DrawOverscanMask(0.035f, 0.035f);
		}
		// Less wide than 16:9 (use 4:3 standard)
		else {
			DrawOverscanMask(.067f, .05f);
			DrawOverscanMask(0.033f, 0.035f);
		}
	}

	if (con->GetUI().visualGuideController) {
		if (!visualGuideText)
			visualGuideText = CreateTextRenderer();
		OpenGLWrapper guide_gl;
		LegacyVideoOverlayDrawContext guide_context(guide_gl, *visualGuideText);
		DrawVisualGuides(guide_context);
	}

	if ((mouse_pos || !autohideTools->GetBool()) && tool) {
		if (tool_presentation.IsActive() && (con->GetUI().videoDisplay != this || con->videoController->IsPlaying()))
			ResetToolPresentation();
		if (auto snapshot = GetToolPresentationSnapshot()) {
			perf_trace::VideoUiDurationScope trace("video_display.tool_snapshot.draw");
			if (!visualGuideText)
				visualGuideText = CreateTextRenderer();
			OpenGLWrapper snapshot_gl;
			LegacyVideoOverlayDrawContext snapshot_context(snapshot_gl, *visualGuideText);
			snapshot->Draw(snapshot_context);
			snapshot->DrawLiveFeedback(mouse_pos);
		}
		else
			tool->Draw();
	}
}

#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
bool VideoDisplay::IsSkiaVideoRuntimeRequested() const noexcept {
	return use_skia_video_tools || use_skia_video_compositor_probe;
}

SkiaVideoCompositor *VideoDisplay::EnsureSkiaVideoCompositor() {
	if (!IsSkiaVideoRuntimeRequested())
		return nullptr;
	if (!skia_video_compositor)
		skia_video_compositor = agi::make_unique<SkiaVideoCompositor>(skia_video_failure_injection);
	return skia_video_compositor.get();
}

SkiaGlContextToken VideoDisplay::CurrentSkiaGlContextToken() const noexcept {
	return { glContext.get(), gl_context_generation };
}

SkiaVideoFrameTarget VideoDisplay::BuildSkiaVideoFrameTarget(wxSize const& client_size) {
	GLint framebuffer = 0;
	GLint stencil_bits = 0;
	// GL_FRAMEBUFFER_BINDING is not a valid query on Windows' software GL 1.1.
	// The wx canvas does not request multisampling, so leave sample_count at zero
	// and only query the FBO binding when the extension/core entry point exists.
	if (GetCaptureFramebufferFunctions().BindFramebuffer)
		glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &framebuffer);
	glGetIntegerv(GL_STENCIL_BITS, &stencil_bits);

	++skia_present_generation;
	if (!skia_present_generation)
		++skia_present_generation;

	SkiaVideoFrameTarget target;
	target.framebuffer_id = static_cast<unsigned int>(std::max(0, framebuffer));
	target.context_generation = gl_context_generation;
	target.width = client_size.GetWidth() * scale_factor;
	target.height = client_size.GetHeight() * scale_factor;
	target.viewport = { 0, 0, target.width, target.height };
	target.origin = SkiaVideoTargetOrigin::BottomLeft;
	target.sample_count = 0;
	target.stencil_bits = std::max(0, stencil_bits);
	target.pixel_format = SkiaVideoTargetPixelFormat::Rgba8;
	target.color_space = SkiaVideoTargetColorSpace::SdrPreview;
	target.hdr_to_sdr_complete = true;
	target.present_generation = skia_present_generation;
	return target;
}

void VideoDisplay::LogSkiaVideoFailureOnce() {
	if (!skia_video_compositor)
		return;
	auto message = skia_video_compositor->TakeFailureLogMessage();
	if (!message.empty())
		LOG_W("video/display/skia") << message;
}

void VideoDisplay::ProbeSkiaVideoCompositor(wxSize const& client_size) {
	if (!use_skia_video_compositor_probe || use_skia_video_tools)
		return;
	auto *compositor = EnsureSkiaVideoCompositor();
	if (!compositor)
		return;

	BindWindowFramebufferForDisplayRender();
	auto const context = CurrentSkiaGlContextToken();
	auto const target = BuildSkiaVideoFrameTarget(client_size);
	if (!compositor->ProbeFrame(context, target))
		LogSkiaVideoFailureOnce();

	// The P1 probe never draws. Restore the explicit legacy boundary anyway so
	// context creation or a failure injection cannot leak GL state into tools.
	BindWindowFramebufferForDisplayRender();
}

bool VideoDisplay::TryDrawSkiaOverlayPass(wxSize const& client_size) try {
	if (!use_skia_video_tools) {
		if (use_skia_video_compositor_probe)
			ProbeSkiaVideoCompositor(client_size);
		return false;
	}
	if (skia_overlay_backing_release_requested) {
		skia_overlay_backing_release_requested = false;
		DestroySkiaOverlayBacking();
	}

	if ((mouse_pos || !autohideTools->GetBool()) && tool && !tool->SupportsOverlayContext())
		return false;

	if (!skia_overlay_text_cache)
		skia_overlay_text_cache = agi::make_unique<SkiaTextLayoutCache>();

	int const canvas_width = client_size.GetWidth() * scale_factor;
	int const canvas_height = client_size.GetHeight() * scale_factor;
	SkiaVideoOverlayCommandBuffer commands;
	{
		perf_trace::VideoUiDurationScope record_trace("video_display.overlay.record");
		SkiaVideoOverlayRecorder recorder(
			[&](std::string const& text, VideoOverlayTextStyle const& style) {
				return skia_overlay_text_cache->MeasureText(text, style);
			},
			static_cast<float>(scale_factor));

		if (OPT_GET("Video/Overscan Mask")->GetBool()) {
			double const ar = con->videoController->GetAspectRatioValue();
			if (ar > 1.75) {
				DrawOverscanMaskSkia(recorder, .1f, .05f);
				DrawOverscanMaskSkia(recorder, 0.035f, 0.035f);
			}
			else {
				DrawOverscanMaskSkia(recorder, 0.067f, 0.05f);
				DrawOverscanMaskSkia(recorder, 0.033f, 0.035f);
			}
			// Overscan previously drew directly to the canvas before a fresh tool
			// context was constructed. Preserve that state boundary in the recorder.
			recorder.SetLineColour(*wxWHITE, 1.0f, 1);
			recorder.SetFillColour(*wxWHITE, 1.0f);
			recorder.ClearInvert();
		}
		DrawVisualGuides(recorder);
		if ((mouse_pos || !autohideTools->GetBool()) && tool)
			tool->DrawOverlay(recorder);

		commands = recorder.TakeBuffer();
		record_trace.SetDetails(static_cast<int>(commands.CommandCount()), tool ? 1 : 0);
	}

	auto const plan_content_bounds = [&](SkiaOverlayLogicalBounds const& logical_bounds) {
		return PlanSkiaOverlayDeviceBounds(
			logical_bounds,
			static_cast<float>(scale_factor),
			canvas_width,
			canvas_height);
	};
	auto const normal_content_bounds = plan_content_bounds(commands.NormalBounds());
	auto const invert_content_bounds = plan_content_bounds(commands.InvertBounds());
	bool const has_normal_content = commands.HasNormalContent() && !normal_content_bounds.IsEmpty();
	bool const has_invert_content = commands.HasInvertContent() && !invert_content_bounds.IsEmpty();
	if (commands.Empty() || (!has_normal_content && !has_invert_content)) {
		skia_overlay_cached_commands.reset();
		skia_overlay_cache_valid = false;
		return true;
	}

	// The draw context always has a normal canvas. An invert-only command stream
	// uses the invert bounds for that unused normal target rather than allocating
	// a second full-window surface.
	auto const normal_required_bounds = has_normal_content
		? normal_content_bounds
		: invert_content_bounds;
	auto const context = CurrentSkiaGlContextToken();
	if (skia_overlay_backing_context_generation
		&& skia_overlay_backing_context_generation != context.generation) {
		DestroySkiaOverlayBacking();
	}
	auto const current_normal_backing =
		skia_overlay_framebuffer
		&& skia_overlay_texture
		&& skia_overlay_stencil_renderbuffer
			? SkiaOverlayDeviceBounds {
				skia_overlay_origin_x,
				skia_overlay_origin_y,
				skia_overlay_width,
				skia_overlay_height,
			}
			: SkiaOverlayDeviceBounds {};
	auto const current_invert_backing =
		skia_overlay_invert_framebuffer
		&& skia_overlay_invert_texture
		&& skia_overlay_invert_stencil_renderbuffer
			? SkiaOverlayDeviceBounds {
				skia_overlay_invert_origin_x,
				skia_overlay_invert_origin_y,
				skia_overlay_invert_width,
				skia_overlay_invert_height,
			}
			: SkiaOverlayDeviceBounds {};
	auto const normal_target_bounds = SelectSkiaOverlayBackingBounds(
		normal_required_bounds,
		current_normal_backing,
		canvas_width,
		canvas_height);
	auto const invert_target_bounds = has_invert_content
		? SelectSkiaOverlayBackingBounds(
			invert_content_bounds,
			current_invert_backing,
			canvas_width,
			canvas_height)
		: SkiaOverlayDeviceBounds {};
	// A failed Ganesh device abandons its context. Do not composite a texture
	// cached before that failure: doing so would bypass BeginFrame and could
	// present stale overlay pixels while the renderer has already fallen back.
	bool const skia_device_healthy = skia_video_compositor
		&& skia_video_compositor->Device().Health() == SkiaGlDeviceHealth::Healthy;
	bool can_reuse_cached_texture = false;
	{
		perf_trace::VideoUiDurationScope cache_trace("video_display.overlay.cache_check");
		can_reuse_cached_texture = skia_device_healthy
			&& skia_overlay_cache_valid
			&& skia_overlay_cached_commands
			&& skia_overlay_cache_context_generation == context.generation
			&& skia_overlay_cache_canvas_width == canvas_width
			&& skia_overlay_cache_canvas_height == canvas_height
			&& skia_overlay_cache_scale_factor == scale_factor
			&& skia_overlay_origin_x == normal_target_bounds.x
			&& skia_overlay_origin_y == normal_target_bounds.y
			&& skia_overlay_width == normal_target_bounds.width
			&& skia_overlay_height == normal_target_bounds.height
			&& skia_overlay_framebuffer
			&& skia_overlay_texture
			&& skia_overlay_stencil_renderbuffer
			&& (!has_invert_content
				|| (skia_overlay_invert_framebuffer
					&& skia_overlay_invert_texture
					&& skia_overlay_invert_stencil_renderbuffer
					&& skia_overlay_invert_origin_x == invert_target_bounds.x
					&& skia_overlay_invert_origin_y == invert_target_bounds.y
					&& skia_overlay_invert_width == invert_target_bounds.width
					&& skia_overlay_invert_height == invert_target_bounds.height))
			&& skia_overlay_cached_commands->NormalBounds() == commands.NormalBounds()
			&& skia_overlay_cached_commands->InvertBounds() == commands.InvertBounds()
			&& skia_overlay_cached_commands->EquivalentTo(commands);
		cache_trace.SetDetails(
			can_reuse_cached_texture ? 1 : 0,
			static_cast<int>(commands.CommandCount()));
	}

	auto composite_overlay = [&] {
		perf_trace::VideoUiDurationScope composite_trace("video_display.overlay.composite");
		BindWindowFramebufferForDisplayRender();
		glViewport(0, 0, canvas_width, canvas_height);
		CompositeSkiaVideoOverlayTextures(
			static_cast<GLuint>(skia_overlay_texture),
			has_normal_content,
			normal_target_bounds,
			static_cast<GLuint>(skia_overlay_invert_texture),
			has_invert_content,
			invert_target_bounds,
			canvas_width,
			canvas_height);
	};
	if (can_reuse_cached_texture) {
		composite_overlay();
		return true;
	}

	auto *compositor = EnsureSkiaVideoCompositor();
	if (!compositor)
		return false;
	auto const target = BuildSkiaVideoFrameTarget(client_size);
	if (!compositor->BeginFrame(context, target)) {
		DestroySkiaOverlayBacking();
		LogSkiaVideoFailureOnce();
		return false;
	}

	if (!skia_overlay_surface_provider)
		skia_overlay_surface_provider = agi::make_unique<SkiaSurfaceProvider>();
	int allocated_targets = 0;
	{
		perf_trace::VideoUiDurationScope backing_trace("video_display.overlay.backing");
		if (!EnsureSkiaOverlayBacking(
			normal_target_bounds.width,
			normal_target_bounds.height,
			invert_target_bounds.width,
			invert_target_bounds.height,
			has_invert_content,
			allocated_targets)) {
			compositor->FailFrame(
				context,
				SkiaGlDeviceFailure::SurfaceAllocationFailed,
				"the bounded Skia video tools framebuffer backing could not be allocated");
			DestroySkiaOverlayBacking();
			LogSkiaVideoFailureOnce();
			return false;
		}
		backing_trace.SetDetails(allocated_targets, has_invert_content ? 1 : 0);
	}
	skia_overlay_backing_context_generation = context.generation;

	auto const& gl = GetCaptureFramebufferFunctions();
	if (!gl.BindFramebuffer) {
		compositor->FailFrame(
			context,
			SkiaGlDeviceFailure::SurfaceAllocationFailed,
			"the framebuffer binding entry point is unavailable");
		DestroySkiaOverlayBacking();
		LogSkiaVideoFailureOnce();
		return false;
	}

	GLint previous_framebuffer = 0;
	GLint previous_draw_buffer = GL_BACK;
	GLint previous_read_buffer = GL_BACK;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &previous_framebuffer);
	glGetIntegerv(GL_DRAW_BUFFER, &previous_draw_buffer);
	glGetIntegerv(GL_READ_BUFFER, &previous_read_buffer);
	auto restore_framebuffer = agi::make_scope_exit([&] {
		gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, static_cast<GLuint>(previous_framebuffer));
		glDrawBuffer(static_cast<GLenum>(previous_draw_buffer));
		glReadBuffer(previous_framebuffer == 0 ? GL_BACK : static_cast<GLenum>(previous_read_buffer));
	});

	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, skia_overlay_framebuffer);
	glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
	glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);

	int surfaces_rewrapped = 0;
	SkCanvas *canvas = nullptr;
	SkCanvas *invert_canvas = nullptr;
	{
		perf_trace::VideoUiDurationScope surface_trace("video_display.overlay.surface");
		SkiaFramebufferSurfaceDescriptor descriptor;
		descriptor.width = normal_target_bounds.width;
		descriptor.height = normal_target_bounds.height;
		descriptor.sample_count = 0;
		descriptor.stencil_bits = 8;
		descriptor.framebuffer_id = skia_overlay_framebuffer;
		descriptor.bottom_left_origin = false;
		// Wrap the FBO as a Ganesh surface. Whenever the FBO id, requested size
		// and GL context generation are unchanged (e.g. a static overlay between
		// paints), the previously wrapped surface is reused instead of being
		// re-created. The cached surface is dropped whenever any of those keys
		// change.
		bool const normal_surface_matches =
			skia_overlay_surface
			&& skia_overlay_surface_context_generation == context.generation
			&& skia_overlay_surface_framebuffer == skia_overlay_framebuffer
			&& skia_overlay_surface_width == normal_target_bounds.width
			&& skia_overlay_surface_height == normal_target_bounds.height;
		if (!normal_surface_matches) {
			skia_overlay_surface = skia_overlay_surface_provider->AcquireFramebufferSurface(
				compositor->Device().Get(),
				descriptor);
			skia_overlay_surface_context_generation = context.generation;
			skia_overlay_surface_framebuffer = skia_overlay_framebuffer;
			skia_overlay_surface_width = normal_target_bounds.width;
			skia_overlay_surface_height = normal_target_bounds.height;
			++surfaces_rewrapped;
		}
		if (!skia_overlay_surface) {
			compositor->FailFrame(
				context,
				SkiaGlDeviceFailure::SurfaceAcquisitionFailed,
				"Skia could not wrap the tools framebuffer as a Ganesh surface");
			DestroySkiaOverlayBacking();
			LogSkiaVideoFailureOnce();
			return false;
		}

		if (has_invert_content) {
			gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, skia_overlay_invert_framebuffer);
			glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
			glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);

			bool const invert_surface_matches =
				skia_overlay_invert_surface
				&& skia_overlay_invert_surface_context_generation == context.generation
				&& skia_overlay_invert_surface_framebuffer == skia_overlay_invert_framebuffer
				&& skia_overlay_invert_surface_width == invert_target_bounds.width
				&& skia_overlay_invert_surface_height == invert_target_bounds.height;
			if (!invert_surface_matches) {
				SkiaFramebufferSurfaceDescriptor invert_descriptor = descriptor;
				invert_descriptor.width = invert_target_bounds.width;
				invert_descriptor.height = invert_target_bounds.height;
				invert_descriptor.framebuffer_id = skia_overlay_invert_framebuffer;
				skia_overlay_invert_surface = skia_overlay_surface_provider->AcquireFramebufferSurface(
					compositor->Device().Get(),
					invert_descriptor);
				skia_overlay_invert_surface_context_generation = context.generation;
				skia_overlay_invert_surface_framebuffer = skia_overlay_invert_framebuffer;
				skia_overlay_invert_surface_width = invert_target_bounds.width;
				skia_overlay_invert_surface_height = invert_target_bounds.height;
				++surfaces_rewrapped;
			}
			if (!skia_overlay_invert_surface) {
				compositor->FailFrame(
					context,
					SkiaGlDeviceFailure::SurfaceAcquisitionFailed,
					"Skia could not wrap the invert framebuffer as a Ganesh surface");
				DestroySkiaOverlayBacking();
				LogSkiaVideoFailureOnce();
				return false;
			}
		}
		surface_trace.SetDetails(surfaces_rewrapped, has_invert_content ? 1 : 0);

		canvas = skia_overlay_surface->getCanvas();
		invert_canvas = has_invert_content && skia_overlay_invert_surface
			? skia_overlay_invert_surface->getCanvas()
			: nullptr;
	}
	if (!canvas || (has_invert_content && !invert_canvas)) {
		compositor->FailFrame(
			context,
			SkiaGlDeviceFailure::SurfaceAcquisitionFailed,
			"Skia returned a framebuffer surface without a canvas");
		DestroySkiaOverlayBacking();
		LogSkiaVideoFailureOnce();
		return false;
	}

	canvas->clear(SK_ColorTRANSPARENT);
	canvas->save();
	canvas->scale(scale_factor, scale_factor);
	canvas->translate(
		-static_cast<float>(normal_target_bounds.x) / scale_factor,
		-static_cast<float>(normal_target_bounds.y) / scale_factor);
	if (invert_canvas) {
		invert_canvas->clear(SK_ColorTRANSPARENT);
		invert_canvas->save();
		invert_canvas->scale(scale_factor, scale_factor);
		invert_canvas->translate(
			-static_cast<float>(invert_target_bounds.x) / scale_factor,
			-static_cast<float>(invert_target_bounds.y) / scale_factor);
	}

	SkiaVideoOverlayDrawContext draw_context(
		*canvas,
		invert_canvas,
		*skia_overlay_text_cache,
		static_cast<float>(scale_factor));
	{
		perf_trace::VideoUiDurationScope replay_trace("video_display.overlay.replay");
		commands.Replay(draw_context);
		replay_trace.SetDetails(static_cast<int>(commands.CommandCount()), has_invert_content ? 1 : 0);
	}

	canvas->restore();
	if (invert_canvas)
		invert_canvas->restore();
	bool frame_succeeded = false;
	{
		perf_trace::VideoUiDurationScope submit_trace("video_display.overlay.submit");
		frame_succeeded = compositor->FinishFrame(context, true);
		submit_trace.SetDetails(frame_succeeded ? 1 : 0, static_cast<int>(commands.CommandCount()));
	}
	if (!frame_succeeded) {
		DestroySkiaOverlayBacking();
		LogSkiaVideoFailureOnce();
		return false;
	}

	skia_overlay_origin_x = normal_target_bounds.x;
	skia_overlay_origin_y = normal_target_bounds.y;
	if (has_invert_content) {
		skia_overlay_invert_origin_x = invert_target_bounds.x;
		skia_overlay_invert_origin_y = invert_target_bounds.y;
	}
	skia_overlay_cache_context_generation = context.generation;
	skia_overlay_cache_canvas_width = canvas_width;
	skia_overlay_cache_canvas_height = canvas_height;
	skia_overlay_cache_scale_factor = scale_factor;
	skia_overlay_cached_commands = std::make_unique<SkiaVideoOverlayCommandBuffer>(std::move(commands));
	skia_overlay_cache_valid = true;
	composite_overlay();
	return true;
}
catch (agi::Exception const& err) {
	if (auto *compositor = EnsureSkiaVideoCompositor()) {
		compositor->FailFrame(
			CurrentSkiaGlContextToken(),
			SkiaGlDeviceFailure::SurfaceAllocationFailed,
			err.GetMessage());
	}
	DestroySkiaOverlayBacking();
	LogSkiaVideoFailureOnce();
	BindWindowFramebufferForDisplayRender();
	return false;
}
catch (std::exception const& err) {
	if (auto *compositor = EnsureSkiaVideoCompositor()) {
		compositor->FailFrame(
			CurrentSkiaGlContextToken(),
			SkiaGlDeviceFailure::SurfaceAcquisitionFailed,
			err.what());
	}
	DestroySkiaOverlayBacking();
	LogSkiaVideoFailureOnce();
	BindWindowFramebufferForDisplayRender();
	return false;
}
catch (...) {
	if (auto *compositor = EnsureSkiaVideoCompositor()) {
		compositor->FailFrame(
			CurrentSkiaGlContextToken(),
			SkiaGlDeviceFailure::SurfaceAcquisitionFailed,
			"an unknown exception escaped the Skia video tools frame");
	}
	DestroySkiaOverlayBacking();
	LogSkiaVideoFailureOnce();
	BindWindowFramebufferForDisplayRender();
	return false;
}
#endif

void VideoDisplay::DrawOverlayPass(wxSize const& client_size) {
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	if (TryDrawSkiaOverlayPass(client_size))
		return;
#endif
	DrawLegacyOverlayPass(client_size);
}

void VideoDisplay::RefreshDisplayedSubtitleSceneSnapshot() {
	displayed_subtitle_scene.clear();

	if (!has_displayed_packet)
		return;

	auto *subs = con->ass.get();
	auto *project = con->project.get();
	if (!subs || !project)
		return;

	auto const frame_time = static_cast<int>(displayed_packet.time);
	auto const& fps = project->Timecodes();
	displayed_subtitle_scene = video_subtitle_scene_cache::CaptureSubtitleSceneSnapshot(
		subs->Events,
		fps,
		frame_time);
}

void VideoDisplay::OnSubtitlesCommit(int type, AssDialogue const* changed) {
	if (!video_subtitle_scene_cache::IsVisualSubtitleCommitType(type))
		return;

	auto render_after_commit = [&] {
		if (tool && tool->IsInteracting())
			return;
		perf_trace::VideoUiDurationScope trace("video_display.subtitle_commit_render_request", type, 0);
		if (trace.IsActive())
			perf_trace::ObserveVideoUiDuration("video_display.subtitle_commit_render_request.begin", 0.0, type, 0);
		if (tool && tool->IsFinishingLocalInteractionCommit())
			RenderToolFeedback();
		else
			Render();
	};

	if (!has_displayed_packet) {
		if (!last_frame_had_separate_overlay)
			InvalidateSceneCache();
		scene_cache_waiting_for_subtitle_packet = false;
		return;
	}

	auto *subs = con->ass.get();
	auto *project = con->project.get();
	if (!subs || !project) {
		if (!last_frame_had_separate_overlay)
			InvalidateSceneCache();
		scene_cache_waiting_for_subtitle_packet = true;
		render_after_commit();
		return;
	}

	bool const keep_interactive_cache = tool
		&& tool->IsInteracting()
		&& !last_frame_had_separate_overlay
		&& ShouldUseSceneCacheForCurrentFrame();

	scene_cache_waiting_for_subtitle_packet = !keep_interactive_cache
		&& video_subtitle_scene_cache::ShouldWaitForFreshPacket(
			type,
			has_displayed_packet,
			changed != nullptr,
			subs->Events,
			project->Timecodes(),
			static_cast<int>(displayed_packet.time),
			displayed_subtitle_scene);

	// Integrated subtitle rendering bakes the subtitle layer into the cached
	// scene, so any visual commit invalidates the cache. In separate-overlay
	// mode the cache is video-only and remains reusable; the wait flag above
	// simply blocks reuse until a fresh overlay packet arrives when needed.
	if (!last_frame_had_separate_overlay && !keep_interactive_cache)
		InvalidateSceneCache();
	render_after_commit();
}

void VideoDisplay::DoRender() try {
	if (render_in_progress) {
		render_requested = true;
		TraceRenderRequest(4);
		ScheduleRender();
		return;
	}
	perf_trace::VideoUiDurationScope render_trace("video_display.render");

	render_in_progress = true;
	render_scheduled = false;
	auto finish_render = agi::make_scope_exit([&] {
		render_in_progress = false;
		if (render_requested)
			ScheduleRender();
	});
	TraceRenderState("video_display.render_consume");
	render_requested = false;
	// Consume this feedback attempt. Release feedback is only satisfied after
	// a successful swap; failure leaves its single deadline fallback intact.
	tool_feedback_dirty = false;
	last_render_succeeded = false;

	if (!con->project->VideoProvider() || !InitContext() || (!videoRenderer && !has_pending_packet))
		return;

	bool presented_new_frame = false;
	int presented_frame_number = -1;
	bool first_presented_frame = false;

	bool renderer_was_just_created = false;
	if (!videoRenderer) {
		auto renderer_result = CreateConfiguredVideoRenderer();
		videoRenderer = std::move(renderer_result.renderer);
		renderer_was_just_created = true;
		if (ApplyRendererSourceModePreference()) {
			pending_packet = { };
			has_pending_packet = false;
			pending_packet_deferred_for_visual_interaction = false;
			displayed_packet = { };
			has_displayed_packet = false;
			con->videoController->JumpToFrame(con->videoController->GetFrameN());
			return;
		}
	}

	if (!tool)
		cmd::call("video/tool/cross", con);

	try {
		if (has_pending_packet && !(pending_packet_deferred_for_visual_interaction && tool && tool->IsInteracting())) {
			aegisub::async_video_trace::ObservePipelineEvent({.stage = "display_upload_begin", .version = pending_packet.delivery_version, .delivery_class = pending_packet.delivery_class, .visual_interaction_id = pending_packet.visual_interaction_id, .frame = pending_packet.frame_number});
			bool const packet_was_deferred_for_visual_interaction = pending_packet_deferred_for_visual_interaction;
			first_presented_frame = !has_displayed_packet;
			bool const reuse_uploaded_source_frame =
				pending_packet.allow_source_frame_upload_reuse
				&&
				!renderer_was_just_created
				&& has_displayed_packet
				&& displayed_packet.allow_source_frame_upload_reuse
				&& pending_packet.frame_number == displayed_packet.frame_number
				&& SourceFrameEquivalentForUpload(pending_packet.source_frame, displayed_packet.source_frame);
			auto const routing = DecideVideoRenderRouting(
				pending_packet,
				videoRenderer->SupportsDirectOverlay());
			if (ShouldInvalidateSceneCacheAfterDeferredPacket(
				packet_was_deferred_for_visual_interaction,
				routing))
				InvalidateSceneCache();
			last_frame_had_separate_overlay = (routing == VideoRenderRoutingMode::SecondaryRendererDirectOverlay);

			if (routing == VideoRenderRoutingMode::SourceFrameOnly) {
				if (!reuse_uploaded_source_frame)
					videoRenderer->UploadFrame(pending_packet.source_frame);
				videoRenderer->UploadOverlay(nullptr);
				if (subtitleOverlayRenderer)
					subtitleOverlayRenderer->UploadOverlay(nullptr);
			}
			else if (routing == VideoRenderRoutingMode::PrimaryRendererDirectOverlay) {
				if (!reuse_uploaded_source_frame)
					videoRenderer->UploadFrame(pending_packet.source_frame);
				videoRenderer->UploadOverlay(&pending_packet.subtitle_overlay);
				if (subtitleOverlayRenderer)
					subtitleOverlayRenderer->UploadOverlay(nullptr);
			}
			else if (routing == VideoRenderRoutingMode::SecondaryRendererDirectOverlay) {
				if (!reuse_uploaded_source_frame)
					videoRenderer->UploadFrame(pending_packet.source_frame);
				videoRenderer->UploadOverlay(nullptr);
				bool const created_overlay_renderer = !subtitleOverlayRenderer;
				if (created_overlay_renderer)
					subtitleOverlayRenderer = agi::make_unique<OpenGLVideoRenderer>(false, true, false);
				if (!reuse_uploaded_source_frame || created_overlay_renderer)
					subtitleOverlayRenderer->UploadFrame(pending_packet.source_frame);
				subtitleOverlayRenderer->UploadOverlay(&pending_packet.subtitle_overlay);
			}
			else {
				auto display_frame = pending_packet.DisplayFrame();
				videoRenderer->UploadFrame(MakeBakedSourceFrameView(*display_frame, pending_packet.source_frame));
				videoRenderer->UploadOverlay(nullptr);
				if (subtitleOverlayRenderer)
					subtitleOverlayRenderer->UploadOverlay(nullptr);
			}
			displayed_packet = pending_packet;
			has_displayed_packet = true;
			pending_packet = { };
			has_pending_packet = false;
			pending_packet_deferred_for_visual_interaction = false;
			RefreshDisplayedSubtitleSceneSnapshot();
			presented_new_frame = true;
			presented_frame_number = displayed_packet.frame_number;
			aegisub::async_video_trace::ObservePipelineEvent({.stage = "display_upload_end", .version = displayed_packet.delivery_version, .delivery_class = displayed_packet.delivery_class, .visual_interaction_id = displayed_packet.visual_interaction_id, .frame = displayed_packet.frame_number});
		}
	}
	catch (const VideoOutInitException& err) {
		wxLogError(
			wxS("Failed to initialize video display. Closing other running "
			    "programs and updating your video card drivers may fix this.\n"
			    "Error message reported: %s"),
			to_wx(err.GetMessage()));
		con->project->CloseVideo();
		return;
	}
	catch (const VideoOutRenderException& err) {
		wxLogError(
			wxS("Could not upload video frame to graphics card.\n"
			    "Error message reported: %s"),
			to_wx(err.GetMessage()));
		return;
	}

	if (videoSize.GetWidth() == 0) videoSize.SetWidth(1);
	if (videoSize.GetHeight() == 0) videoSize.SetHeight(1);

	if (!viewport_height || !viewport_width)
		PositionVideo();

	wxSize client_size = GetClientSize();
	client_size = wxSize(std::max(1, client_size.GetWidth()), std::max(1, client_size.GetHeight()));
	int const canvas_width = client_size.GetWidth() * scale_factor;
	int const canvas_height = client_size.GetHeight() * scale_factor;

	bool const skip_scene_cache_for_renderer_warmup = first_presented_frame || renderer_was_just_created;

	bool rendered_from_scene_cache = false;
	char const* direct_render_phase = nullptr;
	bool attempt_scene_cache = false;
	if (skip_scene_cache_for_renderer_warmup)
		direct_render_phase = "video_display.scene_cache.direct.warmup";
	else if (scene_cache_waiting_for_subtitle_packet)
		direct_render_phase = "video_display.scene_cache.direct.waiting";
	else if (!IsSceneCacheUsableForCurrentPlayback())
		direct_render_phase = "video_display.scene_cache.direct.playback";
	else if (!ShouldAttemptSceneCache(canvas_width, canvas_height))
		direct_render_phase = "video_display.scene_cache.direct.policy";
	else
		attempt_scene_cache = true;

	if (attempt_scene_cache) {
		try {
			if (scene_cache_dirty
				|| !scene_cache_valid
				|| scene_cache_width != canvas_width
				|| scene_cache_height != canvas_height) {
				perf_trace::VideoUiDurationScope fill_trace(
					"video_display.scene_cache.fill",
					canvas_width,
					canvas_height);
				if (!RenderSceneToCache(client_size, canvas_width, canvas_height)) {
					LOG_W("video/display/scene_cache")
						<< "Video scene cache could not be created for canvas "
						<< canvas_width << "x" << canvas_height
						<< "; falling back to direct backend rendering until the display size changes or the renderer resets.";
					BlockSceneCacheUntilRetry(canvas_width, canvas_height);
				}
				if (!scene_cache_retry_blocked) {
					DrawSceneCache(client_size, canvas_width, canvas_height);
					rendered_from_scene_cache = true;
				}
			}
			else {
				perf_trace::VideoUiDurationScope reuse_trace(
					"video_display.scene_cache.reuse",
					canvas_width,
					canvas_height);
				DrawSceneCache(client_size, canvas_width, canvas_height);
				rendered_from_scene_cache = true;
			}
		}
		catch (agi::Exception const& err) {
			LOG_W("video/display/scene_cache")
				<< "Video scene cache failed for canvas "
				<< canvas_width << "x" << canvas_height
				<< "; falling back to direct backend rendering until the display size changes or the renderer resets: "
				<< err.GetMessage();
			BlockSceneCacheUntilRetry(canvas_width, canvas_height);
		}
		if (!rendered_from_scene_cache)
			direct_render_phase = "video_display.scene_cache.direct.fallback";
	}
	if (!rendered_from_scene_cache) {
		perf_trace::VideoUiDurationScope direct_trace(
			direct_render_phase ? direct_render_phase : "video_display.scene_cache.direct.policy",
			canvas_width,
			canvas_height);
		BindWindowFramebufferForDisplayRender();
		RenderBackendScene(canvas_width, canvas_height);
		scene_cache_valid = false;
		scene_cache_dirty = true;
	}

	// When the scene cache is video-only (separate overlay mode), the subtitle
	// overlay pass was skipped during cache fill and must be applied now.
	if (rendered_from_scene_cache && last_frame_had_separate_overlay && subtitleOverlayRenderer) {
		BindWindowFramebufferForDisplayRender();
		subtitleOverlayRenderer->Render(
			{ viewport_left, viewport_bottom, viewport_width, viewport_height },
			canvas_width,
			canvas_height);
	}

	int const overlay_frame_number = con->videoController->GetPresentedFrameN();
	DrawOverlayPass(client_size);

	bool swapped = false;
	{
		perf_trace::VideoUiDurationScope swap_trace("video_display.swap");
		swapped = SwapBuffers();
		swap_trace.SetDetails(swapped ? 1 : 0, presented_new_frame ? 1 : 0);
	}
	last_render_succeeded = swapped;
	if (swapped && tool_presentation.IsActive() && tool && !tool->IsInteracting()) {
		auto *provider = con->project->VideoProvider();
		if (provider && provider->IsCurrent(displayed_packet.delivery_version)) {
			if (displayed_packet.delivery_class == VideoRenderDeliveryClass::VisualSubtitleFinal)
				tool_presentation.OnFinalPresented(displayed_packet.visual_interaction_id);
			if (tool_presentation.IsActive())
				ResetToolPresentation();
		}
	}
	if (swapped && release_tool_feedback.Deadline()) {
		auto const final_id = presented_new_frame && displayed_packet.delivery_class == VideoRenderDeliveryClass::VisualSubtitleFinal
								  ? displayed_packet.visual_interaction_id
								  : 0;
		bool const finished = release_tool_feedback.OnPresented(final_id);
		perf_trace::ObserveVideoUiDuration("video_display.release_feedback.presented", 0.0, finished ? 1 : 0);
		if (finished)
			release_tool_feedback_timer->Stop();
	}
	render_trace.SetDetails(presented_new_frame ? 1 : 0, swapped ? 1 : 0);
	if (presented_new_frame) {
		aegisub::async_video_trace::ObservePipelineEvent({.stage = swapped ? "display_present" : "display_swap_failed", .version = displayed_packet.delivery_version, .delivery_class = displayed_packet.delivery_class, .visual_interaction_id = displayed_packet.visual_interaction_id, .frame = displayed_packet.frame_number});
		if (zoom_preview)
			zoom_preview->OnFramePresented(presented_frame_number);
		FramePresented(presented_frame_number);
		con->videoController->NotifyFramePresented(presented_frame_number);
		// Frame-dependent tools need a paused follow-up only if the overlay
		// was drawn against a different presented frame. A subtitle-only
		// packet for the same frame already has current tool feedback.
		if (tool && !con->videoController->IsPlaying() && overlay_frame_number != presented_frame_number) {
			render_requested = true;
			TraceRenderRequest(5);
			ScheduleRender();
		}
		if (perf_trace::ShouldSampleVideoMemory(first_presented_frame)) {
			auto snapshot = BuildVideoMemorySnapshot(con, this);
			perf_trace::ObserveVideoMemorySnapshot("frame_presented", snapshot, first_presented_frame);
		}
	}
}
catch (const agi::Exception &err) {
	wxLogError(
		wxS("An error occurred trying to render the video frame on the screen.\n"
		    "Error message reported: %s"),
		to_wx(err.GetMessage()));
	con->project->CloseVideo();
}

void VideoDisplay::DrawOverscanMask(float horizontal_percent, float vertical_percent) const {
	// This pass renders in logical client coordinates, so keep the clip rect and
	// mask geometry in the same space on HiDPI displays.
	Vector2D viewport_pos = Vector2D(viewport_left, viewport_top) / scale_factor;
	Vector2D viewport_size = Vector2D(viewport_width, viewport_height) / scale_factor;
	Vector2D v = viewport_size;
	Vector2D size = Vector2D(horizontal_percent, vertical_percent) / 2 * v;

	// Clockwise from top-left
	Vector2D corners[] = {
		size,
		Vector2D(viewport_size.X() - size.X(), size),
		v - size,
		Vector2D(size, viewport_size.Y() - size.Y())
	};

	// Shift to compensate for black bars
	for (auto& corner : corners)
		corner = corner + viewport_pos;

	int count = 0;
	std::vector<float> points;
	for (size_t i = 0; i < 4; ++i) {
		size_t prev = (i + 3) % 4;
		size_t next = (i + 1) % 4;
		count += SplineCurve(
				(corners[prev] + corners[i] * 4) / 5,
				corners[i], corners[i],
				(corners[next] + corners[i] * 4) / 5)
			.GetPoints(points);
	}

	OpenGLWrapper gl;
	gl.SetFillColour(wxColor(30, 70, 200), .5f);
	gl.SetLineColour(*wxBLACK, 0, 1);

	std::vector<int> vstart(1, 0);
	std::vector<int> vcount(1, count);
	gl.DrawMultiPolygon(points, vstart, vcount, viewport_pos, viewport_size, true);
}

#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
void VideoDisplay::DrawOverscanMaskSkia(
	VideoOverlayDrawContext &draw_context,
	float horizontal_percent,
	float vertical_percent) const {
	Vector2D viewport_pos = Vector2D(viewport_left, viewport_top) / scale_factor;
	Vector2D viewport_size = Vector2D(viewport_width, viewport_height) / scale_factor;
	Vector2D const size = Vector2D(horizontal_percent, vertical_percent) / 2 * viewport_size;

	Vector2D corners[] = {
		size,
		Vector2D(viewport_size.X() - size.X(), size),
		viewport_size - size,
		Vector2D(size, viewport_size.Y() - size.Y())
	};

	for (auto& corner : corners)
		corner = corner + viewport_pos;

	int point_count = 0;
	std::vector<float> points;
	for (size_t i = 0; i < 4; ++i) {
		size_t const prev = (i + 3) % 4;
		size_t const next = (i + 1) % 4;
		point_count += SplineCurve(
			(corners[prev] + corners[i] * 4) / 5,
			corners[i], corners[i],
			(corners[next] + corners[i] * 4) / 5)
			.GetPoints(points);
	}

	draw_context.SetFillColour(wxColour(30, 70, 200, 128), 1.0f);
	draw_context.SetLineColour(*wxBLACK, 0.0f, 1);
	draw_context.DrawMultiPolygon(
		points,
		{ 0 },
		{ point_count },
		viewport_pos,
		viewport_size,
		true);
}

bool VideoDisplay::EnsureSkiaOverlayBacking(
	int normal_width,
	int normal_height,
	int invert_width,
	int invert_height,
	bool need_invert,
	int& allocated_targets) {
	allocated_targets = 0;
	if (normal_width <= 0
		|| normal_height <= 0
		|| (need_invert && (invert_width <= 0 || invert_height <= 0))) {
		return false;
	}

	bool const has_normal_target =
		skia_overlay_framebuffer
		&& skia_overlay_texture
		&& skia_overlay_stencil_renderbuffer
		&& skia_overlay_width == normal_width
		&& skia_overlay_height == normal_height;
	bool const has_invert_target =
		skia_overlay_invert_framebuffer
		&& skia_overlay_invert_texture
		&& skia_overlay_invert_stencil_renderbuffer
		&& skia_overlay_invert_width == invert_width
		&& skia_overlay_invert_height == invert_height;
	if (has_normal_target && (!need_invert || has_invert_target)) {
		return true;
	}

	auto const& gl = GetCaptureFramebufferFunctions();
	if (!gl.GenFramebuffers
		|| !gl.BindFramebuffer
		|| !gl.DeleteFramebuffers
		|| !gl.FramebufferTexture2D
		|| !gl.CheckFramebufferStatus
		|| !gl.GenRenderbuffers
		|| !gl.BindRenderbuffer
		|| !gl.DeleteRenderbuffers
		|| !gl.RenderbufferStorage
		|| !gl.FramebufferRenderbuffer) {
		DestroySkiaOverlayBacking();
		return false;
	}

	ScopedFramebufferState restore_state(gl);
	skia_overlay_cached_commands.reset();
	skia_overlay_cache_valid = false;

	auto delete_target = [&](unsigned int& framebuffer, unsigned int& texture, unsigned int& stencil) {
		if (framebuffer)
			gl.DeleteFramebuffers(1, &framebuffer);
		if (stencil)
			gl.DeleteRenderbuffers(1, &stencil);
		if (texture)
			glDeleteTextures(1, &texture);
		framebuffer = 0;
		texture = 0;
		stencil = 0;
	};
	if (!has_normal_target) {
		// The cached Ganesh surface wraps the old FBO id; drop it so the next
		// acquire re-wraps the freshly allocated FBO. It must be released before
		// deleting the GL objects which it references.
		skia_overlay_surface.reset();
		skia_overlay_surface_context_generation = 0;
		skia_overlay_surface_framebuffer = 0;
		skia_overlay_surface_width = 0;
		skia_overlay_surface_height = 0;
		delete_target(
			skia_overlay_framebuffer,
			skia_overlay_texture,
			skia_overlay_stencil_renderbuffer);
		skia_overlay_origin_x = 0;
		skia_overlay_origin_y = 0;
		skia_overlay_width = 0;
		skia_overlay_height = 0;
	}
	if (need_invert && !has_invert_target) {
		skia_overlay_invert_surface.reset();
		skia_overlay_invert_surface_context_generation = 0;
		skia_overlay_invert_surface_framebuffer = 0;
		skia_overlay_invert_surface_width = 0;
		skia_overlay_invert_surface_height = 0;
		delete_target(
			skia_overlay_invert_framebuffer,
			skia_overlay_invert_texture,
			skia_overlay_invert_stencil_renderbuffer);
		skia_overlay_invert_origin_x = 0;
		skia_overlay_invert_origin_y = 0;
		skia_overlay_invert_width = 0;
		skia_overlay_invert_height = 0;
	}

	auto allocate_target = [&](
		unsigned int& framebuffer,
		unsigned int& texture,
		unsigned int& stencil,
		int target_width,
		int target_height) {
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(
			GL_TEXTURE_2D,
			0,
			GL_RGBA8,
			target_width,
			target_height,
			0,
			GL_RGBA,
			GL_UNSIGNED_BYTE,
			nullptr);
		glBindTexture(GL_TEXTURE_2D, 0);

		gl.GenFramebuffers(1, &framebuffer);
		gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, framebuffer);
		gl.FramebufferTexture2D(
			GL_FRAMEBUFFER_EXT,
			GL_COLOR_ATTACHMENT0_EXT,
			GL_TEXTURE_2D,
			texture,
			0);

		gl.GenRenderbuffers(1, &stencil);
		gl.BindRenderbuffer(GL_RENDERBUFFER_EXT, stencil);
		// Visual tools never depth-test. A stencil-only attachment avoids making
		// GL_EXT_packed_depth_stencil an accidental requirement on GL 2.x drivers
		// and reduces bounded backing memory without changing Ganesh semantics.
		gl.RenderbufferStorage(GL_RENDERBUFFER_EXT, GL_STENCIL_INDEX8, target_width, target_height);
		gl.FramebufferRenderbuffer(
			GL_FRAMEBUFFER_EXT,
			GL_STENCIL_ATTACHMENT_EXT,
			GL_RENDERBUFFER_EXT,
			stencil);
		gl.BindRenderbuffer(GL_RENDERBUFFER_EXT, 0);

		GLenum const status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT);
		// Consume errors raised by this isolated allocation attempt so a rejected
		// old-driver format cannot poison the same-frame legacy fallback.
		bool allocation_error = false;
		while (glGetError() != GL_NO_ERROR)
			allocation_error = true;
		return !allocation_error
			&& (status == GL_FRAMEBUFFER_COMPLETE || status == GL_FRAMEBUFFER_COMPLETE_EXT);
	};

	if (!has_normal_target
		&& !allocate_target(
			skia_overlay_framebuffer,
			skia_overlay_texture,
			skia_overlay_stencil_renderbuffer,
			normal_width,
			normal_height)) {
		DestroySkiaOverlayBacking();
		return false;
	}
	if (!has_normal_target)
		++allocated_targets;
	if (need_invert
		&& !has_invert_target
		&& !allocate_target(
			skia_overlay_invert_framebuffer,
			skia_overlay_invert_texture,
			skia_overlay_invert_stencil_renderbuffer,
			invert_width,
			invert_height)) {
		DestroySkiaOverlayBacking();
		return false;
	}
	if (need_invert && !has_invert_target)
		++allocated_targets;

	skia_overlay_width = normal_width;
	skia_overlay_height = normal_height;
	if (need_invert) {
		skia_overlay_invert_width = invert_width;
		skia_overlay_invert_height = invert_height;
	}
	return true;
}

void VideoDisplay::DestroySkiaOverlayBacking() noexcept {
	auto const& gl = GetCaptureFramebufferFunctions();

	// Drop the cached Ganesh surfaces first - they reference the GL FBOs below,
	// and Ganesh must release its backend handles before the GL objects are
	// deleted or the next flush on a recycled FBO id could target stale state.
	skia_overlay_surface.reset();
	skia_overlay_invert_surface.reset();
	skia_overlay_surface_context_generation = 0;
	skia_overlay_surface_framebuffer = 0;
	skia_overlay_surface_width = 0;
	skia_overlay_surface_height = 0;
	skia_overlay_invert_surface_context_generation = 0;
	skia_overlay_invert_surface_framebuffer = 0;
	skia_overlay_invert_surface_width = 0;
	skia_overlay_invert_surface_height = 0;

	if (skia_overlay_framebuffer) {
		if (gl.DeleteFramebuffers)
			gl.DeleteFramebuffers(1, &skia_overlay_framebuffer);
		skia_overlay_framebuffer = 0;
	}
	if (skia_overlay_invert_framebuffer) {
		if (gl.DeleteFramebuffers)
			gl.DeleteFramebuffers(1, &skia_overlay_invert_framebuffer);
		skia_overlay_invert_framebuffer = 0;
	}
	if (skia_overlay_stencil_renderbuffer) {
		if (gl.DeleteRenderbuffers)
			gl.DeleteRenderbuffers(1, &skia_overlay_stencil_renderbuffer);
		skia_overlay_stencil_renderbuffer = 0;
	}
	if (skia_overlay_invert_stencil_renderbuffer) {
		if (gl.DeleteRenderbuffers)
			gl.DeleteRenderbuffers(1, &skia_overlay_invert_stencil_renderbuffer);
		skia_overlay_invert_stencil_renderbuffer = 0;
	}
	if (skia_overlay_texture) {
		glDeleteTextures(1, &skia_overlay_texture);
		skia_overlay_texture = 0;
	}
	if (skia_overlay_invert_texture) {
		glDeleteTextures(1, &skia_overlay_invert_texture);
		skia_overlay_invert_texture = 0;
	}

	skia_overlay_width = 0;
	skia_overlay_height = 0;
	skia_overlay_origin_x = 0;
	skia_overlay_origin_y = 0;
	skia_overlay_invert_origin_x = 0;
	skia_overlay_invert_origin_y = 0;
	skia_overlay_invert_width = 0;
	skia_overlay_invert_height = 0;
	skia_overlay_cached_commands.reset();
	skia_overlay_cache_valid = false;
	skia_overlay_cache_context_generation = 0;
	skia_overlay_cache_canvas_width = 0;
	skia_overlay_cache_canvas_height = 0;
	skia_overlay_cache_scale_factor = 0;
	skia_overlay_backing_release_requested = false;
	skia_overlay_backing_context_generation = 0;
}
#endif

void VideoDisplay::PositionVideo() {
	CancelReleaseToolFeedback();
	auto provider = con->project->VideoProvider();
	if (!provider || !IsShownOnScreen()) return;

	int const canvas_width = GetClientSize().GetWidth() * scale_factor;
	int const canvas_height = GetClientSize().GetHeight() * scale_factor;

	AspectRatio arType = con->videoController->GetAspectRatioType();
	double target_aspect_ratio = 0.0;
	if (freeSize) {
		target_aspect_ratio = arType == AspectRatio::Default
			? static_cast<double>(provider->GetWidth()) / provider->GetHeight()
			: con->videoController->GetAspectRatioValue();
	}
	auto const previous_base_viewport = baseViewport;
	baseViewport = BuildVideoDisplayViewportLayout(
		canvas_width,
		canvas_height,
		videoSize.GetWidth(),
		videoSize.GetHeight(),
		freeSize,
		target_aspect_ratio);
	bool const enable_content_transform =
		(!freeSize && contentZoomValue != 1.0) ||
		pan_x != 0.0 ||
		pan_y != 0.0;
	auto layout = BuildVideoDisplayContentLayout(
		baseViewport,
		canvas_height,
		enable_content_transform,
		{ freeSize ? 1.0 : contentZoomValue, pan_x, pan_y });
	ApplyViewportLayout(layout, viewport_left, viewport_width, viewport_bottom, viewport_top, viewport_height);
	bool const base_viewport_changed = scale_factor != baseViewportScaleFactor
		|| baseViewport.viewport_left != previous_base_viewport.viewport_left
		|| baseViewport.viewport_width != previous_base_viewport.viewport_width
		|| baseViewport.viewport_top != previous_base_viewport.viewport_top
		|| baseViewport.viewport_height != previous_base_viewport.viewport_height;
	if (base_viewport_changed) {
		baseViewportScaleFactor = scale_factor;
		BaseViewportChanged();
	}

	if (tool) {
		wxSize client_size = GetClientSize();
		tool->SetCanvasSize(client_size.GetWidth(), client_size.GetHeight());
		tool->SetDisplayArea(viewport_left / scale_factor, viewport_top / scale_factor,
			viewport_width / scale_factor, viewport_height / scale_factor);
	}

	InvalidateSceneCache();
	Render();
}

void VideoDisplay::UpdateSize() {
	auto provider = con->project->VideoProvider();
	if (!provider || !IsShownOnScreen()) return;

	videoSize.Set(provider->GetWidth(), provider->GetHeight());
	videoSize *= zoomValue * GetVideoScaleFactor();
	if (con->videoController->GetAspectRatioType() != AspectRatio::Default)
		videoSize.SetWidth(videoSize.GetHeight() * con->videoController->GetAspectRatioValue());

	wxEventBlocker blocker(this);
	if (freeSize) {
		wxWindow *top = GetParent();
		while (!top->IsTopLevel()) top = top->GetParent();

		contentZoomValue = 1.0;
		pan_x = 0.0;
		pan_y = 0.0;

		wxSize cs = GetClientSize();
		wxSize oldSize = top->GetSize();
		top->SetSize(top->GetSize() + videoSize / scale_factor - cs);
		SetClientSize(cs + top->GetSize() - oldSize);
	}
	else {
		SetMinClientSize(videoSize / scale_factor);
		SetMaxClientSize(videoSize / scale_factor);

		if (auto frame = con->GetUI().frame)
			frame->UpdateEditGridSplitterForContentChange();
		else
			LayoutContainingSizers();
	}

	PositionVideo();
}

void VideoDisplay::LayoutContainingSizers() {
	wxWindow *layout_root = GetGrandParent();
	if (!layout_root)
		layout_root = GetParent();
	if (layout_root)
		layout_root->Layout();
}

void VideoDisplay::RefreshVideoScale() {
	if (tool && toolBar) {
		toolBar->ClearTools();
		tool->SetToolbar(toolBar);
		LayoutContainingSizers();
	}
	if (con->project->VideoProvider())
		UpdateSize();
}

void VideoDisplay::OnSizeEvent(wxSizeEvent& event) {
	auto const client_size = GetClientSize();
	perf_trace::VideoUiDurationScope trace("video_display.resize", client_size.x, client_size.y);
	// Hidden dialogs still receive layout events, but PositionVideo cannot
	// calculate their viewport. Do not cache that size or publish a zero zoom;
	// showing/restoring the window must process the next event at the same size.
	if (con->project->VideoProvider() == nullptr || !IsShownOnScreen() || client_size.x <= 0 || client_size.y <= 0) {
		last_size_event_client_size = wxDefaultSize;
		return;
	}
	// Sizer layout can send size events for unchanged children. Avoid discarding
	// the paused scene and scheduling another buffer swap for those events.
	if (client_size == last_size_event_client_size && scale_factor == baseViewportScaleFactor)
		return;
	last_size_event_client_size = client_size;
	if (freeSize) {
		wxSize newVideoSize = client_size * scale_factor;
		// Host-owned strip/layout changes resize the canvas without changing the
		// user's view. Real window resizes retain the historical reset behavior.
		if (newVideoSize != videoSize && !internalLayoutResizePending && internalLayoutResizeDepth == 0) {
			contentZoomValue = 1.0;
			pan_x = 0.0;
			pan_y = 0.0;
		}
		videoSize = newVideoSize;
		PositionVideo();
		if (auto provider = con->project->VideoProvider(); provider && provider->GetHeight() > 0 && viewport_width > 0 && viewport_height > 0) {
			zoomValue = double(viewport_height) / provider->GetHeight();
			zoomBox->ChangeValue(fmt_wx("%g%%", zoomValue * 100.));
			con->ass->Properties.video_zoom = zoomValue;
		}
	}
	else {
		PositionVideo();
	}
}

void VideoDisplay::OnMouseEvent(wxMouseEvent& event) {
	// FrameMain clears the status bar on a timer while the session stays armed
	// indefinitely, so re-state the mode whenever the pointer comes back.
	if (point_selection && event.Entering())
		con->ShowStatus(from_wx(point_selection->hint));

	// The colour pick's magnifier follows the pointer. LeftDown is left to
	// the pick itself below, and any pick already finished the session (and
	// with it the magnifier) before further events arrive.
	if (zoom_preview && !event.LeftDown() &&
		(event.Moving() || event.Dragging() || event.Entering()))
		zoom_preview->UpdateAt(MapClientToStoragePixel(event.GetPosition()), event.GetPosition());

	if (point_selection && event.LeftDown()) {
		SetFocus();
		double target_width = 0.0;
		double target_height = 0.0;
		if (point_selection->script_coordinates) {
			int script_width = 0;
			int script_height = 0;
			con->ass->GetResolution(script_width, script_height);
			target_width = script_width;
			target_height = script_height;
		}
		else if (auto *provider = con->project->VideoProvider()) {
			target_width = provider->GetWidth();
			target_height = provider->GetHeight();
		}
		auto mapped = MapClientToVideoPoint(event.GetPosition(), target_width, target_height);
		if (mapped) {
			point_selection->points.emplace_back(*mapped);
			if (static_cast<int>(point_selection->points.size()) >=
				point_selection->point_count)
				FinishPointSelection(false, true);
		}
		return;
	}

	if (hotkey::check("Video", con, event))
		return;

	wxPoint pt = event.GetPosition();
	Vector2D current_pos(pt.x, pt.y);

	if (event.ButtonDown())
		SetFocus();

	if (event.MiddleDown())
		last_mouse_pos = current_pos;
	else if (event.Dragging() && event.MiddleIsDown())
		Pan(current_pos - last_mouse_pos);

	last_mouse_pos = mouse_pos = current_pos;

	bool const interaction_was_active = IsVisualToolInteracting();
	if (tool)
		tool->OnMouseEvent(event);
	if (interaction_was_active && !IsVisualToolInteracting())
		FlushVisualToolEditBoxSync();
}

void VideoDisplay::OnMouseLeave(wxMouseEvent& event) {
	mouse_pos = Vector2D();
	if (zoom_preview)
		zoom_preview->UpdateAt(std::nullopt, wxPoint());
	if (tool)
		tool->OnMouseEvent(event);
}

void VideoDisplay::OnMouseWheel(wxMouseEvent& event) {
	if (int wheel = event.GetWheelRotation()) {
		if (ForwardMouseWheelEvent(this, event)) {
			int wheel_steps = wheel / event.GetWheelDelta();
			if (freeSize) {
				SetZoom(AdvanceDetachedVideoZoomByWheel(zoomValue, wheel_steps, zoomBox->GetCount()));
				return;
			}

			int action = ResolveVideoDisplayScrollAction(
				event.CmdDown(),
				event.ShiftDown(),
				scrollAction->GetInt(),
				ctrlScrollAction->GetInt(),
				shiftScrollAction->GetInt());
			int dir = 1;
			bool swap = false;
			switch (action) {
				case SCALE_VIDEO_REV:
					dir = -1;
					[[fallthrough]];
				case SCALE_VIDEO:
					SetWindowZoom(zoomValue + dir * kVideoZoomStep * wheel_steps);
					break;

				case ZOOM_VIDEO_REV:
					dir = -1;
					[[fallthrough]];
				case ZOOM_VIDEO:
				{
					double newZoomValue = contentZoomValue * (1 + dir * kVideoZoomStep * wheel_steps);
					wxPoint scaled_position = event.GetPosition() * scale_factor;
					ZoomAndPan(newZoomValue, GetZoomAnchorPoint(scaled_position), scaled_position);
					break;
				}

				case PAN_VIDEO_SWAP:
					swap = true;
					[[fallthrough]];
				case PAN_VIDEO:
				{
					double distance = 5.0 * wheel_steps;
					Vector2D pan = event.GetWheelAxis() == wxMOUSE_WHEEL_HORIZONTAL ? Vector2D(-distance, 0) : Vector2D(0, distance);
					Pan(swap ? Vector2D(pan.Y(), pan.X()) : pan);
					break;
				}

				case NOTHING:
				default:
					break;
			}
		}
	}
}

void VideoDisplay::OnContextMenu(wxContextMenuEvent&) {
	// Right-click is the usual "get me out of this mode" gesture, and opening a
	// menu mid-session would strand the pick armed behind it.
	if (point_selection) {
		FinishPointSelection(true, true);
		return;
	}
	if (!context_menu) context_menu = menu::GetMenu("video_context", (wxID_HIGHEST + 1) + 9000, con);
	// Show the pointer for the menu even under a tool that hides it, then hand
	// the canvas back; PopupMenu returns once the menu closes.
	SetCursor(wxNullCursor);
	menu::OpenPopupMenu(context_menu.get(), this);
	RefreshCursor();
}

void VideoDisplay::OnKeyDown(wxKeyEvent &event) {
	if (point_selection && event.GetKeyCode() == WXK_ESCAPE) {
		FinishPointSelection(true, true);
		return;
	}
	if (tool && tool->OnKeyDown(event))
		return;
	// Tool-owned context is exact-match only so Default frame-step bindings
	// cannot steal arrow keys while a nudge-capable tool is active.
	if (tool && hotkey::check_exact(tool->GetHotkeyContext(), con, event))
		return;
	hotkey::check("Video", con, event);
}

void VideoDisplay::BeginPointSelection(
	std::string owner,
	int point_count,
	bool script_coordinates,
	std::function<void(std::vector<std::pair<double, double>>, int, bool)> completed,
	wxCursor cursor,
	wxString hint,
	bool live_zoom) {
	if (owner.empty() || point_count <= 0 || !completed)
		throw std::invalid_argument("Invalid video point-selection session");
	if (!con->project->VideoProvider())
		throw std::runtime_error("Video point selection requires an open video");
	FinishPointSelection(true, true);
	if (hint.empty())
		hint = fmt_plural(
			point_count,
			"Click a point in the video; Escape or right-click cancels.",
			"Click %d points in the video; Escape or right-click cancels.",
			point_count);
	point_selection = PointSelectionSession{
		std::move(owner), point_count, script_coordinates, {}, std::move(completed),
		cursor.IsOk() ? cursor : wxCursor(wxCURSOR_CROSS), hint, live_zoom};
	// The magnifier stays hidden until the pointer actually moves over the
	// canvas: at arm time it is usually still over whatever control started
	// the pick.
	if (live_zoom)
		zoom_preview = std::make_unique<VideoColorZoomPreview>(con, this);
	RefreshCursor();
	// The session answers Escape, so it needs the focus the click came from.
	SetFocus();
	con->ShowStatus(from_wx(hint));
}

void VideoDisplay::CancelPointSelection(std::string const& owner, bool notify) {
	if (point_selection && point_selection->owner == owner)
		FinishPointSelection(true, notify);
}

std::optional<std::pair<double, double>> VideoDisplay::MapClientToVideoPoint(
	wxPoint client_pos, double target_width, double target_height) const {
	auto const left = static_cast<double>(viewport_left) / scale_factor;
	auto const top = static_cast<double>(viewport_top) / scale_factor;
	auto const width = static_cast<double>(viewport_width) / scale_factor;
	auto const height = static_cast<double>(viewport_height) / scale_factor;
	if (width <= 0.0 || height <= 0.0 || target_width <= 0.0 || target_height <= 0.0)
		return {};
	if (client_pos.x < left || client_pos.y < top ||
		client_pos.x > left + width || client_pos.y > top + height)
		return {};
	return std::make_pair(
		(client_pos.x - left) * target_width / width,
		(client_pos.y - top) * target_height / height);
}

std::optional<wxPoint> VideoDisplay::MapClientToStoragePixel(wxPoint client_pos) const {
	auto *provider = con ? con->project->VideoProvider() : nullptr;
	if (!provider || provider->GetWidth() <= 0 || provider->GetHeight() <= 0)
		return {};
	auto mapped = MapClientToVideoPoint(
		client_pos,
		static_cast<double>(provider->GetWidth()),
		static_cast<double>(provider->GetHeight()));
	if (!mapped)
		return {};
	auto const storage = aegisub::color_pick::MapDisplayPointToStorage(
		provider->GetFrameGeometry(), mapped->first, mapped->second);
	if (storage.first < 0 || storage.second < 0)
		return {};
	return wxPoint(storage.first, storage.second);
}

std::optional<wxPoint> VideoDisplay::MapScreenToVideoPixel(wxPoint screen_pos) const {
	auto *provider = con ? con->project->VideoProvider() : nullptr;
	if (!provider || provider->GetWidth() <= 0 || provider->GetHeight() <= 0)
		return {};
	auto mapped = MapClientToVideoPoint(
		ScreenToClient(screen_pos),
		static_cast<double>(provider->GetWidth()),
		static_cast<double>(provider->GetHeight()));
	if (!mapped)
		return {};
	return wxPoint(
		static_cast<int>(std::llround(mapped->first)),
		static_cast<int>(std::llround(mapped->second)));
}

void VideoDisplay::RefreshCursor() {
	if (point_selection) {
		SetCursor(point_selection->cursor);
		return;
	}
	auto const idle = tool ? tool->GetIdleCursor() : wxCURSOR_NONE;
	SetCursor(idle == wxCURSOR_NONE ? wxNullCursor : wxCursor(idle));
}

void VideoDisplay::FinishPointSelection(bool cancelled, bool notify) {
	if (!point_selection) return;
	auto session = std::move(*point_selection);
	point_selection.reset();
	// The magnifier must be gone before the completed callback can hand
	// keyboard focus back to the edit box.
	zoom_preview.reset();
	// Hand the canvas back to the tool: resetting to the platform default here
	// would strand a tool that hides the pointer to draw its own crosshair.
	RefreshCursor();
	if (notify) {
		int frame = 0;
		if (con && con->videoController) {
			// GetFrameN() is the last *requested* frame and can run ahead of
			// the picture on screen during playback or scrubbing; consumers
			// sampling pixels want the frame that was actually presented.
			frame = con->videoController->GetPresentedFrameN();
			if (frame < 0)
				frame = con->videoController->GetFrameN();
		}
		session.completed(std::move(session.points), frame, cancelled);
	}
}

void VideoDisplay::SetZoom(double value) {
	if (value == 0) return;
	zoomValue = std::max(value, .125);
	size_t selIndex = zoomValue / .125 - 1;
	if (selIndex < zoomBox->GetCount())
		zoomBox->SetSelection(selIndex);
	zoomBox->ChangeValue(fmt_wx("%g%%", zoomValue * 100.));
	con->ass->Properties.video_zoom = zoomValue;
	UpdateSize();
}

void VideoDisplay::SetZoomFromBox(wxCommandEvent &) {
	int sel = zoomBox->GetSelection();
	if (sel != wxNOT_FOUND) {
		zoomValue = (sel + 1) * .125;
		con->ass->Properties.video_zoom = zoomValue;
		UpdateSize();
	}
}

void VideoDisplay::SetZoomFromBoxText(wxCommandEvent &) {
	wxString strValue = zoomBox->GetValue();
	if (strValue.EndsWith(wxS("%")))
		strValue.RemoveLast();

	double value;
	if (strValue.ToDouble(&value))
		SetZoom(value / 100.);
}

std::unique_ptr<OpenGLText> VideoDisplay::CreateTextRenderer() {
	if (!text_texture_deleter)
		text_texture_deleter = std::make_shared<OpenGLTextTextureDeleter>();
	return std::make_unique<OpenGLText>(text_texture_deleter);
}

void VideoDisplay::SetTool(std::unique_ptr<VisualToolBase> new_tool) {
	ResetToolPresentation();
	CancelReleaseToolFeedback();
	// Defer GL object destruction until the next render has made this canvas
	// current. This releases a possibly full-window invert backing on tool
	// switches without deleting it on every non-invert frame.
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	skia_overlay_backing_release_requested = true;
#endif
	// Set the tool first to prevent repeated initialization from VideoDisplay::Render
	// Legacy text atlases retire into text_texture_deleter; the old tool and its
	// subscriptions still die immediately without touching the current GL context.
	auto const old_tool = reinterpret_cast<std::uintptr_t>(tool.get());
	TraceVideoGlLifecycle("set_tool_before", this, glContext.get(), old_tool, reinterpret_cast<std::uintptr_t>(new_tool.get()));
	tool = std::move(new_tool);
	TraceVideoGlLifecycle("set_tool_after", this, glContext.get(), old_tool, reinterpret_cast<std::uintptr_t>(tool.get()));
	// A session cursor outranks the tool's, so this is a no-op while one is armed.
	RefreshCursor();

	// Hide the tool bar first to eliminate unecessary size changes
	toolBar->Show(false);
	toolBar->ClearTools();
	tool->SetToolbar(toolBar);

	// Update size as the new typesetting tool may have changed the subtoolbar size
	if (!freeSize)
		UpdateSize();
	else {
		// UpdateSize fits the window to the video, which we don't want to do
		LayoutContainingSizers();
		tool->SetCanvasSize(GetClientSize().GetWidth(), GetClientSize().GetHeight());
		tool->SetDisplayArea(viewport_left / scale_factor, viewport_top / scale_factor,
			viewport_width / scale_factor, viewport_height / scale_factor);
		Render();
	}
}

bool VideoDisplay::IsVisualToolInteracting() const noexcept {
	return tool && tool->IsInteracting();
}

void VideoDisplay::FlushVisualToolEditBoxSync() {
	if (auto *edit_box = con->GetUI().subsEditBox)
		edit_box->FlushVisualToolTextSync();
}

void VideoDisplay::Pan(Vector2D delta) {
	if (baseViewport.viewport_height <= 0)
		return;

	auto transform = PanVideoDisplayContent(
		baseViewport,
		{ freeSize ? 1.0 : contentZoomValue, pan_x, pan_y },
		delta * scale_factor);
	contentZoomValue = transform.zoom;
	pan_x = transform.pan_x;
	pan_y = transform.pan_y;
	PositionVideo();
}

Vector2D VideoDisplay::GetZoomAnchorPoint(wxPoint position) const {
	if (freeSize)
		return {};

	return ::GetVideoDisplayZoomAnchorPoint(
		baseViewport,
		{ contentZoomValue, pan_x, pan_y },
		Vector2D(position.x, position.y));
}

void VideoDisplay::ZoomAndPan(double newZoomValue, Vector2D anchorPoint, wxPoint newPosition) {
	if (freeSize)
		return;

	auto transform = ::ZoomVideoDisplayContent(
		baseViewport,
		{ contentZoomValue, pan_x, pan_y },
		newZoomValue,
		anchorPoint,
		Vector2D(newPosition.x, newPosition.y));
	contentZoomValue = transform.zoom;
	pan_x = transform.pan_x;
	pan_y = transform.pan_y;
	PositionVideo();
}

void VideoDisplay::ResetContentZoom() {
	contentZoomValue = 1.0;
	pan_x = 0.0;
	pan_y = 0.0;
	PositionVideo();
}

void VideoDisplay::BeginInternalLayoutResize() {
	++internalLayoutResizeDepth;
	internalLayoutResizePending = true;
	++internalLayoutResizeGeneration;
}

void VideoDisplay::EndInternalLayoutResize() {
	if (internalLayoutResizeDepth <= 0)
		return;

	--internalLayoutResizeDepth;
	if (internalLayoutResizeDepth > 0)
		return;

	std::uint64_t const generation = internalLayoutResizeGeneration;
	wxWeakRef<VideoDisplay> weak_this(this);
	CallAfter([weak_this, generation] {
		if (auto *self = weak_this.get(); self
			&& self->internalLayoutResizeDepth == 0
			&& self->internalLayoutResizeGeneration == generation)
			self->internalLayoutResizePending = false;
	});
}

bool VideoDisplay::ToolIsType(std::type_info const& type) const {
	return tool && typeid(*tool) == type;
}

bool VideoDisplay::CanNudgeTool() const {
	return tool && tool->SupportsNudge() && tool->HasActiveLine();
}

bool VideoDisplay::NudgeTool(Vector2D direction, VisualNudgeMagnitude magnitude) {
	return tool && tool->Nudge(direction, magnitude);
}

bool VideoDisplay::CanNormalizeScaleTool(VisualScaleAxis axis) const {
	auto *scale_tool = dynamic_cast<VisualToolScale *>(tool.get());
	return scale_tool && scale_tool->CanNormalizeScale(axis);
}

bool VideoDisplay::NormalizeScaleTool(VisualScaleAxis axis) {
	auto *scale_tool = dynamic_cast<VisualToolScale *>(tool.get());
	return scale_tool && scale_tool->NormalizeScale(axis);
}

bool VideoDisplay::CanApplyMeasurePerspective() const {
	auto *measure_tool = dynamic_cast<VisualToolMeasure const *>(tool.get());
	return measure_tool && measure_tool->CanApplyPerspective();
}

void VideoDisplay::ApplyMeasurePerspective() {
	if (auto *measure_tool = dynamic_cast<VisualToolMeasure *>(tool.get()))
		measure_tool->ApplyPerspective();
}

bool VideoDisplay::CanRemoveMeasureGuide() const {
	auto *measure_tool = dynamic_cast<VisualToolMeasure const *>(tool.get());
	return measure_tool && measure_tool->GetSubMode() == 0;
}

void VideoDisplay::RemoveMeasureGuide() {
	if (auto *measure_tool = dynamic_cast<VisualToolMeasure *>(tool.get()))
		measure_tool->RemoveSelectedGuide();
}

bool VideoDisplay::SetToolSubMode(int mode) {
	return tool && tool->SetSubMode(mode);
}

int VideoDisplay::GetToolSubMode() const {
	return tool ? tool->GetSubMode() : -1;
}

Vector2D VideoDisplay::GetMousePosition() const {
	return last_mouse_pos ? tool->ToScriptCoords(last_mouse_pos) : last_mouse_pos;
}

void VideoDisplay::Unload() {
	CancelReleaseToolFeedback();
	ResetRenderers();
	bool const context_active = glContext && glContext->IsOK() && SetCurrent(*glContext);
	DestroySceneCache();
#ifdef AEGISUB_WITH_SKIA_VIDEO_TOOLS
	DestroySkiaOverlayBacking();
	if (skia_video_compositor)
		skia_video_compositor->Release(CurrentSkiaGlContextToken());
	skia_video_compositor.reset();
	skia_overlay_surface_provider.reset();
	skia_overlay_text_cache.reset();
#endif
	visualGuideText.reset();
	tool.reset();
	if (text_texture_deleter) {
		if (context_active)
			text_texture_deleter->Drain();
		text_texture_deleter->Abandon();
		text_texture_deleter.reset();
	}
	glContext.reset();
	presentation_configuration_traced = false;
	pending_packet = { };
	has_pending_packet = false;
	pending_packet_deferred_for_visual_interaction = false;
	displayed_packet = { };
	has_displayed_packet = false;
	ResetDisplayedSubtitleScene();
}
