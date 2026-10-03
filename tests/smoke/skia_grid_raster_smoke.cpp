#include "grid_column_painter.h"
#include "skia_grid_renderer.h"

#include <libaegisub/color.h>

#include <wx/app.h>
#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/font.h>
#include <wx/init.h>
#include <wx/rawbmp.h>
#include <wx/region.h>

#include <iostream>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

// GTK font/bitmap APIs need a real GUI app, not wxInitializer's console fallback.
#ifdef __WXGTK__
wxIMPLEMENT_APP_NO_MAIN(wxApp);
#endif

namespace {

struct Rgb {
	unsigned char red = 0;
	unsigned char green = 0;
	unsigned char blue = 0;
};

Rgb ReadPixel(wxBitmap& bitmap, int x, int y) {
	wxAlphaPixelData data(bitmap);
	if (!data)
		throw std::runtime_error("failed to map smoke output bitmap");
	wxAlphaPixelData::Iterator pixel(data);
	pixel.MoveTo(data, x, y);
	return {pixel.Red(), pixel.Green(), pixel.Blue()};
}

bool Near(Rgb actual, Rgb expected, int tolerance = 3) {
	auto close = [tolerance](unsigned char lhs, unsigned char rhs) {
		return std::abs(static_cast<int>(lhs) - static_cast<int>(rhs)) <= tolerance;
	};
	return close(actual.red, expected.red)
		&& close(actual.green, expected.green)
		&& close(actual.blue, expected.blue);
}

int CountNonWhitePixels(wxBitmap& bitmap, wxRect const& area) {
	wxAlphaPixelData data(bitmap);
	if (!data)
		throw std::runtime_error("failed to map smoke text area");
	int count = 0;
	for (int y = area.y; y < area.y + area.height; ++y) {
		wxAlphaPixelData::Iterator pixel(data);
		pixel.MoveTo(data, area.x, y);
		for (int x = 0; x < area.width; ++x, ++pixel) {
			if (pixel.Red() < 245 || pixel.Green() < 245 || pixel.Blue() < 245)
				++count;
		}
	}
	return count;
}

bool RegionHasOnlyGrayscaleCoverage(wxBitmap& bitmap, wxRect const& area) {
	wxAlphaPixelData data(bitmap);
	if (!data)
		throw std::runtime_error("failed to map smoke grayscale area");
	for (int y = area.y; y < area.y + area.height; ++y) {
		wxAlphaPixelData::Iterator pixel(data);
		pixel.MoveTo(data, area.x, y);
		for (int x = 0; x < area.width; ++x, ++pixel) {
			if (std::abs(static_cast<int>(pixel.Red()) - static_cast<int>(pixel.Green())) > 1
				|| std::abs(static_cast<int>(pixel.Green()) - static_cast<int>(pixel.Blue())) > 1)
				return false;
		}
	}
	return true;
}

void Require(bool condition, char const *message) {
	if (!condition)
		throw std::runtime_error(message);
}

} // namespace

int main() {
	try {
		wxInitializer wx;
		Require(wx.IsOk(), "wxWidgets initialization failed");

		constexpr int width = 220;
		constexpr int height = 90;
		wxBitmap output(width, height, 32);
		Require(output.IsOk(), "failed to allocate smoke output bitmap");
		wxMemoryDC target(output);
		Require(target.IsOk(), "failed to select smoke output bitmap");

		wxFont font(wxFontInfo(12).Family(wxFONTFAMILY_SWISS));
		aegisub::grid::SkiaGridRenderer renderer;
		aegisub::grid::SkiaGridPresentationContext presentation;
#ifdef _WIN32
	presentation.monitor_token = reinterpret_cast<std::uintptr_t>(
		MonitorFromPoint(POINT {0, 0}, MONITOR_DEFAULTTOPRIMARY));
#endif
		std::string error;
		wxRegion full(0, 0, width, height);
		auto painted = renderer.Paint(
			target, wxSize(width, height), full, presentation, font,
			[](GridColumnPainter& painter) {
				painter.Clear(agi::Color(255, 255, 255));
				painter.FillRectangle(4, 4, 20, 10, agi::Color(220, 30, 40));
				painter.DrawLine(30, 2, 30, 25, agi::Color(20, 70, 220));
				painter.StrokeRectangle(36, 3, 28, 18, agi::Color(20, 150, 70));
				painter.SetTextColor(agi::Color(0, 0, 0));
				painter.DrawText(L"DirectWrite: Latin \u4e2d\u6587 \u0645\u0631\u062d\u0628\u0627", 8, 34);
			}, 0.0, error);
		Require(painted, error.c_str());
		target.SelectObject(wxNullBitmap);

		Require(Near(ReadPixel(output, 1, 1), {255, 255, 255}),
			"opaque clear color mismatch");
		Require(Near(ReadPixel(output, 8, 8), {220, 30, 40}),
			"filled rectangle mismatch");
		Require(Near(ReadPixel(output, 30, 8), {20, 70, 220}),
			"vertical 1px rule mismatch");
		Require(Near(ReadPixel(output, 36, 3), {20, 150, 70}),
			"outline rectangle mismatch");
		Require(CountNonWhitePixels(output, wxRect(8, 34, 200, 30)) > 30,
			"DirectWrite produced no visible multilingual glyph pixels");
		auto const initial_text_stats = renderer.Stats().last_frame.text_cache;
#ifdef _WIN32
		Require(initial_text_stats.raster_policy_known,
			"DirectWrite did not report a resolved raster policy");
		Require(!initial_text_stats.clear_type_active,
			"default DirectWrite text policy unexpectedly enabled ClearType");
		Require(RegionHasOnlyGrayscaleCoverage(output, wxRect(8, 34, 200, 30)),
			"default DirectWrite grayscale text contained color-subpixel fringes");
#endif

		// ClearType remains an explicit comparison option, but the backend may
		// activate it only when every local display/session condition is safe.
		presentation.clear_type_requested = true;
		output.ResetAlpha();
		target.SelectObject(output);
		error.clear();
		painted = renderer.Paint(
			target, wxSize(width, height), full, presentation, font,
			[](GridColumnPainter& painter) {
				painter.Clear(agi::Color(255, 255, 255));
				painter.FillRectangle(4, 4, 20, 10, agi::Color(220, 30, 40));
				painter.DrawLine(30, 2, 30, 25, agi::Color(20, 70, 220));
				painter.StrokeRectangle(36, 3, 28, 18, agi::Color(20, 150, 70));
				painter.SetTextColor(agi::Color(0, 0, 0));
				painter.DrawText(L"Opt-in DirectWrite Latin \u4e2d\u6587", 8, 34);
			}, 0.0, error);
		Require(painted, error.c_str());
		target.SelectObject(wxNullBitmap);
		auto const opt_in_text_stats = renderer.Stats().last_frame.text_cache;
#ifdef _WIN32
		if (opt_in_text_stats.clear_type_active) {
			Require(!RegionHasOnlyGrayscaleCoverage(output, wxRect(8, 34, 200, 30)),
				"eligible DirectWrite ClearType text had no subpixel coverage");
		}
		else {
			Require(RegionHasOnlyGrayscaleCoverage(output, wxRect(8, 34, 200, 30)),
				"guarded ClearType opt-in contained color-subpixel fringes");
		}
#endif

		renderer.InvalidateRasterPolicy();
		wxRegion partial(0, 0, 3, 3);
		error.clear();
		// Raw readback marks a 32bpp wxBitmap as alpha-bearing. The production
		// paint target is opaque, so restore the same contract before reselecting
		// this test bitmap into a memory DC.
		output.ResetAlpha();
		target.SelectObject(output);
		painted = renderer.Paint(
			 target, wxSize(width, height), partial, presentation, font,
			[](GridColumnPainter& painter) {
				painter.Clear(agi::Color(20, 180, 120));
				painter.SetTextColor(agi::Color(0, 0, 0));
				painter.DrawText(L"clip clip clip", 0, 0);
			}, 0.0, error);
		Require(painted, error.c_str());
		target.SelectObject(wxNullBitmap);
		Require(Near(ReadPixel(output, 1, 1), {20, 180, 120}),
			"partial dirty clear did not update its clipped pixels");
		Require(Near(ReadPixel(output, 8, 8), {220, 30, 40}),
			"partial dirty frame changed retained pixels outside the clip");

		auto const misses_before_font_change =
			renderer.Stats().last_frame.text_cache.layout_misses;
		renderer.InvalidateSystemFonts();
		wxRect const text_rect(8, 34, 200, 30);
		wxRegion text_region(text_rect);
		output.ResetAlpha();
		target.SelectObject(output);
		error.clear();
		painted = renderer.Paint(
			target, wxSize(width, height), text_region, presentation, font,
			[](GridColumnPainter& painter) {
				painter.Clear(agi::Color(255, 255, 255));
				painter.SetTextColor(agi::Color(0, 0, 0));
				painter.DrawText(L"DirectWrite: Latin \u4e2d\u6587 \u0645\u0631\u062d\u0628\u0627", 8, 34);
			}, 0.0, error);
		Require(painted, error.c_str());
		target.SelectObject(wxNullBitmap);
		Require(CountNonWhitePixels(output, text_rect) > 30,
			"font invalidation repaint produced no text");
		Require(renderer.Stats().last_frame.text_cache.clear_type_active
			== opt_in_text_stats.clear_type_active,
			"font invalidation changed the DirectWrite raster policy");

		// A non-1:1 presentation must deterministically suppress subpixel text,
		// even on a local ClearType-enabled desktop.
		presentation.one_to_one_present = false;
		output.ResetAlpha();
		target.SelectObject(output);
		error.clear();
		painted = renderer.Paint(
			target, wxSize(width, height), full, presentation, font,
			[](GridColumnPainter& painter) {
				painter.Clear(agi::Color(255, 255, 255));
				painter.SetTextColor(agi::Color(0, 0, 0));
				painter.DrawText(L"Forced grayscale Latin \u4e2d\u6587", 8, 34);
			}, 0.0, error);
		Require(painted, error.c_str());
		target.SelectObject(wxNullBitmap);
#ifdef _WIN32
		Require(!renderer.Stats().last_frame.text_cache.clear_type_active,
			"non-1:1 presentation did not force DirectWrite grayscale");
		Require(RegionHasOnlyGrayscaleCoverage(output, text_rect),
			"forced DirectWrite grayscale contained subpixel color fringes");
#endif

		auto const stats = renderer.Stats();
		Require(stats.surface_allocations == 1,
			"stable-size repaint unexpectedly reallocated the raster target");
		Require(stats.frames_presented == 5,
			"not every smoke frame was presented");
#ifdef _WIN32
		Require(stats.copied_bytes == 0,
			"Windows shared DirectWrite/Skia DIB unexpectedly copied pixels");
#else
		Require(stats.copied_bytes
			== static_cast<unsigned long long>(
				3 * width * height * 4 + 3 * 3 * 4
				+ text_rect.width * text_rect.height * 4),
			"dirty-region copied-byte accounting mismatch");
#endif
		Require(stats.last_frame.valid
			&& stats.last_frame.dirty_rectangles == 1
			&& stats.last_frame.dirty_pixels == width * height
#ifdef _WIN32
			&& stats.last_frame.copied_bytes == 0,
#else
			&& stats.last_frame.copied_bytes
				== static_cast<unsigned long long>(width * height * 4),
#endif
			"last-frame dirty-region instrumentation mismatch");
		Require(stats.last_frame.text_cache.layout_entries > 0,
			"DirectWrite layout cache instrumentation reported no retained entries");
		Require(stats.last_frame.text_cache.layout_misses > misses_before_font_change,
			"font invalidation reused a stale DirectWrite layout");

		std::cout << "skia grid raster smoke passed\n";
		return 0;
	}
	catch (std::exception const& error) {
		std::cerr << "skia grid raster smoke failed: " << error.what() << '\n';
		return 1;
	}
}
