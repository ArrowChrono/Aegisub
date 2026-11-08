// Copyright (c) 2011, Thomas Goyne <plorkyeran@aegisub.org>
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

/// @file visual_tool_cross.cpp
/// @brief Crosshair double-click-to-position visual typesetting tool
/// @ingroup visual_ts

#include "visual_tool_cross.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "gl_text.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "selection_controller.h"
#include "video_display.h"
#include "video_overlay_draw_context.h"
#include "video_overlay_draw_context_legacy_gl.h"
#include "visual_ass_edit.h"
#include "visual_tool_cross_shared.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

VisualToolCross::VisualToolCross(VideoDisplay *parent, agi::Context *context)
	: VisualTool<VisualDraggableFeature>(parent, context), gl_text(parent->CreateTextRenderer()), coordinate_font_size_opt(OPT_GET("Tool/Visual/Coordinate Font Size")) {
	connections.push_back(OPT_SUB("Tool/Visual/Coordinate Font Size", [=](agi::OptionValue const&) { parent->Render(); }));
}

// Out of line because the OpenGLText member is only forward declared in the
// header. The cursor is the display's to manage; see GetIdleCursor.
VisualToolCross::~VisualToolCross() = default;

void VisualToolCross::OnDoubleClick() {
	using namespace aegisub::visual_edit;
	Viewport const viewport{.video_position = video_pos, .video_size = video_res, .script_size = script_res, .canvas_size = canvas_size};
	if (!aegisub::visual_cross::IsValidGeometry(mouse_pos, viewport))
		return;
	auto core = c->GetCore();
	auto const active = ReadLine(*active_line, core.ass->GetStyle(active_line->Style), script_res);
	Vector2D const delta = ToScriptCoords(mouse_pos) - active.position;
	std::vector<std::pair<AssDialogue *, std::vector<OverrideEdit>>> plans;
	for (auto line : core.selectionController->GetSelectedSet()) {
		auto const source = line == active_line ? active : ReadLine(*line, core.ass->GetStyle(line->Style), script_res);
		auto plan = aegisub::visual_cross::PlanTranslation(source, delta, video_res.X() > script_res.X());
		if (!plan)
			return;
		plans.emplace_back(line, std::move(*plan));
	}
	for (auto const& [line, plan] : plans)
		for (auto const& edit : plan)
			SetOverride(line, edit.tag, edit.value);
	Commit(_("positioning"));
}

void VisualToolCross::Draw() {
	LegacyVideoOverlayDrawContext context(gl, *gl_text);
	DrawWithContext(context);
}

void VisualToolCross::DrawOverlay(VideoOverlayDrawContext &context) {
	DrawWithContext(context);
}

void VisualToolCross::DrawWithContext(VideoOverlayDrawContext &context) {
	if (!mouse_pos) return;
	auto const font_size = static_cast<int>(std::clamp<std::int64_t>(coordinate_font_size_opt->GetInt(), 6, 72));
	aegisub::visual_cross::DrawOverlay(context, mouse_pos,
									   {.video_position = video_pos, .video_size = video_res, .script_size = script_res, .canvas_size = canvas_size},
									   shift_down, font_size);
}
