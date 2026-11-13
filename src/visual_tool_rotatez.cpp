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

/// @file visual_tool_rotatez.cpp
/// @brief 2D rotation in Z axis visual typesetting tool
/// @ingroup visual_ts

#include "visual_tool_rotatez.h"

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
#include "visual_tool_transform_policy.h"
#include "visual_tool_transform_overlay.h"

#include <libaegisub/format.h>

#include <wx/colour.h>

namespace {
using RotateZState = aegisub::visual_transform::RotateZOverlayState;

void DrawRotateZGeometry(OpenGLWrapper& gl, RotateZState const& state) {
	OpenGLVideoOverlayDrawContext context(gl);
	aegisub::visual_transform::DrawRotateZOverlay(context, state);
}

void DrawRotateZMouseGuide(OpenGLWrapper& gl, Vector2D origin, Vector2D mouse_pos, agi::Color const& line_colour) {
	if (mouse_pos && (mouse_pos - origin).SquareLen() > 100) {
		gl.SetLineColour(line_colour);
		gl.DrawLine(origin, mouse_pos);
	}
}

class RotateZRenderSnapshot final : public VisualToolRenderSnapshot {
	RotateZState state;

public:
	RotateZRenderSnapshot(std::shared_ptr<const VisualToolRenderContext> context, RotateZState const& state)
		: VisualToolRenderSnapshot(std::move(context)), state(state) {}

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
		aegisub::visual_transform::DrawRotateZOverlay(context, state);
	}

	void DrawLiveFeedback(Vector2D const& mouse_pos) const override {
		OpenGLWrapper gl;
		DrawRotateZMouseGuide(gl, state.origin, mouse_pos, state.secondary_colour);
	}

	[[nodiscard]] bool HasMouseDrivenFeedback() const override { return true; }
};
}

VisualToolRotateZ::VisualToolRotateZ(VideoDisplay *parent, agi::Context *context)
: VisualTool<VisualDraggableFeature>(parent, context)
, org(new Feature)
{
	features.push_back(*org);
	org->type = DRAG_BIG_TRIANGLE;
}

void VisualToolRotateZ::Draw() {
	if (!active_line) return;

	DrawAllFeatures();
	RotateZState const state{
		.origin = org->pos,
		.pos = pos,
		.scale = scale,
		.rotation_x = rotation_x,
		.rotation_y = rotation_y,
		.angle = angle,
		.perspective_z_scale = video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(script_res, layout_res),
		.primary_colour = line_color_primary_opt->GetColor(),
		.secondary_colour = line_color_secondary_opt->GetColor(),
		.highlight_colour = highlight_color_primary_opt->GetColor(),
		.feature_fill_colour = {},
	};
	DrawRotateZGeometry(gl, state);
	DrawRotateZMouseGuide(gl, state.origin, mouse_pos, state.secondary_colour);
}

std::shared_ptr<const VisualToolRenderSnapshot> VisualToolRotateZ::CaptureRenderSnapshot(
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
	RotateZState const state{
		.origin = org->pos,
		.pos = pos,
		.scale = scale,
		.rotation_x = rotation_x,
		.rotation_y = rotation_y,
		.angle = angle,
		.perspective_z_scale = video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(script_res, layout_res),
		.primary_colour = VideoOverlayColourFromRgb(primary),
		.secondary_colour = VideoOverlayColourFromRgb(secondary),
		.highlight_colour = VideoOverlayColourFromRgb(highlight),
		.feature_fill_colour = VideoOverlayColourFromRgb(feature_fill),
	};
	return std::make_shared<RotateZRenderSnapshot>(context, state);
}

void VisualToolRotateZ::DrawOverlay(VideoOverlayDrawContext &context) {
	if (!active_line) return;
	DrawAllFeatures(context);
	aegisub::visual_transform::DrawRotateZOverlay(context, {
															   .origin = org->pos,
															   .pos = pos,
															   .scale = scale,
															   .rotation_x = rotation_x,
															   .rotation_y = rotation_y,
															   .angle = angle,
															   .perspective_z_scale = video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(script_res, layout_res),
															   .primary_colour = line_color_primary_opt->GetColor(),
															   .secondary_colour = line_color_secondary_opt->GetColor(),
															   .highlight_colour = highlight_color_primary_opt->GetColor(),
														   });
	if (mouse_pos && (mouse_pos - org->pos).SquareLen() > 100) {
		context.SetLineColour(from_wx(to_wx(line_color_secondary_opt->GetColor())), 1.0f, 1);
		context.SetFillColour(from_wx(to_wx(line_color_secondary_opt->GetColor())), 0.0f);
		context.DrawLine(org->pos, mouse_pos);
	}
}

bool VisualToolRotateZ::InitializeHold() {
	orig_angle = aegisub::visual_transform::RotationAnchor(angle, org->pos, mouse_pos);
	return true;
}

void VisualToolRotateZ::UpdateHold() {
	angle = aegisub::visual_transform::RotateZ(orig_angle, org->pos, mouse_pos, ctrl_down);

	QueueSelectedGeometryOverride("\\frz", agi::format("%.4g", angle));
}

void VisualToolRotateZ::UpdateDrag(Feature *feature) {
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

void VisualToolRotateZ::DoRefresh() {
	if (!active_line) return;

	auto const line = aegisub::visual_edit::ReadLine(*active_line, c->GetCore().ass->GetStyle(active_line->Style), script_res);
	pos = FromScriptCoords(line.position);
	org->pos = FromScriptCoords(line.origin.value_or(line.position));
	rotation_x = line.rotation_x;
	rotation_y = line.rotation_y;
	angle = line.rotation_z;
	scale = line.scale;
}

bool VisualToolRotateZ::Nudge(Vector2D direction, VisualNudgeMagnitude magnitude) {
	BeginGeometryGesture();
	if (!active_line)
		return false;

	bool changed = false;

	if (direction.X() != 0.f) {
		float const step = static_cast<float>(GetNudgeStep(
			"Tool/Visual/Nudge/Rotate Step",
			"Tool/Visual/Nudge/Rotate Step Large",
			magnitude));
		auto next = aegisub::visual_transform::NudgeRotation(angle, direction.X() * step);
		if (!next)
			return false;
		angle = *next;
		QueueSelectedGeometryOverride("\\frz", agi::format("%.4g", angle));
		changed = true;
	}

	if (direction.Y() != 0.f) {
		float const step = static_cast<float>(GetNudgeStep(
			"Tool/Visual/Nudge/Origin Step",
			"Tool/Visual/Nudge/Origin Step Large",
			magnitude));
		// Up = negative script Y (same coordinate system as mouse drag of \\org)
		Vector2D const delta(0.f, direction.Y() * step);

		auto core = c->GetCore();
		for (auto line : core.selectionController->GetSelectedSet()) {
			auto line_org = GetLineOrigin(line);
			if (!line_org)
				line_org = GetLinePosition(line);
			QueueGeometryOverride(line, "\\org", (line_org + delta).PStr());
		}
		changed = true;
	}

	if (!changed)
		return false;

	if (!FinishGeometryEdit()) {
		DoRefresh();
		ReportGeometryError();
		return false;
	}
	DoRefresh();
	CommitNudge();
	return true;
}
