#include "visual_guide_interaction.h"

#include "video_overlay_draw_context.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
constexpr double kTan22Point5Degrees = 0.4142135623730950488;

double SquaredDistance(Vector2D first, Vector2D second) noexcept {
	double const x = static_cast<double>(first.X()) - second.X();
	double const y = static_cast<double>(first.Y()) - second.Y();
	return x * x + y * y;
}

bool PointInsideRectangle(Vector2D point, Vector2D origin, Vector2D size) noexcept {
	return point.X() >= origin.X()
		&& point.Y() >= origin.Y()
		&& point.X() <= origin.X() + size.X()
		&& point.Y() <= origin.Y() + size.Y();
}

double DistanceToSegment(Vector2D point, Vector2D first, Vector2D second) noexcept {
	double const dx = static_cast<double>(second.X()) - first.X();
	double const dy = static_cast<double>(second.Y()) - first.Y();
	double const length_squared = dx * dx + dy * dy;
	if (length_squared == 0.0)
		return std::sqrt(SquaredDistance(point, first));

	double const projection = std::clamp(
		((static_cast<double>(point.X()) - first.X()) * dx
			+ (static_cast<double>(point.Y()) - first.Y()) * dy) / length_squared,
		0.0,
		1.0);
	return std::hypot(
		static_cast<double>(point.X()) - (first.X() + projection * dx),
		static_cast<double>(point.Y()) - (first.Y() + projection * dy));
}

void ClampGuideEndpoints(VisualGuide& guide, VisualGuideViewport const& viewport) noexcept {
	guide.first = ClampVisualGuidePointToScript(guide.first, viewport);
	guide.second = ClampVisualGuidePointToScript(guide.second, viewport);
}

template <typename LabelGeometry>
VisualGuideHit HitTestVisualGuidesImpl(
	Vector2D point,
	VisualGuideSnapshotView const& snapshot,
	VisualGuideViewport const& viewport,
	LabelGeometry&& label_geometry,
	double tolerance) noexcept {
	if (!std::isfinite(point.X()) || !std::isfinite(point.Y())
		|| !std::isfinite(tolerance) || tolerance < 0.0
		|| !IsVisualGuideViewportMappable(viewport))
		return {};

	double const tolerance_squared = tolerance * tolerance;
	auto const endpoint_hit = [&](VisualGuide const& guide) -> VisualGuideHit {
		if (!IsValidVisualGuide(guide))
			return {};

		auto const first = VisualGuideToCanvas(guide.first, viewport);
		if (SquaredDistance(point, first) <= tolerance_squared)
			return { guide.id, VisualGuideHitPart::FirstEndpoint };
		auto const second = VisualGuideToCanvas(guide.second, viewport);
		if (SquaredDistance(point, second) <= tolerance_squared)
			return { guide.id, VisualGuideHitPart::SecondEndpoint };
		return {};
	};

	if (snapshot.selected_id) {
		auto const selected = std::find_if(
			snapshot.guides.begin(), snapshot.guides.end(), [&](VisualGuide const& guide) {
				return guide.id == *snapshot.selected_id;
			});
		if (selected != snapshot.guides.end()) {
			if (auto hit = endpoint_hit(*selected))
				return hit;
		}
	}

	for (auto guide = snapshot.guides.crbegin(); guide != snapshot.guides.crend(); ++guide) {
		if (snapshot.selected_id && guide->id == *snapshot.selected_id)
			continue;
		if (auto hit = endpoint_hit(*guide))
			return hit;
	}

	for (auto guide = snapshot.guides.crbegin(); guide != snapshot.guides.crend(); ++guide) {
		if (!IsValidVisualGuide(*guide))
			continue;

		if (DistanceToSegment(
			point,
			VisualGuideToCanvas(guide->first, viewport),
			VisualGuideToCanvas(guide->second, viewport)) <= tolerance)
			return { guide->id, VisualGuideHitPart::Line };
	}

	// Info boxes last, so they never shadow endpoint or line-body drags. A user
	// can still select the box by aiming at its interior padding.
	for (auto index = snapshot.guides.size(); index > 0; --index) {
		auto const& guide = snapshot.guides[index - 1];
		if (!IsValidVisualGuide(guide))
			continue;

		auto const geometry = label_geometry(guide, index - 1);
		if (PointInsideRectangle(point, geometry.origin, geometry.size))
			return {.id = guide.id, .part = VisualGuideHitPart::Label};
	}

	return {};
}
}

VisualGuideHit HitTestVisualGuides(
	Vector2D point,
	VisualGuideSnapshotView const& snapshot,
	VisualGuideViewport const& viewport,
	VisualGuideOverlayStyle const& style,
	VideoOverlayDrawContext& context,
	double tolerance) noexcept {
	return HitTestVisualGuidesImpl(
		point, snapshot, viewport,
		[&](VisualGuide const& guide, std::size_t) {
			return ComputeMeasurementLabelGeometry(
				context, guide, viewport,
				VisualGuideToCanvas(guide.first, viewport),
				VisualGuideToCanvas(guide.second, viewport),
				style);
		},
		tolerance);
}

VisualGuideHit HitTestVisualGuides(
	Vector2D point,
	VisualGuideSnapshotView const& snapshot,
	VisualGuideViewport const& viewport,
	std::span<VisualGuideLabelGeometry const> rendered_labels,
	double tolerance) {
	if (rendered_labels.size() != snapshot.guides.size())
		throw std::invalid_argument("Visual guide label geometry must match its snapshot");
	return HitTestVisualGuidesImpl(
		point, snapshot, viewport,
		[&](VisualGuide const&, std::size_t index) {
			return rendered_labels[index];
		},
		tolerance);
}

VisualGuidePoint SnapVisualGuideMeasurementPoint(
	VisualGuidePoint fixed_point,
	VisualGuidePoint candidate,
	bool enabled) noexcept {
	if (!enabled || !IsFiniteVisualGuidePoint(fixed_point)
		|| !IsFiniteVisualGuidePoint(candidate))
		return candidate;

	double const delta_x = candidate.x - fixed_point.x;
	double const delta_y = candidate.y - fixed_point.y;
	double const abs_x = std::abs(delta_x);
	double const abs_y = std::abs(delta_y);
	if (abs_x == 0.0 && abs_y == 0.0)
		return candidate;

	if (abs_y <= abs_x * kTan22Point5Degrees)
		candidate.y = fixed_point.y;
	else if (abs_x <= abs_y * kTan22Point5Degrees)
		candidate.x = fixed_point.x;
	else {
		double const length = std::max(abs_x, abs_y);
		candidate.x = fixed_point.x + std::copysign(length, delta_x);
		candidate.y = fixed_point.y + std::copysign(length, delta_y);
	}
	return candidate;
}

VisualGuidePoint ClampVisualGuidePointToScript(
	VisualGuidePoint point,
	VisualGuideViewport const& viewport) noexcept {
	if (!IsFiniteVisualGuidePoint(point) || !IsVisualGuideViewportMappable(viewport))
		return point;

	return {
		std::clamp(point.x, 0.0, viewport.script_width),
		std::clamp(point.y, 0.0, viewport.script_height),
	};
}

VisualGuide ApplyVisualGuideDrag(
	VisualGuide const& original,
	VisualGuideDragAction action,
	VisualGuidePoint drag_start,
	VisualGuidePoint current,
	bool snap_measurement,
	VisualGuideViewport const& viewport) noexcept {
	if (!IsValidVisualGuideData(original) || !IsFiniteVisualGuidePoint(drag_start)
		|| !IsFiniteVisualGuidePoint(current))
		return original;

	VisualGuide updated = original;
	switch (action) {
		case VisualGuideDragAction::MoveGuide: {
			double const delta_x = current.x - drag_start.x;
			double const delta_y = current.y - drag_start.y;
			updated.first.x += delta_x;
			updated.first.y += delta_y;
			updated.second.x += delta_x;
			updated.second.y += delta_y;
			break;
		}

		case VisualGuideDragAction::MoveFirstEndpoint:
			updated.first = SnapVisualGuideMeasurementPoint(
				updated.second, current, snap_measurement);
			break;

		case VisualGuideDragAction::MoveSecondEndpoint:
			updated.second = SnapVisualGuideMeasurementPoint(
				updated.first, current, snap_measurement);
			break;
	}

	ClampGuideEndpoints(updated, viewport);
	return updated;
}
