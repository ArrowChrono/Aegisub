#pragma once

#include "visual_feature.h"
#include "visual_tool_render_snapshot.h"

#include <optional>
#include <vector>

class VisualToolDragSnapshot final : public VisualToolRenderSnapshot {
	public:
	struct Feature {
		DraggableFeatureType type;
		Vector2D pos;
		std::optional<Vector2D> parent;
		unsigned long fill_colour;
		int line_id = 0;
		int layer = 0;
	};
	std::vector<Feature> features;
	unsigned long grid_colour = 0;
	unsigned long line_colour = 0;

	using VisualToolRenderSnapshot::VisualToolRenderSnapshot;

	void Draw(VideoOverlayDrawContext& target) const override;
	[[nodiscard]] std::optional<VisualToolFeatureKey> HitTest(Vector2D const& mouse_pos) const override;
};
