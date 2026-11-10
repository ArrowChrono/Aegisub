// Copyright (c) 2011, 2013, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "visual_edit_controller.h"

#include "video_overlay_draw_context.h"
#include "visual_ass_edit.h"
#include "visual_feature.h"
#include "visual_frame_visibility_index.h"
#include "visual_tool_cross_shared.h"
#include "visual_tool_drag_snapshot.h"
#include "visual_tool_transform_policy.h"
#include "visual_tool_scale_policy.h"
#include "visual_tool_transform_overlay.h"
#include "numeric_utils.h"

#include <libaegisub/format.h>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace aegisub::visual_edit {
namespace {
using Key = std::tuple<int, int, size_t>;
constexpr int NoLine = std::numeric_limits<int>::min();

Key MakeKey(VisualToolFeatureKey const& key) {
	return {key.line_id.value_or(NoLine), key.type, key.line_id ? 0 : key.index};
}

bool SameVectorPath(Spline const& first, Spline const& second) {
	return first.GetScale() == second.GetScale() && first.size() == second.size() &&
		   std::ranges::equal(first, second, [](SplineCurve const& left, SplineCurve const& right) {
			   return left.type == right.type && left.p1 == right.p1 && left.p2 == right.p2 &&
					  left.p3 == right.p3 && left.p4 == right.p4;
		   });
}

bool ValidVectorViewport(Viewport const& viewport) {
	auto finite = [](Vector2D point) { return std::isfinite(point.X()) && std::isfinite(point.Y()); };
	return finite(viewport.video_position) && finite(viewport.video_size) && finite(viewport.script_size) &&
		   viewport.video_size.X() > 0 && viewport.video_size.Y() > 0 && viewport.script_size.X() > 0 && viewport.script_size.Y() > 0;
}

class VectorClipSnapshot final : public VisualToolRenderSnapshot {
	public:
	Spline path;
	std::vector<visual_vector_clip::Feature> features;
	std::shared_ptr<int const> source;
	visual_vector_clip::Mode mode = visual_vector_clip::Mode::Drag;
	Viewport viewport;
	Style style;
	std::optional<Vector2D> pointer;
	Vector2D drag_start;
	bool inverse = false;
	bool editable = false;
	bool holding = false;

	using VisualToolRenderSnapshot::VisualToolRenderSnapshot;

	[[nodiscard]] std::optional<VisualToolFeatureKey> HitTest(Vector2D const& point) const override {
		if (editable)
			if (auto index = visual_vector_clip::HitTest(features, point))
				return VisualToolFeatureKey{.type = features[*index].square ? DRAG_SMALL_SQUARE : DRAG_SMALL_CIRCLE, .index = *index};
		return {};
	}

	void Draw(VideoOverlayDrawContext& target) const override {
		visual_vector_clip::DrawOverlay(target, {
													.spline = path,
													.features = features,
													.mode = mode,
													.inverse = inverse,
													.video_position = viewport.video_position,
													.video_size = viewport.video_size,
													.pointer = pointer.value_or(Vector2D{}),
													.pointer_valid = pointer.has_value(),
													.holding = holding,
													.drag_start = drag_start,
													.line_colour = VideoOverlayColourFromRgb(style.line_primary),
													.highlight_primary = VideoOverlayColourFromRgb(style.highlight_primary),
													.highlight_secondary = VideoOverlayColourFromRgb(style.highlight_secondary),
													.shaded_alpha = style.shaded_alpha,
												});
	}
};

class CrossSnapshot final : public VisualToolRenderSnapshot {
	public:
	Vector2D pointer;
	Viewport viewport;
	bool shift = false;
	int font_size = 12;

	using VisualToolRenderSnapshot::VisualToolRenderSnapshot;

	void Draw(VideoOverlayDrawContext& target) const override {
		visual_cross::DrawOverlay(target, pointer, viewport, shift, font_size);
	}
};

class ShearSnapshot final : public VisualToolRenderSnapshot {
	public:
	visual_shear::OverlayState state;
	bool editable = false;

	using VisualToolRenderSnapshot::VisualToolRenderSnapshot;

	[[nodiscard]] std::optional<VisualToolFeatureKey> HitTest(Vector2D const& point) const override {
		if (!editable)
			return {};
		if (auto axis = visual_shear::HitTest(state.geometry, point)) {
			bool const x = *axis == visual_shear::Axis::X;
			return VisualToolFeatureKey{.type = x ? DRAG_BIG_SQUARE : DRAG_BIG_CIRCLE, .index = x ? 0u : 1u};
		}
		return {};
	}

	void Draw(VideoOverlayDrawContext& target) const override {
		visual_shear::DrawOverlay(target, state);
	}
};

class ClipSnapshot final : public VisualToolRenderSnapshot {
	public:
	std::array<Vector2D, 4> points;
	std::array<unsigned long, 4> fills;
	Vector2D video_position;
	Vector2D video_size;
	Style style;
	bool inverse = false;

	using VisualToolRenderSnapshot::VisualToolRenderSnapshot;

	[[nodiscard]] std::optional<VisualToolFeatureKey> HitTest(Vector2D const& point) const override {
		std::optional<VisualToolFeatureKey> hit;
		for (size_t i = 0; i < points.size(); ++i) {
			VisualDraggableFeature marker;
			marker.pos = points[i];
			marker.type = DRAG_SMALL_CIRCLE;
			if (marker.IsMouseOver(point))
				hit = VisualToolFeatureKey{.type = DRAG_SMALL_CIRCLE, .index = i};
		}
		return hit;
	}

	void Draw(VideoOverlayDrawContext& target) const override {
		target.SetLineColour(VideoOverlayColourFromRgb(style.line_secondary), 1, 1);
		for (size_t i = 0; i < points.size(); ++i) {
			target.SetFillColour(VideoOverlayColourFromRgb(fills[i]), 0.3f);
			VisualDraggableFeature marker;
			marker.pos = points[i];
			marker.type = DRAG_SMALL_CIRCLE;
			marker.Draw(target);
		}
		target.SetLineColour(VideoOverlayColourFromRgb(style.line_primary), 1, 2);
		target.SetFillColour(VideoOverlayColourFromRgb(style.line_primary), 0);
		target.DrawRectangle(points.front(), points.back());
		target.SetLineColour(agi::Color{}, 0);
		target.SetFillColour(agi::Color{}, style.shaded_alpha);
		if (inverse)
			target.DrawRectangle(points.front(), points.back());
		else {
			auto minimum = points.front().Min(points.back());
			auto maximum = points.front().Max(points.back());
			auto edge = video_position + video_size;
			target.DrawRectangle(video_position, Vector2D(edge, minimum));
			target.DrawRectangle(Vector2D(video_position, maximum), edge);
			target.DrawRectangle(Vector2D(video_position, minimum), Vector2D(minimum, maximum));
			target.DrawRectangle(Vector2D(maximum, minimum), Vector2D(edge, maximum));
		}
	}
};
bool HasValidXYGeometry(Line const& line, Viewport const& viewport) {
	auto finite = [](Vector2D value) { return std::isfinite(value.X()) && std::isfinite(value.Y()); };
	if (!finite(viewport.video_position) || !finite(viewport.video_size) || !finite(viewport.script_size) || !finite(viewport.layout_size) ||
		viewport.video_size.X() <= 0 || viewport.video_size.Y() <= 0 || viewport.script_size.X() <= 0 || viewport.script_size.Y() <= 0 ||
		!finite(line.origin.value_or(line.position)) || !std::isfinite(line.rotation_x) || !std::isfinite(line.rotation_y) || !std::isfinite(line.rotation_z) ||
		!std::isfinite(line.fax) || !std::isfinite(line.fay))
		return false;
	return finite(line.origin.value_or(line.position) * viewport.video_size / viewport.script_size + viewport.video_position);
}

class TransformSnapshot final : public VisualToolRenderSnapshot {
	public:
	Mode mode = Mode::RotateZ;
	Line line;
	Viewport viewport;
	Style style;
	std::shared_ptr<VideoOverlayPreparedPerspectiveBatches const> xy_geometry;

	using VisualToolRenderSnapshot::VisualToolRenderSnapshot;
	[[nodiscard]] Vector2D ToCanvas(Vector2D point) const { return point * viewport.video_size / viewport.script_size + viewport.video_position; }
	[[nodiscard]] std::optional<VisualToolFeatureKey> HitTest(Vector2D const& point) const override {
		if (mode != Mode::RotateZ && mode != Mode::RotateXY)
			return {};
		VisualDraggableFeature marker;
		marker.type = DRAG_BIG_TRIANGLE;
		marker.pos = ToCanvas(line.origin.value_or(line.position));
		if (marker.IsMouseOver(point))
			return VisualToolFeatureKey{.line_id = line.id, .type = DRAG_BIG_TRIANGLE};
		return {};
	}
	void Draw(VideoOverlayDrawContext& target) const override {
		float const perspective = video_overlay_helpers::GetLayoutResAdjustedPerspectiveZScale(viewport.script_size, viewport.layout_size);
		if (mode == Mode::RotateZ || mode == Mode::RotateXY) {
			VisualDraggableFeature marker;
			marker.type = DRAG_BIG_TRIANGLE;
			marker.pos = ToCanvas(line.origin.value_or(line.position));
			target.SetLineColour(VideoOverlayColourFromRgb(style.line_secondary), 1, 1);
			target.SetFillColour(VideoOverlayColourFromRgb(style.highlight_primary), .3f);
			marker.Draw(target);
			if (mode == Mode::RotateXY) {
				visual_transform::DrawRotateXYOverlay(target, {
																  .origin = marker.pos,
																  .angle_x = line.rotation_x,
																  .angle_y = line.rotation_y,
																  .angle_z = line.rotation_z,
																  .fax = line.fax,
																  .fay = line.fay,
																  .perspective_z_scale = perspective,
																  .primary_colour = VideoOverlayColourFromRgb(style.line_primary),
																  .secondary_colour = VideoOverlayColourFromRgb(style.line_secondary),
																  .feature_fill_colour = VideoOverlayColourFromRgb(style.highlight_primary),
															  },
													  *xy_geometry);
				return;
			}
			visual_transform::DrawRotateZOverlay(target, {
															 .origin = marker.pos,
															 .pos = ToCanvas(line.position),
															 .scale = line.scale,
															 .rotation_x = line.rotation_x,
															 .rotation_y = line.rotation_y,
															 .angle = line.rotation_z,
															 .perspective_z_scale = perspective,
															 .primary_colour = VideoOverlayColourFromRgb(style.line_primary),
															 .secondary_colour = VideoOverlayColourFromRgb(style.line_secondary),
															 .highlight_colour = VideoOverlayColourFromRgb(style.highlight_primary),
														 });
		}
		else
			visual_transform::DrawScaleOverlay(target, {
														   .scale = line.scale,
														   .pos = ToCanvas(line.position),
														   .video_res = viewport.video_size,
														   .rx = line.rotation_x,
														   .ry = line.rotation_y,
														   .rz = line.rotation_z,
														   .perspective_z_scale = perspective,
														   .line_color_primary = style.line_primary,
														   .line_color_secondary = style.line_secondary,
														   .highlight_color = style.highlight_primary,
														   .video_position = viewport.video_position,
													   });
	}
};
}

struct Controller::Impl {
	struct Feature {
		Key key;
		Vector2D position;
		Vector2D start;
		std::optional<Key> parent;
		int time = 0;
		int layer = 0;
	};

	Mode mode;
	std::vector<Line> lines;
	std::unordered_map<int, size_t> line_index;
	std::unordered_map<int, std::string> originals;
	visual_frame_visibility::Index<Line> visibility;
	std::vector<Feature> features;
	std::map<Key, size_t> feature_index;
	std::set<Key> picked;
	std::set<int> selected;
	std::optional<std::set<int>> pressed_click_selection;
	std::optional<std::set<int>> released_click_selection;
	std::optional<int> active;
	std::optional<Key> hovered;
	std::optional<Key> primary;
	std::optional<Vector2D> cross_pointer;
	bool cross_shift = false;
	Frame frame;
	Viewport viewport;
	Vector2D drag_start;
	Vector2D transform_origin;
	Line transform_active_source;
	Line transform_active_preview;
	Vector2D initial_scale;
	Vector2D initial_xy;
	float rotation_anchor = 0;
	float initial_angle = 0;
	bool dragging = false;
	bool holding = false;
	bool prepared = false;
	std::optional<Key> prepared_feature;
	bool selection_changed_on_press = false;
	std::set<int> dirty;
	std::map<int, VisualTextEdit> drafts;
	std::vector<VisualTextEdit> incremental;
	std::vector<VisualTextEdit> cumulative;
	std::map<int, Line> planned_sources;
	mutable visual_transform::RotateXYOverlayGeometryCache xy_geometry;
	visual_shear::Axis shear_axis = visual_shear::Axis::X;
	Spline vector_path;
	Spline vector_baseline;
	std::vector<visual_vector_clip::Feature> vector_features;
	std::vector<visual_vector_clip::Feature> vector_start_features;
	std::set<size_t> vector_picked;
	std::set<size_t> vector_box_initial;
	std::optional<size_t> vector_hovered;
	std::optional<size_t> vector_prepared_feature;
	std::optional<visual_vector_clip::ClosestPoint> vector_prepared_closest;
	std::optional<Vector2D> vector_pointer;
	std::shared_ptr<int const> vector_source = std::make_shared<int const>(0);
	visual_vector_clip::Mode vector_mode = visual_vector_clip::Mode::Drag;
	std::string vector_original_value;
	bool vector_path_valid = false;

	explicit Impl(Mode mode) : mode(mode) {}

	Line& GetLine(int id) { return lines.at(line_index.at(id)); }
	[[nodiscard]] Line const& GetLine(int id) const { return lines.at(line_index.at(id)); }
	Feature& GetFeature(Key const& key) { return features.at(feature_index.at(key)); }
	[[nodiscard]] Feature const& GetFeature(Key const& key) const { return features.at(feature_index.at(key)); }
	[[nodiscard]] Vector2D ToCanvas(Vector2D point) const { return point * viewport.video_size / viewport.script_size + viewport.video_position; }
	[[nodiscard]] Vector2D ToScript(Vector2D point) const { return (point - viewport.video_position) * viewport.script_size / viewport.video_size; }
	[[nodiscard]] bool ActiveVisible() const {
		return active && visibility.Visible().count(const_cast<Line *>(&GetLine(*active))) != 0;
	}

	[[nodiscard]] bool StaticShearSelection() const {
		return std::ranges::all_of(selected, [&](int id) { return GetLine(id).static_shear; });
	}

	[[nodiscard]] std::optional<visual_shear::Geometry> ShearGeometry() const {
		if (!ActiveVisible() || !std::isfinite(viewport.script_size.X()) || !std::isfinite(viewport.script_size.Y()) ||
			viewport.script_size.X() <= 0 || viewport.script_size.Y() <= 0)
			return {};
		auto const& line = GetLine(*active);
		auto geometry = visual_shear::MakeGeometry(ToCanvas(line.position), viewport.video_position, viewport.video_size, {line.fax, line.fay});
		if (!geometry)
			return {};
		auto representable = [](float value, int offset) {
			double const anchor = std::trunc(static_cast<double>(value));
			return std::isfinite(anchor) && anchor >= std::numeric_limits<int>::min() &&
				   anchor <= std::numeric_limits<int>::max() &&
				   anchor + offset >= std::numeric_limits<int>::min() &&
				   anchor + offset <= std::numeric_limits<int>::max();
		};
		if (!representable(geometry->center.X(), -120) || !representable(geometry->center.X(), 110) ||
			!representable(geometry->center.Y(), -126) || !representable(geometry->center.Y(), 24) ||
			!representable(viewport.video_position.X(), 8) || !representable(viewport.video_position.Y(), 8) ||
			!representable(viewport.video_position.Y() + viewport.video_size.Y(), -24))
			return {};
		return geometry;
	}

	void RetireVectorPress() {
		prepared = false;
		vector_prepared_feature.reset();
		vector_prepared_closest.reset();
		vector_source = std::make_shared<int const>(0);
	}

	void RebuildVectorFeatures(bool clear_selection = false) {
		vector_features = visual_vector_clip::MakeFeatures(vector_path);
		if (clear_selection)
			vector_picked.clear();
		else
			std::erase_if(vector_picked, [&](size_t index) { return index >= vector_features.size(); });
		if (vector_hovered && *vector_hovered >= vector_features.size())
			vector_hovered.reset();
	}

	void RebuildVectorPath() {
		RetireVectorPress();
		vector_path_valid = false;
		vector_path = Spline{};
		vector_features.clear();
		vector_picked.clear();
		vector_hovered.reset();
		if (!ActiveVisible() || !ValidVectorViewport(viewport))
			return;
		auto const& clip = GetLine(*active).vector_clip;
		if (!clip.editable)
			return;
		vector_path.SetCoordinateSystem({.video_position = viewport.video_position, .video_size = viewport.video_size, .script_size = viewport.script_size});
		vector_path.SetScale(clip.scale);
		vector_path.DecodeFromAss(clip.drawing);
		vector_path_valid = visual_vector_clip::IsEncodable(vector_path);
		if (vector_path_valid)
			RebuildVectorFeatures();
	}

	[[nodiscard]] bool CanEditVector() const {
		return ActiveVisible() && vector_path_valid && !selected.empty() &&
			   std::ranges::all_of(selected, [&](int id) { return GetLine(id).vector_clip.editable; });
	}

	[[nodiscard]] bool MatchesVectorSnapshot(VectorClipSnapshot const& snapshot) const {
		return snapshot.source == vector_source && snapshot.mode == vector_mode &&
			   snapshot.viewport.video_position == viewport.video_position && snapshot.viewport.video_size == viewport.video_size &&
			   snapshot.viewport.script_size == viewport.script_size && SameVectorPath(snapshot.path, vector_path);
	}

	bool WriteVectorPath(Spline candidate) {
		if (!visual_vector_clip::IsEncodable(candidate))
			return false;
		bool const geometry_changed = !SameVectorPath(vector_path, candidate);
		auto const value = visual_vector_clip::BuildClipValue(candidate);
		bool const original = value == vector_original_value;
		std::string const drawing = original ? std::string{} : candidate.EncodeToAss();
		std::vector<Line> replacements;
		replacements.reserve(selected.size());
		for (int id : selected) {
			auto replacement = planned_sources.at(id);
			if (!original) {
				replacement.text = SetOverride(replacement.text, replacement.vector_clip.inverse ? "\\iclip" : "\\clip", value);
				replacement.vector_clip.drawing = drawing;
				replacement.vector_clip.scale = candidate.GetScale();
			}
			replacements.push_back(std::move(replacement));
		}
		for (auto& replacement : replacements) {
			auto& current = GetLine(replacement.id);
			if (current.text != replacement.text)
				dirty.insert(replacement.id);
			current = std::move(replacement);
		}
		vector_path = std::move(candidate);
		RebuildVectorFeatures();
		return geometry_changed || !dirty.empty();
	}

	Result BeginVectorPress() {
		using VectorMode = visual_vector_clip::Mode;
		if (!prepared)
			return {};
		prepared = false;
		if (!CanEditVector())
			return {};
		planned_sources.clear();
		CapturePlannedSources();
		vector_baseline = vector_path;
		vector_start_features = vector_features;
		vector_original_value = visual_vector_clip::BuildClipValue(vector_path);
		if (vector_prepared_feature) {
			if (vector_mode == VectorMode::Remove) {
				auto candidate = vector_path;
				auto const& feature = vector_features.at(*vector_prepared_feature);
				if (!visual_vector_clip::RemovePoint(candidate, feature.curve_index, feature.point) || !WriteVectorPath(std::move(candidate)))
					return {};
				vector_picked.clear();
				vector_hovered.reset();
				return {.interaction_began = true, .interaction_ended = true, .geometry_changed = true};
			}
			dragging = true;
			return {.capture = CaptureAction::Capture, .interaction_began = true, .geometry_changed = true};
		}
		if (vector_mode == VectorMode::Drag) {
			vector_box_initial = vector_picked;
			holding = true;
			return {.capture = CaptureAction::Capture, .interaction_began = true, .geometry_changed = true};
		}
		auto candidate = vector_path;
		bool discrete = false;
		if (vector_mode == VectorMode::Convert || vector_mode == VectorMode::Insert) {
			if (!vector_prepared_closest)
				return {};
			bool const changed = vector_mode == VectorMode::Convert ? visual_vector_clip::Convert(candidate, *vector_prepared_closest)
																	: visual_vector_clip::Insert(candidate, *vector_prepared_closest);
			if (!changed)
				return {};
			discrete = true;
		}
		else if (vector_mode == VectorMode::Line || vector_mode == VectorMode::Bicubic || vector_mode == VectorMode::Move) {
			visual_vector_clip::Append(candidate, vector_mode, drag_start);
			discrete = vector_mode == VectorMode::Move;
			if (!discrete)
				visual_vector_clip::UpdateAppend(candidate, vector_mode, drag_start);
		}
		else if (vector_mode == VectorMode::Freehand || vector_mode == VectorMode::FreehandSmooth)
			visual_vector_clip::BeginFreehand(candidate, drag_start);
		else
			return {};
		if (!visual_vector_clip::IsEncodable(candidate))
			return {};
		WriteVectorPath(std::move(candidate));
		vector_picked.clear();
		vector_hovered.reset();
		holding = !discrete;
		return {.capture = discrete ? CaptureAction::None : CaptureAction::Capture, .interaction_began = true, .interaction_ended = discrete, .geometry_changed = true};
	}

	bool UpdateVectorHold(Vector2D position, bool final) {
		using VectorMode = visual_vector_clip::Mode;
		if (vector_mode == VectorMode::Drag) {
			auto picked = vector_box_initial;
			for (size_t i = 0; i < vector_features.size(); ++i)
				if (visual_vector_clip::InBox(drag_start, position, vector_features[i].position))
					picked.insert(i);
			bool const changed = picked != vector_picked || vector_pointer != position;
			vector_picked = std::move(picked);
			vector_pointer = position;
			return changed;
		}
		auto candidate = vector_path;
		if (vector_mode == VectorMode::Line || vector_mode == VectorMode::Bicubic)
			visual_vector_clip::UpdateAppend(candidate, vector_mode, position);
		else if (vector_mode == VectorMode::Freehand || vector_mode == VectorMode::FreehandSmooth)
			visual_vector_clip::UpdateFreehand(candidate, vector_mode, position, final);
		if (!visual_vector_clip::IsEncodable(candidate))
			return false;
		bool const changed = WriteVectorPath(std::move(candidate));
		vector_pointer = position;
		return changed;
	}

	Result VectorPointer(PointerInput const& input, VisualToolRenderSnapshot const *displayed, bool defer_begin) {
		using VectorMode = visual_vector_clip::Mode;
		Result result;
		if (input.phase == PointerPhase::Leave) {
			if (!dragging && !holding) {
				result.geometry_changed = vector_pointer.has_value() || vector_hovered.has_value();
				vector_pointer.reset();
				vector_hovered.reset();
			}
			return result;
		}
		bool const finite = std::isfinite(input.position.X()) && std::isfinite(input.position.Y());
		if (dragging || holding) {
			bool const final = !input.left_down;
			if (finite) {
				if (dragging) {
					auto candidate = vector_baseline;
					auto delta = input.position - drag_start;
					if (input.shift)
						delta = delta.SingleAxis();
					for (size_t index : vector_picked) {
						auto const& feature = vector_start_features.at(index);
						candidate.MovePoint(candidate.begin() + feature.curve_index, feature.point, feature.position + delta);
					}
					if (visual_vector_clip::IsEncodable(candidate)) {
						result.geometry_changed = WriteVectorPath(std::move(candidate));
						vector_pointer = input.position;
					}
				}
				else
					result.geometry_changed = UpdateVectorHold(input.position, final);
			}
			if (final) {
				if (dragging && SameVectorPath(vector_path, vector_baseline) && !selection_changed_on_press && vector_prepared_feature) {
					if (input.control)
						vector_picked.erase(*vector_prepared_feature);
					else
						vector_picked = {*vector_prepared_feature};
					result.geometry_changed = true;
				}
				if (holding && (vector_mode == VectorMode::Freehand || vector_mode == VectorMode::FreehandSmooth)) {
					if (vector_pointer && (!finite || *vector_pointer != input.position))
						result.geometry_changed |= UpdateVectorHold(*vector_pointer, true);
					vector_mode = VectorMode::Drag;
					result.toolbar_changed = true;
				}
				dragging = holding = false;
				vector_hovered.reset();
				result.capture = CaptureAction::Release;
				result.interaction_ended = true;
			}
			return result;
		}
		if (input.phase == PointerPhase::Release || input.phase == PointerPhase::DoubleClick || !finite || !vector_path_valid)
			return result;
		auto const snapshot = dynamic_cast<VectorClipSnapshot const *>(displayed);
		if (input.phase == PointerPhase::Press &&
			(!CanEditVector() || (displayed && (!snapshot || !MatchesVectorSnapshot(*snapshot)))))
			return result;
		auto const& hit_features = snapshot ? snapshot->features : vector_features;
		auto const hit = visual_vector_clip::HitTest(hit_features, input.position);
		result.geometry_changed = vector_hovered != hit || vector_pointer != input.position;
		vector_hovered = hit;
		vector_pointer = input.position;
		if (input.phase != PointerPhase::Press)
			return result;
		drag_start = input.position;
		vector_prepared_feature = hit;
		vector_prepared_closest.reset();
		if (hit) {
			selection_changed_on_press = !vector_picked.contains(*hit);
			if (selection_changed_on_press) {
				if (!input.control)
					vector_picked.clear();
				vector_picked.insert(*hit);
			}
		}
		else {
			if (!input.control && !input.alt)
				vector_picked.clear();
			if (vector_mode == VectorMode::Convert || vector_mode == VectorMode::Insert)
				vector_prepared_closest = visual_vector_clip::FindClosest(snapshot ? snapshot->path : vector_path, input.position);
		}
		prepared = true;
		if (!defer_begin) {
			auto begin = BeginVectorPress();
			result.capture = begin.capture;
			result.interaction_began = begin.interaction_began;
			result.interaction_ended = begin.interaction_ended;
			result.geometry_changed |= begin.geometry_changed;
		}
		return result;
	}

	void ReindexFeatures() {
		feature_index.clear();
		for (size_t i = 0; i < features.size(); ++i)
			feature_index.emplace(features[i].key, i);
		std::erase_if(picked, [&](auto const& key) { return !feature_index.contains(key); });
		if (hovered && !feature_index.contains(*hovered))
			hovered.reset();
		if (primary && !feature_index.contains(*primary))
			primary.reset();
	}

	void AddLineFeatures(Line const& line) {
		Key start{line.id, DRAG_BIG_SQUARE, 0};
		Key end{line.id, DRAG_BIG_CIRCLE, 0};
		features.push_back({.key = start, .position = ToCanvas(line.position), .start = {}, .parent = line.move ? std::optional<Key>(end) : std::nullopt, .time = line.move ? line.move->start_ms : 0, .layer = 0});
		if (selected.contains(line.id))
			picked.insert(start);
		if (line.move)
			features.push_back({.key = end, .position = ToCanvas(line.move->end), .start = {}, .parent = start, .time = line.move->end_ms, .layer = 1});
		if (line.origin)
			features.push_back({.key = {line.id, DRAG_BIG_TRIANGLE, 0}, .position = ToCanvas(*line.origin), .start = {}, .parent = start, .time = 0, .layer = -1});
	}

	void SetClipFeatures(Vector2D first, Vector2D second) {
		std::array<Vector2D, 4> positions{first, Vector2D(second, first), Vector2D(first, second), second};
		if (features.empty()) {
			for (size_t i = 0; i < positions.size(); ++i)
				features.push_back({.key = {NoLine, DRAG_SMALL_CIRCLE, i}, .position = positions[i], .start = {}, .parent = {}, .time = 0, .layer = 0});
			ReindexFeatures();
		}
		else
			for (size_t i = 0; i < positions.size(); ++i)
				features[i].position = positions[i];
	}

	void RebuildFeatures() {
		features.clear();
		picked.clear();
		if (mode == Mode::VectorClip)
			RebuildVectorPath();
		else if (mode == Mode::Drag) {
			for (auto const& line : lines)
				if (visibility.Visible().contains(const_cast<Line *>(&line)))
					AddLineFeatures(line);
		}
		else if (mode == Mode::RectClip && ActiveVisible()) {
			auto const& clip = GetLine(*active).clip;
			SetClipFeatures(ToCanvas(clip.first), ToCanvas(clip.second));
		}
		ReindexFeatures();
	}

	void SetSelection(std::set<int> value, std::optional<int> active_line) {
		if (selected == value && active == active_line)
			return;
		if (mode == Mode::VectorClip) {
			if (dragging || holding)
				throw std::logic_error("Finish vector interaction before changing selection");
			RetireVectorPress();
		}
		for (auto const& feature : features) {
			auto id = std::get<0>(feature.key);
			if (selected.contains(id) && !value.contains(id))
				picked.erase(feature.key);
			else if (!selected.contains(id) && value.contains(id) && std::get<1>(feature.key) == DRAG_BIG_SQUARE)
				picked.insert(feature.key);
		}
		bool active_changed = active != active_line;
		selected = std::move(value);
		active = active_line;
		if (active_changed && mode != Mode::Drag)
			RebuildFeatures();
	}

	void RetireClickSequence() {
		pressed_click_selection.reset();
		released_click_selection.reset();
	}

	void BeginOperation() {
		incremental.clear();
		dirty.clear();
	}
	void Write(Line& line, std::string const& tag, std::string const& value) {
		auto text = SetOverride(line.text, tag, value);
		if (text != line.text) {
			line.text = std::move(text);
			dirty.insert(line.id);
		}
	}
	void EndOperation() {
		if (dirty.empty())
			return;
		for (int id : dirty) {
			auto const& line = GetLine(id);
			VisualTextEdit edit{.id = id, .row = line.row, .original_text = originals.at(id), .text = line.text};
			incremental.push_back(edit);
			if (edit.text == edit.original_text)
				drafts.erase(id);
			else
				drafts.insert_or_assign(id, std::move(edit));
		}
		auto row_less = [](auto const& left, auto const& right) { return left.row < right.row; };
		std::ranges::sort(incremental, row_less);
		cumulative.clear();
		cumulative.reserve(drafts.size());
		for (auto const& [id, edit] : drafts)
			cumulative.push_back(edit);
		std::ranges::sort(cumulative, row_less);
	}

	Result ApplyCommand(std::vector<Line> changes, OverridePlanner const& planner = {}) {
		BeginOperation();
		std::vector<OverrideEdit> writes;
		if (planner)
			writes.reserve(changes.size() * 2);
		for (auto& candidate : changes) {
			auto const& current = GetLine(candidate.id);
			auto const& source = planned_sources.try_emplace(candidate.id, current).first->second;
			candidate.text = source.text;
			auto const write_start = writes.size();
			auto write = [&](char const *tag, std::string const& value) {
				if (planner)
					writes.push_back({.id = candidate.id, .tag = tag, .value = value, .intent = std::string_view(tag) == "\\org" ? OverrideIntent::Origin : mode == Mode::RotateXY ? OverrideIntent::Perspective
																																												   : OverrideIntent::Affine});
				else
					candidate.text = SetOverride(candidate.text, tag, value);
			};
			if (mode == Mode::RotateZ) {
				if (candidate.rotation_z != source.rotation_z) {
					auto value = agi::format("%.4g", candidate.rotation_z);
					write("\\frz", value);
					candidate.rotation_z = std::stof(value);
				}
				if (candidate.origin.value_or(candidate.position) != source.origin.value_or(source.position))
					write("\\org", candidate.origin->PStr());
				else
					candidate.origin = source.origin;
			}
			else if (mode == Mode::RotateXY) {
				if (candidate.rotation_x != source.rotation_x) {
					auto value = agi::format("%.4g", candidate.rotation_x);
					write("\\frx", value);
					candidate.rotation_x = std::stof(value);
				}
				if (candidate.rotation_y != source.rotation_y) {
					auto value = agi::format("%.4g", candidate.rotation_y);
					write("\\fry", value);
					candidate.rotation_y = std::stof(value);
				}
			}
			else if (mode == Mode::Shear) {
				if (candidate.fax != source.fax) {
					auto value = visual_shear::FormatValue(candidate.fax);
					write("\\fax", value);
					candidate.fax = std::stof(value);
				}
				if (candidate.fay != source.fay) {
					auto value = visual_shear::FormatValue(candidate.fay);
					write("\\fay", value);
					candidate.fay = std::stof(value);
				}
			}
			else if (candidate.scale != source.scale) {
				auto const x = float_to_string(candidate.scale.X());
				auto const y = float_to_string(candidate.scale.Y());
				write("\\fscx", x);
				write("\\fscy", y);
				candidate.scale = {std::stof(x), std::stof(y)};
			}
			if (planner && writes.size() == write_start)
				writes.push_back({.id = candidate.id, .tag = {}, .value = {}, .intent = OverrideIntent::Affine, .restore_source = true});
			if (!planner && candidate.text != current.text)
				dirty.insert(candidate.id);
		}
		if (planner) {
			if (!ApplyPlannedOverrides(writes, planner, changes))
				return {};
		}
		else
			for (auto& candidate : changes)
				GetLine(candidate.id) = std::move(candidate);
		EndOperation();
		return {.geometry_changed = !incremental.empty(), .toolbar_changed = !incremental.empty()};
	}

	void SelectFeature(Key key, bool clear, Result& result) {
		if (clear)
			picked.clear();
		picked.insert(key);
		int id = std::get<0>(key);
		if (id == NoLine)
			return;
		auto new_selection = clear ? std::set<int>{id} : selected;
		new_selection.insert(id);
		if (new_selection != selected) {
			selected = std::move(new_selection);
			result.selection_changed = true;
		}
	}
	void RemoveFeature(Key key, Result& result) {
		if (!picked.erase(key))
			return;
		int id = std::get<0>(key);
		if (id == NoLine || selected.size() <= 1)
			return;
		for (auto const& feature : picked)
			if (std::get<0>(feature) == id)
				return;
		selected.erase(id);
		if (active == id)
			active = *selected.begin();
		result.selection_changed = true;
	}

	std::optional<Key> HitTest(Vector2D point, VisualToolRenderSnapshot const *displayed) const {
		if (displayed) {
			auto hit = displayed->HitTest(point);
			if (!hit)
				return {};
			auto key = MakeKey(*hit);
			return feature_index.contains(key) ? std::optional<Key>(key) : std::nullopt;
		}
		int max_layer = std::numeric_limits<int>::min();
		std::optional<Key> hit;
		for (auto const& feature : features) {
			VisualDraggableFeature marker;
			marker.type = static_cast<DraggableFeatureType>(std::get<1>(feature.key));
			marker.pos = feature.position;
			if (feature.layer >= max_layer && marker.IsMouseOver(point)) {
				hit = feature.key;
				max_layer = feature.layer;
			}
		}
		return hit;
	}

	void CapturePlannedSources() {
		for (int id : selected)
			planned_sources.try_emplace(id, GetLine(id));
	}

	bool ApplyPlannedOverrides(std::span<OverrideEdit const> writes, OverridePlanner const& planner, std::span<Line const> candidates = {}) {
		if (writes.empty())
			return true;
		auto planned = planner(writes);
		if (!planned)
			return false;
		std::set<int> expected;
		for (auto const& write : writes)
			expected.insert(write.id);
		std::set<int> received;
		for (auto const& edit : *planned) {
			if (!expected.contains(edit.id) || !received.insert(edit.id).second || edit.row != GetLine(edit.id).row || edit.original_text != originals.at(edit.id))
				throw std::logic_error("Visual override planner returned an inconsistent batch");
		}
		if (received != expected)
			throw std::logic_error("Visual override planner omitted a selected line");
		std::vector<Line> projections;
		projections.reserve(planned->size());
		for (auto const& edit : *planned) {
			auto candidate = std::ranges::find(candidates, edit.id, &Line::id);
			auto projected = candidate == candidates.end() ? planned_sources.at(edit.id) : *candidate;
			if (edit.text != planned_sources.at(edit.id).text) {
				projected.text = edit.text;
				RefreshGeometryFromText(projected, viewport.script_size);
				if (std::ranges::none_of(writes, [&](auto const& write) { return write.id == edit.id && (write.tag == "\\pos" || write.tag == "\\move"); }))
					projected.position = planned_sources.at(edit.id).position;
			}
			else
				projected = planned_sources.at(edit.id);
			projections.push_back(std::move(projected));
		}
		for (auto& projected : projections) {
			auto& line = GetLine(projected.id);
			if (line.text != projected.text)
				dirty.insert(projected.id);
			line = std::move(projected);
			if (mode != Mode::Drag)
				continue;
			for (auto& feature : features) {
				if (std::get<0>(feature.key) != line.id)
					continue;
				switch (std::get<1>(feature.key)) {
					case DRAG_BIG_SQUARE: feature.position = ToCanvas(line.position); break;
					case DRAG_BIG_CIRCLE: feature.position = ToCanvas(line.move.value().end); break;
					case DRAG_BIG_TRIANGLE: feature.position = ToCanvas(line.origin.value()); break;
					default: throw std::logic_error("Unexpected feature in planned drag");
				}
			}
		}
		return true;
	}

	bool WriteDrag(OverridePlanner const& planner) {
		std::set<int> moved;
		for (auto const& key : picked)
			moved.insert(std::get<0>(key));
		if (planner) {
			std::vector<OverrideEdit> writes;
			for (int id : moved) {
				auto const& line = GetLine(id);
				Key start_key{id, DRAG_BIG_SQUARE, 0};
				Key end_key{id, DRAG_BIG_CIRCLE, 0};
				Key origin_key{id, DRAG_BIG_TRIANGLE, 0};
				if (picked.contains(start_key) || picked.contains(end_key)) {
					auto const& start = GetFeature(start_key);
					if (line.move) {
						auto const& end = GetFeature(end_key);
						writes.push_back({.id = id, .tag = "\\move", .value = agi::format("(%s,%s,%d,%d)", ToScript(start.position).Str(), ToScript(end.position).Str(), start.time, end.time), .intent = OverrideIntent::Endpoint});
					}
					else
						writes.push_back({.id = id, .tag = "\\pos", .value = ToScript(start.position).PStr(), .intent = OverrideIntent::Translation, .translation = ToScript(start.position) - planned_sources.at(id).position});
				}
				if (picked.contains(origin_key))
					writes.push_back({.id = id, .tag = "\\org", .value = ToScript(GetFeature(origin_key).position).PStr(), .intent = OverrideIntent::Origin});
			}
			return ApplyPlannedOverrides(writes, planner);
		}
		for (int id : moved) {
			auto& line = GetLine(id);
			Key start_key{id, DRAG_BIG_SQUARE, 0};
			Key end_key{id, DRAG_BIG_CIRCLE, 0};
			Key origin_key{id, DRAG_BIG_TRIANGLE, 0};
			if (picked.contains(start_key) || picked.contains(end_key)) {
				auto const& start = GetFeature(start_key);
				line.position = ToScript(start.position);
				if (line.move) {
					auto const& end = GetFeature(end_key);
					line.move = Move{.start = line.position, .end = ToScript(end.position), .start_ms = start.time, .end_ms = end.time};
					Write(line, "\\move", agi::format("(%s,%s,%d,%d)", line.move->start.Str(), line.move->end.Str(), start.time, end.time));
				}
				else
					Write(line, "\\pos", line.position.PStr());
			}
			if (picked.contains(origin_key)) {
				line.origin = ToScript(GetFeature(origin_key).position);
				Write(line, "\\org", line.origin->PStr());
			}
		}
		return true;
	}

	void WriteClip() {
		auto first = ToScript(features.front().position.Min(features.back().position));
		auto second = ToScript(features.front().position.Max(features.back().position));
		auto value = agi::format("(%s,%s)", first.Str(), second.Str());
		for (int id : selected) {
			auto& line = GetLine(id);
			bool inverse = line.clip.inverse;
			line.clip = {.first = first, .second = second, .inverse = inverse};
			Write(line, inverse ? "\\iclip" : "\\clip", value);
		}
	}

	void MoveFeatures(Vector2D position, bool shift, OverridePlanner const& planner) {
		auto delta = position - drag_start;
		if (shift)
			delta = delta.SingleAxis();
		std::vector<Vector2D> previous_positions;
		if (planner) {
			previous_positions.reserve(picked.size());
			for (auto const& key : picked)
				previous_positions.push_back(GetFeature(key).position);
		}
		for (auto const& key : picked) {
			auto& feature = GetFeature(key);
			feature.position = feature.start + delta;
		}
		if (mode == Mode::Drag) {
			if (!WriteDrag(planner)) {
				size_t i = 0;
				for (auto const& key : picked)
					GetFeature(key).position = previous_positions.at(i++);
			}
		}
		else {
			for (auto const& key : picked) {
				auto i = std::get<2>(key);
				features[i ^ 1].position = Vector2D(features[i ^ 1].position, features[i].position);
				features[i ^ 2].position = Vector2D(features[i].position, features[i ^ 2].position);
			}
			WriteClip();
		}
	}

	void MoveHold(Vector2D position) {
		auto edge = viewport.video_position + viewport.video_size;
		SetClipFeatures(viewport.video_position.Max(edge.Min(drag_start)), viewport.video_position.Max(edge.Min(position)));
		WriteClip();
	}

	[[nodiscard]] bool IsTransform() const { return mode == Mode::RotateZ || mode == Mode::RotateXY || mode == Mode::Scale; }

	Result ShearPointer(PointerInput const& input, VisualToolRenderSnapshot const *displayed, bool defer_begin) {
		Result result;
		if (input.phase == PointerPhase::Leave) {
			if (!dragging) {
				result.geometry_changed = hovered.has_value();
				hovered.reset();
			}
			return result;
		}
		bool const finite = std::isfinite(input.position.X()) && std::isfinite(input.position.Y());
		if (!dragging) {
			if (!finite)
				return result;
			auto const geometry = ShearGeometry();
			std::optional<Key> hit;
			if (geometry && StaticShearSelection()) {
				if (displayed) {
					auto key = displayed->HitTest(input.position);
					if (key && !key->line_id &&
						((key->index == 0 && key->type == DRAG_BIG_SQUARE) ||
						 (key->index == 1 && key->type == DRAG_BIG_CIRCLE)))
						hit = MakeKey(*key);
				}
				else if (auto axis = visual_shear::HitTest(*geometry, input.position)) {
					bool const x = *axis == visual_shear::Axis::X;
					hit = Key{NoLine, x ? DRAG_BIG_SQUARE : DRAG_BIG_CIRCLE, x ? 0u : 1u};
				}
			}
			result.geometry_changed = hovered != hit;
			hovered = hit;
			if (input.phase != PointerPhase::Press || !hit || selected.empty())
				return result;
			drag_start = input.position;
			prepared_feature = hit;
			shear_axis = std::get<2>(*hit) == 0 ? visual_shear::Axis::X : visual_shear::Axis::Y;
			prepared = true;
			if (!defer_begin)
				return BeginPress();
			return result;
		}
		if (input.phase != PointerPhase::Move && input.phase != PointerPhase::Release)
			return result;
		if (finite) {
			float const delta = visual_shear::DragDelta(shear_axis, input.position - drag_start);
			std::vector<Line> changes;
			changes.reserve(selected.size());
			bool valid = true;
			for (int id : selected) {
				auto candidate = planned_sources.at(id);
				auto& value = shear_axis == visual_shear::Axis::X ? candidate.fax : candidate.fay;
				auto adjusted = visual_shear::FormatAdjustedValue(value, delta);
				if (!adjusted) {
					valid = false;
					break;
				}
				value += delta;
				changes.push_back(std::move(candidate));
			}
			if (valid)
				result = ApplyCommand(std::move(changes));
		}
		if (!input.left_down) {
			dragging = false;
			result.capture = CaptureAction::Release;
			result.interaction_ended = true;
		}
		return result;
	}

	Result TransformPointer(PointerInput const& input, VisualToolRenderSnapshot const *displayed, bool defer_begin, OverridePlanner const& planner) {
		Result result;
		if (input.phase == PointerPhase::Leave)
			return result;
		bool const pointer_valid = mode != Mode::RotateXY || (std::isfinite(input.position.X()) && std::isfinite(input.position.Y()));
		if (!dragging && !holding) {
			if (input.phase != PointerPhase::Press || !pointer_valid || !ActiveVisible() || selected.empty())
				return result;
			drag_start = input.position;
			auto const& line = GetLine(*active);
			if (mode == Mode::RotateXY && !HasValidXYGeometry(line, viewport))
				return result;
			transform_active_source = transform_active_preview = line;
			transform_origin = ToCanvas(line.origin.value_or(line.position));
			initial_angle = line.rotation_z;
			initial_xy = {line.rotation_x, line.rotation_y};
			rotation_anchor = visual_transform::RotationAnchor(initial_angle, transform_origin, input.position);
			initial_scale = line.scale;
			prepared_feature.reset();
			if (mode == Mode::RotateZ || mode == Mode::RotateXY) {
				if (displayed) {
					if (auto hit = displayed->HitTest(input.position); hit && hit->line_id == active && hit->type == DRAG_BIG_TRIANGLE)
						prepared_feature = MakeKey(*hit);
				}
				else if (mode == Mode::RotateZ) {
					VisualDraggableFeature marker;
					marker.type = DRAG_BIG_TRIANGLE;
					marker.pos = transform_origin;
					if (marker.IsMouseOver(input.position))
						prepared_feature = Key{*active, DRAG_BIG_TRIANGLE, 0};
				}
			}
			if (mode == Mode::RotateXY && prepared_feature && std::ranges::any_of(selected, [&](int id) {
					auto const origin = GetLine(id).origin.value_or(GetLine(id).position);
					return !std::isfinite(origin.X()) || !std::isfinite(origin.Y());
				}))
				return result;
			prepared = true;
			if (!defer_begin)
				return BeginPress();
			return result;
		}
		if (pointer_valid && (input.position != drag_start || !drafts.empty())) {
			auto delta = ToScript(input.position) - ToScript(drag_start);
			if (input.shift && dragging)
				delta = (input.position - drag_start).SingleAxis() * viewport.script_size / viewport.video_size;
			bool valid_update = mode != Mode::RotateXY || (std::isfinite(delta.X()) && std::isfinite(delta.Y()));
			float const angle = mode == Mode::RotateXY ? initial_angle : visual_transform::RotateZ(rotation_anchor, transform_origin, input.position, input.control);
			std::optional<Vector2D> xy = initial_xy;
			if (mode == Mode::RotateXY && !dragging && (input.position != drag_start || input.control))
				xy = visual_transform::RotateXY(initial_xy, input.position - drag_start, input.shift, input.control);
			auto const scale = mode == Mode::Scale ? visual_tool_scale_policy::ScaleDrag(initial_scale, input.position - drag_start, input.shift, input.alt, input.control) : std::optional<Vector2D>(initial_scale);
			valid_update = valid_update && scale.has_value() && xy.has_value();
			if (mode == Mode::RotateXY && dragging && valid_update) {
				auto candidate = transform_active_source;
				candidate.origin = candidate.origin.value_or(candidate.position) + delta;
				valid_update = HasValidXYGeometry(candidate, viewport) && std::ranges::all_of(selected, [&](int id) {
								   auto const& source = planned_sources.at(id);
								   auto const origin = source.origin.value_or(source.position) + delta;
								   return std::isfinite(origin.X()) && std::isfinite(origin.Y());
							   });
			}
			if (valid_update) {
				bool const restore_source = planner && input.position == drag_start;
				auto active_preview = transform_active_source;
				if (!restore_source && dragging && delta != Vector2D(0, 0))
					active_preview.origin = transform_active_source.origin.value_or(transform_active_source.position) + delta;
				else if (!restore_source && mode == Mode::RotateZ && !dragging)
					active_preview.rotation_z = angle;
				else if (!restore_source && mode == Mode::RotateXY && !dragging) {
					active_preview.rotation_x = xy->X();
					active_preview.rotation_y = xy->Y();
				}
				else if (!restore_source && mode == Mode::Scale)
					active_preview.scale = *scale;
				std::vector<Line> changes;
				std::vector<OverrideEdit> writes;
				changes.reserve(selected.size());
				if (planner)
					writes.reserve(selected.size() * 2);
				for (int id : selected) {
					auto const& source = planned_sources.at(id);
					auto line = source;
					auto const write_start = writes.size();
					auto write = [&](char const *tag, std::string const& value, OverrideIntent intent) {
						if (planner)
							writes.push_back({.id = id, .tag = tag, .value = value, .intent = intent});
						else
							line.text = SetOverride(line.text, tag, value);
					};
					if (!restore_source && dragging && delta != Vector2D(0, 0)) {
						line.origin = source.origin.value_or(source.position) + delta;
						write("\\org", line.origin->PStr(), OverrideIntent::Origin);
					}
					else if (!restore_source && mode == Mode::RotateZ && !dragging && angle != initial_angle) {
						line.rotation_z = angle;
						write("\\frz", agi::format("%.4g", angle), OverrideIntent::Affine);
					}
					else if (!restore_source && mode == Mode::RotateXY && !dragging && *xy != initial_xy) {
						line.rotation_x = xy->X();
						line.rotation_y = xy->Y();
						write("\\frx", agi::format("%.4g", xy->X()), OverrideIntent::Perspective);
						write("\\fry", agi::format("%.4g", xy->Y()), OverrideIntent::Perspective);
					}
					else if (!restore_source && mode == Mode::Scale && *scale != initial_scale) {
						line.scale = *scale;
						write("\\fscx", std::to_string(static_cast<int>(scale->X())), OverrideIntent::Affine);
						write("\\fscy", std::to_string(static_cast<int>(scale->Y())), OverrideIntent::Affine);
					}
					if (planner && writes.size() == write_start)
						writes.push_back({.id = id, .tag = {}, .value = {}, .intent = OverrideIntent::Affine, .restore_source = true});
					changes.push_back(std::move(line));
				}
				if (planner)
					valid_update = ApplyPlannedOverrides(writes, planner, changes);
				else {
					for (auto& line : changes) {
						auto& current = GetLine(line.id);
						if (current.text != line.text)
							dirty.insert(line.id);
						current = std::move(line);
					}
				}
				if (valid_update)
					transform_active_preview = std::move(active_preview);
			}
			if (valid_update || (!planner && mode != Mode::RotateXY))
				result.geometry_changed = true;
		}
		if (!input.left_down) {
			dragging = holding = false;
			result.capture = CaptureAction::Release;
			result.interaction_ended = true;
		}
		return result;
	}

	Result BeginPress(bool preserve_move_times = false, bool planned = false) {
		if (!prepared)
			return {};
		prepared = false;
		if (mode == Mode::Shear) {
			CapturePlannedSources();
			dragging = true;
			picked = {*prepared_feature};
			return {.capture = CaptureAction::Capture, .interaction_began = true, .geometry_changed = true};
		}
		if (IsTransform()) {
			CapturePlannedSources();
			dragging = (mode == Mode::RotateZ || mode == Mode::RotateXY) && prepared_feature.has_value();
			holding = !dragging;
			return {.capture = CaptureAction::Capture, .interaction_began = true, .geometry_changed = true};
		}
		if (planned)
			CapturePlannedSources();
		if (prepared_feature && feature_index.contains(*prepared_feature)) {
			hovered = prepared_feature;
			if (mode == Mode::Drag) {
				primary = hovered;
				if (std::get<1>(*hovered) != DRAG_BIG_TRIANGLE && !preserve_move_times) {
					int id = std::get<0>(*hovered);
					int change = frame.time_ms - GetLine(id).start_ms - GetFeature(*hovered).time;
					for (auto const& key : picked)
						GetFeature(key).time += change;
				}
			}
			for (auto const& key : picked)
				GetFeature(key).start = GetFeature(key).position;
			dragging = true;
		}
		else if (!prepared_feature && mode == Mode::RectClip && ActiveVisible())
			holding = true;
		bool began = dragging || holding;
		return {.capture = began ? CaptureAction::Capture : CaptureAction::None, .interaction_began = began, .geometry_changed = true};
	}
	void CrossDoubleClick(Vector2D position, Result& result, OverridePlanner const& planner) {
		if (!ActiveVisible() || selected.empty())
			return;
		auto const delta = ToScript(position) - GetLine(*active).position;
		std::vector<OverrideEdit> writes;
		writes.reserve(selected.size() * 2);
		for (int id : selected) {
			auto plan = visual_cross::PlanTranslation(GetLine(id), delta, viewport.video_size.X() > viewport.script_size.X());
			if (!plan)
				return;
			for (auto& edit : *plan)
				writes.push_back(std::move(edit));
		}
		if (planner) {
			CapturePlannedSources();
			if (!ApplyPlannedOverrides(writes, planner))
				return;
		}
		else {
			std::map<int, Line> candidates;
			for (auto const& write : writes) {
				auto& candidate = candidates.try_emplace(write.id, GetLine(write.id)).first->second;
				candidate.text = SetOverride(candidate.text, write.tag, write.value);
			}
			for (auto& [id, candidate] : candidates)
				RefreshGeometryFromText(candidate, viewport.script_size);
			for (auto& [id, candidate] : candidates) {
				auto& current = GetLine(id);
				if (current.text != candidate.text) {
					dirty.insert(id);
					current = std::move(candidate);
				}
			}
		}
		result.geometry_changed = !dirty.empty();
	}

	void DoubleClick(Vector2D position, Result& result, OverridePlanner const& planner) {
		if (mode != Mode::Drag || !ActiveVisible() || !released_click_selection)
			return;
		if (selected != *released_click_selection) {
			SetSelection(std::move(*released_click_selection), active);
			result.selection_changed = true;
		}
		RetireClickSequence();
		auto delta = ToScript(position) - (primary ? ToScript(GetFeature(*primary).position) : GetLine(*active).position);
		if (planner) {
			CapturePlannedSources();
			std::vector<OverrideEdit> writes;
			for (int id : selected) {
				auto const& line = GetLine(id);
				if (line.move) {
					auto const& move = *line.move;
					writes.push_back({.id = id, .tag = "\\move", .value = move.start_ms > 0 || move.end_ms > 0 ? agi::format("(%s,%s,%d,%d)", (move.start + delta).Str(), (move.end + delta).Str(), move.start_ms, move.end_ms) : agi::format("(%s,%s)", (move.start + delta).Str(), (move.end + delta).Str()), .intent = OverrideIntent::Translation, .translation = delta});
				}
				else
					writes.push_back({.id = id, .tag = "\\pos", .value = (line.position + delta).PStr(), .intent = OverrideIntent::Translation, .translation = delta});
				if (line.origin)
					writes.push_back({.id = id, .tag = "\\org", .value = (*line.origin + delta).PStr(), .intent = OverrideIntent::Translation, .translation = delta});
			}
			if (!ApplyPlannedOverrides(writes, planner))
				return;
		}
		else {
			for (int id : selected) {
				auto& line = GetLine(id);
				line.position = line.position + delta;
				if (line.move) {
					auto& move = *line.move;
					move.start = move.start + delta;
					move.end = move.end + delta;
					Write(line, "\\move", move.start_ms > 0 || move.end_ms > 0 ? agi::format("(%s,%s,%d,%d)", move.start.Str(), move.end.Str(), move.start_ms, move.end_ms) : agi::format("(%s,%s)", move.start.Str(), move.end.Str()));
				}
				else
					Write(line, "\\pos", line.position.PStr());
				if (line.origin) {
					line.origin = *line.origin + delta;
					Write(line, "\\org", line.origin->PStr());
				}
				RefreshGeometryFromText(line, viewport.script_size);
			}
		}
		RebuildFeatures();
		primary.reset();
		hovered.reset();
		result.geometry_changed = true;
	}
};

Controller::Controller(Mode mode) : impl(std::make_unique<Impl>(mode)) {}
Controller::~Controller() = default;

void Controller::Reset(std::vector<Line> lines, std::set<int> selected, std::optional<int> active, Frame frame, Viewport viewport) {
	auto mode = impl->mode;
	auto replacement = std::make_unique<Impl>(mode);
	replacement->xy_geometry = std::move(impl->xy_geometry);
	impl = std::move(replacement);
	impl->lines = std::move(lines);
	impl->selected = std::move(selected);
	impl->active = active;
	impl->frame = frame;
	impl->viewport = viewport;
	std::vector<visual_frame_visibility::Interval<Line>> intervals;
	intervals.reserve(impl->lines.size());
	for (size_t i = 0; i < impl->lines.size(); ++i) {
		auto& line = impl->lines[i];
		if (!impl->line_index.emplace(line.id, i).second)
			throw std::invalid_argument("Duplicate visual line identity");
		impl->originals.emplace(line.id, line.text);
		intervals.push_back({.first_frame = line.first_frame, .last_frame = line.last_frame, .line = &line});
	}
	impl->visibility.Rebuild(intervals, frame.number);
	impl->RebuildFeatures();
	if (mode == Mode::VectorClip)
		impl->vector_mode = impl->vector_path.empty() ? visual_vector_clip::Mode::Line : visual_vector_clip::Mode::Drag;
}

void Controller::SetFrame(Frame frame) {
	if (IsInteracting())
		throw std::logic_error("Finish visual interaction before changing frame");
	impl->RetireClickSequence();
	if (impl->mode == Mode::VectorClip)
		impl->RetireVectorPress();
	std::vector<Line *> entered;
	std::vector<Line *> exited;
	bool was_active_visible = impl->ActiveVisible();
	impl->visibility.Advance(impl->frame.number, frame.number, entered, exited);
	impl->frame = frame;
	if (impl->mode != Mode::Drag) {
		if (was_active_visible != impl->ActiveVisible())
			impl->RebuildFeatures();
	}
	else if (!entered.empty() || !exited.empty()) {
		std::unordered_set<int> removed;
		for (auto line : exited)
			removed.insert(line->id);
		std::erase_if(impl->features, [&](auto const& feature) { return removed.contains(std::get<0>(feature.key)); });
		for (auto line : entered)
			impl->AddLineFeatures(*line);
		std::stable_sort(impl->features.begin(), impl->features.end(), [&](auto const& left, auto const& right) { return impl->GetLine(std::get<0>(left.key)).row < impl->GetLine(std::get<0>(right.key)).row; });
		impl->ReindexFeatures();
	}
}

void Controller::UpdateLines(std::span<Line const> replacements) {
	auto& s = *impl;
	if (IsInteracting() || !s.drafts.empty())
		throw std::logic_error("Finish or discard visual drafts before replacing lines");
	s.RetireClickSequence();
	if (s.mode == Mode::VectorClip)
		s.RetireVectorPress();
	std::unordered_set<int> ids;
	bool timing_changed = false;
	for (auto const& line : replacements) {
		auto found = s.line_index.find(line.id);
		if (found == s.line_index.end() || s.lines[found->second].row != line.row || !ids.insert(line.id).second)
			throw std::invalid_argument("Visual line update identity mismatch");
		auto const& old = s.lines[found->second];
		timing_changed |= old.first_frame != line.first_frame || old.last_frame != line.last_frame;
	}
	auto picked = s.picked;
	std::unordered_set<int> previously_visible;
	for (auto const& feature : s.features)
		previously_visible.insert(std::get<0>(feature.key));
	for (auto const& line : replacements) {
		s.GetLine(line.id) = line;
		s.originals.at(line.id) = line.text;
	}
	if (timing_changed) {
		std::vector<visual_frame_visibility::Interval<Line>> intervals;
		intervals.reserve(s.lines.size());
		for (auto& line : s.lines)
			intervals.push_back({.first_frame = line.first_frame, .last_frame = line.last_frame, .line = &line});
		s.visibility.Rebuild(intervals, s.frame.number);
	}
	if (s.mode != Mode::Drag) {
		if (s.active && ids.contains(*s.active)) {
			s.RebuildFeatures();
			s.picked = std::move(picked);
			s.ReindexFeatures();
		}
	}
	else {
		std::erase_if(s.features, [&](auto const& feature) { return ids.contains(std::get<0>(feature.key)); });
		for (auto const& replacement : replacements) {
			auto& line = s.GetLine(replacement.id);
			if (s.visibility.Visible().contains(&line)) {
				s.AddLineFeatures(line);
				Key start{line.id, DRAG_BIG_SQUARE, 0};
				if (previously_visible.contains(line.id) && !picked.contains(start))
					s.picked.erase(start);
			}
		}
		std::ranges::stable_sort(s.features, [&](auto const& left, auto const& right) {
			return s.GetLine(std::get<0>(left.key)).row < s.GetLine(std::get<0>(right.key)).row;
		});
		s.ReindexFeatures();
	}
	s.incremental.clear();
}
void Controller::SetViewport(Viewport viewport) {
	if (IsInteracting())
		throw std::logic_error("Finish visual interaction before changing viewport");
	impl->RetireClickSequence();
	if (impl->mode == Mode::VectorClip) {
		auto& s = *impl;
		s.RetireVectorPress();
		if (!s.vector_path_valid || !ValidVectorViewport(viewport)) {
			s.viewport = viewport;
			s.RebuildVectorPath();
			return;
		}
		auto project = [&](Vector2D point) {
			return s.ToScript(point) * viewport.video_size / viewport.script_size + viewport.video_position;
		};
		for (auto& curve : s.vector_path) {
			curve.p1 = project(curve.p1);
			if (curve.type != SplineCurve::POINT)
				curve.p2 = project(curve.p2);
			if (curve.type == SplineCurve::BICUBIC) {
				curve.p3 = project(curve.p3);
				curve.p4 = project(curve.p4);
			}
		}
		s.viewport = viewport;
		s.vector_path.SetCoordinateSystem({.video_position = viewport.video_position, .video_size = viewport.video_size, .script_size = viewport.script_size});
		s.vector_path_valid = visual_vector_clip::IsEncodable(s.vector_path);
		s.RebuildVectorFeatures();
		return;
	}
	for (auto& feature : impl->features)
		feature.position = impl->ToScript(feature.position) * viewport.video_size / viewport.script_size + viewport.video_position;
	impl->viewport = viewport;
}

void Controller::SetSelection(std::set<int> selected, std::optional<int> active) {
	impl->RetireClickSequence();
	impl->SetSelection(std::move(selected), active);
}

Result Controller::Pointer(PointerInput const& input, VisualToolRenderSnapshot const *displayed_snapshot, bool defer_begin, OverridePlanner const& planner) {
	auto& s = *impl;
	s.BeginOperation();
	Result result;
	if (s.mode == Mode::VectorClip) {
		result = s.VectorPointer(input, displayed_snapshot, defer_begin);
		s.EndOperation();
		return result;
	}
	if (s.mode == Mode::Cross) {
		if (input.phase == PointerPhase::Leave) {
			s.cross_pointer.reset();
			return result;
		}
		if (!visual_cross::IsValidGeometry(input.position, s.viewport))
			return result;
		s.cross_pointer = input.position;
		s.cross_shift = input.shift;
		if (input.phase == PointerPhase::DoubleClick)
			s.CrossDoubleClick(input.position, result, planner);
		s.EndOperation();
		return result;
	}
	if (s.IsTransform()) {
		result = s.TransformPointer(input, displayed_snapshot, defer_begin, planner);
		s.EndOperation();
		return result;
	}
	if (s.mode == Mode::Shear) {
		result = s.ShearPointer(input, displayed_snapshot, defer_begin);
		return result;
	}
	if (input.phase == PointerPhase::Leave) {
		s.RetireClickSequence();
		if (!s.dragging)
			s.hovered.reset();
		result.geometry_changed = true;
		return result;
	}
	if (s.mode == Mode::Drag && input.phase == PointerPhase::DoubleClick) {
		if (s.dragging || s.holding) {
			s.dragging = s.holding = false;
			result.capture = CaptureAction::Release;
			result.interaction_ended = true;
		}
		s.prepared = false;
		s.DoubleClick(input.position, result, planner);
		s.pressed_click_selection.reset();
		s.EndOperation();
		return result;
	}
	if (!s.dragging) {
		auto hovered = s.HitTest(input.position, displayed_snapshot);
		result.geometry_changed = s.hovered != hovered;
		s.hovered = hovered;
	}
	if (s.dragging) {
		bool moved = s.hovered && s.GetFeature(*s.hovered).position != s.GetFeature(*s.hovered).start;
		if (input.left_down || (input.phase == PointerPhase::Release && (input.position != s.drag_start || moved))) {
			s.MoveFeatures(input.position, input.shift, planner);
			result.geometry_changed = true;
		}
		if (!input.left_down) {
			if (s.hovered && s.GetFeature(*s.hovered).position == s.GetFeature(*s.hovered).start && !s.selection_changed_on_press) {
				if (input.control)
					s.RemoveFeature(*s.hovered, result);
				else
					s.SelectFeature(*s.hovered, true, result);
			}
			s.dragging = false;
			s.hovered.reset();
			result.capture = CaptureAction::Release;
			result.interaction_ended = true;
		}
	}
	else if (s.holding) {
		s.MoveHold(input.position);
		result.geometry_changed = true;
		if (!input.left_down) {
			s.holding = false;
			result.capture = CaptureAction::Release;
			result.interaction_ended = true;
		}
	}
	else if (input.phase == PointerPhase::Press) {
		s.drag_start = input.position;
		s.pressed_click_selection = s.selected;
		if (s.hovered) {
			s.selection_changed_on_press = !s.picked.contains(*s.hovered);
			if (s.selection_changed_on_press)
				s.SelectFeature(*s.hovered, !input.control, result);
			s.pressed_click_selection = s.selected;
			int id = std::get<0>(*s.hovered);
			if (id != NoLine && s.active != id) {
				s.active = id;
				result.selection_changed = true;
				result.toolbar_changed = true;
			}
		}
		else {
			if (!input.alt && s.features.size() > 1 && s.active) {
				s.picked.clear();
				if (s.selected != std::set<int>{*s.active})
					result.selection_changed = true;
				s.selected = {*s.active};
			}
		}
		s.prepared = true;
		s.prepared_feature = s.hovered;
		if (!defer_begin) {
			auto begin = s.BeginPress(false, static_cast<bool>(planner));
			result.capture = begin.capture;
			result.interaction_began = begin.interaction_began;
		}
		result.geometry_changed = true;
	}
	if (input.phase == PointerPhase::Release && s.pressed_click_selection) {
		if (s.drafts.empty()) {
			s.released_click_selection = std::move(s.pressed_click_selection);
			s.pressed_click_selection.reset();
		}
		else
			s.RetireClickSequence();
	}
	s.EndOperation();
	return result;
}

Result Controller::BeginPreparedPress(bool preserve_move_times, OverridePlanner const& planner) {
	if (impl->mode != Mode::VectorClip)
		return impl->BeginPress(preserve_move_times, static_cast<bool>(planner));
	impl->BeginOperation();
	auto result = impl->BeginVectorPress();
	impl->EndOperation();
	return result;
}

Result Controller::ToggleMovePosition() {
	auto& s = *impl;
	s.BeginOperation();
	if (s.mode != Mode::Drag || IsInteracting())
		return {};
	for (int id : s.selected) {
		auto& line = s.GetLine(id);
		if (line.move) {
			line.position = line.move->start;
			line.move.reset();
			s.Write(line, "\\pos", line.position.PStr());
		}
		else {
			line.move = Move{.start = line.position, .end = line.position, .start_ms = line.position_to_move_start_ms, .end_ms = line.position_to_move_end_ms};
			s.Write(line, "\\move", agi::format("(%s,%s,%d,%d)", line.position.Str(), line.position.Str(), line.move->start_ms, line.move->end_ms));
		}
		RefreshGeometryFromText(line, s.viewport.script_size);
	}
	s.RebuildFeatures();
	s.primary.reset();
	s.hovered.reset();
	s.EndOperation();
	return {.geometry_changed = true, .toolbar_changed = true};
}

bool Controller::CanNudge() const {
	bool const supported = impl->IsTransform() ||
						   (impl->mode == Mode::Shear && impl->StaticShearSelection() && impl->ShearGeometry().has_value());
	return supported && impl->ActiveVisible() && !impl->selected.empty() && !IsInteracting() && !impl->prepared;
}

Result Controller::Nudge(Vector2D direction, float step, float origin_step, OverridePlanner const& planner) {
	auto& s = *impl;
	if (!CanNudge() || !std::isfinite(direction.X()) || !std::isfinite(direction.Y()) ||
		!std::isfinite(step) || !std::isfinite(origin_step) || step <= 0 || origin_step <= 0 ||
		direction == Vector2D(0, 0))
		return {};
	auto const& active = s.GetLine(*s.active);
	std::vector<Line> changes;
	changes.reserve(s.selected.size());
	for (int id : s.selected) {
		auto candidate = s.GetLine(id);
		if (s.mode == Mode::RotateZ) {
			if (direction.X() != 0) {
				auto angle = visual_transform::NudgeRotation(active.rotation_z, direction.X() * step);
				if (!angle)
					return {};
				candidate.rotation_z = *angle;
			}
			if (direction.Y() != 0) {
				auto origin = candidate.origin.value_or(candidate.position) + Vector2D(0.f, direction.Y() * origin_step);
				if (!std::isfinite(origin.X()) || !std::isfinite(origin.Y()))
					return {};
				candidate.origin = origin;
			}
		}
		else if (s.mode == Mode::RotateXY) {
			if (direction.X() != 0) {
				auto angle = visual_transform::NudgeRotation(active.rotation_y, direction.X() * step);
				if (!angle)
					return {};
				candidate.rotation_y = *angle;
			}
			if (direction.Y() != 0) {
				auto angle = visual_transform::NudgeRotation(active.rotation_x, -direction.Y() * step);
				if (!angle)
					return {};
				candidate.rotation_x = *angle;
			}
		}
		else if (s.mode == Mode::Shear) {
			auto const axis = visual_shear::NudgeAxis(direction);
			auto& value = axis == visual_shear::Axis::X ? candidate.fax : candidate.fay;
			auto const delta = visual_shear::NudgeDelta(axis, direction, step);
			if (!visual_shear::FormatAdjustedValue(value, delta))
				return {};
			value += delta;
		}
		else {
			auto scale = visual_tool_scale_policy::Nudge(active.scale, direction, step);
			if (!scale)
				return {};
			candidate.scale = *scale;
		}
		changes.push_back(std::move(candidate));
	}
	return s.ApplyCommand(std::move(changes), s.IsTransform() ? planner : OverridePlanner{});
}

bool Controller::CanNormalizeScale(visual_tool_scale_policy::Axis axis) const {
	return impl->mode == Mode::Scale && CanNudge() &&
		   std::ranges::all_of(impl->selected, [&](int id) { return visual_tool_scale_policy::NormalizeTo100(impl->GetLine(id).scale, axis).has_value(); });
}

Result Controller::NormalizeScale(visual_tool_scale_policy::Axis axis, OverridePlanner const& planner) {
	if (!CanNormalizeScale(axis))
		return {};
	auto& s = *impl;
	std::vector<Line> changes;
	changes.reserve(s.selected.size());
	for (int id : s.selected) {
		auto candidate = s.GetLine(id);
		candidate.scale = *visual_tool_scale_policy::NormalizeTo100(candidate.scale, axis);
		changes.push_back(std::move(candidate));
	}
	return s.ApplyCommand(std::move(changes), planner);
}

bool Controller::CanResetShear(visual_shear::Axis axis) const {
	return (axis == visual_shear::Axis::X || axis == visual_shear::Axis::Y) && impl->mode == Mode::Shear && CanNudge();
}

Result Controller::ResetShear(visual_shear::Axis axis) {
	if (!CanResetShear(axis))
		return {};
	auto& s = *impl;
	std::vector<Line> changes;
	changes.reserve(s.selected.size());
	for (int id : s.selected) {
		auto candidate = s.GetLine(id);
		(axis == visual_shear::Axis::X ? candidate.fax : candidate.fay) = 0;
		changes.push_back(std::move(candidate));
	}
	return s.ApplyCommand(std::move(changes));
}

Result Controller::SetVectorClipMode(visual_vector_clip::Mode mode) {
	if (impl->mode != Mode::VectorClip || static_cast<int>(mode) >= visual_vector_clip::kModeCount ||
		impl->vector_mode == mode || impl->prepared || IsInteracting())
		return {};
	impl->vector_mode = mode;
	impl->vector_hovered.reset();
	impl->RetireVectorPress();
	return {.geometry_changed = true, .toolbar_changed = true};
}

visual_vector_clip::Mode Controller::VectorClipMode() const noexcept { return impl->vector_mode; }

Result Controller::FinishInteraction() {
	impl->BeginOperation();
	bool active = IsInteracting();
	if (active || impl->pressed_click_selection)
		impl->RetireClickSequence();
	bool toolbar_changed = false;
	if (impl->mode == Mode::VectorClip && impl->holding &&
		(impl->vector_mode == visual_vector_clip::Mode::Freehand || impl->vector_mode == visual_vector_clip::Mode::FreehandSmooth)) {
		if (impl->vector_pointer)
			impl->UpdateVectorHold(*impl->vector_pointer, true);
		impl->vector_mode = visual_vector_clip::Mode::Drag;
		toolbar_changed = true;
		impl->EndOperation();
	}
	impl->dragging = false;
	impl->holding = false;
	impl->prepared = false;
	impl->hovered.reset();
	impl->vector_hovered.reset();
	impl->vector_prepared_feature.reset();
	impl->vector_prepared_closest.reset();
	return {.capture = active ? CaptureAction::Release : CaptureAction::None, .interaction_ended = active, .geometry_changed = active, .toolbar_changed = toolbar_changed};
}

void Controller::AcceptChanges() {
	if (IsInteracting())
		throw std::logic_error("Finish visual interaction before accepting changes");
	if (!impl->drafts.empty())
		impl->RetireClickSequence();
	for (auto const& [id, edit] : impl->drafts)
		impl->originals.at(id) = edit.text;
	impl->drafts.clear();
	impl->cumulative.clear();
	impl->incremental.clear();
	impl->planned_sources.clear();
}

bool Controller::IsInteracting() const noexcept { return impl->dragging || impl->holding; }
std::set<int> const& Controller::SelectedLines() const noexcept { return impl->selected; }
std::optional<int> Controller::ActiveLine() const noexcept { return impl->active; }
bool Controller::ActiveLineHasMove() const noexcept { return impl->ActiveVisible() && impl->GetLine(*impl->active).move.has_value(); }
std::vector<int> Controller::SelectedMoveLines() const {
	if (impl->mode != Mode::Drag)
		return {};
	std::set<int> selected;
	for (auto const& key : impl->picked)
		if (std::get<1>(key) != DRAG_BIG_TRIANGLE && impl->GetLine(std::get<0>(key)).move)
			selected.insert(std::get<0>(key));
	return {selected.begin(), selected.end()};
}
std::span<const VisualTextEdit> Controller::IncrementalChanges() const noexcept { return impl->incremental; }
std::span<const VisualTextEdit> Controller::DraftChanges() const noexcept { return impl->cumulative; }

std::shared_ptr<const VisualToolRenderSnapshot> Controller::CaptureSnapshot(std::shared_ptr<const VisualToolRenderContext> const& context, Style const& style) const {
	auto const& s = *impl;
	if (s.mode == Mode::VectorClip) {
		if (!s.ActiveVisible() || !s.vector_path_valid)
			return {};
		auto snapshot = std::make_shared<VectorClipSnapshot>(context);
		snapshot->path = s.vector_path;
		snapshot->features = s.vector_features;
		for (size_t i = 0; i < snapshot->features.size(); ++i) {
			snapshot->features[i].selected = s.vector_picked.contains(i);
			snapshot->features[i].active = s.vector_hovered == i;
		}
		snapshot->source = s.vector_source;
		snapshot->mode = s.vector_mode;
		snapshot->viewport = s.viewport;
		snapshot->style = style;
		snapshot->pointer = s.vector_pointer;
		snapshot->drag_start = s.drag_start;
		snapshot->inverse = s.GetLine(*s.active).vector_clip.inverse;
		snapshot->editable = s.CanEditVector();
		snapshot->holding = s.holding;
		return snapshot;
	}
	if (s.mode == Mode::Cross) {
		if (!s.cross_pointer || !visual_cross::IsValidGeometry(*s.cross_pointer, s.viewport))
			return {};
		auto snapshot = std::make_shared<CrossSnapshot>(context);
		snapshot->pointer = *s.cross_pointer;
		snapshot->viewport = s.viewport;
		snapshot->shift = s.cross_shift;
		snapshot->font_size = style.coordinate_font_size;
		return snapshot;
	}
	auto fill = [&](Impl::Feature const& feature) {
		return s.hovered == feature.key ? style.highlight_secondary : s.picked.contains(feature.key) ? style.line_primary
																									 : style.highlight_primary;
	};
	if (s.mode == Mode::Drag) {
		auto snapshot = std::make_shared<VisualToolDragSnapshot>(context);
		snapshot->grid_colour = style.line_secondary;
		snapshot->line_colour = style.line_primary;
		snapshot->features.reserve(s.features.size());
		for (auto const& feature : s.features)
			snapshot->features.push_back({.type = static_cast<DraggableFeatureType>(std::get<1>(feature.key)), .pos = feature.position, .parent = feature.parent ? std::optional<Vector2D>(s.GetFeature(*feature.parent).position) : std::nullopt, .fill_colour = fill(feature), .line_id = std::get<0>(feature.key), .layer = feature.layer});
		return snapshot;
	}
	if (!s.ActiveVisible())
		return {};
	if (s.mode == Mode::Shear) {
		auto geometry = s.ShearGeometry();
		if (!geometry)
			return {};
		auto snapshot = std::make_shared<ShearSnapshot>(context);
		auto const& line = s.GetLine(*s.active);
		snapshot->editable = s.StaticShearSelection();
		auto& state = snapshot->state;
		state.geometry = *geometry;
		state.coefficients = {line.fax, line.fay};
		state.video_position = s.viewport.video_position;
		state.video_size = s.viewport.video_size;
		state.primary_colour = VideoOverlayColourFromRgb(style.line_primary);
		state.secondary_colour = VideoOverlayColourFromRgb(style.line_secondary);
		for (size_t i = 0; i < state.handle_fill_colours.size(); ++i) {
			Key const key{NoLine, i == 0 ? DRAG_BIG_SQUARE : DRAG_BIG_CIRCLE, i};
			state.handle_fill_colours[i] = VideoOverlayColourFromRgb(s.hovered == key ? style.highlight_secondary : s.picked.contains(key) ? style.line_primary
																																		   : style.highlight_primary);
		}
		state.font_size = style.coordinate_font_size;
		state.hint = style.shear_hint;
		if (!snapshot->editable)
			state.diagnostic = style.shear_diagnostic;
		return snapshot;
	}
	if (s.IsTransform()) {
		if (s.mode == Mode::RotateXY && !HasValidXYGeometry(s.GetLine(*s.active), s.viewport))
			return {};
		auto snapshot = std::make_shared<TransformSnapshot>(context);
		snapshot->mode = s.mode;
		snapshot->line = s.dragging || s.holding ? s.transform_active_preview : s.GetLine(*s.active);
		snapshot->viewport = s.viewport;
		snapshot->style = style;
		if (s.mode == Mode::RotateXY)
			snapshot->xy_geometry = s.xy_geometry.Get(VideoOverlayColourFromRgb(style.line_secondary));
		return snapshot;
	}
	auto snapshot = std::make_shared<ClipSnapshot>(context);
	snapshot->style = style;
	snapshot->video_position = s.viewport.video_position;
	snapshot->video_size = s.viewport.video_size;
	snapshot->inverse = s.GetLine(*s.active).clip.inverse;
	for (size_t i = 0; i < 4; ++i) {
		snapshot->points[i] = s.features[i].position;
		snapshot->fills[i] = fill(s.features[i]);
	}
	return snapshot;
}

}
