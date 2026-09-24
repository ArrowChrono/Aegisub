#include "visual_tool_drag_snapshot.h"

#include "video_overlay_draw_context.h"
#include "video_overlay_helpers.h"

#include <limits>

std::optional<VisualToolFeatureKey> VisualToolDragSnapshot::HitTest(Vector2D const& mouse_pos) const {
	std::optional<VisualToolFeatureKey> hit;
	int layer = std::numeric_limits<int>::min();
	for (auto const& feature : features) {
		VisualDraggableFeature marker;
		marker.type = feature.type;
		marker.pos = feature.pos;
		if (feature.layer >= layer && marker.IsMouseOver(mouse_pos)) {
			hit = VisualToolFeatureKey{.line_id = feature.line_id, .type = feature.type};
			layer = feature.layer;
		}
	}
	return hit;
}

void VisualToolDragSnapshot::Draw(VideoOverlayDrawContext& target) const {
	target.SetLineColour(wxColour(grid_colour), 1.0f, 1);
	for (auto const& feature : features) {
		target.SetFillColour(wxColour(feature.fill_colour), 0.3f);
		VisualDraggableFeature marker;
		marker.type = feature.type;
		marker.pos = feature.pos;
		marker.Draw(target);
	}
	for (auto const& feature : features) {
		if (!feature.parent || feature.type == DRAG_BIG_SQUARE)
			continue;
		bool const has_arrow = feature.type == DRAG_BIG_CIRCLE;
		int const arrow_len = has_arrow ? 10 : 0;
		Vector2D direction = feature.pos - *feature.parent;
		if (direction.SquareLen() < (20 + arrow_len) * (20 + arrow_len))
			continue;
		direction = direction.Unit();
		Vector2D const start = *feature.parent + direction * 10;
		Vector2D const end = feature.pos - direction * (10 + arrow_len);
		if (has_arrow) {
			target.SetLineColour(wxColour(line_colour), 0.8f, 2);
			target.DrawLine(start, end);
			Vector2D const half_base = Vector2D(-direction.Y(), direction.X()) * 4;
			target.DrawTriangle(end + direction * arrow_len, end + half_base, end - half_base);
		}
		else {
			target.SetLineColour(wxColour(line_colour), 0.5f, 2);
			video_overlay_helpers::DrawDashedLine(target, start, end, 6);
		}
	}
}
