#pragma once

#include <memory>

class OpenGLPresenterD3D11 {
	struct Impl;
	std::unique_ptr<Impl> impl;

	public:
	explicit OpenGLPresenterD3D11(void *window);
	~OpenGLPresenterD3D11();

	OpenGLPresenterD3D11(OpenGLPresenterD3D11 const&) = delete;
	OpenGLPresenterD3D11& operator=(OpenGLPresenterD3D11 const&) = delete;

	static bool IsSupportedForCurrentContext();
	void Present(int width, int height, unsigned sync_interval = 1);
};
