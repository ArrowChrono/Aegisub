#include "subtitle_grid_display.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/character_count.h>
#include <libaegisub/vfr.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

namespace aegisub::presentation {
namespace {

agi::Color blend(agi::Color fg, agi::Color bg, double alpha) {
	auto channel = [](unsigned char foreground, unsigned char background, double ratio) -> unsigned char {
		double value = static_cast<double>(background) + (static_cast<double>(foreground) - static_cast<double>(background)) * ratio;
		return value < 0.0 ? 0 : (value > 255.0 ? 255 : static_cast<unsigned char>(value));
	};
	return agi::Color(channel(fg.r, bg.r, alpha), channel(fg.g, bg.g, alpha), channel(fg.b, bg.b, alpha), bg.a);
}

// Empty cell for 0 (layer 0 / margin default-from-style). Non-zero values,
// including negatives, are shown.
std::string optional_int(int value) {
	return value != 0 ? std::to_string(value) : std::string();
}

std::string format_time(
	SubtitleGridRow const& row,
	bool start,
	SubtitleGridDisplayOptions const& options) {
	auto const start_time = agi::Time(row.start_ms);
	auto const end_time = agi::Time(row.end_ms);

	if (options.time_display_mode == SubtitleTimeDisplayMode::Frame) {
		if (!options.timecodes)
			return std::string();

		return std::to_string(options.timecodes->FrameAtTime(
			start ? row.start_ms : row.end_ms,
			start ? agi::vfr::START : agi::vfr::END));
	}

	auto const displayed = GetDialogueTimesForDisplay(
		start_time,
		end_time,
		options.time_display_mode,
		options.timecodes);
	return FormatTimeForDisplay(start ? displayed.first : displayed.second, options.time_display_mode);
}

}

std::string FormatSubtitleGridCell(
	SubtitleGridRow const& row,
	std::string_view column_id,
	SubtitleGridDisplayOptions const& options) {
	if (column_id == SubtitleGridColumnIdLineNumber)
		return std::to_string(row.row_index + 1);
	if (column_id == SubtitleGridColumnIdLayer)
		return optional_int(row.layer);
	if (column_id == SubtitleGridColumnIdStart)
		return format_time(row, true, options);
	if (column_id == SubtitleGridColumnIdEnd)
		return format_time(row, false, options);
	if (column_id == SubtitleGridColumnIdStyle)
		return row.style;
	if (column_id == SubtitleGridColumnIdActor)
		return row.actor;
	if (column_id == SubtitleGridColumnIdEffect)
		return row.effect;
	if (column_id == SubtitleGridColumnIdMarginLeft)
		return optional_int(row.margins[0]);
	if (column_id == SubtitleGridColumnIdMarginRight)
		return optional_int(row.margins[1]);
	if (column_id == SubtitleGridColumnIdMarginVertical)
		return optional_int(row.margins[2]);
	if (column_id == SubtitleGridColumnIdCps)
		return FormatSubtitleGridCps(CalculateSubtitleGridCps(row, options), options.show_decimal_cps);
	if (column_id == SubtitleGridColumnIdText)
		return FormatSubtitleGridText(row.text, options.override_mode, options.override_replacement);

	return std::string();
}

std::string FormatSubtitleGridText(
	std::string const& text,
	SubtitleGridOverrideMode mode,
	std::string const& replacement) {
	if (mode == SubtitleGridOverrideMode::Show)
		return text;

	std::string result;
	result.reserve(text.size());

	size_t start = 0;
	while (true) {
		auto pos = text.find('{', start);
		if (pos == std::string::npos)
			break;

		result.append(text, start, pos - start);
		if (mode == SubtitleGridOverrideMode::Replace)
			result += replacement;

		start = text.find('}', pos);
		if (start == std::string::npos)
			break;
		++start;
	}

	if (start != std::string::npos)
		result.append(text, start, std::string::npos);

	return result;
}

double CalculateSubtitleGridCps(
	SubtitleGridRow const& row,
	SubtitleGridDisplayOptions const& options) {
	auto const duration_mode = options.time_display_mode == SubtitleTimeDisplayMode::Ass
		? SubtitleTimeDisplayMode::Ass
		: SubtitleTimeDisplayMode::Exact;
	int duration = GetDurationForDisplay(
		agi::Time(row.start_ms),
		agi::Time(row.end_ms),
		duration_mode,
		options.timecodes);

	if (duration <= 100 || row.text.size() > static_cast<size_t>(duration))
		return -1;

	int ignore = agi::IGNORE_BLOCKS;
	if (options.ignore_whitespace)
		ignore |= agi::IGNORE_WHITESPACE;
	if (options.ignore_punctuation)
		ignore |= agi::IGNORE_PUNCTUATION;

	auto const characters = agi::RenderedTextCharacterCount(row.text, ignore);
	if (options.show_decimal_cps)
		return characters * 1000.0 / duration;
	return static_cast<double>(characters * 1000 / duration);
}

std::string FormatSubtitleGridCps(double cps, bool show_decimal) {
	if (cps < 0 || cps > 100)
		return std::string();

	if (!show_decimal)
		return std::to_string(static_cast<int>(cps));

	char buffer[32];
	std::snprintf(buffer, sizeof buffer, "%.1f", cps);
	return buffer;
}

SubtitleGridCpsColors ResolveSubtitleGridCpsColors(
	double cps,
	int warning_threshold,
	int error_threshold,
	agi::Color error_color,
	agi::Color row_background,
	agi::Color text_color) {
	SubtitleGridCpsColors colors{.background = row_background, .text = text_color};
	if (cps < 0 || cps > 100 || !(cps > warning_threshold))
		return colors;

	int maximum = std::max(warning_threshold, error_threshold);
	double alpha = std::min((cps - warning_threshold + 1) / (maximum - warning_threshold + 1), 1.0);
	colors.background = blend(error_color, row_background, alpha);
	colors.text = blend(agi::Color(0, 0, 0), text_color, alpha);
	colors.highlighted = true;
	return colors;
}
}
