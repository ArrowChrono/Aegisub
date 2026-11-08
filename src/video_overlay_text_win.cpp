#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#ifdef DrawText
#undef DrawText
#endif

#include "video_overlay_text_win.h"

#include "legacy_gl_draw.h"

#include <libaegisub/charset_conv_win.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <limits>
#include <list>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
constexpr std::size_t layout_cache_limit = 32;
constexpr std::size_t mask_cache_limit = 32;
constexpr std::size_t mask_byte_limit = 16 * 1024 * 1024;

void CheckResult(HRESULT result, char const *operation) {
	if (FAILED(result))
		throw std::runtime_error(std::string("overlay text: ") + operation + ": HRESULT " + std::to_string(result));
}

void CheckGl(char const *operation) {
	auto error = glGetError();
	if (error == GL_NO_ERROR)
		return;
	std::string message = std::string("overlay text: ") + operation + ": GL errors";
	for (int count = 0; error != GL_NO_ERROR && count < 16; ++count) {
		message += " " + std::to_string(error);
		error = glGetError();
	}
	if (error != GL_NO_ERROR)
		std::terminate();
	throw std::runtime_error(message);
}

void CheckGlNoexcept(char const *operation) noexcept {
	if (auto const error = glGetError(); error != GL_NO_ERROR) {
		std::fprintf(stderr, "overlay text: %s: GL error %u\n", operation, error);
		std::terminate();
	}
}

bool HasExtension(std::string_view extensions, std::string_view extension) {
	auto offset = extensions.find(extension);
	while (offset != std::string_view::npos) {
		auto const end = offset + extension.size();
		if ((!offset || extensions[offset - 1] == ' ') && (end == extensions.size() || extensions[end] == ' '))
			return true;
		offset = extensions.find(extension, offset + 1);
	}
	return false;
}

struct GlCapabilities {
	PFNGLACTIVETEXTUREPROC active_texture = nullptr;
	PFNGLBINDBUFFERPROC bind_buffer = nullptr;
	PFNGLUSEPROGRAMPROC use_program = nullptr;
	PFNGLBLENDEQUATIONPROC blend_equation = nullptr;
	PFNGLBINDSAMPLERPROC bind_sampler = nullptr;
	GLint texture_units = 1;
	GLint maximum_texture_size = 0;
	GLint clip_planes = 0;
	bool texture_3d = false;
	bool cube_map = false;
	bool rectangle = false;
	bool non_power_of_two = false;
	bool pixel_unpack_buffer = false;

	static GlCapabilities Read() {
		GlCapabilities caps;
		auto const *version_string = reinterpret_cast<char const *>(glGetString(GL_VERSION));
		auto const *extension_string = reinterpret_cast<char const *>(glGetString(GL_EXTENSIONS));
		CheckGl("read context capabilities");
		int major = 0, minor = 0;
		if (!version_string || !extension_string || std::sscanf(version_string, "%d.%d", &major, &minor) != 2)
			throw std::logic_error("Overlay text requires a compatibility OpenGL context");
		auto const version = [major, minor](int requested_major, int requested_minor) {
			return major > requested_major || (major == requested_major && minor >= requested_minor);
		};
		std::string_view const extensions(extension_string);
		auto const extension = [extensions](std::string_view name) { return HasExtension(extensions, name); };
		if (version(1, 3) || extension("GL_ARB_multitexture"))
			caps.active_texture = legacy_gl::LoadOptionalProc<PFNGLACTIVETEXTUREPROC>("glActiveTexture", "glActiveTextureARB");
		if (version(1, 5) || extension("GL_ARB_vertex_buffer_object"))
			caps.bind_buffer = legacy_gl::LoadOptionalProc<PFNGLBINDBUFFERPROC>("glBindBuffer", "glBindBufferARB");
		if (version(2, 0))
			caps.use_program = legacy_gl::LoadOptionalProc<PFNGLUSEPROGRAMPROC>("glUseProgram");
		if (version(1, 4) || extension("GL_EXT_blend_minmax") || extension("GL_EXT_blend_subtract"))
			caps.blend_equation = legacy_gl::LoadOptionalProc<PFNGLBLENDEQUATIONPROC>("glBlendEquation", "glBlendEquationEXT");
		if (version(3, 3) || extension("GL_ARB_sampler_objects"))
			caps.bind_sampler = legacy_gl::LoadOptionalProc<PFNGLBINDSAMPLERPROC>("glBindSampler");
		caps.texture_3d = version(1, 2) || extension("GL_EXT_texture3D");
		caps.cube_map = version(1, 3) || extension("GL_ARB_texture_cube_map") || extension("GL_EXT_texture_cube_map");
		caps.rectangle = version(3, 1) || extension("GL_ARB_texture_rectangle") || extension("GL_EXT_texture_rectangle") || extension("GL_NV_texture_rectangle");
		caps.non_power_of_two = version(2, 0) || extension("GL_ARB_texture_non_power_of_two");
		caps.pixel_unpack_buffer = version(2, 1) || extension("GL_ARB_pixel_buffer_object") || extension("GL_EXT_pixel_buffer_object");
		if (caps.active_texture)
			glGetIntegerv(GL_MAX_TEXTURE_UNITS, &caps.texture_units);
		glGetIntegerv(GL_MAX_TEXTURE_SIZE, &caps.maximum_texture_size);
		glGetIntegerv(GL_MAX_CLIP_PLANES, &caps.clip_planes);
		CheckGl("read context limits");
		if (caps.texture_units <= 0 || caps.maximum_texture_size <= 0 || caps.clip_planes < 0 ||
			(caps.pixel_unpack_buffer && !caps.bind_buffer))
			throw std::logic_error("Overlay text received invalid OpenGL capabilities");
		return caps;
	}
};

class TextGlState final {
	GlCapabilities const& caps;
	GLint original_active_texture = GL_TEXTURE0;
	GLint original_program = 0;
	GLint original_unpack_buffer = 0;
	GLint original_sampler = 0;
	GLint original_matrix_mode = GL_MODELVIEW;
	bool server_saved = false;
	bool client_saved = false;
	bool texture_matrix_saved = false;
	bool sampler_saved = false;
	bool modern_saved = false;

	void Restore() noexcept {
		if (texture_matrix_saved) {
			if (caps.active_texture)
				caps.active_texture(GL_TEXTURE0);
			glMatrixMode(GL_TEXTURE);
			glPopMatrix();
		}
		glMatrixMode(static_cast<GLenum>(original_matrix_mode));
		if (sampler_saved)
			caps.bind_sampler(0, static_cast<GLuint>(original_sampler));
		if (modern_saved) {
			if (caps.use_program)
				caps.use_program(static_cast<GLuint>(original_program));
			if (caps.pixel_unpack_buffer)
				caps.bind_buffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(original_unpack_buffer));
		}
		if (client_saved)
			glPopClientAttrib();
		if (server_saved)
			glPopAttrib();
		if (caps.active_texture)
			caps.active_texture(static_cast<GLenum>(original_active_texture));
		CheckGlNoexcept("restore caller state");
	}

	public:
	explicit TextGlState(GlCapabilities const& caps) : caps(caps) {
		glGetIntegerv(GL_MATRIX_MODE, &original_matrix_mode);
		if (caps.active_texture)
			glGetIntegerv(GL_ACTIVE_TEXTURE, &original_active_texture);
		if (caps.use_program)
			glGetIntegerv(GL_CURRENT_PROGRAM, &original_program);
		if (caps.pixel_unpack_buffer)
			glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &original_unpack_buffer);
		CheckGl("read caller state");
		auto rollback = agi::make_scope_exit([this] { Restore(); });

		glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT | GL_TEXTURE_BIT | GL_PIXEL_MODE_BIT | GL_POLYGON_BIT);
		CheckGl("save server state");
		server_saved = true;
		glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
		CheckGl("save pixel unpack state");
		client_saved = true;
		modern_saved = true;
		if (caps.use_program)
			caps.use_program(0);
		if (caps.pixel_unpack_buffer)
			caps.bind_buffer(GL_PIXEL_UNPACK_BUFFER, 0);

		for (GLint unit = 0; unit < caps.texture_units; ++unit) {
			if (caps.active_texture)
				caps.active_texture(static_cast<GLenum>(GL_TEXTURE0 + unit));
			glDisable(GL_TEXTURE_1D);
			glDisable(GL_TEXTURE_2D);
			if (caps.texture_3d)
				glDisable(GL_TEXTURE_3D);
			if (caps.cube_map)
				glDisable(GL_TEXTURE_CUBE_MAP);
			if (caps.rectangle)
				glDisable(GL_TEXTURE_RECTANGLE);
			glDisable(GL_TEXTURE_GEN_S);
			glDisable(GL_TEXTURE_GEN_T);
			glDisable(GL_TEXTURE_GEN_R);
			glDisable(GL_TEXTURE_GEN_Q);
		}
		if (caps.active_texture)
			caps.active_texture(GL_TEXTURE0);
		if (caps.bind_sampler) {
			glGetIntegerv(GL_SAMPLER_BINDING, &original_sampler);
			CheckGl("read caller sampler");
			sampler_saved = true;
			caps.bind_sampler(0, 0);
		}
		glMatrixMode(GL_TEXTURE);
		glPushMatrix();
		CheckGl("save texture matrix");
		texture_matrix_saved = true;
		glLoadIdentity();
		glMatrixMode(static_cast<GLenum>(original_matrix_mode));

		glEnable(GL_TEXTURE_2D);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		if (caps.blend_equation)
			caps.blend_equation(GL_FUNC_ADD);
		glDisable(GL_ALPHA_TEST);
		glDisable(GL_COLOR_LOGIC_OP);
		glDisable(GL_LIGHTING);
		glDisable(GL_COLOR_MATERIAL);
		glDisable(GL_FOG);
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_STENCIL_TEST);
		glDisable(GL_CULL_FACE);
		glDisable(GL_POLYGON_SMOOTH);
		glDisable(GL_POLYGON_STIPPLE);
		for (GLint plane = 0; plane < caps.clip_planes; ++plane)
			glDisable(static_cast<GLenum>(GL_CLIP_PLANE0 + plane));
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
		glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE);
		glPixelStorei(GL_UNPACK_LSB_FIRST, GL_FALSE);
		glPixelTransferi(GL_MAP_COLOR, GL_FALSE);
		glPixelTransferf(GL_ALPHA_SCALE, 1.0f);
		glPixelTransferf(GL_ALPHA_BIAS, 0.0f);
		CheckGl("prepare text state");
		rollback.release();
	}

	~TextGlState() { Restore(); }
	TextGlState(TextGlState const&) = delete;
	TextGlState& operator=(TextGlState const&) = delete;
};

int LogicalExtent(float value) {
	if (!std::isfinite(value) || value < 0.0f || std::ceil(static_cast<double>(value)) > std::numeric_limits<int>::max())
		throw std::length_error("Overlay text logical extent is not representable");
	return static_cast<int>(std::ceil(value));
}

int TextureExtent(int extent, GlCapabilities const& caps) {
	if (extent <= 0 || extent > caps.maximum_texture_size)
		throw std::length_error("Overlay text exceeds the OpenGL texture size limit");
	if (caps.non_power_of_two)
		return extent;
	int result = 1;
	while (result < extent) {
		if (result > caps.maximum_texture_size / 2)
			throw std::length_error("Overlay text exceeds the OpenGL texture size limit");
		result *= 2;
	}
	return result;
}

void ExpandMask(std::vector<unsigned char> const& source, std::vector<unsigned char>& destination,
				int width, int height, double radius, bool horizontal) {
	int const length = horizontal ? width : height;
	int const lines = horizontal ? height : width;
	int const integral_radius = static_cast<int>(std::floor(radius));
	double const fraction = radius - integral_radius;
	std::vector<int> queue(static_cast<std::size_t>(length));
	for (int line = 0; line < lines; ++line) {
		auto index = [=](int position) {
			return horizontal ? static_cast<std::size_t>(line) * width + position : static_cast<std::size_t>(position) * width + line;
		};
		auto sample = [&](int position) -> unsigned char {
			return position >= 0 && position < length ? source[index(position)] : 0;
		};
		int head = 0, tail = 0, added = -1;
		for (int position = 0; position < length; ++position) {
			int const right = std::min(length - 1, position + integral_radius);
			while (added < right) {
				++added;
				while (head < tail && sample(queue[tail - 1]) <= sample(added))
					--tail;
				queue[tail++] = added;
			}
			while (head < tail && queue[head] < position - integral_radius)
				++head;
			double maximum = head < tail ? sample(queue[head]) : 0;
			if (fraction > 0.0) {
				for (int sign : {-1, 1}) {
					auto const boundary = position + sign * integral_radius;
					maximum = std::max(maximum, sample(boundary) * (1.0 - fraction) + sample(boundary + sign) * fraction);
				}
			}
			destination[index(position)] = static_cast<unsigned char>(std::lround(maximum));
		}
	}
}
}

class OpenGLVideoOverlayText::Impl {
	struct Layout {
		std::string text, face;
		int size;
		bool bold, italic;
		ComPtr<IDWriteTextLayout> value;
		VideoOverlayTextExtent extent;
		DWRITE_TEXT_METRICS metrics{};
		DWRITE_OVERHANG_METRICS overhang{};

		[[nodiscard]] bool Matches(std::string const& candidate_text, std::string const& candidate_face,
								   VideoOverlayTextStyle const& style) const {
			return text == candidate_text && face == candidate_face && size == style.size &&
				   bold == style.bold && italic == style.italic;
		}
	};

	struct Raster {
		int width, height, texture_width, texture_height;
		double left, top;
		std::array<std::vector<unsigned char>, 2> masks;
	};

	struct Mask {
		std::shared_ptr<Layout> layout;
		std::array<GLuint, 2> textures{};
		std::size_t bytes = 0;
		double left = 0.0, top = 0.0, width = 0.0, height = 0.0;
		double texture_right = 0.0, texture_bottom = 0.0;

		~Mask() {
			if (textures[0] || textures[1])
				glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
		}
	};

	ComPtr<IDWriteFactory> write_factory;
	ComPtr<ID2D1Factory> d2d_factory;
	ComPtr<IWICImagingFactory> imaging_factory;
	std::string default_face;
	DWORD const owner_thread = GetCurrentThreadId();
	HGLRC owner_context = nullptr;
	GlCapabilities capabilities;
	double scale_x = 1.0, scale_y = 1.0;
	double cached_scale_x = 1.0, cached_scale_y = 1.0;
	std::list<std::shared_ptr<Layout>> layouts;
	std::list<std::unique_ptr<Mask>> masks;
	std::size_t mask_bytes = 0;

	void ValidateThread() const {
		if (GetCurrentThreadId() != owner_thread)
			throw std::logic_error("Overlay text must be used on its owning thread");
	}

	static void ValidateStyle(VideoOverlayTextStyle const& style) {
		if (style.size <= 0)
			throw std::invalid_argument("Overlay text requires a positive point size");
	}

	std::shared_ptr<Layout> FindLayout(std::string const& text, VideoOverlayTextStyle const& style) {
		auto const& face = style.face.empty() ? default_face : style.face;
		for (auto it = layouts.begin(); it != layouts.end(); ++it) {
			if ((*it)->Matches(text, face, style)) {
				auto result = *it;
				layouts.splice(layouts.begin(), layouts, it);
				return result;
			}
		}
		if (face.find('\0') != std::string::npos)
			throw std::invalid_argument("Overlay text font family contains a null character");
		auto const wide_text = agi::charset::ConvertW(text);
		auto const wide_face = agi::charset::ConvertW(face);
		if (wide_text.size() > std::numeric_limits<UINT32>::max())
			throw std::length_error("Overlay text exceeds the DirectWrite string length limit");
		ComPtr<IDWriteTextFormat> format;
		CheckResult(write_factory->CreateTextFormat(wide_face.c_str(), nullptr,
													style.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
													style.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
													DWRITE_FONT_STRETCH_NORMAL, static_cast<float>(style.size) * 96.0f / 72.0f, L"", &format),
					"create text format");
		CheckResult(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP), "disable text wrapping");
		auto layout = std::make_shared<Layout>();
		layout->text = text;
		layout->face = face;
		layout->size = style.size;
		layout->bold = style.bold;
		layout->italic = style.italic;
		CheckResult(write_factory->CreateTextLayout(wide_text.data(), static_cast<UINT32>(wide_text.size()),
													format.Get(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), &layout->value),
					"create text layout");
		CheckResult(layout->value->GetMetrics(&layout->metrics), "measure text layout");
		layout->extent = {LogicalExtent(layout->metrics.widthIncludingTrailingWhitespace), LogicalExtent(layout->metrics.height)};
		CheckResult(layout->value->SetMaxWidth(std::max(1.0f, layout->metrics.widthIncludingTrailingWhitespace)),
					"set measured layout width");
		CheckResult(layout->value->SetMaxHeight(std::max(1.0f, layout->metrics.height)), "set measured layout height");
		CheckResult(layout->value->GetOverhangMetrics(&layout->overhang), "measure glyph overhang");
		layouts.push_front(layout);
		if (layouts.size() > layout_cache_limit)
			layouts.pop_back();
		return layout;
	}

	void ValidateContext() {
		auto const current = wglGetCurrentContext();
		if (!current)
			throw std::logic_error("Overlay text drawing requires a current OpenGL context");
		if (owner_context && current != owner_context)
			throw std::logic_error("Overlay text cannot draw in a different OpenGL context");
		CheckGl("begin text draw");
		if (!owner_context) {
			capabilities = GlCapabilities::Read();
			owner_context = current;
		}
		if (scale_x != cached_scale_x || scale_y != cached_scale_y) {
			masks.clear();
			mask_bytes = 0;
			cached_scale_x = scale_x;
			cached_scale_y = scale_y;
			CheckGl("retire text masks after DPI change");
		}
	}

	[[nodiscard]] Raster Rasterize(Layout const& layout) const {
		auto const& overhang = layout.overhang;
		double const left = std::floor(std::min(0.0, -static_cast<double>(overhang.left)) * scale_x) - std::ceil(scale_x) - 1.0;
		double const top = std::floor(std::min(0.0, -static_cast<double>(overhang.top)) * scale_y) - std::ceil(scale_y) - 1.0;
		double const right = std::ceil((std::max(1.0f, layout.metrics.widthIncludingTrailingWhitespace) +
										std::max(0.0f, overhang.right)) *
									   scale_x) +
							 std::ceil(scale_x) + 1.0;
		double const bottom = std::ceil((std::max(1.0f, layout.metrics.height) +
										 std::max(0.0f, overhang.bottom)) *
										scale_y) +
							  std::ceil(scale_y) + 1.0;
		double const width = right - left, height = bottom - top;
		if (!std::isfinite(left) || !std::isfinite(top) || !std::isfinite(width) || !std::isfinite(height) ||
			width <= 0.0 || height <= 0.0 || width > capabilities.maximum_texture_size || height > capabilities.maximum_texture_size)
			throw std::length_error("Overlay text raster bounds exceed the OpenGL texture size limit");
		Raster raster;
		raster.width = static_cast<int>(width);
		raster.height = static_cast<int>(height);
		raster.texture_width = TextureExtent(raster.width, capabilities);
		raster.texture_height = TextureExtent(raster.height, capabilities);
		raster.left = left / scale_x;
		raster.top = top / scale_y;
		auto const texture_pixels = static_cast<std::size_t>(raster.texture_width) * raster.texture_height;
		if (texture_pixels > mask_byte_limit / 2)
			throw std::length_error("Overlay text raster exceeds the mask cache byte limit");
		ComPtr<IWICBitmap> bitmap;
		CheckResult(imaging_factory->CreateBitmap(raster.width, raster.height, GUID_WICPixelFormat32bppPBGRA,
												  WICBitmapCacheOnLoad, &bitmap),
					"create raster bitmap");
		auto const properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
															 D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
															 static_cast<float>(96.0 * scale_x), static_cast<float>(96.0 * scale_y));
		ComPtr<ID2D1RenderTarget> target;
		CheckResult(d2d_factory->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &target), "create raster target");
		target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
		ComPtr<ID2D1SolidColorBrush> brush;
		CheckResult(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush), "create text mask brush");
		target->BeginDraw();
		target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
		target->DrawTextLayout(D2D1::Point2F(static_cast<float>(-raster.left), static_cast<float>(-raster.top)),
							   layout.value.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
		CheckResult(target->EndDraw(), "rasterize text mask");

		ComPtr<IWICBitmapLock> bitmap_lock;
		CheckResult(bitmap->Lock(nullptr, WICBitmapLockRead, &bitmap_lock), "read text mask");
		UINT stride = 0, size = 0;
		BYTE *pixels = nullptr;
		CheckResult(bitmap_lock->GetStride(&stride), "read text mask stride");
		CheckResult(bitmap_lock->GetDataPointer(&size, &pixels), "read text mask pixels");
		if (!pixels || stride < static_cast<UINT>(raster.width) * 4 ||
			static_cast<std::size_t>(size) < static_cast<std::size_t>(raster.height - 1) * stride +
												 static_cast<std::size_t>(raster.width) * 4)
			throw std::logic_error("Overlay text raster returned invalid pixel storage");
		raster.masks[0].resize(texture_pixels);
		for (int y = 0; y < raster.height; ++y)
			for (int x = 0; x < raster.width; ++x)
				raster.masks[0][static_cast<std::size_t>(y) * raster.texture_width + x] =
					pixels[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4 + 3];
		std::vector<unsigned char> horizontal(texture_pixels);
		raster.masks[1].resize(texture_pixels);
		ExpandMask(raster.masks[0], horizontal, raster.texture_width, raster.texture_height, scale_x, true);
		ExpandMask(horizontal, raster.masks[1], raster.texture_width, raster.texture_height, scale_y, false);
		return raster;
	}

	Mask& Upload(std::shared_ptr<Layout> layout, Raster const& raster) {
		auto mask = std::make_unique<Mask>();
		mask->layout = std::move(layout);
		mask->bytes = raster.masks[0].size() + raster.masks[1].size();
		mask->left = raster.left;
		mask->top = raster.top;
		mask->width = raster.width / scale_x;
		mask->height = raster.height / scale_y;
		mask->texture_right = static_cast<double>(raster.width) / raster.texture_width;
		mask->texture_bottom = static_cast<double>(raster.height) / raster.texture_height;
		glGenTextures(static_cast<GLsizei>(mask->textures.size()), mask->textures.data());
		CheckGl("allocate text masks");
		for (std::size_t index = 0; index < mask->textures.size(); ++index) {
			glBindTexture(GL_TEXTURE_2D, mask->textures[index]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA8, raster.texture_width, raster.texture_height, 0,
						 GL_ALPHA, GL_UNSIGNED_BYTE, raster.masks[index].data());
			CheckGl("upload text mask");
		}
		while (!masks.empty() && (masks.size() >= mask_cache_limit || mask_bytes > mask_byte_limit - mask->bytes)) {
			mask_bytes -= masks.back()->bytes;
			masks.pop_back();
		}
		masks.push_front(std::move(mask));
		mask_bytes += masks.front()->bytes;
		return *masks.front();
	}

	static void DrawMask(Mask const& mask, GLuint texture, double x, double y) {
		double const left = x + mask.left, top = y + mask.top;
		glBindTexture(GL_TEXTURE_2D, texture);
		glBegin(GL_QUADS);
		glTexCoord2d(0.0, 0.0);
		glVertex2d(left, top);
		glTexCoord2d(mask.texture_right, 0.0);
		glVertex2d(left + mask.width, top);
		glTexCoord2d(mask.texture_right, mask.texture_bottom);
		glVertex2d(left + mask.width, top + mask.height);
		glTexCoord2d(0.0, mask.texture_bottom);
		glVertex2d(left, top + mask.height);
		glEnd();
	}

	public:
	Impl(IDWriteFactory& write_factory, ID2D1Factory& d2d_factory,
		 IWICImagingFactory& imaging_factory, std::string default_face)
		: write_factory(&write_factory), d2d_factory(&d2d_factory),
		  imaging_factory(&imaging_factory), default_face(std::move(default_face)) {
		ULONG_PTR context_token = 0;
		CheckResult(CoGetContextToken(&context_token), "require initialized COM apartment");
		if (this->default_face.empty() || this->default_face.find('\0') != std::string::npos)
			throw std::invalid_argument("Overlay text requires a default font family");
		agi::charset::ConvertW(this->default_face);
	}

	~Impl() {
		if (GetCurrentThreadId() != owner_thread || (!masks.empty() && wglGetCurrentContext() != owner_context)) {
			std::fputs("overlay text: destruction requires the owning thread and OpenGL context\n", stderr);
			std::terminate();
		}
		if (!masks.empty()) {
			masks.clear();
			CheckGlNoexcept("destroy text masks");
		}
	}

	void SetScale(double x, double y) {
		ValidateThread();
		if (!std::isfinite(x) || !std::isfinite(y) || x <= 0.0 || y <= 0.0 ||
			x > std::numeric_limits<float>::max() / 96.0 || y > std::numeric_limits<float>::max() / 96.0)
			throw std::invalid_argument("Overlay text requires finite positive DPI scales");
		scale_x = x;
		scale_y = y;
	}

	VideoOverlayTextExtent MeasureText(std::string const& text, VideoOverlayTextStyle const& style) {
		ValidateThread();
		ValidateStyle(style);
		if (text.empty())
			return {};
		return FindLayout(text, style)->extent;
	}

	void DrawText(std::string const& text, int x, int y, VideoOverlayTextStyle const& style) {
		ValidateThread();
		ValidateStyle(style);
		ValidateContext();
		if (text.empty() || style.colour.a == 255)
			return;
		auto layout = FindLayout(text, style);
		Mask *mask = nullptr;
		for (auto it = masks.begin(); it != masks.end(); ++it) {
			if ((*it)->layout == layout) {
				mask = it->get();
				masks.splice(masks.begin(), masks, it);
				break;
			}
		}
		Raster raster;
		if (!mask)
			raster = Rasterize(*layout);
		TextGlState const restore(capabilities);
		if (!mask)
			mask = &Upload(std::move(layout), raster);
		auto const opacity = static_cast<GLubyte>(255 - style.colour.a);
		if (style.outline) {
			glColor4ub(0, 0, 0, opacity);
			DrawMask(*mask, mask->textures[1], x, y);
		}
		glColor4ub(style.colour.r, style.colour.g, style.colour.b, opacity);
		DrawMask(*mask, mask->textures[0], x, y);
		CheckGl("draw text");
	}
};

OpenGLVideoOverlayText::OpenGLVideoOverlayText(IDWriteFactory& write_factory, ID2D1Factory& d2d_factory,
											   IWICImagingFactory& imaging_factory, std::string default_face)
	: impl(std::make_unique<Impl>(write_factory, d2d_factory, imaging_factory, std::move(default_face))) {}

OpenGLVideoOverlayText::~OpenGLVideoOverlayText() = default;

void OpenGLVideoOverlayText::SetScale(double x, double y) {
	impl->SetScale(x, y);
}

VideoOverlayTextExtent OpenGLVideoOverlayText::MeasureText(std::string const& text, VideoOverlayTextStyle const& style) {
	return impl->MeasureText(text, style);
}

void OpenGLVideoOverlayText::DrawText(std::string const& text, int x, int y, VideoOverlayTextStyle const& style) {
	impl->DrawText(text, x, y, style);
}
