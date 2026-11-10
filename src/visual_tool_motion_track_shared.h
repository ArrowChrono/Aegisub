#pragma once

#include "motion_track/session.h"
#include "vector2d.h"

#include <libaegisub/color.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class VideoOverlayDrawContext;

namespace aegisub::motion_track {
struct MotionTrackApplyPlan;
}

namespace aegisub::visual_motion_track {

inline constexpr int RoiMinSide = 8;
inline constexpr int RoiMaxSide = 512;

struct EditPose {
	motion_track::TrackTransform forward;
	motion_track::TrackTransform inverse;
	bool identity = true;
	bool valid = true;
};

struct Viewport {
	Vector2D origin{0, 0};
	Vector2D extent{0, 0};
	int storage_width = 0;
	int storage_height = 0;
};

struct FrameIdentity {
	std::uint64_t source_id = 0;
	std::uint64_t generation = 0;
	int frame = -1;
};

struct PlanPreviewPoint {
	float x = 0.f;
	float y = 0.f;
};

struct PlanPreviewData {
	std::vector<std::vector<PlanPreviewPoint>> paths;
};

[[nodiscard]] std::shared_ptr<const PlanPreviewData> BuildPlanPreview(
	motion_track::MotionTrackApplyPlan const& plan, int storage_width, int storage_height,
	int script_width, int script_height);

struct OverlaySnapshot {
	FrameIdentity identity;
	Viewport viewport;
	std::optional<motion_track::RoiRect> roi;
	motion_track::RoiRideAnchor anchor;
	std::shared_ptr<const motion_track::MotionTrackSnapshot> track;
	std::shared_ptr<const PlanPreviewData> plan;
	std::optional<EditPose> editing_pose;
};

enum class RoiGrab : std::uint8_t { None,
									Move,
									Band,
									Nw,
									N,
									Ne,
									E,
									Se,
									S,
									Sw,
									W };

struct RoiEdit {
	motion_track::RoiRect roi;
	bool reanchor = true;
};

struct InteractionResult {
	bool handled = false;
	std::optional<RoiEdit> edit;
};

[[nodiscard]] EditPose PoseFromAnchorRide(motion_track::TrackSample const& ride,
										  motion_track::RoiRideAnchor const& anchor, motion_track::TrackModel model);
[[nodiscard]] std::optional<Vector2D> PoseToStorage(EditPose const& pose, Vector2D point);
[[nodiscard]] std::optional<Vector2D> PoseToAnchor(EditPose const& pose, Vector2D point);
[[nodiscard]] bool PoseSupportsRoi(EditPose const& pose, motion_track::RoiRect roi);
[[nodiscard]] EditPose CurrentPose(OverlaySnapshot const& snapshot);
[[nodiscard]] std::optional<Vector2D> CanvasToStorage(Viewport const& viewport, Vector2D point);
[[nodiscard]] RoiGrab HitTest(Vector2D point, motion_track::RoiRect roi, float tolerance_x, float tolerance_y);

class RoiController {
	RoiGrab grab = RoiGrab::None;
	EditPose pose;
	FrameIdentity identity;
	int storage_width = 0;
	int storage_height = 0;
	Vector2D last_point{0, 0};
	Vector2D drag_offset{0, 0};
	Vector2D band_start{0, 0};
	float fixed_left = 0.f, fixed_top = 0.f, fixed_right = 0.f, fixed_bottom = 0.f;

	[[nodiscard]] std::optional<RoiEdit> Normalize(motion_track::RoiRect roi) const;
	[[nodiscard]] bool Matches(OverlaySnapshot const& snapshot) const;

	public:
	[[nodiscard]] InteractionResult Press(OverlaySnapshot const& snapshot, Vector2D point);
	[[nodiscard]] InteractionResult Move(OverlaySnapshot const& snapshot, Vector2D point);
	[[nodiscard]] InteractionResult Release(OverlaySnapshot const& snapshot, Vector2D point);
	void CaptureLost();
	[[nodiscard]] bool Active() const { return grab != RoiGrab::None; }
	[[nodiscard]] RoiGrab Grab() const { return grab; }
	[[nodiscard]] std::optional<EditPose> CapturedPose() const {
		return Active() ? std::optional(pose) : std::nullopt;
	}
};

struct OverlayStyle {
	agi::Color primary;
	agi::Color secondary;
};

void DrawOverlay(VideoOverlayDrawContext& draw, OverlaySnapshot const& snapshot, OverlayStyle const& style);

}
