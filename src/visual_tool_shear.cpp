#include "visual_tool_shear.h"

#include "ass_dialogue.h"
#include "compat.h"
#include "gl_text.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "selection_controller.h"
#include "video_display.h"
#include "video_overlay_draw_context_legacy_gl.h"
#include "visual_tool_shear_policy.h"

#include <libaegisub/scope_exit.h>

VisualToolShear::VisualToolShear(VideoDisplay *parent, agi::Context *context)
	: VisualTool<VisualDraggableFeature>(parent, context), text(parent->CreateTextRenderer()) {
	connections.emplace_back(context->GetCore().selectionController->AddSelectionListener(
		&VisualToolShear::OnSelectionChanged, this));
	connections.emplace_back(OPT_SUB("Tool/Visual/Coordinate Font Size", [this](agi::OptionValue const&) { this->parent->Render(); }));
}

VisualToolShear::~VisualToolShear() = default;

void VisualToolShear::RetireDerivedSubtitleRowBorrows() noexcept {
	VisualTool<VisualDraggableFeature>::RetireDerivedSubtitleRowBorrows();
	initial_values.clear();
	fax_handle = fay_handle = nullptr;
	features.clear();
	diagnostic.clear();
}

void VisualToolShear::OnSelectionChanged() {
	if (!SubtitleRowsReady()) return;
	bool const cancelled = CancelInteraction(true);
	active_feature = nullptr;
	initial_values.clear();
	DoRefresh();
	if (cancelled) parent->RenderNow();
}

std::string VisualToolShear::ValidateSelection() const {
	if (!active_line) return {};
	for (auto *line : c->GetCore().selectionController->GetSelectedSet()) {
		if (!aegisub::visual_shear::IsStatic(*line))
			return from_wx(_("Shear editing requires static shear in the initial override block."));
	}
	return {};
}

void VisualToolShear::UpdateHandles() {
	if (!fax_handle || !fay_handle) return;
	auto const geometry = aegisub::visual_shear::CalculateGeometry(center, video_pos, video_res, shear);
	fax_handle->pos = geometry.handles[0];
	fay_handle->pos = geometry.handles[1];
}

void VisualToolShear::DoRefresh() {
	diagnostic = ValidateSelection();
	if (!active_line || !diagnostic.empty()) {
		sel_features.clear();
		features.clear();
		fax_handle = fay_handle = nullptr;
		return;
	}
	float fax, fay;
	GetLineShear(active_line, fax, fay);
	shear = Vector2D(fax, fay);
	auto const geometry = aegisub::visual_shear::CalculateGeometry(FromScriptCoords(GetLinePosition(active_line)), video_pos, video_res, shear);
	center = geometry.center;
	if (!fax_handle) {
		fax_handle = new VisualDraggableFeature;
		fax_handle->type = DRAG_BIG_SQUARE;
		features.push_back(*fax_handle);
		fay_handle = new VisualDraggableFeature;
		fay_handle->type = DRAG_BIG_CIRCLE;
		features.push_back(*fay_handle);
	}
	fax_handle->pos = geometry.handles[0];
	fay_handle->pos = geometry.handles[1];
}

bool VisualToolShear::InitializeDrag(VisualDraggableFeature *feature) {
	if (!SubtitleRowsReady() || !active_line || !ValidateSelection().empty()) return false;
	command_session.ResetCommitId();
	drag_axis = feature == fax_handle ? VisualShearAxis::X : VisualShearAxis::Y;
	sel_features = {feature};
	initial_values.clear();
	for (auto *line : c->GetCore().selectionController->GetSelectedSet()) {
		float fax, fay;
		GetLineShear(line, fax, fay);
		initial_values.emplace(line, drag_axis == VisualShearAxis::X ? fax : fay);
	}
	return true;
}

void VisualToolShear::UpdateDrag(VisualDraggableFeature *) {
	Vector2D const delta = mouse_pos - drag_start;
	float const change = aegisub::visual_shear::DragDelta(drag_axis, delta);
	for (auto const& [line, value] : initial_values)
		SetOverride(line, aegisub::visual_shear::Tag(drag_axis), aegisub::visual_shear::FormatValue(value + change));
	float fax, fay;
	GetLineShear(active_line, fax, fay);
	shear = Vector2D(fax, fay);
	UpdateHandles();
}

bool VisualToolShear::Nudge(Vector2D direction, VisualNudgeMagnitude magnitude) {
	if (!SubtitleRowsReady() || IsInteracting() || !active_line || !ValidateSelection().empty()) return false;
	VisualShearAxis const axis = aegisub::visual_shear::NudgeAxis(direction);
	auto const step = static_cast<float>(GetNudgeStep(
		"Tool/Visual/Nudge/Shear Step", "Tool/Visual/Nudge/Shear Step Large", magnitude));
	float const change = aegisub::visual_shear::NudgeDelta(axis, direction, step);
	for (auto *line : c->GetCore().selectionController->GetSelectedSet()) {
		float fax, fay;
		GetLineShear(line, fax, fay);
		SetOverride(line, aegisub::visual_shear::Tag(axis), aegisub::visual_shear::FormatValue((axis == VisualShearAxis::X ? fax : fay) + change));
	}
	DoRefresh();
	CommitNudge(_("shearing"));
	return true;
}

bool VisualToolShear::CanReset(VisualShearAxis) const {
	return SubtitleRowsReady() && !IsInteracting() && active_line && ValidateSelection().empty();
}

bool VisualToolShear::Reset(VisualShearAxis axis) {
	if (!CanReset(axis)) return false;
	command_session.ResetCommitId();
	auto reset_commit_id = agi::make_scope_exit([this] { command_session.ResetCommitId(); });
	SetSelectedOverride(aegisub::visual_shear::Tag(axis), "0");
	CommitAndRefresh(_("shear reset"));
	return true;
}

void VisualToolShear::Draw() {
	if (!active_line) return;
	LegacyVideoOverlayDrawContext context(gl, *text);
	aegisub::visual_shear::OverlayState state;
	state.coefficients = shear;
	state.video_position = video_pos;
	state.video_size = video_res;
	state.primary_colour = line_color_primary_opt->GetColor();
	state.secondary_colour = line_color_secondary_opt->GetColor();
	state.font_size = static_cast<int>(OPT_GET("Tool/Visual/Coordinate Font Size")->GetInt());
	state.diagnostic = diagnostic;
	if (diagnostic.empty()) {
		state.geometry = {.center = center, .handles = {fax_handle->pos, fay_handle->pos}};
		std::array handles{fax_handle, fay_handle};
		for (std::size_t i = 0; i < handles.size(); ++i) {
			auto *feature = handles[i];
			state.handle_fill_colours[i] = feature == active_feature     ? highlight_color_secondary_opt->GetColor()
										   : sel_features.count(feature) ? line_color_primary_opt->GetColor()
																		 : highlight_color_primary_opt->GetColor();
		}
		state.hint = from_wx(_("Arrows: shear   Alt: large step   Ctrl+Home: fax=0   Ctrl+End: fay=0"));
	}
	aegisub::visual_shear::DrawOverlay(context, state);
}
