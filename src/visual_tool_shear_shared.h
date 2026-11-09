#pragma once

#include "vector2d.h"

#include <libaegisub/color.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

class VideoOverlayDrawContext;

namespace aegisub::visual_shear {

enum class Axis : std::uint8_t { X,
								 Y };

struct Geometry {
	Vector2D center;
	std::array<Vector2D, 2> handles;
};

struct OverlayState {
	Geometry geometry;
	Vector2D coefficients;
	Vector2D video_position;
	Vector2D video_size;
	agi::Color primary_colour;
	agi::Color secondary_colour;
	std::array<agi::Color, 2> handle_fill_colours;
	int font_size = 12;
	std::string hint;
	std::string diagnostic;
};

[[nodiscard]] Geometry CalculateGeometry(Vector2D active_canvas_position, Vector2D video_position,
										 Vector2D video_size, Vector2D coefficients);
[[nodiscard]] std::optional<Geometry> MakeGeometry(Vector2D active_canvas_position, Vector2D video_position,
												   Vector2D video_size, Vector2D coefficients);
[[nodiscard]] std::optional<Axis> HitTest(Geometry const& geometry, Vector2D pointer);
[[nodiscard]] char const *Tag(Axis axis);
[[nodiscard]] Axis NudgeAxis(Vector2D direction);
[[nodiscard]] float DragDelta(Axis axis, Vector2D displacement);
[[nodiscard]] float NudgeDelta(Axis axis, Vector2D direction, float step);
[[nodiscard]] std::optional<std::string> FormatAdjustedValue(float initial, float delta);
[[nodiscard]] std::string FormatValue(float value);
void DrawOverlay(VideoOverlayDrawContext& context, OverlayState const& state);

}
