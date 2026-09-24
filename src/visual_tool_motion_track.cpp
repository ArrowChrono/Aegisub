#include "visual_tool_motion_track.h"

#include "async_video_provider.h"
#include "compat.h"
#include "dialog_motion_track.h"
#include "gl_text.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "dialog_manager.h"
#include "include/aegisub/context_ui.h"
#include "project.h"
#include "perspective_quad_geometry.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_overlay_draw_context.h"
#include "video_overlay_draw_context_legacy_gl.h"
#include "visual_tool_roi_press_policy.h"

#include <libaegisub/make_unique.h>

#include <libaegisub/color.h>

#include <wx/event.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>

using namespace aegisub::motion_track;

namespace {
// Hit tolerance for the resize grips, in display pixels; converted to storage
// units per axis at use sites so it holds at every zoom level.
constexpr float kHandleTolPx = 5.f;
// Half-size of a drawn grip square, in display pixels.
constexpr float kHandleHalfPx = 3.f;

// Clamp to the frame before measuring, not after: PushRoi can only push the
// box back inside, which would keep the out-of-frame part as width and give a
// bigger rectangle than the pointer ever covered.
Vector2D ClampToFrame(Vector2D v, int sw, int sh) {
	return Vector2D(std::clamp(v.X(), 0.f, float(sw)),
					std::clamp(v.Y(), 0.f, float(sh)));
}

std::optional<Vector2D> PoseToStorage(VisualToolMotionTrack::EditPose const& pose,
									  Vector2D seed_pt) {
	if (!pose.valid)
		return std::nullopt;
	if (pose.identity)
		return seed_pt;
	auto const mapped = perspective::Homography(perspective::Matrix3(pose.forward.matrix))
							.Map({.x = seed_pt.X(), .y = seed_pt.Y()});
	if (!mapped)
		return std::nullopt;
	return Vector2D(static_cast<float>(mapped->x), static_cast<float>(mapped->y));
}

std::optional<Vector2D> PoseToSeed(VisualToolMotionTrack::EditPose const& pose,
								   Vector2D storage_pt) {
	if (!pose.valid)
		return std::nullopt;
	if (pose.identity)
		return storage_pt;
	auto const mapped = perspective::Homography(perspective::Matrix3(pose.inverse.matrix))
							.Map({.x = storage_pt.X(), .y = storage_pt.Y()});
	if (!mapped)
		return std::nullopt;
	return Vector2D(static_cast<float>(mapped->x), static_cast<float>(mapped->y));
}

bool PoseSupportsRoi(VisualToolMotionTrack::EditPose const& pose, RoiRect roi) {
	if (!pose.valid)
		return false;
	if (pose.identity)
		return true;
	perspective::Homography const homography(perspective::Matrix3(pose.forward.matrix));
	perspective::Rect const domain{.left = static_cast<double>(roi.x), .top = static_cast<double>(roi.y), .right = static_cast<double>(roi.x) + roi.w, .bottom = static_cast<double>(roi.y) + roi.h};
	if (perspective::ValidateProjectionDomain(homography, domain) != perspective::GeometryError::None)
		return false;
	// Same-sign denominators prevent crossing infinity; successful corner
	// maps also bound coordinates before display conversion or ROI editing.
	for (auto const corner : perspective::MakeQuad(domain))
		if (!homography.Map(corner))
			return false;
	return true;
}

// Pose from the ride sample onto the anchor's frame: maps the ROI's
// coordinate space (the anchor frame, where the spin values were last
// expressed) to storage coordinates at the ride's frame, including the
// projective denominator. Its inverse keeps editing in the ROI's own space.
VisualToolMotionTrack::EditPose PoseFromAnchorRide(
	TrackSample const& ride, aegisub::motion_track::RoiRideAnchor const& anchor,
	TrackModel model) {
	VisualToolMotionTrack::EditPose pose;
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
} // namespace

VisualToolMotionTrack::VisualToolMotionTrack(VideoDisplay *parent,
											 agi::Context *context)
	: VisualToolBase(parent, context), context(context), gl_text(parent->CreateTextRenderer()) {}

// Out of line: OpenGLText is only forward-declared in the header, so the
// unique_ptr deleter needs the complete type here.
VisualToolMotionTrack::~VisualToolMotionTrack() = default;

std::shared_ptr<const MotionTrackSnapshot>
VisualToolMotionTrack::SnapshotForOverlay() const {
	auto *dialog = context->GetUI().dialog->Get<::DialogMotionTrack>();
	if (!dialog)
		return nullptr;
	return dialog->CaptureForOverlay();
}

bool VisualToolMotionTrack::CurrentRoi(RoiRect& roi) const {
	auto *dialog = context->GetUI().dialog->Get<::DialogMotionTrack>();
	if (!dialog)
		return false;
	roi = dialog->OverlayRoi();
	return true;
}

aegisub::motion_track::RoiRideAnchor VisualToolMotionTrack::RoiAnchor() const {
	auto *dialog = context->GetUI().dialog->Get<::DialogMotionTrack>();
	return dialog ? dialog->OverlayRoiAnchor()
				  : aegisub::motion_track::RoiRideAnchor{};
}

bool VisualToolMotionTrack::StorageDims(int& width, int& height) const {
	// Prefer the snapshot: a trajectory must be drawn against the dimensions it
	// was measured at, not whatever is loaded now.
	if (auto snap = SnapshotForOverlay();
		snap && snap->storage_width > 0 && snap->storage_height > 0) {
		width = snap->storage_width;
		height = snap->storage_height;
		return true;
	}
	// No run yet, so fall back to the live provider. Without this the overlay
	// stayed invisible until the first Analyze -- which is exactly when the ROI
	// still needs to be drawn on screen.
	if (auto *provider = context->GetCore().project->VideoProvider();
		provider && provider->GetWidth() > 0 && provider->GetHeight() > 0) {
		width = provider->GetWidth();
		height = provider->GetHeight();
		return true;
	}
	return false;
}

Vector2D VisualToolMotionTrack::MouseToStorage(
	wxMouseEvent const& event) const {
	int sw = 0, sh = 0;
	if (!StorageDims(sw, sh))
		return Vector2D();
	// video_res is the display-area size, so display -> storage scales by
	// storage/video_res (same direction as VisualToolBase::ToScriptCoords).
	double const sx = video_res.X() > 0 ? double(sw) / video_res.X() : 0.0;
	double const sy = video_res.Y() > 0 ? double(sh) / video_res.Y() : 0.0;
	wxPoint const mpos = event.GetPosition();
	Vector2D const mouse(float(mpos.x), float(mpos.y));
	return Vector2D(float((mouse.X() - video_pos.X()) * float(sx)),
					float((mouse.Y() - video_pos.Y()) * float(sy)));
}

void VisualToolMotionTrack::PushRoi(RoiRect roi, bool reanchor,
									bool anchor_space) {
	int sw = 0, sh = 0;
	if (!StorageDims(sw, sh))
		return;
	if (sw < ::DialogMotionTrack::kMinRoiSide || sh < ::DialogMotionTrack::kMinRoiSide)
		return;

	if (anchor_space) {
		// The rectangle lives in a pose's anchor space, so clamping x/y to the
		// frame would pin the box in place as soon as the pose translated it
		// off the seed position. Analyze re-clamps in storage space after
		// MapRoiToFrame rides the ROI onto the seed frame, so the push accepts
		// the full anchor range. The sides still keep the dialog's accepted
		// range, but without the storage-dimension cap: pose scaling maps
		// anchor lengths onto storage, so the storage size is not an upper
		// bound in this space.
		roi.w = std::clamp(roi.w, ::DialogMotionTrack::kMinRoiSide,
						   ::DialogMotionTrack::kMaxRoiSide);
		roi.h = std::clamp(roi.h, ::DialogMotionTrack::kMinRoiSide,
						   ::DialogMotionTrack::kMaxRoiSide);
		if (!PoseSupportsRoi(drag_pose, roi))
			return;
	}
	else {
		// The dialog's spin controls define the accepted range; clamping here
		// keeps a drag that leaves the frame from being silently truncated to
		// a different rectangle than the one drawn.
		int const max_w = std::min(::DialogMotionTrack::kMaxRoiSide, sw);
		int const max_h = std::min(::DialogMotionTrack::kMaxRoiSide, sh);
		roi.w = std::clamp(roi.w, ::DialogMotionTrack::kMinRoiSide, max_w);
		roi.h = std::clamp(roi.h, ::DialogMotionTrack::kMinRoiSide, max_h);
		roi.x = std::clamp(roi.x, 0, sw - roi.w);
		roi.y = std::clamp(roi.y, 0, sh - roi.h);
	}

	if (auto *dialog = context->GetUI().dialog->Get<::DialogMotionTrack>()) {
		dialog->SetOverlayRoi(roi, reanchor);
		// Nothing else invalidates the display while a drag is in progress, so
		// without this the rectangle only catches up on the next unrelated
		// repaint.
		parent->Render();
	}
}

bool VisualToolMotionTrack::IsResize(RoiGrab grab) {
	switch (grab) {
		case RoiGrab::Nw:
		case RoiGrab::N:
		case RoiGrab::Ne:
		case RoiGrab::E:
		case RoiGrab::Se:
		case RoiGrab::S:
		case RoiGrab::Sw:
		case RoiGrab::W:
			return true;
		default:
			return false;
	}
}

VisualToolMotionTrack::RoiGrab VisualToolMotionTrack::HitTest(
	Vector2D p, RoiRect roi, float tol_x, float tol_y) const {
	float const l = float(roi.x), t = float(roi.y);
	float const r = float(roi.x + roi.w), b = float(roi.y + roi.h);

	bool const near_l = std::abs(p.X() - l) <= tol_x;
	bool const near_r = std::abs(p.X() - r) <= tol_x;
	bool const near_t = std::abs(p.Y() - t) <= tol_y;
	bool const near_b = std::abs(p.Y() - b) <= tol_y;
	bool const span_x = p.X() >= l && p.X() <= r;
	bool const span_y = p.Y() >= t && p.Y() <= b;

	// Corners win over edges so the diagonal grips survive on boxes smaller
	// than the tolerance; edges win over the interior so grabbing the border
	// resizes rather than pans.
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

void VisualToolMotionTrack::OnMouseEvent(wxMouseEvent& event) {
	RoiRect roi;
	wxPoint const point = event.GetPosition();
	Vector2D const next_mouse_pos(point.x, point.y);
	bool const have_roi = CurrentRoi(roi);
	int sw = 0, sh = 0;
	bool const have_dims = StorageDims(sw, sh);

	// Losing the video mid-drag makes MouseToStorage return the invalid
	// sentinel, so stop rather than feeding garbage coordinates forward.
	if (!have_dims && grab != RoiGrab::None)
		EndRoiDrag();

	if (event.LeftDown() && have_dims) {
		mouse_pos = next_mouse_pos;
		// Capture the edit pose: the ride's affine relative to the ROI's
		// anchor when the pressed box rides a tracked frame, identity
		// otherwise. A band (drawing anew) always stays axis-aligned — it is
		// a fresh seed.
		drag_pose = EditPose{};
		auto snap = SnapshotForOverlay();
		if (snap && !snap->samples.empty() && have_roi) {
			int const current_frame =
				context->GetCore().videoController->GetPresentedFrameN();
			if (current_frame >= snap->samples.front().frame && current_frame <= snap->samples.back().frame) {
				if (auto const *s = FindSample(snap->samples, current_frame);
					s && s->status == TrackStatus::Ok) {
					if (auto const anchor = RoiAnchor(); anchor.valid)
						drag_pose = PoseFromAnchorRide(*s, anchor, snap->model);
				}
			}
		}

		// Hit testing and dragging run in the ROI's anchor space, where the
		// dialog ROI is axis-aligned even when the on-screen quad is rotated.
		Vector2D const press = MouseToStorage(event);
		auto const p = PoseToSeed(drag_pose, press);
		float const tol_x = video_res.X() > 0
								? kHandleTolPx * float(sw) / float(video_res.X())
								: 0.f;
		float const tol_y = video_res.Y() > 0
								? kHandleTolPx * float(sh) / float(video_res.Y())
								: 0.f;
		RoiGrab const hit =
			have_roi && p && PoseSupportsRoi(drag_pose, roi)
				? HitTest(*p, roi, tol_x, tol_y)
				: RoiGrab::None;
		// The frame test is on the untransformed storage point: the pose maps
		// into anchor space, where an ROI riding off the frame is expected to
		// sit outside it, so testing `p` would reject legitimate presses.
		bool const on_frame = visual_tool_roi_press_policy::PressIsOnFrame(
			press.X(), press.Y(), sw, sh);
		if (IsResize(hit)) {
			grab = hit;
			fixed_left = float(roi.x);
			fixed_top = float(roi.y);
			fixed_right = float(roi.x + roi.w);
			fixed_bottom = float(roi.y + roi.h);
		}
		else if (hit == RoiGrab::Move) {
			// Grab inside the box moves it; grab on a grip resizes it; grab
			// anywhere else draws a new one.
			grab = RoiGrab::Move;
			drag_offset = Vector2D(p->X() - roi.x, p->Y() - roi.y);
		}
		else if (visual_tool_roi_press_policy::PressMayStartBand(on_frame)) {
			grab = RoiGrab::Band;
			drag_pose = EditPose{};
			band_start = press;
			// Commit to the new box straight away: a click with no drag should
			// still land a rectangle at the minimum size rather than nothing.
			PushRoi(RoiRect{int(std::lround(band_start.X())),
							int(std::lround(band_start.Y())), 0, 0},
					/*reanchor=*/true, /*anchor_space=*/false);
		}
		else {
			// A press on a letterbox bar that grabbed nothing: leave the ROI
			// and its anchor exactly as they were, and take no capture, so the
			// display keeps its usual handling of the click.
			event.Skip();
			return;
		}
		// Keep receiving motion after the pointer leaves the display, so a drag
		// towards the frame edge does not freeze half-finished.
		if (!parent->HasCapture())
			parent->CaptureMouse();
		event.Skip(false);
		return;
	}

	bool const update = event.Dragging() || (event.LeftUp() && next_mouse_pos != mouse_pos);
	auto finish_update = [&] {
		mouse_pos = next_mouse_pos;
		if (event.LeftUp())
			EndRoiDrag();
		event.Skip(false);
	};
	if (update && grab == RoiGrab::Band) {
		Vector2D const p = MouseToStorage(event);
		Vector2D const a = ClampToFrame(band_start, sw, sh);
		Vector2D const b = ClampToFrame(p, sw, sh);
		// Anchor stays put and either corner may lead, so normalise instead of
		// assuming the drag runs down-right.
		int const x0 = int(std::lround(std::min(a.X(), b.X())));
		int const y0 = int(std::lround(std::min(a.Y(), b.Y())));
		int const x1 = int(std::lround(std::max(a.X(), b.X())));
		int const y1 = int(std::lround(std::max(a.Y(), b.Y())));
		PushRoi(RoiRect{x0, y0, x1 - x0, y1 - y0},
				/*reanchor=*/true, /*anchor_space=*/false);
		finish_update();
		return;
	}

	if (update && grab == RoiGrab::Move && have_roi) {
		auto const p = PoseToSeed(drag_pose, MouseToStorage(event));
		if (!p) {
			finish_update();
			return;
		}
		// The pushed rectangle is in the anchor's space while a pose is
		// active, plain storage coordinates otherwise; PushRoi clamps it in
		// the matching space.
		PushRoi(RoiRect{.x = static_cast<int>(std::lround(p->X() - drag_offset.X())),
						.y = static_cast<int>(std::lround(p->Y() - drag_offset.Y())),
						.w = roi.w,
						.h = roi.h},
				/*reanchor=*/drag_pose.identity,
				/*anchor_space=*/!drag_pose.identity);
		finish_update();
		return;
	}

	if (update && IsResize(grab)) {
		// Anchor-space point: the posed grips map onto the axis-aligned dialog
		// ROI. With a pose active the rectangle lives in that space, so PushRoi
		// must not clamp it to the frame (anchor_space below); an identity pose
		// yields plain storage coordinates and clamps as before.
		auto const p = PoseToSeed(drag_pose, MouseToStorage(event));
		if (!p) {
			finish_update();
			return;
		}
		bool const follow_l =
			grab == RoiGrab::Nw || grab == RoiGrab::W || grab == RoiGrab::Sw;
		bool const follow_r =
			grab == RoiGrab::Ne || grab == RoiGrab::E || grab == RoiGrab::Se;
		bool const follow_t =
			grab == RoiGrab::Nw || grab == RoiGrab::N || grab == RoiGrab::Ne;
		bool const follow_b =
			grab == RoiGrab::Sw || grab == RoiGrab::S || grab == RoiGrab::Se;
		float const l = follow_l ? p->X() : fixed_left;
		float const r = follow_r ? p->X() : fixed_right;
		float const t = follow_t ? p->Y() : fixed_top;
		float const b = follow_b ? p->Y() : fixed_bottom;
		// The pointer may cross a fixed edge mid-drag, so normalise instead of
		// assuming which side leads; PushRoi enforces the minimum side from
		// there, which grows the box back towards the mouse.
		int const x0 = int(std::lround(std::min(l, r)));
		int const x1 = int(std::lround(std::max(l, r)));
		int const y0 = int(std::lround(std::min(t, b)));
		int const y1 = int(std::lround(std::max(t, b)));
		PushRoi(RoiRect{x0, y0, x1 - x0, y1 - y0},
				/*reanchor=*/drag_pose.identity,
				/*anchor_space=*/!drag_pose.identity);
		finish_update();
		return;
	}

	if (event.LeftUp() && grab != RoiGrab::None) {
		EndRoiDrag();
		event.Skip(false);
		return;
	}
	event.Skip();
}

void VisualToolMotionTrack::EndRoiDrag() {
	grab = RoiGrab::None;
	if (parent->HasCapture())
		parent->ReleaseMouse();
}

void VisualToolMotionTrack::OnMouseCaptureLost(wxMouseCaptureLostEvent& event) {
	// Alt-tab or a modal stealing focus mid-drag must not leave the tool
	// rewriting the ROI on every later mouse move.
	grab = RoiGrab::None;
	VisualToolBase::OnMouseCaptureLost(event);
}
void VisualToolMotionTrack::DrawWith(VideoOverlayDrawContext& draw) {
	int sw = 0, sh = 0;
	if (!StorageDims(sw, sh) || sw <= 0 || sh <= 0)
		return;
	// Drawing goes the other way from MouseToStorage: storage -> display
	// scales by video_res/storage (as in VisualToolBase::FromScriptCoords).
	double const scale_x = double(video_res.X()) / sw;
	double const scale_y = double(video_res.Y()) / sh;

	wxColour const line =
		to_wx(OPT_GET("Colour/Visual Tools/Lines Primary")->GetColor());
	draw.SetLineColour(line, 1.0f, 2);
	draw.SetFillColour(wxColour(0, 0, 0), 0.0f);

	auto snap = SnapshotForOverlay();
	// Trajectory and failure marks only make sense while the presented frame
	// is inside the tracked span; outside it they are noise over the video.
	int current_frame = -1;
	bool in_range = false;
	if (snap && !snap->samples.empty()) {
		current_frame = context->GetCore().videoController->GetPresentedFrameN();
		in_range = current_frame >= snap->samples.front().frame && current_frame <= snap->samples.back().frame;
	}

	// Ghost of the last built apply plan: what Apply would write, drawn under
	// everything else in the secondary tool colour.
	if (auto *dialog = context->GetUI().dialog->Get<::DialogMotionTrack>()) {
		if (auto plan = dialog->CapturePlanPreview();
			plan && !plan->paths.empty()) {
			wxColour const ghost =
				to_wx(OPT_GET("Colour/Visual Tools/Lines Secondary")->GetColor());
			draw.SetLineColour(ghost, 0.8f, 1);
			draw.SetFillColour(ghost, 0.8f);
			for (auto const& path : plan->paths) {
				std::vector<Vector2D> pts;
				pts.reserve(path.size());
				for (auto const& p : path)
					pts.emplace_back(
						float(video_pos.X() + p.x * scale_x),
						float(video_pos.Y() + p.y * scale_y));
				if (pts.size() >= 2)
					draw.DrawLineStrip(pts.data(), pts.size());
				for (auto const& p : pts)
					draw.DrawRectangle(
						Vector2D(p.X() - 2, p.Y() - 2),
						Vector2D(p.X() + 2, p.Y() + 2));
			}
		}
	}

	RoiRect roi;
	if (CurrentRoi(roi)) {
		// The box rides the trajectory at the presented frame when that frame
		// has an Ok sample and the ROI's anchor defines a ride mapping: the
		// pose maps the anchor space onto the tracked affine, so scrubbing
		// shows the tracking quality directly and the box keeps whatever
		// offset the user dragged it to. While a drag is in progress the box
		// shows the editing target under the drag's captured pose instead, so
		// the box visibly follows the gesture. Without an anchor (or outside
		// the tracked span) the dialog ROI is plain storage coordinates and
		// draws as-is -- the same rectangle Analyze would seed.
		TrackSample const *ride = nullptr;
		if (in_range) {
			auto const *s = FindSample(snap->samples, current_frame);
			if (s && s->status == TrackStatus::Ok)
				ride = s;
		}

		EditPose pose;
		if (grab != RoiGrab::None) {
			pose = drag_pose;
		}
		else if (ride) {
			if (auto const anchor = RoiAnchor(); anchor.valid)
				pose = PoseFromAnchorRide(*ride, anchor, snap->model);
		}

		// Corner + edge-midpoint grip positions in seed space — the same
		// points OnMouseEvent hit-tests.
		Vector2D const seed_pts[8] = {
			Vector2D(float(roi.x), float(roi.y)),
			Vector2D(roi.x + (roi.w - 1) / 2.0f, float(roi.y)),
			Vector2D(float(roi.x + roi.w), float(roi.y)),
			Vector2D(float(roi.x + roi.w), roi.y + (roi.h - 1) / 2.0f),
			Vector2D(float(roi.x + roi.w), float(roi.y + roi.h)),
			Vector2D(roi.x + (roi.w - 1) / 2.0f, float(roi.y + roi.h)),
			Vector2D(float(roi.x), float(roi.y + roi.h)),
			Vector2D(float(roi.x), roi.y + (roi.h - 1) / 2.0f)};
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
			// A valid projective domain keeps the entire quad finite; no
			// individual corner falls back to the untransformed ROI space.
			Vector2D pts[8];
			for (int i = 0; i < 8; ++i)
				pts[i] = Vector2D(
					static_cast<float>(video_pos.X() + storage_pts[i].X() * scale_x),
					static_cast<float>(video_pos.Y() + storage_pts[i].Y() * scale_y));
			Vector2D const quad[4] = {pts[0], pts[2], pts[4], pts[6]};
			draw.SetFillColour(wxColour(0, 0, 0), 0.0f);
			draw.DrawPolygon(quad, 4);
			draw.SetFillColour(line, 1.0f);
			for (auto const& p : pts)
				draw.DrawRectangle(
					Vector2D(p.X() - kHandleHalfPx, p.Y() - kHandleHalfPx),
					Vector2D(p.X() + kHandleHalfPx, p.Y() + kHandleHalfPx));
			box_top_left = pts[0];
		}
		else if (valid_roi) {
			// Identity pose: the raw dialog rectangle, exactly the storage
			// coordinates CommitAnalyzeRequest would seed.
			Vector2D const p1(float(video_pos.X() + roi.x * scale_x),
							  float(video_pos.Y() + roi.y * scale_y));
			Vector2D const p2(p1.X() + float(roi.w * scale_x),
							  p1.Y() + float(roi.h * scale_y));
			draw.SetFillColour(wxColour(0, 0, 0), 0.0f);
			draw.DrawRectangle(p1, p2);
			draw.SetFillColour(line, 1.0f);
			for (auto const& storage_pt : storage_pts) {
				Vector2D const p(
					static_cast<float>(video_pos.X() + storage_pt.X() * scale_x),
					static_cast<float>(video_pos.Y() + storage_pt.Y() * scale_y));
				draw.DrawRectangle(
					Vector2D(p.X() - kHandleHalfPx, p.Y() - kHandleHalfPx),
					Vector2D(p.X() + kHandleHalfPx, p.Y() + kHandleHalfPx));
			}
			box_top_left = p1;
		}

		// Per-frame readout above the box: scrub feedback without opening
		// anything. Only meaningful inside the tracked span and not while
		// the box is being edited.
		if (valid_roi && grab == RoiGrab::None && in_range) {
			auto const *s = FindSample(snap->samples, current_frame);
			char label[32] = {0};
			if (s && s->status == TrackStatus::Ok)
				std::snprintf(label, sizeof(label), "ncc %.2f", s->confidence);
			else if (s && s->status == TrackStatus::Failed)
				std::snprintf(label, sizeof(label), "lost");
			if (label[0]) {
				VideoOverlayTextStyle style;
				style.colour = line;
				wxSize const extent = draw.MeasureText(label, style);
				draw.DrawText(label, int(box_top_left.X()),
							  int(box_top_left.Y()) - extent.GetY() - 4, style);
			}
		}
	}

	if (!snap || snap->samples.empty() || !in_range)
		return;

	// Already-tracked frames draw solid; future frames are dimmed, the usual
	// past/future split tracker UIs use while scrubbing.
	std::vector<Vector2D> past;
	std::vector<Vector2D> future;
	for (auto const& s : snap->samples) {
		if (s.status != TrackStatus::Ok)
			continue;
		Vector2D const pt(float(video_pos.X() + s.center_x * scale_x),
						  float(video_pos.Y() + s.center_y * scale_y));
		(s.frame <= current_frame ? past : future).push_back(pt);
	}
	if (past.size() == 1 && !future.empty())
		// At the very first tracked frame the solid side is a single point
		// and nothing would draw; extend it with the next sample so the
		// path stays visible there.
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
		float const cx = float(video_pos.X() + s.center_x * scale_x);
		float const cy = float(video_pos.Y() + s.center_y * scale_y);
		constexpr float r = 5.f;
		draw.DrawLine(Vector2D(cx - r, cy - r), Vector2D(cx + r, cy + r));
		draw.DrawLine(Vector2D(cx - r, cy + r), Vector2D(cx + r, cy - r));
	}
}

void VisualToolMotionTrack::Draw() {
	// The legacy GL pass calls Draw(); only the Skia overlay pass calls
	// DrawOverlay(), and Skia is off by default. Leaving this empty made the ROI
	// box invisible in every default configuration.
	LegacyVideoOverlayDrawContext draw(gl, *gl_text);
	DrawWith(draw);
}

void VisualToolMotionTrack::DrawOverlay(
	VideoOverlayDrawContext& context_draw) {
	DrawWith(context_draw);
}
