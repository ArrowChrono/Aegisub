#include "visual_guide_overlay.h"

#include "compat.h"

#include "video_overlay_draw_context.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace {
constexpr double kDisplayZeroThreshold = 0.0005;
constexpr double kLabelPadding = 4.0;
constexpr double kLabelOffset = 12.0;
// Filled-quad stroke widths in canvas logical pixels. Geometry owns the width
// so the result does not depend on glLineWidth / GL_LINE_SMOOTH quality.
// Sized to match the smaller measure-tool endpoints (radius 3).
constexpr float kGuideOutlineWidth = 3.0f;
constexpr float kGuideCoreWidth = 1.5f;
// Arrowhead at the second endpoint, built as a filled triangle. The outline
// pass is one pixel larger in both length and half-width so it peeks around
// the core the same way the stroke outline does.
constexpr float kArrowCoreLength = 9.0f;
constexpr float kArrowCoreHalfWidth = 3.5f;
constexpr float kArrowOutlineGrowth = 1.0f;
constexpr float kArrowOutlineAlpha = 0.85f;
// Reject near-zero segments before Unit(). Sub-pixel directions make
// Perpendicular()*half-width collapse the quad so Skia fill area is zero.
// FinishInteraction already refuses commits shorter than the hit tolerance;
// this mainly protects the creation preview.
constexpr float kMinStrokeSquareLen = 1e-4f;

bool IsFinite(double value) {
	return std::isfinite(value);
}

bool IsFiniteCanvas(VisualGuideViewport const& viewport) {
	return IsFinite(viewport.canvas_x) && IsFinite(viewport.canvas_y)
		&& IsFinite(viewport.canvas_width) && IsFinite(viewport.canvas_height)
		&& viewport.canvas_width > 0.0 && viewport.canvas_height > 0.0;
}

bool HasScriptDimensions(VisualGuideViewport const& viewport) {
	return IsFinite(viewport.script_width) && IsFinite(viewport.script_height)
		&& viewport.script_width > 0.0 && viewport.script_height > 0.0;
}

void DrawGuideStroke(
	VideoOverlayDrawContext& context,
	Vector2D first,
	Vector2D second,
	wxColour const& colour,
	float alpha,
	float width) {
	Vector2D direction = second - first;
	if (direction.SquareLen() <= kMinStrokeSquareLen || width <= 0.0f || alpha <= 0.0f)
		return;

	Vector2D const unit = direction.Unit();
	if (unit.SquareLen() <= 0.0f)
		return;

	Vector2D const half_normal = unit.Perpendicular() * (width * 0.5f);
	Vector2D const polygon[4] = {
		first + half_normal,
		second + half_normal,
		second - half_normal,
		first - half_normal,
	};

	// Fill only: line alpha 0 skips stroke on both legacy and Skia backends.
	context.SetFillColour(from_wx(colour), alpha);
	context.SetLineColour(from_wx(colour), 0.0f, 1);
	context.DrawPolygon(polygon, 4);
}

void DrawGuideArrowhead(
	VideoOverlayDrawContext& context,
	Vector2D tip,
	Vector2D direction,
	wxColour const& colour,
	float alpha,
	float length,
	float half_width) {
	if (direction.SquareLen() <= 0.0f || alpha <= 0.0f || length <= 0.0f)
		return;

	Vector2D const unit = direction.Unit();
	if (unit.SquareLen() <= 0.0f)
		return;

	Vector2D const perp = Vector2D(-unit.Y(), unit.X());
	Vector2D const base = tip - unit * length;
	Vector2D const polygon[3] = {
		tip,
		base + perp * half_width,
		base - perp * half_width,
	};

	context.SetFillColour(from_wx(colour), alpha);
	context.SetLineColour(from_wx(colour), 0.0f, 1);
	context.DrawPolygon(polygon, 3);
}

void DrawGuideLine(
	VideoOverlayDrawContext& context,
	Vector2D first,
	Vector2D second,
	bool selected,
	VisualGuideOverlayStyle const& style) {
	DrawGuideStroke(
		context, first, second, style.outline_colour, 0.85f, kGuideOutlineWidth);
	DrawGuideStroke(
		context,
		first,
		second,
		selected ? style.highlight_colour : style.line_colour,
		1.0f,
		kGuideCoreWidth);

	Vector2D const direction = second - first;
	if (direction.SquareLen() > kMinStrokeSquareLen) {
		Vector2D const unit = direction.Unit();
		DrawGuideArrowhead(
			context,
			second,
			unit,
			style.outline_colour,
			kArrowOutlineAlpha,
			kArrowCoreLength + kArrowOutlineGrowth,
			kArrowCoreHalfWidth + kArrowOutlineGrowth);
		DrawGuideArrowhead(
			context,
			second,
			unit,
			selected ? style.highlight_colour : style.line_colour,
			1.0f,
			kArrowCoreLength,
			kArrowCoreHalfWidth);
	}
}

std::string TrimFixedNumber(double value, int precision = 2) {
	std::ostringstream stream;
	stream << std::fixed << std::setprecision(precision) << value;
	auto text = stream.str();
	while (!text.empty() && text.back() == '0')
		text.pop_back();
	if (!text.empty() && text.back() == '.')
		text.pop_back();
	return text.empty() || text == "-0" ? "0" : text;
}

/// Format a shear coefficient with up to three decimals. Trailing zeros are
/// stripped and positive values do NOT get a leading '+', matching how raw
/// \fax/\fay values read. The caller passes std::nullopt for the axis-aligned
/// case (denominator zero) to render a dash instead of a number.
std::string FormatVisualGuideShear(std::optional<double> value) {
	if (!value)
		return "-";
	auto const unwrapped = *value;
	if (!std::isfinite(unwrapped) || std::abs(unwrapped) < kDisplayZeroThreshold)
		return "0";
	return TrimFixedNumber(unwrapped, 3);
}

struct MeasurementLabel {
	std::string delta_x;
	std::string delta_y;
	std::string distance;
	std::string shear_x;
	std::string shear_y;
	std::string angle_h;
	std::string angle_v;
};

MeasurementLabel BuildMeasurementLabel(VisualGuideMetrics metrics) {
	auto const shear_x_value = metrics.delta_y == 0.0
		? std::optional<double>{}
		: std::optional<double>{metrics.shear_x};
	auto const shear_y_value = metrics.delta_x == 0.0
		? std::optional<double>{}
		: std::optional<double>{metrics.shear_y};
	return {
		std::string("dX ") + FormatVisualGuideLabelNumber(metrics.delta_x, true),
		std::string("dY ") + FormatVisualGuideLabelNumber(metrics.delta_y, true),
		std::string("L ") + FormatVisualGuideLabelNumber(metrics.distance, false),
		std::string("sx ") + FormatVisualGuideShear(shear_x_value),
		std::string("sy ") + FormatVisualGuideShear(shear_y_value),
		std::string("angH ") + TrimFixedNumber(metrics.angle_horizontal) + " deg",
		std::string("angV ") + TrimFixedNumber(metrics.angle_vertical) + " deg",
	};
}

VideoOverlayTextStyle MakeLabelTextStyle(VisualGuideOverlayStyle const& style) {
	VideoOverlayTextStyle text_style;
	text_style.face = "Verdana";
	text_style.size = std::clamp(style.label_font_size, 6, 72);
	text_style.bold = true;
	// Fixed white text: line_colour is tuned for drawing strokes over video
	// (e.g. the saturated red default) and collapses to ~2:1 contrast on the
	// semi-transparent black label background. White keeps the label legible
	// regardless of the user's Lines Primary colour.
	text_style.colour = {255, 255, 255};
	return text_style;
}
} // close anonymous namespace; label-layout helpers below are file-scope so
  // the exported ComputeMeasurementLabelGeometry can reuse them for hit-testing.

/// Measure the label rows and return the label box size including padding.
/// Exposed so ComputeMeasurementLabelGeometry can share the exact same layout
/// that the draw pass uses, keeping hit-testing in sync.
Vector2D MeasureLabelBox(
	VideoOverlayDrawContext& context,
	MeasurementLabel const& label,
	VideoOverlayTextStyle const& text_style) {
	std::array<std::string_view, 7> const lines = {
		label.delta_x, label.delta_y, label.distance,
		label.shear_x, label.shear_y, label.angle_h, label.angle_v,
	};
	int width = 0;
	int height = 0;
	for (auto const& line : lines) {
		auto const extent = context.MeasureText(std::string(line), text_style);
		width = std::max(width, extent.GetWidth());
		height += extent.GetHeight();
	}
	width += static_cast<int>(kLabelPadding * 2.0);
	height += static_cast<int>(kLabelPadding * 2.0);
	return { static_cast<float>(width), static_cast<float>(height) };
}

/// Resolve the label origin for the second-endpoint anchor with auto-flip and
/// viewport clamping. The box is placed so one of its corners sits a small gap
/// away from the second endpoint on the line's normal side, keeping it clear of
/// the arrowhead rather than centered on top of it. Kept free of the draw
/// context so hit-testing (which only needs the size) reuses the same path.
Vector2D ResolveLabelOrigin(
	Vector2D second,
	Vector2D box_size,
	Vector2D direction,
	VisualGuideViewport const& viewport) {
	Vector2D normal(0.0f, -1.0f);
	if (direction.SquareLen() > 0.0)
		normal = Vector2D(-direction.Y(), direction.X()).Unit();

	auto const width = static_cast<double>(box_size.X());
	auto const height = static_cast<double>(box_size.Y());
	// Anchor corner sits a gap out from the endpoint along the normal; the box
	// extends away from the endpoint along both axes so it never overlaps the
	// arrowhead. The corner chosen follows the normal's sign on each axis.
	auto label_origin = [&](Vector2D offset_normal) {
		Vector2D const anchor = second + offset_normal * static_cast<float>(kLabelOffset);
		float const ox = offset_normal.X() >= 0.0f
			? anchor.X()
			: anchor.X() - static_cast<float>(width);
		float const oy = offset_normal.Y() >= 0.0f
			? anchor.Y()
			: anchor.Y() - static_cast<float>(height);
		return Vector2D(ox, oy);
	};
	Vector2D origin = label_origin(normal);
	auto fits = [&](Vector2D value) {
		return value.X() >= viewport.canvas_x
			&& value.Y() >= viewport.canvas_y
			&& value.X() + width <= viewport.canvas_x + viewport.canvas_width
			&& value.Y() + height <= viewport.canvas_y + viewport.canvas_height;
	};
	if (!fits(origin))
		origin = label_origin(normal * -1.0);

	if (viewport.canvas_width > 0.0 && viewport.canvas_height > 0.0) {
		origin = Vector2D(
			static_cast<float>(std::clamp(
				static_cast<double>(origin.X()),
				viewport.canvas_x,
				std::max(viewport.canvas_x, viewport.canvas_x + viewport.canvas_width - width))),
			static_cast<float>(std::clamp(
				static_cast<double>(origin.Y()),
				viewport.canvas_y,
				std::max(viewport.canvas_y, viewport.canvas_y + viewport.canvas_height - height))));
	}
	return origin;
}

Vector2D ComputeMeasurementLabelSize(
	VideoOverlayDrawContext& context,
	VisualGuide const& guide,
	VisualGuideOverlayStyle const& style) {
	return MeasureLabelBox(
		context, BuildMeasurementLabel(CalculateVisualGuideMetrics(guide)), MakeLabelTextStyle(style));
}

void DrawMeasurementLabel(
	VideoOverlayDrawContext& context,
	VisualGuide const& guide,
	VisualGuideViewport const& viewport,
	Vector2D first,
	Vector2D second,
	bool selected,
	VisualGuideOverlayStyle const& style) {
	auto const metrics = CalculateVisualGuideMetrics(guide);
	auto const label = BuildMeasurementLabel(metrics);
	auto const text_style = MakeLabelTextStyle(style);

	auto const box_size = ComputeMeasurementLabelSize(context, guide, style);
	auto const origin = ResolveLabelOrigin(second, box_size, second - first, viewport);
	auto const width = static_cast<int>(std::lround(box_size.X()));
	auto const height = static_cast<int>(std::lround(box_size.Y()));

	Vector2D const background_end = origin + Vector2D(
		static_cast<float>(width), static_cast<float>(height));
	context.SetFillColour(from_wx(style.label_background_colour), 0.70f);
	wxColour const border = selected ? style.highlight_colour : style.label_border_colour;
	context.SetLineColour(from_wx(border), 0.9f, 1);
	context.DrawRectangle(origin, background_end);

	std::array<std::string_view, 7> const lines = {
		label.delta_x, label.delta_y, label.distance,
		label.shear_x, label.shear_y, label.angle_h, label.angle_v,
	};
	int cursor_y = static_cast<int>(std::lround(origin.Y() + kLabelPadding));
	int const text_x = static_cast<int>(std::lround(origin.X() + kLabelPadding));
	for (auto const& line : lines) {
		context.DrawText(std::string(line), text_x, cursor_y, text_style);
		auto const extent = context.MeasureText(std::string(line), text_style);
		cursor_y += extent.GetHeight();
	}
}

VisualGuideLabelGeometry ComputeMeasurementLabelGeometry(
	VideoOverlayDrawContext& context,
	VisualGuide const& guide,
	VisualGuideViewport const& viewport,
	Vector2D first,
	Vector2D second,
	VisualGuideOverlayStyle const& style) {
	auto const box_size = ComputeMeasurementLabelSize(context, guide, style);
	auto const origin = ResolveLabelOrigin(second, box_size, second - first, viewport);
	return { origin, box_size };
}

bool IsVisualGuideViewportMappable(VisualGuideViewport const& viewport) noexcept {
	return IsFiniteCanvas(viewport) && HasScriptDimensions(viewport);
}

Vector2D VisualGuideToCanvas(
	VisualGuidePoint point,
	VisualGuideViewport const& viewport) {
	if (!IsVisualGuideViewportMappable(viewport)
		|| !IsFinite(point.x) || !IsFinite(point.y))
		return {};

	return {
		static_cast<float>(viewport.canvas_x + point.x * viewport.canvas_width / viewport.script_width),
		static_cast<float>(viewport.canvas_y + point.y * viewport.canvas_height / viewport.script_height),
	};
}

std::optional<VisualGuidePoint> CanvasToVisualGuide(
	Vector2D point,
	VisualGuideViewport const& viewport) {
	if (!IsVisualGuideViewportMappable(viewport)
		|| !IsFinite(point.X()) || !IsFinite(point.Y()))
		return std::nullopt;

	return VisualGuidePoint{
		(point.X() - viewport.canvas_x) * viewport.script_width / viewport.canvas_width,
		(point.Y() - viewport.canvas_y) * viewport.script_height / viewport.canvas_height,
	};
}

std::string FormatVisualGuideLabelNumber(double value, bool show_positive_sign) {
	if (!std::isfinite(value) || std::abs(value) < kDisplayZeroThreshold)
		value = 0.0;

	auto text = TrimFixedNumber(value);
	if (show_positive_sign && value > 0.0)
		text.insert(text.begin(), '+');
	return text;
}

void VisualGuideOverlay::Draw(
	VideoOverlayDrawContext& context,
	VisualGuideViewport const& viewport,
	VisualGuideSnapshotView const& snapshot,
	VisualGuideOverlayStyle const& style) const {
	if (!IsFiniteCanvas(viewport))
		return;

	for (auto const& guide : snapshot.guides) {
		bool const selected = snapshot.selected_id && *snapshot.selected_id == guide.id;
		DrawGuide(context, viewport, guide, selected, style);
	}
}

void VisualGuideOverlay::DrawGuide(
	VideoOverlayDrawContext& context,
	VisualGuideViewport const& viewport,
	VisualGuide const& guide,
	bool selected,
	VisualGuideOverlayStyle const& style) const {
	if (!IsVisualGuideViewportMappable(viewport) || !IsValidVisualGuide(guide))
		return;

	Vector2D const first = VisualGuideToCanvas(guide.first, viewport);
	Vector2D const second = VisualGuideToCanvas(guide.second, viewport);
	DrawGuideLine(context, first, second, selected, style);
	DrawMeasurementLabel(context, guide, viewport, first, second, selected, style);
}
