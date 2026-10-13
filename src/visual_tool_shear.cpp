#include "visual_tool_shear.h"

#include "ass_dialogue.h"
#include "ass_tag_scanner.h"
#include "compat.h"
#include "gl_text.h"
#include "include/aegisub/context.h"
#include "numeric_utils.h"
#include "options.h"
#include "selection_controller.h"
#include "video_display.h"
#include "video_overlay_draw_context_legacy_gl.h"

#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace {
constexpr float kLever = 80.f;
constexpr float kRailOffset = 100.f;
constexpr float kMargin = 160.f;

char const *Tag(VisualShearAxis axis) {
	return axis == VisualShearAxis::X ? "\\fax" : "\\fay";
}

bool HasAnimatedShear(std::string_view body, unsigned depth = 0) {
	using namespace aegisub::ass_tag_scanner;
	bool animated = false;
	ScanRawTags(body, [&](RawTag const& tag) {
		if (depth && (NameHasPrefix(tag.name, "fax") || NameHasPrefix(tag.name, "fay")))
			animated = true;
		if (NameHasPrefix(tag.name, "t") && tag.has_paren)
			animated |= depth >= 32 || HasAnimatedShear(tag.args, depth + 1);
	});
	return animated;
}

bool HasStaticShear(AssDialogue const& line) {
	using namespace aegisub::ass_tag_scanner;
	auto const blocks = line.ParseTags();
	bool fax_seen = false;
	bool fay_seen = false;
	for (std::size_t index = 0; index < blocks.size(); ++index) {
		auto const& block = blocks[index];
		if (block->GetType() != AssBlockType::OVERRIDE) {
			continue;
		}
		auto const& body = block->GetRawText();
		if (HasAnimatedShear(body)) return false;
		bool unsupported = false;
		ScanRawTags(body, [&](RawTag const& tag) {
			bool const fax = NameHasPrefix(tag.name, "fax");
			bool const fay = NameHasPrefix(tag.name, "fay");
			bool const shear = fax || fay;
			bool const reset = NameHasPrefix(tag.name, "r");
			if ((index != 0 && (shear || reset)) || (reset && (fax_seen || fay_seen)))
				unsupported = true;
			if ((fax && fax_seen) || (fay && fay_seen))
				unsupported = true;
			if (shear && (tag.has_paren || (!tag.bytes.starts_with("\\fax") && !tag.bytes.starts_with("\\fay"))))
				unsupported = true;
			if (shear && !std::isfinite(static_cast<float>(ArgToDouble(tag.name.substr(3)))))
				unsupported = true;
			fax_seen |= fax;
			fay_seen |= fay;
		});
		if (unsupported) return false;
	}
	return true;
}
}

VisualToolShear::VisualToolShear(VideoDisplay *parent, agi::Context *context)
	: VisualTool<VisualDraggableFeature>(parent, context), text(parent->CreateTextRenderer()) {
	connections.emplace_back(context->GetCore().selectionController->AddSelectionListener(
		&VisualToolShear::OnSelectionChanged, this));
	connections.emplace_back(OPT_SUB("Tool/Visual/Coordinate Font Size", [this](agi::OptionValue const&) { this->parent->Render(); }));
}

VisualToolShear::~VisualToolShear() = default;

void VisualToolShear::OnSelectionChanged() {
	bool const cancelled = CancelInteraction(true);
	active_feature = nullptr;
	initial_values.clear();
	DoRefresh();
	if (cancelled) parent->RenderNow();
}

std::string VisualToolShear::ValidateSelection() const {
	if (!active_line) return {};
	for (auto *line : c->GetCore().selectionController->GetSelectedSet()) {
		if (!HasStaticShear(*line))
			return from_wx(_("Shear editing requires static shear in the initial override block."));
	}
	return {};
}

void VisualToolShear::UpdateHandles() {
	if (!fax_handle || !fay_handle) return;
	Vector2D const minimum = video_pos + Vector2D(10, 10);
	Vector2D const maximum = video_pos + video_res - Vector2D(10, 10);
	fax_handle->pos = (center + Vector2D(shear.X() * kLever, -kRailOffset)).Max(minimum).Min(maximum);
	fay_handle->pos = (center + Vector2D(kRailOffset, shear.Y() * kLever)).Max(minimum).Min(maximum);
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
	Vector2D const margin(std::min(kMargin, video_res.X() / 2), std::min(kMargin, video_res.Y() / 2));
	center = FromScriptCoords(GetLinePosition(active_line)).Max(video_pos + margin).Min(video_pos + video_res - margin);
	if (!fax_handle) {
		fax_handle = new VisualDraggableFeature;
		fax_handle->type = DRAG_BIG_SQUARE;
		features.push_back(*fax_handle);
		fay_handle = new VisualDraggableFeature;
		fay_handle->type = DRAG_BIG_CIRCLE;
		features.push_back(*fay_handle);
	}
	UpdateHandles();
}

bool VisualToolShear::InitializeDrag(VisualDraggableFeature *feature) {
	if (!active_line || !ValidateSelection().empty()) return false;
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
	float const change = (drag_axis == VisualShearAxis::X ? delta.X() : delta.Y()) / kLever;
	for (auto const& [line, value] : initial_values)
		SetOverride(line, Tag(drag_axis), float_to_string(value + change));
	float fax, fay;
	GetLineShear(active_line, fax, fay);
	shear = Vector2D(fax, fay);
	UpdateHandles();
}

bool VisualToolShear::Nudge(Vector2D direction, VisualNudgeMagnitude magnitude) {
	if (IsInteracting() || !active_line || !ValidateSelection().empty()) return false;
	VisualShearAxis const axis = direction.X() != 0 ? VisualShearAxis::X : VisualShearAxis::Y;
	auto const step = static_cast<float>(GetNudgeStep(
		"Tool/Visual/Nudge/Shear Step", "Tool/Visual/Nudge/Shear Step Large", magnitude));
	float const change = (axis == VisualShearAxis::X ? direction.X() : direction.Y()) * step;
	for (auto *line : c->GetCore().selectionController->GetSelectedSet()) {
		float fax, fay;
		GetLineShear(line, fax, fay);
		SetOverride(line, Tag(axis), float_to_string((axis == VisualShearAxis::X ? fax : fay) + change));
	}
	DoRefresh();
	CommitNudge(_("shearing"));
	return true;
}

bool VisualToolShear::CanReset(VisualShearAxis) const {
	return !IsInteracting() && active_line && ValidateSelection().empty();
}

bool VisualToolShear::Reset(VisualShearAxis axis) {
	if (!CanReset(axis)) return false;
	command_session.ResetCommitId();
	auto reset_commit_id = agi::make_scope_exit([this] { command_session.ResetCommitId(); });
	SetSelectedOverride(Tag(axis), "0");
	CommitAndRefresh(_("shear reset"));
	return true;
}

void VisualToolShear::Draw() {
	if (!active_line) return;
	LegacyVideoOverlayDrawContext context(gl, *text);
	VideoOverlayTextStyle style;
	style.size = static_cast<int>(OPT_GET("Tool/Visual/Coordinate Font Size")->GetInt());
	style.colour = to_wx(line_color_primary_opt->GetColor());
	if (!diagnostic.empty()) {
		context.DrawText(diagnostic, static_cast<int>(video_pos.X()) + 8, static_cast<int>(video_pos.Y()) + 8, style);
		return;
	}
	context.SetLineColour(to_wx(line_color_secondary_opt->GetColor()), 0.7f, 1);
	context.DrawLine(center + Vector2D(-120, -100), center + Vector2D(120, -100));
	context.DrawLine(center + Vector2D(100, -120), center + Vector2D(100, 120));
	context.DrawLine(center + Vector2D(0, -106), center + Vector2D(0, -94));
	context.DrawLine(center + Vector2D(94, 0), center + Vector2D(106, 0));
	DrawAllFeatures(context);
	context.DrawText("fax " + float_to_string(shear.X()), static_cast<int>(center.X()) - 120, static_cast<int>(center.Y()) - 126, style);
	context.DrawText("fay " + float_to_string(shear.Y()), static_cast<int>(center.X()) + 110, static_cast<int>(center.Y()) + 24, style);
	context.DrawText(from_wx(_("Arrows: shear   Alt: large step   Ctrl+Home: fax=0   Ctrl+End: fay=0")), static_cast<int>(video_pos.X()) + 8, static_cast<int>(video_pos.Y() + video_res.Y()) - 24, style);
}
