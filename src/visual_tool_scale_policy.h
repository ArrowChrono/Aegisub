// Copyright (c) 2026, Aegisub contributors
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#pragma once

#include "vector2d.h"

#include <cmath>
#include <optional>
#include <vector>

/// Pure math for ratio-preserving Scale tool actions.
namespace visual_tool_scale_policy {

inline std::optional<Vector2D> ScaleDrag(Vector2D initial, Vector2D delta, bool single_axis, bool preserve_ratio, bool snap) {
	delta = delta * Vector2D(1, -1);
	if (single_axis)
		delta = delta.SingleAxis();
	if (preserve_ratio) {
		if (std::abs(delta.X()) > std::abs(delta.Y())) {
			if (initial.X() == 0)
				return std::nullopt;
			delta = Vector2D(delta.X(), delta.X() * (initial.Y() / initial.X()));
		}
		else {
			if (initial.Y() == 0)
				return std::nullopt;
			delta = Vector2D(delta.Y() * (initial.X() / initial.Y()), delta.Y());
		}
	}
	auto scale = Vector2D(0, 0).Max(delta * 1.25f + initial);
	if (!std::isfinite(scale.X()) || !std::isfinite(scale.Y()))
		return std::nullopt;
	return snap ? scale.Round(25.f) : scale;
}

enum class Axis {
	X,
	Y,
};

/// Return the same scale with one axis normalized to 100%, preserving the
/// current ratio. Zero and negative fixed axes are rejected because no finite
/// multiplier can normalize them without inventing or flipping geometry.
inline std::optional<Vector2D> NormalizeTo100(Vector2D current, Axis fixed_axis) noexcept {
	if (!std::isfinite(current.X()) || !std::isfinite(current.Y())
		|| current.X() < 0.0f || current.Y() < 0.0f)
		return std::nullopt;

	float const fixed = fixed_axis == Axis::X ? current.X() : current.Y();
	if (fixed <= 0.0f)
		return std::nullopt;

	float const factor = 100.0f / fixed;
	Vector2D result = fixed_axis == Axis::X
		? Vector2D(100.0f, current.Y() * factor)
		: Vector2D(current.X() * factor, 100.0f);
	return std::isfinite(result.X()) && std::isfinite(result.Y())
		? std::optional<Vector2D>(result)
		: std::nullopt;
}

/// Build an all-or-nothing normalization plan for a subtitle selection.
inline std::optional<std::vector<Vector2D>> NormalizeSelectionTo100(
	std::vector<Vector2D> const& current,
	Axis fixed_axis) {
	if (current.empty())
		return std::nullopt;

	std::vector<Vector2D> normalized;
	normalized.reserve(current.size());
	for (auto scale : current) {
		auto value = NormalizeTo100(scale, fixed_axis);
		if (!value)
			return std::nullopt;
		normalized.push_back(*value);
	}
	return normalized;
}

} // namespace visual_tool_scale_policy
