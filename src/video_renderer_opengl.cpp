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

#include "video_renderer_opengl.h"

#include "legacy_gl_draw.h"
#include "perf_trace.h"
#include "video_render_opengl_proc_loader.h"
#include "video_renderer_opengl_overlay_upload_plan.h"
#include "video_renderer_error.h"

#include <libaegisub/compiler.h>
#include <libaegisub/log.h>

#include <cctype>
#include <cstdlib>

#ifdef _WIN32
#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/glext.h>
#else
#include <GL/glext.h>
#endif
#endif

namespace {
template<typename Exception>
AGI_NOINLINE void throw_error(GLenum err, const char *msg) {
	LOG_E("video/out/opengl") << msg << " failed with error code " << err;
	throw Exception(msg, err);
}

template<typename Exception>
AGI_NOINLINE void throw_message(char const *msg) {
	LOG_E("video/out/opengl") << msg;
	throw Exception(msg);
}
}

#define DO_CHECK_ERROR(cmd, Exception, msg) \
	do { \
		cmd; \
		GLenum err = glGetError(); \
		AGI_UNLIKELY_IF(err) \
			throw_error<Exception>(err, msg); \
	} while (0)

#define CHECK_INIT_ERROR(cmd) DO_CHECK_ERROR(cmd, VideoOutInitException, #cmd)
#define CHECK_RENDER_ERROR(cmd) DO_CHECK_ERROR(cmd, VideoOutRenderException, #cmd)

struct OpenGLVideoRenderer::Functions {
	PFNGLATTACHSHADERPROC AttachShader = nullptr;
	PFNGLBINDATTRIBLOCATIONPROC BindAttribLocation = nullptr;
	PFNGLBINDBUFFERPROC BindBuffer = nullptr;
	PFNGLBUFFERDATAPROC BufferData = nullptr;
	PFNGLCOMPILESHADERPROC CompileShader = nullptr;
	PFNGLCREATEPROGRAMPROC CreateProgram = nullptr;
	PFNGLCREATESHADERPROC CreateShader = nullptr;
	PFNGLDELETEBUFFERSPROC DeleteBuffers = nullptr;
	PFNGLDELETEPROGRAMPROC DeleteProgram = nullptr;
	PFNGLDELETESHADERPROC DeleteShader = nullptr;
	PFNGLDISABLEVERTEXATTRIBARRAYPROC DisableVertexAttribArray = nullptr;
	PFNGLENABLEVERTEXATTRIBARRAYPROC EnableVertexAttribArray = nullptr;
	PFNGLGENBUFFERSPROC GenBuffers = nullptr;
	PFNGLGETPROGRAMINFOLOGPROC GetProgramInfoLog = nullptr;
	PFNGLGETPROGRAMIVPROC GetProgramiv = nullptr;
	PFNGLGETSHADERINFOLOGPROC GetShaderInfoLog = nullptr;
	PFNGLGETSHADERIVPROC GetShaderiv = nullptr;
	PFNGLGETUNIFORMLOCATIONPROC GetUniformLocation = nullptr;
	PFNGLLINKPROGRAMPROC LinkProgram = nullptr;
	PFNGLSHADERSOURCEPROC ShaderSource = nullptr;
	PFNGLUNIFORM1IPROC Uniform1i = nullptr;
	PFNGLUNIFORMMATRIX4FVPROC UniformMatrix4fv = nullptr;
	PFNGLUSEPROGRAMPROC UseProgram = nullptr;
	PFNGLVERTEXATTRIBPOINTERPROC VertexAttribPointer = nullptr;
};

namespace {
template<typename Proc>
bool LoadOptionalProc(Proc& proc, char const *name) {
	proc = reinterpret_cast<Proc>(opengl::GetProcAddress(name));
	return proc != nullptr;
}

bool ReadEnvFlag(char const *name) {
	auto const* value = std::getenv(name);
	if (!value || !*value)
		return false;

	char const first = static_cast<char>(std::tolower(static_cast<unsigned char>(*value)));
	return first != '0' && first != 'f' && first != 'n';
}

template<typename Exception>
AGI_NOINLINE void throw_shader_message(char const *label, std::string const& log) {
	LOG_E("video/out/opengl") << label << ": " << log;
	throw Exception(label);
}

bool TestTexture(int width, int height, GLint format) {
	glTexImage2D(GL_PROXY_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
	while (glGetError()) { }
	LOG_I("video/out/opengl") << "OpenGLVideoRenderer::TestTexture " << width << "x" << height;
	return format != 0;
}
}

OpenGLVideoRenderer::OpenGLVideoRenderer(bool render_video, bool render_overlay, bool clear_before_render)
	: render_video_layer(render_video)
	, render_overlay_layer(render_overlay)
	, clear_before_render(clear_before_render) {
}

OpenGLVideoRenderer::~OpenGLVideoRenderer() {
	DestroyResources();
}

void OpenGLVideoRenderer::LoadFunctions() {
	if (functions || compatibility_pipeline)
		return;

	if (ReadEnvFlag("AEGISUB_OPENGL_FORCE_COMPATIBILITY_PIPELINE")) {
		compatibility_pipeline = true;
		LOG_W("video/out/opengl") << "Forcing the fixed-function compatibility pipeline via "
			<< "AEGISUB_OPENGL_FORCE_COMPATIBILITY_PIPELINE.";
		return;
	}

	auto loaded = std::make_unique<Functions>();
	bool have_modern_pipeline = true;
	have_modern_pipeline = LoadOptionalProc(loaded->AttachShader, "glAttachShader") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->BindAttribLocation, "glBindAttribLocation") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->BindBuffer, "glBindBuffer") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->BufferData, "glBufferData") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->CompileShader, "glCompileShader") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->CreateProgram, "glCreateProgram") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->CreateShader, "glCreateShader") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->DeleteBuffers, "glDeleteBuffers") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->DeleteProgram, "glDeleteProgram") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->DeleteShader, "glDeleteShader") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->DisableVertexAttribArray, "glDisableVertexAttribArray") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->EnableVertexAttribArray, "glEnableVertexAttribArray") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->GenBuffers, "glGenBuffers") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->GetProgramInfoLog, "glGetProgramInfoLog") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->GetProgramiv, "glGetProgramiv") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->GetShaderInfoLog, "glGetShaderInfoLog") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->GetShaderiv, "glGetShaderiv") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->GetUniformLocation, "glGetUniformLocation") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->LinkProgram, "glLinkProgram") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->ShaderSource, "glShaderSource") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->Uniform1i, "glUniform1i") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->UniformMatrix4fv, "glUniformMatrix4fv") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->UseProgram, "glUseProgram") && have_modern_pipeline;
	have_modern_pipeline = LoadOptionalProc(loaded->VertexAttribPointer, "glVertexAttribPointer") && have_modern_pipeline;

	if (!have_modern_pipeline) {
		compatibility_pipeline = true;
		LOG_W("video/out/opengl") << "Required shader or buffer OpenGL entry points are unavailable; "
			<< "falling back to the fixed-function compatibility pipeline.";
		return;
	}

	functions = std::move(loaded);
}

void OpenGLVideoRenderer::DetectOpenGLCapabilities() {
	if (max_texture_size != 0)
		return;

	if (TestTexture(64, 64, GL_RGBA8)) internal_format = GL_RGBA8;
	else if (TestTexture(64, 64, GL_RGBA)) internal_format = GL_RGBA;
	else throw_message<VideoOutInitException>("Could not create a 64x64 RGB texture in any format.");

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
	while (max_texture_size > 64 && !TestTexture(max_texture_size, max_texture_size, internal_format))
		max_texture_size >>= 1;
	if (max_texture_size <= 0)
		throw_message<VideoOutInitException>("Could not determine a valid maximum texture size.");

	supports_rectangular_textures = TestTexture(max_texture_size, max_texture_size >> 1, internal_format);
	LOG_I("video/out/opengl") << "Maximum texture size is " << max_texture_size << "x" << max_texture_size;
	LOG_I("video/out/opengl") << "Rectangular textures supported: " << supports_rectangular_textures;
}

void OpenGLVideoRenderer::CreateProgram() {
	if (compatibility_pipeline)
		return;

	if (program)
		return;

	auto& gl = *functions;
	char const *vertex_shader_source =
		"uniform mat4 u_projection;\n"
		"attribute vec2 a_position;\n"
		"attribute vec2 a_texcoord;\n"
		"varying vec2 v_texcoord;\n"
		"void main() {\n"
		"    gl_Position = u_projection * vec4(a_position, 0.0, 1.0);\n"
		"    v_texcoord = a_texcoord;\n"
		"}\n";
	char const *fragment_shader_source =
		"uniform sampler2D u_texture;\n"
		"varying vec2 v_texcoord;\n"
		"void main() {\n"
		"    gl_FragColor = texture2D(u_texture, v_texcoord);\n"
		"}\n";

	auto compile_shader = [&](GLenum type, char const *source, char const *label) -> GLuint {
		GLuint shader = gl.CreateShader(type);
		if (!shader)
			throw_message<VideoOutInitException>(label);

		gl.ShaderSource(shader, 1, &source, nullptr);
		gl.CompileShader(shader);

		GLint compiled = GL_FALSE;
		gl.GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
		if (compiled == GL_FALSE) {
			GLint log_length = 0;
			gl.GetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
			std::string info_log(static_cast<size_t>(std::max(log_length, 1)), '\0');
			GLsizei written = 0;
			gl.GetShaderInfoLog(shader, log_length, &written, info_log.data());
			gl.DeleteShader(shader);
			throw_shader_message<VideoOutInitException>(label, info_log);
		}
		return shader;
	};

	GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_shader_source, "Failed to compile video vertex shader.");
	GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_shader_source, "Failed to compile video fragment shader.");

	program = gl.CreateProgram();
	if (!program) {
		gl.DeleteShader(vertex_shader);
		gl.DeleteShader(fragment_shader);
		throw_message<VideoOutInitException>("Failed to create video shader program.");
	}

	gl.AttachShader(program, vertex_shader);
	gl.AttachShader(program, fragment_shader);
	gl.BindAttribLocation(program, 0, "a_position");
	gl.BindAttribLocation(program, 1, "a_texcoord");
	gl.LinkProgram(program);

	GLint linked = GL_FALSE;
	gl.GetProgramiv(program, GL_LINK_STATUS, &linked);
	if (linked == GL_FALSE) {
		GLint log_length = 0;
		gl.GetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
		std::string info_log(static_cast<size_t>(std::max(log_length, 1)), '\0');
		GLsizei written = 0;
		gl.GetProgramInfoLog(program, log_length, &written, info_log.data());
		gl.DeleteProgram(program);
		program = 0;
		gl.DeleteShader(vertex_shader);
		gl.DeleteShader(fragment_shader);
		throw_shader_message<VideoOutInitException>("Failed to link video shader program.", info_log);
	}

	gl.DeleteShader(vertex_shader);
	gl.DeleteShader(fragment_shader);

	projection_matrix_uniform = gl.GetUniformLocation(program, "u_projection");
	texture_uniform = gl.GetUniformLocation(program, "u_texture");
	if (projection_matrix_uniform < 0 || texture_uniform < 0)
		throw_message<VideoOutInitException>("Failed to locate video shader uniforms.");

	gl.UseProgram(program);
	gl.Uniform1i(texture_uniform, 0);
	gl.UseProgram(0);
}

void OpenGLVideoRenderer::CreateLayerBuffers(LayerResources& layer) {
	if (compatibility_pipeline)
		return;

	if (layer.vertex_buffer && layer.element_buffer)
		return;

	auto& gl = *functions;
	if (!layer.vertex_buffer)
		gl.GenBuffers(1, &layer.vertex_buffer);
	if (!layer.element_buffer)
		gl.GenBuffers(1, &layer.element_buffer);
	if (!layer.vertex_buffer || !layer.element_buffer)
		throw_message<VideoOutInitException>("Failed to create video vertex/index buffers.");
}

size_t OpenGLVideoRenderer::EstimateTextureBytes() const noexcept {
	size_t total_bytes = 0;
	for (auto const& tile : video_layer.layout.tiles)
		total_bytes += static_cast<size_t>(tile.texture_w) * static_cast<size_t>(tile.texture_h) * 4;
	for (auto const& tile : overlay_layer.layout.tiles)
		total_bytes += static_cast<size_t>(tile.texture_w) * static_cast<size_t>(tile.texture_h) * 4;
	return total_bytes;
}

void OpenGLVideoRenderer::EnsureInitialized() {
	LoadFunctions();
	DetectOpenGLCapabilities();
	CreateProgram();
	if (!compatibility_pipeline && render_video_layer)
		CreateLayerBuffers(video_layer);
	if (!compatibility_pipeline && render_overlay_layer)
		CreateLayerBuffers(overlay_layer);
}

void OpenGLVideoRenderer::DeleteLayerTextures(LayerResources& layer) noexcept {
	if (!layer.texture_ids.empty()) {
		glDeleteTextures(static_cast<GLsizei>(layer.texture_ids.size()), layer.texture_ids.data());
		layer.texture_ids.clear();
	}
}

void OpenGLVideoRenderer::DestroyResources() noexcept {
	if (functions) {
		if (video_layer.vertex_buffer || video_layer.element_buffer) {
			GLuint buffers[] = { video_layer.vertex_buffer, video_layer.element_buffer };
			functions->DeleteBuffers(2, buffers);
			video_layer.vertex_buffer = 0;
			video_layer.element_buffer = 0;
		}
		if (overlay_layer.vertex_buffer || overlay_layer.element_buffer) {
			GLuint buffers[] = { overlay_layer.vertex_buffer, overlay_layer.element_buffer };
			functions->DeleteBuffers(2, buffers);
			overlay_layer.vertex_buffer = 0;
			overlay_layer.element_buffer = 0;
		}
		if (program) {
			functions->DeleteProgram(program);
			program = 0;
		}
	}

	DeleteLayerTextures(video_layer);
	DeleteLayerTextures(overlay_layer);
	projection_matrix_uniform = -1;
	texture_uniform = -1;
	video_layer = { };
	overlay_layer = { };
}

void OpenGLVideoRenderer::Reset() {
	DestroyResources();
	functions.reset();
	compatibility_pipeline = false;
	max_texture_size = 0;
	supports_rectangular_textures = false;
	internal_format = 0;
	source_geometry = {};
	source_output_mode = SourceFrameOutputMode::Bgra8;
	has_source_geometry = false;
}

VideoRenderOutputLayout OpenGLVideoRenderer::ResolveLayerRenderOutputLayout(
	bool apply_source_display_transform,
	int canvas_width,
	int canvas_height) const {
	if (apply_source_display_transform && has_source_geometry)
		return BuildVideoRenderOutputLayout(canvas_width, canvas_height, source_geometry);
	return BuildVideoRenderOutputLayout(canvas_width, canvas_height);
}

bool OpenGLVideoRenderer::UpdateLayerRenderOutputLayout(
	LayerResources& layer,
	bool apply_source_display_transform) {
	auto const next_layout = ResolveLayerRenderOutputLayout(
		apply_source_display_transform,
		layer.canvas_width,
		layer.canvas_height);
	bool const changed =
		layer.apply_source_display_transform != apply_source_display_transform ||
		layer.render_output_layout.source_width != next_layout.source_width ||
		layer.render_output_layout.source_height != next_layout.source_height ||
		layer.render_output_layout.output_width != next_layout.output_width ||
		layer.render_output_layout.output_height != next_layout.output_height ||
		layer.render_output_layout.rotation != next_layout.rotation ||
		layer.render_output_layout.display_vflip != next_layout.display_vflip;
	if (changed) {
		layer.apply_source_display_transform = apply_source_display_transform;
		layer.render_output_layout = next_layout;
	}
	return changed;
}

void OpenGLVideoRenderer::RebuildLayerGeometry(LayerResources& layer) {
	layer.vertices.clear();
	layer.indices.clear();

	layer.vertices.reserve(layer.layout.tiles.size() * 4);
	layer.indices.reserve(layer.layout.tiles.size() * 6);

	for (size_t i = 0; i < layer.layout.tiles.size(); ++i) {
		auto const& tile = layer.layout.tiles[i];
		GLuint base = static_cast<GLuint>(layer.vertices.size());
		auto const quad = TransformVideoRenderQuad(
			layer.render_output_layout,
			tile.x1 + layer.offset_x,
			tile.y1 + layer.offset_y,
			tile.x2 + layer.offset_x,
			tile.y2 + layer.offset_y);

		layer.vertices.push_back({ { quad.p0.x, quad.p0.y }, { tile.u1, tile.v1 } });
		layer.vertices.push_back({ { quad.p1.x, quad.p1.y }, { tile.u2, tile.v1 } });
		layer.vertices.push_back({ { quad.p2.x, quad.p2.y }, { tile.u2, tile.v2 } });
		layer.vertices.push_back({ { quad.p3.x, quad.p3.y }, { tile.u1, tile.v2 } });

		layer.indices.push_back(base + 0);
		layer.indices.push_back(base + 1);
		layer.indices.push_back(base + 2);
		layer.indices.push_back(base + 0);
		layer.indices.push_back(base + 2);
		layer.indices.push_back(base + 3);
	}

	if (compatibility_pipeline)
		return;

	auto& gl = *functions;
	gl.BindBuffer(GL_ARRAY_BUFFER, layer.vertex_buffer);
	gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(layer.vertices.size() * sizeof(Vertex)), layer.vertices.data(), GL_STATIC_DRAW);
	gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, layer.element_buffer);
	gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(layer.indices.size() * sizeof(GLuint)), layer.indices.data(), GL_STATIC_DRAW);
	gl.BindBuffer(GL_ARRAY_BUFFER, 0);
	gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void OpenGLVideoRenderer::RecreateLayerTextures(LayerResources& layer) {
	DeleteLayerTextures(layer);
	layer.texture_ids.resize(layer.layout.tiles.size());
	if (layer.texture_ids.empty())
		return;

	CHECK_INIT_ERROR(glGenTextures(static_cast<GLsizei>(layer.texture_ids.size()), layer.texture_ids.data()));
	for (size_t i = 0; i < layer.layout.tiles.size(); ++i) {
		auto const& tile = layer.layout.tiles[i];
		CHECK_INIT_ERROR(glBindTexture(GL_TEXTURE_2D, layer.texture_ids[i]));
		CHECK_INIT_ERROR(glTexImage2D(GL_TEXTURE_2D, 0, internal_format, tile.texture_w, tile.texture_h, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, nullptr));
		CHECK_INIT_ERROR(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
		CHECK_INIT_ERROR(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
		CHECK_INIT_ERROR(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP));
		CHECK_INIT_ERROR(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP));
	}
	CHECK_INIT_ERROR(glBindTexture(GL_TEXTURE_2D, 0));
}

void OpenGLVideoRenderer::ClearLayer(LayerResources& layer) noexcept {
	DeleteLayerTextures(layer);
	layer.layout = { };
	layer.vertices.clear();
	layer.indices.clear();
	layer.canvas_width = 0;
	layer.canvas_height = 0;
	layer.offset_x = 0;
	layer.offset_y = 0;
	layer.render_output_layout = { };
	layer.apply_source_display_transform = false;
	layer.continuity_generation = 0;
	layer.composition_mode = SubtitleOverlayCompositionMode::OpaqueReplace;
	layer.has_content = false;
}

void OpenGLVideoRenderer::HideLayer(LayerResources& layer) noexcept {
	layer.has_content = false;
}

void OpenGLVideoRenderer::UploadBgraLayer(LayerResources& layer, unsigned char const* data, int width, int height, ptrdiff_t pitch, bool flipped, int canvas_width, int canvas_height, int offset_x, int offset_y, SubtitleOverlayCompositionMode composition_mode, bool apply_source_display_transform) {
	if (!data || width <= 0 || height <= 0 || pitch <= 0) {
		ClearLayer(layer);
		return;
	}

	EnsureInitialized();

	bool textures_changed =
		layer.layout.frame_width != width ||
		layer.layout.frame_height != height ||
		layer.layout.flipped != flipped ||
		layer.texture_ids.size() != layer.layout.tiles.size();
	bool geometry_changed =
		layer.canvas_width != canvas_width ||
		layer.canvas_height != canvas_height ||
		layer.offset_x != offset_x ||
		layer.offset_y != offset_y;

	layer.canvas_width = canvas_width;
	layer.canvas_height = canvas_height;
	layer.offset_x = offset_x;
	layer.offset_y = offset_y;
	layer.composition_mode = composition_mode;
	bool const output_layout_changed = UpdateLayerRenderOutputLayout(layer, apply_source_display_transform);
	geometry_changed = geometry_changed || output_layout_changed;

	if (textures_changed) {
		layer.layout = BuildOpenGLVideoRendererTileLayout(
			width,
			height,
			4,
			max_texture_size,
			supports_rectangular_textures,
			flipped);
		LOG_I("video/out/opengl") << "Layer size: " << layer.layout.frame_width << "x" << layer.layout.frame_height << ", tiles: " << layer.layout.tiles.size();
		RecreateLayerTextures(layer);
	}
	if (textures_changed || geometry_changed)
		RebuildLayerGeometry(layer);

	CHECK_RENDER_ERROR(glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(pitch / 4)));
	for (size_t i = 0; i < layer.layout.tiles.size(); ++i) {
		auto const& tile = layer.layout.tiles[i];
		CHECK_RENDER_ERROR(glBindTexture(GL_TEXTURE_2D, layer.texture_ids[i]));
		CHECK_RENDER_ERROR(glTexSubImage2D(
			GL_TEXTURE_2D,
			0,
			0,
			0,
			tile.source_w,
			tile.source_h,
			GL_BGRA_EXT,
			GL_UNSIGNED_BYTE,
			data + tile.data_offset));
	}
	CHECK_RENDER_ERROR(glBindTexture(GL_TEXTURE_2D, 0));
	CHECK_RENDER_ERROR(glPixelStorei(GL_UNPACK_ROW_LENGTH, 0));
	layer.has_content = true;
}

void OpenGLVideoRenderer::UploadDirtyRects(LayerResources& layer, unsigned char const* data, ptrdiff_t pitch, SubtitleOverlayDirtyRect const* dirty_rects, int dirty_rect_count) {
	if (!dirty_rects || dirty_rect_count <= 0)
		return;

	CHECK_RENDER_ERROR(glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(pitch / 4)));
	for (int rect_index = 0; rect_index < dirty_rect_count; ++rect_index) {
		auto const& rect = dirty_rects[rect_index];
		if (rect.width <= 0 || rect.height <= 0)
			continue;

		int rect_x1 = rect.x + rect.width;
		int rect_y1 = rect.y + rect.height;
		for (size_t tile_index = 0; tile_index < layer.layout.tiles.size(); ++tile_index) {
			auto const& tile = layer.layout.tiles[tile_index];
			int overlap_x0 = std::max(rect.x, tile.source_x);
			int overlap_y0 = std::max(rect.y, tile.source_y);
			int overlap_x1 = std::min(rect_x1, tile.source_x + tile.source_w);
			int overlap_y1 = std::min(rect_y1, tile.source_y + tile.source_h);
			if (overlap_x0 >= overlap_x1 || overlap_y0 >= overlap_y1)
				continue;

			CHECK_RENDER_ERROR(glBindTexture(GL_TEXTURE_2D, layer.texture_ids[tile_index]));
			CHECK_RENDER_ERROR(glTexSubImage2D(
				GL_TEXTURE_2D,
				0,
				overlap_x0 - tile.source_x,
				overlap_y0 - tile.source_y,
				overlap_x1 - overlap_x0,
				overlap_y1 - overlap_y0,
				GL_BGRA_EXT,
				GL_UNSIGNED_BYTE,
				data + static_cast<ptrdiff_t>(overlap_y0) * pitch + static_cast<ptrdiff_t>(overlap_x0) * 4));
		}
	}
	CHECK_RENDER_ERROR(glBindTexture(GL_TEXTURE_2D, 0));
	CHECK_RENDER_ERROR(glPixelStorei(GL_UNPACK_ROW_LENGTH, 0));
	layer.has_content = true;
}

void OpenGLVideoRenderer::RenderLayer(LayerResources& layer) {
	if (!layer.has_content || layer.layout.tiles.empty())
		return;
	int const render_width = layer.render_output_layout.output_width > 0
		? layer.render_output_layout.output_width
		: layer.canvas_width;
	int const render_height = layer.render_output_layout.output_height > 0
		? layer.render_output_layout.output_height
		: layer.canvas_height;

	if (compatibility_pipeline) {
		legacy_gl::ResetCompatibilityState();
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		if (layer.layout.flipped)
			glOrtho(0.0, render_width, 0.0, render_height, -1.0, 1.0);
		else
			glOrtho(0.0, render_width, render_height, 0.0, -1.0, 1.0);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();

		if (layer.composition_mode == SubtitleOverlayCompositionMode::PremultipliedAlpha) {
			CHECK_RENDER_ERROR(glEnable(GL_BLEND));
			CHECK_RENDER_ERROR(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
		}
		else {
			CHECK_RENDER_ERROR(glDisable(GL_BLEND));
		}

		CHECK_RENDER_ERROR(glDisableClientState(GL_COLOR_ARRAY));
		CHECK_RENDER_ERROR(glColor4f(1.0f, 1.0f, 1.0f, 1.0f));
		CHECK_RENDER_ERROR(glEnable(GL_TEXTURE_2D));
		CHECK_RENDER_ERROR(glEnableClientState(GL_VERTEX_ARRAY));
		CHECK_RENDER_ERROR(glEnableClientState(GL_TEXTURE_COORD_ARRAY));
		CHECK_RENDER_ERROR(glVertexPointer(
			2,
			GL_FLOAT,
			sizeof(Vertex),
			reinterpret_cast<void const*>(&layer.vertices[0].position[0])));
		CHECK_RENDER_ERROR(glTexCoordPointer(
			2,
			GL_FLOAT,
			sizeof(Vertex),
			reinterpret_cast<void const*>(&layer.vertices[0].texcoord[0])));

		for (size_t i = 0; i < layer.texture_ids.size(); ++i) {
			CHECK_RENDER_ERROR(glBindTexture(GL_TEXTURE_2D, layer.texture_ids[i]));
			CHECK_RENDER_ERROR(glDrawElements(
				GL_TRIANGLES,
				6,
				GL_UNSIGNED_INT,
				static_cast<void const*>(layer.indices.data() + i * 6)));
		}

		CHECK_RENDER_ERROR(glDisableClientState(GL_TEXTURE_COORD_ARRAY));
		CHECK_RENDER_ERROR(glDisableClientState(GL_VERTEX_ARRAY));
		CHECK_RENDER_ERROR(glBindTexture(GL_TEXTURE_2D, 0));
		CHECK_RENDER_ERROR(glDisable(GL_TEXTURE_2D));
		return;
	}

	auto& gl = *functions;
	auto projection_matrix = BuildOpenGLVideoRendererOrthoMatrix(render_width, render_height, layer.layout.flipped);

	if (layer.composition_mode == SubtitleOverlayCompositionMode::PremultipliedAlpha) {
		CHECK_RENDER_ERROR(glEnable(GL_BLEND));
		CHECK_RENDER_ERROR(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
	}
	else {
		CHECK_RENDER_ERROR(glDisable(GL_BLEND));
	}

	gl.UseProgram(program);
	gl.UniformMatrix4fv(projection_matrix_uniform, 1, GL_FALSE, projection_matrix.data());
	gl.BindBuffer(GL_ARRAY_BUFFER, layer.vertex_buffer);
	gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, layer.element_buffer);
	gl.EnableVertexAttribArray(0);
	gl.EnableVertexAttribArray(1);
	gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, position)));
	gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(offsetof(Vertex, texcoord)));

	for (size_t i = 0; i < layer.texture_ids.size(); ++i) {
		CHECK_RENDER_ERROR(glBindTexture(GL_TEXTURE_2D, layer.texture_ids[i]));
		CHECK_RENDER_ERROR(glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, reinterpret_cast<void *>(i * 6 * sizeof(GLuint))));
	}

	gl.DisableVertexAttribArray(0);
	gl.DisableVertexAttribArray(1);
	gl.BindBuffer(GL_ARRAY_BUFFER, 0);
	gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	CHECK_RENDER_ERROR(glBindTexture(GL_TEXTURE_2D, 0));
	gl.UseProgram(0);
}

void OpenGLVideoRenderer::UploadFrame(SourceFrame const& frame) {
	if (!frame.IsValid()) {
		source_output_mode = SourceFrameOutputMode::Bgra8;
		has_source_geometry = false;
		if (render_video_layer)
			video_layer.has_content = false;
		return;
	}

	source_geometry = frame.geometry;
	source_output_mode = frame.output_mode;
	has_source_geometry = true;

	if (!render_video_layer)
		return;

	if (frame.pixel_format != SourceFramePixelFormat::Bgra8) {
		video_layer.has_content = false;
		return;
	}

	auto const layout = BuildVideoRenderCanvasLayout(frame);
	UploadBgraLayer(
		video_layer,
		frame.planes[0].data,
		frame.width,
		frame.height,
		frame.planes[0].stride,
		frame.flipped,
		layout.canvas_width,
		layout.canvas_height,
		layout.offset_x,
		layout.offset_y,
		SubtitleOverlayCompositionMode::OpaqueReplace,
		false);
}

void OpenGLVideoRenderer::UploadOverlay(SubtitleOverlay const* overlay) {
	if (!render_overlay_layer)
		return;

	OpenGLVideoRendererOverlayLayerState state;
	state.width = overlay_layer.layout.frame_width;
	state.height = overlay_layer.layout.frame_height;
	state.canvas_width = overlay_layer.canvas_width;
	state.canvas_height = overlay_layer.canvas_height;
	state.offset_x = overlay_layer.offset_x;
	state.offset_y = overlay_layer.offset_y;
	state.flipped = overlay_layer.layout.flipped;
	state.has_allocated_resources = !overlay_layer.texture_ids.empty() && overlay_layer.texture_ids.size() == overlay_layer.layout.tiles.size();
	state.has_visible_content = overlay_layer.has_content;
	state.continuity_generation = overlay_layer.continuity_generation;
	state.composition_mode = overlay_layer.composition_mode;

	SubtitleOverlay adjusted_overlay;
	if (overlay)
		adjusted_overlay = has_source_geometry
			? AdjustSubtitleOverlayForSourceGeometry(*overlay, source_geometry)
			: *overlay;
	auto const* render_overlay = overlay ? &adjusted_overlay : nullptr;

	auto plan = DecideOpenGLVideoRendererOverlayUploadPlan(state, render_overlay);
	char const* upload_phase = nullptr;
	switch (plan.action) {
		case OpenGLVideoRendererOverlayUploadAction::HideKeepResources:
			upload_phase = "video_overlay.upload.hide";
			break;
		case OpenGLVideoRendererOverlayUploadAction::FullUpload:
			upload_phase = "video_overlay.upload.full";
			break;
		case OpenGLVideoRendererOverlayUploadAction::DirtyUpload:
			upload_phase = "video_overlay.upload.dirty";
			break;
		case OpenGLVideoRendererOverlayUploadAction::ReuseExistingContent:
			upload_phase = "video_overlay.upload.reuse";
			break;
	}
	perf_trace::VideoUiDurationScope upload_trace(upload_phase);
	if (upload_trace.IsActive()) {
		upload_trace.SetDetails(
			EstimateOpenGLVideoRendererOverlayUploadBytes(plan.action, render_overlay),
			render_overlay ? render_overlay->dirty_rect_count : 0);
	}
	bool const apply_source_display_transform =
		has_source_geometry && source_output_mode == SourceFrameOutputMode::Native;
	if (plan.action == OpenGLVideoRendererOverlayUploadAction::HideKeepResources) {
		HideLayer(overlay_layer);
		return;
	}
	if (plan.action == OpenGLVideoRendererOverlayUploadAction::FullUpload) {
		UploadBgraLayer(
			overlay_layer,
			render_overlay->planes[0].data,
			render_overlay->width,
			render_overlay->height,
			render_overlay->planes[0].stride,
			render_overlay->flipped,
			render_overlay->canvas_width,
			render_overlay->canvas_height,
			render_overlay->target_x,
			render_overlay->target_y,
			render_overlay->composition_mode,
			apply_source_display_transform);
		overlay_layer.continuity_generation = render_overlay->continuity_generation;
		return;
	}
	if (UpdateLayerRenderOutputLayout(overlay_layer, apply_source_display_transform))
		RebuildLayerGeometry(overlay_layer);
	if (plan.action == OpenGLVideoRendererOverlayUploadAction::DirtyUpload) {
		UploadDirtyRects(
			overlay_layer,
			render_overlay->planes[0].data,
			render_overlay->planes[0].stride,
			render_overlay->dirty_rects,
			render_overlay->dirty_rect_count);
		overlay_layer.continuity_generation = render_overlay->continuity_generation;
		return;
	}

	overlay_layer.continuity_generation = render_overlay->continuity_generation;
	overlay_layer.has_content = true;
}

void OpenGLVideoRenderer::Render(RenderViewport const& viewport, int, int) {
	bool const has_video = render_video_layer && video_layer.has_content && !video_layer.layout.tiles.empty();
	bool const has_overlay = render_overlay_layer && overlay_layer.has_content && !overlay_layer.layout.tiles.empty();
	if ((!has_video && !has_overlay) || viewport.width <= 0 || viewport.height <= 0)
		return;

	EnsureInitialized();

	CHECK_RENDER_ERROR(glDisable(GL_SCISSOR_TEST));
	CHECK_RENDER_ERROR(glDisable(GL_STENCIL_TEST));
	CHECK_RENDER_ERROR(glDisable(GL_CULL_FACE));
	CHECK_RENDER_ERROR(glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));
	if (clear_before_render) {
		CHECK_RENDER_ERROR(glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
		CHECK_RENDER_ERROR(glClearStencil(0));
		CHECK_RENDER_ERROR(glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT));
	}
	CHECK_RENDER_ERROR(glViewport(viewport.x, viewport.y, viewport.width, viewport.height));

	if (has_video)
		RenderLayer(video_layer);
	if (has_overlay)
		RenderLayer(overlay_layer);
}
