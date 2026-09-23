#include "../../src/source_frame.h"
#include "../../src/subtitle_overlay.h"
#include "../../src/video_render_geometry.h"
#include "../../src/video_renderer_error.h"

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <eh.h>
#ifdef GetMessage
#undef GetMessage
#endif

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "../../src/video_renderer_opengl.h"

#include <libaegisub/log.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

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
using WglCreateContextAttribsArbProc = HGLRC (WINAPI *)(HDC, HGLRC, int const*);

void SehTranslator(unsigned int code, EXCEPTION_POINTERS*) {
	char buffer[64];
	std::snprintf(buffer, sizeof(buffer), "SEH exception 0x%08X", code);
	throw std::runtime_error(buffer);
}

struct Rgba8 {
	unsigned char r = 0;
	unsigned char g = 0;
	unsigned char b = 0;
	unsigned char a = 255;
};

struct ValidationResult {
	std::string name;
	int max_abs_error = 0;
	double mean_abs_error = 0.0;
	int compared_pixels = 0;
	int max_x = 0;
	int max_y = 0;
	int max_channel = 0;
	std::array<int, 4> reference_rgba = { { 0, 0, 0, 0 } };
	std::array<int, 4> candidate_rgba = { { 0, 0, 0, 0 } };
};

struct ActiveBounds {
	bool valid = false;
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
	int non_zero_alpha_pixels = 0;
};

struct SecondaryOverlayScenario {
	std::string name;
	int source_width = 0;
	int source_height = 0;
	SourceFrameGeometry geometry = { };
	SourceFrameRect patch_rect = { };
	Rgba8 color = { 32, 255, 32, 255 };
};

struct EquivalentOverlayPair {
	SubtitleOverlayStorage storage_overlay_storage;
	SubtitleOverlayStorage visible_overlay_storage;
	SubtitleOverlay storage_overlay;
	SubtitleOverlay visible_overlay;
};

class HiddenGLWindow {
	HWND hwnd = nullptr;
	HDC dc = nullptr;
	HGLRC context = nullptr;
	int width = 0;
	int height = 0;

	static char const *WindowClassName() {
		return "AegisubOpenGLOverlaySmokeWindow";
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
			throw std::runtime_error("RegisterClassA failed for hidden OpenGL smoke window.");

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
			"Aegisub OpenGL overlay smoke",
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
			throw std::runtime_error("CreateWindowExA failed for hidden OpenGL smoke window.");

		dc = GetDC(hwnd);
		if (!dc)
			throw std::runtime_error("GetDC failed for hidden OpenGL smoke window.");

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
			throw std::runtime_error("ChoosePixelFormat failed for hidden OpenGL smoke window.");
		if (!SetPixelFormat(dc, pixel_format, &pfd))
			throw std::runtime_error("SetPixelFormat failed for hidden OpenGL smoke window.");

		HGLRC legacy_context = wglCreateContext(dc);
		if (!legacy_context)
			throw std::runtime_error("wglCreateContext failed for hidden OpenGL smoke window.");

		if (!wglMakeCurrent(dc, legacy_context))
			throw std::runtime_error("wglMakeCurrent failed for hidden OpenGL smoke window.");

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
					throw std::runtime_error("wglMakeCurrent failed for modern hidden OpenGL smoke window.");
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
			throw std::runtime_error("wglMakeCurrent failed for hidden OpenGL smoke window.");
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

struct NativeFrameStorage {
	std::vector<unsigned char> plane0;
	std::vector<unsigned char> plane1;
};

ValidationResult CompareRgbaImages(
	std::string name,
	std::vector<unsigned char> const& reference,
	std::vector<unsigned char> const& candidate,
	int width,
	int height) {
	ValidationResult result;
	result.name = std::move(name);

	if (reference.size() != candidate.size()) {
		result.max_abs_error = 255;
		return result;
	}

	double total_abs = 0.0;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			std::size_t base = (static_cast<std::size_t>(y) * width + x) * 4;
			for (int channel = 0; channel < 4; ++channel) {
				int ref = reference[base + channel];
				int got = candidate[base + channel];
				int abs_err = std::abs(ref - got);
				total_abs += abs_err;
				++result.compared_pixels;
				if (abs_err > result.max_abs_error) {
					result.max_abs_error = abs_err;
					result.max_x = x;
					result.max_y = y;
					result.max_channel = channel;
					for (int i = 0; i < 4; ++i) {
						result.reference_rgba[static_cast<std::size_t>(i)] = reference[base + i];
						result.candidate_rgba[static_cast<std::size_t>(i)] = candidate[base + i];
					}
				}
			}
		}
	}

	result.mean_abs_error = total_abs / static_cast<double>(reference.size());
	return result;
}

ActiveBounds FindActiveBounds(
	std::vector<unsigned char> const& pixels,
	int width,
	int height) {
	ActiveBounds bounds;
	bounds.x0 = width;
	bounds.y0 = height;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			std::size_t base = (static_cast<std::size_t>(y) * width + x) * 4;
			if (pixels[base + 3] == 0)
				continue;

			bounds.valid = true;
			bounds.non_zero_alpha_pixels++;
			bounds.x0 = std::min(bounds.x0, x);
			bounds.y0 = std::min(bounds.y0, y);
			bounds.x1 = std::max(bounds.x1, x + 1);
			bounds.y1 = std::max(bounds.y1, y + 1);
		}
	}

	if (!bounds.valid) {
		bounds.x0 = 0;
		bounds.y0 = 0;
	}
	return bounds;
}

bool IsStablePixel(
	std::vector<unsigned char> const& pixels,
	int width,
	int height,
	int x,
	int y) {
	if (x <= 0 || y <= 0 || x >= width - 1 || y >= height - 1)
		return false;

	auto same_pixel = [&](int other_x, int other_y) {
		std::size_t base = (static_cast<std::size_t>(y) * width + x) * 4;
		std::size_t other = (static_cast<std::size_t>(other_y) * width + other_x) * 4;
		return pixels[base + 0] == pixels[other + 0]
			&& pixels[base + 1] == pixels[other + 1]
			&& pixels[base + 2] == pixels[other + 2]
			&& pixels[base + 3] == pixels[other + 3];
	};

	return same_pixel(x - 1, y)
		&& same_pixel(x + 1, y)
		&& same_pixel(x, y - 1)
		&& same_pixel(x, y + 1);
}

ValidationResult CompareRgbaImagesOnStableMask(
	std::string name,
	std::vector<unsigned char> const& mask_source,
	std::vector<unsigned char> const& reference,
	std::vector<unsigned char> const& candidate,
	int width,
	int height) {
	ValidationResult result;
	result.name = std::move(name);

	if (mask_source.size() != reference.size() || reference.size() != candidate.size()) {
		result.max_abs_error = 255;
		return result;
	}

	double total_abs = 0.0;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			if (!IsStablePixel(mask_source, width, height, x, y))
				continue;

			std::size_t base = (static_cast<std::size_t>(y) * width + x) * 4;
			for (int channel = 0; channel < 4; ++channel) {
				int ref = reference[base + channel];
				int got = candidate[base + channel];
				int abs_err = std::abs(ref - got);
				total_abs += abs_err;
				++result.compared_pixels;
				if (abs_err > result.max_abs_error) {
					result.max_abs_error = abs_err;
					result.max_x = x;
					result.max_y = y;
					result.max_channel = channel;
					for (int i = 0; i < 4; ++i) {
						result.reference_rgba[static_cast<std::size_t>(i)] = reference[base + i];
						result.candidate_rgba[static_cast<std::size_t>(i)] = candidate[base + i];
					}
				}
			}
		}
	}

	if (result.compared_pixels > 0)
		result.mean_abs_error = total_abs / static_cast<double>(result.compared_pixels);
	else
		result.max_abs_error = 255;
	return result;
}

void PrintValidationResult(ValidationResult const& result) {
	std::cout
		<< result.name
		<< " compared=" << result.compared_pixels
		<< " max_abs=" << result.max_abs_error
		<< " mean_abs=" << result.mean_abs_error
		<< " worst=(" << result.max_x << "," << result.max_y << "," << result.max_channel << ")"
		<< " ref=("
		<< result.reference_rgba[0] << ","
		<< result.reference_rgba[1] << ","
		<< result.reference_rgba[2] << ","
		<< result.reference_rgba[3] << ")"
		<< " got=("
		<< result.candidate_rgba[0] << ","
		<< result.candidate_rgba[1] << ","
		<< result.candidate_rgba[2] << ","
		<< result.candidate_rgba[3] << ")"
		<< "\n";
}

SubtitleOverlayStorage MakeSolidOverlayStorage(int width, int height, Rgba8 color) {
	SubtitleOverlayStorage storage;
	storage.Reset(width, height, false);
	storage.has_visible_content = true;
	for (int y = 0; y < height; ++y) {
		auto* row = storage.pixels.data() + static_cast<std::size_t>(y) * storage.pitch;
		for (int x = 0; x < width; ++x) {
			auto* pixel = row + static_cast<std::size_t>(x) * 4;
			pixel[0] = color.b;
			pixel[1] = color.g;
			pixel[2] = color.r;
			pixel[3] = color.a;
		}
	}
	return storage;
}

SourceFrame MakeNativeNv12SourceFrame(
	int width,
	int height,
	SourceFrameGeometry geometry,
	NativeFrameStorage& storage) {
	auto format = MakeSemiplanar420SourceFrameFormatInfo(8, 1, 2);
	int chroma_width = GetSourceFramePlaneWidth(format, width, 1);
	int chroma_height = GetSourceFramePlaneHeight(format, height, 1);
	storage.plane0.assign(static_cast<std::size_t>(width) * height, 64);
	storage.plane1.assign(static_cast<std::size_t>(chroma_width) * chroma_height * 2, 128);

	SourceFrame frame;
	frame.output_mode = SourceFrameOutputMode::Native;
	frame.native_format = { SourceFrameNativeFormatNamespace::FFmpegAVPixelFormat, 23 };
	frame.format_info = format;
	frame.width = width;
	frame.height = height;
	frame.plane_count = format.plane_count;
	frame.geometry = geometry;
	frame.color = SourceFrameColorMetadataFromLegacyColorSpace("TV.709");
	frame.planes[0] = { storage.plane0.data(), width, width, height };
	frame.planes[1] = {
		storage.plane1.data(),
		chroma_width * format.planes[1].bytes_per_sample,
		chroma_width,
		chroma_height
	};
	return frame;
}

EquivalentOverlayPair BuildEquivalentOverlayPair(SecondaryOverlayScenario const& scenario) {
	auto const visible = GetSourceFrameVisibleRect(
		scenario.geometry,
		scenario.source_width,
		scenario.source_height);
	int const patch_x0 = scenario.patch_rect.x;
	int const patch_y0 = scenario.patch_rect.y;
	int const patch_x1 = scenario.patch_rect.x + scenario.patch_rect.width;
	int const patch_y1 = scenario.patch_rect.y + scenario.patch_rect.height;
	int const clip_x0 = std::max(patch_x0, visible.x);
	int const clip_y0 = std::max(patch_y0, visible.y);
	int const clip_x1 = std::min(patch_x1, visible.x + visible.width);
	int const clip_y1 = std::min(patch_y1, visible.y + visible.height);
	if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1)
		throw std::runtime_error("Secondary overlay scenario does not intersect visible rect: " + scenario.name);

	EquivalentOverlayPair overlays;
	overlays.storage_overlay_storage = MakeSolidOverlayStorage(
		scenario.patch_rect.width,
		scenario.patch_rect.height,
		scenario.color);
	overlays.storage_overlay = overlays.storage_overlay_storage.MakeView(true);
	overlays.storage_overlay.canvas_width = scenario.source_width;
	overlays.storage_overlay.canvas_height = scenario.source_height;
	overlays.storage_overlay.target_x = scenario.patch_rect.x;
	overlays.storage_overlay.target_y = scenario.patch_rect.y;
	overlays.storage_overlay.coordinate_space = SubtitleOverlayCoordinateSpace::SourceStorage;
	overlays.storage_overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	overlays.visible_overlay_storage = MakeSolidOverlayStorage(
		clip_x1 - clip_x0,
		clip_y1 - clip_y0,
		scenario.color);
	overlays.visible_overlay = overlays.visible_overlay_storage.MakeView(true);
	overlays.visible_overlay.canvas_width = visible.width;
	overlays.visible_overlay.canvas_height = visible.height;
	overlays.visible_overlay.target_x = clip_x0 - visible.x;
	overlays.visible_overlay.target_y = clip_y0 - visible.y;
	overlays.visible_overlay.coordinate_space = SubtitleOverlayCoordinateSpace::SourceVisible;
	overlays.visible_overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;

	return overlays;
}

SubtitleOverlay BuildStorageOverlay(SecondaryOverlayScenario const& scenario, SubtitleOverlayStorage& storage) {
	storage = MakeSolidOverlayStorage(
		scenario.patch_rect.width,
		scenario.patch_rect.height,
		scenario.color);
	auto overlay = storage.MakeView(true);
	overlay.canvas_width = scenario.source_width;
	overlay.canvas_height = scenario.source_height;
	overlay.target_x = scenario.patch_rect.x;
	overlay.target_y = scenario.patch_rect.y;
	overlay.coordinate_space = SubtitleOverlayCoordinateSpace::SourceStorage;
	overlay.composition_mode = SubtitleOverlayCompositionMode::PremultipliedAlpha;
	return overlay;
}

std::vector<unsigned char> RenderSecondaryOverlayScenario(
	SecondaryOverlayScenario const& scenario,
	SubtitleOverlay const* overlay) {
	auto const display_rect = GetSourceFrameDisplayOutputRect(scenario.geometry);
	if (display_rect.width <= 0 || display_rect.height <= 0)
		throw std::runtime_error("Invalid display rect for secondary overlay scenario: " + scenario.name);

	HiddenGLWindow window(display_rect.width, display_rect.height);
	OpenGLVideoRenderer renderer(false, true, true);
	window.MakeCurrent();

	NativeFrameStorage native_storage;
	auto source = MakeNativeNv12SourceFrame(
		scenario.source_width,
		scenario.source_height,
		scenario.geometry,
		native_storage);

	renderer.UploadFrame(source);
	renderer.UploadOverlay(overlay);
	renderer.Render(
		{ 0, 0, display_rect.width, display_rect.height },
		display_rect.width,
		display_rect.height);
	auto result = window.ReadBackRgbaTopLeft();
	window.MakeCurrent();
	renderer.Reset();
	return result;
}

bool ValidateEquivalentOverlayScenario(SecondaryOverlayScenario const& scenario) {
	std::cout << "Validation for OpenGL secondary overlay transform: " << scenario.name << "\n";

	auto overlays = BuildEquivalentOverlayPair(scenario);
	auto const display_rect = GetSourceFrameDisplayOutputRect(scenario.geometry);
	auto actual_storage = RenderSecondaryOverlayScenario(scenario, &overlays.storage_overlay);
	auto actual_visible = RenderSecondaryOverlayScenario(scenario, &overlays.visible_overlay);
	auto storage_bounds = FindActiveBounds(actual_storage, display_rect.width, display_rect.height);
	auto visible_bounds = FindActiveBounds(actual_visible, display_rect.width, display_rect.height);
	auto full_parity_result = CompareRgbaImages(
		scenario.name + "/parity/full",
		actual_storage,
		actual_visible,
		display_rect.width,
		display_rect.height);
	auto stable_parity_result = CompareRgbaImagesOnStableMask(
		scenario.name + "/parity/stable",
		actual_storage,
		actual_storage,
		actual_visible,
		display_rect.width,
		display_rect.height);
	std::cout
		<< scenario.name << "/storage bounds="
		<< storage_bounds.x0 << "," << storage_bounds.y0 << " -> "
		<< storage_bounds.x1 << "," << storage_bounds.y1
		<< " alpha_pixels=" << storage_bounds.non_zero_alpha_pixels << "\n";
	std::cout
		<< scenario.name << "/visible bounds="
		<< visible_bounds.x0 << "," << visible_bounds.y0 << " -> "
		<< visible_bounds.x1 << "," << visible_bounds.y1
		<< " alpha_pixels=" << visible_bounds.non_zero_alpha_pixels << "\n";
	PrintValidationResult(full_parity_result);
	PrintValidationResult(stable_parity_result);
	return storage_bounds.valid
		&& visible_bounds.valid
		&& storage_bounds.x0 == visible_bounds.x0
		&& storage_bounds.y0 == visible_bounds.y0
		&& storage_bounds.x1 == visible_bounds.x1
		&& storage_bounds.y1 == visible_bounds.y1
		&& storage_bounds.non_zero_alpha_pixels == visible_bounds.non_zero_alpha_pixels
		&& stable_parity_result.compared_pixels > 0
		&& stable_parity_result.max_abs_error == 0;
}

bool ValidateHiddenOutsideVisibleScenario() {
	SecondaryOverlayScenario scenario;
	scenario.name = "secondary/outside_hidden";
	scenario.source_width = 15;
	scenario.source_height = 11;
	scenario.geometry = MakeDefaultSourceFrameGeometry(15, 11);
	scenario.geometry.visible_rect = { 4, 3, 9, 6 };
	scenario.geometry.rotation = 90;
	scenario.patch_rect = { 0, 0, 3, 2 };

	std::cout << "Validation for OpenGL secondary overlay transform: " << scenario.name << "\n";
	SubtitleOverlayStorage storage_overlay_storage;
	auto storage_overlay = BuildStorageOverlay(scenario, storage_overlay_storage);
	auto const display_rect = GetSourceFrameDisplayOutputRect(scenario.geometry);
	auto actual_hidden = RenderSecondaryOverlayScenario(scenario, &storage_overlay);
	auto actual_none = RenderSecondaryOverlayScenario(scenario, nullptr);
	auto hidden_bounds = FindActiveBounds(actual_hidden, display_rect.width, display_rect.height);
	auto none_bounds = FindActiveBounds(actual_none, display_rect.width, display_rect.height);
	auto parity = CompareRgbaImages(
		scenario.name + "/hidden/full",
		actual_none,
		actual_hidden,
		display_rect.width,
		display_rect.height);
	std::cout
		<< scenario.name << "/hidden bounds="
		<< hidden_bounds.x0 << "," << hidden_bounds.y0 << " -> "
		<< hidden_bounds.x1 << "," << hidden_bounds.y1
		<< " alpha_pixels=" << hidden_bounds.non_zero_alpha_pixels << "\n";
	PrintValidationResult(parity);
	return !hidden_bounds.valid
		&& !none_bounds.valid
		&& parity.max_abs_error == 0;
}

bool ValidateExpectedOverlayRectangle(
	std::string const& name,
	std::vector<unsigned char> const& actual,
	int width,
	int height,
	SourceFrameRect const& expected_rect,
	Rgba8 color) {
	std::vector<unsigned char> expected(static_cast<std::size_t>(width) * height * 4, 0);
	for (int y = expected_rect.y; y < expected_rect.y + expected_rect.height; ++y) {
		for (int x = expected_rect.x; x < expected_rect.x + expected_rect.width; ++x) {
			std::size_t base = ((static_cast<std::size_t>(y) * width) + x) * 4;
			expected[base + 0] = color.r;
			expected[base + 1] = color.g;
			expected[base + 2] = color.b;
			expected[base + 3] = color.a;
		}
	}
	auto comparison = CompareRgbaImages(name, expected, actual, width, height);
	auto bounds = FindActiveBounds(actual, width, height);
	PrintValidationResult(comparison);
	std::cout << name << "/bounds=" << bounds.x0 << "," << bounds.y0 << " -> "
			  << bounds.x1 << "," << bounds.y1 << " alpha_pixels=" << bounds.non_zero_alpha_pixels << "\n";
	return comparison.max_abs_error == 0 && bounds.valid && bounds.x0 == expected_rect.x && bounds.y0 == expected_rect.y && bounds.x1 == expected_rect.x + expected_rect.width && bounds.y1 == expected_rect.y + expected_rect.height && bounds.non_zero_alpha_pixels == expected_rect.width * expected_rect.height;
}

bool ValidateOverlayLayoutRefresh(
	std::string const& name,
	int rotation,
	bool display_vflip,
	SourceFrameRect const& first_expected_rect,
	SourceFrameRect const& changed_expected_rect) {
	constexpr int output_width = 6;
	constexpr int output_height = 8;
	HiddenGLWindow window(output_width, output_height);
	OpenGLVideoRenderer renderer(false, true, true);
	window.MakeCurrent();

	auto validate_stage = [&](SecondaryOverlayScenario const& scenario, SourceFrameRect const& expected_rect) {
		NativeFrameStorage native_storage;
		auto source = MakeNativeNv12SourceFrame(
			scenario.source_width, scenario.source_height, scenario.geometry, native_storage);
		SubtitleOverlayStorage overlay_storage;
		auto overlay = BuildStorageOverlay(scenario, overlay_storage);
		renderer.UploadFrame(source);
		renderer.UploadOverlay(&overlay);
		renderer.Render({.x = 0, .y = 0, .width = output_width, .height = output_height}, output_width, output_height);
		auto first = window.ReadBackRgbaTopLeft();
		bool passed = ValidateExpectedOverlayRectangle(
			scenario.name, first, output_width, output_height, expected_rect, scenario.color);

		renderer.UploadOverlay(&overlay);
		renderer.Render({.x = 0, .y = 0, .width = output_width, .height = output_height}, output_width, output_height);
		auto repeated = window.ReadBackRgbaTopLeft();
		auto stability = CompareRgbaImages(
			scenario.name + "/repeated", first, repeated, output_width, output_height);
		PrintValidationResult(stability);
		return stability.max_abs_error == 0 && passed;
	};

	SecondaryOverlayScenario scenario;
	scenario.name = name + "/first";
	scenario.source_width = 8;
	scenario.source_height = 6;
	scenario.geometry = MakeDefaultSourceFrameGeometry(8, 6);
	scenario.geometry.rotation = rotation;
	scenario.geometry.display_vflip = display_vflip;
	scenario.patch_rect = {.x = 1, .y = 1, .width = 2, .height = 3};
	bool passed = validate_stage(scenario, first_expected_rect);

	// Reuse the renderer while changing the canvas, patch size and offset. The
	// doubled source canvas is displayed at half scale in the same viewport.
	scenario.name = name + "/canvas_change";
	scenario.source_width = 16;
	scenario.source_height = 12;
	scenario.geometry = MakeDefaultSourceFrameGeometry(16, 12);
	scenario.geometry.rotation = rotation;
	scenario.geometry.display_vflip = display_vflip;
	scenario.patch_rect = {.x = 6, .y = 2, .width = 4, .height = 6};
	passed = validate_stage(scenario, changed_expected_rect) && passed;
	window.MakeCurrent();
	renderer.Reset();
	return passed;
}

bool RunSecondaryOverlayTransformValidation() {
	std::vector<SecondaryOverlayScenario> scenarios;

	SecondaryOverlayScenario full_identity;
	full_identity.name = "secondary/full_identity";
	full_identity.source_width = 11;
	full_identity.source_height = 9;
	full_identity.geometry = MakeDefaultSourceFrameGeometry(11, 9);
	full_identity.patch_rect = { 2, 2, 5, 3 };
	scenarios.push_back(full_identity);

	SecondaryOverlayScenario cropped_odd;
	cropped_odd.name = "secondary/crop_odd";
	cropped_odd.source_width = 13;
	cropped_odd.source_height = 11;
	cropped_odd.geometry = MakeDefaultSourceFrameGeometry(13, 11);
	cropped_odd.geometry.visible_rect = { 2, 1, 9, 7 };
	cropped_odd.patch_rect = { 1, 2, 7, 5 };
	scenarios.push_back(cropped_odd);

	SecondaryOverlayScenario rotate90;
	rotate90.name = "secondary/rotate90_crop";
	rotate90.source_width = 12;
	rotate90.source_height = 10;
	rotate90.geometry = MakeDefaultSourceFrameGeometry(12, 10);
	rotate90.geometry.visible_rect = { 2, 1, 8, 6 };
	rotate90.geometry.rotation = 90;
	rotate90.patch_rect = { 1, 2, 9, 5 };
	scenarios.push_back(rotate90);

	SecondaryOverlayScenario rotate270_vflip;
	rotate270_vflip.name = "secondary/rotate270_vflip_crop";
	rotate270_vflip.source_width = 17;
	rotate270_vflip.source_height = 13;
	rotate270_vflip.geometry = MakeDefaultSourceFrameGeometry(17, 13);
	rotate270_vflip.geometry.visible_rect = { 3, 2, 11, 7 };
	rotate270_vflip.geometry.rotation = 270;
	rotate270_vflip.geometry.display_vflip = true;
	rotate270_vflip.patch_rect = { 2, 3, 8, 5 };
	scenarios.push_back(rotate270_vflip);

	SecondaryOverlayScenario rotate180_vflip;
	rotate180_vflip.name = "secondary/rotate180_vflip_crop";
	rotate180_vflip.source_width = 14;
	rotate180_vflip.source_height = 10;
	rotate180_vflip.geometry = MakeDefaultSourceFrameGeometry(14, 10);
	rotate180_vflip.geometry.visible_rect = { 1, 1, 11, 7 };
	rotate180_vflip.geometry.rotation = 180;
	rotate180_vflip.geometry.display_vflip = true;
	rotate180_vflip.patch_rect = { 0, 2, 8, 4 };
	scenarios.push_back(rotate180_vflip);

	bool passed = true;
	for (auto const& scenario : scenarios)
		passed = ValidateEquivalentOverlayScenario(scenario) && passed;
	passed = ValidateHiddenOutsideVisibleScenario() && passed;
	// Independent raster expectations: clockwise 90 degrees maps (x, y) to
	// (height - y, x); 270 degrees followed by a display vflip maps it to (y, x).
	// All bounds align with output pixels, including the half-scale canvas change.
	passed = ValidateOverlayLayoutRefresh(
				 "secondary/rotate90_layout", 90, false,
				 {.x = 2, .y = 1, .width = 3, .height = 2},
				 {.x = 2, .y = 3, .width = 3, .height = 2}) &&
			 passed;
	passed = ValidateOverlayLayoutRefresh(
				 "secondary/rotate270_vflip_layout", 270, true,
				 {.x = 1, .y = 1, .width = 3, .height = 2},
				 {.x = 1, .y = 3, .width = 3, .height = 2}) &&
			 passed;
	return passed;
}
}

int main() try {
	std::cout.setf(std::ios::unitbuf);
	std::cerr.setf(std::ios::unitbuf);
	_set_se_translator(SehTranslator);
	agi::log::log = new agi::log::LogSink;

	bool passed = true;
	passed = RunSecondaryOverlayTransformValidation() && passed;
	delete agi::log::log;
	agi::log::log = nullptr;
	return passed ? 0 : 3;
}
catch (std::exception const& err) {
	std::cerr << "opengl-overlay-smoke failed: " << err.what() << std::endl;
	return 2;
}
catch (agi::Exception const& err) {
	std::cerr << "opengl-overlay-smoke failed: " << err.GetMessage() << std::endl;
	return 2;
}
catch (...) {
	std::cerr << "opengl-overlay-smoke failed: unknown exception" << std::endl;
	return 2;
}

#else

int main() {
	std::cout << "opengl-overlay-smoke is currently only implemented on Windows builds." << std::endl;
	return 0;
}

#endif
