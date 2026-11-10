#pragma once

#include "spline.h"

#include <libaegisub/color.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class VideoOverlayDrawContext;

namespace aegisub::visual_vector_clip {

enum class Mode : std::uint8_t {
	Drag,
	Line,
	Bicubic,
	Convert,
	Insert,
	Remove,
	Freehand,
	FreehandSmooth,
	Move
};

constexpr int kModeCount = 9;

struct Feature {
	std::size_t curve_index = 0;
	int point = 0;
	Vector2D position;
	bool square = false;
	bool selected = false;
	bool active = false;
};

struct ClosestPoint {
	std::size_t curve_index = 0;
	float parameter = 0;
	Vector2D position;
	std::optional<spline_detail::ClosingSegment> closing;
};

struct OverlayState {
	Spline const& spline;
	std::span<Feature const> features;
	Mode mode = Mode::Drag;
	bool inverse = false;
	Vector2D video_position;
	Vector2D video_size;
	Vector2D pointer;
	bool pointer_valid = false;
	bool holding = false;
	Vector2D drag_start;
	agi::Color line_colour;
	agi::Color highlight_primary;
	agi::Color highlight_secondary;
	float shaded_alpha = 0.5F;
};

void AppendFeatures(Spline const& spline, std::size_t curve_index, std::vector<Feature>& features);
[[nodiscard]] std::vector<Feature> MakeFeatures(Spline const& spline);
[[nodiscard]] std::optional<std::size_t> HitTest(std::span<Feature const> features, Vector2D pointer);
[[nodiscard]] bool InBox(Vector2D first, Vector2D second, Vector2D point);
[[nodiscard]] std::optional<ClosestPoint> FindClosest(Spline const& spline, Vector2D pointer);
[[nodiscard]] std::string BuildClipValue(Spline const& spline);
[[nodiscard]] bool IsEditableDrawing(std::string_view drawing, int scale);
[[nodiscard]] bool IsEncodable(Spline const& spline);
bool RemovePoint(Spline& spline, std::size_t curve_index, int point);
bool Convert(Spline& spline, ClosestPoint const& closest);
bool Insert(Spline& spline, ClosestPoint const& closest);
void Append(Spline& spline, Mode mode, Vector2D pointer);
void UpdateAppend(Spline& spline, Mode mode, Vector2D pointer);
void BeginFreehand(Spline& spline, Vector2D pointer);
bool UpdateFreehand(Spline& spline, Mode mode, Vector2D pointer, bool final);
void DrawOverlay(VideoOverlayDrawContext& context, OverlayState const& state);

}
