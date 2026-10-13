#include "opengl_presenter_d3d11.h"

#include "video_render_opengl_proc_loader.h"

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <GL/gl.h>
#include <GL/glext.h>

#include <charconv>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
using Microsoft::WRL::ComPtr;

template <typename Proc>
Proc LoadProc(char const *name, char const *alternative = nullptr) {
	auto proc = reinterpret_cast<Proc>(opengl::GetProcAddress(name));
	if (!proc && alternative)
		proc = reinterpret_cast<Proc>(opengl::GetProcAddress(alternative));
	return proc;
}

bool HasExtension(char const *extensions, char const *name) {
	if (!extensions)
		return false;
	auto const length = std::strlen(name);
	for (auto match = std::strstr(extensions, name); match; match = std::strstr(match + length, name)) {
		if ((match == extensions || match[-1] == ' ') && (match[length] == ' ' || match[length] == '\0'))
			return true;
	}
	return false;
}

void CheckResult(HRESULT result, char const *operation) {
	if (FAILED(result)) {
		char code[16];
		std::snprintf(code, sizeof(code), "0x%08lx", static_cast<unsigned long>(result));
		throw std::runtime_error(std::string(operation) + " failed (" + code + ")");
	}
}

void CheckInterop(BOOL result, char const *operation) {
	if (!result)
		throw std::runtime_error(std::string(operation) + " failed (Win32 " + std::to_string(GetLastError()) + ")");
}

void ClearGLErrors() {
	for (int count = 0; count < 32 && glGetError() != GL_NO_ERROR; ++count) {
	}
}

void CheckGL(char const *operation) {
	auto error = glGetError();
	if (error != GL_NO_ERROR) {
		ClearGLErrors();
		throw std::runtime_error(std::string(operation) + " failed (OpenGL " + std::to_string(error) + ")");
	}
}

struct Functions {
	using OpenDeviceProc = HANDLE(WINAPI *)(void *);
	using CloseDeviceProc = BOOL(WINAPI *)(HANDLE);
	using RegisterObjectProc = HANDLE(WINAPI *)(HANDLE, void *, GLuint, GLenum, GLenum);
	using UnregisterObjectProc = BOOL(WINAPI *)(HANDLE, HANDLE);
	using LockObjectsProc = BOOL(WINAPI *)(HANDLE, GLint, HANDLE *);
	using ExtensionsARBProc = char const *(WINAPI *)(HDC);
	using ExtensionsEXTProc = char const *(WINAPI *)();

	OpenDeviceProc OpenDevice = LoadProc<OpenDeviceProc>("wglDXOpenDeviceNV");
	CloseDeviceProc CloseDevice = LoadProc<CloseDeviceProc>("wglDXCloseDeviceNV");
	RegisterObjectProc RegisterObject = LoadProc<RegisterObjectProc>("wglDXRegisterObjectNV");
	UnregisterObjectProc UnregisterObject = LoadProc<UnregisterObjectProc>("wglDXUnregisterObjectNV");
	LockObjectsProc LockObjects = LoadProc<LockObjectsProc>("wglDXLockObjectsNV");
	LockObjectsProc UnlockObjects = LoadProc<LockObjectsProc>("wglDXUnlockObjectsNV");
	PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = LoadProc<PFNGLGENFRAMEBUFFERSPROC>("glGenFramebuffers", "glGenFramebuffersEXT");
	PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = LoadProc<PFNGLDELETEFRAMEBUFFERSPROC>("glDeleteFramebuffers", "glDeleteFramebuffersEXT");
	PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = LoadProc<PFNGLBINDFRAMEBUFFERPROC>("glBindFramebuffer", "glBindFramebufferEXT");
	PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer = LoadProc<PFNGLFRAMEBUFFERRENDERBUFFERPROC>("glFramebufferRenderbuffer", "glFramebufferRenderbufferEXT");
	PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = LoadProc<PFNGLCHECKFRAMEBUFFERSTATUSPROC>("glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");
	PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer = LoadProc<PFNGLBLITFRAMEBUFFERPROC>("glBlitFramebuffer", "glBlitFramebufferEXT");
	PFNGLGENRENDERBUFFERSPROC GenRenderbuffers = LoadProc<PFNGLGENRENDERBUFFERSPROC>("glGenRenderbuffers", "glGenRenderbuffersEXT");
	PFNGLDELETERENDERBUFFERSPROC DeleteRenderbuffers = LoadProc<PFNGLDELETERENDERBUFFERSPROC>("glDeleteRenderbuffers", "glDeleteRenderbuffersEXT");

	[[nodiscard]] bool Supported() const {
		if (!wglGetCurrentContext() || !OpenDevice || !CloseDevice || !RegisterObject || !UnregisterObject || !LockObjects || !UnlockObjects || !GenFramebuffers || !DeleteFramebuffers || !BindFramebuffer || !FramebufferRenderbuffer || !CheckFramebufferStatus || !BlitFramebuffer || !GenRenderbuffers || !DeleteRenderbuffers)
			return false;
		auto arb = LoadProc<ExtensionsARBProc>("wglGetExtensionsStringARB");
		auto ext = LoadProc<ExtensionsEXTProc>("wglGetExtensionsStringEXT");
		auto extensions = arb ? arb(wglGetCurrentDC()) : nullptr;
		if (!extensions && ext)
			extensions = ext();
		if (!HasExtension(extensions, "WGL_NV_DX_interop2"))
			return false;
		auto version = reinterpret_cast<char const *>(glGetString(GL_VERSION));
		int major = 0;
		if (version)
			std::from_chars(version, version + std::strlen(version), major);
		if (major >= 3)
			return true;
		auto gl_extensions = reinterpret_cast<char const *>(glGetString(GL_EXTENSIONS));
		return HasExtension(gl_extensions, "GL_ARB_framebuffer_object") || (HasExtension(gl_extensions, "GL_EXT_framebuffer_object") && HasExtension(gl_extensions, "GL_EXT_framebuffer_blit"));
	}
};

bool FramebufferSrgbSupported() {
	auto version = reinterpret_cast<char const *>(glGetString(GL_VERSION));
	int major = 0;
	if (version)
		std::from_chars(version, version + std::strlen(version), major);
	if (major >= 3)
		return true;
	auto extensions = reinterpret_cast<char const *>(glGetString(GL_EXTENSIONS));
	return HasExtension(extensions, "GL_ARB_framebuffer_sRGB") || HasExtension(extensions, "GL_EXT_framebuffer_sRGB");
}

struct FramebufferState {
	Functions const& functions;
	GLint read_framebuffer = 0;
	GLint draw_framebuffer = 0;
	GLint default_read_buffer = GL_BACK;
	GLboolean scissor = GL_FALSE;
	bool srgb_supported = FramebufferSrgbSupported();
	GLboolean srgb = GL_FALSE;

	explicit FramebufferState(Functions const& functions) : functions(functions) {
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer);
		scissor = glIsEnabled(GL_SCISSOR_TEST);
		if (srgb_supported)
			srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
		functions.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glGetIntegerv(GL_READ_BUFFER, &default_read_buffer);
	}

	~FramebufferState() {
		functions.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glReadBuffer(default_read_buffer);
		functions.BindFramebuffer(GL_READ_FRAMEBUFFER, read_framebuffer);
		functions.BindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_framebuffer);
		if (scissor)
			glEnable(GL_SCISSOR_TEST);
		else
			glDisable(GL_SCISSOR_TEST);
		if (srgb_supported) {
			if (srgb)
				glEnable(GL_FRAMEBUFFER_SRGB);
			else
				glDisable(GL_FRAMEBUFFER_SRGB);
		}
		ClearGLErrors();
	}
};
}

struct OpenGLPresenterD3D11::Impl {
	Functions functions;
	HWND window;
	HGLRC owner_context = wglGetCurrentContext();
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	ComPtr<IDXGIFactory2> factory;
	ComPtr<IDXGISwapChain1> swap_chain;
	ComPtr<ID3D11Texture2D> texture;
	HANDLE interop_device = nullptr;
	HANDLE interop_object = nullptr;
	GLuint framebuffer = 0;
	GLuint renderbuffer = 0;
	int width = 0;
	int height = 0;
	bool locked = false;

	explicit Impl(HWND window) : window(window) {}

	~Impl() {
		if (locked && wglGetCurrentContext() == owner_context && functions.UnlockObjects(interop_device, 1, &interop_object))
			locked = false;
		if (!locked && interop_object && functions.UnregisterObject(interop_device, interop_object))
			interop_object = nullptr;
		if (!interop_object && owner_context && wglGetCurrentContext() == owner_context) {
			DeleteGLObjects();
			ClearGLErrors();
		}
		if (!interop_object && interop_device && functions.CloseDevice(interop_device))
			interop_device = nullptr;
		if (interop_device) {
			texture.Detach();
			context.Detach();
			device.Detach();
		}
	}

	bool TryDevice(IDXGIAdapter *adapter) {
		ComPtr<ID3D11Device> candidate;
		ComPtr<ID3D11DeviceContext> candidate_context;
		auto result = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
										nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &candidate, nullptr, &candidate_context);
		if (FAILED(result))
			return false;
		auto handle = functions.OpenDevice(candidate.Get());
		if (!handle)
			return false;
		device = candidate;
		context = candidate_context;
		interop_device = handle;
		return true;
	}

	void Initialize() {
		if (!window || !functions.Supported())
			throw std::runtime_error("WGL_NV_DX_interop2 framebuffer presentation is unavailable");
		if (!TryDevice(nullptr)) {
			ComPtr<IDXGIFactory1> adapters;
			CheckResult(CreateDXGIFactory1(IID_PPV_ARGS(&adapters)), "CreateDXGIFactory1");
			for (UINT index = 0; !interop_device; ++index) {
				ComPtr<IDXGIAdapter1> adapter;
				auto result = adapters->EnumAdapters1(index, &adapter);
				if (result == DXGI_ERROR_NOT_FOUND)
					break;
				CheckResult(result, "EnumAdapters1");
				DXGI_ADAPTER_DESC1 desc{};
				CheckResult(adapter->GetDesc1(&desc), "GetDesc1");
				if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
					TryDevice(adapter.Get());
			}
		}
		if (!interop_device)
			throw std::runtime_error("No D3D11 hardware adapter interoperates with the current OpenGL context");
		ComPtr<IDXGIDevice> dxgi_device;
		ComPtr<IDXGIAdapter> adapter;
		CheckResult(device.As(&dxgi_device), "Query IDXGIDevice");
		CheckResult(dxgi_device->GetAdapter(&adapter), "GetAdapter");
		CheckResult(adapter->GetParent(IID_PPV_ARGS(&factory)), "Get IDXGIFactory2");
		CheckResult(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation");
	}

	void DeleteGLObjects() {
		if (framebuffer)
			functions.DeleteFramebuffers(1, &framebuffer);
		if (renderbuffer)
			functions.DeleteRenderbuffers(1, &renderbuffer);
		framebuffer = renderbuffer = 0;
	}

	void ReleaseTexture() {
		if (locked) {
			CheckInterop(functions.UnlockObjects(interop_device, 1, &interop_object), "wglDXUnlockObjectsNV");
			locked = false;
		}
		if (interop_object) {
			CheckInterop(functions.UnregisterObject(interop_device, interop_object), "wglDXUnregisterObjectNV");
			interop_object = nullptr;
		}
		DeleteGLObjects();
		texture.Reset();
	}

	void Resize(int new_width, int new_height) {
		ReleaseTexture();
		width = height = 0;
		if (swap_chain) {
			CheckResult(swap_chain->ResizeBuffers(2, new_width, new_height, DXGI_FORMAT_R8G8B8A8_UNORM, 0), "ResizeBuffers");
		}
		else {
			DXGI_SWAP_CHAIN_DESC1 desc{};
			desc.Width = new_width;
			desc.Height = new_height;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.BufferCount = 2;
			desc.Scaling = DXGI_SCALING_STRETCH;
			desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
			CheckResult(factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &swap_chain), "CreateSwapChainForHwnd");
		}
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = new_width;
		desc.Height = new_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET;
		CheckResult(device->CreateTexture2D(&desc, nullptr, &texture), "CreateTexture2D");
		functions.GenRenderbuffers(1, &renderbuffer);
		CheckGL("glGenRenderbuffers");
		interop_object = functions.RegisterObject(interop_device, texture.Get(), renderbuffer, GL_RENDERBUFFER, 0x0001);
		CheckInterop(interop_object != nullptr, "wglDXRegisterObjectNV");
		functions.GenFramebuffers(1, &framebuffer);
		CheckGL("glGenFramebuffers");
		width = new_width;
		height = new_height;
	}

	void Present(int source_width, int source_height, unsigned sync_interval) {
		if (wglGetCurrentContext() != owner_context)
			throw std::runtime_error("D3D11 presenter requires its owning OpenGL context");
		RECT client{};
		CheckInterop(GetClientRect(window, &client), "GetClientRect");
		auto client_width = client.right - client.left;
		auto client_height = client.bottom - client.top;
		if (source_width <= 0 || source_height <= 0 || client_width <= 0 || client_height <= 0)
			return;
		ClearGLErrors();
		FramebufferState state(functions);
		CheckGL("Save framebuffer state");
		if (width != client_width || height != client_height)
			Resize(client_width, client_height);
		CheckInterop(functions.LockObjects(interop_device, 1, &interop_object), "wglDXLockObjectsNV");
		locked = true;
		try {
			functions.BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
			functions.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
			glDrawBuffer(GL_COLOR_ATTACHMENT0);
			if (functions.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
				throw std::runtime_error("D3D11 interop framebuffer is incomplete");
			glReadBuffer(GL_BACK);
			glDisable(GL_SCISSOR_TEST);
			if (state.srgb_supported)
				glDisable(GL_FRAMEBUFFER_SRGB);
			functions.BlitFramebuffer(0, 0, source_width, source_height, 0, height, width, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			CheckGL("glBlitFramebuffer");
			CheckInterop(functions.UnlockObjects(interop_device, 1, &interop_object), "wglDXUnlockObjectsNV");
			locked = false;
		}
		catch (...) {
			if (locked && functions.UnlockObjects(interop_device, 1, &interop_object))
				locked = false;
			throw;
		}
		ComPtr<ID3D11Texture2D> backbuffer;
		CheckResult(swap_chain->GetBuffer(0, IID_PPV_ARGS(&backbuffer)), "GetBuffer");
		context->CopyResource(backbuffer.Get(), texture.Get());
		CheckResult(device->GetDeviceRemovedReason(), "CopyResource device status");
		CheckResult(swap_chain->Present(sync_interval, 0), "Present");
	}
};

OpenGLPresenterD3D11::OpenGLPresenterD3D11(void *window)
	: impl(std::make_unique<Impl>(static_cast<HWND>(window))) {
	impl->Initialize();
}

OpenGLPresenterD3D11::~OpenGLPresenterD3D11() = default;

bool OpenGLPresenterD3D11::IsSupportedForCurrentContext() {
	return Functions().Supported();
}

void OpenGLPresenterD3D11::Present(int width, int height, unsigned sync_interval) {
	impl->Present(width, height, sync_interval);
}
