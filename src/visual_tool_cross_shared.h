#pragma once

#include "visual_edit_controller.h"

class VideoOverlayDrawContext;

namespace aegisub::visual_cross {

[[nodiscard]] bool IsValidGeometry(Vector2D pointer, visual_edit::Viewport const& viewport);
[[nodiscard]] std::string FormatCoordinates(Vector2D point, bool fractional);
void DrawOverlay(VideoOverlayDrawContext& target, Vector2D pointer,
				 visual_edit::Viewport const& viewport, bool shift, int font_size);
[[nodiscard]] std::optional<std::vector<visual_edit::OverrideEdit>> PlanTranslation(
	visual_edit::Line const& line, Vector2D delta, bool fractional);

}
