#include "visual_tool_motion_track.h"

#include "async_video_provider.h"
#include "dialog_motion_track.h"
#include "dialog_manager.h"
#include "gl_text.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "project.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_overlay_draw_context_legacy_gl.h"

#include <wx/event.h>

namespace roi = aegisub::visual_motion_track;

static_assert(roi::RoiMinSide == DialogMotionTrack::kMinRoiSide);
static_assert(roi::RoiMaxSide == DialogMotionTrack::kMaxRoiSide);

VisualToolMotionTrack::VisualToolMotionTrack(VideoDisplay *parent, agi::Context *context)
	: VisualToolBase(parent, context), context(context), gl_text(parent->CreateTextRenderer()) {}

VisualToolMotionTrack::~VisualToolMotionTrack() = default;

roi::OverlaySnapshot VisualToolMotionTrack::CaptureOverlay() const {
	roi::OverlaySnapshot snapshot;
	snapshot.viewport.origin = video_pos;
	snapshot.viewport.extent = video_res;
	snapshot.identity.frame = context->GetCore().videoController->GetPresentedFrameN();
	snapshot.editing_pose = roi_controller.CapturedPose();
	if (auto *dialog = context->GetUI().dialog->Get<DialogMotionTrack>()) {
		snapshot.track = dialog->CaptureForOverlay();
		snapshot.plan = dialog->CapturePlanPreview();
		snapshot.roi = dialog->OverlayRoi();
		snapshot.anchor = dialog->OverlayRoiAnchor();
	}
	if (auto *provider = context->GetCore().project->VideoProvider()) {
		snapshot.identity.source_id = reinterpret_cast<std::uintptr_t>(provider);
		snapshot.identity.generation = provider->GetRawVideoIdentity().generation;
		snapshot.viewport.storage_width = provider->GetWidth();
		snapshot.viewport.storage_height = provider->GetHeight();
	}
	if (snapshot.track && snapshot.track->storage_width > 0 && snapshot.track->storage_height > 0) {
		snapshot.viewport.storage_width = snapshot.track->storage_width;
		snapshot.viewport.storage_height = snapshot.track->storage_height;
	}
	return snapshot;
}

void VisualToolMotionTrack::PushRoi(roi::RoiEdit const& edit) {
	if (auto *dialog = context->GetUI().dialog->Get<DialogMotionTrack>()) {
		dialog->SetOverlayRoi(edit.roi, edit.reanchor);
		parent->Render();
	}
}

void VisualToolMotionTrack::OnMouseEvent(wxMouseEvent& event) {
	auto const snapshot = CaptureOverlay();
	auto const position = event.GetPosition();
	Vector2D const point(position.x, position.y);
	if (roi_controller.Active() && !roi::CanvasToStorage(snapshot.viewport, point))
		EndRoiDrag();
	roi::InteractionResult result;
	if (event.LeftDown())
		result = roi_controller.Press(snapshot, point);
	else if (event.LeftUp())
		result = roi_controller.Release(snapshot, point);
	else if (event.Dragging())
		result = roi_controller.Move(snapshot, point);
	if (result.edit)
		PushRoi(*result.edit);
	if (result.handled) {
		mouse_pos = point;
		if (roi_controller.Active()) {
			if (!parent->HasCapture())
				parent->CaptureMouse();
		}
		else if (parent->HasCapture())
			parent->ReleaseMouse();
	}
	event.Skip(!result.handled);
}

void VisualToolMotionTrack::EndRoiDrag() {
	roi_controller.CaptureLost();
	if (parent->HasCapture())
		parent->ReleaseMouse();
}

void VisualToolMotionTrack::OnMouseCaptureLost(wxMouseCaptureLostEvent& event) {
	roi_controller.CaptureLost();
	VisualToolBase::OnMouseCaptureLost(event);
}

void VisualToolMotionTrack::DrawWith(VideoOverlayDrawContext& draw) {
	roi::DrawOverlay(draw, CaptureOverlay(), {.primary = OPT_GET("Colour/Visual Tools/Lines Primary")->GetColor(), .secondary = OPT_GET("Colour/Visual Tools/Lines Secondary")->GetColor()});
}

void VisualToolMotionTrack::Draw() {
	LegacyVideoOverlayDrawContext draw(gl, *gl_text);
	DrawWith(draw);
}

void VisualToolMotionTrack::DrawOverlay(VideoOverlayDrawContext& context_draw) {
	DrawWith(context_draw);
}
