#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#ifdef DrawText
#undef DrawText
#endif
#ifdef GetMessage
#undef GetMessage
#endif

#include <libaegisub/charset_conv.h>

#include "../../src/gl_wrap.h"
#include "../../src/video_overlay_draw_context_opengl.h"
#include "../../src/video_overlay_text_win.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using Pixels = std::vector<unsigned char>;
constexpr int logical_width = 384;
constexpr int logical_height = 256;

void Require(bool condition, std::string const& message) {
	if (!condition)
		throw std::runtime_error(message);
}

void Check(HRESULT result, char const *operation) {
	Require(SUCCEEDED(result), std::string(operation) + " failed: " + std::to_string(result));
}

template <typename Error, typename Action>
void RequireThrows(Action&& action, char const *message) {
	try {
		action();
	}
	catch (Error const&) {
		return;
	}
	throw std::runtime_error(message);
}

class Apartment final {
	public:
	Apartment() { Check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "CoInitializeEx"); }
	~Apartment() { CoUninitialize(); }
	Apartment(Apartment const&) = delete;
	Apartment& operator=(Apartment const&) = delete;
};

struct NativeFactories {
	ComPtr<IDWriteFactory> write;
	ComPtr<ID2D1Factory> d2d;
	ComPtr<IWICImagingFactory> imaging;

	NativeFactories() {
		Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
								  reinterpret_cast<IUnknown **>(write.GetAddressOf())),
			  "DWriteCreateFactory");
		Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()), "D2D1CreateFactory");
		Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
							   IID_PPV_ARGS(imaging.GetAddressOf())),
			  "CoCreateInstance WIC");
	}
};

class HiddenGlWindow final {
	HWND window = nullptr;
	HDC dc = nullptr;
	HGLRC context = nullptr;

	public:
	HiddenGlWindow() {
		WNDCLASSW cls{};
		cls.style = CS_OWNDC;
		cls.lpfnWndProc = DefWindowProcW;
		cls.hInstance = GetModuleHandleW(nullptr);
		cls.lpszClassName = L"AegisubVideoOverlayTextSmoke";
		Require(RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "RegisterClassW failed");
		window = CreateWindowExW(0, cls.lpszClassName, L"", WS_POPUP, 0, 0,
								 logical_width * 2, logical_height * 2, nullptr, nullptr, cls.hInstance, nullptr);
		Require(window != nullptr, "CreateWindowExW failed");
		dc = GetDC(window);
		Require(dc != nullptr, "GetDC failed");
		PIXELFORMATDESCRIPTOR descriptor{};
		descriptor.nSize = sizeof(descriptor);
		descriptor.nVersion = 1;
		descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		descriptor.iPixelType = PFD_TYPE_RGBA;
		descriptor.cColorBits = 32;
		descriptor.cAlphaBits = 8;
		auto const format = ChoosePixelFormat(dc, &descriptor);
		Require(format && SetPixelFormat(dc, format, &descriptor), "SetPixelFormat failed");
		context = wglCreateContext(dc);
		Require(context != nullptr, "wglCreateContext failed");
		MakeCurrent();
	}

	~HiddenGlWindow() {
		if (wglGetCurrentContext() == context)
			wglMakeCurrent(nullptr, nullptr);
		if (context)
			wglDeleteContext(context);
		if (dc)
			ReleaseDC(window, dc);
		if (window)
			DestroyWindow(window);
	}

	HiddenGlWindow(HiddenGlWindow const&) = delete;
	HiddenGlWindow& operator=(HiddenGlWindow const&) = delete;
	void MakeCurrent() const { Require(wglMakeCurrent(dc, context), "wglMakeCurrent failed"); }
	[[nodiscard]] HGLRC Identity() const { return context; }
};

class CurrentContextGuard final {
	HDC const dc = wglGetCurrentDC();
	HGLRC const context = wglGetCurrentContext();

	public:
	~CurrentContextGuard() { wglMakeCurrent(dc, context); }
	CurrentContextGuard() = default;
	CurrentContextGuard(CurrentContextGuard const&) = delete;
	CurrentContextGuard& operator=(CurrentContextGuard const&) = delete;
};

std::wstring Wide(std::string const& input) {
	if (input.empty())
		return {};
	int const count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
	Require(count > 0, "UTF-8 fixture conversion failed");
	std::wstring result(static_cast<std::size_t>(count), L'\0');
	Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), result.data(), count) == count,
			"UTF-8 fixture conversion length changed");
	return result;
}

class MetricsOracle final {
	ComPtr<IDWriteFactory> factory;

	public:
	MetricsOracle() {
		Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
								  reinterpret_cast<IUnknown **>(factory.GetAddressOf())),
			  "DWriteCreateFactory");
	}

	[[nodiscard]] VideoOverlayTextExtent Measure(std::string const& text, VideoOverlayTextStyle const& style) const {
		if (text.empty())
			return {};
		auto face = Wide(style.face);
		if (face.empty())
			face = L"Segoe UI";
		ComPtr<IDWriteTextFormat> format;
		Check(factory->CreateTextFormat(face.c_str(), nullptr, style.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
										style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
										DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(style.size) * 96.0f / 72.0f,
										L"", format.GetAddressOf()),
			  "CreateTextFormat");
		Check(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "SetWordWrapping");
		auto const wide = Wide(text);
		ComPtr<IDWriteTextLayout> layout;
		Check(factory->CreateTextLayout(wide.data(), static_cast<UINT32>(wide.size()), format.Get(),
										1000000.0f, 1000000.0f, layout.GetAddressOf()),
			  "CreateTextLayout");
		DWRITE_TEXT_METRICS metrics{};
		Check(layout->GetMetrics(&metrics), "GetMetrics");
		return {static_cast<int>(std::ceil(metrics.widthIncludingTrailingWhitespace)), static_cast<int>(std::ceil(metrics.height))};
	}
};

void BeginFrame(double scale_x = 1.0, double scale_y = 1.0, float background = 0.0f) {
	int const width = static_cast<int>(std::lround(logical_width * scale_x));
	int const height = static_cast<int>(std::lround(logical_height * scale_y));
	glViewport(0, 0, width, height);
	glDrawBuffer(GL_BACK);
	glReadBuffer(GL_BACK);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, logical_width, logical_height, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glClearColor(background, background, background, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
}

Pixels ReadPixels(double scale_x = 1.0, double scale_y = 1.0) {
	int const width = static_cast<int>(std::lround(logical_width * scale_x));
	int const height = static_cast<int>(std::lround(logical_height * scale_y));
	Pixels bottom_up(static_cast<std::size_t>(width) * height * 4);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, bottom_up.data());
	Require(glGetError() == GL_NO_ERROR, "render/readback generated a GL error");
	Pixels top_down(bottom_up.size());
	for (int y = 0; y < height; ++y)
		std::copy_n(bottom_up.data() + static_cast<std::size_t>(height - y - 1) * width * 4, width * 4,
					top_down.data() + static_cast<std::size_t>(y) * width * 4);
	return top_down;
}

void WritePpm(std::filesystem::path const& path, Pixels const& pixels, int width, int height) {
	std::ofstream file(path, std::ios::binary);
	Require(static_cast<bool>(file), "could not create PPM artifact");
	file << "P6\n"
		 << width << ' ' << height << "\n255\n";
	for (std::size_t index = 0; index < pixels.size(); index += 4)
		file.write(reinterpret_cast<char const *>(pixels.data() + index), 3);
	file.close();
	Require(static_cast<bool>(file), "could not finish PPM artifact");
}

struct Bounds {
	int left = std::numeric_limits<int>::max();
	int top = std::numeric_limits<int>::max();
	int right = -1;
	int bottom = -1;
	std::size_t count = 0;
};

Bounds InkBounds(Pixels const& pixels, int width, unsigned char background = 0) {
	Bounds bounds;
	for (std::size_t index = 0; index < pixels.size(); index += 4) {
		bool ink = false;
		for (int channel = 0; channel < 3; ++channel)
			ink = ink || std::abs(static_cast<int>(pixels[index + channel]) - background) > 12;
		if (!ink)
			continue;
		int const x = static_cast<int>(index / 4 % width);
		int const y = static_cast<int>(index / 4 / width);
		bounds.left = std::min(bounds.left, x);
		bounds.top = std::min(bounds.top, y);
		bounds.right = std::max(bounds.right, x);
		bounds.bottom = std::max(bounds.bottom, y);
		++bounds.count;
	}
	return bounds;
}

struct GlState {
	std::array<GLint, 18> integers{};
	std::array<GLint, 4> scissor{};
	std::array<GLboolean, 10> booleans{};
	std::array<GLfloat, 4> colour{};
	std::array<GLdouble, 16> projection{};
	std::array<GLdouble, 16> modelview{};
	std::array<GLvoid *, 3> pointers{};

	static GlState Read() {
		GlState result;
		constexpr std::array<GLenum, 17> names{
			GL_TEXTURE_BINDING_2D, GL_BLEND_SRC, GL_BLEND_DST, GL_MATRIX_MODE,
			GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS,
			GL_VERTEX_ARRAY_SIZE, GL_VERTEX_ARRAY_TYPE, GL_VERTEX_ARRAY_STRIDE,
			GL_TEXTURE_COORD_ARRAY_SIZE, GL_TEXTURE_COORD_ARRAY_TYPE, GL_TEXTURE_COORD_ARRAY_STRIDE,
			GL_COLOR_ARRAY_SIZE, GL_COLOR_ARRAY_TYPE, GL_COLOR_ARRAY_STRIDE};
		for (std::size_t index = 0; index < names.size(); ++index)
			glGetIntegerv(names[index], &result.integers[index]);
		glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &result.integers[17]);
		glGetIntegerv(GL_SCISSOR_BOX, result.scissor.data());
		constexpr std::array<GLenum, 6> enabled{GL_TEXTURE_2D, GL_BLEND, GL_VERTEX_ARRAY, GL_TEXTURE_COORD_ARRAY, GL_COLOR_ARRAY, GL_SCISSOR_TEST};
		for (std::size_t index = 0; index < enabled.size(); ++index)
			result.booleans[index] = glIsEnabled(enabled[index]);
		glGetBooleanv(GL_COLOR_WRITEMASK, result.booleans.data() + 6);
		glGetFloatv(GL_CURRENT_COLOR, result.colour.data());
		glGetDoublev(GL_PROJECTION_MATRIX, result.projection.data());
		glGetDoublev(GL_MODELVIEW_MATRIX, result.modelview.data());
		glGetPointerv(GL_VERTEX_ARRAY_POINTER, &result.pointers[0]);
		glGetPointerv(GL_TEXTURE_COORD_ARRAY_POINTER, &result.pointers[1]);
		glGetPointerv(GL_COLOR_ARRAY_POINTER, &result.pointers[2]);
		return result;
	}
	bool operator==(GlState const&) const = default;
};

class Runner final {
	std::filesystem::path artifacts;
	std::vector<std::string> passed;
	std::string active;
	std::chrono::steady_clock::time_point const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);

	public:
	explicit Runner(std::filesystem::path directory) : artifacts(std::move(directory)) {
		std::filesystem::create_directories(artifacts);
	}
	void CheckDeadline() const { Require(std::chrono::steady_clock::now() < deadline, "native text suite exceeded 60-second deadline"); }
	template <typename Action>
	void Run(char const *name, Action&& action) {
		CheckDeadline();
		active = name;
		action();
		passed.emplace_back(name);
		std::cout << name << " passed\n";
		active.clear();
	}
	void Picture(char const *name, Pixels const& pixels, double scale_x = 1.0, double scale_y = 1.0) const {
		WritePpm(artifacts / (std::string(name) + ".ppm"), pixels,
				 static_cast<int>(std::lround(logical_width * scale_x)), static_cast<int>(std::lround(logical_height * scale_y)));
	}
	void Report(bool success) const {
		std::ofstream file(artifacts / "result.json");
		Require(static_cast<bool>(file), "could not create JSON result");
		file << R"({"suite":"video-overlay-text","success":)" << (success ? "true" : "false") << R"(,"passed":[)";
		for (std::size_t index = 0; index < passed.size(); ++index) {
			if (index)
				file << ',';
			file << '"' << passed[index] << '"';
		}
		file << "],\"failed\":";
		if (active.empty())
			file << "null";
		else
			file << '"' << active << '"';
		file << "}\n";
		file.close();
		Require(static_cast<bool>(file), "could not finish JSON result");
	}
};

void RunChecks(Runner& runner) {
	Apartment apartment;
	NativeFactories factories;
	MetricsOracle oracle;
	VideoOverlayTextStyle style;
	style.face = "Arial";
	style.size = 18;
	style.outline = false;
	std::string const unicode = "AV fi e\xCC\x81 \xE4\xB8\xAD\xE6\x96\x87 \xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A";
	OpenGLVideoOverlayText measurement(*factories.write.Get(), *factories.d2d.Get(), *factories.imaging.Get(), "Segoe UI");
	runner.Run("MeasurementWithoutContextMatchesDirectWrite", [&] {
		Require(wglGetCurrentContext() == nullptr, "fixture unexpectedly has a GL context");
		for (auto const& text : {std::string{}, std::string("AV  "), std::string("one\nsecond line"), unicode}) {
			Require(measurement.MeasureText(text, style) == oracle.Measure(text, style), "logical metrics differ from DirectWrite");
			Require(wglGetCurrentContext() == nullptr, "measurement activated a GL context");
		}
		Require(measurement.MeasureText("AV  ", style).width > measurement.MeasureText("AV", style).width,
				"trailing whitespace was omitted from measured width");
		Require(measurement.MeasureText("one\nsecond line", style).height > measurement.MeasureText("one", style).height,
				"multiline text omitted the second line");
		RequireThrows<std::logic_error>([&] { measurement.DrawText("A", 0, 0, style); }, "draw without a GL context did not fail");
	});
	runner.Run("FontStyleAndDefaultFamilyMatchDirectWrite", [&] {
		for (auto const& face : {std::string{}, std::string("Arial"), std::string("Courier New")}) {
			for (bool bold : {false, true}) {
				for (bool italic : {false, true}) {
					auto variant = style;
					variant.face = face;
					variant.bold = bold;
					variant.italic = italic;
					variant.size = bold ? 27 : 14;
					Require(measurement.MeasureText(unicode, variant) == oracle.Measure(unicode, variant),
							"font/style measurement differs from DirectWrite");
				}
			}
		}
	});
	runner.Run("InvalidSizesScalesAndUtf8AreRejected", [&] {
		for (int size : {0, -1}) {
			auto invalid = style;
			invalid.size = size;
			RequireThrows<std::invalid_argument>([&] { measurement.MeasureText("A", invalid); }, "invalid point size accepted");
		}
		for (double scale : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
			RequireThrows<std::invalid_argument>([&] { measurement.SetScale(scale, 1.0); }, "invalid x scale accepted");
			RequireThrows<std::invalid_argument>([&] { measurement.SetScale(1.0, scale); }, "invalid y scale accepted");
		}
		RequireThrows<agi::charset::BadInput>([&] { measurement.MeasureText(std::string("\xC3\x28", 2), style); }, "invalid UTF-8 accepted");
	});

	HiddenGlWindow owner;
	OpenGLVideoOverlayText text(*factories.write.Get(), *factories.d2d.Get(), *factories.imaging.Get(), "Segoe UI");
	OpenGLWrapper wrapper;
	OpenGLTextVideoOverlayDrawContext overlay(wrapper, text);
	runner.Run("ContextWithoutTextRendererRejectsTextOperations", [&] {
		OpenGLVideoOverlayDrawContext absent(wrapper);
		RequireThrows<std::logic_error>([&] { absent.MeasureText("A", style); }, "context without text renderer accepted measurement");
		RequireThrows<std::logic_error>([&] { absent.DrawText("A", 0, 0, style); }, "context without text renderer accepted drawing");
	});
	runner.Run("MeasurementAndScaleChangesLeaveGlErrorsUntouched", [&] {
		glEnable(static_cast<GLenum>(0xffffffffu));
		Require(text.MeasureText(unicode, style) == oracle.Measure(unicode, style), "measurement changed with a current GL context");
		text.SetScale(1.25, 1.5);
		Require(glGetError() == GL_INVALID_ENUM, "CPU-only text operations consumed a pending GL error");
		Require(wglGetCurrentContext() == owner.Identity(), "CPU-only text operations changed context");
		text.SetScale(1.0, 1.0);
	});
	runner.Run("EmptyTextAndFullyTransparentTextDrawNoPixels", [&] {
		BeginFrame();
		auto const clear = ReadPixels();
		overlay.DrawText("", 12, 16, style);
		Require(ReadPixels() == clear, "empty text changed the framebuffer");
		auto transparent = style;
		transparent.colour.a = 255;
		transparent.outline = true;
		overlay.DrawText(unicode, 12, 16, transparent);
		Require(ReadPixels() == clear, "fully transparent text or outline changed the framebuffer");
	});
	Pixels baseline;
	runner.Run("UnicodeMultilineAndTrailingWhitespaceRenderActualPixels", [&] {
		BeginFrame();
		overlay.DrawText(unicode + "\nSecond 42", 16, 20, style);
		auto const image = ReadPixels();
		auto const bounds = InkBounds(image, logical_width);
		auto const one_line = oracle.Measure(unicode, style);
		Require(bounds.count > 200 && bounds.top >= 20 && bounds.bottom > 20 + one_line.height,
				"Unicode multiline text did not render both lines at its logical origin");
		Require(bounds.right < 16 + oracle.Measure(unicode + "\nSecond 42", style).width + 3, "glyphs exceed the measured layout unexpectedly");
		runner.Picture("unicode-multiline", image);
		BeginFrame();
		overlay.DrawText("AV", 16, 20, style);
		auto const trimmed = ReadPixels();
		BeginFrame();
		overlay.DrawText("AV  ", 16, 20, style);
		Require(ReadPixels() == trimmed, "trailing whitespace changed visible glyph placement");
		BeginFrame();
		overlay.DrawText("Scale 42", 16, 20, style);
		baseline = ReadPixels();
		Require(InkBounds(baseline, logical_width).count > 100, "ASCII text contains no usable ink");
	});
	runner.Run("FontStyleSizeAndRgbChangesReachRenderedPixels", [&] {
		for (int variant_index = 0; variant_index < 4; ++variant_index) {
			auto variant = style;
			if (variant_index == 0)
				variant.bold = true;
			else if (variant_index == 1)
				variant.italic = true;
			else if (variant_index == 2)
				variant.face = "Courier New";
			else
				variant.size = 27;
			BeginFrame();
			overlay.DrawText("Scale 42", 16, 20, variant);
			auto const image = ReadPixels();
			Require(image != baseline && InkBounds(image, logical_width).count > 100,
					"font/style change did not reach rendered glyphs");
			if (variant_index == 3)
				Require(InkBounds(image, logical_width).bottom > InkBounds(baseline, logical_width).bottom,
						"larger point size did not increase rendered text height");
		}
		auto coloured = style;
		coloured.colour = {240, 120, 60};
		Require(overlay.MeasureText("Scale 42", coloured) == overlay.MeasureText("Scale 42", style),
				"RGB colour changed measurement");
		BeginFrame();
		overlay.DrawText("Scale 42", 16, 20, coloured);
		auto const image = ReadPixels();
		std::size_t interiors = 0;
		for (std::size_t index = 0; index < image.size(); index += 4) {
			if (baseline[index] > 250) {
				Require(std::abs(static_cast<int>(image[index]) - 240) <= 5 &&
							std::abs(static_cast<int>(image[index + 1]) - 120) <= 3 &&
							std::abs(static_cast<int>(image[index + 2]) - 60) <= 2,
						"RGB colour was not applied to glyph interiors");
				++interiors;
			}
		}
		Require(interiors > 20, "RGB fixture has no opaque glyph interiors");
	});
	runner.Run("FractionalAndAnisotropicDpiPreserveLogicalMetrics", [&] {
		auto const base_bounds = InkBounds(baseline, logical_width);
		auto const extent = overlay.MeasureText("Scale 42", style);
		struct Scale {
			char const *name;
			double x;
			double y;
		};
		for (auto const scale : {Scale{.name = "dpi-100", .x = 1.0, .y = 1.0}, Scale{.name = "dpi-125", .x = 1.25, .y = 1.25},
								 Scale{.name = "dpi-150", .x = 1.5, .y = 1.5}, Scale{.name = "dpi-200", .x = 2.0, .y = 2.0}, Scale{.name = "dpi-anisotropic", .x = 1.25, .y = 1.5}}) {
			text.SetScale(scale.x, scale.y);
			Require(overlay.MeasureText("Scale 42", style) == extent, "DPI scale changed logical text extent");
			BeginFrame(scale.x, scale.y);
			overlay.DrawText("Scale 42", 16, 20, style);
			auto const image = ReadPixels(scale.x, scale.y);
			auto const bounds = InkBounds(image, static_cast<int>(std::lround(logical_width * scale.x)));
			Require(bounds.count > 100, "scaled text disappeared");
			Require(std::abs(bounds.left / scale.x - base_bounds.left) <= 2.0 &&
						std::abs(bounds.right / scale.x - base_bounds.right) <= 2.0 &&
						std::abs(bounds.top / scale.y - base_bounds.top) <= 2.0 &&
						std::abs(bounds.bottom / scale.y - base_bounds.bottom) <= 2.0,
					"DPI scale changed the logical ink bounds");
			runner.Picture(scale.name, image, scale.x, scale.y);
		}
		text.SetScale(1.0, 1.0);
	});
	runner.Run("AssAlphaAndOutlineAffectPixelsWithoutChangingMetrics", [&] {
		auto const extent = overlay.MeasureText("Scale 42", style);
		auto half = style;
		half.colour.a = 128;
		BeginFrame();
		overlay.DrawText("Scale 42", 16, 20, half);
		auto const translucent = ReadPixels();
		std::size_t tested = 0;
		for (std::size_t index = 0; index < baseline.size(); index += 4) {
			if (baseline[index] > 220) {
				Require(std::abs(static_cast<int>(translucent[index]) * 255 - static_cast<int>(baseline[index]) * 127) <= 510,
						"ASS alpha was not applied to text fill");
				++tested;
			}
		}
		Require(tested > 20, "opacity fixture has no opaque glyph interiors");
		auto outlined = style;
		outlined.outline = true;
		Require(overlay.MeasureText("Scale 42", outlined) == extent, "outline changed logical measurement");
		BeginFrame(1.0, 1.0, 0.5f);
		overlay.DrawText("Scale 42", 16, 20, style);
		auto const unoutlined = ReadPixels();
		BeginFrame(1.0, 1.0, 0.5f);
		overlay.DrawText("Scale 42", 16, 20, outlined);
		auto const image = ReadPixels();
		auto const plain_bounds = InkBounds(unoutlined, logical_width, 128);
		auto const outlined_bounds = InkBounds(image, logical_width, 128);
		std::size_t black_outline = 0;
		for (std::size_t index = 0; index < image.size(); index += 4)
			if (image[index] < 100 && unoutlined[index] >= 126 && unoutlined[index] <= 129)
				++black_outline;
		Require(black_outline > 30 && outlined_bounds.count > plain_bounds.count,
				"outline did not add black pixels outside the original glyph mask");
		Require(outlined_bounds.left >= plain_bounds.left - 2 && outlined_bounds.right <= plain_bounds.right + 2,
				"one logical pixel outline expanded beyond its stated width");
		runner.Picture("outline", image);
		outlined.colour.a = 128;
		BeginFrame(1.0, 1.0, 0.5f);
		overlay.DrawText("Scale 42", 16, 20, outlined);
		auto const half_outline = ReadPixels();
		std::size_t tested_outline = 0;
		for (std::size_t index = 0; index < image.size(); index += 4) {
			if (image[index] < 30 && unoutlined[index] >= 126 && unoutlined[index] <= 129) {
				Require(half_outline[index] > 45 && half_outline[index] < 100,
						"ASS alpha was not applied to the black outline");
				++tested_outline;
			}
		}
		Require(tested_outline > 10, "outline opacity fixture lacks solid outline pixels");
	});
	runner.Run("TextRestoresCallerGlStateAndShapeInterleaving", [&] {
		BeginFrame();
		GLuint sentinel = 0;
		glGenTextures(1, &sentinel);
		glBindTexture(GL_TEXTURE_2D, sentinel);
		std::array<unsigned char, 4> const pixel{10, 20, 30, 255};
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
		std::array<GLfloat, 12> const vertices{};
		std::array<GLfloat, 8> const texcoords{};
		std::array<GLubyte, 16> const colours{};
		glEnable(GL_TEXTURE_2D);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE);
		glColor4f(0.2f, 0.3f, 0.4f, 0.7f);
		glVertexPointer(3, GL_FLOAT, 0, vertices.data());
		glTexCoordPointer(2, GL_FLOAT, 0, texcoords.data());
		glColorPointer(4, GL_UNSIGNED_BYTE, 0, colours.data());
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glEnableClientState(GL_COLOR_ARRAY);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 7);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, 1);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, 2);
		glColorMask(GL_TRUE, GL_FALSE, GL_TRUE, GL_FALSE);
		glMatrixMode(GL_PROJECTION);
		auto const before = GlState::Read();
		overlay.DrawText("State", 16, 20, style);
		Require(GlState::Read() == before, "drawing text changed caller GL state");
		Require(glGetError() == GL_NO_ERROR, "state-preserving draw generated a GL error");
		glDeleteTextures(1, &sentinel);
		auto draw_shapes = [&] {
			overlay.SetLineColour({255, 0, 0}, 1.0f, 1);
			overlay.SetFillColour({0, 255, 0}, 1.0f);
			overlay.DrawRectangle({8, 100}, {70, 145});
		};
		BeginFrame();
		draw_shapes();
		draw_shapes();
		auto const shapes = ReadPixels();
		runner.Picture("shape-interleave-reference", shapes);
		BeginFrame();
		draw_shapes();
		overlay.DrawText("Interleave", 90, 20, style);
		draw_shapes();
		auto const interleaved = ReadPixels();
		runner.Picture("shape-interleave-text", interleaved);
		for (int y = 90; y < 150; ++y)
			for (int x = 0; x < 80; ++x)
				for (int channel = 0; channel < 4; ++channel) {
					auto const index = static_cast<std::size_t>(y * logical_width + x) * 4 + channel;
					Require(shapes[index] == interleaved[index], "text changed shape rendering before or after its draw");
				}
		Require(InkBounds(interleaved, logical_width).count > InkBounds(shapes, logical_width).count, "interleaved text did not render");
	});
	runner.Run("OwnedOverlayPassPreservesScissorAndAlphaMask", [&] {
		BeginFrame();
		GLint max_clip_planes = 0;
		glGetIntegerv(GL_MAX_CLIP_PLANES, &max_clip_planes);
		Require(glGetError() == GL_NO_ERROR && max_clip_planes >= 6, "owning context lacks required clip planes");
		{
			OpenGLWindowOverlayPass pass(logical_width, logical_height, logical_width, logical_height, 1000.0, false, max_clip_planes);
			OpenGLTextVideoOverlayDrawContext owned_overlay(wrapper, pass, text);
			glEnable(GL_SCISSOR_TEST);
			glScissor(0, logical_height - 80, 80, 80);
			auto const before = GlState::Read();
			owned_overlay.DrawText("Clipped text spanning the viewport", 10, 20, style);
			Require(GlState::Read() == before && glIsEnabled(GL_SCISSOR_TEST), "text changed the owned pass state");
			auto const image = ReadPixels();
			auto const bounds = InkBounds(image, logical_width);
			Require(bounds.count > 100 && bounds.right < 80 && bounds.bottom < 80, "text ignored the owned scissor");
			for (std::size_t index = 3; index < image.size(); index += 4)
				Require(image[index] == 255, "text changed alpha while the owned pass disabled alpha writes");
			pass.Finish();
		}
	});
	runner.Run("WarmCacheAndBoundedStressPreserveGlyphPixels", [&] {
		auto const& expected = baseline;
		for (int iteration = 0; iteration < 512; ++iteration) {
			runner.CheckDeadline();
			auto variant = style;
			variant.bold = iteration % 2 != 0;
			variant.italic = iteration % 3 == 0;
			variant.size = 12 + iteration % 14;
			BeginFrame();
			overlay.DrawText("Cache " + std::to_string(iteration), 8, 8, variant);
			Require(glGetError() == GL_NO_ERROR, "cache stress generated a GL error");
		}
		BeginFrame();
		overlay.DrawText("Scale 42", 16, 20, style);
		Require(ReadPixels() == expected, "cache eviction or font change altered regenerated glyph pixels");
		BeginFrame();
		overlay.DrawText("Scale 42", 16, 20, style);
		Require(ReadPixels() == expected, "warm draw altered cached glyph pixels");
	});
	runner.Run("WrongContextRejectedAndScaleInvalidationIsCpuOnly", [&] {
		CurrentContextGuard restore_owner;
		HiddenGlWindow other;
		Require(other.Identity() != owner.Identity(), "fixture contexts are not distinct");
		RequireThrows<std::logic_error>([&] { overlay.DrawText("Wrong", 0, 0, style); }, "text draw accepted a foreign context");
		glEnable(static_cast<GLenum>(0xffffffffu));
		text.SetScale(1.5, 1.5);
		Require(text.MeasureText("Scale 42", style) == oracle.Measure("Scale 42", style), "foreign-context measurement failed");
		Require(glGetError() == GL_INVALID_ENUM, "scale invalidation touched the foreign GL context");
		Require(wglGetCurrentContext() == other.Identity(), "text operations switched away from the foreign context");
		owner.MakeCurrent();
		text.SetScale(1.0, 1.0);
		BeginFrame();
		overlay.DrawText("Scale 42", 16, 20, style);
		Require(ReadPixels() == baseline, "owner rendering failed after rejected context or scale change");
		Require(wglMakeCurrent(nullptr, nullptr), "could not detach owner context");
		RequireThrows<std::logic_error>([&] { overlay.DrawText("Missing", 0, 0, style); }, "bound text accepted a missing context");
		owner.MakeCurrent();
	});
	runner.Run("FreshRendererSurvivesContextRecreation", [&] {
		CurrentContextGuard restore_owner;
		for (int epoch = 0; epoch < 3; ++epoch) {
			{
				HiddenGlWindow fresh_context;
				OpenGLVideoOverlayText fresh_text(*factories.write.Get(), *factories.d2d.Get(), *factories.imaging.Get(), "Segoe UI");
				BeginFrame();
				fresh_text.DrawText("Scale 42", 16, 20, style);
				Require(ReadPixels() == baseline, "new context epoch changed text pixels");
			}
			owner.MakeCurrent();
		}
	});
}
}

int main(int argc, char **argv) {
	if (argc != 3 || std::string_view(argv[1]) != "--artifacts") {
		std::cerr << "usage: video-overlay-text-smoke --artifacts <directory>\n";
		return 2;
	}
	try {
		Runner runner{std::filesystem::path(argv[2])};
		try {
			RunChecks(runner);
			runner.Report(true);
			return 0;
		}
		catch (...) {
			runner.Report(false);
			throw;
		}
	}
	catch (std::exception const& error) {
		std::cerr << "video-overlay-text-smoke: " << error.what() << '\n';
		return 1;
	}
	catch (agi::Exception const& error) {
		std::cerr << "video-overlay-text-smoke: " << error.GetMessage() << '\n';
		return 1;
	}
}
