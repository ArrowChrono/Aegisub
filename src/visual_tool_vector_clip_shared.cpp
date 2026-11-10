#include "visual_tool_vector_clip_shared.h"

#include "video_overlay_draw_context.h"
#include "video_overlay_helpers.h"
#include "visual_feature.h"

#include <libaegisub/split.h>
#include <libaegisub/util.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>

namespace aegisub::visual_vector_clip {
namespace {
bool IsIntegerEncodable(Vector2D value) {
	auto in_range = [](float coordinate) {
		double const truncated = std::trunc(static_cast<double>(coordinate));
		return std::isfinite(coordinate) &&
			   truncated >= std::numeric_limits<int>::min() &&
			   truncated <= std::numeric_limits<int>::max();
	};
	return in_range(value.X()) && in_range(value.Y());
}
}

void AppendFeatures(Spline const& spline, std::size_t curve_index, std::vector<Feature>& features) {
	auto const& curve = spline[curve_index];
	switch (curve.type) {
		case SplineCurve::POINT:
			features.push_back({.curve_index = curve_index, .point = 0, .position = curve.p1});
			break;
		case SplineCurve::LINE:
			features.push_back({.curve_index = curve_index, .point = 1, .position = curve.p2});
			break;
		case SplineCurve::BICUBIC:
			features.push_back({.curve_index = curve_index, .point = 1, .position = curve.p2, .square = true});
			features.push_back({.curve_index = curve_index, .point = 2, .position = curve.p3, .square = true});
			features.push_back({.curve_index = curve_index, .point = 3, .position = curve.p4});
			break;
	}
}

std::vector<Feature> MakeFeatures(Spline const& spline) {
	std::vector<Feature> result;
	result.reserve(spline.size());
	for (std::size_t index = 0; index < spline.size(); ++index)
		AppendFeatures(spline, index, result);
	return result;
}

std::optional<std::size_t> HitTest(std::span<Feature const> features, Vector2D pointer) {
	std::optional<std::size_t> result;
	for (std::size_t index = 0; index < features.size(); ++index) {
		VisualDraggableFeature feature;
		feature.type = features[index].square ? DRAG_SMALL_SQUARE : DRAG_SMALL_CIRCLE;
		feature.pos = features[index].position;
		if (feature.IsMouseOver(pointer))
			result = index;
	}
	return result;
}

bool InBox(Vector2D first, Vector2D second, Vector2D point) {
	auto const minimum = first.Min(second);
	auto const maximum = first.Max(second);
	return point.X() >= minimum.X() && point.X() <= maximum.X() &&
		   point.Y() >= minimum.Y() && point.Y() <= maximum.Y();
}

std::optional<ClosestPoint> FindClosest(Spline const& spline, Vector2D pointer) {
	ClosestPoint result;
	if (!spline.FindClosestParametricPoint(pointer, result.curve_index, result.parameter, result.position, &result.closing))
		return {};
	return result;
}

std::string BuildClipValue(Spline const& spline) {
	std::string result = "(";
	if (spline.GetScale() != 1)
		result += std::to_string(spline.GetScale()) + ",";
	result += spline.EncodeToAss() + ")";
	return result;
}

bool IsEditableDrawing(std::string_view drawing, int scale) {
	if (scale < 1 || scale > 31)
		return false;
	char command = 0;
	std::size_t count = 0;
	std::size_t group_size = 0;
	std::string const text(drawing);
	for (auto const token : agi::Split(text, ' ')) {
		if (token.empty())
			continue;
		if (token.size() == 1 && (token[0] == 'm' || token[0] == 'l' || token[0] == 'b')) {
			if ((!command && token[0] != 'm') || (command && (count == 0 || count % group_size != 0)))
				return false;
			command = token[0];
			group_size = command == 'b' ? 6 : 2;
			count = 0;
		}
		else {
			double number = 0;
			if (!command || !agi::util::try_parse(agi::str(token), &number) ||
				!std::isfinite(number) || std::trunc(number) < std::numeric_limits<int>::min() ||
				std::trunc(number) > std::numeric_limits<int>::max() ||
				!IsIntegerEncodable(Vector2D(static_cast<float>(number), 0.F)))
				return false;
			++count;
		}
	}
	return !command || (count != 0 && count % group_size == 0);
}

bool IsEncodable(Spline const& spline) {
	auto point_encodable = [&spline](Vector2D point) {
		return std::isfinite(point.X()) && std::isfinite(point.Y()) &&
			   IsIntegerEncodable(spline.ToScript(point));
	};
	for (auto const& curve : spline) {
		if (!point_encodable(curve.p1))
			return false;
		if (curve.type != SplineCurve::POINT && !point_encodable(curve.p2))
			return false;
		if (curve.type == SplineCurve::BICUBIC) {
			if (!point_encodable(curve.p3) || !point_encodable(curve.p4))
				return false;
			float const length = (curve.p2 - curve.p1).Len() + (curve.p3 - curve.p2).Len() + (curve.p4 - curve.p3).Len();
			if (!std::isfinite(length) || static_cast<double>(length) > std::numeric_limits<int>::max())
				return false;
		}
	}
	return true;
}

bool RemovePoint(Spline& spline, std::size_t curve_index, int point) {
	if (curve_index >= spline.size())
		return false;
	auto curve = spline.begin() + curve_index;
	if ((curve->type == SplineCurve::POINT && point != 0) ||
		(curve->type == SplineCurve::LINE && point != 1) ||
		(curve->type == SplineCurve::BICUBIC && (point < 1 || point > 3)))
		return false;
	if (curve->type == SplineCurve::BICUBIC && (point == 1 || point == 2)) {
		curve->type = SplineCurve::LINE;
		curve->p2 = curve->p4;
		return true;
	}
	auto const next = std::next(curve);
	if (next != spline.end() && next->type != SplineCurve::POINT) {
		if (curve->type == SplineCurve::POINT) {
			next->p1 = next->EndPoint();
			next->type = SplineCurve::POINT;
		}
		else
			next->p1 = curve->p1;
	}
	spline.erase(curve);
	return true;
}

bool Convert(Spline& spline, ClosestPoint const& closest) {
	if (closest.curve_index >= spline.size())
		return false;
	auto& curve = spline[closest.curve_index];
	if (curve.type == SplineCurve::LINE) {
		curve.type = SplineCurve::BICUBIC;
		curve.p4 = curve.p2;
		curve.p2 = curve.p1 * .75F + curve.p4 * .25F;
		curve.p3 = curve.p1 * .25F + curve.p4 * .75F;
		return true;
	}
	if (curve.type == SplineCurve::BICUBIC) {
		curve.type = SplineCurve::LINE;
		curve.p2 = curve.p4;
		return true;
	}
	return false;
}

bool Insert(Spline& spline, ClosestPoint const& closest) {
	if (closest.closing) {
		auto const [first, last] = *closest.closing;
		if (first > last || last >= spline.size() ||
			spline[first].type != SplineCurve::POINT ||
			(last + 1 < spline.size() && spline[last + 1].type != SplineCurve::POINT))
			return false;
		auto const start = spline[last].EndPoint();
		auto const end = spline[first].p1;
		spline.insert(spline.begin() + last + 1,
					  SplineCurve(start, start * (1 - closest.parameter) + end * closest.parameter));
		return true;
	}
	if (closest.curve_index >= spline.size())
		return false;
	auto curve = spline.begin() + closest.curve_index;
	auto const split = curve->Split(closest.parameter);
	*curve = split.first;
	spline.insert(std::next(curve), split.second);
	return true;
}

void Append(Spline& spline, Mode mode, Vector2D pointer) {
	if (mode != Mode::Line && mode != Mode::Bicubic && mode != Mode::Move)
		throw std::invalid_argument("Vector clip append requires a drawing mode");
	if (spline.empty() || mode == Mode::Move)
		spline.emplace_back(pointer);
	else if (mode == Mode::Line)
		spline.emplace_back(spline.back().EndPoint(), pointer);
	else {
		auto const start = spline.back().EndPoint();
		spline.emplace_back(start, start, start, pointer);
	}
}

void UpdateAppend(Spline& spline, Mode mode, Vector2D pointer) {
	if (spline.empty() || (mode != Mode::Line && mode != Mode::Bicubic))
		throw std::invalid_argument("Vector clip update requires an appended segment");
	auto& curve = spline.back();
	curve.EndPoint() = pointer;
	if (mode == Mode::Bicubic && curve.type == SplineCurve::BICUBIC) {
		auto const& previous = spline[spline.size() - 2];
		Vector2D tangent(0, 0);
		if (previous.type == SplineCurve::LINE)
			tangent = previous.p2 - previous.p1;
		else if (previous.type == SplineCurve::BICUBIC)
			tangent = previous.p4 - previous.p3;
		curve.p2 = curve.p1 + tangent.Unit() * ((curve.p4 - curve.p1).Len() * .25F);
		curve.p3 = curve.p1 * .25F + curve.p4 * .75F;
	}
}

void BeginFreehand(Spline& spline, Vector2D pointer) {
	spline.clear();
	spline.emplace_back(pointer);
}

bool UpdateFreehand(Spline& spline, Mode mode, Vector2D pointer, bool final) {
	if (spline.empty() || (mode != Mode::Freehand && mode != Mode::FreehandSmooth))
		throw std::invalid_argument("Vector clip freehand update requires an active path");
	auto const last = spline.back().EndPoint();
	float const distance = (last - pointer).SquareLen();
	bool changed = false;
	if ((final && distance > 0) || distance >= (mode == Mode::Freehand ? 900.F : 3600.F)) {
		spline.emplace_back(last, pointer);
		changed = true;
	}
	if (final && mode == Mode::FreehandSmooth && spline.size() >= 3) {
		spline.Smooth();
		changed = true;
	}
	return changed;
}

void DrawOverlay(VideoOverlayDrawContext& context, OverlayState const& state) {
	auto const& spline = state.spline;
	if (spline.empty())
		return;
	std::vector<int> starts;
	std::vector<int> counts;
	auto const points = spline.GetPointList(starts, counts);
	context.SetLineColour(state.line_colour, .5F, 2);
	context.SetFillColour(agi::Color(0, 0, 0), state.shaded_alpha);
	context.DrawMultiPolygon(points, starts, counts, state.video_position, state.video_size, !state.inverse);

	if (state.mode == Mode::Drag && state.holding && state.pointer_valid) {
		auto const first = state.drag_start.Min(state.pointer);
		auto const second = state.drag_start.Max(state.pointer);
		video_overlay_helpers::DrawDashedLine(context, first, Vector2D(first.X(), second.Y()), 6);
		video_overlay_helpers::DrawDashedLine(context, Vector2D(first.X(), second.Y()), second, 6);
		video_overlay_helpers::DrawDashedLine(context, second, Vector2D(second.X(), first.Y()), 6);
		video_overlay_helpers::DrawDashedLine(context, Vector2D(second.X(), first.Y()), first, 6);
	}

	bool const has_active = std::ranges::any_of(state.features,
												[](Feature const& feature) { return feature.active; });
	std::optional<ClosestPoint> closest;
	if (state.pointer_valid && (state.mode == Mode::Insert || (state.mode == Mode::Convert && !has_active && points.size() > 2)))
		closest = FindClosest(spline, state.pointer);
	if (closest && !has_active && points.size() > 2) {
		std::vector<float> highlighted;
		if (closest->closing) {
			auto const first = spline[closest->closing->last].EndPoint();
			auto const last = spline[closest->closing->first].p1;
			highlighted = {first.X(), first.Y(), last.X(), last.Y()};
		}
		else if (closest->curve_index < spline.size())
			highlighted = spline.GetPointList(spline.begin() + closest->curve_index);
		if (!highlighted.empty()) {
			context.SetLineColour(state.highlight_secondary, 1.F, 2);
			context.SetFillColour(state.highlight_secondary, 0.F);
			video_overlay_helpers::DrawLineStripFromFloatPoints(context, highlighted);
		}
	}

	context.SetLineColour(state.line_colour, .9F, 1);
	context.SetFillColour(state.line_colour, 0.F);
	for (auto const& curve : spline) {
		if (curve.type == SplineCurve::BICUBIC) {
			video_overlay_helpers::DrawDashedLine(context, curve.p1, curve.p2, 6);
			video_overlay_helpers::DrawDashedLine(context, curve.p3, curve.p4, 6);
		}
	}
	for (auto const& feature : state.features) {
		auto const colour = feature.active ? state.highlight_primary : feature.selected ? state.highlight_secondary
																						: state.line_colour;
		context.SetFillColour(colour, .6F);
		context.SetLineColour(feature.square ? state.line_colour : colour, .5F, 1);
		if (feature.square)
			context.DrawRectangle(feature.position - 3, feature.position + 3);
		else
			context.DrawCircle(feature.position, 3);
	}
	if (state.pointer_valid && (state.mode == Mode::Line || state.mode == Mode::Bicubic)) {
		auto const start = std::ranges::find_if(std::views::reverse(spline),
												[](SplineCurve const& curve) { return curve.type == SplineCurve::POINT; });
		context.SetLineColour(state.line_colour, .9F, 1);
		context.SetFillColour(state.line_colour, 0.F);
		video_overlay_helpers::DrawDashedLine(context, state.pointer, start->p1, 6);
		video_overlay_helpers::DrawDashedLine(context, state.pointer, spline.back().EndPoint(), 6);
	}
	if (state.mode == Mode::Insert && closest) {
		context.SetLineColour(state.line_colour, .5F, 1);
		context.SetFillColour(state.line_colour, 0.F);
		context.DrawCircle(closest->position, 4);
	}
}

}
