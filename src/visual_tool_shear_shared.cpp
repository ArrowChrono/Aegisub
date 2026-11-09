#include "visual_tool_shear_shared.h"

#include "numeric_utils.h"
#include "video_overlay_draw_context.h"
#include "visual_feature.h"

#include <algorithm>
#include <cmath>

namespace aegisub::visual_shear {
namespace {
constexpr float kLever = 80.F;
constexpr float kRailOffset = 100.F;
constexpr float kMargin = 160.F;

bool IsFinite(Vector2D value) {
	return std::isfinite(value.X()) && std::isfinite(value.Y());
}
}

Geometry CalculateGeometry(Vector2D active_canvas_position, Vector2D video_position,
						   Vector2D video_size, Vector2D coefficients) {
	Vector2D const margin(std::min(kMargin, video_size.X() / 2), std::min(kMargin, video_size.Y() / 2));
	auto const center = active_canvas_position.Max(video_position + margin).Min(video_position + video_size - margin);
	Vector2D const minimum = video_position + Vector2D(10, 10);
	Vector2D const maximum = video_position + video_size - Vector2D(10, 10);
	return {
		.center = center,
		.handles = {
			(center + Vector2D(coefficients.X() * kLever, -kRailOffset)).Max(minimum).Min(maximum),
			(center + Vector2D(kRailOffset, coefficients.Y() * kLever)).Max(minimum).Min(maximum),
		},
	};
}

std::optional<Geometry> MakeGeometry(Vector2D active_canvas_position, Vector2D video_position,
									 Vector2D video_size, Vector2D coefficients) {
	if (!IsFinite(active_canvas_position) || !IsFinite(video_position) || !IsFinite(video_size) ||
		!IsFinite(coefficients) || video_size.X() <= 0 || video_size.Y() <= 0)
		return {};
	auto const geometry = CalculateGeometry(active_canvas_position, video_position, video_size, coefficients);
	if (!IsFinite(geometry.center) || !IsFinite(geometry.handles[0]) || !IsFinite(geometry.handles[1]))
		return {};
	return geometry;
}

std::optional<Axis> HitTest(Geometry const& geometry, Vector2D pointer) {
	std::optional<Axis> result;
	for (std::size_t i = 0; i < geometry.handles.size(); ++i) {
		VisualDraggableFeature feature;
		feature.type = i == 0 ? DRAG_BIG_SQUARE : DRAG_BIG_CIRCLE;
		feature.pos = geometry.handles[i];
		if (feature.IsMouseOver(pointer))
			result = i == 0 ? Axis::X : Axis::Y;
	}
	return result;
}

char const *Tag(Axis axis) {
	return axis == Axis::X ? "\\fax" : "\\fay";
}

Axis NudgeAxis(Vector2D direction) {
	return direction.X() != 0 ? Axis::X : Axis::Y;
}

float DragDelta(Axis axis, Vector2D displacement) {
	return (axis == Axis::X ? displacement.X() : displacement.Y()) / kLever;
}

float NudgeDelta(Axis axis, Vector2D direction, float step) {
	return (axis == Axis::X ? direction.X() : direction.Y()) * step;
}

std::optional<std::string> FormatAdjustedValue(float initial, float delta) {
	float const value = initial + delta;
	if (!std::isfinite(initial) || !std::isfinite(delta) || !std::isfinite(value))
		return {};
	return FormatValue(value);
}

std::string FormatValue(float value) {
	return float_to_string(value);
}

void DrawOverlay(VideoOverlayDrawContext& context, OverlayState const& state) {
	VideoOverlayTextStyle style;
	style.size = state.font_size;
	style.colour = state.primary_colour;
	if (!state.diagnostic.empty()) {
		context.DrawText(state.diagnostic, static_cast<int>(state.video_position.X()) + 8, static_cast<int>(state.video_position.Y()) + 8, style);
		return;
	}
	auto const center = state.geometry.center;
	context.SetLineColour(state.secondary_colour, 0.7F, 1);
	context.DrawLine(center + Vector2D(-120, -100), center + Vector2D(120, -100));
	context.DrawLine(center + Vector2D(100, -120), center + Vector2D(100, 120));
	context.DrawLine(center + Vector2D(0, -106), center + Vector2D(0, -94));
	context.DrawLine(center + Vector2D(94, 0), center + Vector2D(106, 0));
	context.SetLineColour(state.secondary_colour, 1.F, 1);
	for (std::size_t i = 0; i < state.geometry.handles.size(); ++i) {
		context.SetFillColour(state.handle_fill_colours[i], 0.3F);
		VisualDraggableFeature feature;
		feature.type = i == 0 ? DRAG_BIG_SQUARE : DRAG_BIG_CIRCLE;
		feature.pos = state.geometry.handles[i];
		feature.Draw(context);
	}
	context.DrawText("fax " + FormatValue(state.coefficients.X()), static_cast<int>(center.X()) - 120, static_cast<int>(center.Y()) - 126, style);
	context.DrawText("fay " + FormatValue(state.coefficients.Y()), static_cast<int>(center.X()) + 110, static_cast<int>(center.Y()) + 24, style);
	context.DrawText(state.hint, static_cast<int>(state.video_position.X()) + 8, static_cast<int>(state.video_position.Y() + state.video_size.Y()) - 24, style);
}
}
