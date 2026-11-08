#pragma once

#include "vector2d.h"

#include <cmath>
#include <numbers>
#include <optional>

namespace aegisub::visual_transform {
constexpr float DegreesPerRadian = 180.f / std::numbers::pi_v<float>;

inline std::optional<float> NudgeRotation(float initial, float delta) {
	float const angle = initial + delta + 360.f;
	if (!std::isfinite(angle))
		return {};
	return std::fmod(angle, 360.f);
}

inline float RotationAnchor(float angle, Vector2D origin, Vector2D pointer) {
	return angle + (origin - pointer).Angle() * DegreesPerRadian;
}

inline float RotateZ(float anchor, Vector2D origin, Vector2D pointer, bool snap) {
	float angle = anchor - (origin - pointer).Angle() * DegreesPerRadian;
	if (snap)
		angle = std::floor(angle / 30.f + .5f) * 30.f;
	return std::fmod(angle + 360.f, 360.f);
}

inline std::optional<Vector2D> RotateXY(Vector2D initial, Vector2D display_delta, bool constrain, bool snap) {
	if (!std::isfinite(initial.X()) || !std::isfinite(initial.Y()) || !std::isfinite(display_delta.X()) || !std::isfinite(display_delta.Y()))
		return {};
	Vector2D delta = display_delta * 2;
	if (constrain)
		delta = delta.SingleAxis();
	float x = initial.X() - delta.Y();
	float y = initial.Y() + delta.X();
	if (snap) {
		x = std::floor(x / 30.f + .5f) * 30.f;
		y = std::floor(y / 30.f + .5f) * 30.f;
	}
	if (!std::isfinite(x) || !std::isfinite(y))
		return {};
	return Vector2D(std::fmod(x + 360.f, 360.f), std::fmod(y + 360.f, 360.f));
}
}
