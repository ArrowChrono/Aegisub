#pragma once

#include "presentation_contract.h"

#include "../time_display_mode.h"

#include <libaegisub/color.h>

#include <string>
#include <string_view>

namespace agi::vfr {
class Framerate;
}

namespace aegisub::presentation {

enum class SubtitleGridOverrideMode {
	Show,
	Replace,
	Hide
};

struct SubtitleGridDisplayOptions {
	SubtitleTimeDisplayMode time_display_mode = SubtitleTimeDisplayMode::Ass;
	agi::vfr::Framerate const* timecodes = nullptr;
	bool ignore_whitespace = false;
	bool ignore_punctuation = false;
	bool show_decimal_cps = false;
	SubtitleGridOverrideMode override_mode = SubtitleGridOverrideMode::Show;
	std::string override_replacement;
};

std::string FormatSubtitleGridCell(
	SubtitleGridRow const& row,
	std::string_view column_id,
	SubtitleGridDisplayOptions const& options = {});

std::string FormatSubtitleGridText(
	std::string const& text,
	SubtitleGridOverrideMode mode,
	std::string const& replacement);

double CalculateSubtitleGridCps(
	SubtitleGridRow const& row,
	SubtitleGridDisplayOptions const& options);

std::string FormatSubtitleGridCps(double cps, bool show_decimal);

struct SubtitleGridCpsColors {
	agi::Color background;
	agi::Color text;
	bool highlighted = false;
};

SubtitleGridCpsColors ResolveSubtitleGridCpsColors(
	double cps,
	int warning_threshold,
	int error_threshold,
	agi::Color error_color,
	agi::Color row_background,
	agi::Color text_color);
}
