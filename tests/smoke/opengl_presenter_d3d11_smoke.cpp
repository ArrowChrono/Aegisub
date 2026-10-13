#include "../../src/opengl_presenter_d3d11.h"
#include "../../src/video_render_opengl_proc_loader.h"

#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

void Require(bool condition, char const *message) {
	if (!condition) throw std::runtime_error(message);
}

class Window {
	HWND parent = nullptr;
	HWND canvas = nullptr;
	HDC dc = nullptr;
	HGLRC context = nullptr;

	public:
	Window(int x, int y, int width, int height) {
		WNDCLASSW cls{};
		cls.style = CS_OWNDC;
		cls.lpfnWndProc = DefWindowProcW;
		cls.hInstance = GetModuleHandleW(nullptr);
		cls.lpszClassName = L"AegisubDxgiPresentationSmoke";
		Require(RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "Window class registration failed");
		parent = CreateWindowW(cls.lpszClassName, L"Aegisub DXGI presentation verification", WS_POPUP | WS_VISIBLE,
							   x, y, width, height, nullptr, nullptr, cls.hInstance, nullptr);
		Require(parent != nullptr, "Parent window creation failed");
		canvas = CreateWindowW(cls.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
							   0, 0, width, height, parent, nullptr, cls.hInstance, nullptr);
		Require(canvas != nullptr, "OpenGL child creation failed");
		dc = GetDC(canvas);
		PIXELFORMATDESCRIPTOR pfd{};
		pfd.nSize = sizeof(pfd);
		pfd.nVersion = 1;
		pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		pfd.iPixelType = PFD_TYPE_RGBA;
		pfd.cColorBits = 24;
		pfd.cAlphaBits = 8;
		pfd.cStencilBits = 8;
		int format = ChoosePixelFormat(dc, &pfd);
		Require(format != 0 && SetPixelFormat(dc, format, &pfd), "OpenGL pixel format creation failed");
		context = wglCreateContext(dc);
		Require(context != nullptr, "OpenGL context creation failed");
		MakeCurrent();
		ShowWindow(parent, SW_SHOW);
		ShowWindow(parent, SW_SHOW);
		Require(IsWindowVisible(parent) != FALSE, "Verification window is not visible");
		SetForegroundWindow(parent);
	}
	~Window() {
		if (wglGetCurrentContext() == context) wglMakeCurrent(nullptr, nullptr);
		if (context) wglDeleteContext(context);
		if (dc) ReleaseDC(canvas, dc);
		if (parent) DestroyWindow(parent);
	}
	void MakeCurrent() { Require(wglMakeCurrent(dc, context) != FALSE, "OpenGL context activation failed"); }
	[[nodiscard]] void *Handle() const { return canvas; }
	void Resize(int width, int height) {
		Require(SetWindowPos(parent, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER), "Parent resize failed");
		Require(SetWindowPos(canvas, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER), "Canvas resize failed");
	}
	void Hide(bool hide) { ShowWindow(parent, hide ? SW_HIDE : SW_SHOW); }
	void NativePresent() { Require(SwapBuffers(dc) != FALSE, "Native OpenGL presentation failed"); }
	[[nodiscard]] POINT Origin() const {
		POINT point{};
		Require(ClientToScreen(canvas, &point) != FALSE, "Client origin query failed");
		return point;
	}
};

void Pump() {
	MSG message{};
	while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
}

void Draw(int width, int height, int frame) {
	glViewport(0, 0, width, height);
	glDisable(GL_DITHER);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	glEnable(GL_SCISSOR_TEST);
	auto clear = [](int x, int y, int w, int h, float r, float g, float b) {
		glScissor(x, y, w, h);
		glClearColor(r, g, b, 1);
		glClear(GL_COLOR_BUFFER_BIT);
	};
	clear(0, height / 2, width / 2, height - height / 2, 1, 0, 0);
	clear(width / 2, height / 2, width - width / 2, height - height / 2, 0, 1, 0);
	clear(0, 0, width / 2, height / 2, 0, 0, 1);
	clear(width / 2, 0, width - width / 2, height / 2, 1, 1, 0);
	clear(width / 3 + frame % 30, height / 3, 20, 20, 1, 1, 1);
	float const stamp = static_cast<float>(frame * 7 % 240 + 8) / 255.0f;
	clear(width / 2 - 5, height / 2 - 5, 10, 10, stamp, stamp, stamp);
	glDisable(GL_SCISSOR_TEST);
}

std::vector<unsigned char> Capture(Window const& window, int width, int height) {
	auto origin = window.Origin();
	HDC screen = GetDC(nullptr);
	Require(screen != nullptr, "Desktop capture DC failed");
	HDC memory = CreateCompatibleDC(screen);
	BITMAPINFO info{};
	info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	info.bmiHeader.biWidth = width;
	info.bmiHeader.biHeight = -height;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	void *bits = nullptr;
	HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
	Require(memory && bitmap && bits, "Desktop capture bitmap failed");
	auto previous = SelectObject(memory, bitmap);
	bool copied = BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY | CAPTUREBLT) != FALSE;
	std::vector<unsigned char> pixels(static_cast<unsigned char *>(bits), static_cast<unsigned char *>(bits) + width * height * 4);
	SelectObject(memory, previous);
	DeleteObject(bitmap);
	DeleteDC(memory);
	ReleaseDC(nullptr, screen);
	Require(copied, "Desktop capture failed");
	return pixels;
}

void CheckColors(std::vector<unsigned char> const& pixels, int width, int height, int frame) {
	auto matches = [&](int x, int y, int r, int g, int b) {
		auto pos = static_cast<std::size_t>(y * width + x) * 4;
		return std::abs(static_cast<int>(pixels[pos]) - b) < 12 && std::abs(static_cast<int>(pixels[pos + 1]) - g) < 12 && std::abs(static_cast<int>(pixels[pos + 2]) - r) < 12;
	};
	Require(matches(width / 4, height / 4, 255, 0, 0), "Displayed top-left red quadrant mismatches");
	Require(matches(width * 3 / 4, height / 4, 0, 255, 0), "Displayed top-right green quadrant mismatches");
	Require(matches(width / 4, height * 3 / 4, 0, 0, 255), "Displayed bottom-left blue quadrant mismatches");
	Require(matches(width * 3 / 4, height * 3 / 4, 255, 255, 0), "Displayed bottom-right yellow quadrant mismatches");
	Require(matches(width / 3 + frame % 30 + 10, height - height / 3 - 10, 255, 255, 255), "Displayed animated overlay mismatches");
	auto const center = static_cast<std::size_t>((height / 2) * width + width / 2) * 4;
	int const stamp = frame * 7 % 240 + 8;
	Require(std::abs(static_cast<int>(pixels[center]) - stamp) <= 2 && std::abs(static_cast<int>(pixels[center + 1]) - stamp) <= 2 && std::abs(static_cast<int>(pixels[center + 2]) - stamp) <= 2,
			"Displayed frame stamp is stale or its gray value mismatches");
}

void Save(std::filesystem::path const& path, std::vector<unsigned char> const& pixels, int width, int height) {
	std::ofstream stream(path, std::ios::binary);
	stream << "P6\n"
		   << width << " " << height << "\n255\n";
	for (std::size_t pos = 0; pos < pixels.size(); pos += 4) {
		char rgb[] = {static_cast<char>(pixels[pos + 2]), static_cast<char>(pixels[pos + 1]), static_cast<char>(pixels[pos])};
		stream.write(rgb, 3);
	}
	Require(stream.good(), "Presentation image artifact write failed");
}
}

int main(int argc, char **argv) try {
	Require(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE, "Per-monitor DPI awareness setup failed");
	Require(argc == 3, "Usage: video-presenter-d3d11-smoke <dxgi|native> <artifact-directory>");
	bool const native = std::string(argv[1]) == "native";
	Require(native || std::string(argv[1]) == "dxgi", "Unknown presentation mode");
	std::filesystem::path directory(argv[2]);
	std::filesystem::create_directories(directory);
	auto started = Clock::now();
	std::vector<double> durations;
	int assertions = 0;
	for (int cycle = 0; cycle < 2; ++cycle) {
		int width = cycle == 0 ? 960 : 640;
		int height = cycle == 0 ? 540 : 480;
		Window window(100, 100, width, height);
		std::unique_ptr<OpenGLPresenterD3D11> presenter;
		if (!native) {
			Require(OpenGLPresenterD3D11::IsSupportedForCurrentContext(), "DXGI OpenGL interoperability is unavailable");
			presenter = std::make_unique<OpenGLPresenterD3D11>(window.Handle());
		}
		for (int frame = 0; frame < 72; ++frame) {
			Require(Clock::now() - started < std::chrono::seconds(25), "Presentation smoke exceeded total timeout");
			Pump();
			if (frame == 24) {
				width += 160;
				height += 90;
				window.Resize(width, height);
			}
			if (frame == 48) {
				window.Hide(true);
				Pump();
				window.Hide(false);
				Pump();
			}
			window.MakeCurrent();
			glDisable(GL_FRAMEBUFFER_SRGB);
			Draw(width, height, frame);
			if (!native) glEnable(GL_FRAMEBUFFER_SRGB);
			GLint read_buffer = 0, draw_buffer = 0;
			glGetIntegerv(GL_READ_BUFFER, &read_buffer);
			glGetIntegerv(GL_DRAW_BUFFER, &draw_buffer);
			auto begin = Clock::now();
			if (native)
				window.NativePresent();
			else
				presenter->Present(width, height);
			if (!native) Require(glIsEnabled(GL_FRAMEBUFFER_SRGB) == GL_TRUE, "Presentation changed framebuffer sRGB state");
			durations.push_back(std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
			GLint restored_read = 0, restored_draw = 0;
			glGetIntegerv(GL_READ_BUFFER, &restored_read);
			glGetIntegerv(GL_DRAW_BUFFER, &restored_draw);
			Require(read_buffer == restored_read && draw_buffer == restored_draw, "Presentation changed GL read/draw buffer state");
			Require(glGetError() == GL_NO_ERROR, "Presentation left an OpenGL error");
			Sleep(35);
			if (frame == 20 || frame == 44 || frame == 68) {
				auto pixels = Capture(window, width, height);
				Save(directory / ("cycle-" + std::to_string(cycle) + "-frame-" + std::to_string(frame) + ".ppm"), pixels, width, height);
				CheckColors(pixels, width, height, frame);
				++assertions;
			}
		}
		presenter.reset();
	}
	std::ranges::sort(durations);
	std::ofstream report(directory / "result.json");
	report << R"({"passed":true,"mode":")" << argv[1] << R"(","frames":)" << durations.size()
		   << ",\"display_color_checks\":" << assertions << ",\"present_p95_ms\":" << durations[durations.size() * 95 / 100]
		   << ",\"present_max_ms\":" << durations.back() << "}\n";
	Require(report.good(), "Presentation report write failed");
	std::cout << "PASS: 144 presented frames, resize/hide/recreate, six displayed quadrant checks\n";
	return 0;
}
catch (std::exception const& error) {
	std::cerr << "FAIL: " << error.what() << '\n';
	return 1;
}
