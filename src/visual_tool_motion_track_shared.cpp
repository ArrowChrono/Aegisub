#include "visual_tool_motion_track_shared.h"

#include "motion_track/apply_plan.h"
#include "perspective_quad_geometry.h"
#include "translation_service.h"
#include "video_overlay_draw_context.h"
#include "visual_tool_roi_press_policy.h"

#include <libaegisub/format.h>

#include <algorithm>
#include <cmath>
#include <ranges>
#include <string>

namespace aegisub::visual_motion_track {
using namespace motion_track;

namespace {
constexpr float HandleTolerance = 5.f;
constexpr float HandleHalfSize = 3.f;

bool Finite(Vector2D point) {
	return std::isfinite(point.X()) && std::isfinite(point.Y());
}

bool IsResize(RoiGrab grab) {
	return grab >= RoiGrab::Nw && grab <= RoiGrab::W;
}

Vector2D ClampToFrame(Vector2D point, int width, int height) {
	return {std::clamp(point.X(), 0.f, static_cast<float>(width)),
			std::clamp(point.Y(), 0.f, static_cast<float>(height))};
}
}

std::optional<Vector2D> PoseToStorage(EditPose const& pose,
									  Vector2D seed_pt) {
	if (!pose.valid || !Finite(seed_pt))
		return std::nullopt;
	if (pose.identity)
		return seed_pt;
	auto const mapped = perspective::Homography(perspective::Matrix3(pose.forward.matrix))
							.Map({.x = seed_pt.X(), .y = seed_pt.Y()});
	if (!mapped)
		return std::nullopt;
	return Vector2D(static_cast<float>(mapped->x), static_cast<float>(mapped->y));
}

std::optional<Vector2D> PoseToAnchor(EditPose const& pose,
									 Vector2D storage_pt) {
	if (!pose.valid || !Finite(storage_pt))
		return std::nullopt;
	if (pose.identity)
		return storage_pt;
	auto const mapped = perspective::Homography(perspective::Matrix3(pose.inverse.matrix))
							.Map({.x = storage_pt.X(), .y = storage_pt.Y()});
	if (!mapped)
		return std::nullopt;
	return Vector2D(static_cast<float>(mapped->x), static_cast<float>(mapped->y));
}

bool PoseSupportsRoi(EditPose const& pose, RoiRect roi) {
	if (!pose.valid)
		return false;
	if (pose.identity)
		return true;
	perspective::Homography const homography(perspective::Matrix3(pose.forward.matrix));
	perspective::Rect const domain{.left = static_cast<double>(roi.x), .top = static_cast<double>(roi.y), .right = static_cast<double>(roi.x) + roi.w, .bottom = static_cast<double>(roi.y) + roi.h};
	if (perspective::ValidateProjectionDomain(homography, domain) != perspective::GeometryError::None)
		return false;
	for (auto const corner : perspective::MakeQuad(domain))
		if (!homography.Map(corner))
			return false;
	return true;
}

EditPose PoseFromAnchorRide(
	TrackSample const& ride, aegisub::motion_track::RoiRideAnchor const& anchor,
	TrackModel model) {
	EditPose pose;
	using perspective::Matrix3;
	Matrix3 const to_ride({1, 0, ride.center_x, 0, 1, ride.center_y, 0, 0, 1});
	Matrix3 const from_anchor({1, 0, -anchor.center_x, 0, 1, -anchor.center_y, 0, 0, 1});
	Matrix3 relative;
	if (model != TrackModel::Translation) {
		auto const& m = ride.transform.matrix;
		Matrix3 const centered({m[0] - m[2] * m[6], m[1] - m[2] * m[7], 0,
								m[3] - m[5] * m[6], m[4] - m[5] * m[7], 0, m[6], m[7], 1});
		auto const anchor_inverse = Matrix3({anchor.m00, anchor.m01, 0,
											 anchor.m10, anchor.m11, 0, anchor.p, anchor.q, 1})
										.Inverse();
		if (!anchor_inverse) {
			pose.valid = false;
			return pose;
		}
		relative = centered * *anchor_inverse;
	}
	Matrix3 const forward = to_ride * relative * from_anchor;
	auto const inverse = forward.Inverse();
	if (!inverse) {
		pose.valid = false;
		return pose;
	}
	pose.forward.matrix = forward.Values();
	pose.inverse.matrix = inverse->Values();
	pose.identity = false;
	return pose;
}

RoiGrab HitTest(
	Vector2D p, RoiRect roi, float tol_x, float tol_y) {
	auto const l = static_cast<float>(roi.x), t = static_cast<float>(roi.y);
	auto const r = static_cast<float>(roi.x + roi.w), b = static_cast<float>(roi.y + roi.h);

	bool const near_l = std::abs(p.X() - l) <= tol_x;
	bool const near_r = std::abs(p.X() - r) <= tol_x;
	bool const near_t = std::abs(p.Y() - t) <= tol_y;
	bool const near_b = std::abs(p.Y() - b) <= tol_y;
	bool const span_x = p.X() >= l && p.X() <= r;
	bool const span_y = p.Y() >= t && p.Y() <= b;

	if (near_t && near_l)
		return RoiGrab::Nw;
	if (near_t && near_r)
		return RoiGrab::Ne;
	if (near_b && near_l)
		return RoiGrab::Sw;
	if (near_b && near_r)
		return RoiGrab::Se;
	if (near_t && span_x)
		return RoiGrab::N;
	if (near_b && span_x)
		return RoiGrab::S;
	if (near_l && span_y)
		return RoiGrab::W;
	if (near_r && span_y)
		return RoiGrab::E;
	if (span_x && span_y)
		return RoiGrab::Move;
	return RoiGrab::None;
}

EditPose CurrentPose(OverlaySnapshot const& snapshot) {
	if (snapshot.editing_pose)
		return *snapshot.editing_pose;
	if (snapshot.track && snapshot.roi && snapshot.anchor.valid) {
		auto const *sample = FindSample(snapshot.track->samples, snapshot.identity.frame);
		if (sample && sample->status == TrackStatus::Ok)
			return PoseFromAnchorRide(*sample, snapshot.anchor, snapshot.track->model);
	}
	return {};
}

std::optional<Vector2D> CanvasToStorage(Viewport const& viewport, Vector2D point) {
	if (!Finite(point) || !Finite(viewport.origin) || !Finite(viewport.extent) || viewport.extent.X() <= 0 || viewport.extent.Y() <= 0 || viewport.storage_width <= 0 || viewport.storage_height <= 0)
		return std::nullopt;
	double const sx = static_cast<double>(viewport.storage_width) / viewport.extent.X();
	double const sy = static_cast<double>(viewport.storage_height) / viewport.extent.Y();
	Vector2D const storage((point.X() - viewport.origin.X()) * static_cast<float>(sx),
						   (point.Y() - viewport.origin.Y()) * static_cast<float>(sy));
	return Finite(storage) ? std::optional(storage) : std::nullopt;
}

bool RoiController::Matches(OverlaySnapshot const& snapshot) const {
	return identity.source_id == snapshot.identity.source_id && identity.generation == snapshot.identity.generation && storage_width == snapshot.viewport.storage_width && storage_height == snapshot.viewport.storage_height;
}

std::optional<RoiEdit> RoiController::Normalize(RoiRect roi) const {
	if (storage_width < RoiMinSide || storage_height < RoiMinSide)
		return std::nullopt;
	if (!pose.identity) {
		roi.w = std::clamp(roi.w, RoiMinSide, RoiMaxSide);
		roi.h = std::clamp(roi.h, RoiMinSide, RoiMaxSide);
		if (!PoseSupportsRoi(pose, roi))
			return std::nullopt;
	}
	else {
		roi.w = std::clamp(roi.w, RoiMinSide, std::min(RoiMaxSide, storage_width));
		roi.h = std::clamp(roi.h, RoiMinSide, std::min(RoiMaxSide, storage_height));
		roi.x = std::clamp(roi.x, 0, storage_width - roi.w);
		roi.y = std::clamp(roi.y, 0, storage_height - roi.h);
	}
	return RoiEdit{.roi = roi, .reanchor = pose.identity};
}

InteractionResult RoiController::Press(OverlaySnapshot const& snapshot, Vector2D point) {
	CaptureLost();
	auto const storage = CanvasToStorage(snapshot.viewport, point);
	if (!storage)
		return {};
	identity = snapshot.identity;
	storage_width = snapshot.viewport.storage_width;
	storage_height = snapshot.viewport.storage_height;
	last_point = point;
	pose = CurrentPose(snapshot);
	auto const anchor_point = PoseToAnchor(pose, *storage);
	float const tolerance_x = HandleTolerance * static_cast<float>(storage_width) / snapshot.viewport.extent.X();
	float const tolerance_y = HandleTolerance * static_cast<float>(storage_height) / snapshot.viewport.extent.Y();
	auto const hit = snapshot.roi && anchor_point && PoseSupportsRoi(pose, *snapshot.roi)
						 ? HitTest(*anchor_point, *snapshot.roi, tolerance_x, tolerance_y)
						 : RoiGrab::None;
	if (IsResize(hit)) {
		grab = hit;
		fixed_left = static_cast<float>(snapshot.roi->x);
		fixed_top = static_cast<float>(snapshot.roi->y);
		fixed_right = static_cast<float>(snapshot.roi->x + snapshot.roi->w);
		fixed_bottom = static_cast<float>(snapshot.roi->y + snapshot.roi->h);
	}
	else if (hit == RoiGrab::Move) {
		grab = hit;
		drag_offset = {anchor_point->X() - snapshot.roi->x, anchor_point->Y() - snapshot.roi->y};
	}
	else if (visual_tool_roi_press_policy::PressMayStartBand(
				 visual_tool_roi_press_policy::PressIsOnFrame(storage->X(), storage->Y(), storage_width, storage_height))) {
		grab = RoiGrab::Band;
		pose = {};
		band_start = *storage;
		return {.handled = true, .edit = Normalize({.x = static_cast<int>(std::lround(storage->X())), .y = static_cast<int>(std::lround(storage->Y())), .w = 0, .h = 0})};
	}
	return {.handled = Active(), .edit = std::nullopt};
}

InteractionResult RoiController::Move(OverlaySnapshot const& snapshot, Vector2D point) {
	if (!Active())
		return {};
	auto const storage = CanvasToStorage(snapshot.viewport, point);
	if (!Matches(snapshot) || !storage) {
		CaptureLost();
		return {.handled = true, .edit = std::nullopt};
	}
	last_point = point;
	if (grab == RoiGrab::Band) {
		auto const a = ClampToFrame(band_start, storage_width, storage_height);
		auto const b = ClampToFrame(*storage, storage_width, storage_height);
		auto const x0 = static_cast<int>(std::lround(std::min(a.X(), b.X())));
		auto const y0 = static_cast<int>(std::lround(std::min(a.Y(), b.Y())));
		auto const x1 = static_cast<int>(std::lround(std::max(a.X(), b.X())));
		auto const y1 = static_cast<int>(std::lround(std::max(a.Y(), b.Y())));
		return {.handled = true, .edit = Normalize({.x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0})};
	}
	auto const anchor_point = PoseToAnchor(pose, *storage);
	if (!anchor_point)
		return {.handled = true, .edit = std::nullopt};
	if (grab == RoiGrab::Move) {
		if (!snapshot.roi)
			return {.handled = true, .edit = std::nullopt};
		return {.handled = true, .edit = Normalize({.x = static_cast<int>(std::lround(anchor_point->X() - drag_offset.X())), .y = static_cast<int>(std::lround(anchor_point->Y() - drag_offset.Y())), .w = snapshot.roi->w, .h = snapshot.roi->h})};
	}
	bool const follow_left = grab == RoiGrab::Nw || grab == RoiGrab::W || grab == RoiGrab::Sw;
	bool const follow_right = grab == RoiGrab::Ne || grab == RoiGrab::E || grab == RoiGrab::Se;
	bool const follow_top = grab == RoiGrab::Nw || grab == RoiGrab::N || grab == RoiGrab::Ne;
	bool const follow_bottom = grab == RoiGrab::Sw || grab == RoiGrab::S || grab == RoiGrab::Se;
	float const left = follow_left ? anchor_point->X() : fixed_left;
	float const right = follow_right ? anchor_point->X() : fixed_right;
	float const top = follow_top ? anchor_point->Y() : fixed_top;
	float const bottom = follow_bottom ? anchor_point->Y() : fixed_bottom;
	auto const x0 = static_cast<int>(std::lround(std::min(left, right)));
	auto const y0 = static_cast<int>(std::lround(std::min(top, bottom)));
	auto const x1 = static_cast<int>(std::lround(std::max(left, right)));
	auto const y1 = static_cast<int>(std::lround(std::max(top, bottom)));
	return {.handled = true, .edit = Normalize({.x = x0, .y = y0, .w = x1 - x0, .h = y1 - y0})};
}

InteractionResult RoiController::Release(OverlaySnapshot const& snapshot, Vector2D point) {
	if (!Active())
		return {};
	InteractionResult result{.handled = true, .edit = std::nullopt};
	if (point != last_point)
		result = Move(snapshot, point);
	CaptureLost();
	return result;
}

void RoiController::CaptureLost() {
	grab = RoiGrab::None;
}

std::shared_ptr<const PlanPreviewData> BuildPlanPreview(
	MotionTrackApplyPlan const& plan, int storage_width, int storage_height,
	int script_width, int script_height) {
	if (!plan.has_mutations() || storage_width <= 0 || storage_height <= 0 || script_width <= 0 || script_height <= 0)
		return {};
	auto data = std::make_shared<PlanPreviewData>();
	double const scale_x = static_cast<double>(storage_width) / script_width;
	double const scale_y = static_cast<double>(storage_height) / script_height;
	for (auto const& line : plan.lines) {
		std::vector<PlanPreviewPoint> path;
		for (auto const& part : line.parts) {
			if (!part.covered)
				continue;
			path.push_back({.x = static_cast<float>(part.x0 * scale_x), .y = static_cast<float>(part.y0 * scale_y)});
		}
		for (auto const& part : line.parts | std::views::reverse) {
			if (!part.covered)
				continue;
			path.push_back({.x = static_cast<float>(part.x1 * scale_x), .y = static_cast<float>(part.y1 * scale_y)});
			break;
		}
		if (path.size() >= 2)
			data->paths.push_back(std::move(path));
	}
	return data;
}

void DrawOverlay(VideoOverlayDrawContext& draw, OverlaySnapshot const& snapshot, OverlayStyle const& style) {
	int const sw = snapshot.viewport.storage_width;
	int const sh = snapshot.viewport.storage_height;
	if (!CanvasToStorage(snapshot.viewport, snapshot.viewport.origin))
		return;
	auto const video_pos = snapshot.viewport.origin;
	auto const video_res = snapshot.viewport.extent;
	double const scale_x = static_cast<double>(video_res.X()) / sw;
	double const scale_y = static_cast<double>(video_res.Y()) / sh;

	agi::Color const& line = style.primary;
	draw.SetLineColour(line, 1.0f, 2);
	draw.SetFillColour(agi::Color(0, 0, 0), 0.0f);

	auto const& snap = snapshot.track;
	int current_frame = -1;
	bool in_range = false;
	if (snap && !snap->samples.empty()) {
		current_frame = snapshot.identity.frame;
		in_range = current_frame >= snap->samples.front().frame && current_frame <= snap->samples.back().frame;
	}

	if (auto const& plan = snapshot.plan; plan && !plan->paths.empty()) {
		{
			agi::Color const& ghost = style.secondary;
			draw.SetLineColour(ghost, 0.8f, 1);
			draw.SetFillColour(ghost, 0.8f);
			for (auto const& path : plan->paths) {
				std::vector<Vector2D> pts;
				pts.reserve(path.size());
				for (auto const& p : path)
					pts.emplace_back(
						static_cast<float>(video_pos.X() + p.x * scale_x),
						static_cast<float>(video_pos.Y() + p.y * scale_y));
				if (pts.size() >= 2)
					draw.DrawLineStrip(pts.data(), pts.size());
				for (auto const& p : pts)
					draw.DrawRectangle(
						Vector2D(p.X() - 2, p.Y() - 2),
						Vector2D(p.X() + 2, p.Y() + 2));
			}
		}
	}

	if (snapshot.roi) {
		auto const roi = *snapshot.roi;
		EditPose const pose = CurrentPose(snapshot);

		Vector2D const seed_pts[8] = {
			Vector2D(static_cast<float>(roi.x), static_cast<float>(roi.y)),
			Vector2D(roi.x + (roi.w - 1) / 2.0f, static_cast<float>(roi.y)),
			Vector2D(static_cast<float>(roi.x + roi.w), static_cast<float>(roi.y)),
			Vector2D(static_cast<float>(roi.x + roi.w), roi.y + (roi.h - 1) / 2.0f),
			Vector2D(static_cast<float>(roi.x + roi.w), static_cast<float>(roi.y + roi.h)),
			Vector2D(roi.x + (roi.w - 1) / 2.0f, static_cast<float>(roi.y + roi.h)),
			Vector2D(static_cast<float>(roi.x), static_cast<float>(roi.y + roi.h)),
			Vector2D(static_cast<float>(roi.x), roi.y + (roi.h - 1) / 2.0f)};
		bool valid_roi = PoseSupportsRoi(pose, roi);
		Vector2D storage_pts[8];
		if (valid_roi) {
			for (int i = 0; i < 8; ++i) {
				auto const point = PoseToStorage(pose, seed_pts[i]);
				if (!point) {
					valid_roi = false;
					break;
				}
				storage_pts[i] = *point;
			}
		}

		Vector2D box_top_left;
		if (valid_roi && !pose.identity) {
			Vector2D pts[8];
			for (int i = 0; i < 8; ++i)
				pts[i] = Vector2D(
					static_cast<float>(video_pos.X() + storage_pts[i].X() * scale_x),
					static_cast<float>(video_pos.Y() + storage_pts[i].Y() * scale_y));
			Vector2D const quad[4] = {pts[0], pts[2], pts[4], pts[6]};
			draw.SetFillColour(agi::Color(0, 0, 0), 0.0f);
			draw.DrawPolygon(quad, 4);
			draw.SetFillColour(line, 1.0f);
			for (auto const& p : pts)
				draw.DrawRectangle(
					Vector2D(p.X() - HandleHalfSize, p.Y() - HandleHalfSize),
					Vector2D(p.X() + HandleHalfSize, p.Y() + HandleHalfSize));
			box_top_left = pts[0];
		}
		else if (valid_roi) {
			Vector2D const p1(static_cast<float>(video_pos.X() + roi.x * scale_x),
							  static_cast<float>(video_pos.Y() + roi.y * scale_y));
			Vector2D const p2(p1.X() + static_cast<float>(roi.w * scale_x),
							  p1.Y() + static_cast<float>(roi.h * scale_y));
			draw.SetFillColour(agi::Color(0, 0, 0), 0.0f);
			draw.DrawRectangle(p1, p2);
			draw.SetFillColour(line, 1.0f);
			for (auto const& storage_pt : storage_pts) {
				Vector2D const p(
					static_cast<float>(video_pos.X() + storage_pt.X() * scale_x),
					static_cast<float>(video_pos.Y() + storage_pt.Y() * scale_y));
				draw.DrawRectangle(
					Vector2D(p.X() - HandleHalfSize, p.Y() - HandleHalfSize),
					Vector2D(p.X() + HandleHalfSize, p.Y() + HandleHalfSize));
			}
			box_top_left = p1;
		}

		if (valid_roi && !snapshot.editing_pose && in_range) {
			auto const *s = FindSample(snap->samples, current_frame);
			std::string label;
			if (s && s->status == TrackStatus::Ok)
				label = agi::format(_("ncc %.2f").c_str(), s->confidence);
			else if (s && s->status == TrackStatus::Failed)
				label = _("lost");
			if (!label.empty()) {
				VideoOverlayTextStyle style;
				style.colour = line;
				auto const extent = draw.MeasureText(label, style);
				draw.DrawText(label, static_cast<int>(box_top_left.X()),
							  static_cast<int>(box_top_left.Y()) - extent.GetHeight() - 4, style);
			}
		}
	}

	if (!snap || snap->samples.empty() || !in_range)
		return;

	std::vector<Vector2D> past;
	std::vector<Vector2D> future;
	for (auto const& s : snap->samples) {
		if (s.status != TrackStatus::Ok)
			continue;
		Vector2D const pt(static_cast<float>(video_pos.X() + s.center_x * scale_x),
						  static_cast<float>(video_pos.Y() + s.center_y * scale_y));
		(s.frame <= current_frame ? past : future).push_back(pt);
	}
	if (past.size() == 1 && !future.empty())
		past.push_back(future.front());
	if (past.size() >= 2)
		draw.DrawLineStrip(past.data(), past.size());
	if (future.size() >= 2) {
		draw.SetLineColour(line, 0.3f, 2);
		draw.DrawLineStrip(future.data(), future.size());
		draw.SetLineColour(line, 1.0f, 2);
	}

	for (auto const& s : snap->samples) {
		if (s.status != TrackStatus::Failed)
			continue;
		auto const cx = static_cast<float>(video_pos.X() + s.center_x * scale_x);
		auto const cy = static_cast<float>(video_pos.Y() + s.center_y * scale_y);
		constexpr float r = 5.f;
		draw.DrawLine(Vector2D(cx - r, cy - r), Vector2D(cx + r, cy + r));
		draw.DrawLine(Vector2D(cx - r, cy + r), Vector2D(cx + r, cy - r));
	}
}

}
