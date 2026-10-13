#pragma once

#include "visual_feature.h"
#include "visual_tool.h"

#include <map>
#include <memory>
#include <string>

class OpenGLText;

enum class VisualShearAxis {
	X,
	Y,
};

class VisualToolShear final : public VisualTool<VisualDraggableFeature> {
	std::unique_ptr<OpenGLText> text;
	VisualDraggableFeature *fax_handle = nullptr;
	VisualDraggableFeature *fay_handle = nullptr;
	Vector2D shear;
	Vector2D center;
	VisualShearAxis drag_axis = VisualShearAxis::X;
	std::map<AssDialogue *, float> initial_values;
	std::string diagnostic;

	std::string ValidateSelection() const;
	void OnSelectionChanged();
	void UpdateHandles();
	void DoRefresh() override;
	bool InitializeDrag(VisualDraggableFeature *feature) override;
	void UpdateDrag(VisualDraggableFeature *feature) override;
	void Draw() override;

	public:
	VisualToolShear(VideoDisplay *parent, agi::Context *context);
	~VisualToolShear() override;
	bool CanReset(VisualShearAxis axis) const;
	bool Reset(VisualShearAxis axis);
	bool Nudge(Vector2D direction, VisualNudgeMagnitude magnitude) override;
	bool SupportsNudge() const override { return true; }
	std::string GetHotkeyContext() const override { return "Visual Shear"; }
};
