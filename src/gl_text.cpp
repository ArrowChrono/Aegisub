// Copyright (c) 2007, Rodrigo Braz Monteiro
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

/// @file gl_text.cpp
/// @brief Create and render text using OpenGL
/// @ingroup video_output
///

#include "gl_text.h"

#include "audio_tile_diagnostics_enabled.h"
#include "compat.h"
#include "utils.h"

#include <libaegisub/color.h>
#include <libaegisub/compiler.h>
#include <libaegisub/exception.h>
#include <libaegisub/log.h>

#include <wx/bitmap.h>
#include <wx/dcmemory.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <locale>
#include <sstream>
#ifdef _WIN32
#include <wx/msw/wrapwin.h>
#endif
#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

namespace {
void const *CurrentTextNativeContext() noexcept {
#ifdef _WIN32
	return wglGetCurrentContext();
#else
	return nullptr;
#endif
}

void TraceTextLifecycle(char const *phase, OpenGLText const *text, std::size_t texture_count) noexcept try {
	if (!aegisub::AudioTileDiagnosticsEnabled() || !agi::log::log)
		return;
	std::ostringstream message;
	message.imbue(std::locale::classic());
	message << "phase=" << phase << " source=" << phase << " text=" << text
			<< " current=" << CurrentTextNativeContext() << " texture_count=" << texture_count;
	LOG_I("audio/tile-diagnostics/gl-text") << message.str();
}
catch (...) {
	// Optional diagnostics must not change GL object lifetime on failure.
}
}

/// @class OpenGLTextGlyph
/// @brief Struct storing the information needed to draw a glyph
struct OpenGLText::OpenGLTextGlyph {
	wxString str; ///< String containing the glyph(s) this is for
	int tex = 0;  ///< OpenGL texture to draw for this glyph
	float x1 = 0; ///< Left x coordinate of this glyph in the containing texture
	float x2 = 0; ///< Right x coordinate of this glyph in the containing texture
	float y1 = 0; ///< Left y coordinate of this glyph in the containing texture
	float y2 = 0; ///< Right y coordinate of this glyph in the containing texture
	int w = 0;    ///< Width of the glyph in pixels
	int h = 0;    ///< Height of the glyph in pixels
	wxFont font;  ///< Font used for this glyph

	OpenGLTextGlyph(int value, wxFont const& font)
	: str(wxChar(value))
	, font(font)
	{
		wxCoord desc,lead;

		wxBitmap tempBmp(32, 32, 24);
		wxMemoryDC dc(tempBmp);
		dc.SetFont(font);
		dc.GetTextExtent(str, &w, &h, &desc, &lead);
	}

	void Draw(float x, float y) const {
		glBindTexture(GL_TEXTURE_2D, tex);
		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);

		float tex_coords[] = {
			x1, y1,
			x1, y2,
			x2, y2,
			x2, y1
		};

		float vert_coords[] = {
			x, y,
			x, y + h,
			x + w, y + h,
			x + w, y
		};

		glVertexPointer(2, GL_FLOAT, 0, vert_coords);
		glTexCoordPointer(2, GL_FLOAT, 0, tex_coords);
		glDrawArrays(GL_QUADS, 0, 4);

		glDisableClientState(GL_VERTEX_ARRAY);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	}
};

// Allocate the retirement node with the atlas, not in a destructor. Moving it
// onto the owning context's queue cannot allocate, throw or call OpenGL.
struct OpenGLTextTextureDeleter::Texture {
	GLuint tex = 0;
	int width = 0;
	int height = 0;
	std::unique_ptr<Texture> next;
	std::uint64_t serial = 0;
	void const *owner = nullptr;
	OpenGLText const *text = nullptr;
	char const *delete_source = "text_destruction";

	void Trace(char const *phase, char const *source) const noexcept try {
		if (!serial || !agi::log::log)
			return;
		auto const *current = CurrentTextNativeContext();
		std::ostringstream message;
		message.imbue(std::locale::classic());
		message << "phase=" << phase << " source=" << source << " text=" << text
				<< " serial=" << serial << " id=" << tex
				<< " width=" << width << " height=" << height
				<< " owner=" << owner << " current=" << current
				<< " mismatch=" << (owner != current);
		LOG_I("audio/tile-diagnostics/gl-text") << message.str();
	}
	catch (...) {
		// Optional diagnostics must not affect retirement or deletion.
	}
};

OpenGLTextTextureDeleter::OpenGLTextTextureDeleter() = default;
OpenGLTextTextureDeleter::~OpenGLTextTextureDeleter() { Abandon(); }

void OpenGLTextTextureDeleter::Retire(std::unique_ptr<Texture> texture) noexcept {
	if (!texture || !texture->tex)
		return;
	if (abandoned) {
		texture->Trace("texture_abandon", texture->delete_source);
		return;
	}
	texture->Trace("texture_retire", texture->delete_source);
	texture->next = std::move(pending);
	pending = std::move(texture);
}

void OpenGLTextTextureDeleter::Drain() noexcept {
	while (pending) {
		auto texture = std::move(pending);
		pending = std::move(texture->next);
		texture->Trace("texture_delete", texture->delete_source);
		glDeleteTextures(1, &texture->tex);
	}
}

void OpenGLTextTextureDeleter::Abandon() noexcept {
	abandoned = true;
	while (pending) {
		auto texture = std::move(pending);
		pending = std::move(texture->next);
		texture->Trace("texture_abandon", texture->delete_source);
	}
}

/// @class OpenGLTextTexture
/// @brief OpenGL texture which stores one or more glyphs as sprites
class OpenGLText::OpenGLTextTexture final {
	int x = 0;      ///< Next x coordinate at which a glyph can be inserted
	int y = 0;      ///< Next y coordinate at which a glyph can be inserted
	int nextY = 0;  ///< Y coordinate of the next line; tracked due to that lines
	                ///< are only as tall as needed to fit the glyphs in them
	int width;      ///< Width of the texture
	int height;     ///< Height of the texture
	OpenGLTextTextureDeleter& deleter;
	std::unique_ptr<OpenGLTextTextureDeleter::Texture> texture;

	/// Insert the glyph into this texture at the current coordinates
	void Insert(OpenGLTextGlyph &glyph) {
		int w = glyph.w;
		int h = glyph.h;

		// Fill glyph structure
		glyph.x1 = float(x)/width;
		glyph.y1 = float(y)/height;
		glyph.x2 = float(x+w)/width;
		glyph.y2 = float(y+h)/height;

		// Create bitmap and bind it to a DC
		wxBitmap bmp(w + (w & 1), h + (h & 1), 24);
		wxMemoryDC dc(bmp);

		// Draw text and convert to image
		dc.SetBackground(*wxBLACK_BRUSH);
		dc.Clear();
		dc.SetFont(glyph.font);
		dc.SetTextForeground(*wxWHITE);
		dc.DrawText(glyph.str, 0, 0);

		// Convert RGB24 to Luminance + Alpha by using an arbitrary channel as A
		wxImage img = bmp.ConvertToImage();
		int imgw = img.GetWidth();
		int imgh = img.GetHeight();
		std::vector<unsigned char> alpha(imgw * imgh * 2, 255);
		const unsigned char *read = img.GetData();
		for (size_t write = 1; write < alpha.size(); write += 2, read += 3)
			alpha[write] = *read;

		// Upload image to video memory
		glBindTexture(GL_TEXTURE_2D, texture->tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, imgw, imgh, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, &alpha[0]);
		if (glGetError()) throw agi::EnvironmentError("Internal OpenGL text renderer error: Error uploading glyph data to video memory.");
		glyph.tex = texture->tex;
	}

public:
	OpenGLTextTexture(OpenGLTextTexture const&) = delete;
	OpenGLTextTexture& operator=(OpenGLTextTexture const&) = delete;

	OpenGLTextTexture(OpenGLTextGlyph& glyph, OpenGLText const *text)
		: width(std::max(SmallestPowerOf2(glyph.w), 64)), height(std::max(SmallestPowerOf2(glyph.h), 64)),
		  deleter(*text->texture_deleter), texture(std::make_unique<OpenGLTextTextureDeleter::Texture>()) {
		try {
			width = height = std::max(width, height);

			// Generate and bind
			glGenTextures(1, &texture->tex);
			glBindTexture(GL_TEXTURE_2D, texture->tex);

			// Texture parameters
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

			// Allocate texture
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_ALPHA, GL_UNSIGNED_BYTE, nullptr);
			if (glGetError())
				throw agi::EnvironmentError("Internal OpenGL text renderer error: Could not allocate text texture");

			if (aegisub::AudioTileDiagnosticsEnabled()) {
				static std::atomic<std::uint64_t> next_serial{0};
				texture->serial = next_serial.fetch_add(1, std::memory_order_relaxed) + 1;
				texture->owner = CurrentTextNativeContext();
				texture->text = text;
				texture->width = width;
				texture->height = height;
				texture->Trace("texture_create", "glyph_atlas");
			}

			TryToInsert(glyph);
		}
		catch (...) {
			deleter.Retire(std::move(texture));
			throw;
		}
	}

	OpenGLTextTexture(OpenGLTextTexture&&) noexcept = default;

	~OpenGLTextTexture() { deleter.Retire(std::move(texture)); }

	void SetDiagnosticDeleteSource(char const *source) noexcept { texture->delete_source = source; }

	/// @brief Try to insert a glyph into this texture
	/// @param[in][out] glyph Texture to insert
	/// @return Was the texture successfully added?
	bool TryToInsert(OpenGLTextGlyph &glyph) {
		if (glyph.w > width) return false;
		if (y + glyph.h > height) return false;

		// Can fit in this row?
		if (x + glyph.w < width) {
			Insert(glyph);
			x += glyph.w;
			nextY = std::max(nextY, y + glyph.h);
			return true;
		}

		// Can fit the next row?
		if (nextY + glyph.h > height) return false;
		x = 0;
		y = nextY;
		return TryToInsert(glyph);
	}
};

OpenGLText::OpenGLText(std::shared_ptr<OpenGLTextTextureDeleter> texture_deleter)
	: texture_deleter(std::move(texture_deleter)) {}
OpenGLText::~OpenGLText() {
	TraceTextLifecycle("text_destruction", this, textures.size());
}

void OpenGLText::SetFont(std::string const& face, int size, bool bold, bool italics) {
	// No change required
	if (size == fontSize && face == fontFace && bold == fontBold && italics == fontItalics) return;

	// Set font
	fontFace = face;
	fontSize = size;
	fontBold = bold;
	fontItalics = italics;
	font.SetFaceName(to_wx(fontFace));
	font.SetPointSize(size);
	font.SetWeight(bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);

	// Retire GPU handles without touching whichever context is current here.
	if (aegisub::AudioTileDiagnosticsEnabled()) {
		TraceTextLifecycle("font_reset", this, textures.size());
		for (auto& texture : textures)
			texture.SetDiagnosticDeleteSource("font_reset");
	}
	textures.clear();
	glyphs.clear();
}

void OpenGLText::SetColour(agi::Color col) {
	r = col.r / 255.f;
	g = col.g / 255.f;
	b = col.b / 255.f;
	a = col.a / 255.f;
}

void OpenGLText::Print(const std::string &text, int x, int y) {
	glEnable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// Draw border
	glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
	DrawString(text, x-1, y);
	DrawString(text, x+1, y);
	DrawString(text, x, y-1);
	DrawString(text, x, y+1);

	// Draw primary string
	glColor4f(r, g, b, a);
	DrawString(text, x, y);

	// Disable blend
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
}

void OpenGLText::DrawString(const std::string &text, int x, int y) {
	for (char curChar : text) {
		OpenGLTextGlyph& glyph = GetGlyph(curChar);
		if (!glyph.tex)
			UploadGlyph(glyph);
		glyph.Draw(x, y);
		x += glyph.w;
	}
}

void OpenGLText::GetExtent(std::string const& text, int &w, int &h) {
	w = h = 0;

	for (char curChar : text) {
		OpenGLTextGlyph const& glyph = GetGlyph(curChar);
		w += glyph.w;
		h = std::max(h, glyph.h);
	}
}

OpenGLText::OpenGLTextGlyph& OpenGLText::GetGlyph(int i) {
	auto res = glyphs.find(i);
	return res != glyphs.end() ? res->second : glyphs.emplace(i, OpenGLTextGlyph(i, font)).first->second;
}

void OpenGLText::UploadGlyph(OpenGLTextGlyph& glyph) {
	for (auto& texture : textures) {
		if (texture.TryToInsert(glyph))
			return;
	}
	textures.emplace_back(glyph, this);
}
