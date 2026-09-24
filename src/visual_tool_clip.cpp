// Copyright (c) 2011, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

/// @file visual_tool_clip.cpp
/// @brief Rectangular clipping visual typesetting tool
/// @ingroup visual_ts

#include "visual_tool_clip.h"

#include "ass_dialogue.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "selection_controller.h"
#include "video_overlay_draw_context.h"
#include "visual_tool_render_snapshot.h"

#include <libaegisub/format.h>

#include <array>
#include <wx/colour.h>

namespace {

void DrawClipRectangles(VideoOverlayDrawContext& context, Vector2D cur_1, Vector2D cur_2,
						Vector2D video_pos, Vector2D video_res, bool inverse, wxColour const& line_color, float shaded_alpha) {
	context.SetLineColour(line_color, 1.0f, 2);
	context.SetFillColour(line_color, 0.0f);
	context.DrawRectangle(cur_1, cur_2);

	context.SetLineColour(line_color, 0.0f);
	context.SetFillColour(*wxBLACK, shaded_alpha);
	if (inverse) {
		context.DrawRectangle(cur_1, cur_2);
	}
	else {
		Vector2D const v_min = video_pos;
		Vector2D const v_max = video_pos + video_res;
		Vector2D const c_min = cur_1.Min(cur_2);
		Vector2D const c_max = cur_1.Max(cur_2);
		context.DrawRectangle(v_min, Vector2D(v_max, c_min));
		context.DrawRectangle(Vector2D(v_min, c_max), v_max);
		context.DrawRectangle(Vector2D(v_min, c_min), Vector2D(c_min, c_max));
		context.DrawRectangle(Vector2D(c_max, c_min), Vector2D(v_max, c_max));
	}
}

class ClipRenderSnapshot final : public VisualToolRenderSnapshot {
	public:
	struct Marker {
		Vector2D pos;
		unsigned long fill_colour;
	};

	std::array<Marker, 4> markers;
	Vector2D cur_1;
	Vector2D cur_2;
	Vector2D video_pos;
	Vector2D video_res;
	unsigned long grid_colour;
	unsigned long line_colour;
	float shaded_alpha;
	bool inverse;

	using VisualToolRenderSnapshot::VisualToolRenderSnapshot;

	[[nodiscard]] std::optional<VisualToolFeatureKey> HitTest(Vector2D const& mouse_pos) const override {
		std::optional<VisualToolFeatureKey> hit;
		for (std::size_t i = 0; i < markers.size(); ++i) {
			VisualDraggableFeature marker;
			marker.type = DRAG_SMALL_CIRCLE;
			marker.pos = markers[i].pos;
			if (marker.IsMouseOver(mouse_pos))
				hit = VisualToolFeatureKey{.type = DRAG_SMALL_CIRCLE, .index = i};
		}
		return hit;
	}

	void Draw(VideoOverlayDrawContext& context) const override {
		context.SetLineColour(wxColour(grid_colour), 1.0f, 1);
		for (auto const& marker : markers) {
			context.SetFillColour(wxColour(marker.fill_colour), 0.3f);
			VisualDraggableFeature feature;
			feature.type = DRAG_SMALL_CIRCLE;
			feature.pos = marker.pos;
			feature.Draw(context);
		}
		DrawClipRectangles(context, cur_1, cur_2, video_pos, video_res, inverse,
						   wxColour(line_colour), shaded_alpha);
	}
};

}

VisualToolClip::VisualToolClip(VideoDisplay *parent, agi::Context *context)
: VisualTool<ClipCorner>(parent, context)
, cur_1(0, 0)
, cur_2(video_res)
{
	ClipCorner *feats[4];
	for (auto& feat : feats) {
		feat = new ClipCorner;
		features.push_back(*feat);
	}

	// Attach each feature to the two features it shares edges with
	// Top-left
	int i = 0;
	feats[i]->horiz = feats[1];
	feats[i]->vert = feats[2];
	i++;

	// Top-right
	feats[i]->horiz = feats[0];
	feats[i]->vert = feats[3];
	i++;

	// Bottom-left
	feats[i]->horiz = feats[3];
	feats[i]->vert = feats[0];
	i++;

	// Bottom-right
	feats[i]->horiz = feats[2];
	feats[i]->vert = feats[1];
}

void VisualToolClip::Draw() {
	if (!active_line) return;

	DrawAllFeatures();

	// Load colors from options
	wxColour line_color = to_wx(line_color_primary_opt->GetColor());
	float shaded_alpha = static_cast<float>(shaded_area_alpha_opt->GetDouble());

	// Draw rectangle
	gl.SetLineColour(line_color, 1.0f, 2);
	gl.SetFillColour(line_color, 0.0f);
	gl.DrawRectangle(cur_1, cur_2);

	// Draw outside area
	gl.SetLineColour(line_color, 0.0f);
	gl.SetFillColour(*wxBLACK, shaded_alpha);
	if (inverse) {
		gl.DrawRectangle(cur_1, cur_2);
	}
	else {
		Vector2D v_min = video_pos;
		Vector2D v_max = video_pos + video_res;
		Vector2D c_min = cur_1.Min(cur_2);
		Vector2D c_max = cur_1.Max(cur_2);
		gl.DrawRectangle(v_min,                  Vector2D(v_max, c_min));
		gl.DrawRectangle(Vector2D(v_min, c_max), v_max);
		gl.DrawRectangle(Vector2D(v_min, c_min), Vector2D(c_min, c_max));
		gl.DrawRectangle(Vector2D(c_max, c_min), Vector2D(v_max, c_max));
	}
}

void VisualToolClip::DrawOverlay(VideoOverlayDrawContext &context) {
	if (!active_line) return;

	DrawAllFeatures(context);

	wxColour const line_color = to_wx(line_color_primary_opt->GetColor());
	float const shaded_alpha = static_cast<float>(shaded_area_alpha_opt->GetDouble());
	DrawClipRectangles(context, cur_1, cur_2, video_pos, video_res, inverse, line_color, shaded_alpha);
}

std::shared_ptr<const VisualToolRenderSnapshot> VisualToolClip::CaptureRenderSnapshot(
	std::shared_ptr<const VisualToolRenderContext> const& context) const {
	if (!active_line)
		return {};

	auto snapshot = std::make_shared<ClipRenderSnapshot>(context);
	snapshot->cur_1 = cur_1;
	snapshot->cur_2 = cur_2;
	snapshot->video_pos = video_pos;
	snapshot->video_res = video_res;
	snapshot->inverse = inverse;
	snapshot->shaded_alpha = static_cast<float>(shaded_area_alpha_opt->GetDouble());
	snapshot->grid_colour = to_wx(line_color_secondary_opt->GetColor()).GetRGB();
	snapshot->line_colour = to_wx(line_color_primary_opt->GetColor()).GetRGB();
	auto const base_fill = to_wx(highlight_color_primary_opt->GetColor()).GetRGB();
	auto const active_fill = to_wx(highlight_color_secondary_opt->GetColor()).GetRGB();
	for (size_t i = 0; auto const& feature : features) {
		auto const fill = &feature == active_feature                            ? active_fill
						  : sel_features.count(const_cast<Feature *>(&feature)) ? snapshot->line_colour
																				: base_fill;
		snapshot->markers[i++] = {.pos = feature.pos, .fill_colour = fill};
	}
	return snapshot;
}

bool VisualToolClip::InitializeHold() {
	return true;
}

void VisualToolClip::UpdateHold() {
	// Limit to video area
	cur_1 = video_pos.Max((video_pos + video_res).Min(drag_start));
	cur_2 = video_pos.Max((video_pos + video_res).Min(mouse_pos));

	SetFeaturePositions();
	CommitHold();
}

void VisualToolClip::CommitHold() {
	std::string value = agi::format("(%s,%s)", ToScriptCoords(cur_1.Min(cur_2)).Str(), ToScriptCoords(cur_1.Max(cur_2)).Str());

	auto core = c->GetCore();
	for (auto line : core.selectionController->GetSelectedSet()) {
		// This check is technically not correct as it could be outside of an
		// override block... but that's rather unlikely
		bool has_iclip = line->Text.get().find("\\iclip") != std::string::npos;
		SetOverride(line, has_iclip ? "\\iclip" : "\\clip", value);
	}
}

void VisualToolClip::UpdateDrag(ClipCorner *feature) {
	// Update features which share an edge with the dragged one
	feature->horiz->pos = Vector2D(feature->horiz->pos, feature->pos);
	feature->vert->pos = Vector2D(feature->pos, feature->vert->pos);

	cur_1 = features.front().pos;
	cur_2 = features.back().pos;

	CommitHold();
}

void VisualToolClip::SetFeaturePositions() {
	auto it = features.begin();
	(it++)->pos = cur_1; // Top-left
	(it++)->pos = Vector2D(cur_2, cur_1); // Top-right
	(it++)->pos = Vector2D(cur_1, cur_2); // Bottom-left
	it->pos = cur_2; // Bottom-right
}

void VisualToolClip::DoRefresh() {
	if (active_line) {
		GetLineClip(active_line, cur_1, cur_2, inverse);
		cur_1 = FromScriptCoords(cur_1);
		cur_2 = FromScriptCoords(cur_2);
		SetFeaturePositions();
	}
}
