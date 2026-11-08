// Copyright (c) 2026
// All rights reserved.

#pragma once

#include "vector2d.h"
#include "video_overlay_draw_context.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace video_overlay_helpers {

constexpr float kDegToRad = 3.1415926536f / 180.0f;
constexpr float kPerspectiveDistance = 2500.0f;
constexpr float kPerspectiveZScale = 8.0f;

inline float GetLayoutResAdjustedPerspectiveZScale(Vector2D script_res, Vector2D layout_res) {
	if (script_res.Y() <= 0.0f || layout_res.Y() <= 0.0f)
		return kPerspectiveZScale;
	return kPerspectiveZScale * layout_res.Y() / script_res.Y();
}

struct Vec3 {
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

inline Vec3 ApplyShear(Vec3 v, float fax, float fay) {
	return {
		v.x + fax * v.y,
		fay * v.x + v.y,
		v.z
	};
}

inline Vec3 RotateZNeg(Vec3 v, float degrees) {
	float const angle = -degrees * kDegToRad;
	float const c = std::cos(angle);
	float const s = std::sin(angle);
	return { c * v.x - s * v.y, s * v.x + c * v.y, v.z };
}

inline Vec3 RotateXNeg(Vec3 v, float degrees) {
	float const angle = -degrees * kDegToRad;
	float const c = std::cos(angle);
	float const s = std::sin(angle);
	return { v.x, c * v.y - s * v.z, s * v.y + c * v.z };
}

inline Vec3 RotateYNeg(Vec3 v, float degrees) {
	float const angle = -degrees * kDegToRad;
	float const c = std::cos(angle);
	float const s = std::sin(angle);
	return { c * v.x + s * v.z, v.y, -s * v.x + c * v.z };
}

inline Vector2D ProjectPerspective(Vec3 v, Vector2D origin, float z_scale = kPerspectiveZScale, float distance = kPerspectiveDistance) {
	v.z *= z_scale;

	float denom = v.z + distance;
	if (std::abs(denom) < 1.0f)
		denom = denom < 0.0f ? -1.0f : 1.0f;
	float const perspective = distance / denom;
	return origin + Vector2D(v.x * perspective, v.y * perspective);
}

inline Vector2D ProjectScaledRotatedPoint(Vector2D local, Vector2D origin, Vector2D scale, float rx, float ry, float rz, float perspective_z_scale = kPerspectiveZScale) {
	Vec3 v {
		local.X() * scale.X() / 100.0f,
		local.Y() * scale.Y() / 100.0f,
		0.0f
	};
	v = RotateZNeg(v, rz);
	v = RotateXNeg(v, rx);
	v = RotateYNeg(v, ry);
	return ProjectPerspective(v, origin, perspective_z_scale);
}

inline Vector2D ProjectRotatedShearedPoint(Vec3 v, Vector2D origin, float ax, float ay, float az, float fax, float fay, float perspective_z_scale = kPerspectiveZScale) {
	v = ApplyShear(v, fax, fay);
	v = RotateZNeg(v, az);
	v = RotateXNeg(v, ax);
	v = RotateYNeg(v, ay);
	return ProjectPerspective(v, origin, perspective_z_scale);
}

inline float ProjectCircleRadius(Vector2D center_local, float radius, Vector2D origin, Vector2D scale, float rx, float ry, float rz, float perspective_z_scale = kPerspectiveZScale) {
	Vector2D const center = ProjectScaledRotatedPoint(center_local, origin, scale, rx, ry, rz, perspective_z_scale);
	Vector2D const px = ProjectScaledRotatedPoint(center_local + Vector2D(radius, 0.0f), origin, scale, rx, ry, rz, perspective_z_scale);
	Vector2D const py = ProjectScaledRotatedPoint(center_local + Vector2D(0.0f, radius), origin, scale, rx, ry, rz, perspective_z_scale);
	return std::max(1.0f, (px - center).Len() + (py - center).Len()) * 0.5f;
}

inline void DrawDashedLine(VideoOverlayDrawContext &context, Vector2D p1, Vector2D p2, float step) {
	Vector2D const delta = p2 - p1;
	float const len = delta.Len();
	if (len <= 0.0f || step <= 0.0f)
		return;

	Vector2D const unit = delta / len;
	std::vector<float> lines;
	for (float offset = 0.0f; offset < len; offset += step * 2.0f) {
		float const end_offset = std::min(len, offset + step);
		Vector2D const start = p1 + unit * offset;
		Vector2D const end = p1 + unit * end_offset;
		lines.insert(lines.end(), { start.X(), start.Y(), end.X(), end.Y() });
	}
	context.DrawLines(2, lines.data(), lines.size() / 2);
}

inline void DrawLineStripFromFloatPoints(VideoOverlayDrawContext &context, std::vector<float> const& points) {
	if (points.size() < 4 || points.size() % 2 != 0)
		return;

	std::vector<Vector2D> polyline;
	polyline.reserve(points.size() / 2);
	for (size_t i = 0; i + 1 < points.size(); i += 2)
		polyline.emplace_back(points[i], points[i + 1]);
	context.DrawLineStrip(polyline.data(), polyline.size());
}

inline void DrawProjectedLine(VideoOverlayDrawContext &context, Vector2D p1, Vector2D p2, Vector2D origin, Vector2D scale, float rx, float ry, float rz, float perspective_z_scale = kPerspectiveZScale) {
	context.DrawLine(
		ProjectScaledRotatedPoint(p1, origin, scale, rx, ry, rz, perspective_z_scale),
		ProjectScaledRotatedPoint(p2, origin, scale, rx, ry, rz, perspective_z_scale));
}

inline void DrawProjectedLine(VideoOverlayDrawContext &context, Vec3 p1, Vec3 p2, Vector2D origin, float ax, float ay, float az, float fax, float fay, float perspective_z_scale = kPerspectiveZScale) {
	context.DrawLine(
		ProjectRotatedShearedPoint(p1, origin, ax, ay, az, fax, fay, perspective_z_scale),
		ProjectRotatedShearedPoint(p2, origin, ax, ay, az, fax, fay, perspective_z_scale));
}

inline void DrawProjectedFadedLine(VideoOverlayDrawContext& context, Vec3 p1, Vec3 p2, Vector2D origin, float ax, float ay, float az, float fax, float fay, agi::Color const& colour, float alpha1, float alpha2, int width, float perspective_z_scale = kPerspectiveZScale) {
	static constexpr int segments = 8;
	for (int i = 0; i < segments; ++i) {
		float const t0 = static_cast<float>(i) / segments;
		float const t1 = static_cast<float>(i + 1) / segments;
		Vec3 const a {
			p1.x + (p2.x - p1.x) * t0,
			p1.y + (p2.y - p1.y) * t0,
			p1.z + (p2.z - p1.z) * t0
		};
		Vec3 const b {
			p1.x + (p2.x - p1.x) * t1,
			p1.y + (p2.y - p1.y) * t1,
			p1.z + (p2.z - p1.z) * t1
		};
		float const alpha = (alpha1 + (alpha2 - alpha1) * (t0 + t1) * 0.5f);
		context.SetLineColour(colour, alpha, width);
		DrawProjectedLine(context, a, b, origin, ax, ay, az, fax, fay, perspective_z_scale);
	}
}

inline void DrawProjectedQuad(VideoOverlayDrawContext &context, Vector2D p1, Vector2D p2, Vector2D p3, Vector2D p4, Vector2D origin, Vector2D scale, float rx, float ry, float rz, float perspective_z_scale = kPerspectiveZScale) {
	Vector2D const polygon[] = {
		ProjectScaledRotatedPoint(p1, origin, scale, rx, ry, rz, perspective_z_scale),
		ProjectScaledRotatedPoint(p2, origin, scale, rx, ry, rz, perspective_z_scale),
		ProjectScaledRotatedPoint(p3, origin, scale, rx, ry, rz, perspective_z_scale),
		ProjectScaledRotatedPoint(p4, origin, scale, rx, ry, rz, perspective_z_scale),
	};
	context.DrawPolygon(polygon, 4);
}

} // namespace video_overlay_helpers
