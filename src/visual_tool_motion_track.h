#pragma once

#include "visual_tool.h"
#include "visual_tool_motion_track_shared.h"

#include <memory>

class wxMouseEvent;
class wxKeyEvent;
class OpenGLText;
class VideoDisplay;

class VisualToolMotionTrack final : public VisualToolBase {
	agi::Context *context;
	aegisub::visual_motion_track::RoiController roi_controller;
	std::unique_ptr<OpenGLText> gl_text;

	[[nodiscard]] aegisub::visual_motion_track::OverlaySnapshot CaptureOverlay() const;
	void PushRoi(aegisub::visual_motion_track::RoiEdit const& edit);
	void EndRoiDrag();
	void DrawWith(VideoOverlayDrawContext& draw);
	void OnMouseCaptureLost(wxMouseCaptureLostEvent& event) override;

	public:
	VisualToolMotionTrack(VideoDisplay *parent, agi::Context *context);
	~VisualToolMotionTrack();
	void OnMouseEvent(wxMouseEvent& event) override;
	bool OnKeyDown(wxKeyEvent&) override { return false; }
	void Draw() override;
	bool SupportsOverlayContext() const override { return true; }
	void DrawOverlay(VideoOverlayDrawContext& context) override;
};
