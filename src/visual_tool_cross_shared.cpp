#include "visual_tool_cross_shared.h"

#include "video_overlay_draw_context.h"

#include <libaegisub/format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace aegisub::visual_cross {
namespace {
bool IsFinite(Vector2D point) {
	return std::isfinite(point.X()) && std::isfinite(point.Y());
}

bool FitsInteger(double value) {
	return value >= std::numeric_limits<int>::min() && value <= std::numeric_limits<int>::max();
}

bool CanFormat(Vector2D point, bool fractional) {
	return IsFinite(point) && (fractional || (FitsInteger(point.X()) && FitsInteger(point.Y())));
}
}

bool IsValidGeometry(Vector2D pointer, visual_edit::Viewport const& viewport) {
	auto positive = [](Vector2D value) {
		return IsFinite(value) && value.X() > 0 && value.Y() > 0;
	};
	if (!CanFormat(pointer, false) || !IsFinite(viewport.video_position) ||
		!positive(viewport.video_size) || !positive(viewport.script_size) || !positive(viewport.canvas_size))
		return false;
	bool const fractional = viewport.video_size.X() > viewport.script_size.X();
	auto const label = (pointer - viewport.video_position) * viewport.script_size / viewport.video_size;
	auto const mirrored = (2 * viewport.video_position + viewport.video_size - pointer - viewport.video_position) * viewport.script_size / viewport.video_size;
	return CanFormat(label, fractional) && CanFormat(mirrored, fractional);
}

std::string FormatCoordinates(Vector2D point, bool fractional) {
	if (!CanFormat(point, fractional))
		throw std::invalid_argument("Cross coordinates are not representable");
	return fractional ? point.Str() : point.DStr();
}

void DrawOverlay(VideoOverlayDrawContext& target, Vector2D pointer,
				 visual_edit::Viewport const& viewport, bool shift, int font_size) {
	if (!IsValidGeometry(pointer, viewport))
		return;
	target.SetInvert();
	target.SetLineColour(agi::Color{255, 255, 255}, 1, 1);
	float const lines[] = {
		0, pointer.Y(), viewport.canvas_size.X(), pointer.Y(),
		pointer.X(), 0, pointer.X(), viewport.canvas_size.Y()};
	target.DrawLines(2, lines, 4);
	target.ClearInvert();

	auto const label = shift ? 2 * viewport.video_position + viewport.video_size - pointer : pointer;
	auto const text = FormatCoordinates((label - viewport.video_position) * viewport.script_size / viewport.video_size,
										viewport.video_size.X() > viewport.script_size.X());
	VideoOverlayTextStyle style;
	style.face = "Verdana";
	style.size = std::clamp(font_size, 6, 72);
	style.bold = true;
	auto const extent = target.MeasureText(text, style);
	std::int64_t x = static_cast<int>(pointer.X());
	std::int64_t y = static_cast<int>(pointer.Y());
	x += x > viewport.canvas_size.X() / 2 ? -static_cast<std::int64_t>(extent.width) - 4 : 4;
	y += y < viewport.canvas_size.Y() / 2 ? 3 : -static_cast<std::int64_t>(extent.height) - 3;
	if (!FitsInteger(static_cast<double>(x)) || !FitsInteger(static_cast<double>(y)))
		throw std::invalid_argument("Cross label position is not representable");
	target.DrawText(text, static_cast<int>(x), static_cast<int>(y), style);
}

std::optional<std::vector<visual_edit::OverrideEdit>> PlanTranslation(
	visual_edit::Line const& line, Vector2D delta, bool fractional) {
	if (!IsFinite(delta) || !CanFormat(line.position + delta, fractional) ||
		(line.move && (!CanFormat(line.move->start + delta, fractional) || !CanFormat(line.move->end + delta, fractional))) ||
		(line.origin && !CanFormat(*line.origin + delta, fractional)))
		return {};
	using visual_edit::OverrideIntent;
	std::vector<visual_edit::OverrideEdit> writes;
	writes.reserve(line.origin ? 2 : 1);
	if (line.move) {
		auto const& move = *line.move;
		auto const start = FormatCoordinates(move.start + delta, fractional);
		auto const end = FormatCoordinates(move.end + delta, fractional);
		auto value = move.start_ms > 0 || move.end_ms > 0
						 ? agi::format("(%s,%s,%d,%d)", start, end, move.start_ms, move.end_ms)
						 : agi::format("(%s,%s)", start, end);
		writes.push_back({.id = line.id, .tag = "\\move", .value = std::move(value), .intent = OverrideIntent::Translation, .translation = delta});
	}
	else
		writes.push_back({.id = line.id, .tag = "\\pos", .value = "(" + FormatCoordinates(line.position + delta, fractional) + ")", .intent = OverrideIntent::Translation, .translation = delta});
	if (line.origin)
		writes.push_back({.id = line.id, .tag = "\\org", .value = "(" + FormatCoordinates(*line.origin + delta, fractional) + ")", .intent = OverrideIntent::Translation, .translation = delta});
	return writes;
}
}
