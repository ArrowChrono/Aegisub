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
	GLint texture = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
	Require(texture > 0 && glIsTexture(static_cast<GLuint>(texture)), "glyph atlas was not allocated");
	Require(glGetError() == GL_NO_ERROR, "glyph allocation generated a GL error");
	return static_cast<GLuint>(texture);
}

void RunLifecycleChecks(bool enabled, agi::log::LogSink const& sink) {
	HiddenGlWindow owner;
	auto text = std::make_unique<OpenGLText>();
	auto const text_identity = PointerText(text.get());
	auto const owner_identity = PointerText(owner.Identity());
	text->SetFont("Arial", 18, false, false);
	std::set<GLuint> initial_textures;
	for (char character = '!'; character <= '~'; ++character)
		initial_textures.insert(BuildGlyph(*text, character));
	Require(initial_textures.size() >= 2, "fixture must grow the texture vector to exercise moves");

	// SetFont's original texture deletion neither changes context nor consumes GL errors.
	glEnable(static_cast<GLenum>(0xffffffffu));
	text->SetFont("Arial", 20, false, false);
	Require(glGetError() == GL_INVALID_ENUM, "font-reset diagnostics consumed the pending GL error");
	Require(wglGetCurrentContext() == owner.Identity(), "font reset switched the native context");
	for (auto const texture : initial_textures)
		Require(!glIsTexture(texture), "font reset did not delete the original atlas");

	auto const remaining_texture = BuildGlyph(*text, 'A');
	HiddenGlWindow other;
	auto const other_identity = PointerText(other.Identity());
	Require(owner.Identity() != other.Identity(), "fixture contexts must be distinct");
	Require(!glIsTexture(remaining_texture), "fixture contexts unexpectedly share texture objects");
	// Reuse the numeric name in a separate namespace. This intentionally reproduces
	// legacy deletion on the wrong current context, entirely inside the fixture.
	glBindTexture(GL_TEXTURE_2D, remaining_texture);
	std::uint32_t const pixel = 0xff332211u;
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &pixel);
	Require(glIsTexture(remaining_texture) && glGetError() == GL_NO_ERROR,
			"could not allocate the colliding texture in the second context");
	glEnable(static_cast<GLenum>(0xffffffffu));
	text.reset();
	Require(glGetError() == GL_INVALID_ENUM, "destruction diagnostics consumed the pending GL error");
	Require(wglGetCurrentContext() == other.Identity(), "diagnostics changed the destruction context");
	Require(!glIsTexture(remaining_texture), "diagnostics skipped the original cross-context deletion");
	Require(owner.MakeCurrent(), "could not restore the owning fixture context");
	Require(glIsTexture(remaining_texture), "diagnostics repaired deletion instead of observing it");
	glDeleteTextures(1, &remaining_texture);
	Require(glGetError() == GL_NO_ERROR, "fixture cleanup generated a GL error");

	auto const events = ReadEvents(sink);
	if (!enabled) {
		Require(events.empty(), "disabled diagnostics emitted lifecycle events");
		std::cout << "DisabledDiagnosticsPreserveLegacyDeletion passed\n";
		return;
	}

	std::map<std::string, Fields> creations;
	std::set<GLuint> logged_initial_textures;
	std::size_t font_deletions = 0;
	std::size_t destruction_deletions = 0;
	bool saw_font_reset = false;
	bool saw_text_destruction = false;
	for (auto const& event : events) {
		Require(event.at("text") == text_identity, "text identity changed across lifecycle events");
		auto const& phase = event.at("phase");
		if (phase == "font_reset") {
			if (std::stoull(event.at("texture_count")) == initial_textures.size()) {
				Require(event.at("source") == "font_reset" && event.at("current") == owner_identity,
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
		Require(phase == "texture_delete", "unexpected lifecycle phase");
		auto const created = creations.find(serial);
		Require(created != creations.end(), "texture move or deletion lost its creation serial");
		for (auto const *field : {"id", "width", "height", "owner"})
			Require(event.at(field) == created->second.at(field), "moved texture metadata changed");
		if (event.at("source") == "font_reset") {
			Require(saw_font_reset && event.at("current") == owner_identity && event.at("mismatch") == "0",
					"font-reset deletion was not correctly ordered or attributed");
			logged_initial_textures.insert(static_cast<GLuint>(std::stoul(event.at("id"))));
			++font_deletions;
		}
		else {
			Require(event.at("source") == "text_destruction" && saw_text_destruction && event.at("current") == other_identity && event.at("mismatch") == "1" && std::stoul(event.at("id")) == remaining_texture,
					"cross-context destruction did not report the actual mismatch");
			++destruction_deletions;
		}
	}
	Require(logged_initial_textures == initial_textures && font_deletions == initial_textures.size(),
			"not all moved atlases were attributed to font reset exactly once");
	Require(creations.size() == initial_textures.size() + 1 && destruction_deletions == 1,
			"creation/destruction event counts do not match actual GL resources");
	std::cout << "MovedTextureSerialsAndFontResetAttribution passed\n"
			  << "CrossContextDestructionReportsButDoesNotRepairMismatch passed\n"
			  << "DiagnosticsPreservePendingGlErrors passed\n";
}
}

int main(int argc, char **argv) {
	try {
		Require(argc == 2, "expected --enabled or --disabled");
		bool const enabled = std::string_view(argv[1]) == "--enabled";
		Require(enabled || std::string_view(argv[1]) == "--disabled", "unknown test mode");
		Require(_putenv_s("AEGISUB_AUDIO_TILE_DIAGNOSTICS", enabled ? "1" : "0") == 0,
				"could not set the diagnostic environment");
		Require(aegisub::AudioTileDiagnosticsEnabled() == enabled, "diagnostic opt-in was not honored");
		wxInitializer wx;
		Require(wx.IsOk(), "wxWidgets initialization failed");
		agi::log::LogSink sink;
		agi::log::log = &sink;
		RunLifecycleChecks(enabled, sink);
		agi::log::log = nullptr;
		return 0;
	}
	catch (std::exception const& error) {
		agi::log::log = nullptr;
		std::cerr << "gl-text-diagnostics-smoke: " << error.what() << '\n';
		return 1;
	}
}
