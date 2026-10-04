// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#pragma once

#include "video_render_opengl_proc_loader.h"

#include <algorithm>

#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

namespace legacy_gl {

using ClientActiveTextureProc = void (APIENTRY *)(GLenum);

template <typename Proc>
inline Proc LoadOptionalProc(char const *name, char const *fallback_name = nullptr) {
	if (auto *proc = opengl::GetProcAddress(name))
		return reinterpret_cast<Proc>(proc);
	if (fallback_name) {
		if (auto *proc = opengl::GetProcAddress(fallback_name))
			return reinterpret_cast<Proc>(proc);
	}
	return nullptr;
}

struct CompatibilityFunctions {
	PFNGLACTIVETEXTUREPROC ActiveTexture = nullptr;
	ClientActiveTextureProc ClientActiveTexture = nullptr;
	PFNGLBINDBUFFERPROC BindBuffer = nullptr;
	PFNGLUSEPROGRAMPROC UseProgram = nullptr;
	PFNGLBINDVERTEXARRAYPROC BindVertexArray = nullptr;
	PFNGLDISABLEVERTEXATTRIBARRAYPROC DisableVertexAttribArray = nullptr;
};

inline CompatibilityFunctions const& GetCompatibilityFunctions() {
	static const CompatibilityFunctions functions = {
		LoadOptionalProc<PFNGLACTIVETEXTUREPROC>("glActiveTexture"),
		LoadOptionalProc<ClientActiveTextureProc>("glClientActiveTexture"),
		LoadOptionalProc<PFNGLBINDBUFFERPROC>("glBindBuffer"),
		LoadOptionalProc<PFNGLUSEPROGRAMPROC>("glUseProgram"),
		LoadOptionalProc<PFNGLBINDVERTEXARRAYPROC>("glBindVertexArray"),
		LoadOptionalProc<PFNGLDISABLEVERTEXATTRIBARRAYPROC>("glDisableVertexAttribArray"),
	};
	return functions;
}

inline GLint GetCompatibilityTextureUnitResetCount() {
	auto const& gl = GetCompatibilityFunctions();
	if (!gl.ActiveTexture)
		return 1;

	static GLint texture_units = [] {
		GLint value = 1;
		glGetIntegerv(GL_MAX_TEXTURE_UNITS, &value);
		return std::max<GLint>(1, std::min<GLint>(value, 8));
	}();
	return texture_units;
}

inline void ResetPixelStoreState() {
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glPixelStorei(GL_PACK_ALIGNMENT, 4);
	glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
	glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
	glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
	glPixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
#ifdef GL_UNPACK_ROW_LENGTH
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
#ifdef GL_UNPACK_SKIP_ROWS
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
#endif
#ifdef GL_UNPACK_SKIP_PIXELS
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
#endif
#ifdef GL_PACK_ROW_LENGTH
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
#endif
#ifdef GL_PACK_SKIP_ROWS
	glPixelStorei(GL_PACK_SKIP_ROWS, 0);
#endif
#ifdef GL_PACK_SKIP_PIXELS
	glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
#endif
}

inline void ResetCompatibilityState() {
	auto const& gl = GetCompatibilityFunctions();
	if (gl.UseProgram)
		gl.UseProgram(0);
	if (gl.BindVertexArray)
		gl.BindVertexArray(0);
	if (gl.BindBuffer) {
		gl.BindBuffer(GL_ARRAY_BUFFER, 0);
		gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	}
	GLint const texture_units = GetCompatibilityTextureUnitResetCount();
	for (GLint unit = 0; unit < texture_units; ++unit) {
		if (gl.ActiveTexture)
			gl.ActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
		if (gl.ClientActiveTexture)
			gl.ClientActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
		glDisable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, 0);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	}
	if (gl.ActiveTexture)
		gl.ActiveTexture(GL_TEXTURE0);
	if (gl.ClientActiveTexture)
		gl.ClientActiveTexture(GL_TEXTURE0);
	if (gl.DisableVertexAttribArray) {
		for (GLuint i = 0; i < 8; ++i)
			gl.DisableVertexAttribArray(i);
	}
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_LINE_SMOOTH);
	glDisable(GL_COLOR_LOGIC_OP);
	glDisable(GL_ALPHA_TEST);
	glBlendFunc(GL_ONE, GL_ZERO);
	glLogicOp(GL_COPY);
	glAlphaFunc(GL_ALWAYS, 0.0f);
	glLineWidth(1.0f);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	ResetPixelStoreState();
}

inline void SetupBottomLeftOrtho(int canvas_width, int canvas_height) {
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, canvas_width, 0.0, canvas_height, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

inline void DrawTexturedQuad(GLuint texture, int canvas_width, int canvas_height) {
	ResetCompatibilityState();
	SetupBottomLeftOrtho(canvas_width, canvas_height);

	GLfloat const tex_coords[] = {
		0.0f, 0.0f,
		1.0f, 0.0f,
		1.0f, 1.0f,
		0.0f, 1.0f
	};
	GLfloat const vertices[] = {
		0.0f, 0.0f,
		static_cast<GLfloat>(canvas_width), 0.0f,
		static_cast<GLfloat>(canvas_width), static_cast<GLfloat>(canvas_height),
		0.0f, static_cast<GLfloat>(canvas_height)
	};

	glDisableClientState(GL_COLOR_ARRAY);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, texture);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, vertices);
	glTexCoordPointer(2, GL_FLOAT, 0, tex_coords);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_TEXTURE_2D);
}

inline void DrawTexturedQuadFlippedY(GLuint texture, int canvas_width, int canvas_height) {
	ResetCompatibilityState();
	SetupBottomLeftOrtho(canvas_width, canvas_height);

	GLfloat const tex_coords[] = {
		0.0f, 1.0f,
		1.0f, 1.0f,
		1.0f, 0.0f,
		0.0f, 0.0f
	};
	GLfloat const vertices[] = {
		0.0f, 0.0f,
		static_cast<GLfloat>(canvas_width), 0.0f,
		static_cast<GLfloat>(canvas_width), static_cast<GLfloat>(canvas_height),
		0.0f, static_cast<GLfloat>(canvas_height)
	};

	glDisableClientState(GL_COLOR_ARRAY);
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, texture);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, vertices);
	glTexCoordPointer(2, GL_FLOAT, 0, tex_coords);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_TEXTURE_2D);
}

}

