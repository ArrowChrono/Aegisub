#pragma once

#include "video_overlay_draw_context.h"

#include <memory>
#include <string>

struct IDWriteFactory;
struct ID2D1Factory;
struct IWICImagingFactory;

class OpenGLVideoOverlayText final : public VideoOverlayTextRenderer {
	class Impl;
	std::unique_ptr<Impl> impl;

	public:
	OpenGLVideoOverlayText(IDWriteFactory& write_factory, ID2D1Factory& d2d_factory,
						   IWICImagingFactory& imaging_factory, std::string default_face);
	~OpenGLVideoOverlayText() override;
	OpenGLVideoOverlayText(OpenGLVideoOverlayText const&) = delete;
	OpenGLVideoOverlayText& operator=(OpenGLVideoOverlayText const&) = delete;
	OpenGLVideoOverlayText(OpenGLVideoOverlayText&&) = delete;
	OpenGLVideoOverlayText& operator=(OpenGLVideoOverlayText&&) = delete;

	void SetScale(double x, double y);
	VideoOverlayTextExtent MeasureText(std::string const& text, VideoOverlayTextStyle const& style) override;
	void DrawText(std::string const& text, int x, int y, VideoOverlayTextStyle const& style) override;
};
