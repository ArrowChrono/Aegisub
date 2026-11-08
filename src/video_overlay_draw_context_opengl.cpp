#include "video_overlay_draw_context_opengl.h"

#include "gl_wrap.h"

#include <libaegisub/exception.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#ifdef DrawText
#undef DrawText
#endif
#endif
#ifdef HAVE_OPENGL_GL_H
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "legacy_gl_draw.h"

void legacy_gl::PrepareVisualToolState(int max_clip_planes) {
	if (max_clip_planes < 6)
		throw std::logic_error("Visual tools require initialized OpenGL clip plane capabilities");
	glDisable(GL_FOG);
	glDisable(GL_LIGHTING);
	glDisable(GL_COLOR_MATERIAL);
	for (GLint i = 0; i < max_clip_planes; ++i)
		glDisable(GL_CLIP_PLANE0 + static_cast<GLenum>(i));
}

namespace {
void CheckPerspectiveGl(char const *operation) {
	if (auto const error = glGetError(); error != GL_NO_ERROR)
		throw agi::EnvironmentError(std::string("perspective lines: ") + operation + ": GL error " + std::to_string(error));
}

bool IsRecoverableWindowError(GLenum error) noexcept {
	return error == GL_INVALID_ENUM || error == GL_INVALID_VALUE || error == GL_INVALID_OPERATION;
}

struct WindowErrors {
	std::array<GLenum, 16> values{};
	std::size_t count = 0;
};

WindowErrors ReadWindowErrors(char const *operation) noexcept {
	WindowErrors errors;
	for (std::size_t i = 0; i <= errors.values.size(); ++i) {
		auto const error = glGetError();
		if (error == GL_NO_ERROR)
			return errors;
		if (i == errors.values.size()) {
			std::fprintf(stderr, "perspective lines: %s error drain did not converge: GL errors", operation);
			for (std::size_t j = 0; j < errors.count; ++j)
				std::fprintf(stderr, " %u", errors.values[j]);
			std::fprintf(stderr, " %u\n", error);
			std::terminate();
		}
		errors.values[errors.count++] = error;
	}
	std::terminate();
}

[[noreturn]] void TerminateWindowErrors(char const *operation, WindowErrors const& errors) noexcept {
	std::fprintf(stderr, "perspective lines: %s failed: GL errors", operation);
	for (std::size_t i = 0; i < errors.count; ++i)
		std::fprintf(stderr, " %u", errors.values[i]);
	std::fputc('\n', stderr);
	std::terminate();
}

void ValidateWindowErrors(char const *operation, WindowErrors const& errors) noexcept {
	for (std::size_t i = 0; i < errors.count; ++i)
		if (!IsRecoverableWindowError(errors.values[i]))
			TerminateWindowErrors(operation, errors);
}

void ThrowWindowErrors(char const *operation, WindowErrors const& errors) {
	ValidateWindowErrors(operation, errors);
	if (!errors.count)
		return;
	std::string message = std::string("perspective lines: ") + operation + ": GL errors";
	for (std::size_t i = 0; i < errors.count; ++i)
		message += " " + std::to_string(errors.values[i]);
	throw agi::EnvironmentError(message);
}

void CheckPerspectiveRestore(char const *operation) noexcept {
	if (auto const error = glGetError(); error != GL_NO_ERROR) {
		std::fprintf(stderr, "perspective lines: %s failed: GL error %u\n", operation, error);
		std::terminate();
	}
}

void CheckPerspectivePush(char const *operation) {
	if (auto const error = glGetError(); error != GL_NO_ERROR) {
		if (error != GL_STACK_OVERFLOW && error != GL_INVALID_OPERATION) {
			std::fprintf(stderr, "perspective lines: %s outcome unknown: GL error %u\n", operation, error);
			std::terminate();
		}
		throw agi::EnvironmentError(std::string("perspective lines: ") + operation + ": GL error " + std::to_string(error));
	}
}
}

int legacy_gl::QueryMaxClipPlanes() {
	CheckPerspectiveGl("clip plane limit entry");
	GLint value = -1;
	glGetIntegerv(GL_MAX_CLIP_PLANES, &value);
	CheckPerspectiveGl("clip plane limit");
	if (value < 6)
		throw agi::EnvironmentError("Could not query OpenGL user clip plane limit");
	return value;
}

OpenGLWindowOverlayPass::Canvas OpenGLWindowOverlayPass::SetupCanvas(int physical_width, int physical_height,
																	 double logical_width, double logical_height, double depth) {
	if (physical_width <= 0 || physical_height <= 0 || !std::isfinite(logical_width) || logical_width <= 0 ||
		!std::isfinite(logical_height) || logical_height <= 0 || !std::isfinite(depth) || depth <= 0)
		throw agi::EnvironmentError("window overlay: invalid canvas");
	for (double value : {2 / logical_width, 2 / logical_height, 1 / depth})
		if (!std::isfinite(value) || value > std::numeric_limits<float>::max() || static_cast<float>(value) == 0)
			throw agi::EnvironmentError("window overlay: unrepresentable projection");
	CheckPerspectiveGl("window canvas entry");
	legacy_gl::ResetCompatibilityState();
	glViewport(0, 0, physical_width, physical_height);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, logical_width, logical_height, 0, -depth, depth);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	CheckPerspectiveGl("window canvas setup");
	double const max_matrix = std::max({1., 2 / logical_width, 2 / logical_height, 1 / depth}) *
							  (1. + 2 * std::numeric_limits<float>::epsilon());
	return Canvas(logical_width, logical_height, depth, max_matrix);
}

OpenGLWindowOverlayPass::OpenGLWindowOverlayPass(Canvas const& canvas, bool alpha_write, int max_clip_planes)
	: logical_width(canvas.logical_width), logical_height(canvas.logical_height), depth(canvas.depth), max_matrix(canvas.max_matrix),
	  alpha_write(alpha_write) {
	Initialize(max_clip_planes);
}

OpenGLWindowOverlayPass::OpenGLWindowOverlayPass(int physical_width, int physical_height,
												 double logical_width, double logical_height, double depth, bool alpha_write, int max_clip_planes)
	: OpenGLWindowOverlayPass(SetupCanvas(physical_width, physical_height, logical_width, logical_height, depth), alpha_write, max_clip_planes) {
}

void OpenGLWindowOverlayPass::Initialize(int max_clip_planes) {
	legacy_gl::PrepareVisualToolState(max_clip_planes);
	glShadeModel(GL_FLAT);
	if (!alpha_write)
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	active = true;
}

void OpenGLWindowOverlayPass::Restore(bool restore_batch_baseline) noexcept {
	if (restore_batch_baseline) {
		glDisableClientState(GL_COLOR_ARRAY);
		glDisableClientState(GL_VERTEX_ARRAY);
		glDisableClientState(GL_NORMAL_ARRAY);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(0, logical_width, logical_height, 0, -depth, depth);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glDisable(GL_CLIP_PLANE0);
		glShadeModel(GL_FLAT);
	}
	if (restore_batch_baseline || !alpha_write) {
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		CheckPerspectiveRestore("window overlay finish");
	}
	active = false;
}

OpenGLWindowOverlayPass::~OpenGLWindowOverlayPass() noexcept {
	if (active) {
		auto const errors = ReadWindowErrors("window submission during unwind");
		ValidateWindowErrors("window submission during unwind", errors);
		Restore(true);
		if (errors.count)
			TerminateWindowErrors("window submission during unwind", errors);
	}
}

void OpenGLWindowOverlayPass::Finish() {
	if (!active)
		throw std::logic_error("window overlay pass is already finished");
	auto const errors = ReadWindowErrors("window submission");
	ValidateWindowErrors("window submission", errors);
	Restore(errors.count != 0);
	ThrowWindowErrors("window submission", errors);
}

OpenGLVideoOverlayDrawContext::OpenGLVideoOverlayDrawContext(OpenGLWrapper& gl) : gl(gl) {}
OpenGLVideoOverlayDrawContext::OpenGLVideoOverlayDrawContext(OpenGLWrapper& gl, OpenGLWindowOverlayPass& pass)
	: gl(gl), window_pass(&pass) {
	if (!pass.active)
		throw std::logic_error("window overlay context requires an active pass");
}
void OpenGLVideoOverlayDrawContext::SetLineColour(agi::Color const& colour, float alpha, int width) { gl.SetLineColour(colour, alpha, width); }
void OpenGLVideoOverlayDrawContext::SetFillColour(agi::Color const& colour, float alpha) { gl.SetFillColour(colour, alpha); }
void OpenGLVideoOverlayDrawContext::SetInvert() { gl.SetInvert(); }
void OpenGLVideoOverlayDrawContext::ClearInvert() { gl.ClearInvert(); }
void OpenGLVideoOverlayDrawContext::ValidatePerspectiveTransform(VideoOverlayPerspectiveTransform const& transform, int width) const {
	if (window_pass && !window_pass->active)
		throw std::logic_error("window overlay context outlived its active pass");
	static_assert(std::is_standard_layout_v<VideoOverlayPerspectiveVertex>);
	static_assert(offsetof(VideoOverlayPerspectiveVertex, opacity) == 3 * sizeof(float));
	if (width <= 0)
		throw agi::EnvironmentError("perspective lines: invalid batch");
	float const parameters[] = {transform.origin.X(), transform.origin.Y(), transform.angle_x, transform.angle_y,
								transform.angle_z, transform.fax, transform.fay, transform.perspective_z_scale};
	for (float value : parameters)
		if (!std::isfinite(value))
			throw agi::EnvironmentError("perspective lines: non-finite transform");
	if (transform.perspective_z_scale <= 0)
		throw agi::EnvironmentError("perspective lines: invalid perspective scale");
}

void OpenGLVideoOverlayDrawContext::DrawPreparedPerspectiveBatches(VideoOverlayPreparedPerspectiveBatches const& prepared,
																   VideoOverlayPerspectiveTransform const& transform, agi::Color const& primary, int width) {
	ValidatePerspectiveTransform(transform, width);
	SubmitPerspectiveBatches(prepared.Grid(), prepared.Axes(), prepared.Arrows(), transform, primary, prepared.SecondaryColour(),
							 width, prepared.MaxCoordinate(), prepared.GridColours());
}

void OpenGLVideoOverlayDrawContext::DrawPerspectiveBatches(std::span<VideoOverlayPerspectiveVertex const> grid,
														   std::span<VideoOverlayPerspectiveVertex const> axes, std::span<VideoOverlayPerspectiveVertex const> arrows,
														   VideoOverlayPerspectiveTransform const& transform, agi::Color const& primary, agi::Color const& secondary, int width) {
	ValidatePerspectiveTransform(transform, width);
	if (grid.empty() || axes.empty() || arrows.empty() || grid.size() % 2 || axes.size() % 2 || arrows.size() % 2 ||
		grid.size() > static_cast<size_t>(std::numeric_limits<GLsizei>::max()) ||
		axes.size() > static_cast<size_t>(std::numeric_limits<GLsizei>::max()) ||
		arrows.size() > static_cast<size_t>(std::numeric_limits<GLsizei>::max()))
		throw agi::EnvironmentError("perspective lines: invalid batch");
	float max_vertex = 0;
	for (auto const vertices : {grid, axes, arrows})
		for (auto const& vertex : vertices) {
			if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z) ||
				!std::isfinite(vertex.opacity) || vertex.opacity < 0 || vertex.opacity > 1)
				throw agi::EnvironmentError("perspective lines: invalid vertex");
			max_vertex = std::max(max_vertex, std::abs(vertex.x));
			max_vertex = std::max(max_vertex, std::abs(vertex.y));
			max_vertex = std::max(max_vertex, std::abs(vertex.z));
		}
	constexpr size_t stack_grid_vertices = 256;
	std::array<float, stack_grid_vertices * 4> stack_colours;
	std::vector<float> heap_colours;
	float *colours = stack_colours.data();
	if (grid.size() > stack_grid_vertices) {
		heap_colours.resize(grid.size() * 4);
		colours = heap_colours.data();
	}
	FillVideoOverlayPerspectiveGridColours(grid, secondary, {colours, grid.size() * 4});
	SubmitPerspectiveBatches(grid, axes, arrows, transform, primary, secondary, width, max_vertex, {colours, grid.size() * 4});
}

void OpenGLVideoOverlayDrawContext::SubmitPerspectiveBatches(std::span<VideoOverlayPerspectiveVertex const> grid,
															 std::span<VideoOverlayPerspectiveVertex const> axes, std::span<VideoOverlayPerspectiveVertex const> arrows,
															 VideoOverlayPerspectiveTransform const& transform, agi::Color const& primary, agi::Color const& secondary,
															 int width, float max_vertex, std::span<float const> colours) {
	if (!window_pass)
		CheckPerspectiveGl("entry");
	std::array<GLfloat, 16> modelview{}, projection{};
	double max_matrix = 0;
	if (window_pass) {
		max_matrix = window_pass->max_matrix;
	}
	else {
		if (auto const enabled = glIsEnabled(GL_CLIP_PLANE0); (CheckPerspectiveGl("clip plane"), enabled == GL_TRUE))
			throw agi::EnvironmentError("perspective lines: clip plane 0 is already active");
		if (auto const enabled = glIsEnabled(GL_COLOR_MATERIAL); (CheckPerspectiveGl("color material"), enabled == GL_TRUE))
			throw agi::EnvironmentError("perspective lines: color material is active");
		glGetFloatv(GL_MODELVIEW_MATRIX, modelview.data());
		CheckPerspectiveGl("modelview");
		glGetFloatv(GL_PROJECTION_MATRIX, projection.data());
		CheckPerspectiveGl("projection");
		for (auto matrix : {modelview, projection})
			for (float value : matrix)
				if (!std::isfinite(value))
					throw agi::EnvironmentError("perspective lines: non-finite matrix");
		for (int i : {2, 3, 6, 7, 8, 9, 11, 14})
			if (modelview[i] != 0 || projection[i] != 0)
				throw agi::EnvironmentError("perspective lines: unsupported matrix");
		if (modelview[10] != 1 || modelview[15] != 1 || projection[15] != 1 || projection[0] == 0 || projection[5] == 0)
			throw agi::EnvironmentError("perspective lines: unsupported matrix");
		for (float value : modelview)
			max_matrix = std::max(max_matrix, std::abs(static_cast<double>(value)));
		for (float value : projection)
			max_matrix = std::max(max_matrix, std::abs(static_cast<double>(value)));
	}

	double const bound = (1 + static_cast<double>(max_vertex)) * (1 + std::abs(static_cast<double>(transform.fax)) + std::abs(static_cast<double>(transform.fay))) *
						 (1 + transform.perspective_z_scale) * (1 + std::abs(static_cast<double>(transform.origin.X())) + std::abs(static_cast<double>(transform.origin.Y()))) *
						 (1 + max_matrix) * 2500;
	if (!std::isfinite(bound) || bound > std::numeric_limits<float>::max() / 128.)
		throw agi::EnvironmentError("perspective lines: transform exceeds finite range");

	auto check = [&](char const *operation) {
		if (!window_pass)
			CheckPerspectiveGl(operation);
	};
	bool owned_changed = false;
	bool colours_enabled = false;
	bool const flatten_projection = !window_pass || window_pass->depth < 1;
	bool attrib_pushed = false, projection_pushed = false, modelview_pushed = false, client_pushed = false;
	auto restore = [&]() noexcept {
		if (owned_changed) {
			if (colours_enabled)
				glDisableClientState(GL_COLOR_ARRAY);
			glDisableClientState(GL_VERTEX_ARRAY);
			if (flatten_projection) {
				glMatrixMode(GL_PROJECTION);
				glLoadIdentity();
				glOrtho(0, window_pass->logical_width, window_pass->logical_height, 0, -window_pass->depth, window_pass->depth);
			}
			glShadeModel(GL_FLAT);
			if (flatten_projection)
				glMatrixMode(GL_MODELVIEW);
			glLoadIdentity();
			glDisable(GL_CLIP_PLANE0);
			owned_changed = false;
			return;
		}
		if (client_pushed) {
			glPopClientAttrib();
			CheckPerspectiveRestore("client arrays");
			client_pushed = false;
		}
		if (modelview_pushed) {
			glMatrixMode(GL_MODELVIEW);
			CheckPerspectiveRestore("modelview mode");
			glPopMatrix();
			CheckPerspectiveRestore("modelview");
			modelview_pushed = false;
		}
		if (projection_pushed) {
			glMatrixMode(GL_PROJECTION);
			CheckPerspectiveRestore("projection mode");
			glPopMatrix();
			CheckPerspectiveRestore("projection");
			projection_pushed = false;
		}
		if (attrib_pushed) {
			glPopAttrib();
			CheckPerspectiveRestore("attributes");
			attrib_pushed = false;
		}
	};
	auto on_exit = agi::make_scope_exit([&] {
		restore();
	});
	if (window_pass) {
		owned_changed = true;
	}
	else {
		glPushAttrib(GL_TRANSFORM_BIT | GL_LIGHTING_BIT);
		CheckPerspectivePush("attribute push");
		attrib_pushed = true;
	}
	if (flatten_projection) {
		glMatrixMode(GL_PROJECTION);
		check("projection mode");
		if (!window_pass) {
			glPushMatrix();
			CheckPerspectivePush("projection push");
			projection_pushed = true;
		}
		glScalef(1, 1, 0);
		check("projection flatten");
	}
	if (flatten_projection) {
		glMatrixMode(GL_MODELVIEW);
		check("modelview mode");
	}
	if (!window_pass) {
		glPushMatrix();
		CheckPerspectivePush("modelview push");
		modelview_pushed = true;
	}
	glLoadIdentity();
	check("identity");
	GLdouble const near_plane[] = {0, 0, 1, 1};
	glClipPlane(GL_CLIP_PLANE0, near_plane);
	check("near plane");
	glEnable(GL_CLIP_PLANE0);
	check("enable near plane");
	if (!window_pass) {
		glMultMatrixf(modelview.data());
		check("modelview composition");
	}
	glTranslatef(transform.origin.X(), transform.origin.Y(), -1);
	check("origin");
	GLfloat const perspective[] = {2500, 0, 0, 0, 0, 2500, 0, 0, 0, 0, 1, 1, 0, 0, 2499, 2500};
	glMultMatrixf(perspective);
	check("perspective");
	glScalef(1, 1, transform.perspective_z_scale);
	check("scale");
	glRotatef(transform.angle_y, 0, -1, 0);
	check("rotate y");
	glRotatef(transform.angle_x, -1, 0, 0);
	check("rotate x");
	glRotatef(transform.angle_z, 0, 0, -1);
	check("rotate z");
	GLfloat const shear[] = {1, transform.fay, 0, 0, transform.fax, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	glMultMatrixf(shear);
	check("shear");
	if (!window_pass) {
		glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
		CheckPerspectivePush("client push");
		client_pushed = true;
		glDisable(GL_LIGHTING);
		check("disable lighting");
	}
	gl.SetLineColour(secondary, 1, width);
	gl.SetModeLine();
	check("grid style");
	glShadeModel(GL_SMOOTH);
	check("smooth shading");
	glEnableClientState(GL_VERTEX_ARRAY);
	check("enable vertices");
	glEnableClientState(GL_COLOR_ARRAY);
	colours_enabled = true;
	check("enable colours");
	glColorPointer(4, GL_FLOAT, 0, colours.data());
	check("colour array");
	glVertexPointer(3, GL_FLOAT, sizeof(VideoOverlayPerspectiveVertex), &grid.front().x);
	check("grid array");
	glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(grid.size()));
	check("grid draw");
	glDisableClientState(GL_COLOR_ARRAY);
	check("disable colours");
	colours_enabled = false;
	gl.SetLineColour(primary, 1, width);
	gl.ApplyLineColour();
	check("primary style");
	glVertexPointer(3, GL_FLOAT, sizeof(VideoOverlayPerspectiveVertex), &axes.front().x);
	check("axes array");
	glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(axes.size()));
	check("axes draw");
	glVertexPointer(3, GL_FLOAT, sizeof(VideoOverlayPerspectiveVertex), &arrows.front().x);
	check("arrow array");
	glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(arrows.size()));
	check("arrow draw");
	restore();
	on_exit.release();
}
void OpenGLVideoOverlayDrawContext::DrawLine(Vector2D p1, Vector2D p2) { gl.DrawLine(p1, p2); }
void OpenGLVideoOverlayDrawContext::DrawLines(size_t dim, float const *lines, size_t n) { gl.DrawLines(dim, lines, n); }
void OpenGLVideoOverlayDrawContext::DrawLineStrip(Vector2D const *points, size_t n) { gl.DrawLineStrip(points, n); }
void OpenGLVideoOverlayDrawContext::DrawRectangle(Vector2D p1, Vector2D p2) { gl.DrawRectangle(p1, p2); }
void OpenGLVideoOverlayDrawContext::DrawPolygon(Vector2D const *points, size_t n) { gl.DrawPolygon(points, n); }
void OpenGLVideoOverlayDrawContext::DrawMultiPolygon(std::vector<float> const& points, std::vector<int> const& start, std::vector<int> const& count, Vector2D video_pos, Vector2D video_size, bool invert) { gl.DrawMultiPolygon(points, start, count, video_pos, video_size, invert); }
void OpenGLVideoOverlayDrawContext::DrawCircle(Vector2D center, float radius) { gl.DrawCircle(center, radius); }
void OpenGLVideoOverlayDrawContext::DrawTriangle(Vector2D p1, Vector2D p2, Vector2D p3) { gl.DrawTriangle(p1, p2, p3); }
VideoOverlayTextExtent OpenGLVideoOverlayDrawContext::MeasureText(std::string const&, VideoOverlayTextStyle const&) { throw std::logic_error("This OpenGL overlay context has no text renderer"); }
void OpenGLVideoOverlayDrawContext::DrawText(std::string const&, int, int, VideoOverlayTextStyle const&) { throw std::logic_error("This OpenGL overlay context has no text renderer"); }

OpenGLTextVideoOverlayDrawContext::OpenGLTextVideoOverlayDrawContext(OpenGLWrapper& gl, VideoOverlayTextRenderer& text)
	: OpenGLVideoOverlayDrawContext(gl), text_renderer(text) {}
OpenGLTextVideoOverlayDrawContext::OpenGLTextVideoOverlayDrawContext(OpenGLWrapper& gl, OpenGLWindowOverlayPass& pass, VideoOverlayTextRenderer& text)
	: OpenGLVideoOverlayDrawContext(gl, pass), text_renderer(text) {}
VideoOverlayTextExtent OpenGLTextVideoOverlayDrawContext::MeasureText(std::string const& text, VideoOverlayTextStyle const& style) {
	return text_renderer.MeasureText(text, style);
}
void OpenGLTextVideoOverlayDrawContext::DrawText(std::string const& text, int x, int y, VideoOverlayTextStyle const& style) {
	text_renderer.DrawText(text, x, y, style);
}
