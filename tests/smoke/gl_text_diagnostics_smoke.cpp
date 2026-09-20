#include "../../src/audio_tile_diagnostics_enabled.h"
#include "../../src/compat.h"
#include "../../src/gl_text.h"
#include "../../src/utils.h"

#include <libaegisub/log.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>

#include <wx/init.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <locale>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Narrow harness support avoids linking unrelated GUI configuration helpers.
// The real OpenGLText, GL operations and log sink remain the code under test.
wxString to_wx(std::string const& text) {
	return wxString::FromUTF8(text);
}

int SmallestPowerOf2(int value) {
	return static_cast<int>(std::bit_ceil(static_cast<unsigned>(value)));
}

namespace {
void Require(bool condition, char const *message) {
	if (!condition)
		throw std::runtime_error(message);
}

class HiddenGlWindow final {
	HWND window = nullptr;
	HDC dc = nullptr;
	HGLRC context = nullptr;

	public:
	HiddenGlWindow(HiddenGlWindow const&) = delete;
	HiddenGlWindow& operator=(HiddenGlWindow const&) = delete;

	HiddenGlWindow() {
		WNDCLASSW cls{};
		cls.style = CS_OWNDC;
		cls.lpfnWndProc = DefWindowProcW;
		cls.hInstance = GetModuleHandleW(nullptr);
		cls.lpszClassName = L"AegisubGlTextDiagnosticsSmoke";
		Require(RegisterClassW(&cls) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS,
				"RegisterClassW failed");
		window = CreateWindowExW(0, cls.lpszClassName, L"GL text diagnostics smoke",
								 WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, cls.hInstance, nullptr);
		Require(window != nullptr, "CreateWindowExW failed");
		dc = GetDC(window);
		Require(dc != nullptr, "GetDC failed");
		PIXELFORMATDESCRIPTOR descriptor{};
		descriptor.nSize = sizeof(descriptor);
		descriptor.nVersion = 1;
		descriptor.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
		descriptor.iPixelType = PFD_TYPE_RGBA;
		descriptor.cColorBits = 32;
		descriptor.cAlphaBits = 8;
		auto const format = ChoosePixelFormat(dc, &descriptor);
		Require(format && SetPixelFormat(dc, format, &descriptor), "setting pixel format failed");
		context = wglCreateContext(dc);
		Require(context && MakeCurrent(), "creating WGL context failed");
	}

	~HiddenGlWindow() {
		if (wglGetCurrentContext() == context)
			wglMakeCurrent(nullptr, nullptr);
		if (context)
			wglDeleteContext(context);
		if (dc)
			ReleaseDC(window, dc);
		if (window)
			DestroyWindow(window);
	}

	[[nodiscard]] bool MakeCurrent() const noexcept { return wglMakeCurrent(dc, context) == TRUE; }
	[[nodiscard]] void const *Identity() const noexcept { return context; }
};

std::string PointerText(void const *pointer) {
	std::ostringstream text;
	text.imbue(std::locale::classic());
	text << pointer;
	return text.str();
}

using Fields = std::map<std::string, std::string>;

std::vector<Fields> ReadEvents(agi::log::LogSink const& sink) {
	std::vector<Fields> events;
	for (auto const& entry : sink.GetMessages()) {
		if (std::string_view(entry.section) != "audio/tile-diagnostics/gl-text")
			continue;
		std::istringstream words(entry.message);
		Fields fields;
		for (std::string word; words >> word;) {
			auto const separator = word.find('=');
			Require(separator != std::string::npos, "diagnostic field is not key=value");
			Require(fields.emplace(word.substr(0, separator), word.substr(separator + 1)).second,
					"duplicate diagnostic field");
		}
		events.push_back(std::move(fields));
	}
	return events;
}

GLuint BuildGlyph(OpenGLText& text, char character) {
	int width = 0;
	int height = 0;
	text.GetExtent(std::string(1, character), width, height);
	Require(width > 0 && height > 0, "glyph did not have positive extents");
	text.Print(std::string(1, character), 2, 2);
	GLint texture = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	Require(texture > 0 && glIsTexture(static_cast<GLuint>(texture)), "glyph atlas was not allocated");
	Require(glGetError() == GL_NO_ERROR, "glyph allocation generated a GL error");
	return static_cast<GLuint>(texture);
}

std::vector<unsigned char> MakeCollisionTexture(GLuint texture) {
	constexpr int width = 256;
	constexpr int height = 276;
	std::vector<unsigned char> pixels(width * height * 4);
	for (std::size_t index = 0; index < pixels.size(); index += 4) {
		pixels[index] = static_cast<unsigned char>((index / 4 + texture) % 251);
		pixels[index + 1] = static_cast<unsigned char>((index / 4 / width + 31) % 253);
		pixels[index + 2] = static_cast<unsigned char>((index / 4 * 3 + 17) % 255);
		pixels[index + 3] = 255;
	}
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	Require(glIsTexture(texture) && glGetError() == GL_NO_ERROR,
			"could not allocate the colliding texture in the second context");
	return pixels;
}

void RequireCollisionIntact(GLuint texture, std::vector<unsigned char> const& expected) {
	Require(glIsTexture(texture), "foreign texture was deleted");
	glBindTexture(GL_TEXTURE_2D, texture);
	GLint width = 0;
	GLint height = 0;
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
	Require(width == 256 && height == 276, "foreign texture storage was changed");
	std::vector<unsigned char> actual(expected.size(), 0xa7);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, actual.data());
	Require(glGetError() == GL_NO_ERROR && actual == expected, "foreign texture pixels were changed");
}

void RunMeasurementChecks() {
	HiddenGlWindow owner;
	auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
	OpenGLText text(deleter);
	text.SetFont("Arial", 18, false, false);
	Require(wglMakeCurrent(nullptr, nullptr), "could not detach the native context");
	int width = -1;
	int height = -1;
	text.GetExtent("", width, height);
	Require(width == 0 && height == 0, "empty string extent was not zero");
	text.GetExtent("AV9", width, height);
	Require(width > 0 && height > 0, "measurement without a context failed");
	int const measured_width = width;
	int const measured_height = height;
	Require(!wglGetCurrentContext(), "CPU text measurement activated a context");
	Require(owner.MakeCurrent(), "could not restore the measurement context");
	GLuint sentinel = 0;
	glGenTextures(1, &sentinel);
	auto const sentinel_pixels = MakeCollisionTexture(sentinel);
	glEnable(static_cast<GLenum>(0xffffffffu));
	text.GetExtent("xyz", width, height);
	Require(width > 0 && height > 0, "new glyph measurement failed");
	Require(glGetError() == GL_INVALID_ENUM, "CPU measurement consumed a pending GL error");
	GLint bound_texture = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound_texture);
	Require(std::cmp_equal(bound_texture, sentinel), "CPU measurement changed texture binding");
	RequireCollisionIntact(sentinel, sentinel_pixels);
	glDeleteTextures(1, &sentinel);

	glViewport(0, 0, 64, 64);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, 64, 64, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDrawBuffer(GL_BACK);
	glReadBuffer(GL_BACK);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	text.Print("AV9", 3, 3);
	text.GetExtent("AV9", width, height);
	Require(width == measured_width && height == measured_height, "upload changed measured glyph extents");
	std::vector<unsigned char> pixels(64 * 64 * 4);
	glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	bool saw_text_pixel = false;
	for (std::size_t index = 0; index < pixels.size(); index += 4)
		saw_text_pixel = saw_text_pixel || pixels[index] > 32 || pixels[index + 1] > 32 || pixels[index + 2] > 32;
	Require(saw_text_pixel && glGetError() == GL_NO_ERROR, "lazy text upload did not render glyph pixels");
	text.SetFont("Arial", 20, false, false);
	deleter->Drain();
	std::cout << "TextMeasurementNeedsNoContextAndPreservesGlState passed\n"
			  << "LazyGlyphUploadPreservesExtentsAndRendersPixels passed\n";
}

void RunLifecycleChecks(bool enabled, agi::log::LogSink const& sink) {
	auto const first_event = ReadEvents(sink).size();
	HiddenGlWindow owner;
	auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
	auto text = std::make_unique<OpenGLText>(deleter);
	auto const text_identity = PointerText(text.get());
	auto const owner_identity = PointerText(owner.Identity());
	text->SetFont("Arial", 18, false, false);
	std::map<char, GLuint> glyph_textures;
	std::set<GLuint> initial_textures;
	for (char character = '!'; character <= '~'; ++character) {
		auto const texture = BuildGlyph(*text, character);
		glyph_textures.emplace(character, texture);
		initial_textures.insert(texture);
	}
	Require(initial_textures.size() >= 2, "fixture must grow the texture vector to exercise moves");
	for (auto const& [character, texture] : glyph_textures)
		Require(BuildGlyph(*text, character) == texture, "warm text rendering replaced a glyph atlas");
	std::cout << "WarmPrintReusesGlyphAtlases passed\n";

	HiddenGlWindow other;
	auto const other_identity = PointerText(other.Identity());
	Require(owner.Identity() != other.Identity(), "fixture contexts must be distinct");
	std::map<GLuint, std::vector<unsigned char>> foreign_textures;
	for (auto const texture : initial_textures) {
		Require(!glIsTexture(texture), "fixture contexts unexpectedly share texture objects");
		foreign_textures.emplace(texture, MakeCollisionTexture(texture));
	}
	glEnable(static_cast<GLenum>(0xffffffffu));
	text->SetFont("Arial", 20, false, false);
	Require(glGetError() == GL_INVALID_ENUM, "font reset consumed the pending GL error");
	Require(wglGetCurrentContext() == other.Identity(), "font reset switched the native context");
	for (auto const& [texture, pixels] : foreign_textures)
		RequireCollisionIntact(texture, pixels);
	Require(owner.MakeCurrent(), "could not restore the owning fixture context");
	for (auto const texture : initial_textures)
		Require(glIsTexture(texture), "font reset deleted an atlas before the owner drain");
	glEnable(static_cast<GLenum>(0xffffffffu));
	deleter->Drain();
	Require(glGetError() == GL_INVALID_ENUM, "owner drain consumed the pending GL error");
	Require(wglGetCurrentContext() == owner.Identity(), "owner drain switched the native context");
	for (auto const texture : initial_textures)
		Require(!glIsTexture(texture), "owner drain did not delete the retired atlas");
	// A consumed queue must not later delete a newly reused name in the owner.
	auto const reused_texture = *initial_textures.begin();
	auto const reused_pixels = MakeCollisionTexture(reused_texture);
	deleter->Drain();
	RequireCollisionIntact(reused_texture, reused_pixels);
	glDeleteTextures(1, &reused_texture);

	auto const remaining_texture = BuildGlyph(*text, 'A');
	Require(other.MakeCurrent(), "could not activate the foreign context");
	foreign_textures[remaining_texture] = MakeCollisionTexture(remaining_texture);
	glEnable(static_cast<GLenum>(0xffffffffu));
	text.reset();
	Require(glGetError() == GL_INVALID_ENUM, "text destruction consumed the pending GL error");
	Require(wglGetCurrentContext() == other.Identity(), "text destruction switched the native context");
	for (auto const& [texture, pixels] : foreign_textures)
		RequireCollisionIntact(texture, pixels);
	Require(owner.MakeCurrent(), "could not restore the owning fixture context");
	Require(glIsTexture(remaining_texture), "text destruction deleted an atlas before the owner drain");
	deleter->Drain();
	Require(!glIsTexture(remaining_texture), "owner drain did not delete the destroyed text's atlas");
	Require(other.MakeCurrent(), "could not restore the foreign fixture context");
	for (auto const& [texture, pixels] : foreign_textures) {
		RequireCollisionIntact(texture, pixels);
		glDeleteTextures(1, &texture);
	}
	Require(glGetError() == GL_NO_ERROR, "fixture cleanup generated a GL error");
	std::cout << "CrossContextFontResetPreservesForeignTexture passed\n"
			  << "CrossContextTextDestructionPreservesForeignTexture passed\n"
			  << "OwnerContextDrainsRetiredTextTexturesExactlyOnce passed\n"
			  << "TextureRetirementAndDrainPreservePendingGlErrors passed\n";

	auto const events = ReadEvents(sink);
	if (!enabled) {
		Require(events.empty(), "disabled diagnostics emitted lifecycle events");
		std::cout << "DisabledDiagnosticsPreserveContextOwnership passed\n";
		return;
	}

	std::map<std::string, Fields> creations;
	std::set<std::string> retired;
	std::set<std::string> deleted;
	std::set<GLuint> logged_initial_textures;
	std::size_t destruction_deletions = 0;
	bool saw_font_reset = false;
	bool saw_text_destruction = false;
	for (std::size_t index = first_event; index < events.size(); ++index) {
		auto const& event = events[index];
		Require(event.at("text") == text_identity, "text identity changed across lifecycle events");
		auto const& phase = event.at("phase");
		if (phase == "font_reset") {
			if (std::stoull(event.at("texture_count")) == initial_textures.size()) {
				Require(event.at("source") == "font_reset" && event.at("current") == other_identity,
						"font reset did not identify its source and current context");
				saw_font_reset = true;
			}
			continue;
		}
		if (phase == "text_destruction") {
			Require(event.at("source") == "text_destruction" && event.at("current") == other_identity && event.at("texture_count") == "1", "text destruction metadata is incorrect");
			saw_text_destruction = true;
			continue;
		}
		auto const& serial = event.at("serial");
		Require(std::stoull(serial) != 0, "diagnostic serial must be nonzero");
		Require(event.at("owner") == owner_identity, "texture lost its native owner identity");
		if (phase == "texture_create") {
			Require(creations.emplace(serial, event).second, "diagnostic serial was reused");
			Require(event.at("current") == owner_identity && event.at("mismatch") == "0",
					"creation did not record the native owner context");
			Require(std::stoul(event.at("width")) >= 64 && event.at("width") == event.at("height"),
					"creation did not record the atlas dimensions");
			continue;
		}
		auto const created = creations.find(serial);
		Require(created != creations.end(), "texture move or retirement lost its creation serial");
		for (auto const *field : {"id", "width", "height", "owner"})
			Require(event.at(field) == created->second.at(field), "moved texture metadata changed");
		Require((event.at("source") == "font_reset" && saw_font_reset) || (event.at("source") == "text_destruction" && saw_text_destruction),
				"retired texture did not retain its deletion source");
		if (phase == "texture_retire") {
			Require(retired.insert(serial).second && event.at("current") == other_identity && event.at("mismatch") == "1", "retirement did not report the foreign current context");
			continue;
		}
		Require(phase == "texture_delete" && retired.count(serial) && deleted.insert(serial).second,
				"texture deletion was not preceded by exactly one retirement");
		Require(event.at("current") == owner_identity && event.at("mismatch") == "0",
				"actual texture deletion did not occur in its owner context");
		if (event.at("source") == "font_reset")
			logged_initial_textures.insert(static_cast<GLuint>(std::stoul(event.at("id"))));
		else {
			Require(std::stoul(event.at("id")) == remaining_texture, "destruction deleted the wrong atlas");
			++destruction_deletions;
		}
	}
	Require(logged_initial_textures == initial_textures, "not all moved atlases were attributed to font reset");
	Require(creations.size() == initial_textures.size() + 1 && creations.size() == retired.size() && retired == deleted && destruction_deletions == 1,
			"creation/retirement/deletion event counts do not match actual GL resources");
	std::cout << "MovedTextureSerialsAndDeferredDeletionAttribution passed\n";
}

void RunAbandonmentChecks(bool enabled, agi::log::LogSink const& sink) {
	auto const first_event = ReadEvents(sink).size();
	auto owner = std::make_unique<HiddenGlWindow>();
	auto deleter = std::make_shared<OpenGLTextTextureDeleter>();
	auto text = std::make_unique<OpenGLText>(deleter);
	text->SetFont("Arial", 18, false, false);
	auto const texture = BuildGlyph(*text, 'A');
	auto late_text = std::make_unique<OpenGLText>(deleter);
	late_text->SetFont("Arial", 20, false, false);
	auto const late_texture = BuildGlyph(*late_text, 'C');
	auto const late_text_identity = PointerText(late_text.get());
	std::string late_serial;
	if (enabled) {
		for (auto const& event : ReadEvents(sink)) {
			if (event.at("phase") == "texture_create" && event.at("text") == late_text_identity)
				late_serial = event.at("serial");
		}
		Require(!late_serial.empty(), "late atlas creation was not logged");
	}
	Require(late_texture != texture, "live texts must own distinct atlas objects");
	HiddenGlWindow next_epoch;
	auto const pixels = MakeCollisionTexture(texture);
	auto const late_pixels = MakeCollisionTexture(late_texture);
	text.reset();
	glEnable(static_cast<GLenum>(0xffffffffu));
	deleter->Abandon();
	Require(glGetError() == GL_INVALID_ENUM, "abandonment consumed the pending GL error");
	Require(wglGetCurrentContext() == next_epoch.Identity(), "abandonment changed the current context");
	RequireCollisionIntact(texture, pixels);
	RequireCollisionIntact(late_texture, late_pixels);
	Require(owner->MakeCurrent(), "could not activate the abandoned owner context");
	Require(glIsTexture(texture), "abandonment performed GL deletion");
	Require(glIsTexture(late_texture), "abandonment deleted a live text's atlas");
	Require(next_epoch.MakeCurrent(), "could not restore the replacement context");
	owner.reset();
	glEnable(static_cast<GLenum>(0xffffffffu));
	late_text.reset();
	Require(glGetError() == GL_INVALID_ENUM, "late retirement consumed the pending GL error");
	Require(wglGetCurrentContext() == next_epoch.Identity(), "late retirement changed the replacement context");
	deleter->Drain();
	deleter.reset();
	RequireCollisionIntact(texture, pixels);
	RequireCollisionIntact(late_texture, late_pixels);
	glDeleteTextures(1, &texture);
	glDeleteTextures(1, &late_texture);
	std::cout << "AbandonDropsRetiredNamesAcrossContextEpochs passed\n"
			  << "LateRetirementAfterAbandonDoesNotReachReplacementContext passed\n";

	// An owner destroyed without an explicit drain is also CPU-only. It must
	// not assume that the currently active context still belongs to it.
	HiddenGlWindow final_owner;
	auto final_deleter = std::make_shared<OpenGLTextTextureDeleter>();
	auto final_text = std::make_unique<OpenGLText>(final_deleter);
	final_text->SetFont("Arial", 18, false, false);
	auto const final_texture = BuildGlyph(*final_text, 'B');
	Require(next_epoch.MakeCurrent(), "could not activate the foreign final context");
	auto const final_pixels = MakeCollisionTexture(final_texture);
	// Text keeps its owner alive even if the display-side reference is gone.
	final_deleter.reset();
	glEnable(static_cast<GLenum>(0xffffffffu));
	final_text.reset();
	Require(glGetError() == GL_INVALID_ENUM, "owner destruction consumed the pending GL error");
	Require(wglGetCurrentContext() == next_epoch.Identity(), "owner destruction changed the current context");
	RequireCollisionIntact(final_texture, final_pixels);
	glDeleteTextures(1, &final_texture);
	Require(final_owner.MakeCurrent(), "could not restore the final owner context");
	Require(glIsTexture(final_texture), "owner destruction performed GL deletion");
	glDeleteTextures(1, &final_texture);
	Require(glGetError() == GL_NO_ERROR, "abandonment fixture cleanup failed");
	std::cout << "OwnerDestructionNeverDeletesForeignTextures passed\n";

	auto const events = ReadEvents(sink);
	if (!enabled) {
		Require(events.empty(), "disabled diagnostics emitted abandonment events");
		return;
	}
	std::set<std::string> retired;
	std::set<std::string> abandoned;
	std::size_t late_abandonments = 0;
	for (std::size_t index = first_event; index < events.size(); ++index) {
		auto const& event = events[index];
		auto const& phase = event.at("phase");
		Require(phase != "texture_delete", "abandoned resources logged an actual GL deletion");
		if (phase == "texture_retire")
			Require(retired.insert(event.at("serial")).second, "atlas retired more than once");
		if (phase == "texture_abandon") {
			Require(abandoned.insert(event.at("serial")).second, "atlas abandoned more than once");
			if (event.at("serial") == late_serial) {
				Require(!retired.count(event.at("serial")) && std::stoul(event.at("id")) == late_texture,
						"late atlas was queued for deletion after its owner was abandoned");
				++late_abandonments;
			}
			else
				Require(retired.count(event.at("serial")), "abandonment did not correspond to a retired texture");
			Require(event.at("current") == PointerText(next_epoch.Identity()) && event.at("mismatch") == "1",
					"abandonment did not report its actual foreign current context");
		}
	}
	Require(retired.size() == 2 && abandoned.size() == 3 && late_abandonments == 1,
			"abandonment diagnostics lost retired or live texture ownership");
	for (auto const& serial : retired)
		Require(abandoned.count(serial), "queued atlas was not abandoned");
	std::cout << "AbandonedTextureDiagnosticsNeverReportDeletion passed\n";
}
}

int main(int argc, char **argv) {
	try {
		Require(argc == 2, "expected --enabled or --disabled");
		bool const enabled = std::string_view(argv[1]) == "--enabled";
		Require(enabled || std::string_view(argv[1]) == "--disabled", "unknown test mode");
		Require(_putenv_s("AEGISUB_AUDIO_TILE_DIAGNOSTICS", enabled ? "1" : "") == 0,
				"could not set the diagnostic environment");
		Require(aegisub::AudioTileDiagnosticsEnabled() == enabled, "diagnostic opt-in was not honored");
		wxInitializer wx;
		Require(wx.IsOk(), "wxWidgets initialization failed");
		agi::log::LogSink sink;
		agi::log::log = &sink;
		RunMeasurementChecks();
		RunLifecycleChecks(enabled, sink);
		RunAbandonmentChecks(enabled, sink);
		agi::log::log = nullptr;
		return 0;
	}
	catch (std::exception const& error) {
		agi::log::log = nullptr;
		std::cerr << "gl-text-diagnostics-smoke: " << error.what() << '\n';
		return 1;
	}
}
