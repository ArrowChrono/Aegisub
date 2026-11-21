#pragma once

#include "subtitle_edit_ops.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct FontGeometry {
	double scale_x = AssStyle::DefaultScale;
	double scale_y = AssStyle::DefaultScale;
	double spacing = AssStyle::DefaultSpacing;
};

enum class FontOverrideField : std::uint8_t {
	Face,
	Size,
	Weight,
	Italic,
	Underline,
	ScaleX,
	ScaleY,
	Spacing,
	Count,
};
constexpr auto FontOverrideIndex(FontOverrideField field) { return static_cast<std::size_t>(field); }

using FontOverrideValues =
	std::array<std::optional<std::string>, FontOverrideIndex(FontOverrideField::Count)>;

struct FontOverrideResult {
	std::string text;
	int selection_start;
	int selection_end;
};

struct FontOverrideEdit {
	struct Placement {
		std::size_t begin = 0;
		std::size_t end = 0;
		bool replaces_argument = false;
		bool inherits_animation = false;
	};

	// Read existing font properties through this block at the write location.
	int read_block = -1;
	FontGeometry geometry;
	bool create_block = false;
	std::array<Placement, FontOverrideIndex(FontOverrideField::Count)> placements;

	[[nodiscard]] bool InheritsAnimation(FontOverrideField field) const;
	[[nodiscard]] FontOverrideResult Apply(std::string_view original, FontOverrideValues const& values,
										   int selection_start, int selection_end) const;
};

FontGeometry ReadFontGeometry(aegisub::subtitle_edit_ops::ParsedLine const& line, int block,
							  AssStyle const& event_style, aegisub::subtitle_edit_ops::ResetStyleResolver const& resolve_reset);

FontOverrideEdit CaptureFontOverrideEdit(aegisub::subtitle_edit_ops::ParsedLine const& line,
										 int normalized_position, int raw_position, AssStyle const& event_style,
										 aegisub::subtitle_edit_ops::ResetStyleResolver const& resolve_reset);

bool ShouldWriteFontFace(
	std::string_view stored_name,
	std::string_view selected_name,
	bool face_modified);
