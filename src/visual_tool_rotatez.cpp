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

#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "selection_controller.h"
#include "video_overlay_draw_context.h"
#include "video_overlay_helpers.h"
#include "visual_tool_render_snapshot.h"

#include <libaegisub/format.h>

#include <cmath>
#include <wx/colour.h>

static const float deg2rad = 3.1415926536f / 180.f;
static const float rad2deg = 180.f / 3.1415926536f;

namespace {
void DrawProjectedAnnulus(VideoOverlayDrawContext &context, Vector2D origin, Vector2D scale, float rx, float ry, float outer_radius, float inner_radius, float start_deg, float end_deg, float perspective_z_scale) {
	bool const full_circle = std::abs(end_deg - start_deg) < 0.001f;
	float arc_start = start_deg * deg2rad;
	float arc_end = full_circle ? (start_deg + 360.0f) * deg2rad : end_deg * deg2rad;
	if (arc_end <= arc_start)
		arc_end += 2.0f * 3.1415926536f;

	int const steps = std::max(24, static_cast<int>(outer_radius * (arc_end - arc_start) / 12.0f));
	std::vector<Vector2D> outer_points;
	std::vector<Vector2D> inner_points;
	outer_points.reserve(steps + 1);
	inner_points.reserve(steps + 1);
	for (int i = 0; i <= steps; ++i) {
		float const t = static_cast<float>(i) / steps;
		float const angle = arc_start + (arc_end - arc_start) * t;
		Vector2D const dir = Vector2D::FromAngle(angle);
		outer_points.push_back(video_overlay_helpers::ProjectScaledRotatedPoint(dir * outer_radius, origin, scale, rx, ry, 0.0f, perspective_z_scale));
		inner_points.push_back(video_overlay_helpers::ProjectScaledRotatedPoint(dir * inner_radius, origin, scale, rx, ry, 0.0f, perspective_z_scale));
	}

	for (int i = 0; i < steps; ++i) {
		Vector2D const quad[] = {
			outer_points[i],
			outer_points[i + 1],
			inner_points[i + 1],
			inner_points[i],
		};
		context.DrawPolygon(quad, 4);
	}
}

struct RotateZState {
	Vector2D origin;
	Vector2D pos;
	Vector2D scale;
	float rotation_x;
	float rotation_y;
	float angle;
	float perspective_z_scale;
	unsigned long primary_colour;
	unsigned long secondary_colour;
	unsigned long highlight_colour;
	unsigned long feature_fill_colour;
};

void DrawRotateZGeometry(OpenGLWrapper& gl, RotateZState const& state) {
	float radius = (state.pos - state.origin).Len();
	float const original_radius = radius;
	if (radius < 50)
		radius = 50;

	gl.SetOrigin(state.origin);
	gl.SetRotation(state.rotation_x, state.rotation_y, 0, state.perspective_z_scale);
	gl.SetScale(state.scale);

	gl.SetLineColour(wxColour(state.secondary_colour));
	gl.SetFillColour(wxColour(state.highlight_colour), 0.3f);
	gl.DrawRing(Vector2D(0, 0), radius + 4, radius - 4);

	int const markers = 6;
	float const mark_start = -90.f / markers;
	float const mark_end = mark_start + (180.f / markers);
	for (int i = 0; i < markers; ++i) {
		float const marker_angle = i * (360.f / markers);
		gl.DrawRing(Vector2D(0, 0), radius + 30, radius + 12, 1.0, marker_angle + mark_start, marker_angle + mark_end);
	}

	Vector2D const angle_vec(Vector2D::FromAngle(state.angle * deg2rad));
	gl.SetLineColour(wxColour(state.primary_colour), 1, 2);
	gl.DrawLine(angle_vec * -radius, angle_vec * radius);

	if (state.origin != state.pos) {
		Vector2D const rotated_pos = Vector2D::FromAngle(state.angle * deg2rad - (state.pos - state.origin).Angle()) * original_radius;
		gl.DrawLine(Vector2D(), rotated_pos);
		gl.DrawLine(rotated_pos - angle_vec * 20, rotated_pos + angle_vec * 20);
	}

	gl.SetLineColour(wxColour(state.secondary_colour), 1.f, 1);
	gl.SetFillColour(wxColour(state.highlight_colour), 0.3f);
	gl.DrawCircle(angle_vec * radius, 4);
	gl.DrawCircle(angle_vec * -radius, 4);
	gl.ResetTransform();
}

void DrawRotateZMouseGuide(OpenGLWrapper& gl, Vector2D origin, Vector2D mouse_pos, unsigned long line_colour) {
	if (mouse_pos && (mouse_pos - origin).SquareLen() > 100) {
		gl.SetLineColour(wxColour(line_colour));
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

	void Draw(VideoOverlayDrawContext&) const override {
		OpenGLWrapper gl;
		gl.SetLineColour(wxColour(state.secondary_colour), 1.f, 1);
		gl.SetFillColour(wxColour(state.feature_fill_colour), 0.3f);
		VisualDraggableFeature feature;
		feature.type = DRAG_BIG_TRIANGLE;
		feature.pos = state.origin;
		feature.Draw(gl);
		DrawRotateZGeometry(gl, state);
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
		.primary_colour = to_wx(line_color_primary_opt->GetColor()).GetRGB(),
		.secondary_colour = to_wx(line_color_secondary_opt->GetColor()).GetRGB(),
		.highlight_colour = to_wx(highlight_color_primary_opt->GetColor()).GetRGB(),
		.feature_fill_colour = 0,
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
		.primary_colour = primary,
		.secondary_colour = secondary,
		.highlight_colour = highlight,
		.feature_fill_colour = feature_fill,
	};
	return std::make_shared<RotateZRenderSnapshot>(context, state);
}

void VisualToolRotateZ::DrawOverlay(VideoOverlayDrawContext &context) {
	if (!active_line) return;

	DrawAllFeatures(context);

	wxColour const line_color_primary = to_wx(line_color_primary_opt->GetColor());
	wxColour const line_color_secondary = to_wx(line_color_secondary_opt->GetColor());
	wxColour const highlight_color = to_wx(highlight_color_primary_opt->GetColor());
	float const perspective_z_scale = video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(script_res, layout_res);

	float radius = (pos - org->pos).Len();
	float const original_radius = radius;
	if (radius < 50)
		radius = 50;

	context.SetLineColour(line_color_secondary, 1.0f, 1);
	context.SetFillColour(highlight_color, 0.3f);
	DrawProjectedAnnulus(context, org->pos, scale, rotation_x, rotation_y, radius + 4, radius - 4, 0.0f, 0.0f, perspective_z_scale);

	int const markers = 6;
	float const mark_start = -90.0f / markers;
	float const mark_end = mark_start + (180.0f / markers);
	for (int i = 0; i < markers; ++i) {
		float const marker_angle = i * (360.0f / markers);
		DrawProjectedAnnulus(context, org->pos, scale, rotation_x, rotation_y, radius + 30, radius + 12, marker_angle + mark_start, marker_angle + mark_end, perspective_z_scale);
	}

	Vector2D const angle_vec = Vector2D::FromAngle(angle * deg2rad);
	context.SetLineColour(line_color_primary, 1.0f, 2);
	context.SetFillColour(line_color_primary, 0.0f);
	video_overlay_helpers::DrawProjectedLine(context, angle_vec * -radius, angle_vec * radius, org->pos, scale, rotation_x, rotation_y, 0.0f, perspective_z_scale);

	if (org->pos != pos) {
		Vector2D const rotated_pos = Vector2D::FromAngle(angle * deg2rad - (pos - org->pos).Angle()) * original_radius;
		video_overlay_helpers::DrawProjectedLine(context, Vector2D(), rotated_pos, org->pos, scale, rotation_x, rotation_y, 0.0f, perspective_z_scale);
		video_overlay_helpers::DrawProjectedLine(context, rotated_pos - angle_vec * 20, rotated_pos + angle_vec * 20, org->pos, scale, rotation_x, rotation_y, 0.0f, perspective_z_scale);
	}

	context.SetLineColour(line_color_secondary, 1.0f, 1);
	context.SetFillColour(highlight_color, 0.3f);
	context.DrawCircle(
		video_overlay_helpers::ProjectScaledRotatedPoint(angle_vec * radius, org->pos, scale, rotation_x, rotation_y, 0.0f, perspective_z_scale),
		video_overlay_helpers::ProjectCircleRadius(angle_vec * radius, 4.0f, org->pos, scale, rotation_x, rotation_y, 0.0f, perspective_z_scale));
	context.DrawCircle(
		video_overlay_helpers::ProjectScaledRotatedPoint(angle_vec * -radius, org->pos, scale, rotation_x, rotation_y, 0.0f, perspective_z_scale),
		video_overlay_helpers::ProjectCircleRadius(angle_vec * -radius, 4.0f, org->pos, scale, rotation_x, rotation_y, 0.0f, perspective_z_scale));

	if (mouse_pos && (mouse_pos - org->pos).SquareLen() > 100) {
		context.SetLineColour(line_color_secondary, 1.0f, 1);
		context.SetFillColour(line_color_secondary, 0.0f);
		context.DrawLine(org->pos, mouse_pos);
	}
}

bool VisualToolRotateZ::InitializeHold() {
	orig_angle = angle + (org->pos - mouse_pos).Angle() * rad2deg;
	return true;
}

void VisualToolRotateZ::UpdateHold() {
	angle = orig_angle - (org->pos - mouse_pos).Angle() * rad2deg;

	if (ctrl_down)
		angle = floorf(angle / 30.f + .5f) * 30.f;

	angle = fmodf(angle + 360.f, 360.f);

	SetSelectedOverride("\\frz", agi::format("%.4g", angle));
}

void VisualToolRotateZ::UpdateDrag(Feature *feature) {
	auto org = GetLineOrigin(active_line);
	if (!org) org = GetLinePosition(active_line);
	auto d = ToScriptCoords(feature->pos) - org;

	auto core = c->GetCore();
	for (auto line : core.selectionController->GetSelectedSet()) {
		org = GetLineOrigin(line);
		if (!org) org = GetLinePosition(line);
		SetOverride(line, "\\org", (d + org).PStr());
	}
}

void VisualToolRotateZ::DoRefresh() {
	if (!active_line) return;

	pos = FromScriptCoords(GetLinePosition(active_line));
	if (!(org->pos = GetLineOrigin(active_line)))
		org->pos = pos;
	else
		org->pos = FromScriptCoords(org->pos);

	GetLineRotation(active_line, rotation_x, rotation_y, angle);
	GetLineScale(active_line, scale);
}

bool VisualToolRotateZ::Nudge(Vector2D direction, VisualNudgeMagnitude magnitude) {
	if (!active_line)
		return false;

	bool changed = false;

	if (direction.X() != 0.f) {
		float const step = static_cast<float>(GetNudgeStep(
			"Tool/Visual/Nudge/Rotate Step",
			"Tool/Visual/Nudge/Rotate Step Large",
			magnitude));
		// Right = clockwise = +\frz in ASS
		angle = fmodf(angle + direction.X() * step + 360.f, 360.f);
		SetSelectedOverride("\\frz", agi::format("%.4g", angle));
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
			SetOverride(line, "\\org", (line_org + delta).PStr());
		}
		changed = true;
	}

	if (!changed)
		return false;

	DoRefresh();
	CommitNudge();
	return true;
}
