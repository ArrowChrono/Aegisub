// Copyright (c) 2026
// All rights reserved.

#include "video_overlay_draw_context_legacy_gl.h"

#include "gl_text.h"
#include "gl_wrap.h"

#include <libaegisub/color.h>

LegacyVideoOverlayDrawContext::LegacyVideoOverlayDrawContext(OpenGLWrapper& gl, OpenGLText& text)
	: OpenGLVideoOverlayDrawContext(gl), text(text) {
}

LegacyVideoOverlayDrawContext::LegacyVideoOverlayDrawContext(OpenGLWrapper& gl, OpenGLText& text, OpenGLWindowOverlayPass& pass)
	: OpenGLVideoOverlayDrawContext(gl, pass), text(text) {
}

VideoOverlayTextExtent LegacyVideoOverlayDrawContext::MeasureText(std::string const& value, VideoOverlayTextStyle const& style) {
	text.SetFont(style.face, style.size, style.bold, style.italic);
	int width = 0;
	int height = 0;
	text.GetExtent(value, width, height);
	return {width, height};
}

void LegacyVideoOverlayDrawContext::DrawText(std::string const& value, int x, int y, VideoOverlayTextStyle const& style) {
	text.SetFont(style.face, style.size, style.bold, style.italic);
	text.SetColour(agi::Color(style.colour.r, style.colour.g, style.colour.b, 255 - style.colour.a));
	text.Print(value, x, y);
}
