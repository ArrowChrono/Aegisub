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

#include "gl_text.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "selection_controller.h"
#include "video_display.h"
#include "video_overlay_draw_context.h"
#include "video_overlay_draw_context_legacy_gl.h"

#include <libaegisub/color.h>
#include <libaegisub/format.h>
#include <libaegisub/make_unique.h>

#include <algorithm>

VisualToolCross::VisualToolCross(VideoDisplay *parent, agi::Context *context)
	: VisualTool<VisualDraggableFeature>(parent, context), gl_text(parent->CreateTextRenderer()), coordinate_font_size_opt(OPT_GET("Tool/Visual/Coordinate Font Size")) {
	connections.push_back(OPT_SUB("Tool/Visual/Coordinate Font Size", [=](agi::OptionValue const&) { parent->Render(); }));
}

// Out of line because the OpenGLText member is only forward declared in the
// header. The cursor is the display's to manage; see GetIdleCursor.
VisualToolCross::~VisualToolCross() = default;

void VisualToolCross::OnDoubleClick() {
	Vector2D d = ToScriptCoords(mouse_pos) - GetLinePosition(active_line);

	auto core = c->GetCore();
	for (auto line : core.selectionController->GetSelectedSet()) {
		Vector2D p1, p2;
		int t1, t2;
		if (GetLineMove(line, p1, p2, t1, t2)) {
			if (t1 > 0 || t2 > 0)
				SetOverride(line, "\\move", agi::format("(%s,%s,%d,%d)", Text(p1 + d), Text(p2 + d), t1, t2));
			else
				SetOverride(line, "\\move", agi::format("(%s,%s)", Text(p1 + d), Text(p2 + d)));
		}
		else
			SetOverride(line, "\\pos", "(" + Text(GetLinePosition(line) + d) + ")");

		if (Vector2D org = GetLineOrigin(line))
			SetOverride(line, "\\org", "(" + Text(org + d) + ")");
	}

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

	// Draw cross
	context.SetInvert();
	context.SetLineColour(*wxWHITE, 1.0f, 1);
	float lines[] = {
		0.f, mouse_pos.Y(),
		canvas_size.X(), mouse_pos.Y(),
		mouse_pos.X(), 0.f,
		mouse_pos.X(), canvas_size.Y()
	};
	context.DrawLines(2, lines, 4);
	context.ClearInvert();

	std::string mouse_text = Text(ToScriptCoords(shift_down ? 2 * video_pos + video_res - mouse_pos : mouse_pos));

	int font_size = coordinate_font_size_opt->GetInt();
	font_size = std::min(72, std::max(6, font_size));
	VideoOverlayTextStyle text_style;
	text_style.face = "Verdana";
	text_style.size = font_size;
	text_style.bold = true;
	text_style.colour = *wxWHITE;
	wxSize const extent = context.MeasureText(mouse_text, text_style);
	int const tw = extent.GetWidth();
	int const th = extent.GetHeight();

	// Place the text in the corner of the cross closest to the center of the video
	int dx = mouse_pos.X();
	int dy = mouse_pos.Y();
	if (dx > canvas_size.X() / 2)
		dx -= tw + 4;
	else
		dx += 4;

	if (dy < canvas_size.Y() / 2)
		dy += 3;
	else
		dy -= th + 3;

	context.DrawText(mouse_text, dx, dy, text_style);
}

std::string VisualToolCross::Text(Vector2D v) {
	return video_res.X() > script_res.X() ? v.Str() : v.DStr();
}
