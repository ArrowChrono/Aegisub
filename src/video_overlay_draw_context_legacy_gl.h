// Copyright (c) 2026
// All rights reserved.

#pragma once

#include "video_overlay_draw_context_opengl.h"

class OpenGLText;

class LegacyVideoOverlayDrawContext final : public OpenGLVideoOverlayDrawContext {
	OpenGLText &text;

public:
	LegacyVideoOverlayDrawContext(OpenGLWrapper &gl, OpenGLText &text);
	LegacyVideoOverlayDrawContext(OpenGLWrapper &gl, OpenGLText &text, OpenGLWindowOverlayPass &pass);
	VideoOverlayTextExtent MeasureText(std::string const& text, VideoOverlayTextStyle const& style) override;
	void DrawText(std::string const& text, int x, int y, VideoOverlayTextStyle const& style) override;
};
