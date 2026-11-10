#pragma once

#include "visual_guide_controller.h"

#include "vector2d.h"

#include <functional>
#include <optional>
#include <string>

#include <libaegisub/color.h>

class VideoOverlayDrawContext;
struct VideoOverlayTextExtent;
struct VideoOverlayTextStyle;

/// The logical video-content rectangle and the script coordinate system used
/// by visual guides. Canvas values are always in logical pixels, after the
/// current letterbox, zoom and pan layout has been applied. Guide endpoints are
/// stored in script (PlayRes) pixels.
struct VisualGuideViewport {
	double canvas_x = 0.0;
	double canvas_y = 0.0;
	double canvas_width = 0.0;
	double canvas_height = 0.0;
	double script_width = 0.0;
	double script_height = 0.0;
};

/// Return whether canvas geometry and script resolution can be mapped without
/// producing non-finite values or dividing by zero.
[[nodiscard]] bool IsVisualGuideViewportMappable(
	VisualGuideViewport const& viewport) noexcept;

[[nodiscard]] Vector2D VisualGuideToCanvas(
	VisualGuidePoint point,
	VisualGuideViewport const& viewport);

[[nodiscard]] std::optional<VisualGuidePoint> CanvasToVisualGuide(
	Vector2D point,
	VisualGuideViewport const& viewport);

struct VisualGuideOverlayStyle {
	agi::Color line_colour{255, 255, 255};
	agi::Color highlight_colour{255, 255, 0};
	agi::Color outline_colour{0, 0, 0};
	agi::Color label_background_colour{0, 0, 0};
	agi::Color label_border_colour{255, 255, 255};
	int label_font_size = 12;
};

/// Geometry of the floating measurement label, in canvas logical pixels. Shared
/// between the draw pass and hit-testing so selection stays in sync with what
/// the user sees. Origin is the top-left of the background rectangle.
struct VisualGuideLabelGeometry {
	Vector2D origin;
	Vector2D size;
};

/// Compute the label rectangle that DrawMeasurementLabel would render. The
/// anchor is the second endpoint; the rectangle is auto-flipped and clamped to
/// the viewport the same way the draw pass does.
[[nodiscard]] VisualGuideLabelGeometry ComputeMeasurementLabelGeometry(
	VideoOverlayDrawContext& context,
	VisualGuide const& guide,
	VisualGuideViewport const& viewport,
	Vector2D first,
	Vector2D second,
	VisualGuideOverlayStyle const& style);

[[nodiscard]] VisualGuideLabelGeometry ComputeMeasurementLabelGeometry(
	std::function<VideoOverlayTextExtent(std::string const&, VideoOverlayTextStyle const&)> const& measure_text,
	VisualGuide const& guide,
	VisualGuideViewport const& viewport,
	Vector2D first,
	Vector2D second,
	VisualGuideOverlayStyle const& style);

/// Format one value exactly as it is displayed in the measurement labels.
/// This is intentionally separate from the Lua DTO, which always receives
/// unrounded doubles.
[[nodiscard]] std::string FormatVisualGuideLabelNumber(
	double value,
	bool show_positive_sign);

class VisualGuideOverlay {
public:
	/// Draw one transient or persistent guide using the same geometry and label
	/// rules as a controller snapshot. The measure tool uses this only for its
	/// in-progress creation preview; persistent guides are drawn from a view.
	void DrawGuide(
		VideoOverlayDrawContext& context,
		VisualGuideViewport const& viewport,
		VisualGuide const& guide,
		bool selected,
		VisualGuideOverlayStyle const& style) const;
	/// Draw directly from the controller's UI-thread view so a drag repaint does
	/// not allocate or deep-copy the guide vector.
	void Draw(
		VideoOverlayDrawContext& context,
		VisualGuideViewport const& viewport,
		VisualGuideSnapshotView const& snapshot,
		VisualGuideOverlayStyle const& style) const;
};
