#pragma once

#include "visual_guide_controller.h"
#include "visual_guide_overlay.h"

#include <span>
#include <string>

class VideoOverlayDrawContext;

enum class VisualGuideHitPart {
	None,
	FirstEndpoint,
	SecondEndpoint,
	Line,
	/// The floating measurement info box. Dragging it moves the whole guide.
	Label,
};

struct VisualGuideHit {
	std::string id;
	VisualGuideHitPart part = VisualGuideHitPart::None;

	explicit operator bool() const noexcept {
		return part != VisualGuideHitPart::None;
	}
};

enum class VisualGuideDragAction {
	MoveGuide,
	MoveFirstEndpoint,
	MoveSecondEndpoint,
};

/// Hit-test using canvas logical pixels. Selected measurement endpoints have
/// priority, followed by other endpoints, then guide bodies, and finally the
/// info boxes (in reverse draw order). The draw context is needed only to
/// measure the info-box text so its selection rectangle matches the render.
[[nodiscard]] VisualGuideHit HitTestVisualGuides(
	Vector2D point,
	VisualGuideSnapshotView const& snapshot,
	VisualGuideViewport const& viewport,
	VisualGuideOverlayStyle const& style,
	VideoOverlayDrawContext& context,
	double tolerance) noexcept;

[[nodiscard]] VisualGuideHit HitTestVisualGuides(
	Vector2D point,
	VisualGuideSnapshotView const& snapshot,
	VisualGuideViewport const& viewport,
	std::span<VisualGuideLabelGeometry const> rendered_labels,
	double tolerance);

/// Snap a measurement endpoint to 0, 45 or 90 degrees when enabled.
[[nodiscard]] VisualGuidePoint SnapVisualGuideMeasurementPoint(
	VisualGuidePoint fixed_point,
	VisualGuidePoint candidate,
	bool enabled) noexcept;

/// Clamp a guide point into the script-resolution rectangle described by the
/// viewport. Non-finite points are returned unchanged.
[[nodiscard]] VisualGuidePoint ClampVisualGuidePointToScript(
	VisualGuidePoint point,
	VisualGuideViewport const& viewport) noexcept;

/// Apply one drag motion relative to the original guide. Returning an updated
/// value rather than mutating the controller keeps cancellation transactional.
/// Endpoints and whole-guide moves are clamped to the script rectangle.
[[nodiscard]] VisualGuide ApplyVisualGuideDrag(
	VisualGuide const& original,
	VisualGuideDragAction action,
	VisualGuidePoint drag_start,
	VisualGuidePoint current,
	bool snap_measurement,
	VisualGuideViewport const& viewport) noexcept;
