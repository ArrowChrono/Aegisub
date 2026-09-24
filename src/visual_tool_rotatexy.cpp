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

#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "selection_controller.h"
#include "video_overlay_draw_context.h"
#include "video_overlay_helpers.h"
#include "visual_tool_render_snapshot.h"

#include <libaegisub/format.h>

#include <wx/colour.h>

namespace {
struct RotateXYState {
	Vector2D origin;
	float angle_x;
	float angle_y;
	float angle_z;
	float fax;
	float fay;
	float perspective_z_scale;
	unsigned long primary_colour;
	unsigned long secondary_colour;
	unsigned long feature_fill_colour;
};

void DrawRotateXYGeometry(OpenGLWrapper& gl, RotateXYState const& state) {
	wxColour const line_color_primary(state.primary_colour);
	wxColour const line_color_secondary(state.secondary_colour);
	// Transform grid
	gl.SetOrigin(state.origin);
	// libass uses camera distance = 20000 * blur_scale_y,
	// where blur_scale_y = frame_height / LayoutResY.
	// Compensate for PlayRes ≠ LayoutRes in the OpenGL preview.
	float const perspective_z_scale = state.perspective_z_scale;
	gl.SetRotation(state.angle_x, state.angle_y, state.angle_z, perspective_z_scale);
	gl.SetShear(state.fax, state.fay);

	// Draw grid
	gl.SetLineColour(line_color_secondary, 0.5f, 2);
	gl.SetModeLine();
	float r = line_color_secondary.Red() / 255.f;
	float g = line_color_secondary.Green() / 255.f;
	float b = line_color_secondary.Blue() / 255.f;

	// Number of lines on each side of each axis
	static const int radius = 15;
	// Total number of lines, including center axis line
	static const int line_count = radius * 2 + 1;
	// Distance between each line in pixels
	static const int spacing = 20;
	// Length of each grid line in pixels from axis to one end
	static const int half_line_length = spacing * (radius + 1);
	static const float fade_factor = 0.9f / radius;

	std::vector<float> colors(line_count * 8 * 4);
	for (int i = 0; i < line_count * 8; ++i) {
		colors[i * 4 + 0] = r;
		colors[i * 4 + 1] = g;
		colors[i * 4 + 2] = b;
		colors[i * 4 + 3] = (i + 3) % 4 > 1 ? 0 : (1.f - abs(i / 8 - radius) * fade_factor);
	}

	std::vector<float> points(line_count * 8 * 2);
	for (int i = 0; i < line_count; ++i) {
		int pos = spacing * (i - radius);

		points[i * 16 + 0] = pos;
		points[i * 16 + 1] = half_line_length;

		points[i * 16 + 2] = pos;
		points[i * 16 + 3] = 0;

		points[i * 16 + 4] = pos;
		points[i * 16 + 5] = 0;

		points[i * 16 + 6] = pos;
		points[i * 16 + 7] = -half_line_length;

		points[i * 16 + 8] = half_line_length;
		points[i * 16 + 9] = pos;

		points[i * 16 + 10] = 0;
		points[i * 16 + 11] = pos;

		points[i * 16 + 12] = 0;
		points[i * 16 + 13] = pos;

		points[i * 16 + 14] = -half_line_length;
		points[i * 16 + 15] = pos;
	}

	gl.DrawLines(2, points, 4, colors);

	// Draw vectors
	gl.SetLineColour(line_color_primary, 1.f, 2);
	float vectors[] = {
		0.f, 0.f, 0.f,
		50.f, 0.f, 0.f,
		0.f, 0.f, 0.f,
		0.f, 50.f, 0.f,
		0.f, 0.f, 0.f,
		0.f, 0.f, 50.f,
	};
	gl.DrawLines(3, vectors, 6);

	// Draw arrow tops
	float arrows[] = {
		60.f,  0.f,  0.f,
		50.f, -3.f, -3.f,
		50.f,  3.f, -3.f,
		50.f,  3.f,  3.f,
		50.f, -3.f,  3.f,
		50.f, -3.f, -3.f,

		 0.f, 60.f,  0.f,
		-3.f, 50.f, -3.f,
		 3.f, 50.f, -3.f,
		 3.f, 50.f,  3.f,
		-3.f, 50.f,  3.f,
		-3.f, 50.f, -3.f,

		 0.f,  0.f, 60.f,
		-3.f, -3.f, 50.f,
		 3.f, -3.f, 50.f,
		 3.f,  3.f, 50.f,
		-3.f,  3.f, 50.f,
		-3.f, -3.f, 50.f,
	};

	gl.DrawLines(3, arrows, 18);

	gl.ResetTransform();
}

class RotateXYRenderSnapshot final : public VisualToolRenderSnapshot {
	RotateXYState state;

public:
	RotateXYRenderSnapshot(std::shared_ptr<const VisualToolRenderContext> context, RotateXYState const& state)
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
		DrawRotateXYGeometry(gl, state);
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
		.primary_colour = to_wx(line_color_primary_opt->GetColor()).GetRGB(),
		.secondary_colour = to_wx(line_color_secondary_opt->GetColor()).GetRGB(),
		.feature_fill_colour = 0,
	};
	DrawRotateXYGeometry(gl, state);
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
		.primary_colour = primary,
		.secondary_colour = secondary,
		.feature_fill_colour = feature_fill,
	};
	return std::make_shared<RotateXYRenderSnapshot>(context, state);
}

void VisualToolRotateXY::DrawOverlay(VideoOverlayDrawContext &context) {
	if (!active_line) return;

	DrawAllFeatures(context);

	wxColour const line_color_primary = to_wx(line_color_primary_opt->GetColor());
	wxColour const line_color_secondary = to_wx(line_color_secondary_opt->GetColor());
	float const perspective_z_scale = video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(script_res, layout_res);

	static const int radius = 15;
	static const int line_count = radius * 2 + 1;
	static const int spacing = 20;
	static const int half_line_length = spacing * (radius + 1);
	static const float fade_factor = 0.9f / radius;

	for (int i = 0; i < line_count; ++i) {
		int const pos = spacing * (i - radius);
		float const alpha = 1.0f - std::abs(i - radius) * fade_factor;

		video_overlay_helpers::DrawProjectedFadedLine(
			context,
			{ static_cast<float>(pos), static_cast<float>(half_line_length), 0.0f },
			{ static_cast<float>(pos), 0.0f, 0.0f },
			org->pos, angle_x, angle_y, angle_z, fax, fay,
			line_color_secondary, 0.0f, alpha, 2, perspective_z_scale);
		video_overlay_helpers::DrawProjectedFadedLine(
			context,
			{ static_cast<float>(pos), 0.0f, 0.0f },
			{ static_cast<float>(pos), static_cast<float>(-half_line_length), 0.0f },
			org->pos, angle_x, angle_y, angle_z, fax, fay,
			line_color_secondary, alpha, 0.0f, 2, perspective_z_scale);

		video_overlay_helpers::DrawProjectedFadedLine(
			context,
			{ static_cast<float>(half_line_length), static_cast<float>(pos), 0.0f },
			{ 0.0f, static_cast<float>(pos), 0.0f },
			org->pos, angle_x, angle_y, angle_z, fax, fay,
			line_color_secondary, 0.0f, alpha, 2, perspective_z_scale);
		video_overlay_helpers::DrawProjectedFadedLine(
			context,
			{ 0.0f, static_cast<float>(pos), 0.0f },
			{ static_cast<float>(-half_line_length), static_cast<float>(pos), 0.0f },
			org->pos, angle_x, angle_y, angle_z, fax, fay,
			line_color_secondary, alpha, 0.0f, 2, perspective_z_scale);
	}

	context.SetLineColour(line_color_primary, 1.0f, 2);
	video_overlay_helpers::DrawProjectedLine(context, { 0.0f, 0.0f, 0.0f }, { 50.0f, 0.0f, 0.0f }, org->pos, angle_x, angle_y, angle_z, fax, fay, perspective_z_scale);
	video_overlay_helpers::DrawProjectedLine(context, { 0.0f, 0.0f, 0.0f }, { 0.0f, 50.0f, 0.0f }, org->pos, angle_x, angle_y, angle_z, fax, fay, perspective_z_scale);
	video_overlay_helpers::DrawProjectedLine(context, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 50.0f }, org->pos, angle_x, angle_y, angle_z, fax, fay, perspective_z_scale);

	video_overlay_helpers::Vec3 const arrow_sets[][6] = {
		{{60.f,0.f,0.f},{50.f,-3.f,-3.f},{50.f,3.f,-3.f},{50.f,3.f,3.f},{50.f,-3.f,3.f},{50.f,-3.f,-3.f}},
		{{0.f,60.f,0.f},{-3.f,50.f,-3.f},{3.f,50.f,-3.f},{3.f,50.f,3.f},{-3.f,50.f,3.f},{-3.f,50.f,-3.f}},
		{{0.f,0.f,60.f},{-3.f,-3.f,50.f},{3.f,-3.f,50.f},{3.f,3.f,50.f},{-3.f,3.f,50.f},{-3.f,-3.f,50.f}},
	};
	for (auto const& arrow : arrow_sets) {
		for (int i = 0; i < 5; ++i)
			video_overlay_helpers::DrawProjectedLine(context, arrow[i], arrow[i + 1], org->pos, angle_x, angle_y, angle_z, fax, fay, perspective_z_scale);
	}
}

bool VisualToolRotateXY::InitializeHold() {
	orig_x = angle_x;
	orig_y = angle_y;

	return true;
}

void VisualToolRotateXY::UpdateHold() {
	Vector2D delta = (mouse_pos - drag_start) * 2;
	if (shift_down)
		delta = delta.SingleAxis();

	angle_x = orig_x - delta.Y();
	angle_y = orig_y + delta.X();

	if (ctrl_down) {
		angle_x = floorf(angle_x / 30.f + .5f) * 30.f;
		angle_y = floorf(angle_y / 30.f + .5f) * 30.f;
	}

	angle_x = fmodf(angle_x + 360.f, 360.f);
	angle_y = fmodf(angle_y + 360.f, 360.f);

	SetSelectedOverride("\\frx", agi::format("%.4g", angle_x));
	SetSelectedOverride("\\fry", agi::format("%.4g", angle_y));
}

void VisualToolRotateXY::UpdateDrag(Feature *feature) {
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

void VisualToolRotateXY::DoRefresh() {
	if (!active_line) return;

	if (!(org->pos = GetLineOrigin(active_line)))
		org->pos = GetLinePosition(active_line);
	org->pos = FromScriptCoords(org->pos);

	GetLineRotation(active_line, angle_x, angle_y, angle_z);
	GetLineShear(active_line, fax, fay);
}

bool VisualToolRotateXY::Nudge(Vector2D direction, VisualNudgeMagnitude magnitude) {
	if (!active_line)
		return false;

	float const step = static_cast<float>(GetNudgeStep(
		"Tool/Visual/Nudge/Rotate Step",
		"Tool/Visual/Nudge/Rotate Step Large",
		magnitude));

	// Same polarity as UpdateHold: right → +\fry, up → +\frx
	if (direction.X() != 0.f) {
		angle_y = fmodf(angle_y + direction.X() * step + 360.f, 360.f);
		SetSelectedOverride("\\fry", agi::format("%.4g", angle_y));
	}
	if (direction.Y() != 0.f) {
		// direction.Y is -1 for Up; UpdateHold uses angle_x = orig_x - delta.Y with
		// screen-Y-down, so Up decreases screen Y → increases frx when mirrored as -direction.Y.
		angle_x = fmodf(angle_x - direction.Y() * step + 360.f, 360.f);
		SetSelectedOverride("\\frx", agi::format("%.4g", angle_x));
	}

	DoRefresh();
	CommitNudge();
	return true;
}
