#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

#include <libaegisub/exception.h>

#include "../../src/async_video_provider_host.h"
#include "../../src/ivideo_renderer.h"
#include "../../src/gl_wrap.h"
#include "../../src/gl_text.h"
#include "../../src/legacy_gl_draw.h"
#include "../../src/source_frame.h"
#include "../../src/video_display_layout.h"
#include "../../src/video_frame.h"
#ifdef DrawText
#undef DrawText
#endif
#include "../../src/video_overlay_draw_context_legacy_gl.h"
#include "../../src/video_renderer_error.h"
#include "../../src/video_renderer_opengl.h"
#include "../../src/visual_feature.h"
#include "../../src/visual_tool_drag_snapshot.h"
#include "../../src/visual_tool_transform_overlay.h"
#include "../../src/visual_tool_presentation.h"

#ifdef WITH_LIBPLACEBO
#include "../../src/video_renderer_placebo_gl.h"
#endif

#ifdef _WIN32

#include <libaegisub/log.h>
#include <windows.h>
#include <eh.h>
#ifdef GetMessage
#undef GetMessage
#endif

#include "../../src/video_render_opengl_proc_loader.h"
#ifdef GetMessage
#undef GetMessage
#endif

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <wx/colour.h>
#ifdef DrawText
#undef DrawText
#endif
#ifdef GetMessage
#undef GetMessage
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <libaegisub/scope_exit.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The snapshot adapter requires a real OpenGLText object, but these marker
// checks never measure or draw text. Keep its support linkage narrow.
wxString to_wx(std::string const& text) {
	return wxString::FromUTF8(text);
}

int SmallestPowerOf2(int value) {
	return static_cast<int>(std::bit_ceil(static_cast<unsigned>(value)));
}

// vector2d.cpp references this formatting helper, but this smoke does not
// exercise Vector2D's text formatting paths.
std::string float_to_string(double val) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "%.3f", val);
	std::string result(buffer);
	auto pos = result.find_last_not_of('0');
	if (pos != std::string::npos) {
		if (result[pos] != '.')
			++pos;
		result.erase(pos);
	}
	return result;
}

#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#endif
#ifndef WGL_CONTEXT_MINOR_VERSION_ARB
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#endif
#ifndef WGL_CONTEXT_PROFILE_MASK_ARB
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#endif
#ifndef WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
#endif

namespace {
using WglCreateContextAttribsArbProc = HGLRC(WINAPI*)(HDC, HGLRC, int const*);

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

struct FramebufferFunctions {
	PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = nullptr;
	PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = nullptr;
	PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = nullptr;
	PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = nullptr;
	PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = nullptr;
};

FramebufferFunctions const& GetFramebufferFunctions() {
	static const FramebufferFunctions functions = {
		LoadOptionalProc<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer", "glBindFramebufferEXT"),
		LoadOptionalProc<PFNGLDELETEFRAMEBUFFERSPROC>("glDeleteFramebuffers", "glDeleteFramebuffersEXT"),
		LoadOptionalProc<PFNGLGENFRAMEBUFFERSPROC>("glGenFramebuffers", "glGenFramebuffersEXT"),
		LoadOptionalProc<PFNGLFRAMEBUFFERTEXTURE2DPROC>("glFramebufferTexture2D", "glFramebufferTexture2DEXT"),
		LoadOptionalProc<PFNGLCHECKFRAMEBUFFERSTATUSPROC>("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT"),
	};
	return functions;
}

void SehTranslator(unsigned int code, EXCEPTION_POINTERS*) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "SEH exception 0x%08X", code);
	throw std::runtime_error(buffer);
}

class HiddenGLWindow {
	HWND hwnd = nullptr;
	HDC dc = nullptr;
	HGLRC context = nullptr;
	int width = 0;
	int height = 0;
	int maximum_clip_planes = 0;

	static char const *WindowClassName() {
		return "AegisubDetachedVisualToolsSmokeWindow";
	}

	static void EnsureWindowClassRegistered() {
		static bool registered = false;
		if (registered)
			return;

		WNDCLASSA cls = {};
		cls.lpfnWndProc = DefWindowProcA;
		cls.hInstance = GetModuleHandleA(nullptr);
		cls.lpszClassName = WindowClassName();
		cls.style = CS_OWNDC;

		ATOM atom = RegisterClassA(&cls);
		if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
			throw std::runtime_error("RegisterClassA failed for detached visual tools smoke window.");

		registered = true;
	}

public:
	HiddenGLWindow(int new_width, int new_height)
	: width(new_width)
	, height(new_height) {
		EnsureWindowClassRegistered();

		hwnd = CreateWindowExA(
			0,
			WindowClassName(),
			"Aegisub detached visual tools smoke",
			WS_POPUP,
			0,
			0,
			width,
			height,
			nullptr,
			nullptr,
			GetModuleHandleA(nullptr),
			nullptr);
		if (!hwnd)
			throw std::runtime_error("CreateWindowExA failed for detached visual tools smoke window.");

		dc = GetDC(hwnd);
		if (!dc)
			throw std::runtime_error("GetDC failed for detached visual tools smoke window.");

		PIXELFORMATDESCRIPTOR pfd = {};
		pfd.nSize = sizeof(pfd);
		pfd.nVersion = 1;
		pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		pfd.iPixelType = PFD_TYPE_RGBA;
		pfd.cColorBits = 32;
		pfd.cAlphaBits = 8;
		pfd.cDepthBits = 24;
		pfd.cStencilBits = 8;
		pfd.iLayerType = PFD_MAIN_PLANE;

		int pixel_format = ChoosePixelFormat(dc, &pfd);
		if (!pixel_format)
			throw std::runtime_error("ChoosePixelFormat failed for detached visual tools smoke window.");
		if (!SetPixelFormat(dc, pixel_format, &pfd))
			throw std::runtime_error("SetPixelFormat failed for detached visual tools smoke window.");

		HGLRC legacy_context = wglCreateContext(dc);
		if (!legacy_context)
			throw std::runtime_error("wglCreateContext failed for detached visual tools smoke window.");

		if (!wglMakeCurrent(dc, legacy_context))
			throw std::runtime_error("wglMakeCurrent failed for detached visual tools smoke window.");

		auto create_context_attribs = reinterpret_cast<WglCreateContextAttribsArbProc>(
			wglGetProcAddress("wglCreateContextAttribsARB"));
		if (create_context_attribs) {
			int const attribs[] = {
				WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
				WGL_CONTEXT_MINOR_VERSION_ARB, 3,
				WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB,
				0
			};
			HGLRC modern_context = create_context_attribs(dc, nullptr, attribs);
			if (modern_context) {
				wglMakeCurrent(nullptr, nullptr);
				wglDeleteContext(legacy_context);
				context = modern_context;
				if (!wglMakeCurrent(dc, context))
					throw std::runtime_error("wglMakeCurrent failed for modern detached visual tools smoke window.");
			}
			else {
				context = legacy_context;
			}
		}
		else {
			context = legacy_context;
		}

		glViewport(0, 0, width, height);
		glDisable(GL_DITHER);
		maximum_clip_planes = legacy_gl::QueryMaxClipPlanes();
	}

	[[nodiscard]] int MaximumClipPlanes() const { return maximum_clip_planes; }

	~HiddenGLWindow() {
		if (wglGetCurrentContext() == context)
			wglMakeCurrent(nullptr, nullptr);
		if (context)
			wglDeleteContext(context);
		if (dc && hwnd)
			ReleaseDC(hwnd, dc);
		if (hwnd)
			DestroyWindow(hwnd);
	}

	void MakeCurrent() {
		if (!wglMakeCurrent(dc, context))
			throw std::runtime_error("wglMakeCurrent failed for detached visual tools smoke window.");
	}

	std::vector<unsigned char> ReadBackRgbaTopLeft() {
		MakeCurrent();
		glFinish();
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadBuffer(GL_BACK);

		std::vector<unsigned char> raw(static_cast<std::size_t>(width) * height * 4);
		std::vector<unsigned char> flipped(static_cast<std::size_t>(width) * height * 4);
		glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());

		std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
		for (int y = 0; y < height; ++y) {
			auto const* src = raw.data() + static_cast<std::size_t>(height - 1 - y) * row_bytes;
			auto* dst = flipped.data() + static_cast<std::size_t>(y) * row_bytes;
			std::memcpy(dst, src, row_bytes);
		}

		return flipped;
	}
};

struct BgraScenario {
	VideoFrame storage;
	SourceFrame frame;
};

struct OverlayScenario {
	std::string name;
	int canvas_width = 0;
	int canvas_height = 0;
	int source_width = 0;
	int source_height = 0;
	double target_display_aspect_ratio = 1.0;
	VideoDisplayContentTransform transform = { 1.0, 0.0, 0.0 };
	Vector2D mouse_pos;
	Vector2D script_res;
	Vector2D feature_script_pos;
};

struct PixelColorExpectation {
	std::string label;
	int x = 0;
	int y = 0;
	int min_r = 0;
	int min_g = 0;
	int min_b = 0;
	int max_r = 255;
	int max_g = 255;
	int max_b = 255;
};

struct PixelChangeExpectation {
	std::string label;
	int x = 0;
	int y = 0;
	int min_delta = 32;
};

void FillBgraScenario(BgraScenario& scenario, int width, int height) {
	scenario.storage.width = static_cast<std::size_t>(width);
	scenario.storage.height = static_cast<std::size_t>(height);
	scenario.storage.pitch = static_cast<std::size_t>(width) * 4;
	scenario.storage.flipped = false;
	scenario.storage.data.resize(scenario.storage.pitch * scenario.storage.height);

	for (int y = 0; y < height; ++y) {
		auto* row = scenario.storage.data.data() + static_cast<std::size_t>(y) * scenario.storage.pitch;
		for (int x = 0; x < width; ++x) {
			auto* pixel = row + static_cast<std::size_t>(x) * 4;
			pixel[0] = static_cast<unsigned char>((24 + x * 3 + y) & 0xFF);
			pixel[1] = static_cast<unsigned char>((40 + y * 4) & 0xFF);
			pixel[2] = static_cast<unsigned char>((72 + x * 2) & 0xFF);
			pixel[3] = 255;
		}
	}

	scenario.frame = MakeSourceFrameView(
		scenario.storage,
		SourceFrameColorMetadata { "RGB", "BT.709", "BT.1886", SourceFrameColorRange::Full });
}

BgraScenario MakeBgraScenario(int width, int height) {
	BgraScenario scenario;
	FillBgraScenario(scenario, width, height);
	return scenario;
}

void SetupOverlayProjection(int width, int height) {
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, width, height, 0.0, -1000.0, 1000.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_COLOR_LOGIC_OP);
}

void DrawVisualToolCross(OpenGLWrapper& gl, int canvas_width, int canvas_height, Vector2D mouse_pos) {
	gl.SetInvert();
	gl.SetLineColour(wxColour(255, 255, 255), 1.0f, 1);
	float lines[] = {
		0.0f, mouse_pos.Y(),
		static_cast<float>(canvas_width), mouse_pos.Y(),
		mouse_pos.X(), 0.0f,
		mouse_pos.X(), static_cast<float>(canvas_height)
	};
	gl.DrawLines(2, lines, 4);
	gl.ClearInvert();
}

void DrawVisualToolFeatureMarker(OpenGLWrapper const& gl, Vector2D feature_position) {
	VisualDraggableFeature feature;
	feature.type = DRAG_BIG_SQUARE;
	feature.pos = feature_position;
	feature.Draw(gl);
}

bool PixelMatchesColor(
	std::vector<unsigned char> const& pixels,
	int width,
	PixelColorExpectation const& expectation) {
	auto const sample_index = static_cast<std::size_t>(expectation.y * width + expectation.x) * 4;
	int const sample_r = pixels[sample_index + 0];
	int const sample_g = pixels[sample_index + 1];
	int const sample_b = pixels[sample_index + 2];
	return sample_r >= expectation.min_r && sample_r <= expectation.max_r
		&& sample_g >= expectation.min_g && sample_g <= expectation.max_g
		&& sample_b >= expectation.min_b && sample_b <= expectation.max_b;
}

bool PixelChangedEnough(
	std::vector<unsigned char> const& before,
	std::vector<unsigned char> const& after,
	int width,
	int height,
	PixelChangeExpectation const& expectation) {
	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			int const x = expectation.x + dx;
			int const y = expectation.y + dy;
			if (x < 0 || y < 0 || x >= width || y >= height)
				continue;

			auto const sample_index = static_cast<std::size_t>(y * width + x) * 4;
			int const delta_r = std::abs(static_cast<int>(before[sample_index + 0]) - static_cast<int>(after[sample_index + 0]));
			int const delta_g = std::abs(static_cast<int>(before[sample_index + 1]) - static_cast<int>(after[sample_index + 1]));
			int const delta_b = std::abs(static_cast<int>(before[sample_index + 2]) - static_cast<int>(after[sample_index + 2]));
			if (std::max({ delta_r, delta_g, delta_b }) >= expectation.min_delta)
				return true;
		}
	}
	return false;
}

bool PixelHasVideoSignal(
	std::vector<unsigned char> const& pixels,
	int width,
	int x,
	int y) {
	auto const sample_index = static_cast<std::size_t>(y * width + x) * 4;
	int const sample_r = pixels[sample_index + 0];
	int const sample_g = pixels[sample_index + 1];
	int const sample_b = pixels[sample_index + 2];
	return std::max({ sample_r, sample_g, sample_b }) > 20;
}

void SetupWindowRenderTarget(int width, int height) {
	auto const& gl = GetFramebufferFunctions();
	if (gl.BindFramebuffer)
		gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
	glDrawBuffer(GL_BACK);
	glReadBuffer(GL_BACK);
	glViewport(0, 0, width, height);
	legacy_gl::ResetCompatibilityState();
}

void RequireSnapshotPixel(bool condition, std::string const& message) {
	if (!condition)
		throw std::runtime_error("drag snapshot pixels: " + message);
}

template <class Draw>
std::vector<unsigned char> RenderDragTestPixels(HiddenGLWindow& window, int width, int height, Draw&& draw) {
	window.MakeCurrent();
	SetupWindowRenderTarget(width, height);
	SetupOverlayProjection(width, height);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDisable(GL_DEPTH_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	OpenGLWrapper gl;
	draw(gl);
	auto pixels = window.ReadBackRgbaTopLeft();
	RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "render/readback generated a GL error");
	return pixels;
}

std::vector<unsigned char> RenderLegacyDragGlyph(HiddenGLWindow& window, int width, int height,
												 DraggableFeatureType type, Vector2D position, wxColour const& line_colour, wxColour const& fill_colour) {
	return RenderDragTestPixels(window, width, height, [&](OpenGLWrapper& gl) {
		gl.SetLineColour(line_colour, 1.0f, 1);
		gl.SetFillColour(fill_colour, 0.3f);
		VisualDraggableFeature feature;
		feature.type = type;
		feature.pos = position;
		feature.Draw(gl);
	});
}

std::vector<unsigned char> RenderDragSnapshot(HiddenGLWindow& window, int width, int height,
											  std::shared_ptr<const VisualToolRenderSnapshot> const& snapshot) {
	RequireSnapshotPixel(snapshot != nullptr, "expected a selected snapshot");
	return RenderDragTestPixels(window, width, height, [&](OpenGLWrapper& gl) {
		auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
		OpenGLText text(deleter);
		LegacyVideoOverlayDrawContext target(gl, text);
		snapshot->Draw(target);
	});
}

struct XYOverlayStateSnapshot {
	GLint matrix_mode = 0;
	std::array<GLdouble, 16> modelview{}, projection{}, texture{};
	std::array<GLint, 4> viewport{};
	std::array<GLboolean, 4> colour_mask{};
};

XYOverlayStateSnapshot ReadXYOverlayState() {
	XYOverlayStateSnapshot state;
	glGetIntegerv(GL_MATRIX_MODE, &state.matrix_mode);
	glGetDoublev(GL_MODELVIEW_MATRIX, state.modelview.data());
	glGetDoublev(GL_PROJECTION_MATRIX, state.projection.data());
	glGetDoublev(GL_TEXTURE_MATRIX, state.texture.data());
	glGetIntegerv(GL_VIEWPORT, state.viewport.data());
	glGetBooleanv(GL_COLOR_WRITEMASK, state.colour_mask.data());
	return state;
}

bool SameXYOverlayState(XYOverlayStateSnapshot const& a, XYOverlayStateSnapshot const& b) {
	return a.matrix_mode == b.matrix_mode && a.modelview == b.modelview &&
		   a.projection == b.projection && a.texture == b.texture &&
		   a.viewport == b.viewport && a.colour_mask == b.colour_mask;
}

void PrepareXYOverlayTarget(HiddenGLWindow& window, int width, int height, double depth) {
	window.MakeCurrent();
	SetupWindowRenderTarget(width, height);
	SetupOverlayProjection(width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, width, height, 0.0, -depth, depth);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_DITHER);
	glDisable(GL_FOG);
	glShadeModel(GL_FLAT);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor(64.f / 255.f, 64.f / 255.f, 64.f / 255.f, 64.f / 255.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "XY target setup failed");
}

void DrawXYReferenceLine(float x1, float y1, float alpha1, float x2, float y2, float alpha2) {
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_LINE_SMOOTH);
	glLineWidth(2.f);
	glShadeModel(GL_SMOOTH);
	glBegin(GL_LINES);
	glColor4f(0.f, 1.f, 0.f, alpha1);
	glVertex2f(x1, y1);
	glColor4f(0.f, 1.f, 0.f, alpha2);
	glVertex2f(x2, y2);
	glEnd();
}

std::array<int, 4> XYGreenSample(std::vector<unsigned char> const& pixels, int width, int x, int y) {
	std::array<int, 4> result{};
	int best = -1;
	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			auto const offset = (static_cast<std::size_t>(y + dy) * width + x + dx) * 4;
			if (std::cmp_greater(pixels[offset + 1], best)) {
				best = pixels[offset + 1];
				for (std::size_t channel = 0; channel < result.size(); ++channel)
					result[channel] = pixels[offset + channel];
			}
		}
	}
	return result;
}

void SaveXYRaster(std::string const& name, std::vector<unsigned char> const& pixels) {
	std::ofstream output(name, std::ios::binary);
	output.write(reinterpret_cast<char const *>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
	output.close();
	RequireSnapshotPixel(static_cast<bool>(output), "could not save XY raster " + name);
}

std::vector<unsigned char> ReadXYRaster(HiddenGLWindow& window, HGLRC context, HDC dc) {
	RequireSnapshotPixel(wglGetCurrentContext() == context && wglGetCurrentDC() == dc,
						 "XY drawing changed the current WGL context or DC");
	RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "XY drawing generated a GL error");
	auto pixels = window.ReadBackRgbaTopLeft();
	RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "XY readback generated a GL error");
	return pixels;
}

struct XYReferenceVertex {
	double x, y, w;
	std::array<float, 4> colour;
};

void DrawXYHomogeneousReference(std::array<XYReferenceVertex, 2> const& vertices) {
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_LINE_SMOOTH);
	glLineWidth(2.f);
	glShadeModel(GL_SMOOTH);
	glBegin(GL_LINES);
	for (auto const& vertex : vertices) {
		glColor4fv(vertex.colour.data());
		glVertex4d(vertex.x * vertex.w, vertex.y * vertex.w, 0.0, vertex.w);
	}
	glEnd();
}

std::size_t XYReferenceSampleOffset(std::vector<unsigned char> const& pixels, int width,
									int x, int y, std::size_t colour_channel) {
	std::size_t best_offset = 0;
	int best = -1;
	for (int dy = -1; dy <= 1; ++dy)
		for (int dx = -1; dx <= 1; ++dx) {
			auto const offset = (static_cast<std::size_t>(y + dy) * width + x + dx) * 4;
			if (std::cmp_greater(pixels[offset + colour_channel], best)) {
				best = pixels[offset + colour_channel];
				best_offset = offset;
			}
		}
	return best_offset;
}

bool XYReferenceSampleMatches(std::vector<unsigned char> const& reference,
							  std::vector<unsigned char> const& actual, std::size_t offset) {
	for (std::size_t channel = 0; channel < 4; ++channel)
		if (std::abs(static_cast<int>(reference[offset + channel]) - static_cast<int>(actual[offset + channel])) > 3)
			return false;
	return true;
}

bool ValidateXYPerspectiveBatchNearPlane() {
	constexpr int width = 160, height = 128;
	struct Case {
		char const *name;
		float denominator;
		bool visible;
	};
	std::array<Case, 6> const cases{{{.name = "zero", .denominator = 0.f, .visible = false},
									 {.name = "negative-one", .denominator = -1.f, .visible = false},
									 {.name = "negative-four", .denominator = -4.f, .visible = false},
									 {.name = "outside", .denominator = 1.f - 1.f / 1024.f, .visible = false},
									 {.name = "boundary", .denominator = 1.f, .visible = true},
									 {.name = "inside", .denominator = 1.f + 1.f / 1024.f, .visible = true}}};
	std::array<std::pair<char const *, double>, 2> const hosts{{{"wx", 1000.0}, {"gac", 1.0}}};
	bool passed = true;
	HiddenGLWindow window(width, height);
	for (auto const& [host, depth] : hosts)
		for (auto const& test : cases) {
			std::string const label = std::string(host) + '-' + test.name;
			PrepareXYOverlayTarget(window, width, height, depth);
			HGLRC const context = wglGetCurrentContext();
			HDC const dc = wglGetCurrentDC();
			RequireSnapshotPixel(context && dc, "XY batch boundary has no current context");
			if (test.visible) {
				std::array<XYReferenceVertex, 2> const vertices{{{.x = 40.0, .y = 40.0, .w = test.denominator, .colour = {1.f, 0.f, 0.f, .25f}},
																 {.x = 120.0, .y = 40.0, .w = test.denominator, .colour = {1.f, 0.f, 0.f, .9f}}}};
				DrawXYHomogeneousReference(vertices);
			}
			auto const reference = ReadXYRaster(window, context, dc);
			SaveXYRaster("xy-batch-boundary-" + label + "-reference.rgba", reference);

			PrepareXYOverlayTarget(window, width, height, depth);
			float const coordinate_scale = test.denominator > 0.f ? test.denominator : 1.f;
			std::array<VideoOverlayPerspectiveVertex, 2> const vertices{{{.x = -.016f * coordinate_scale, .y = -.0096f * coordinate_scale, .z = test.denominator - 2500.f, .opacity = .25f},
																		 {.x = .016f * coordinate_scale, .y = -.0096f * coordinate_scale, .z = test.denominator - 2500.f, .opacity = .9f}}};
			RequireSnapshotPixel(vertices[0].x < vertices[1].x && vertices[0].y == vertices[1].y &&
									 vertices[0].z + 2500.f == test.denominator && vertices[1].z == vertices[0].z,
								 "XY batch boundary fixture lost its exact denominator");
			VideoOverlayPerspectiveTransform const transform{
				.origin = {80.f, 64.f},
				.angle_x = 0.f,
				.angle_y = 0.f,
				.angle_z = 0.f,
				.fax = 0.f,
				.fay = 0.f,
				.perspective_z_scale = 1.f};
			OpenGLWrapper gl;
			auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
			OpenGLText text(deleter);
			LegacyVideoOverlayDrawContext target(gl, text);
			std::array<VideoOverlayPerspectiveVertex, 2> const offscreen{{{.x = 10000.f, .y = 10000.f, .z = 0.f, .opacity = 1.f}, {.x = 10100.f, .y = 10000.f, .z = 0.f, .opacity = 1.f}}};
			auto const before = ReadXYOverlayState();
			target.DrawPerspectiveBatches(vertices, offscreen, offscreen, transform, agi::Color(0, 0, 0), agi::Color(255, 0, 0), 2);
			auto const after = ReadXYOverlayState();
			auto const actual = ReadXYRaster(window, context, dc);
			SaveXYRaster("xy-batch-boundary-" + label + "-actual.rgba", actual);
			bool pixels_match = true;
			if (!test.visible)
				pixels_match = actual == reference;
			else {
				std::array<int, 3> reds{};
				for (std::size_t i = 0; i < reds.size(); ++i) {
					int const x = 50 + 30 * static_cast<int>(i);
					auto const offset = XYReferenceSampleOffset(reference, width, x, 40, 0);
					reds[i] = reference[offset];
					pixels_match = XYReferenceSampleMatches(reference, actual, offset) && pixels_match;
				}
				pixels_match = reds[0] > 80 && reds[2] - reds[0] >= 60 && pixels_match;
				for (int x = 43; x <= 117; ++x) {
					bool reference_covered = false, actual_covered = false;
					for (int y = 39; y <= 41; ++y) {
						auto const offset = (static_cast<std::size_t>(y) * width + x) * 4;
						reference_covered = reference[offset] > 80 || reference_covered;
						actual_covered = actual[offset] > 80 || actual_covered;
					}
					pixels_match = reference_covered && actual_covered && pixels_match;
				}
				for (int y = 0; y < height; ++y)
					for (int x = 0; x < width; ++x) {
						if (x >= 38 && x <= 122 && y >= 37 && y <= 43)
							continue;
						auto const offset = (static_cast<std::size_t>(y) * width + x) * 4;
						for (std::size_t channel = 0; channel < 4; ++channel)
							pixels_match = actual[offset + channel] == reference[offset + channel] && pixels_match;
					}
			}
			bool const state_restored = SameXYOverlayState(before, after);
			std::cout << "xy_batch_near_plane/" << label << " pixels_match=" << pixels_match << " state_restored=" << state_restored << '\n';
			passed = state_restored && pixels_match && passed;
		}
	return passed;
}

bool ValidateXYOwnedPass() {
	constexpr int width = 160, height = 128;
	struct Host {
		char const *name;
		double depth;
		bool alpha_write, legacy_context;
	};
	struct Case {
		char const *name;
		float denominator;
		bool visible, crossing, reverse, invalid;
	};
	std::array<Host, 3> const hosts{{
		{.name = "wx", .depth = 1000.0, .alpha_write = true, .legacy_context = true},
		{.name = "gac", .depth = 1.0, .alpha_write = false, .legacy_context = false},
		{.name = "small-depth", .depth = .5, .alpha_write = false, .legacy_context = false},
	}};
	std::array<Case, 6> const cases{{
		{.name = "outside", .denominator = 1.f - 1.f / 1024.f, .visible = false, .crossing = false, .reverse = false, .invalid = false},
		{.name = "boundary", .denominator = 1.f, .visible = true, .crossing = false, .reverse = false, .invalid = true},
		{.name = "inside", .denominator = 1.f + 1.f / 1024.f, .visible = true, .crossing = false, .reverse = false, .invalid = false},
		{.name = "inside-one-and-half", .denominator = 1.5f, .visible = true, .crossing = false, .reverse = false, .invalid = false},
		{.name = "cross-forward", .denominator = 0.f, .visible = true, .crossing = true, .reverse = false, .invalid = false},
		{.name = "cross-reverse", .denominator = 0.f, .visible = true, .crossing = true, .reverse = true, .invalid = false},
	}};
	struct OwnedState {
		XYOverlayStateSnapshot xy;
		GLint shade = 0, blend_src = 0, blend_dst = 0;
		GLfloat line_width = 0;
		std::array<GLfloat, 4> color{};
		std::array<GLboolean, 7> flags{};
	};
	auto read_state = [] {
		OwnedState state;
		state.xy = ReadXYOverlayState();
		glGetIntegerv(GL_SHADE_MODEL, &state.shade);
		glGetIntegerv(GL_BLEND_SRC, &state.blend_src);
		glGetIntegerv(GL_BLEND_DST, &state.blend_dst);
		glGetFloatv(GL_LINE_WIDTH, &state.line_width);
		glGetFloatv(GL_CURRENT_COLOR, state.color.data());
		state.flags[0] = glIsEnabled(GL_CLIP_PLANE0);
		glGetBooleanv(GL_VERTEX_ARRAY, &state.flags[1]);
		glGetBooleanv(GL_COLOR_ARRAY, &state.flags[2]);
		glGetBooleanv(GL_NORMAL_ARRAY, &state.flags[3]);
		glGetBooleanv(GL_TEXTURE_COORD_ARRAY, &state.flags[4]);
		state.flags[5] = glIsEnabled(GL_BLEND);
		state.flags[6] = glIsEnabled(GL_LINE_SMOOTH);
		RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "owned XY state query failed");
		return state;
	};
	auto protected_state = [](OwnedState const& entry, OwnedState const& actual) {
		return SameXYOverlayState(entry.xy, actual.xy) && actual.shade == GL_FLAT &&
			   std::all_of(actual.flags.begin(), actual.flags.begin() + 5, [](GLboolean value) { return value == GL_FALSE; });
	};
	auto same_state = [](OwnedState const& a, OwnedState const& b) {
		return SameXYOverlayState(a.xy, b.xy) && a.shade == b.shade && a.blend_src == b.blend_src &&
			   a.blend_dst == b.blend_dst && a.line_width == b.line_width && a.color == b.color && a.flags == b.flags;
	};
	std::array<VideoOverlayPerspectiveVertex, 2> const axes{{{.x = -40.f, .y = 16.f, .z = 0.f, .opacity = 1.f}, {.x = 40.f, .y = 16.f, .z = 0.f, .opacity = 1.f}}};
	std::array<VideoOverlayPerspectiveVertex, 2> const arrows{{{.x = -40.f, .y = 36.f, .z = 0.f, .opacity = 1.f}, {.x = 40.f, .y = 36.f, .z = 0.f, .opacity = 1.f}}};
	std::array<XYReferenceVertex, 2> const axis_ref{{{.x = 40., .y = 80., .w = 2500., .colour = {0.f, 0.f, 1.f, 1.f}},
													 {.x = 120., .y = 80., .w = 2500., .colour = {0.f, 0.f, 1.f, 1.f}}}};
	std::array<XYReferenceVertex, 2> const arrow_ref{{{.x = 40., .y = 100., .w = 2500., .colour = {0.f, 0.f, 1.f, 1.f}},
													  {.x = 120., .y = 100., .w = 2500., .colour = {0.f, 0.f, 1.f, 1.f}}}};
	VideoOverlayPerspectiveTransform const transform{.origin = {80.f, 64.f}, .angle_x = 0.f, .angle_y = 0.f, .angle_z = 0.f, .fax = 0.f, .fay = 0.f, .perspective_z_scale = 1.f};
	bool passed = true;
	for (auto const& host : hosts)
		for (auto const& test : cases) {
			std::string const label = std::string(host.name) + '-' + test.name;
			HiddenGLWindow window(width, height);
			PrepareXYOverlayTarget(window, width, height, host.depth);
			HGLRC const context = wglGetCurrentContext();
			HDC const dc = wglGetCurrentDC();
			RequireSnapshotPixel(context && dc, "owned XY fixture has no current context");
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, host.alpha_write ? GL_TRUE : GL_FALSE);
			auto const reference_state = ReadXYOverlayState();
			std::array<VideoOverlayPerspectiveVertex, 2> vertices{};
			std::array<XYReferenceVertex, 2> grid_ref{};
			if (test.crossing) {
				VideoOverlayPerspectiveVertex const outside{.x = .008f, .y = -.0048f, .z = -2499.5f, .opacity = .9f};
				VideoOverlayPerspectiveVertex const inside{.x = -.032f, .y = -.0192f, .z = -2498.f, .opacity = .1f};
				vertices = test.reverse ? std::array{inside, outside} : std::array{outside, inside};
				grid_ref = {{{.x = 40., .y = 40., .w = 2., .colour = {1.f, 0.f, 0.f, .1f}},
							 {.x = 200. / 3., .y = 40., .w = 1., .colour = {1.f, 0.f, 0.f, 19.f / 30.f}}}};
			}
			else {
				float const d = test.denominator;
				vertices = {{{.x = -.016f * d, .y = -.0096f * d, .z = d - 2500.f, .opacity = .25f},
							 {.x = .016f * d, .y = -.0096f * d, .z = d - 2500.f, .opacity = .9f}}};
				grid_ref = {{{.x = 40., .y = 40., .w = d, .colour = {1.f, 0.f, 0.f, .25f}},
							 {.x = 120., .y = 40., .w = d, .colour = {1.f, 0.f, 0.f, .9f}}}};
			}
			auto draw_reference = [&] {
				if (test.visible) DrawXYHomogeneousReference(grid_ref);
				DrawXYHomogeneousReference(axis_ref);
				DrawXYHomogeneousReference(arrow_ref);
			};
			draw_reference();
			auto const reference_first = ReadXYRaster(window, context, dc);
			draw_reference();
			DrawXYReferenceLine(20.f, 112.f, 1.f, 140.f, 112.f, 1.f);
			auto const reference_second = ReadXYRaster(window, context, dc);
			SaveXYRaster("xy-owned-" + label + "-reference.rgba", reference_second);

			PrepareXYOverlayTarget(window, width, height, host.depth);
			std::array<GLdouble, 4> const equation{.25, .5, 0., .75};
			std::array<GLdouble, 4> original_plane{};
			glClipPlane(GL_CLIP_PLANE0, equation.data());
			glDisable(GL_CLIP_PLANE0);
			glGetClipPlane(GL_CLIP_PLANE0, original_plane.data());
			RequireSnapshotPixel(glGetError() == GL_NO_ERROR && original_plane == equation,
								 "owned XY plane sentinel was not established");
			glShadeModel(GL_SMOOTH);
			OpenGLWindowOverlayPass pass(width, height, width, height, host.depth, host.alpha_write, window.MaximumClipPlanes());
			OpenGLWrapper gl;
			auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
			OpenGLText text(deleter);
			std::unique_ptr<VideoOverlayDrawContext> target;
			if (host.legacy_context)
				target = std::make_unique<LegacyVideoOverlayDrawContext>(gl, text, pass);
			else
				target = std::make_unique<OpenGLVideoOverlayDrawContext>(gl, pass);
			auto const entry = read_state();
			auto const expected_mask = std::array<GLboolean, 4>{GL_TRUE, GL_TRUE, GL_TRUE,
																static_cast<GLboolean>(host.alpha_write ? GL_TRUE : GL_FALSE)};
			bool const entry_valid = SameXYOverlayState(reference_state, entry.xy) && entry.xy.colour_mask == expected_mask &&
									 protected_state(entry, entry);
			auto draw = [&] {
				target->DrawPerspectiveBatches(vertices, axes, arrows, transform, agi::Color(0, 0, 255), agi::Color(255, 0, 0), 2);
			};
			draw();
			auto const first_state = read_state();
			auto const first = ReadXYRaster(window, context, dc);
			draw();
			auto const second_state = read_state();
			target->SetLineColour(agi::Color(0, 255, 0), 1.f, 2);
			target->DrawLine(Vector2D(20.f, 112.f), Vector2D(140.f, 112.f));
			auto const after_line_state = read_state();
			auto const second = ReadXYRaster(window, context, dc);
			SaveXYRaster("xy-owned-" + label + "-first.rgba", first);
			SaveXYRaster("xy-owned-" + label + "-second.rgba", second);
			bool state_ok = entry_valid && protected_state(entry, first_state) && protected_state(entry, second_state) &&
							protected_state(entry, after_line_state);
			bool pixels_ok = true;
			if (!test.visible) {
				for (int y = 37; y <= 43; ++y)
					for (int x = 38; x <= 122; ++x) {
						auto const offset = (static_cast<std::size_t>(y) * width + x) * 4;
						for (std::size_t channel = 0; channel < 4; ++channel)
							pixels_ok = reference_first[offset + channel] == first[offset + channel] &&
										reference_second[offset + channel] == second[offset + channel] && pixels_ok;
					}
			}
			else {
				int const x0 = test.crossing ? 45 : 50;
				int const x1 = test.crossing ? 62 : 110;
				for (int x : {x0, x1}) {
					auto const a = XYReferenceSampleOffset(reference_first, width, x, 40, 0);
					auto const b = XYReferenceSampleOffset(reference_second, width, x, 40, 0);
					pixels_ok = XYReferenceSampleMatches(reference_first, first, a) &&
								XYReferenceSampleMatches(reference_second, second, b) && pixels_ok;
				}
				auto const near_sample = XYReferenceSampleOffset(reference_first, width, x0, 40, 0);
				auto const far_sample = XYReferenceSampleOffset(reference_first, width, x1, 40, 0);
				pixels_ok = static_cast<int>(reference_first[far_sample]) - reference_first[near_sample] >= (test.crossing ? 40 : 60) &&
							second[far_sample] > first[far_sample] && pixels_ok;
			}
			for (auto const [y, channel] : {std::pair{80, std::size_t{2}}, std::pair{100, std::size_t{2}},
											std::pair{112, std::size_t{1}}}) {
				auto const offset = XYReferenceSampleOffset(reference_second, width, 80, y, channel);
				pixels_ok = reference_second[offset + channel] >= 200 &&
							XYReferenceSampleMatches(reference_second, second, offset) && pixels_ok;
			}
			bool invalid_ok = true;
			if (test.invalid) {
				glClear(GL_COLOR_BUFFER_BIT);
				RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "owned XY invalid-case clear failed");
				auto invalid_arrows = std::array{arrows[0], arrows[1], arrows[0], arrows[1]};
				invalid_arrows.back().x = std::numeric_limits<float>::quiet_NaN();
				auto const before_invalid = read_state();
				auto const before_pixels = ReadXYRaster(window, context, dc);
				std::string error;
				try {
					target->DrawPerspectiveBatches(vertices, axes, invalid_arrows, transform, agi::Color(0, 0, 255), agi::Color(255, 0, 0), 2);
				}
				catch (agi::EnvironmentError const& failure) {
					error = failure.GetMessage();
				}
				auto const after_invalid = read_state();
				auto const after_pixels = ReadXYRaster(window, context, dc);
				invalid_ok = !error.empty() && same_state(before_invalid, after_invalid) && before_pixels == after_pixels;
				target->DrawLine(Vector2D(20.f, 112.f), Vector2D(140.f, 112.f));
				auto const paint_witness = ReadXYRaster(window, context, dc);
				auto const paint_offset = XYReferenceSampleOffset(paint_witness, width, 80, 112, 1);
				invalid_ok = paint_witness[paint_offset + 1] >= 200 && paint_witness[paint_offset] <= 80 && paint_witness[paint_offset + 2] <= 80 && invalid_ok;
				SaveXYRaster("xy-owned-" + label + "-invalid-paint.rgba", paint_witness);
			}
			std::array<GLdouble, 4> const polluted_equation{0., 0., 1., .5};
			std::array<GLdouble, 4> polluted_plane{};
			glClipPlane(GL_CLIP_PLANE0, polluted_equation.data());
			glGetClipPlane(GL_CLIP_PLANE0, polluted_plane.data());
			RequireSnapshotPixel(glGetError() == GL_NO_ERROR && polluted_plane == polluted_equation && polluted_plane != original_plane,
								 "owned XY normal finish pollution setup failed");
			pass.Finish();
			GLint finished_shade = 0;
			glGetIntegerv(GL_SHADE_MODEL, &finished_shade);
			std::array<GLboolean, 4> finished_mask{};
			glGetBooleanv(GL_COLOR_WRITEMASK, finished_mask.data());
			bool const finished_clip_disabled = glIsEnabled(GL_CLIP_PLANE0) == GL_FALSE;
			bool const finished_ok = glGetError() == GL_NO_ERROR && finished_clip_disabled && finished_shade == GL_FLAT &&
									 finished_mask == std::array<GLboolean, 4>{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
			std::cout << "xy_owned_pass/" << label << " pixels=" << pixels_ok << " state=" << state_ok
					  << " invalid=" << invalid_ok << " finish=" << finished_ok << '\n';
			passed = pixels_ok && state_ok && invalid_ok && finished_ok && passed;
		}
	return passed;
}

bool ValidateXYOwnedPassStackBoundaries() {
	constexpr int width = 160, height = 128;
	struct Host {
		char const *name;
		double depth, logical_width, logical_height;
		bool alpha_write, legacy_context;
	};
	std::array<Host, 2> const hosts{{{.name = "wx", .depth = 1000.0, .logical_width = width, .logical_height = height, .alpha_write = true, .legacy_context = true},
									 {.name = "gac", .depth = 1.0, .logical_width = 173.25, .logical_height = 91.75, .alpha_write = false, .legacy_context = false}}};
	struct StackCase {
		char const *name;
		GLenum depth, maximum, matrix_mode;
		GLint free_entries;
	};
	std::array<StackCase, 3> const cases{{{.name = "attribute-full", .depth = GL_ATTRIB_STACK_DEPTH, .maximum = GL_MAX_ATTRIB_STACK_DEPTH, .matrix_mode = 0, .free_entries = 0},
										  {.name = "attribute-one-free", .depth = GL_ATTRIB_STACK_DEPTH, .maximum = GL_MAX_ATTRIB_STACK_DEPTH, .matrix_mode = 0, .free_entries = 1},
										  {.name = "projection-full", .depth = GL_PROJECTION_STACK_DEPTH, .maximum = GL_MAX_PROJECTION_STACK_DEPTH, .matrix_mode = GL_PROJECTION, .free_entries = 0}}};
	struct State {
		XYOverlayStateSnapshot xy;
		std::array<GLdouble, 4> plane{};
		std::array<GLboolean, 4> arrays{};
		std::array<GLint, 4> depths{};
		GLint shade = 0;
		GLboolean clip_enabled = GL_FALSE;
	};
	auto read_depths = [] {
		std::array<GLint, 4> depths{};
		glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &depths[0]);
		glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &depths[1]);
		glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &depths[2]);
		glGetIntegerv(GL_CLIENT_ATTRIB_STACK_DEPTH, &depths[3]);
		RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "owned XY stack depth query failed");
		return depths;
	};
	auto read_state = [&] {
		State state;
		state.xy = ReadXYOverlayState();
		glGetClipPlane(GL_CLIP_PLANE0, state.plane.data());
		glGetIntegerv(GL_SHADE_MODEL, &state.shade);
		glGetBooleanv(GL_VERTEX_ARRAY, &state.arrays[0]);
		glGetBooleanv(GL_COLOR_ARRAY, &state.arrays[1]);
		glGetBooleanv(GL_NORMAL_ARRAY, &state.arrays[2]);
		glGetBooleanv(GL_TEXTURE_COORD_ARRAY, &state.arrays[3]);
		state.clip_enabled = glIsEnabled(GL_CLIP_PLANE0);
		state.depths = read_depths();
		return state;
	};
	auto batch_state_preserved = [](State const& entry, State const& actual) {
		return SameXYOverlayState(entry.xy, actual.xy) && entry.shade == actual.shade &&
			   entry.arrays == actual.arrays && actual.clip_enabled == GL_FALSE;
	};
	bool passed = true;
	for (auto const& host : hosts)
		for (auto const& test : cases) {
			std::string const label = std::string(host.name) + '-' + test.name;
			HiddenGLWindow window(width, height);
			PrepareXYOverlayTarget(window, width, height, host.depth);
			HGLRC const context = wglGetCurrentContext();
			HDC const dc = wglGetCurrentDC();
			RequireSnapshotPixel(context && dc, "owned XY stack boundary fixture has no context");
			glMatrixMode(GL_PROJECTION);
			glLoadIdentity();
			glOrtho(0.0, host.logical_width, host.logical_height, 0.0, -host.depth, host.depth);
			glMatrixMode(GL_MODELVIEW);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, host.alpha_write ? GL_TRUE : GL_FALSE);
			double const sx = host.logical_width / width, sy = host.logical_height / height;
			std::array<XYReferenceVertex, 2> const grid_ref{{{.x = 40. * sx, .y = 40. * sy, .w = 2., .colour = {1.f, 0.f, 0.f, .1f}},
															 {.x = (200. / 3.) * sx, .y = 40. * sy, .w = 1., .colour = {1.f, 0.f, 0.f, 19.f / 30.f}}}};
			std::array<XYReferenceVertex, 2> const axis_ref{{{.x = 40. * sx, .y = 80. * sy, .w = 2500., .colour = {0.f, 0.f, 1.f, 1.f}},
															 {.x = 120. * sx, .y = 80. * sy, .w = 2500., .colour = {0.f, 0.f, 1.f, 1.f}}}};
			std::array<XYReferenceVertex, 2> const arrow_ref{{{.x = 40. * sx, .y = 100. * sy, .w = 2500., .colour = {0.f, 0.f, 1.f, 1.f}},
															  {.x = 120. * sx, .y = 100. * sy, .w = 2500., .colour = {0.f, 0.f, 1.f, 1.f}}}};
			auto draw_reference = [&] {
				DrawXYHomogeneousReference(grid_ref);
				DrawXYHomogeneousReference(axis_ref);
				DrawXYHomogeneousReference(arrow_ref);
			};
			draw_reference();
			auto const reference_first = ReadXYRaster(window, context, dc);
			draw_reference();
			auto const reference_second = ReadXYRaster(window, context, dc);
			SaveXYRaster("xy-owned-stack-" + label + "-reference.rgba", reference_second);

			PrepareXYOverlayTarget(window, width, height, host.depth);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			std::array<GLdouble, 4> const sentinel{.25, .5, 0., .75};
			glClipPlane(GL_CLIP_PLANE0, sentinel.data());
			glDisable(GL_CLIP_PLANE0);
			GLint baseline = -1, limit = -1;
			glGetIntegerv(test.depth, &baseline);
			glGetIntegerv(test.maximum, &limit);
			RequireSnapshotPixel(glGetError() == GL_NO_ERROR && baseline >= 0 && limit > baseline &&
									 limit <= 1024 && limit - test.free_entries >= baseline,
								 "owned XY stack boundary fixture cannot establish the requested depth");
			auto const initial_depths = read_depths();
			auto restore = [&]() noexcept {
				if (wglGetCurrentContext() != context || wglGetCurrentDC() != dc) std::terminate();
				if (test.matrix_mode) glMatrixMode(test.matrix_mode);
				GLint depth = -1;
				glGetIntegerv(test.depth, &depth);
				if (glGetError() != GL_NO_ERROR || depth < baseline || depth > limit) std::terminate();
				for (; depth > baseline; --depth) {
					if (test.matrix_mode)
						glPopMatrix();
					else
						glPopAttrib();
					if (glGetError() != GL_NO_ERROR) std::terminate();
				}
				glMatrixMode(GL_MODELVIEW);
				if (glGetError() != GL_NO_ERROR) std::terminate();
			};
			auto cleanup = agi::make_scope_exit([&] { restore(); });
			if (test.matrix_mode) glMatrixMode(test.matrix_mode);
			for (GLint current = baseline; current < limit - test.free_entries; ++current) {
				if (test.matrix_mode)
					glPushMatrix();
				else
					glPushAttrib(GL_TRANSFORM_BIT | GL_LIGHTING_BIT);
				RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "owned XY stack boundary push failed");
			}
			glMatrixMode(GL_MODELVIEW);
			GLint prepared_depth = -1;
			glGetIntegerv(test.depth, &prepared_depth);
			RequireSnapshotPixel(glGetError() == GL_NO_ERROR && prepared_depth == limit - test.free_entries,
								 "owned XY stack boundary has the wrong depth");
			auto const before_pass_depths = read_depths();
			glShadeModel(GL_FLAT);
			OpenGLWindowOverlayPass pass(width, height, host.logical_width, host.logical_height, host.depth, host.alpha_write, window.MaximumClipPlanes());
			OpenGLWrapper gl;
			auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
			OpenGLText text(deleter);
			std::unique_ptr<VideoOverlayDrawContext> target;
			if (host.legacy_context)
				target = std::make_unique<LegacyVideoOverlayDrawContext>(gl, text, pass);
			else
				target = std::make_unique<OpenGLVideoOverlayDrawContext>(gl, pass);
			auto const entry = read_state();
			std::array<GLdouble, 16> const expected_projection{{2. / host.logical_width, 0., 0., 0., 0., -2. / host.logical_height, 0., 0.,
																0., 0., -1. / host.depth, 0., -1., 1., 0., 1.}};
			bool const projection_ok = std::equal(entry.xy.projection.begin(), entry.xy.projection.end(),
												  expected_projection.begin(), [](double a, double b) { return std::abs(a - b) <= 1e-6; });
			auto const expected_mask = std::array<GLboolean, 4>{GL_TRUE, GL_TRUE, GL_TRUE,
																static_cast<GLboolean>(host.alpha_write ? GL_TRUE : GL_FALSE)};
			bool const entry_valid = projection_ok && entry.xy.matrix_mode == GL_MODELVIEW && entry.xy.colour_mask == expected_mask &&
									 entry.clip_enabled == GL_FALSE && entry.shade == GL_FLAT &&
									 entry.arrays == std::array<GLboolean, 4>{GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE};
			auto const fx = static_cast<float>(sx), fy = static_cast<float>(sy);
			std::array<VideoOverlayPerspectiveVertex, 2> const grid{{{.x = .008f * fx, .y = -.0048f * fy, .z = -2499.5f, .opacity = .9f},
																	 {.x = -.032f * fx, .y = -.0192f * fy, .z = -2498.f, .opacity = .1f}}};
			std::array<VideoOverlayPerspectiveVertex, 2> const axes{{{.x = -40.f * fx, .y = 16.f * fy, .z = 0.f, .opacity = 1.f},
																	 {.x = 40.f * fx, .y = 16.f * fy, .z = 0.f, .opacity = 1.f}}};
			std::array<VideoOverlayPerspectiveVertex, 2> const arrows{{{.x = -40.f * fx, .y = 36.f * fy, .z = 0.f, .opacity = 1.f},
																	   {.x = 40.f * fx, .y = 36.f * fy, .z = 0.f, .opacity = 1.f}}};
			VideoOverlayPerspectiveTransform const transform{.origin = {80.f * fx, 64.f * fy}, .angle_x = 0.f, .angle_y = 0.f, .angle_z = 0.f, .fax = 0.f, .fay = 0.f, .perspective_z_scale = 1.f};
			auto draw = [&] {
				target->DrawPerspectiveBatches(grid, axes, arrows, transform, agi::Color(0, 0, 255), agi::Color(255, 0, 0), 2);
			};
			draw();
			auto const first_state = read_state();
			auto const first = ReadXYRaster(window, context, dc);
			draw();
			auto const second_state = read_state();
			auto const second = ReadXYRaster(window, context, dc);
			SaveXYRaster("xy-owned-stack-" + label + "-first.rgba", first);
			SaveXYRaster("xy-owned-stack-" + label + "-second.rgba", second);
			bool pixels_ok = true;
			for (int x : {45, 62}) {
				auto const a = XYReferenceSampleOffset(reference_first, width, x, 40, 0);
				auto const b = XYReferenceSampleOffset(reference_second, width, x, 40, 0);
				pixels_ok = XYReferenceSampleMatches(reference_first, first, a) &&
							XYReferenceSampleMatches(reference_second, second, b) && pixels_ok;
			}
			auto const near_sample = XYReferenceSampleOffset(reference_first, width, 45, 40, 0);
			auto const far_sample = XYReferenceSampleOffset(reference_first, width, 62, 40, 0);
			pixels_ok = reference_first[near_sample] > 64 &&
						static_cast<int>(reference_first[far_sample]) - reference_first[near_sample] >= 40 &&
						second[far_sample] > first[far_sample] &&
						(host.alpha_write ? reference_first[far_sample + 3] > 64 : reference_first[far_sample + 3] == 64) && pixels_ok;
			for (int x : {90, 110}) {
				auto const offset = XYReferenceSampleOffset(reference_first, width, x, 40, 0);
				pixels_ok = reference_first[offset] == 64 && XYReferenceSampleMatches(reference_first, first, offset) &&
							XYReferenceSampleMatches(reference_second, second, offset) && pixels_ok;
			}
			for (int y : {80, 100}) {
				auto const offset = XYReferenceSampleOffset(reference_second, width, 80, y, 2);
				pixels_ok = reference_second[offset + 2] >= 200 && XYReferenceSampleMatches(reference_second, second, offset) && pixels_ok;
			}
			pass.Finish();
			auto const finished = read_state();
			auto expected_finished = entry.xy;
			expected_finished.colour_mask = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
			bool const batch_state_ok = entry_valid && batch_state_preserved(entry, first_state) && batch_state_preserved(entry, second_state);
			bool const finished_ok = SameXYOverlayState(expected_finished, finished.xy) &&
									 finished.clip_enabled == GL_FALSE && finished.shade == entry.shade && finished.arrays == entry.arrays &&
									 finished.depths == before_pass_depths;
			restore();
			cleanup.release();
			bool const cleaned = initial_depths == read_depths();
			std::cout << "xy_owned_stack_boundaries/" << label << " pixels=" << pixels_ok << " state=" << batch_state_ok
					  << " finish=" << finished_ok << " cleaned=" << cleaned << '\n';
			passed = pixels_ok && batch_state_ok && finished_ok && cleaned && passed;
		}
	return passed;
}

bool ValidateXYOwnedPassFinishError() {
	constexpr int width = 160, height = 128;
	struct Host {
		char const *name;
		double depth;
		bool alpha_write, legacy_context;
	};
	std::array<Host, 2> const hosts{{{.name = "wx", .depth = 1000.0, .alpha_write = true, .legacy_context = true}, {.name = "gac", .depth = 1.0, .alpha_write = false, .legacy_context = false}}};
	std::array<GLdouble, 16> const identity{{1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.}};
	std::array<VideoOverlayPerspectiveVertex, 2> const line{{{.x = -40.f, .y = -24.f, .z = 0.f, .opacity = 1.f}, {.x = 40.f, .y = -24.f, .z = 0.f, .opacity = 1.f}}};
	std::array<VideoOverlayPerspectiveVertex, 2> const offscreen{{{.x = 10000.f, .y = 10000.f, .z = 0.f, .opacity = 1.f}, {.x = 10100.f, .y = 10000.f, .z = 0.f, .opacity = 1.f}}};
	VideoOverlayPerspectiveTransform const transform{.origin = {80.f, 64.f}, .angle_x = 0.f, .angle_y = 0.f, .angle_z = 0.f, .fax = 0.f, .fay = 0.f, .perspective_z_scale = 1.f};
	bool passed = true;
	for (auto const& host : hosts) {
		HiddenGLWindow window(width, height);
		PrepareXYOverlayTarget(window, width, height, host.depth);
		HGLRC const context = wglGetCurrentContext();
		HDC const dc = wglGetCurrentDC();
		RequireSnapshotPixel(context && dc, "owned XY finish error fixture has no current context");
		std::array<GLdouble, 4> const sentinel{.25, .5, 0., .75};
		std::array<GLdouble, 4> original_plane{};
		glClipPlane(GL_CLIP_PLANE0, sentinel.data());
		glDisable(GL_CLIP_PLANE0);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glGetClipPlane(GL_CLIP_PLANE0, original_plane.data());
		bool const original_clip_disabled = glIsEnabled(GL_CLIP_PLANE0) == GL_FALSE;
		auto const original = ReadXYOverlayState();
		RequireSnapshotPixel(glGetError() == GL_NO_ERROR && original_plane == sentinel && original_clip_disabled &&
								 original.modelview == identity && original.matrix_mode == GL_MODELVIEW &&
								 original.colour_mask == std::array<GLboolean, 4>{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE},
							 "owned XY finish error sentinel setup failed");

		bool reached_finish = false, returned_from_finish = false;
		std::string finish_error;
		try {
			glShadeModel(GL_SMOOTH);
			OpenGLWindowOverlayPass pass(width, height, width, height, host.depth, host.alpha_write, window.MaximumClipPlanes());
			OpenGLWrapper gl;
			auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
			OpenGLText text(deleter);
			std::unique_ptr<VideoOverlayDrawContext> target;
			if (host.legacy_context)
				target = std::make_unique<LegacyVideoOverlayDrawContext>(gl, text, pass);
			else
				target = std::make_unique<OpenGLVideoOverlayDrawContext>(gl, pass);
			std::array<GLdouble, 4> active_plane{};
			glGetClipPlane(GL_CLIP_PLANE0, active_plane.data());
			bool const clip_disabled = glIsEnabled(GL_CLIP_PLANE0) == GL_FALSE;
			GLint entry_shade = 0;
			std::array<GLboolean, 2> entry_arrays{};
			glGetIntegerv(GL_SHADE_MODEL, &entry_shade);
			glGetBooleanv(GL_VERTEX_ARRAY, &entry_arrays[0]);
			glGetBooleanv(GL_COLOR_ARRAY, &entry_arrays[1]);
			auto const entry = ReadXYOverlayState();
			auto const expected_mask = std::array<GLboolean, 4>{GL_TRUE, GL_TRUE, GL_TRUE,
																static_cast<GLboolean>(host.alpha_write ? GL_TRUE : GL_FALSE)};
			RequireSnapshotPixel(glGetError() == GL_NO_ERROR && clip_disabled &&
									 entry.modelview == identity && entry.projection == original.projection &&
									 entry.matrix_mode == GL_MODELVIEW && entry.colour_mask == expected_mask && entry_shade == GL_FLAT &&
									 entry_arrays == std::array<GLboolean, 2>{GL_FALSE, GL_FALSE},
								 "owned XY finish error pass setup failed");
			target->DrawPerspectiveBatches(line, offscreen, offscreen, transform, agi::Color(0, 0, 0), agi::Color(255, 0, 0), 2);
			glGetClipPlane(GL_CLIP_PLANE0, active_plane.data());
			bool const batch_clip_disabled = glIsEnabled(GL_CLIP_PLANE0) == GL_FALSE;
			RequireSnapshotPixel(glGetError() == GL_NO_ERROR && batch_clip_disabled,
								 "owned XY finish error valid batch did not disable its plane");
			auto const pixels = ReadXYRaster(window, context, dc);
			SaveXYRaster(std::string("xy-owned-finish-error-") + host.name + ".rgba", pixels);
			auto const sample = XYReferenceSampleOffset(pixels, width, 80, 40, 0);
			RequireSnapshotPixel(pixels[sample] >= 200 && pixels[sample + 1] <= 80 && pixels[sample + 2] <= 80,
								 "owned XY finish error valid batch did not draw its red line");
			glMatrixMode(GL_PROJECTION);
			glLoadIdentity();
			glTranslatef(.125f, .25f, 0.f);
			glMatrixMode(GL_MODELVIEW);
			glLoadIdentity();
			glTranslatef(3.f, 5.f, 0.f);
			glMatrixMode(GL_TEXTURE);
			glShadeModel(GL_SMOOTH);
			glEnableClientState(GL_VERTEX_ARRAY);
			glEnableClientState(GL_COLOR_ARRAY);
			std::array<GLdouble, 4> const polluted_equation{0., 0., 1., .5};
			std::array<GLdouble, 4> polluted_plane{};
			glClipPlane(GL_CLIP_PLANE0, polluted_equation.data());
			glGetClipPlane(GL_CLIP_PLANE0, polluted_plane.data());
			glEnable(GL_CLIP_PLANE0);
			auto const polluted = ReadXYOverlayState();
			GLint polluted_shade = 0;
			std::array<GLboolean, 2> polluted_arrays{};
			glGetIntegerv(GL_SHADE_MODEL, &polluted_shade);
			glGetBooleanv(GL_VERTEX_ARRAY, &polluted_arrays[0]);
			glGetBooleanv(GL_COLOR_ARRAY, &polluted_arrays[1]);
			bool const polluted_clip_enabled = glIsEnabled(GL_CLIP_PLANE0) == GL_TRUE;
			RequireSnapshotPixel(glGetError() == GL_NO_ERROR && polluted.projection != entry.projection &&
									 polluted.modelview != entry.modelview && polluted.matrix_mode == GL_TEXTURE && polluted_shade == GL_SMOOTH &&
									 polluted_arrays == std::array<GLboolean, 2>{GL_TRUE, GL_TRUE} && polluted_clip_enabled &&
									 polluted_plane == polluted_equation && polluted_plane != original_plane,
								 "owned XY finish error pollution setup failed");
			glEnable(static_cast<GLenum>(0xFFFFFFFFu));
			reached_finish = true;
			pass.Finish();
			returned_from_finish = true;
		}
		catch (agi::EnvironmentError const& failure) {
			finish_error = failure.GetMessage();
		}
		bool const same_context = wglGetCurrentContext() == context && wglGetCurrentDC() == dc;
		bool restored = false;
		if (same_context) {
			std::array<GLdouble, 4> restored_plane{};
			glGetClipPlane(GL_CLIP_PLANE0, restored_plane.data());
			bool const clip_disabled = glIsEnabled(GL_CLIP_PLANE0) == GL_FALSE;
			auto const after = ReadXYOverlayState();
			GLint restored_shade = 0;
			std::array<GLboolean, 2> restored_arrays{};
			glGetIntegerv(GL_SHADE_MODEL, &restored_shade);
			glGetBooleanv(GL_VERTEX_ARRAY, &restored_arrays[0]);
			glGetBooleanv(GL_COLOR_ARRAY, &restored_arrays[1]);
			GLenum const remaining_error = glGetError();
			restored = remaining_error == GL_NO_ERROR && clip_disabled &&
					   after.modelview == identity && after.projection == original.projection && after.matrix_mode == GL_MODELVIEW &&
					   after.colour_mask == std::array<GLboolean, 4>{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE} &&
					   SameXYOverlayState(original, after) && restored_shade == GL_FLAT &&
					   restored_arrays == std::array<GLboolean, 2>{GL_FALSE, GL_FALSE};
		}
		bool const reported = reached_finish && !returned_from_finish &&
							  finish_error.find(std::to_string(GL_INVALID_ENUM)) != std::string::npos;
		std::cout << "xy_owned_pass_finish_error/" << host.name << " reached_finish=" << reached_finish
				  << " reported=" << reported << " same_context=" << same_context << " restored=" << restored
				  << " error=" << finish_error << '\n';
		passed = reported && same_context && restored && passed;
	}
	return passed;
}

struct XYOwnedInputState {
	XYOverlayStateSnapshot xy;
	std::array<GLdouble, 4> plane{};
	std::array<GLboolean, 4> arrays{};
	std::array<GLint, 4> depths{};
	GLint shade = 0;
	GLboolean clip_enabled = GL_FALSE;
};

XYOwnedInputState ReadXYOwnedInputState() {
	XYOwnedInputState state;
	state.xy = ReadXYOverlayState();
	glGetClipPlane(GL_CLIP_PLANE0, state.plane.data());
	glGetIntegerv(GL_SHADE_MODEL, &state.shade);
	glGetBooleanv(GL_VERTEX_ARRAY, &state.arrays[0]);
	glGetBooleanv(GL_COLOR_ARRAY, &state.arrays[1]);
	glGetBooleanv(GL_NORMAL_ARRAY, &state.arrays[2]);
	glGetBooleanv(GL_TEXTURE_COORD_ARRAY, &state.arrays[3]);
	glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &state.depths[0]);
	glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &state.depths[1]);
	glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &state.depths[2]);
	glGetIntegerv(GL_CLIENT_ATTRIB_STACK_DEPTH, &state.depths[3]);
	state.clip_enabled = glIsEnabled(GL_CLIP_PLANE0);
	RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "owned XY input state query failed");
	return state;
}

bool SameXYOwnedInputState(XYOwnedInputState const& a, XYOwnedInputState const& b) {
	return SameXYOverlayState(a.xy, b.xy) && a.plane == b.plane && a.arrays == b.arrays &&
		   a.depths == b.depths && a.shade == b.shade && a.clip_enabled == b.clip_enabled;
}

bool IsXYOwnedBaseline(XYOwnedInputState const& state, double width, double height, double depth, bool alpha_write) {
	std::array<GLdouble, 16> const identity{{1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1., 0., 0., 0., 0., 1.}};
	std::array<GLdouble, 16> const projection{{2. / width, 0., 0., 0., 0., -2. / height, 0., 0.,
											   0., 0., -1. / depth, 0., -1., 1., 0., 1.}};
	return state.xy.modelview == identity &&
		   std::equal(state.xy.projection.begin(), state.xy.projection.end(), projection.begin(),
					  [](double a, double b) { return std::abs(a - b) <= 1e-6; }) &&
		   state.xy.matrix_mode == GL_MODELVIEW &&
		   state.xy.colour_mask == std::array<GLboolean, 4>{GL_TRUE, GL_TRUE, GL_TRUE,
															static_cast<GLboolean>(alpha_write ? GL_TRUE : GL_FALSE)} &&
		   state.clip_enabled == GL_FALSE && state.shade == GL_FLAT &&
		   state.arrays == std::array<GLboolean, 4>{GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE};
}

bool ValidateXYOwnedWithoutPerspective() {
	constexpr int width = 160, height = 128;
	bool passed = true;
	for (bool alpha_write : {false, true})
		for (int mode : {0, 1, 2}) {
			HiddenGLWindow window(width, height);
			PrepareXYOverlayTarget(window, width, height, 1.);
			auto const before = ReadXYOwnedInputState();
			glShadeModel(GL_SMOOTH);
			glEnable(GL_CLIP_PLANE0);
			glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
			OpenGLWindowOverlayPass pass(width, height, width, height, 1., alpha_write, window.MaximumClipPlanes());
			OpenGLWrapper gl;
			auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
			OpenGLText text(deleter);
			LegacyVideoOverlayDrawContext target(gl, text, pass);
			bool state_ok = IsXYOwnedBaseline(ReadXYOwnedInputState(), width, height, 1., alpha_write);
			if (mode) {
				target.SetLineColour(agi::Color(0, 255, 0), 1.f, 2);
				target.DrawLine({20.f, 112.f}, {140.f, 112.f});
			}
			if (mode == 2)
				target.DrawText("Overlay", 10, 10, {});
			state_ok = IsXYOwnedBaseline(ReadXYOwnedInputState(), width, height, 1., alpha_write) && state_ok;
			auto const pixels = window.ReadBackRgbaTopLeft();
			bool pixels_ok = true;
			if (mode) {
				auto const sample = XYReferenceSampleOffset(pixels, width, 80, 112, 1);
				pixels_ok = pixels[sample + 1] >= 200;
			}
			if (mode == 2) {
				int text_pixels = 0;
				for (int y = 10; y < 50; ++y)
					for (int x = 10; x < 120; ++x) {
						auto const offset = (static_cast<std::size_t>(y) * width + x) * 4;
						if (pixels[offset] != 64 || pixels[offset + 1] != 64 || pixels[offset + 2] != 64)
							++text_pixels;
					}
				pixels_ok = text_pixels > 20 && pixels_ok;
			}
			if (!alpha_write)
				for (std::size_t offset = 3; offset < pixels.size(); offset += 4)
					pixels_ok = pixels[offset] == 64 && pixels_ok;
			pass.Finish();
			auto const finished = ReadXYOwnedInputState();
			bool const finish_ok = IsXYOwnedBaseline(finished, width, height, 1., true) && before.depths == finished.depths;
			std::cout << "xy_owned_without_perspective/" << (alpha_write ? "wx" : "gac") << '-' << mode
					  << " state=" << state_ok << " pixels=" << pixels_ok << " finish=" << finish_ok << '\n';
			passed = state_ok && pixels_ok && finish_ok && passed;
		}
	return passed;
}

bool ValidateXYOwnedBatchEntryChanges() {
	constexpr int width = 160, height = 128;
	struct Host {
		char const *name;
		double depth;
		bool alpha_write, legacy_context;
	};
	std::array<Host, 2> const hosts{{{.name = "wx", .depth = 1000.0, .alpha_write = true, .legacy_context = true}, {.name = "gac", .depth = 1.0, .alpha_write = false, .legacy_context = false}}};
	std::array<GLdouble, 4> const first_plane{.25, .5, 0., .75};
	std::array<VideoOverlayPerspectiveVertex, 2> const grid{{{.x = .008f, .y = -.0048f, .z = -2499.5f, .opacity = .9f},
															 {.x = -.032f, .y = -.0192f, .z = -2498.f, .opacity = .1f}}};
	std::array<VideoOverlayPerspectiveVertex, 2> const offscreen{{{.x = 10000.f, .y = 10000.f, .z = 0.f, .opacity = 1.f},
																  {.x = 10100.f, .y = 10000.f, .z = 0.f, .opacity = 1.f}}};
	std::array<XYReferenceVertex, 2> const reference_grid{{{.x = 40., .y = 40., .w = 2., .colour = {1.f, 0.f, 0.f, .1f}},
														   {.x = 200. / 3., .y = 40., .w = 1., .colour = {1.f, 0.f, 0.f, 19.f / 30.f}}}};
	VideoOverlayPerspectiveTransform const transform{.origin = {80.f, 64.f}, .angle_x = 0.f, .angle_y = 0.f, .angle_z = 0.f, .fax = 0.f, .fay = 0.f, .perspective_z_scale = 1.f};
	bool passed = true;
	for (auto const& host : hosts)
		for (bool finish_error : {false, true}) {
			std::string const label = std::string(host.name) + (finish_error ? "-finish-error" : "-finish-normal");
			HiddenGLWindow window(width, height);
			PrepareXYOverlayTarget(window, width, height, host.depth);
			HGLRC const context = wglGetCurrentContext();
			HDC const dc = wglGetCurrentDC();
			RequireSnapshotPixel(context && dc, "owned XY changing-entry fixture has no current context");
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, host.alpha_write ? GL_TRUE : GL_FALSE);
			DrawXYHomogeneousReference(reference_grid);
			auto const reference_first = ReadXYRaster(window, context, dc);
			DrawXYHomogeneousReference(reference_grid);
			auto const reference_second = ReadXYRaster(window, context, dc);
			PrepareXYOverlayTarget(window, width, height, host.depth);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glClipPlane(GL_CLIP_PLANE0, first_plane.data());
			glDisable(GL_CLIP_PLANE0);
			glShadeModel(GL_SMOOTH);
			auto const outside = ReadXYOwnedInputState();
			bool state_ok = false, pixels_ok = false, reached_finish = false, returned_from_finish = false;
			std::string error;
			{
				OpenGLWindowOverlayPass pass(width, height, width, height, host.depth, host.alpha_write, window.MaximumClipPlanes());
				OpenGLWrapper gl;
				auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
				OpenGLText text(deleter);
				std::unique_ptr<VideoOverlayDrawContext> target;
				if (host.legacy_context)
					target = std::make_unique<LegacyVideoOverlayDrawContext>(gl, text, pass);
				else
					target = std::make_unique<OpenGLVideoOverlayDrawContext>(gl, pass);
				auto draw = [&] {
					target->DrawPerspectiveBatches(grid, offscreen, offscreen, transform, agi::Color(0, 0, 0), agi::Color(255, 0, 0), 2);
				};
				draw();
				auto const first_after = ReadXYOwnedInputState();
				auto const first = ReadXYRaster(window, context, dc);
				draw();
				auto const second_after = ReadXYOwnedInputState();
				auto const second = ReadXYRaster(window, context, dc);
				state_ok = first_after.shade == GL_FLAT && first_after.clip_enabled == GL_FALSE &&
						   second_after.shade == GL_FLAT && second_after.clip_enabled == GL_FALSE &&
						   SameXYOwnedInputState(first_after, second_after);
				pixels_ok = true;
				for (int x : {45, 62}) {
					auto const a = XYReferenceSampleOffset(reference_first, width, x, 40, 0);
					auto const b = XYReferenceSampleOffset(reference_second, width, x, 40, 0);
					pixels_ok = XYReferenceSampleMatches(reference_first, first, a) &&
								XYReferenceSampleMatches(reference_second, second, b) && pixels_ok;
				}
				auto const near_sample = XYReferenceSampleOffset(reference_first, width, 45, 40, 0);
				auto const far_sample = XYReferenceSampleOffset(reference_first, width, 62, 40, 0);
				auto const outside = XYReferenceSampleOffset(reference_first, width, 90, 40, 0);
				pixels_ok = reference_first[near_sample] > 64 &&
							static_cast<int>(reference_first[far_sample]) - reference_first[near_sample] >= 40 &&
							second[far_sample] > first[far_sample] && reference_first[outside] == 64 &&
							XYReferenceSampleMatches(reference_first, first, outside) &&
							XYReferenceSampleMatches(reference_second, second, outside) && pixels_ok;
				SaveXYRaster("xy-owned-entry-" + label + "-reference.rgba", reference_second);
				SaveXYRaster("xy-owned-entry-" + label + "-first.rgba", first);
				SaveXYRaster("xy-owned-entry-" + label + "-second.rgba", second);
				if (finish_error) {
					glEnable(static_cast<GLenum>(0xFFFFFFFFu));
				}
				reached_finish = true;
				try {
					pass.Finish();
					returned_from_finish = true;
				}
				catch (agi::EnvironmentError const& failure) {
					error = failure.GetMessage();
				}
			}
			auto const finished = ReadXYOwnedInputState();
			auto expected_xy = outside.xy;
			expected_xy.colour_mask = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
			bool const finish_status = reached_finish && (finish_error ? (!returned_from_finish && error.find(std::to_string(GL_INVALID_ENUM)) != std::string::npos) : (returned_from_finish && error.empty()));
			bool const restored = SameXYOverlayState(expected_xy, finished.xy) &&
								  finished.clip_enabled == GL_FALSE && finished.shade == GL_FLAT && finished.arrays == outside.arrays &&
								  finished.depths == outside.depths;
			std::cout << "xy_owned_batch_entry_changes/" << label << " pixels=" << pixels_ok << " batches=" << state_ok
					  << " finish_status=" << finish_status << " restored=" << restored << " error=" << error << '\n';
			passed = pixels_ok && state_ok && finish_status && restored && passed;
		}
	return passed;
}

bool ValidateXYOwnedPendingEntryError() {
	constexpr int width = 160, height = 128;
	struct Host {
		char const *name;
		double depth;
		bool alpha_write, legacy_context;
	};
	std::array<Host, 2> const hosts{{{.name = "wx", .depth = 1000.0, .alpha_write = true, .legacy_context = true}, {.name = "gac", .depth = 1.0, .alpha_write = false, .legacy_context = false}}};
	std::array<GLdouble, 4> const original_plane{.25, .5, 0., .75};
	std::array<VideoOverlayPerspectiveVertex, 2> const prior_line{{{.x = -40.f, .y = -24.f, .z = 0.f, .opacity = 1.f}, {.x = 40.f, .y = -24.f, .z = 0.f, .opacity = 1.f}}};
	std::array<VideoOverlayPerspectiveVertex, 2> const pending_error_line{{{.x = -40.f, .y = -4.f, .z = 0.f, .opacity = 1.f}, {.x = 40.f, .y = -4.f, .z = 0.f, .opacity = 1.f}}};
	std::array<VideoOverlayPerspectiveVertex, 2> const offscreen{{{.x = 10000.f, .y = 10000.f, .z = 0.f, .opacity = 1.f},
																  {.x = 10100.f, .y = 10000.f, .z = 0.f, .opacity = 1.f}}};
	VideoOverlayPerspectiveTransform const transform{.origin = {80.f, 64.f}, .angle_x = 0.f, .angle_y = 0.f, .angle_z = 0.f, .fax = 0.f, .fay = 0.f, .perspective_z_scale = 1.f};
	bool passed = true;
	for (auto const& host : hosts)
		for (bool prior_batch : {false, true}) {
			std::string const label = std::string(host.name) + (prior_batch ? "-after-batch" : "-first-batch");
			HiddenGLWindow window(width, height);
			PrepareXYOverlayTarget(window, width, height, host.depth);
			HGLRC const context = wglGetCurrentContext();
			HDC const dc = wglGetCurrentDC();
			RequireSnapshotPixel(context && dc, "owned XY pending-error fixture has no current context");
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, host.alpha_write ? GL_TRUE : GL_FALSE);
			glClipPlane(GL_CLIP_PLANE0, original_plane.data());
			glDisable(GL_CLIP_PLANE0);
			glShadeModel(GL_FLAT);
			bool returned_from_draw = false, returned_from_finish = false;
			std::string error;
			XYOwnedInputState before, finished;
			{
				OpenGLWindowOverlayPass pass(width, height, width, height, host.depth, host.alpha_write, window.MaximumClipPlanes());
				OpenGLWrapper gl;
				auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
				OpenGLText text(deleter);
				std::unique_ptr<VideoOverlayDrawContext> target;
				if (host.legacy_context)
					target = std::make_unique<LegacyVideoOverlayDrawContext>(gl, text, pass);
				else
					target = std::make_unique<OpenGLVideoOverlayDrawContext>(gl, pass);
				if (prior_batch)
					target->DrawPerspectiveBatches(prior_line, offscreen, offscreen, transform, agi::Color(0, 0, 0), agi::Color(255, 0, 0), 2);
				before = ReadXYOwnedInputState();
				RequireSnapshotPixel(before.clip_enabled == GL_FALSE &&
										 before.shade == GL_FLAT,
									 "owned XY pending-error baseline setup failed");
				glEnable(static_cast<GLenum>(0xFFFFFFFFu));
				try {
					target->DrawPerspectiveBatches(pending_error_line, offscreen, offscreen, transform,
												   agi::Color(0, 0, 0), agi::Color(0, 255, 0), 2);
					returned_from_draw = true;
				}
				catch (agi::EnvironmentError const& failure) {
					error = failure.GetMessage();
				}
				RequireSnapshotPixel(returned_from_draw, "owned batch rejected a pending error instead of deferring it to Finish");
				try {
					pass.Finish();
					returned_from_finish = true;
				}
				catch (agi::EnvironmentError const& failure) {
					error = failure.GetMessage();
				}
				finished = ReadXYOwnedInputState();
			}
			auto expected_xy = before.xy;
			expected_xy.colour_mask = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
			bool const reported = returned_from_draw && !returned_from_finish &&
								  error.find(std::to_string(GL_INVALID_ENUM)) != std::string::npos;
			bool const restored = SameXYOverlayState(expected_xy, finished.xy) &&
								  finished.clip_enabled == GL_FALSE && finished.shade == before.shade && finished.arrays == before.arrays &&
								  finished.depths == before.depths;
			std::cout << "xy_owned_pending_entry_error/" << label << " deferred=" << returned_from_draw
					  << " reported=" << reported << " restored=" << restored << " error=" << error << '\n';
			passed = reported && restored && passed;
		}
	return passed;
}

bool ValidateXYOwnedPassBoundaries() {
	constexpr int width = 160, height = 128;
	std::array<VideoOverlayPerspectiveVertex, 2> const line{{{.x = -40.f, .y = -24.f, .z = 0.f, .opacity = 1.f},
															 {.x = 40.f, .y = -24.f, .z = 0.f, .opacity = 1.f}}};
	VideoOverlayPerspectiveTransform const transform{
		.origin = {80.f, 64.f}, .angle_x = 0.f, .angle_y = 0.f, .angle_z = 0.f, .fax = 0.f, .fay = 0.f, .perspective_z_scale = 1.f};
	bool passed = true;
	for (bool alpha_write : {false, true})
		for (bool split_canvas : {false, true}) {
			HiddenGLWindow window(width, height);
			PrepareXYOverlayTarget(window, width, height, 1.);
			auto const canvas = OpenGLWindowOverlayPass::SetupCanvas(width, height, width, height, 1.);
			auto const before = ReadXYOwnedInputState();
			glEnable(static_cast<GLenum>(0xFFFFFFFFu));
			bool rejected = false, constructed = false;
			try {
				if (split_canvas) {
					OpenGLWindowOverlayPass pass(canvas, alpha_write, window.MaximumClipPlanes());
					constructed = true;
					pass.Finish();
				}
				else {
					OpenGLWindowOverlayPass pass(width, height, width, height, 1., alpha_write, window.MaximumClipPlanes());
					constructed = true;
					pass.Finish();
				}
			}
			catch (agi::EnvironmentError const& error) {
				rejected = error.GetMessage().find(std::to_string(GL_INVALID_ENUM)) != std::string::npos;
			}
			auto const after = ReadXYOwnedInputState();
			bool const entry_ok = rejected && constructed == split_canvas &&
								  (split_canvas ? IsXYOwnedBaseline(after, width, height, 1., true) : SameXYOwnedInputState(before, after));

			std::array<GLdouble, 4> const sentinel{.25, .5, 0., .75};
			glClipPlane(GL_CLIP_PLANE0, sentinel.data());
			glShadeModel(GL_SMOOTH);
			auto const unwind_entry = ReadXYOwnedInputState();
			bool unwound = false;
			try {
				OpenGLWindowOverlayPass pass(canvas, alpha_write, window.MaximumClipPlanes());
				OpenGLWrapper gl;
				OpenGLVideoOverlayDrawContext target(gl, pass);
				target.DrawPerspectiveBatches(line, line, line, transform, agi::Color(255, 0, 0), agi::Color(0, 255, 0), 2);
				glMatrixMode(GL_PROJECTION);
				glLoadIdentity();
				glMatrixMode(GL_MODELVIEW);
				glTranslatef(3.f, 5.f, 0.f);
				glShadeModel(GL_SMOOTH);
				glEnable(GL_CLIP_PLANE0);
				glEnableClientState(GL_VERTEX_ARRAY);
				glEnableClientState(GL_COLOR_ARRAY);
				glEnableClientState(GL_TEXTURE_COORD_ARRAY);
				glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
				throw std::runtime_error("owned pass unwind witness");
			}
			catch (std::runtime_error const& error) {
				unwound = std::string(error.what()) == "owned pass unwind witness";
			}
			auto const unwind_after = ReadXYOwnedInputState();
			bool const unwind_ok = unwound && IsXYOwnedBaseline(unwind_after, width, height, 1., true) &&
								   unwind_entry.depths == unwind_after.depths;
			std::cout << "xy_owned_pass_boundaries/" << (alpha_write ? "wx" : "gac")
					  << (split_canvas ? "-split" : "-direct") << " entry=" << entry_ok << " unwind=" << unwind_ok << '\n';
			passed = entry_ok && unwind_ok && passed;
		}
	return passed;
}

bool ValidateXYPreparedPerspectiveBatches() {
	constexpr int width = 400, height = 300;
	struct Pose {
		char const *name;
		VideoOverlayPerspectiveTransform transform;
	};
	std::array<Pose, 4> const poses{{
		{.name = "neutral", .transform = {.origin = {200.f, 150.f}, .angle_x = 0.f, .angle_y = 0.f, .angle_z = 0.f, .fax = 0.f, .fay = 0.f, .perspective_z_scale = 8.f}},
		{.name = "rotated-shear", .transform = {.origin = {200.f, 150.f}, .angle_x = 25.f, .angle_y = 40.f, .angle_z = 15.f, .fax = .2f, .fay = -.1f, .perspective_z_scale = 8.f}},
		{.name = "edge", .transform = {.origin = {200.f, 150.f}, .angle_x = 90.f, .angle_y = 90.f, .angle_z = 10.f, .fax = .2f, .fay = -.1f, .perspective_z_scale = 8.f}},
		{.name = "near", .transform = {.origin = {200.f, 150.f}, .angle_x = 75.f, .angle_y = 80.f, .angle_z = 15.f, .fax = .2f, .fay = -.1f, .perspective_z_scale = 640.f}},
	}};
	struct Host {
		char const *name;
		double depth;
		bool alpha_write;
	};
	std::array<Host, 3> const hosts{{
		{.name = "wx", .depth = 1000., .alpha_write = true},
		{.name = "gac", .depth = 1., .alpha_write = false},
		{.name = "small-depth", .depth = .5, .alpha_write = false},
	}};
	agi::Color const primary(37, 211, 89);
	agi::Color const first_colour(211, 37, 89), second_colour(37, 89, 211);
	aegisub::visual_transform::RotateXYOverlayGeometryCache cache;
	auto const first = cache.Get(first_colour);
	auto const second = cache.Get(second_colour);
	HiddenGLWindow window(width, height);
	bool passed = true;
	for (auto const& [host, depth, alpha_write] : hosts)
		for (auto const& pose : poses) {
			std::string const label = std::string(host) + '-' + pose.name;
			auto draw = [&](VideoOverlayPreparedPerspectiveBatches const& prepared, bool optimized) {
				PrepareXYOverlayTarget(window, width, height, depth);
				OpenGLWindowOverlayPass pass(width, height, width, height, depth, alpha_write, window.MaximumClipPlanes());
				OpenGLWrapper gl;
				auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
				OpenGLText text(deleter);
				LegacyVideoOverlayDrawContext target(gl, text, pass);
				if (optimized)
					target.DrawPreparedPerspectiveBatches(prepared, pose.transform, primary, 2);
				else
					target.DrawPerspectiveBatches(prepared.Grid(), prepared.Axes(), prepared.Arrows(), pose.transform,
												  primary, prepared.SecondaryColour(), 2);
				pass.Finish();
				return ReadXYRaster(window, wglGetCurrentContext(), wglGetCurrentDC());
			};
			auto const original = draw(*first, false);
			auto const prepared = draw(*first, true);
			auto const recoloured = draw(*second, true);
			auto const recoloured_reference = draw(*second, false);
			auto const retained = draw(*first, true);
			bool const equivalent = original == prepared && recoloured == recoloured_reference && original == retained;
			bool const colour_changed = recoloured != prepared;
			SaveXYRaster("xy-prepared-" + label + ".rgba", prepared);
			SaveXYRaster("xy-prepared-" + label + "-colour.rgba", recoloured);
			std::cout << "xy_prepared/" << label << " equivalent=" << equivalent << " colour_changed=" << colour_changed << '\n';
			passed = equivalent && colour_changed && passed;
		}

	constexpr std::array<VideoOverlayPerspectiveVertex, 2> literal_grid{{
		{.x = .008f, .y = -.0048f, .z = -2499.5f, .opacity = .9f},
		{.x = -.032f, .y = -.0192f, .z = -2498.f, .opacity = .1f},
	}};
	constexpr std::array<VideoOverlayPerspectiveVertex, 2> literal_axes{{
		{.x = -40.f, .y = 16.f, .z = 0.f, .opacity = 1.f},
		{.x = 40.f, .y = 16.f, .z = 0.f, .opacity = 1.f},
	}};
	constexpr std::array<VideoOverlayPerspectiveVertex, 2> literal_arrows{{
		{.x = -40.f, .y = 36.f, .z = 0.f, .opacity = 1.f},
		{.x = 40.f, .y = 36.f, .z = 0.f, .opacity = 1.f},
	}};
	constexpr VideoOverlayStaticPerspectiveBatches literal(literal_grid, literal_axes, literal_arrows);
	VideoOverlayPreparedPerspectiveBatches const literal_prepared(literal, agi::Color(255, 0, 0));
	VideoOverlayPerspectiveTransform const literal_transform{
		.origin = {80.f, 64.f}, .angle_x = 0.f, .angle_y = 0.f, .angle_z = 0.f, .fax = 0.f, .fay = 0.f, .perspective_z_scale = 1.f};
	std::array<XYReferenceVertex, 2> const clipped_reference{{
		{.x = 40., .y = 40., .w = 2., .colour = {1.f, 0.f, 0.f, .1f}},
		{.x = 200. / 3., .y = 40., .w = 1., .colour = {1.f, 0.f, 0.f, 19.f / 30.f}},
	}};
	for (auto const& [host, depth, alpha_write] : hosts) {
		PrepareXYOverlayTarget(window, width, height, depth);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, alpha_write ? GL_TRUE : GL_FALSE);
		DrawXYHomogeneousReference(clipped_reference);
		auto const reference = ReadXYRaster(window, wglGetCurrentContext(), wglGetCurrentDC());
		PrepareXYOverlayTarget(window, width, height, depth);
		OpenGLWindowOverlayPass literal_pass(width, height, width, height, depth, alpha_write, window.MaximumClipPlanes());
		OpenGLWrapper literal_gl;
		OpenGLVideoOverlayDrawContext literal_target(literal_gl, literal_pass);
		literal_target.DrawPreparedPerspectiveBatches(literal_prepared, literal_transform, agi::Color(0, 0, 255), 2);
		literal_pass.Finish();
		auto const rendered = ReadXYRaster(window, wglGetCurrentContext(), wglGetCurrentDC());
		auto const near_sample = XYReferenceSampleOffset(reference, width, 45, 40, 0);
		auto const far_sample = XYReferenceSampleOffset(reference, width, 62, 40, 0);
		bool const gradient = XYReferenceSampleMatches(reference, rendered, near_sample) &&
							  XYReferenceSampleMatches(reference, rendered, far_sample) &&
							  static_cast<int>(rendered[far_sample]) - rendered[near_sample] >= 40;
		std::cout << "xy_prepared_literal/" << host << " clipped_gradient=" << gradient << '\n';
		passed = gradient && passed;
		SaveXYRaster(std::string("xy-prepared-literal-") + host + ".rgba", rendered);
	}

	PrepareXYOverlayTarget(window, width, height, 1.);
	OpenGLWindowOverlayPass pass(width, height, width, height, 1., true, window.MaximumClipPlanes());
	OpenGLWrapper gl;
	OpenGLVideoOverlayDrawContext target(gl, pass);
	auto const before = ReadXYOwnedInputState();
	auto const before_pixels = ReadXYRaster(window, wglGetCurrentContext(), wglGetCurrentDC());
	for (int invalid = 0; invalid != 5; ++invalid) {
		auto transform = poses.front().transform;
		int line_width = 2;
		if (invalid == 0) transform.angle_x = std::numeric_limits<float>::quiet_NaN();
		if (invalid == 1) transform.perspective_z_scale = 0;
		if (invalid == 2) transform.perspective_z_scale = -1;
		if (invalid == 3) transform.fax = std::numeric_limits<float>::max();
		if (invalid == 4) line_width = 0;
		bool rejected = false;
		try {
			target.DrawPreparedPerspectiveBatches(*first, transform, primary, line_width);
		}
		catch (agi::EnvironmentError const&) {
			rejected = true;
		}
		bool const unchanged = SameXYOwnedInputState(before, ReadXYOwnedInputState()) &&
							   before_pixels == ReadXYRaster(window, wglGetCurrentContext(), wglGetCurrentDC());
		std::cout << "xy_prepared_invalid/" << invalid << " rejected=" << rejected << " unchanged=" << unchanged << '\n';
		passed = rejected && unchanged && passed;
	}
	auto dynamic_grid = std::vector<VideoOverlayPerspectiveVertex>(first->Grid().begin(), first->Grid().end());
	for (int invalid = 0; invalid != 2; ++invalid) {
		dynamic_grid.front() = first->Grid().front();
		if (invalid == 0)
			dynamic_grid.front().x = std::numeric_limits<float>::quiet_NaN();
		else
			dynamic_grid.front().opacity = 1.5f;
		bool rejected = false;
		try {
			target.DrawPerspectiveBatches(dynamic_grid, first->Axes(), first->Arrows(), poses.front().transform,
										  primary, first_colour, 2);
		}
		catch (agi::EnvironmentError const&) {
			rejected = true;
		}
		bool const unchanged = SameXYOwnedInputState(before, ReadXYOwnedInputState()) &&
							   before_pixels == ReadXYRaster(window, wglGetCurrentContext(), wglGetCurrentDC());
		std::cout << "xy_prepared_dynamic_invalid/" << invalid << " rejected=" << rejected << " unchanged=" << unchanged << '\n';
		passed = rejected && unchanged && passed;
	}
	pass.Finish();
	std::vector<VideoOverlayPerspectiveVertex> changed_grid(first->Grid().begin(), first->Grid().end());
	for (auto& vertex : changed_grid) {
		vertex.x += 37.f;
		vertex.opacity *= .4f;
	}
	auto draw_dynamic = [&](std::span<VideoOverlayPerspectiveVertex const> grid) {
		PrepareXYOverlayTarget(window, width, height, 1.);
		OpenGLWindowOverlayPass dynamic_pass(width, height, width, height, 1., true, window.MaximumClipPlanes());
		OpenGLWrapper dynamic_gl;
		OpenGLVideoOverlayDrawContext dynamic_target(dynamic_gl, dynamic_pass);
		dynamic_target.DrawPerspectiveBatches(grid, first->Axes(), first->Arrows(), poses.front().transform, primary, first_colour, 2);
		dynamic_pass.Finish();
		return ReadXYRaster(window, wglGetCurrentContext(), wglGetCurrentDC());
	};
	bool const dynamic_changed = draw_dynamic(first->Grid()) != draw_dynamic(changed_grid);
	std::cout << "xy_prepared_dynamic_changed respected=" << dynamic_changed << '\n';
	passed = dynamic_changed && passed;
	return passed;
}

bool ValidateXYInvalidPerspectiveInput() {
	constexpr int width = 160, height = 128;
	struct Case {
		char const *name;
		VideoOverlayPerspectiveTransform transform;
		char const *error;
	};
	VideoOverlayPerspectiveTransform const valid{.origin = {80.f, 64.f}, .angle_x = 0.f, .angle_y = 0.f, .angle_z = 0.f, .fax = 0.f, .fay = 0.f, .perspective_z_scale = 1.f};
	std::array<Case, 4> cases{{
		{.name = "nan-angle", .transform = valid, .error = "non-finite transform"},
		{.name = "zero-scale", .transform = valid, .error = "invalid perspective scale"},
		{.name = "negative-scale", .transform = valid, .error = "invalid perspective scale"},
		{.name = "overflow-shear", .transform = valid, .error = "transform exceeds finite range"},
	}};
	cases[0].transform.angle_x = std::numeric_limits<float>::quiet_NaN();
	cases[1].transform.perspective_z_scale = 0.f;
	cases[2].transform.perspective_z_scale = -1.f;
	cases[3].transform.fax = std::numeric_limits<float>::max();
	std::array<std::pair<char const *, double>, 2> const hosts{{{"wx", 1000.0}, {"gac", 1.0}}};
	std::array<VideoOverlayPerspectiveVertex, 2> const line{{{.x = -40.f, .y = -24.f, .z = 0.f, .opacity = .25f}, {.x = 40.f, .y = -24.f, .z = 0.f, .opacity = .9f}}};
	HiddenGLWindow window(width, height);
	bool passed = true;
	for (auto const& [host, depth] : hosts)
		for (auto const& test : cases) {
			std::string const label = std::string(host) + '-' + test.name;
			PrepareXYOverlayTarget(window, width, height, depth);
			HGLRC const context = wglGetCurrentContext();
			HDC const dc = wglGetCurrentDC();
			RequireSnapshotPixel(context && dc, "XY invalid input has no current context");
			glDisable(GL_COLOR_MATERIAL);
			glDisable(GL_CLIP_PLANE0);
			glMatrixMode(GL_MODELVIEW);
			glTranslatef(3.f, 5.f, 0.f);
			glMatrixMode(GL_TEXTURE);
			glLoadIdentity();
			glTranslatef(.125f, .25f, 0.f);
			auto const before_pixels = ReadXYRaster(window, context, dc);
			auto const before = ReadXYOverlayState();
			OpenGLWrapper gl;
			auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
			OpenGLText text(deleter);
			LegacyVideoOverlayDrawContext target(gl, text);
			std::string error;
			try {
				target.DrawPerspectiveBatches(line, line, line, test.transform, agi::Color(0, 0, 0), agi::Color(255, 0, 0), 2);
			}
			catch (agi::EnvironmentError const& failure) {
				error = failure.GetMessage();
			}
			RequireSnapshotPixel(wglGetCurrentContext() == context && wglGetCurrentDC() == dc,
								 "XY invalid input changed the current context or DC");
			auto const after = ReadXYOverlayState();
			auto const after_pixels = ReadXYRaster(window, context, dc);
			bool const rejected = error.find(test.error) != std::string::npos;
			bool const state_unchanged = SameXYOverlayState(before, after);
			bool const pixels_unchanged = before_pixels == after_pixels;
			std::cout << "xy_invalid_input/" << label << " rejected=" << rejected << " state_unchanged=" << state_unchanged
					  << " pixels_unchanged=" << pixels_unchanged << " error=" << error << '\n';
			if (!rejected || !state_unchanged || !pixels_unchanged) {
				SaveXYRaster("xy-invalid-" + label + "-before.rgba", before_pixels);
				SaveXYRaster("xy-invalid-" + label + "-after.rgba", after_pixels);
				passed = false;
			}
		}
	return passed;
}

bool ValidateXYStackFull() {
	constexpr int width = 160, height = 128;
	struct StackCase {
		char const *operation;
		GLenum depth, maximum, matrix_mode;
	};
	std::array<StackCase, 4> const cases{{
		{.operation = "attribute push", .depth = GL_ATTRIB_STACK_DEPTH, .maximum = GL_MAX_ATTRIB_STACK_DEPTH, .matrix_mode = 0},
		{.operation = "projection push", .depth = GL_PROJECTION_STACK_DEPTH, .maximum = GL_MAX_PROJECTION_STACK_DEPTH, .matrix_mode = GL_PROJECTION},
		{.operation = "modelview push", .depth = GL_MODELVIEW_STACK_DEPTH, .maximum = GL_MAX_MODELVIEW_STACK_DEPTH, .matrix_mode = GL_MODELVIEW},
		{.operation = "client push", .depth = GL_CLIENT_ATTRIB_STACK_DEPTH, .maximum = GL_MAX_CLIENT_ATTRIB_STACK_DEPTH, .matrix_mode = 0},
	}};
	auto read_depths = [] {
		std::array<GLint, 4> depths{};
		glGetIntegerv(GL_ATTRIB_STACK_DEPTH, &depths[0]);
		glGetIntegerv(GL_PROJECTION_STACK_DEPTH, &depths[1]);
		glGetIntegerv(GL_MODELVIEW_STACK_DEPTH, &depths[2]);
		glGetIntegerv(GL_CLIENT_ATTRIB_STACK_DEPTH, &depths[3]);
		RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "XY stack depth query failed");
		return depths;
	};
	std::array<std::pair<char const *, double>, 2> const hosts{{{"wx", 1000.0}, {"gac", 1.0}}};
	std::array<VideoOverlayPerspectiveVertex, 2> const line{{{.x = -40.f, .y = -24.f, .z = 0.f, .opacity = 1.f}, {.x = 40.f, .y = -24.f, .z = 0.f, .opacity = 1.f}}};
	VideoOverlayPerspectiveTransform const transform{.origin = {80.f, 64.f}, .angle_x = 0.f, .angle_y = 0.f, .angle_z = 0.f, .fax = 0.f, .fay = 0.f, .perspective_z_scale = 1.f};
	bool passed = true;
	for (auto const& [host, host_depth] : hosts)
		for (auto const& test : cases) {
			HiddenGLWindow window(width, height);
			PrepareXYOverlayTarget(window, width, height, host_depth);
			legacy_gl::PrepareVisualToolState(window.MaximumClipPlanes());
			auto const context = wglGetCurrentContext();
			auto const dc = wglGetCurrentDC();
			RequireSnapshotPixel(context && dc, "XY stack fixture has no context");
			glMatrixMode(GL_TEXTURE);
			GLint baseline = -1, limit = -1;
			glGetIntegerv(test.depth, &baseline);
			glGetIntegerv(test.maximum, &limit);
			RequireSnapshotPixel(glGetError() == GL_NO_ERROR && baseline >= 0 && limit > baseline && limit <= 1024,
								 "XY stack fixture cannot establish a bounded full stack");
			auto const initial_state = ReadXYOverlayState();
			auto const initial_depths = read_depths();
			auto restore = [&]() noexcept {
				if (wglGetCurrentContext() != context || wglGetCurrentDC() != dc) std::terminate();
				if (test.matrix_mode) glMatrixMode(test.matrix_mode);
				GLint depth = -1;
				glGetIntegerv(test.depth, &depth);
				if (glGetError() != GL_NO_ERROR || depth < baseline || depth > limit) std::terminate();
				for (; depth > baseline; --depth) {
					if (test.matrix_mode)
						glPopMatrix();
					else if (test.depth == GL_ATTRIB_STACK_DEPTH)
						glPopAttrib();
					else
						glPopClientAttrib();
					if (glGetError() != GL_NO_ERROR) std::terminate();
				}
				glMatrixMode(GL_TEXTURE);
				if (glGetError() != GL_NO_ERROR) std::terminate();
			};
			auto cleanup = agi::make_scope_exit([&] { restore(); });
			if (test.matrix_mode) glMatrixMode(test.matrix_mode);
			for (GLint current = baseline; current < limit; ++current) {
				if (test.matrix_mode)
					glPushMatrix();
				else if (test.depth == GL_ATTRIB_STACK_DEPTH)
					glPushAttrib(GL_TRANSFORM_BIT | GL_LIGHTING_BIT);
				else
					glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
				RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "XY stack fixture push failed");
			}
			glMatrixMode(GL_TEXTURE);
			GLint full_depth = -1;
			glGetIntegerv(test.depth, &full_depth);
			RequireSnapshotPixel(glGetError() == GL_NO_ERROR && full_depth == limit, "XY stack fixture is not full");
			auto const before_state = ReadXYOverlayState();
			auto const before_depths = read_depths();
			auto const before_pixels = ReadXYRaster(window, context, dc);
			OpenGLWrapper gl;
			auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
			OpenGLText text(deleter);
			LegacyVideoOverlayDrawContext target(gl, text);
			std::string error;
			try {
				target.DrawPerspectiveBatches(line, line, line, transform, agi::Color(0, 0, 0), agi::Color(255, 0, 0), 2);
			}
			catch (agi::EnvironmentError const& failure) {
				error = failure.GetMessage();
			}
			bool const no_error = glGetError() == GL_NO_ERROR;
			auto const after_state = ReadXYOverlayState();
			auto const after_depths = read_depths();
			auto const after_pixels = ReadXYRaster(window, context, dc);
			bool const rejected = error.find(test.operation) != std::string::npos &&
								  error.find(std::to_string(GL_STACK_OVERFLOW)) != std::string::npos;
			bool const unchanged = no_error && before_depths == after_depths &&
								   SameXYOverlayState(before_state, after_state) && before_pixels == after_pixels;
			restore();
			cleanup.release();
			bool const cleaned = initial_depths == read_depths() && SameXYOverlayState(initial_state, ReadXYOverlayState());
			std::string const label = std::string(host) + '-' + test.operation;
			SaveXYRaster("xy-stack-full-" + label + "-before.rgba", before_pixels);
			SaveXYRaster("xy-stack-full-" + label + "-after.rgba", after_pixels);
			std::cout << "xy_stack_full/" << label << " rejected=" << rejected << " unchanged=" << unchanged
					  << " cleaned=" << cleaned << " error=" << error << '\n';
			passed = rejected && unchanged && cleaned && passed;
		}
	return passed;
}

bool ValidateXYClipPlaneState() {
	constexpr int width = 160, height = 128;
	HiddenGLWindow window(width, height);
	PrepareXYOverlayTarget(window, width, height, 1.0);
	HGLRC const context = wglGetCurrentContext();
	HDC const dc = wglGetCurrentDC();
	std::array<GLdouble, 4> const equation{1.0, 0.0, 0.0, -1000.0};
	glClipPlane(GL_CLIP_PLANE0, equation.data());
	glEnable(GL_CLIP_PLANE0);
	std::array<GLdouble, 4> before_plane{}, after_plane{};
	glGetClipPlane(GL_CLIP_PLANE0, before_plane.data());
	auto const before_pixels = ReadXYRaster(window, context, dc);
	auto const before = ReadXYOverlayState();
	RequireSnapshotPixel(before_plane == equation && glIsEnabled(GL_CLIP_PLANE0) == GL_TRUE && glGetError() == GL_NO_ERROR,
						 "XY active clip-plane setup failed");
	std::array<VideoOverlayPerspectiveVertex, 2> const line{{{.x = -40.f, .y = -24.f, .z = 0.f, .opacity = 1.f}, {.x = 40.f, .y = -24.f, .z = 0.f, .opacity = 1.f}}};
	std::array<VideoOverlayPerspectiveVertex, 2> const offscreen{{{.x = 10000.f, .y = 10000.f, .z = 0.f, .opacity = 1.f}, {.x = 10100.f, .y = 10000.f, .z = 0.f, .opacity = 1.f}}};
	VideoOverlayPerspectiveTransform const transform{.origin = {80.f, 64.f}, .angle_x = 0.f, .angle_y = 0.f, .angle_z = 0.f, .fax = 0.f, .fay = 0.f, .perspective_z_scale = 1.f};
	OpenGLWrapper gl;
	auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
	OpenGLText text(deleter);
	LegacyVideoOverlayDrawContext target(gl, text);
	std::string error;
	try {
		target.DrawPerspectiveBatches(line, offscreen, offscreen, transform, agi::Color(0, 0, 0), agi::Color(255, 0, 0), 2);
	}
	catch (agi::EnvironmentError const& failure) {
		error = failure.GetMessage();
	}
	auto const after = ReadXYOverlayState();
	glGetClipPlane(GL_CLIP_PLANE0, after_plane.data());
	bool const enabled_after = glIsEnabled(GL_CLIP_PLANE0) == GL_TRUE;
	auto const after_pixels = ReadXYRaster(window, context, dc);
	SaveXYRaster("xy-clip-state-rejected.rgba", after_pixels);
	bool const rejected = error.find("clip plane 0") != std::string::npos && before_plane == after_plane && enabled_after &&
						  SameXYOverlayState(before, after) && before_pixels == after_pixels;
	glDisable(GL_CLIP_PLANE0);
	auto const disabled_before = ReadXYOverlayState();
	target.DrawPerspectiveBatches(line, offscreen, offscreen, transform, agi::Color(0, 0, 0), agi::Color(255, 0, 0), 2);
	auto const disabled_after = ReadXYOverlayState();
	glGetClipPlane(GL_CLIP_PLANE0, after_plane.data());
	bool const plane_restored = glIsEnabled(GL_CLIP_PLANE0) == GL_FALSE && after_plane == before_plane && SameXYOverlayState(disabled_before, disabled_after);
	auto const pixels = ReadXYRaster(window, context, dc);
	SaveXYRaster("xy-clip-state-visible.rgba", pixels);
	auto const offset = XYReferenceSampleOffset(pixels, width, 80, 40, 0);
	bool const visible = pixels[offset] >= 200;
	std::cout << "xy_clip_state rejected=" << rejected << " disabled_plane_restored=" << plane_restored << " visible=" << visible << '\n';
	return rejected && plane_restored && visible;
}

bool ValidateXYHostInheritedClipAndFog() {
	constexpr int width = 740, height = 740;
	struct Host {
		char const *name;
		double depth;
		bool alpha_write;
		bool prepare_before_reset;
	};
	std::array<Host, 2> const hosts{{{.name = "wx", .depth = 1000.0, .alpha_write = true, .prepare_before_reset = false}, {.name = "gac", .depth = 1.0, .alpha_write = false, .prepare_before_reset = true}}};
	std::array<char const *, 2> const pollutants{{"clip-plane-1", "fog"}};
	bool passed = true;
	for (auto const& host : hosts) {
		HiddenGLWindow window(width, height);
		PrepareXYOverlayTarget(window, width, height, host.depth);
		HGLRC const context = wglGetCurrentContext();
		HDC const dc = wglGetCurrentDC();
		GLint max_planes = 0;
		glGetIntegerv(GL_MAX_CLIP_PLANES, &max_planes);
		RequireSnapshotPixel(context && dc && max_planes >= 2 && glGetError() == GL_NO_ERROR,
							 "XY inherited-state fixture requires a current context and user clip plane 1");
		OpenGLWrapper gl;
		auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
		OpenGLText text(deleter);
		LegacyVideoOverlayDrawContext target(gl, text);
		aegisub::visual_transform::RotateXYOverlayState const model{
			.origin = {370.f, 370.f}, .angle_x = 0.f, .angle_y = 0.f, .angle_z = 0.f, .fax = 0.f, .fay = 0.f, .perspective_z_scale = 8.f, .primary_colour = {255, 0, 0, 0}, .secondary_colour = {0, 255, 0, 0}, .feature_fill_colour = {0, 0, 255, 0}};
		auto alpha_mask = [&] { glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, host.alpha_write ? GL_TRUE : GL_FALSE); };
		auto draw_marker_and_grid = [&] {
			target.SetLineColour(agi::Color(0, 255, 0), 1.f, 1);
			target.SetFillColour(agi::Color(0, 0, 255), .3f);
			VisualDraggableFeature marker;
			marker.type = DRAG_BIG_TRIANGLE;
			marker.pos = model.origin;
			marker.Draw(target);
			aegisub::visual_transform::DrawRotateXYOverlay(target, model);
		};
		struct InheritedState {
			std::array<GLdouble, 4> plane{};
			std::array<GLfloat, 4> fog_colour{};
			GLfloat fog_start = 0, fog_end = 0, fog_density = 0;
			GLint fog_mode = 0;
			GLboolean clip_enabled = GL_FALSE, fog_enabled = GL_FALSE;
			bool operator==(InheritedState const&) const = default;
		};
		auto read_state = [&] {
			InheritedState state;
			glGetClipPlane(GL_CLIP_PLANE1, state.plane.data());
			glGetFloatv(GL_FOG_COLOR, state.fog_colour.data());
			glGetFloatv(GL_FOG_START, &state.fog_start);
			glGetFloatv(GL_FOG_END, &state.fog_end);
			glGetFloatv(GL_FOG_DENSITY, &state.fog_density);
			glGetIntegerv(GL_FOG_MODE, &state.fog_mode);
			state.clip_enabled = glIsEnabled(GL_CLIP_PLANE1);
			state.fog_enabled = glIsEnabled(GL_FOG);
			RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "XY inherited-state capture generated a GL error");
			return state;
		};

		PrepareXYOverlayTarget(window, width, height, host.depth);
		alpha_mask();
		DrawXYReferenceLine(390.f, 370.f, .94f, 390.f, 690.f, 0.f);
		auto const independent_line = ReadXYRaster(window, context, dc);
		SaveXYRaster(std::string("xy-host-inherited-") + host.name + "-line-reference.rgba", independent_line);
		PrepareXYOverlayTarget(window, width, height, host.depth);
		alpha_mask();
		draw_marker_and_grid();
		auto const reference = ReadXYRaster(window, context, dc);
		SaveXYRaster(std::string("xy-host-inherited-") + host.name + "-scene-reference.rgba", reference);
		auto const green_near = XYGreenSample(independent_line, width, 390, 380);
		auto const green_far = XYGreenSample(independent_line, width, 390, 400);
		auto const scene_near = XYGreenSample(reference, width, 390, 380);
		auto const scene_far = XYGreenSample(reference, width, 390, 400);
		bool green_reference_matches = true;
		for (std::size_t channel = 0; channel < green_near.size(); ++channel) {
			green_reference_matches = std::abs(green_near[channel] - scene_near[channel]) <= 3 && green_reference_matches;
			green_reference_matches = std::abs(green_far[channel] - scene_far[channel]) <= 3 && green_reference_matches;
		}
		auto const marker_pixel = (static_cast<std::size_t>(365) * width + 367) * 4;
		RequireSnapshotPixel(green_reference_matches && green_near[1] > 200 && green_near[1] - green_far[1] >= 8 &&
								 reference[marker_pixel + 2] >= 100 && reference[marker_pixel + 2] >= reference[marker_pixel] + 40,
							 "XY inherited-state clean reference lacks the independent gradient or marker fill");

		for (auto const *pollutant : pollutants) {
			bool const clip_case = std::string(pollutant) == "clip-plane-1";
			std::string const label = std::string(host.name) + '-' + pollutant;
			PrepareXYOverlayTarget(window, width, height, host.depth);
			alpha_mask();
			if (clip_case) {
				std::array<GLdouble, 4> const equation{-1.0, 0.0, 0.0, 200.0};
				glClipPlane(GL_CLIP_PLANE1, equation.data());
				glEnable(GL_CLIP_PLANE1);
			}
			else {
				std::array<GLfloat, 4> const magenta{1.f, 0.f, 1.f, 1.f};
				glFogi(GL_FOG_MODE, GL_LINEAR);
				glFogf(GL_FOG_START, -1.f);
				glFogf(GL_FOG_END, 0.f);
				glFogf(GL_FOG_DENSITY, .5f);
				glFogfv(GL_FOG_COLOR, magenta.data());
				glEnable(GL_FOG);
			}
			auto const polluted_state = read_state();
			RequireSnapshotPixel((clip_case && polluted_state.clip_enabled == GL_TRUE && polluted_state.fog_enabled == GL_FALSE &&
								  polluted_state.plane == std::array<GLdouble, 4>{-1.0, 0.0, 0.0, 200.0}) ||
									 (!clip_case && polluted_state.clip_enabled == GL_FALSE && polluted_state.fog_enabled == GL_TRUE &&
									  polluted_state.fog_mode == GL_LINEAR && polluted_state.fog_start == -1.f && polluted_state.fog_end == 0.f &&
									  polluted_state.fog_density == .5f && polluted_state.fog_colour == std::array<GLfloat, 4>{1.f, 0.f, 1.f, 1.f}),
								 "XY inherited-state pollution was not established");
			glPushAttrib(GL_ALL_ATTRIB_BITS);
			DrawXYReferenceLine(390.f, 370.f, .94f, 390.f, 690.f, 0.f);
			auto const polluted_line = ReadXYRaster(window, context, dc);
			glPopAttrib();
			SaveXYRaster("xy-host-inherited-" + label + "-polluted-line.rgba", polluted_line);
			auto const polluted_offset = XYReferenceSampleOffset(polluted_line, width, 390, 380, 0);
			bool const pollution_visible = clip_case ? XYGreenSample(polluted_line, width, 390, 380) == std::array<int, 4>{64, 64, 64, 64} : polluted_line[polluted_offset] >= 180 && polluted_line[polluted_offset + 2] >= 180 && polluted_line[polluted_offset + 1] <= 70;
			RequireSnapshotPixel(pollution_visible && read_state() == polluted_state,
								 "XY inherited-state fixture did not visibly contaminate its independent line");
			glClear(GL_COLOR_BUFFER_BIT);
			if (host.prepare_before_reset)
				legacy_gl::PrepareVisualToolState(window.MaximumClipPlanes());
			legacy_gl::ResetCompatibilityState();
			glViewport(0, 0, width, height);
			glMatrixMode(GL_PROJECTION);
			glLoadIdentity();
			glOrtho(0.0, width, height, 0.0, -host.depth, host.depth);
			glMatrixMode(GL_MODELVIEW);
			glLoadIdentity();
			alpha_mask();
			if (!host.prepare_before_reset)
				legacy_gl::PrepareVisualToolState(window.MaximumClipPlanes());
			auto const normalized = read_state();
			auto const entry = ReadXYOverlayState();
			draw_marker_and_grid();
			auto const exit = ReadXYOverlayState();
			auto const restored = read_state();
			auto const actual = ReadXYRaster(window, context, dc);
			SaveXYRaster("xy-host-inherited-" + label + "-actual.rgba", actual);
			bool const normalized_flags = normalized.clip_enabled == GL_FALSE && normalized.fog_enabled == GL_FALSE;
			bool const parameters_preserved = normalized.plane == polluted_state.plane && normalized.fog_colour == polluted_state.fog_colour &&
											  normalized.fog_start == polluted_state.fog_start && normalized.fog_end == polluted_state.fog_end &&
											  normalized.fog_density == polluted_state.fog_density && normalized.fog_mode == polluted_state.fog_mode;
			bool const restored_state = restored == normalized && SameXYOverlayState(entry, exit);
			bool const marker_matches = XYReferenceSampleMatches(reference, actual, marker_pixel);
			bool const grid_matches = XYGreenSample(reference, width, 390, 380) == XYGreenSample(actual, width, 390, 380) &&
									  XYGreenSample(reference, width, 390, 400) == XYGreenSample(actual, width, 390, 400);
			bool const full_scene_matches = reference == actual;
			std::cout << "xy_host_inherited/" << label << " pollution_visible=" << pollution_visible
					  << " normalized_flags=" << normalized_flags << " parameters_preserved=" << parameters_preserved
					  << " restored_state=" << restored_state << " marker_matches=" << marker_matches << " grid_matches=" << grid_matches
					  << " full_scene_matches=" << full_scene_matches << '\n';
			passed = parameters_preserved && restored_state && marker_matches && grid_matches && full_scene_matches && passed;
			glDisable(GL_CLIP_PLANE1);
			glDisable(GL_FOG);
			RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "XY inherited-state fixture cleanup failed");
		}
	}
	return passed;
}

bool ValidateXYColorMaterialResetOrder() {
	constexpr int width = 740, height = 740;
	struct Host {
		char const *name;
		double depth;
		bool alpha_write;
		bool prepare_before_reset;
	};
	std::array<Host, 2> const hosts{{{.name = "wx", .depth = 1000.0, .alpha_write = true, .prepare_before_reset = false}, {.name = "gac", .depth = 1.0, .alpha_write = false, .prepare_before_reset = true}}};
	struct Material {
		std::array<GLfloat, 4> ambient{}, diffuse{}, specular{}, emission{};
		GLfloat shininess = 0;
		bool operator==(Material const&) const = default;
	};
	bool passed = true;
	for (auto const& host : hosts) {
		HiddenGLWindow window(width, height);
		PrepareXYOverlayTarget(window, width, height, host.depth);
		HGLRC const context = wglGetCurrentContext();
		HDC const dc = wglGetCurrentDC();
		RequireSnapshotPixel(context && dc, "XY material-order test has no current context");
		OpenGLWrapper gl;
		auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
		OpenGLText text(deleter);
		LegacyVideoOverlayDrawContext target(gl, text);
		aegisub::visual_transform::RotateXYOverlayState const model{
			.origin = {370.f, 370.f}, .angle_x = 0.f, .angle_y = 0.f, .angle_z = 0.f, .fax = 0.f, .fay = 0.f, .perspective_z_scale = 8.f, .primary_colour = {255, 0, 0, 0}, .secondary_colour = {0, 255, 0, 0}, .feature_fill_colour = {0, 0, 255, 0}};
		auto alpha_mask = [&] { glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, host.alpha_write ? GL_TRUE : GL_FALSE); };
		auto draw_scene = [&] {
			target.SetLineColour(agi::Color(0, 255, 0), 1.f, 1);
			target.SetFillColour(agi::Color(0, 0, 255), .3f);
			VisualDraggableFeature marker;
			marker.type = DRAG_BIG_TRIANGLE;
			marker.pos = model.origin;
			marker.Draw(target);
			aegisub::visual_transform::DrawRotateXYOverlay(target, model);
		};
		auto read_material = [&] {
			Material value;
			glGetMaterialfv(GL_FRONT, GL_AMBIENT, value.ambient.data());
			glGetMaterialfv(GL_FRONT, GL_DIFFUSE, value.diffuse.data());
			glGetMaterialfv(GL_FRONT, GL_SPECULAR, value.specular.data());
			glGetMaterialfv(GL_FRONT, GL_EMISSION, value.emission.data());
			glGetMaterialfv(GL_FRONT, GL_SHININESS, &value.shininess);
			RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "XY material query generated a GL error");
			return value;
		};
		auto set_projection = [&] {
			glViewport(0, 0, width, height);
			glMatrixMode(GL_PROJECTION);
			glLoadIdentity();
			glOrtho(0.0, width, height, 0.0, -host.depth, host.depth);
			glMatrixMode(GL_MODELVIEW);
			glLoadIdentity();
		};

		PrepareXYOverlayTarget(window, width, height, host.depth);
		legacy_gl::ResetCompatibilityState();
		legacy_gl::PrepareVisualToolState(window.MaximumClipPlanes());
		set_projection();
		alpha_mask();
		draw_scene();
		auto const reference = ReadXYRaster(window, context, dc);
		SaveXYRaster(std::string("xy-color-material-") + host.name + "-reference.rgba", reference);

		PrepareXYOverlayTarget(window, width, height, host.depth);
		legacy_gl::ResetCompatibilityState();
		GLfloat const ambient[] = {.11f, .22f, .33f, .44f};
		GLfloat const diffuse[] = {.55f, .66f, .77f, .88f};
		GLfloat const specular[] = {.14f, .24f, .34f, .44f};
		GLfloat const emission[] = {.03f, .06f, .09f, .12f};
		GLfloat const tracked_colour[] = {.19f, .37f, .61f, .83f};
		glDisable(GL_COLOR_MATERIAL);
		glDisable(GL_LIGHTING);
		glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT, ambient);
		glMaterialfv(GL_FRONT_AND_BACK, GL_DIFFUSE, diffuse);
		glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, specular);
		glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, emission);
		glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 17.f);
		glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
		glEnable(GL_LIGHTING);
		glEnable(GL_COLOR_MATERIAL);
		glColor4fv(tracked_colour);
		auto const before = read_material();
		RequireSnapshotPixel(glIsEnabled(GL_LIGHTING) == GL_TRUE && glIsEnabled(GL_COLOR_MATERIAL) == GL_TRUE &&
								 before.ambient == std::array<GLfloat, 4>{tracked_colour[0], tracked_colour[1], tracked_colour[2], tracked_colour[3]} &&
								 before.diffuse == std::array<GLfloat, 4>{tracked_colour[0], tracked_colour[1], tracked_colour[2], tracked_colour[3]} &&
								 glGetError() == GL_NO_ERROR,
							 "XY color-material fixture did not capture the post-enable tracked material");
		if (host.prepare_before_reset)
			legacy_gl::PrepareVisualToolState(window.MaximumClipPlanes());
		legacy_gl::ResetCompatibilityState();
		set_projection();
		alpha_mask();
		if (!host.prepare_before_reset)
			legacy_gl::PrepareVisualToolState(window.MaximumClipPlanes());
		auto const after_prepare = read_material();
		bool const material_preserved = after_prepare == before;
		bool const normalized_flags = glIsEnabled(GL_LIGHTING) == GL_FALSE && glIsEnabled(GL_COLOR_MATERIAL) == GL_FALSE;
		glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		draw_scene();
		auto const after_draw = read_material();
		auto const actual = ReadXYRaster(window, context, dc);
		SaveXYRaster(std::string("xy-color-material-") + host.name + "-actual.rgba", actual);
		bool const pixels_match = actual == reference;
		bool const draw_left_material = after_draw == after_prepare;
		RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "XY color-material order test left a GL error");
		std::cout << "xy_color_material_order/" << host.name << " material_preserved=" << material_preserved
				  << " normalized_flags=" << normalized_flags << " pixels_match=" << pixels_match
				  << " draw_left_material=" << draw_left_material << '\n';
		passed = material_preserved && normalized_flags && pixels_match && draw_left_material && passed;
		legacy_gl::PrepareVisualToolState(window.MaximumClipPlanes());
		RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "XY color-material cleanup failed");
	}
	return passed;
}

bool ValidateXYClippedGradient() {
	constexpr int width = 160, height = 128;
	struct Direction {
		char const *name;
		float angle, origin_y;
		std::array<int, 3> samples;
	};
	std::array<Direction, 2> const directions{{{.name = "outside-to-inside", .angle = 89.99f, .origin_y = 40.f, .samples = {103, 105, 107}},
											   {.name = "inside-to-outside", .angle = -89.99f, .origin_y = 88.f, .samples = {24, 22, 20}}}};
	std::array<std::pair<char const *, double>, 2> const hosts{{{"wx", 1000.0}, {"gac", 1.0}}};
	bool passed = true;
	HiddenGLWindow window(width, height);
	for (auto const& [host, depth] : hosts)
		for (auto const& direction : directions) {
			std::string const label = std::string(host) + '-' + direction.name;
			PrepareXYOverlayTarget(window, width, height, depth);
			HGLRC const context = wglGetCurrentContext();
			HDC const dc = wglGetCurrentDC();
			RequireSnapshotPixel(context && dc, "XY clipped gradient has no current context");
			double const radians = std::abs(static_cast<double>(direction.angle)) * std::numbers::pi / 180.0;
			double const local_near = 2499.0 / (15.625 * std::sin(radians));
			double const sign = direction.angle > 0.f ? 1.0 : -1.0;
			double const near_y = direction.origin_y + sign * 2500.0 * std::cos(radians) * local_near;
			auto const near_alpha = static_cast<float>(1.0 - local_near / 320.0);
			XYReferenceVertex const near_vertex{.x = 80.0, .y = near_y, .w = 1.0, .colour = {0.f, 1.f, 0.f, near_alpha}};
			XYReferenceVertex const inner_vertex{.x = 80.0, .y = direction.origin_y, .w = 2500.0, .colour = {0.f, 1.f, 0.f, 1.f}};
			DrawXYHomogeneousReference(direction.angle > 0.f ? std::array{near_vertex, inner_vertex} : std::array{inner_vertex, near_vertex});
			auto const reference = ReadXYRaster(window, context, dc);
			SaveXYRaster("xy-clipped-gradient-" + label + "-reference.rgba", reference);

			PrepareXYOverlayTarget(window, width, height, depth);
			OpenGLWrapper gl;
			auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
			OpenGLText text(deleter);
			LegacyVideoOverlayDrawContext target(gl, text);
			aegisub::visual_transform::RotateXYOverlayState const state{
				.origin = {80.f, direction.origin_y},
				.angle_x = direction.angle,
				.angle_y = 0.f,
				.angle_z = 0.f,
				.fax = 0.f,
				.fay = 0.f,
				.perspective_z_scale = 15.625f,
				.primary_colour = {255, 0, 0, 0},
				.secondary_colour = {0, 255, 0, 0},
				.feature_fill_colour = {}};
			aegisub::visual_transform::DrawRotateXYOverlay(target, state);
			auto const actual = ReadXYRaster(window, context, dc);
			SaveXYRaster("xy-clipped-gradient-" + label + "-actual.rgba", actual);
			bool samples_match = true;
			for (int y : direction.samples) {
				auto const offset = XYReferenceSampleOffset(reference, width, 80, y, 1);
				samples_match = reference[offset + 1] >= 140 && reference[offset + 1] <= 180 &&
								XYReferenceSampleMatches(reference, actual, offset) && samples_match;
				std::cout << "xy_clipped_gradient/sample " << label << " y=" << y
						  << " reference_green=" << static_cast<int>(reference[offset + 1])
						  << " actual_green=" << static_cast<int>(actual[offset + 1]) << '\n';
			}
			std::cout << "xy_clipped_gradient/" << label << " match=" << samples_match << '\n';
			passed = samples_match && passed;
		}
	return passed;
}

bool ValidateXYClippedGradientContrast() {
	constexpr int width = 160, height = 128;
	std::array<std::pair<char const *, double>, 2> const hosts{{{"wx", 1000.0}, {"gac", 1.0}}};
	std::array<std::pair<char const *, bool>, 2> const directions{{{"outside-to-inside", false}, {"inside-to-outside", true}}};
	bool passed = true;
	HiddenGLWindow window(width, height);
	for (auto const& [host, depth] : hosts)
		for (auto const& [direction, reverse] : directions) {
			std::string const label = std::string(host) + '-' + direction;
			PrepareXYOverlayTarget(window, width, height, depth);
			HGLRC const context = wglGetCurrentContext();
			HDC const dc = wglGetCurrentDC();
			RequireSnapshotPixel(context && dc, "XY gradient contrast has no current context");
			std::array<XYReferenceVertex, 2> const reference_vertices{{{.x = 40.0, .y = 40.0, .w = 2.0, .colour = {1.f, 0.f, 0.f, .1f}},
																	   {.x = 200.0 / 3.0, .y = 40.0, .w = 1.0, .colour = {1.f, 0.f, 0.f, 19.f / 30.f}}}};
			DrawXYHomogeneousReference(reference_vertices);
			auto const reference = ReadXYRaster(window, context, dc);
			SaveXYRaster("xy-clipped-gradient-contrast-" + label + "-reference.rgba", reference);
			PrepareXYOverlayTarget(window, width, height, depth);
			VideoOverlayPerspectiveVertex const outside{.x = .008f, .y = -.0048f, .z = -2499.5f, .opacity = .9f};
			VideoOverlayPerspectiveVertex const inside{.x = -.032f, .y = -.0192f, .z = -2498.f, .opacity = .1f};
			std::array<VideoOverlayPerspectiveVertex, 2> const vertices = reverse ? std::array{inside, outside} : std::array{outside, inside};
			std::array<VideoOverlayPerspectiveVertex, 2> const offscreen{{{.x = 10000.f, .y = 10000.f, .z = 0.f, .opacity = 1.f}, {.x = 10100.f, .y = 10000.f, .z = 0.f, .opacity = 1.f}}};
			VideoOverlayPerspectiveTransform const transform{.origin = {80.f, 64.f}, .angle_x = 0.f, .angle_y = 0.f, .angle_z = 0.f, .fax = 0.f, .fay = 0.f, .perspective_z_scale = 1.f};
			OpenGLWrapper gl;
			auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
			OpenGLText text(deleter);
			LegacyVideoOverlayDrawContext target(gl, text);
			target.DrawPerspectiveBatches(vertices, offscreen, offscreen, transform, agi::Color(0, 0, 0), agi::Color(255, 0, 0), 2);
			auto const actual = ReadXYRaster(window, context, dc);
			SaveXYRaster("xy-clipped-gradient-contrast-" + label + "-actual.rgba", actual);
			auto const near_offset = XYReferenceSampleOffset(reference, width, 45, 40, 0);
			auto const far_offset = XYReferenceSampleOffset(reference, width, 62, 40, 0);
			int const reference_difference = static_cast<int>(reference[far_offset]) - reference[near_offset];
			int const actual_difference = static_cast<int>(actual[far_offset]) - actual[near_offset];
			bool const matches = reference_difference >= 40 && actual_difference >= 35 &&
								 XYReferenceSampleMatches(reference, actual, near_offset) && XYReferenceSampleMatches(reference, actual, far_offset);
			std::cout << "xy_clipped_gradient_contrast/" << label << " reference_difference=" << reference_difference
					  << " actual_difference=" << actual_difference << " matches=" << matches << '\n';
			passed = matches && passed;
		}
	return passed;
}

bool ValidateRotateXYOverlayPixels() {
	struct HostDepth {
		char const *name;
		double depth;
	};
	std::array<HostDepth, 2> const hosts{{{.name = "wx", .depth = 1000.0}, {.name = "gac", .depth = 1.0}}};
	bool passed = true;
	auto check = [&](bool condition, std::string const& name) {
		std::cout << "rotate_xy_overlay/" << name << " pass=" << condition << '\n';
		passed = condition && passed;
	};

	for (auto const& host : hosts) {
		std::string const host_name(host.name);
		{
			constexpr int width = 740, height = 740;
			HiddenGLWindow window(width, height);
			PrepareXYOverlayTarget(window, width, height, host.depth);
			HGLRC const context = wglGetCurrentContext();
			HDC const dc = wglGetCurrentDC();
			RequireSnapshotPixel(context && dc, "XY gradient has no current context");
			DrawXYReferenceLine(390.f, 370.f, .94f, 390.f, 690.f, 0.f);
			auto const reference = ReadXYRaster(window, context, dc);
			SaveXYRaster("xy-gradient-" + host_name + "-reference.rgba", reference);

			PrepareXYOverlayTarget(window, width, height, host.depth);
			OpenGLWrapper gl;
			auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
			OpenGLText text(deleter);
			LegacyVideoOverlayDrawContext target(gl, text);
			aegisub::visual_transform::RotateXYOverlayState const state{
				.origin = {370.f, 370.f},
				.angle_x = 0.f,
				.angle_y = 0.f,
				.angle_z = 0.f,
				.fax = 0.f,
				.fay = 0.f,
				.perspective_z_scale = 8.f,
				.primary_colour = {255, 0, 0, 0},
				.secondary_colour = {0, 255, 0, 0},
				.feature_fill_colour = {}};
			aegisub::visual_transform::DrawRotateXYOverlay(target, state);
			auto const actual = ReadXYRaster(window, context, dc);
			SaveXYRaster("xy-gradient-" + host_name + "-actual.rgba", actual);
			auto const near_reference = XYGreenSample(reference, width, 390, 380);
			auto const far_reference = XYGreenSample(reference, width, 390, 400);
			auto const near_actual = XYGreenSample(actual, width, 390, 380);
			auto const far_actual = XYGreenSample(actual, width, 390, 400);
			bool colours_match = true;
			for (std::size_t channel = 0; channel < near_actual.size(); ++channel) {
				colours_match = std::abs(near_reference[channel] - near_actual[channel]) <= 3 && colours_match;
				colours_match = std::abs(far_reference[channel] - far_actual[channel]) <= 3 && colours_match;
			}
			std::cout << "rotate_xy_overlay/gradient-" << host_name << " reference_green="
					  << near_reference[1] << ',' << far_reference[1] << " actual_green="
					  << near_actual[1] << ',' << far_actual[1] << '\n';
			check(near_reference[1] - far_reference[1] >= 8 && colours_match,
				  "endpoint_gradient-" + host_name);
			check(XYGreenSample(actual, width, 10, 10) == std::array<int, 4>{64, 64, 64, 64},
				  "gradient_background_alpha-" + host_name);
		}
		{
			constexpr int width = 160, height = 128;
			HiddenGLWindow window(width, height);
			PrepareXYOverlayTarget(window, width, height, host.depth);
			HGLRC const context = wglGetCurrentContext();
			HDC const dc = wglGetCurrentDC();
			RequireSnapshotPixel(context && dc, "XY near clip has no current context");
			double const angle = 89.99 * std::numbers::pi / 180.0;
			double const local_near = 2499.0 / (15.625 * std::sin(angle));
			auto const near_y = static_cast<float>(40.0 + 2500.0 * std::cos(angle) * local_near);
			DrawXYReferenceLine(80.f, 40.f, 1.f, 80.f, near_y,
								static_cast<float>(1.0 - local_near / 320.0));
			auto const reference = ReadXYRaster(window, context, dc);
			SaveXYRaster("xy-near-" + host_name + "-reference.rgba", reference);

			PrepareXYOverlayTarget(window, width, height, host.depth);
			OpenGLWrapper gl;
			auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
			OpenGLText text(deleter);
			LegacyVideoOverlayDrawContext target(gl, text);
			aegisub::visual_transform::RotateXYOverlayState const state{
				.origin = {80.f, 40.f},
				.angle_x = 89.99f,
				.angle_y = 0.f,
				.angle_z = 0.f,
				.fax = 0.f,
				.fay = 0.f,
				.perspective_z_scale = 15.625f,
				.primary_colour = {255, 0, 0, 0},
				.secondary_colour = {0, 255, 0, 0},
				.feature_fill_colour = {}};
			aegisub::visual_transform::DrawRotateXYOverlay(target, state);
			auto const actual = ReadXYRaster(window, context, dc);
			SaveXYRaster("xy-near-" + host_name + "-actual.rgba", actual);
			bool visible_to_near_plane = true;
			for (int y = 103; y <= 108; ++y)
				visible_to_near_plane = XYGreenSample(reference, width, 80, y)[1] >= 100 &&
										XYGreenSample(actual, width, 80, y)[1] >= 100 && visible_to_near_plane;
			bool behind_is_empty = true;
			for (int y = 114; y <= 124; ++y)
				behind_is_empty = XYGreenSample(reference, width, 80, y)[1] <= 68 &&
								  XYGreenSample(actual, width, 80, y)[1] <= 68 && behind_is_empty;
			std::cout << "rotate_xy_overlay/near-" << host_name << " reference_endpoint_y=" << near_y << '\n';
			check(visible_to_near_plane, "near_plane_visible-" + host_name);
			check(behind_is_empty, "behind_near_plane_empty-" + host_name);
		}
		{
			constexpr int width = 160, height = 128;
			HiddenGLWindow window(width, height);
			PrepareXYOverlayTarget(window, width, height, host.depth);
			glMatrixMode(GL_MODELVIEW);
			glTranslatef(3.f, 5.f, 0.f);
			glMatrixMode(GL_TEXTURE);
			glLoadIdentity();
			glTranslatef(.125f, .25f, 0.f);
			HGLRC const context = wglGetCurrentContext();
			HDC const dc = wglGetCurrentDC();
			auto const before = ReadXYOverlayState();
			RequireSnapshotPixel(context && dc && glGetError() == GL_NO_ERROR, "XY state setup failed");
			OpenGLWrapper gl;
			auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
			OpenGLText text(deleter);
			LegacyVideoOverlayDrawContext target(gl, text);
			aegisub::visual_transform::RotateXYOverlayState const state{
				.origin = {80.f, 40.f},
				.angle_x = 0.f,
				.angle_y = 0.f,
				.angle_z = 0.f,
				.fax = 0.f,
				.fay = 0.f,
				.perspective_z_scale = 8.f,
				.primary_colour = {255, 0, 0, 0},
				.secondary_colour = {0, 255, 0, 0},
				.feature_fill_colour = {}};
			aegisub::visual_transform::DrawRotateXYOverlay(target, state);
			RequireSnapshotPixel(wglGetCurrentContext() == context && wglGetCurrentDC() == dc,
								 "XY state case changed the current WGL context or DC");
			auto const after = ReadXYOverlayState();
			RequireSnapshotPixel(glGetError() == GL_NO_ERROR, "XY state case generated a GL error");
			auto const actual = ReadXYRaster(window, context, dc);
			SaveXYRaster("xy-state-" + host_name + "-actual.rgba", actual);
			check(SameXYOverlayState(before, after), "state_restored-" + host_name);
			check(XYGreenSample(actual, width, 103, 55)[1] >= 100, "state_scene_visible-" + host_name);
		}
	}
	return passed;
}

std::shared_ptr<const VisualToolDragSnapshot> MakeDragGlyphSnapshot(VisualToolPresentation const& presentation,
																	DraggableFeatureType type, Vector2D position, wxColour const& line_colour, wxColour const& fill_colour) {
	auto snapshot = std::make_shared<VisualToolDragSnapshot>(presentation.Context());
	snapshot->grid_colour = line_colour.GetRGB();
	snapshot->line_colour = line_colour.GetRGB();
	snapshot->features.push_back({.type = type, .pos = position, .parent = std::nullopt, .fill_colour = fill_colour.GetRGB()});
	return snapshot;
}

void RequireGlyphLocationAndColour(std::vector<unsigned char> const& pixels, int width, int height,
								   Vector2D position, wxColour const& fill_colour, std::string const& name) {
	RequireSnapshotPixel(pixels.size() == static_cast<size_t>(width) * height * 4, name + ": wrong raster dimensions");
	int coloured = 0;
	int green_outline = 0;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			auto const index = static_cast<size_t>(y * width + x) * 4;
			int const r = pixels[index], g = pixels[index + 1], b = pixels[index + 2];
			if (r || g || b) {
				++coloured;
				RequireSnapshotPixel(std::abs(x - position.X()) <= 18 && std::abs(y - position.Y()) <= 18,
									 name + ": coloured pixel outside the expected glyph location");
				if (g > 60 && g > r * 2 && g > b * 2)
					++green_outline;
			}
		}
	}
	RequireSnapshotPixel(coloured > 25, name + ": glyph is empty or too small");
	RequireSnapshotPixel(green_outline > 6, name + ": expected green outline is absent");
	auto const sample = static_cast<size_t>((static_cast<int>(position.Y()) - 3) * width + static_cast<int>(position.X()) + 3) * 4;
	std::array<int, 4> const expected{
		static_cast<int>(std::lround(fill_colour.Red() * 0.3)),
		static_cast<int>(std::lround(fill_colour.Green() * 0.3)),
		static_cast<int>(std::lround(fill_colour.Blue() * 0.3)),
		static_cast<int>(std::lround((0.3 * 0.3 + 0.7) * 255))};
	for (size_t channel = 0; channel < expected.size(); ++channel)
		RequireSnapshotPixel(std::abs(static_cast<int>(pixels[sample + channel]) - expected[channel]) <= 2,
							 name + ": independent interior RGBA colour/alpha mismatch at channel " + std::to_string(channel));
}

bool ValidateDragSnapshotGlyphPixels() {
	constexpr int width = 160, height = 128;
	HiddenGLWindow window(width, height);
	VisualToolPresentation presentation;
	wxColour const line_colour(32, 224, 64);
	wxColour const fill_colour(240, 80, 32);
	Vector2D const position(48, 64);
	std::array<std::pair<DraggableFeatureType, char const *>, 3> const cases{{{DRAG_BIG_SQUARE, "square"}, {DRAG_BIG_CIRCLE, "circle"}, {DRAG_BIG_TRIANGLE, "triangle"}}};
	for (auto const& [type, name] : cases) {
		auto const reference = RenderLegacyDragGlyph(window, width, height, type, position, line_colour, fill_colour);
		auto const snapshot = MakeDragGlyphSnapshot(presentation, type, position, line_colour, fill_colour);
		auto const actual = RenderDragSnapshot(window, width, height, snapshot);
		RequireGlyphLocationAndColour(reference, width, height, position, fill_colour, std::string(name) + "/legacy");
		RequireGlyphLocationAndColour(actual, width, height, position, fill_colour, std::string(name) + "/snapshot");
		RequireSnapshotPixel(actual == reference, std::string(name) + ": snapshot and legacy RGBA rasters differ");
		std::cout << "drag_snapshot_glyph/" << name << " rgba_exact=1 nonempty_location_colour=1\n";
	}
	return true;
}

bool ValidateDragSnapshotPresentationPixels() {
	constexpr int width = 192, height = 128;
	HiddenGLWindow window(width, height);
	VisualToolPresentation presentation;
	wxColour const line_colour(32, 224, 64);
	wxColour const released_colour(240, 80, 32);
	wxColour const pressed_colour(32, 80, 240);
	Vector2D const baseline_position(36, 64), displayed_position(84, 64), input_position(144, 64);
	auto const baseline = MakeDragGlyphSnapshot(presentation, DRAG_BIG_SQUARE, baseline_position, line_colour, released_colour);
	auto const displayed = MakeDragGlyphSnapshot(presentation, DRAG_BIG_SQUARE, displayed_position, line_colour, released_colour);
	auto const newest_input = MakeDragGlyphSnapshot(presentation, DRAG_BIG_SQUARE, input_position, line_colour, released_colour);
	RequireSnapshotPixel(presentation.Begin(41, baseline), "could not begin first interaction");
	auto const baseline_pixels = RenderDragSnapshot(window, width, height, presentation.Select({}));
	RequireGlyphLocationAndColour(baseline_pixels, width, height, baseline_position, released_colour, "baseline");
	auto const selected_pixels = RenderDragSnapshot(window, width, height, presentation.Select(displayed));
	auto const expected_displayed = RenderLegacyDragGlyph(window, width, height, DRAG_BIG_SQUARE, displayed_position, line_colour, released_colour);
	auto const input_pixels = RenderDragSnapshot(window, width, height, newest_input);
	RequireGlyphLocationAndColour(selected_pixels, width, height, displayed_position, released_colour, "displayed_not_input");
	RequireGlyphLocationAndColour(input_pixels, width, height, input_position, released_colour, "newest_input_control");
	RequireSnapshotPixel(selected_pixels == expected_displayed, "selected packet did not retain its displayed position");
	RequireSnapshotPixel(selected_pixels != input_pixels, "fixture did not distinguish displayed and newest-input positions");
	RequireSnapshotPixel(!presentation.OnFinalPresented(40), "unrelated Final ended the current interaction");
	RequireSnapshotPixel(RenderDragSnapshot(window, width, height, presentation.Select(displayed)) == selected_pixels,
						 "unrelated Final changed displayed geometry");
	auto const final_pixels = RenderDragSnapshot(window, width, height, presentation.Select(newest_input));
	RequireGlyphLocationAndColour(final_pixels, width, height, input_position, released_colour, "matching_final");
	RequireSnapshotPixel(presentation.OnFinalPresented(41), "matching Final did not end first interaction");
	RequireSnapshotPixel(!presentation.Matches(newest_input), "completed Final snapshot context remained reusable");
	RequireSnapshotPixel(!presentation.Begin(42, newest_input), "new press accepted the old released snapshot as its baseline");
	auto const new_press = MakeDragGlyphSnapshot(presentation, DRAG_BIG_SQUARE, input_position, line_colour, pressed_colour);
	RequireSnapshotPixel(presentation.Begin(42, new_press), "new press did not accept its fresh feedback snapshot");
	RequireSnapshotPixel(!presentation.OnFinalPresented(41), "late old Final ended the new press");
	auto const press_pixels = RenderDragSnapshot(window, width, height, presentation.Select(newest_input));
	auto const expected_press = RenderLegacyDragGlyph(window, width, height, DRAG_BIG_SQUARE, input_position, line_colour, pressed_colour);
	RequireGlyphLocationAndColour(press_pixels, width, height, input_position, pressed_colour, "new_press_not_old_release");
	RequireSnapshotPixel(press_pixels == expected_press, "new press did not draw its independently expected colour");
	RequireSnapshotPixel(press_pixels != final_pixels, "new press reused the completed Final's released colour");
	std::cout << "drag_snapshot_presentation displayed_not_input=1 final_retired=1 fresh_press_colour=1\n";
	return true;
}

// Use the production main-thread transport, but advance its tasks explicitly.
// This controls ordering without adding delays or gates to the application.
class ScopedSnapshotDeliveryQueue {
	std::deque<agi::dispatch::Thunk> pending;

	public:
	ScopedSnapshotDeliveryQueue() {
		agi::dispatch::Init([this](agi::dispatch::Thunk thunk) { pending.push_back(std::move(thunk)); }, [] { return true; });
	}
	~ScopedSnapshotDeliveryQueue() {
		agi::dispatch::Init([](agi::dispatch::Thunk const&) {}, [] { return false; });
		pending.clear();
	}
	ScopedSnapshotDeliveryQueue(ScopedSnapshotDeliveryQueue const&) = delete;
	ScopedSnapshotDeliveryQueue& operator=(ScopedSnapshotDeliveryQueue const&) = delete;
	[[nodiscard]] size_t Size() const { return pending.size(); }
	void PumpOne() {
		RequireSnapshotPixel(pending.size() == 1, "expected exactly one queued delivery");
		auto thunk = std::move(pending.front());
		pending.pop_front();
		thunk();
	}
};

VideoRenderPacket MakeSnapshotPacket(std::shared_ptr<const VisualToolRenderSnapshot> snapshot,
									 VideoRenderDeliveryClass delivery_class, std::uint64_t interaction, std::uint64_t content) {
	VideoRenderPacket packet;
	packet.frame_number = 17;
	packet.time = 725.0;
	packet.delivery_version = {.provider = 7, .content = content, .request = 11};
	packet.delivery_class = delivery_class;
	packet.visual_interaction_id = interaction;
	packet.visual_tool_snapshot = std::move(snapshot);
	return packet;
}

bool ValidateQueuedDragFinalPixels() {
	constexpr int width = 224, height = 128;
	ScopedSnapshotDeliveryQueue queue;
	HiddenGLWindow window(width, height);
	VisualToolPresentation presentation;
	auto lifetime = agi::ui::MakeLifetime();
	std::optional<VideoRenderPacket> delivered;
	int callbacks = 0;
	auto sink = CreateAsyncVideoProviderMainThreadSink(lifetime,
													   {.on_frame_ready = [&](VideoRenderPacket packet, double time) {
														   RequireSnapshotPixel(time == 725.0 && packet.time == time && packet.frame_number == 17,
																				"queued delivery changed frame/time");
														   ++callbacks;
														   delivered = std::move(packet);
													   }},
													   AsyncVideoFrameDeliveryMode::VisualSubtitleBatches);
	wxColour const line(32, 224, 64), released(240, 80, 32), pressed(32, 80, 240);
	Vector2D const old_position(48, 64), final_position(112, 64), new_position(176, 64);
	auto const displayed = MakeDragGlyphSnapshot(presentation, DRAG_BIG_SQUARE, old_position, line, pressed);
	auto const old_final = MakeSnapshotPacket(
		MakeDragGlyphSnapshot(presentation, DRAG_BIG_SQUARE, final_position, line, released),
		VideoRenderDeliveryClass::VisualSubtitleFinal, 41, 101);
	RequireSnapshotPixel(presentation.Begin(41, displayed), "first drag did not start");
	sink.on_frame_ready(old_final, old_final.time);
	RequireSnapshotPixel(queue.Size() == 1 && callbacks == 0 && !delivered,
						 "old Final was not held at the real main-thread boundary");
	RequireSnapshotPixel(presentation.Begin(42, displayed), "second drag did not start before old Final");
	auto const before = RenderDragSnapshot(window, width, height, presentation.Select({}));
	RequireGlyphLocationAndColour(before, width, height, old_position, pressed, "queued_final/baseline");

	queue.PumpOne();
	RequireSnapshotPixel(callbacks == 1 && delivered.has_value() && queue.Size() == 0,
						 "old Final did not traverse its queued callback exactly once");
	RequireSnapshotPixel(delivered->delivery_version == old_final.delivery_version && delivered->delivery_class == VideoRenderDeliveryClass::VisualSubtitleFinal && delivered->visual_interaction_id == 41 && delivered->visual_tool_snapshot == old_final.visual_tool_snapshot,
						 "old Final identity or snapshot changed in transport");
	auto const old_pixels = RenderDragSnapshot(window, width, height, presentation.Select(delivered->visual_tool_snapshot));
	RequireGlyphLocationAndColour(old_pixels, width, height, final_position, released, "queued_final/late_A");
	RequireSnapshotPixel(old_pixels == RenderLegacyDragGlyph(window, width, height, DRAG_BIG_SQUARE, final_position, line, released),
						 "late A Final pixels differ from the independent legacy glyph");
	RequireSnapshotPixel(!presentation.OnFinalPresented(delivered->visual_interaction_id) && presentation.IsActive() && presentation.InteractionId() == 42,
						 "late A Final retired B");

	for (auto const delivery_class : {VideoRenderDeliveryClass::VisualSubtitleIntermediate, VideoRenderDeliveryClass::VisualSubtitleFinal}) {
		bool const final = delivery_class == VideoRenderDeliveryClass::VisualSubtitleFinal;
		auto const colour = final ? released : pressed;
		auto const packet = MakeSnapshotPacket(
			MakeDragGlyphSnapshot(presentation, DRAG_BIG_SQUARE, new_position, line, colour), delivery_class, 42, final ? 103 : 102);
		sink.on_frame_ready(packet, packet.time);
		queue.PumpOne();
		RequireSnapshotPixel(delivered->delivery_version == packet.delivery_version && delivered->delivery_class == delivery_class && delivered->visual_interaction_id == 42 && delivered->visual_tool_snapshot == packet.visual_tool_snapshot, "B packet identity changed");
		auto const pixels = RenderDragSnapshot(window, width, height, presentation.Select(delivered->visual_tool_snapshot));
		RequireGlyphLocationAndColour(pixels, width, height, new_position, colour, final ? "queued_final/B_final" : "queued_final/B_intermediate");
		RequireSnapshotPixel(pixels == RenderLegacyDragGlyph(window, width, height, DRAG_BIG_SQUARE, new_position, line, colour),
							 "B pixels differ from the independent legacy glyph");
		if (final)
			RequireSnapshotPixel(presentation.OnFinalPresented(42) && !presentation.IsActive(), "B Final did not retire B");
	}
	RequireSnapshotPixel(callbacks == 3 && queue.Size() == 0, "unexpected queued Final callback count");
	std::cout << "drag_snapshot_queued_final old_final_after_next_begin=1 payload_exact=1 old_cannot_retire_new=1 pixels_exact=1\n";
	return true;
}

bool ValidateQueuedDragResetPixels() {
	constexpr int width = 192, height = 128;
	ScopedSnapshotDeliveryQueue queue;
	HiddenGLWindow window(width, height);
	VisualToolPresentation presentation;
	auto lifetime = agi::ui::MakeLifetime();
	std::optional<VideoRenderPacket> delivered;
	int callbacks = 0;
	auto sink = CreateAsyncVideoProviderMainThreadSink(lifetime,
													   {.on_frame_ready = [&](VideoRenderPacket packet, double) { ++callbacks; delivered = std::move(packet); }},
													   AsyncVideoFrameDeliveryMode::VisualSubtitleBatches);
	wxColour const line(32, 224, 64), old_colour(240, 80, 32), new_colour(32, 80, 240);
	Vector2D const old_position(48, 64), new_position(144, 64);
	auto const old_snapshot = MakeDragGlyphSnapshot(presentation, DRAG_BIG_SQUARE, old_position, line, old_colour);
	auto const old_final = MakeSnapshotPacket(old_snapshot, VideoRenderDeliveryClass::VisualSubtitleFinal, 71, 201);
	RequireSnapshotPixel(presentation.Begin(71, old_snapshot), "old context did not begin");
	sink.on_frame_ready(old_final, old_final.time);
	RequireSnapshotPixel(callbacks == 0 && queue.Size() == 1, "reset fixture did not retain old queued packet");

	presentation.Reset();
	auto const fresh = MakeDragGlyphSnapshot(presentation, DRAG_BIG_SQUARE, new_position, line, new_colour);
	RequireSnapshotPixel(presentation.Begin(72, fresh), "replacement context did not begin");
	queue.PumpOne();
	RequireSnapshotPixel(callbacks == 1 && delivered && delivered->visual_tool_snapshot == old_snapshot && delivered->delivery_version == old_final.delivery_version, "reset lost the actual queued old snapshot");
	RequireSnapshotPixel(!presentation.Matches(delivered->visual_tool_snapshot), "reset accepted the old context");
	auto const pixels = RenderDragSnapshot(window, width, height, presentation.Select(delivered->visual_tool_snapshot));
	RequireGlyphLocationAndColour(pixels, width, height, new_position, new_colour, "queued_reset/fresh_baseline");
	RequireSnapshotPixel(pixels == RenderLegacyDragGlyph(window, width, height, DRAG_BIG_SQUARE, new_position, line, new_colour),
						 "old queued snapshot overrode fresh context pixels");
	RequireSnapshotPixel(!presentation.OnFinalPresented(71) && presentation.InteractionId() == 72,
						 "old context's Final retired the replacement interaction");
	RequireSnapshotPixel(queue.Size() == 0, "reset left a queued callback");
	std::cout << "drag_snapshot_queued_reset old_context_rejected=1 fresh_geometry_colour=1 late_final_ignored=1\n";
	return true;
}

template<class RendererFactory>
bool ValidateSceneCacheVisualToolSequence(
	RendererFactory const& factory,
	char const* renderer_name) {
	constexpr int width = 224;
	constexpr int height = 144;

	HiddenGLWindow window(width, height);
	window.MakeCurrent();
	auto const& gl = GetFramebufferFunctions();
	if (!gl.BindFramebuffer
		|| !gl.DeleteFramebuffers
		|| !gl.GenFramebuffers
		|| !gl.FramebufferTexture2D
		|| !gl.CheckFramebufferStatus) {
		std::cout << renderer_name << "/scene_cache_visual_tools skipped: FBO functions unavailable\n";
		return true;
	}

	auto renderer = factory();
	auto frame = MakeBgraScenario(width, height);
	renderer->UploadFrame(frame.frame);
	renderer->UploadOverlay(nullptr);

	GLuint scene_texture = 0;
	GLuint scene_framebuffer = 0;
	glGenTextures(1, &scene_texture);
	gl.GenFramebuffers(1, &scene_framebuffer);

	glBindTexture(GL_TEXTURE_2D, scene_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glBindTexture(GL_TEXTURE_2D, 0);

	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, scene_framebuffer);
	gl.FramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, scene_texture, 0);
	if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT)
		throw std::runtime_error("detached visual tools smoke could not create a scene cache framebuffer.");

	bool passed = true;
	for (int frame_index = 0; frame_index < 4; ++frame_index) {
		gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, scene_framebuffer);
		glDrawBuffer(GL_COLOR_ATTACHMENT0_EXT);
		glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
		renderer->Render({ 0, 0, width, height }, width, height);

		SetupWindowRenderTarget(width, height);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		legacy_gl::DrawTexturedQuad(scene_texture, width, height);

		SetupOverlayProjection(width, height);
		OpenGLWrapper glw;
		DrawVisualToolCross(glw, width, height, Vector2D(34 + frame_index * 41, 28 + frame_index * 17));
		glw.SetLineColour(wxColour(255, 32, 32), 1.0f, 3);
		glw.SetFillColour(wxColour(255, 32, 32), 1.0f);
		DrawVisualToolFeatureMarker(glw, Vector2D(70 + frame_index * 27, 84));

		auto pixels = window.ReadBackRgbaTopLeft();
		bool const video_left = PixelHasVideoSignal(pixels, width, width / 4, height / 2);
		bool const video_right = PixelHasVideoSignal(pixels, width, width * 3 / 4, height / 2);
		bool const feature_visible = PixelMatchesColor(
			pixels,
			width,
			{ "scene_feature", 70 + frame_index * 27, 84, 200, 0, 0, 255, 80, 80 });

		std::cout
			<< renderer_name << "/scene_cache_visual_tools frame=" << frame_index
			<< " video_left=" << video_left
			<< " video_right=" << video_right
			<< " feature=" << feature_visible
			<< "\n";

		passed = video_left && video_right && feature_visible && passed;
	}

	renderer->Reset();
	gl.BindFramebuffer(GL_FRAMEBUFFER_EXT, 0);
	gl.DeleteFramebuffers(1, &scene_framebuffer);
	glDeleteTextures(1, &scene_texture);
	return passed;
}

Vector2D ComputeFeaturePosition(
	VideoDisplayViewportLayout const& layout,
	Vector2D script_res,
	Vector2D script_position) {
	Vector2D video_pos(layout.viewport_left, layout.viewport_top);
	Vector2D video_res(layout.viewport_width, layout.viewport_height);
	return video_pos + script_position * video_res / script_res;
}

std::vector<PixelChangeExpectation> BuildChangeExpectations(
	OverlayScenario const& scenario) {
	int const mouse_x = static_cast<int>(std::lround(scenario.mouse_pos.X()));
	int const mouse_y = static_cast<int>(std::lround(scenario.mouse_pos.Y()));
	return {
		{ "left_cross", 4, mouse_y, 32 },
		{ "right_cross", scenario.canvas_width - 5, mouse_y, 32 },
		{ "top_cross", mouse_x, 4, 32 },
		{ "bottom_cross", mouse_x, scenario.canvas_height - 5, 32 }
	};
}

std::vector<PixelColorExpectation> BuildColorExpectations(
	OverlayScenario const& scenario,
	Vector2D feature_position) {
	int const feature_x = static_cast<int>(std::lround(feature_position.X()));
	int const feature_y = static_cast<int>(std::lround(feature_position.Y()));
	return {
		{ "feature_marker", feature_x, feature_y, 200, 0, 0, 255, 80, 80 }
	};
}

void PrintFailure(
	std::string const& renderer_name,
	OverlayScenario const& scenario,
	PixelColorExpectation const& expectation,
	std::vector<unsigned char> const& pixels) {
	auto const sample_index = static_cast<std::size_t>(expectation.y * scenario.canvas_width + expectation.x) * 4;
	std::cout
		<< renderer_name << "/" << scenario.name << " failed at " << expectation.label
		<< " (" << expectation.x << ", " << expectation.y << ")"
		<< " rgba=("
		<< static_cast<int>(pixels[sample_index + 0]) << ", "
		<< static_cast<int>(pixels[sample_index + 1]) << ", "
		<< static_cast<int>(pixels[sample_index + 2]) << ", "
		<< static_cast<int>(pixels[sample_index + 3]) << ")\n";
}

void PrintChangeFailure(
	std::string const& renderer_name,
	OverlayScenario const& scenario,
	PixelChangeExpectation const& expectation,
	std::vector<unsigned char> const& before,
	std::vector<unsigned char> const& after) {
	auto const sample_index = static_cast<std::size_t>(expectation.y * scenario.canvas_width + expectation.x) * 4;
	std::cout
		<< renderer_name << "/" << scenario.name << " failed at " << expectation.label
		<< " (" << expectation.x << ", " << expectation.y << ")"
		<< " before=("
		<< static_cast<int>(before[sample_index + 0]) << ", "
		<< static_cast<int>(before[sample_index + 1]) << ", "
		<< static_cast<int>(before[sample_index + 2]) << ", "
		<< static_cast<int>(before[sample_index + 3]) << ")"
		<< " after=("
		<< static_cast<int>(after[sample_index + 0]) << ", "
		<< static_cast<int>(after[sample_index + 1]) << ", "
		<< static_cast<int>(after[sample_index + 2]) << ", "
		<< static_cast<int>(after[sample_index + 3]) << ")"
		<< " min_delta=" << expectation.min_delta
		<< "\n";
}

template<class RendererFactory>
bool ValidateOverlayScenario(
	RendererFactory const& factory,
	char const* renderer_name,
	OverlayScenario const& scenario) {
	HiddenGLWindow window(scenario.canvas_width, scenario.canvas_height);
	window.MakeCurrent();
	auto renderer = factory();

	auto frame = MakeBgraScenario(scenario.source_width, scenario.source_height);
	auto base_viewport = BuildVideoDisplayViewportLayout(
		scenario.canvas_width,
		scenario.canvas_height,
		scenario.canvas_width,
		scenario.canvas_height,
		true,
		scenario.target_display_aspect_ratio);
	auto viewport = BuildVideoDisplayContentLayout(
		base_viewport,
		scenario.canvas_height,
		scenario.transform.pan_x != 0.0 || scenario.transform.pan_y != 0.0,
		scenario.transform);
	Vector2D feature_position = ComputeFeaturePosition(viewport, scenario.script_res, scenario.feature_script_pos);

	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearStencil(0);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	renderer->UploadFrame(frame.frame);
	renderer->UploadOverlay(nullptr);
	renderer->Render(
		{ viewport.viewport_left, viewport.viewport_bottom, viewport.viewport_width, viewport.viewport_height },
		scenario.canvas_width,
		scenario.canvas_height);
	auto before_overlay = window.ReadBackRgbaTopLeft();

	SetupOverlayProjection(scenario.canvas_width, scenario.canvas_height);
	OpenGLWrapper gl;
	DrawVisualToolCross(gl, scenario.canvas_width, scenario.canvas_height, scenario.mouse_pos);
	gl.SetLineColour(wxColour(255, 32, 32), 1.0f, 3);
	gl.SetFillColour(wxColour(255, 32, 32), 1.0f);
	DrawVisualToolFeatureMarker(gl, feature_position);

	auto pixels = window.ReadBackRgbaTopLeft();
	bool passed = true;
	for (auto const& expectation : BuildChangeExpectations(scenario)) {
		if (!PixelChangedEnough(before_overlay, pixels, scenario.canvas_width, scenario.canvas_height, expectation)) {
			PrintChangeFailure(renderer_name, scenario, expectation, before_overlay, pixels);
			passed = false;
		}
	}
	for (auto const& expectation : BuildColorExpectations(scenario, feature_position)) {
		if (!PixelMatchesColor(pixels, scenario.canvas_width, expectation)) {
			PrintFailure(renderer_name, scenario, expectation, pixels);
			passed = false;
		}
	}
	if (passed)
		std::cout << renderer_name << "/" << scenario.name << " overlay=ok\n";
	renderer->Reset();
	return passed;
}

template<class RendererFactory>
bool ValidateRenderer(char const* renderer_name, RendererFactory&& factory, std::vector<OverlayScenario> const& scenarios) {
	bool passed = true;
	for (auto const& scenario : scenarios) {
		passed = ValidateOverlayScenario(factory, renderer_name, scenario) && passed;
	}
	return passed;
}

std::vector<OverlayScenario> BuildScenarios() {
	return {
		{
			"wide_panned_down",
			240,
			160,
			160,
			90,
			16.0 / 9.0,
			{ 1.0, 0.2, 0.25 },
			Vector2D(70, 36),
			Vector2D(192, 108),
			Vector2D(128, 64)
		},
		{
			"pillarboxed_panned_right",
			200,
			200,
			120,
			160,
			3.0 / 4.0,
			{ 1.0, 0.35, 0.0 },
			Vector2D(124, 90),
			Vector2D(160, 120),
			Vector2D(64, 72)
		}
	};
}
}

int main() try {
	std::cout.setf(std::ios::unitbuf);
	std::cerr.setf(std::ios::unitbuf);
	_set_se_translator(SehTranslator);
	agi::log::log = new agi::log::LogSink;

	auto scenarios = BuildScenarios();
	bool passed = true;
	passed = ValidateXYPerspectiveBatchNearPlane() && passed;
	passed = ValidateXYOwnedPass() && passed;
	passed = ValidateXYOwnedPassStackBoundaries() && passed;
	passed = ValidateXYOwnedPassFinishError() && passed;
	passed = ValidateXYOwnedBatchEntryChanges() && passed;
	passed = ValidateXYOwnedPendingEntryError() && passed;
	passed = ValidateXYOwnedPassBoundaries() && passed;
	passed = ValidateXYOwnedWithoutPerspective() && passed;
	passed = ValidateXYStackFull() && passed;
	passed = ValidateXYClipPlaneState() && passed;
	passed = ValidateXYHostInheritedClipAndFog() && passed;
	passed = ValidateXYInvalidPerspectiveInput() && passed;
	passed = ValidateXYPreparedPerspectiveBatches() && passed;
	passed = ValidateXYClippedGradient() && passed;
	passed = ValidateXYClippedGradientContrast() && passed;
	passed = ValidateRotateXYOverlayPixels() && passed;
	passed = ValidateDragSnapshotGlyphPixels() && passed;
	passed = ValidateDragSnapshotPresentationPixels() && passed;
	passed = ValidateQueuedDragFinalPixels() && passed;
	passed = ValidateQueuedDragResetPixels() && passed;
	passed = ValidateXYColorMaterialResetOrder() && passed;

	passed = ValidateRenderer(
		"opengl",
		[] { return std::make_unique<OpenGLVideoRenderer>(true, false, true); },
		scenarios) && passed;
	passed = ValidateSceneCacheVisualToolSequence(
		[] { return std::make_unique<OpenGLVideoRenderer>(true, false, true); },
		"opengl") && passed;

#ifdef WITH_LIBPLACEBO
	passed = ValidateRenderer(
		"placebo",
		[] { return std::make_unique<PlaceboRendererGL>(); },
		scenarios) && passed;
	passed = ValidateSceneCacheVisualToolSequence(
		[] { return std::make_unique<PlaceboRendererGL>(); },
		"placebo") && passed;
#endif

	delete agi::log::log;
	agi::log::log = nullptr;
	return passed ? 0 : 1;
}
catch (std::exception const& err) {
	std::cerr << "detached-visual-tools-smoke failed: " << err.what() << std::endl;
	delete agi::log::log;
	agi::log::log = nullptr;
	return 1;
}
catch (agi::Exception const& err) {
	std::cerr << "detached-visual-tools-smoke failed: " << err.GetMessage() << std::endl;
	delete agi::log::log;
	agi::log::log = nullptr;
	return 1;
}
catch (...) {
	std::cerr << "detached-visual-tools-smoke failed: unknown exception" << std::endl;
	delete agi::log::log;
	agi::log::log = nullptr;
	return 1;
}

#else

#include <iostream>

int main() {
	std::cout << "detached-visual-tools-smoke is currently only implemented on Windows builds." << std::endl;
	return 0;
}

#endif
