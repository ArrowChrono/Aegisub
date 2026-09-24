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
#include "../../src/video_overlay_draw_context_legacy_gl.h"
#include "../../src/video_renderer_error.h"
#include "../../src/video_renderer_opengl.h"
#include "../../src/visual_feature.h"
#include "../../src/visual_tool_drag_snapshot.h"
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
#include <iostream>
#include <memory>
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
	}

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
	passed = ValidateDragSnapshotGlyphPixels() && passed;
	passed = ValidateDragSnapshotPresentationPixels() && passed;
	passed = ValidateQueuedDragFinalPixels() && passed;
	passed = ValidateQueuedDragResetPixels() && passed;

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
