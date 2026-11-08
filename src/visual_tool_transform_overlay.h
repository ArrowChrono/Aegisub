#pragma once

#include "video_overlay_helpers.h"

#include <array>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <utility>

namespace aegisub::visual_transform {
constexpr float deg2rad = std::numbers::pi_v<float> / 180.f;

inline void DrawProjectedAnnulus(VideoOverlayDrawContext& context, Vector2D origin, Vector2D scale, float rx, float ry, float outer_radius, float inner_radius, float start_deg, float end_deg, float perspective_z_scale) {
	bool const full_circle = std::abs(end_deg - start_deg) < 0.001f;
	float arc_start = start_deg * deg2rad;
	float arc_end = full_circle ? (start_deg + 360.0f) * deg2rad : end_deg * deg2rad;
	if (arc_end <= arc_start)
		arc_end += 2.0f * std::numbers::pi_v<float>;

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

struct RotateXYOverlayState {
	Vector2D origin;
	float angle_x;
	float angle_y;
	float angle_z;
	float fax;
	float fay;
	float perspective_z_scale;
	agi::Color primary_colour;
	agi::Color secondary_colour;
	agi::Color feature_fill_colour;
};

inline auto const& GetRotateXYOverlayGeometry() {
	static constexpr auto geometry = []() consteval {
		constexpr int radius = 15;
		constexpr int spacing = 20;
		constexpr float half_length = spacing * (radius + 1);
		struct Geometry {
			std::array<VideoOverlayPerspectiveVertex, (2 * radius + 1) * 8> grid{};
			std::array<VideoOverlayPerspectiveVertex, 6> axes{};
			std::array<VideoOverlayPerspectiveVertex, 18> arrows{};
		} geometry;
		auto append = [](auto& vertices, size_t& count, video_overlay_helpers::Vec3 first, video_overlay_helpers::Vec3 second, float from, float to) {
			vertices[count++] = {.x = first.x, .y = first.y, .z = first.z, .opacity = from};
			vertices[count++] = {.x = second.x, .y = second.y, .z = second.z, .opacity = to};
		};
		size_t count = 0;
		for (int i = -radius; i <= radius; ++i) {
			auto const position = static_cast<float>(spacing * i);
			float const alpha = 1.f - (i < 0 ? -i : i) * (.9f / radius);
			append(geometry.grid, count, {.x = position, .y = half_length, .z = 0}, {.x = position, .y = 0, .z = 0}, 0, alpha);
			append(geometry.grid, count, {.x = position, .y = 0, .z = 0}, {.x = position, .y = -half_length, .z = 0}, alpha, 0);
			append(geometry.grid, count, {.x = half_length, .y = position, .z = 0}, {.x = 0, .y = position, .z = 0}, 0, alpha);
			append(geometry.grid, count, {.x = 0, .y = position, .z = 0}, {.x = -half_length, .y = position, .z = 0}, alpha, 0);
		}
		count = 0;
		append(geometry.axes, count, {.x = 0, .y = 0, .z = 0}, {.x = 50, .y = 0, .z = 0}, 1, 1);
		append(geometry.axes, count, {.x = 0, .y = 0, .z = 0}, {.x = 0, .y = 50, .z = 0}, 1, 1);
		append(geometry.axes, count, {.x = 0, .y = 0, .z = 0}, {.x = 0, .y = 0, .z = 50}, 1, 1);
		video_overlay_helpers::Vec3 const points[][6] = {
			{{.x = 60, .y = 0, .z = 0}, {.x = 50, .y = -3, .z = -3}, {.x = 50, .y = 3, .z = -3}, {.x = 50, .y = 3, .z = 3}, {.x = 50, .y = -3, .z = 3}, {.x = 50, .y = -3, .z = -3}},
			{{.x = 0, .y = 60, .z = 0}, {.x = -3, .y = 50, .z = -3}, {.x = 3, .y = 50, .z = -3}, {.x = 3, .y = 50, .z = 3}, {.x = -3, .y = 50, .z = 3}, {.x = -3, .y = 50, .z = -3}},
			{{.x = 0, .y = 0, .z = 60}, {.x = -3, .y = -3, .z = 50}, {.x = 3, .y = -3, .z = 50}, {.x = 3, .y = 3, .z = 50}, {.x = -3, .y = 3, .z = 50}, {.x = -3, .y = -3, .z = 50}},
		};
		count = 0;
		for (auto const& arrow : points)
			for (int i = 0; i < 6; i += 2)
				append(geometry.arrows, count, arrow[i], arrow[i + 1], 1, 1);
		return VideoOverlayStaticPerspectiveBatches(geometry.grid, geometry.axes, geometry.arrows);
	}();
	return geometry;
}

class RotateXYOverlayGeometryCache {
	std::shared_ptr<VideoOverlayPreparedPerspectiveBatches const> prepared;

	public:
	std::shared_ptr<VideoOverlayPreparedPerspectiveBatches const> Get(agi::Color const& secondary) {
		if (!prepared || prepared->SecondaryColour() != secondary)
			prepared = std::make_shared<VideoOverlayPreparedPerspectiveBatches>(GetRotateXYOverlayGeometry(), secondary);
		return prepared;
	}
};

inline void DrawRotateXYOverlay(VideoOverlayDrawContext& context, RotateXYOverlayState const& state) {
	auto const& geometry = GetRotateXYOverlayGeometry();
	context.DrawPerspectiveBatches(geometry.Grid(), geometry.Axes(), geometry.Arrows(),
								   {.origin = state.origin, .angle_x = state.angle_x, .angle_y = state.angle_y, .angle_z = state.angle_z, .fax = state.fax, .fay = state.fay, .perspective_z_scale = state.perspective_z_scale},
								   state.primary_colour, state.secondary_colour, 2);
}

inline void DrawRotateXYOverlay(VideoOverlayDrawContext& context, RotateXYOverlayState const& state,
								VideoOverlayPreparedPerspectiveBatches const& prepared) {
	context.DrawPreparedPerspectiveBatches(prepared,
										   {.origin = state.origin, .angle_x = state.angle_x, .angle_y = state.angle_y, .angle_z = state.angle_z, .fax = state.fax, .fay = state.fay, .perspective_z_scale = state.perspective_z_scale},
										   state.primary_colour, 2);
}

struct RotateZOverlayState {
	Vector2D origin;
	Vector2D pos;
	Vector2D scale;
	float rotation_x;
	float rotation_y;
	float angle;
	float perspective_z_scale;
	agi::Color primary_colour;
	agi::Color secondary_colour;
	agi::Color highlight_colour;
};

inline void DrawRotateZOverlay(VideoOverlayDrawContext& context, RotateZOverlayState const& state) {
	auto const& line_color_primary = state.primary_colour;
	auto const& line_color_secondary = state.secondary_colour;
	auto const& highlight_color = state.highlight_colour;
	float const perspective_z_scale = state.perspective_z_scale;

	float radius = (state.pos - state.origin).Len();
	float const original_radius = radius;
	if (radius < 50)
		radius = 50;

	context.SetLineColour(line_color_secondary, 1.0f, 1);
	context.SetFillColour(highlight_color, 0.3f);
	DrawProjectedAnnulus(context, state.origin, state.scale, state.rotation_x, state.rotation_y, radius + 4, radius - 4, 0.0f, 0.0f, perspective_z_scale);

	int const markers = 6;
	float const mark_start = -90.0f / markers;
	float const mark_end = mark_start + (180.0f / markers);
	for (int i = 0; i < markers; ++i) {
		float const marker_angle = i * (360.0f / markers);
		DrawProjectedAnnulus(context, state.origin, state.scale, state.rotation_x, state.rotation_y, radius + 30, radius + 12, marker_angle + mark_start, marker_angle + mark_end, perspective_z_scale);
	}

	Vector2D const angle_vec = Vector2D::FromAngle(state.angle * deg2rad);
	context.SetLineColour(line_color_primary, 1.0f, 2);
	context.SetFillColour(line_color_primary, 0.0f);
	video_overlay_helpers::DrawProjectedLine(context, angle_vec * -radius, angle_vec * radius, state.origin, state.scale, state.rotation_x, state.rotation_y, 0.0f, perspective_z_scale);

	if (state.origin != state.pos) {
		Vector2D const rotated_pos = Vector2D::FromAngle(state.angle * deg2rad - (state.pos - state.origin).Angle()) * original_radius;
		video_overlay_helpers::DrawProjectedLine(context, Vector2D(), rotated_pos, state.origin, state.scale, state.rotation_x, state.rotation_y, 0.0f, perspective_z_scale);
		video_overlay_helpers::DrawProjectedLine(context, rotated_pos - angle_vec * 20, rotated_pos + angle_vec * 20, state.origin, state.scale, state.rotation_x, state.rotation_y, 0.0f, perspective_z_scale);
	}

	context.SetLineColour(line_color_secondary, 1.0f, 1);
	context.SetFillColour(highlight_color, 0.3f);
	context.DrawCircle(
		video_overlay_helpers::ProjectScaledRotatedPoint(angle_vec * radius, state.origin, state.scale, state.rotation_x, state.rotation_y, 0.0f, perspective_z_scale),
		video_overlay_helpers::ProjectCircleRadius(angle_vec * radius, 4.0f, state.origin, state.scale, state.rotation_x, state.rotation_y, 0.0f, perspective_z_scale));
	context.DrawCircle(
		video_overlay_helpers::ProjectScaledRotatedPoint(angle_vec * -radius, state.origin, state.scale, state.rotation_x, state.rotation_y, 0.0f, perspective_z_scale),
		video_overlay_helpers::ProjectCircleRadius(angle_vec * -radius, 4.0f, state.origin, state.scale, state.rotation_x, state.rotation_y, 0.0f, perspective_z_scale));
}

struct ScaleOverlayState {
	Vector2D scale;
	Vector2D pos;
	Vector2D video_res;
	float rx;
	float ry;
	float rz;
	float perspective_z_scale;
	unsigned long line_color_primary;
	unsigned long line_color_secondary;
	unsigned long highlight_color;
	Vector2D video_position{0, 0};
};

inline void DrawScaleOverlay(VideoOverlayDrawContext& context, ScaleOverlayState const& state) {
	static const int base_len = 160;
	static const int guide_size = 10;

	auto const line_color_primary = VideoOverlayColourFromRgb(state.line_color_primary);
	auto const line_color_secondary = VideoOverlayColourFromRgb(state.line_color_secondary);
	auto const highlight_color = VideoOverlayColourFromRgb(state.highlight_color);
	Vector2D const overlay_scale(100.0f, 100.0f);

	Vector2D const base_point = (state.pos - state.video_position)
									.Max(Vector2D(base_len / 2 + guide_size, base_len / 2 + guide_size))
									.Min(state.video_res - base_len / 2 - guide_size * 3) +
								state.video_position;

	Vector2D const scale_half_length = state.scale * base_len / 200;
	float const minor_dim_offset = base_len / 2 + guide_size * 1.5f;

	Vector2D const x_p1(minor_dim_offset, -scale_half_length.Y());
	Vector2D const x_p2(minor_dim_offset, scale_half_length.Y());
	Vector2D const y_p1(-scale_half_length.X(), minor_dim_offset);
	Vector2D const y_p2(scale_half_length.X(), minor_dim_offset);

	context.SetLineColour(line_color_primary, 1.f, 2);
	video_overlay_helpers::DrawProjectedLine(context, x_p1, x_p2, base_point, overlay_scale, state.rx, state.ry, state.rz, state.perspective_z_scale);
	video_overlay_helpers::DrawProjectedLine(context, y_p1, y_p2, base_point, overlay_scale, state.rx, state.ry, state.rz, state.perspective_z_scale);

	context.SetLineColour(line_color_secondary, 1.f, 1);
	context.SetFillColour(highlight_color, 0.3f);
	context.DrawCircle(
		video_overlay_helpers::ProjectScaledRotatedPoint(x_p1, base_point, overlay_scale, state.rx, state.ry, state.rz, state.perspective_z_scale),
		video_overlay_helpers::ProjectCircleRadius(x_p1, 4.0f, base_point, overlay_scale, state.rx, state.ry, state.rz, state.perspective_z_scale));
	context.DrawCircle(
		video_overlay_helpers::ProjectScaledRotatedPoint(x_p2, base_point, overlay_scale, state.rx, state.ry, state.rz, state.perspective_z_scale),
		video_overlay_helpers::ProjectCircleRadius(x_p2, 4.0f, base_point, overlay_scale, state.rx, state.ry, state.rz, state.perspective_z_scale));
	context.DrawCircle(
		video_overlay_helpers::ProjectScaledRotatedPoint(y_p1, base_point, overlay_scale, state.rx, state.ry, state.rz, state.perspective_z_scale),
		video_overlay_helpers::ProjectCircleRadius(y_p1, 4.0f, base_point, overlay_scale, state.rx, state.ry, state.rz, state.perspective_z_scale));
	context.DrawCircle(
		video_overlay_helpers::ProjectScaledRotatedPoint(y_p2, base_point, overlay_scale, state.rx, state.ry, state.rz, state.perspective_z_scale),
		video_overlay_helpers::ProjectCircleRadius(y_p2, 4.0f, base_point, overlay_scale, state.rx, state.ry, state.rz, state.perspective_z_scale));

	int const half_len = base_len / 2;
	context.SetLineColour(line_color_secondary, 1.0f, 1);
	context.SetFillColour(highlight_color, 0.3f);
	video_overlay_helpers::DrawProjectedQuad(
		context,
		Vector2D(half_len, -half_len),
		Vector2D(half_len + guide_size, -half_len),
		Vector2D(half_len + guide_size, half_len),
		Vector2D(half_len, half_len),
		base_point, overlay_scale, state.rx, state.ry, state.rz, state.perspective_z_scale);
	video_overlay_helpers::DrawProjectedQuad(
		context,
		Vector2D(-half_len, half_len),
		Vector2D(half_len, half_len),
		Vector2D(half_len, half_len + guide_size),
		Vector2D(-half_len, half_len + guide_size),
		base_point, overlay_scale, state.rx, state.ry, state.rz, state.perspective_z_scale);
	context.SetFillColour(highlight_color, 0.0f);

	context.SetLineColour(line_color_secondary, 1.f, 2);
	video_overlay_helpers::DrawProjectedLine(context, Vector2D(half_len + guide_size, -half_len), Vector2D(half_len + guide_size + guide_size / 2, -half_len), base_point, overlay_scale, state.rx, state.ry, state.rz, state.perspective_z_scale);
	video_overlay_helpers::DrawProjectedLine(context, Vector2D(half_len + guide_size, half_len), Vector2D(half_len + guide_size + guide_size / 2, half_len), base_point, overlay_scale, state.rx, state.ry, state.rz, state.perspective_z_scale);
	video_overlay_helpers::DrawProjectedLine(context, Vector2D(-half_len, half_len + guide_size), Vector2D(-half_len, half_len + guide_size + guide_size / 2), base_point, overlay_scale, state.rx, state.ry, state.rz, state.perspective_z_scale);
	video_overlay_helpers::DrawProjectedLine(context, Vector2D(half_len, half_len + guide_size), Vector2D(half_len, half_len + guide_size + guide_size / 2), base_point, overlay_scale, state.rx, state.ry, state.rz, state.perspective_z_scale);
}

}
