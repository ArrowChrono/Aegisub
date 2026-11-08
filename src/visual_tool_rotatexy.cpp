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

/// @file visual_tool_rotatexy.cpp
/// @brief 3D rotation in X/Y axes visual typesetting tool
/// @ingroup visual_ts

#include "visual_tool_rotatexy.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "selection_controller.h"
#include "video_overlay_draw_context.h"
#include "video_overlay_draw_context_opengl.h"
#include "video_overlay_helpers.h"
#include "visual_ass_edit.h"
#include "visual_tool_render_snapshot.h"
#include "visual_tool_transform_overlay.h"
#include "visual_tool_transform_policy.h"

#include <libaegisub/format.h>

#include <wx/colour.h>

namespace {
using RotateXYState = aegisub::visual_transform::RotateXYOverlayState;

void DrawRotateXYGeometry(OpenGLWrapper& gl, RotateXYState const& state, VideoOverlayPreparedPerspectiveBatches const& prepared) {
	OpenGLVideoOverlayDrawContext context(gl);
	aegisub::visual_transform::DrawRotateXYOverlay(context, state, prepared);
}

class RotateXYRenderSnapshot final : public VisualToolRenderSnapshot {
	RotateXYState state;
	std::shared_ptr<VideoOverlayPreparedPerspectiveBatches const> prepared;

	public:
	RotateXYRenderSnapshot(std::shared_ptr<const VisualToolRenderContext> context, RotateXYState const& state,
						   std::shared_ptr<VideoOverlayPreparedPerspectiveBatches const> prepared)
		: VisualToolRenderSnapshot(std::move(context)), state(state), prepared(std::move(prepared)) {}

	[[nodiscard]] std::optional<VisualToolFeatureKey> HitTest(Vector2D const& mouse_pos) const override {
		VisualDraggableFeature marker;
		marker.type = DRAG_BIG_TRIANGLE;
		marker.pos = state.origin;
		if (marker.IsMouseOver(mouse_pos))
			return VisualToolFeatureKey{.type = DRAG_BIG_TRIANGLE};
		return {};
	}

	void Draw(VideoOverlayDrawContext& context) const override {
		context.SetLineColour(state.secondary_colour, 1.f, 1);
		context.SetFillColour(state.feature_fill_colour, 0.3f);
		VisualDraggableFeature feature;
		feature.type = DRAG_BIG_TRIANGLE;
		feature.pos = state.origin;
		feature.Draw(context);
		aegisub::visual_transform::DrawRotateXYOverlay(context, state, *prepared);
	}
};
}

VisualToolRotateXY::VisualToolRotateXY(VideoDisplay *parent, agi::Context *context)
: VisualTool<VisualDraggableFeature>(parent, context)
{
	org = new Feature;
	org->type = DRAG_BIG_TRIANGLE;
	features.push_back(*org);
}

void VisualToolRotateXY::Draw() {
	if (!active_line) return;

	DrawAllFeatures();
	RotateXYState const state{
		.origin = org->pos,
		.angle_x = angle_x,
		.angle_y = angle_y,
		.angle_z = angle_z,
		.fax = fax,
		.fay = fay,
		.perspective_z_scale = video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(script_res, layout_res),
		.primary_colour = line_color_primary_opt->GetColor(),
		.secondary_colour = line_color_secondary_opt->GetColor(),
		.feature_fill_colour = {},
	};
	DrawRotateXYGeometry(gl, state, *overlay_geometry.Get(state.secondary_colour));
}

std::shared_ptr<const VisualToolRenderSnapshot> VisualToolRotateXY::CaptureRenderSnapshot(
	std::shared_ptr<const VisualToolRenderContext> const& context) const {
	if (!active_line)
		return {};
	unsigned long const primary = to_wx(line_color_primary_opt->GetColor()).GetRGB();
	unsigned long const secondary = to_wx(line_color_secondary_opt->GetColor()).GetRGB();
	unsigned long const highlight = to_wx(highlight_color_primary_opt->GetColor()).GetRGB();
	unsigned long feature_fill = highlight;
	if (org == active_feature)
		feature_fill = to_wx(highlight_color_secondary_opt->GetColor()).GetRGB();
	else if (sel_features.count(org))
		feature_fill = primary;
	RotateXYState const state{
		.origin = org->pos,
		.angle_x = angle_x,
		.angle_y = angle_y,
		.angle_z = angle_z,
		.fax = fax,
		.fay = fay,
		.perspective_z_scale = video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(script_res, layout_res),
		.primary_colour = VideoOverlayColourFromRgb(primary),
		.secondary_colour = VideoOverlayColourFromRgb(secondary),
		.feature_fill_colour = VideoOverlayColourFromRgb(feature_fill),
	};
	return std::make_shared<RotateXYRenderSnapshot>(context, state, overlay_geometry.Get(state.secondary_colour));
}

void VisualToolRotateXY::DrawOverlay(VideoOverlayDrawContext& context) {
	if (!active_line) return;
	DrawAllFeatures(context);
	auto const secondary = line_color_secondary_opt->GetColor();
	aegisub::visual_transform::DrawRotateXYOverlay(context, {
																.origin = org->pos,
																.angle_x = angle_x,
																.angle_y = angle_y,
																.angle_z = angle_z,
																.fax = fax,
																.fay = fay,
																.perspective_z_scale = video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(script_res, layout_res),
																.primary_colour = line_color_primary_opt->GetColor(),
																.secondary_colour = secondary,
																.feature_fill_colour = {},
															},
												   *overlay_geometry.Get(secondary));
}

bool VisualToolRotateXY::InitializeHold() {
	SetGeometryEditKind(GeometryEditKind::Perspective);
	orig_x = angle_x;
	orig_y = angle_y;

	return true;
}

void VisualToolRotateXY::UpdateHold() {
	auto const angles = aegisub::visual_transform::RotateXY({orig_x, orig_y}, mouse_pos - drag_start, shift_down, ctrl_down);
	if (!angles)
		return;
	angle_x = angles->X();
	angle_y = angles->Y();

	QueueSelectedGeometryOverride("\\frx", agi::format("%.4g", angle_x));
	QueueSelectedGeometryOverride("\\fry", agi::format("%.4g", angle_y));
}

void VisualToolRotateXY::UpdateDrag(Feature *feature) {
	auto org = GetLineOrigin(GeometrySource(active_line));
	if (!org)
		org = GetLinePosition(GeometrySource(active_line));
	auto d = ToScriptCoords(feature->pos) - org;

	auto core = c->GetCore();
	for (auto line : core.selectionController->GetSelectedSet()) {
		org = GetLineOrigin(GeometrySource(line));
		if (!org)
			org = GetLinePosition(GeometrySource(line));
		QueueGeometryOverride(line, "\\org", (d + org).PStr());
	}
}

void VisualToolRotateXY::DoRefresh() {
	if (!active_line) return;

	auto const line = aegisub::visual_edit::ReadLine(*active_line, c->GetCore().ass->GetStyle(active_line->Style), script_res);
	org->pos = FromScriptCoords(line.origin.value_or(line.position));
	angle_x = line.rotation_x;
	angle_y = line.rotation_y;
	angle_z = line.rotation_z;
	fax = line.fax;
	fay = line.fay;
}

bool VisualToolRotateXY::Nudge(Vector2D direction, VisualNudgeMagnitude magnitude) {
	BeginGeometryGesture();
	SetGeometryEditKind(GeometryEditKind::Perspective);
	if (!active_line)
		return false;

	float const step = static_cast<float>(GetNudgeStep(
		"Tool/Visual/Nudge/Rotate Step",
		"Tool/Visual/Nudge/Rotate Step Large",
		magnitude));

	// Same polarity as UpdateHold: right → +\fry, up → +\frx
	if (direction.X() != 0.f) {
		angle_y = fmodf(angle_y + direction.X() * step + 360.f, 360.f);
		QueueSelectedGeometryOverride("\\fry", agi::format("%.4g", angle_y));
	}
	if (direction.Y() != 0.f) {
		// direction.Y is -1 for Up; UpdateHold uses angle_x = orig_x - delta.Y with
		// screen-Y-down, so Up decreases screen Y → increases frx when mirrored as -direction.Y.
		angle_x = fmodf(angle_x - direction.Y() * step + 360.f, 360.f);
		QueueSelectedGeometryOverride("\\frx", agi::format("%.4g", angle_x));
	}

	if (!FinishGeometryEdit()) {
		DoRefresh();
		ReportGeometryError();
		return false;
	}
	DoRefresh();
	CommitNudge();
	return true;
}
