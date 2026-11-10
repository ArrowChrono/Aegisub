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

#include "visual_tool_vector_clip.h"

#include "ass_dialogue.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "perf_trace.h"
#include "selection_controller.h"
#include "utils.h"
#include "video_overlay_draw_context_opengl.h"

#include <libaegisub/make_unique.h>

#include <algorithm>
#include <wx/toolbar.h>

/// Button IDs
enum {
	BUTTON_DRAG = 1300,
	BUTTON_LINE,
	BUTTON_BICUBIC,
	BUTTON_CONVERT,
	BUTTON_INSERT,
	BUTTON_REMOVE,
	BUTTON_FREEHAND,
	BUTTON_FREEHAND_SMOOTH,
	BUTTON_MOVE,
	BUTTON_LAST // Leave this at the end and don't use it
};

static_assert(VisualToolVectorClip::kSubModeCount == BUTTON_LAST - BUTTON_DRAG,
	"vector clip sub-mode count must match toolbar button range");

VisualToolVectorClip::VisualToolVectorClip(VideoDisplay *parent, agi::Context *context)
: VisualTool<VisualToolVectorClipDraggableFeature>(parent, context)
{
}

void VisualToolVectorClip::SetToolbar(wxToolBar *toolBar) {
	if (this->toolBar)
		this->toolBar->Unbind(wxEVT_TOOL, &VisualToolVectorClip::OnSubTool, this);
	this->toolBar = toolBar;

	toolBar->AddSeparator();
	const int icon_size = GetVideoUiIconSize(toolBar, OPT_GET("App/Toolbar Icon Size")->GetInt());
	toolBar->SetToolBitmapSize(wxSize(icon_size, icon_size));

#define ICON(name) wxBitmapBundle::FromBitmap(CMD_ICON_GET(name, wxLayout_Default, icon_size))
	toolBar->AddTool(BUTTON_DRAG, _("Drag"), ICON(visual_vector_clip_drag), _("Drag control points"), wxITEM_CHECK);
	toolBar->AddTool(BUTTON_LINE, _("Line"), ICON(visual_vector_clip_line), _("Appends a line"), wxITEM_CHECK);
	toolBar->AddTool(BUTTON_MOVE, _("Move"), ICON(visual_vector_clip_move), _("Appends a move point"), wxITEM_CHECK);
	toolBar->AddTool(BUTTON_BICUBIC, _("Bicubic"), ICON(visual_vector_clip_bicubic), _("Appends a bezier bicubic curve"), wxITEM_CHECK);
	toolBar->AddSeparator();
	toolBar->AddTool(BUTTON_CONVERT, _("Convert"), ICON(visual_vector_clip_convert), _("Converts a segment between line and bicubic"), wxITEM_CHECK);
	toolBar->AddTool(BUTTON_INSERT, _("Insert"), ICON(visual_vector_clip_insert), _("Inserts a control point"), wxITEM_CHECK);
	toolBar->AddTool(BUTTON_REMOVE, _("Remove"), ICON(visual_vector_clip_remove), _("Removes a control point"), wxITEM_CHECK);
	toolBar->AddSeparator();
	toolBar->AddTool(BUTTON_FREEHAND, _("Freehand"), ICON(visual_vector_clip_freehand), _("Draws a freehand shape"), wxITEM_CHECK);
	toolBar->AddTool(BUTTON_FREEHAND_SMOOTH, _("Freehand smooth"), ICON(visual_vector_clip_freehand_smooth), _("Draws a smoothed freehand shape"), wxITEM_CHECK);
	toolBar->ToggleTool(BUTTON_DRAG, true);
	toolBar->Realize();
	toolBar->Show(true);
	toolBar->Bind(wxEVT_TOOL, &VisualToolVectorClip::OnSubTool, this);
	SetMode(features.empty());
#undef ICON
}

void VisualToolVectorClip::OnSubTool(wxCommandEvent &event) {
	SetMode(event.GetId() - BUTTON_DRAG);
}

void VisualToolVectorClip::SetMode(int new_mode) {
	mode = new_mode;

	// Toolbar is null until SetToolbar runs; command/hotkey paths may call earlier.
	if (!toolBar)
		return;

	// Manually enforce radio behavior as we want one selection in the bar
	// rather than one per group
	for (int i = BUTTON_DRAG; i < BUTTON_LAST; i++)
		toolBar->ToggleTool(i, i == new_mode + BUTTON_DRAG);
}

bool VisualToolVectorClip::SetSubMode(int new_mode) {
	if (new_mode < 0 || new_mode >= kSubModeCount)
		return false;
	SetMode(new_mode);
	return true;
}

void VisualToolVectorClip::Draw() {
	OpenGLVideoOverlayDrawContext context(gl);
	DrawOverlay(context);
}

void VisualToolVectorClip::DrawOverlay(VideoOverlayDrawContext &context) {
	if (!active_line || spline.empty()) return;
	assert(overlay_features.size() == features.size());
	std::size_t index = 0;
	for (auto& feature : features) {
		auto& overlay = overlay_features[index++];
		overlay.position = feature.pos;
		overlay.active = &feature == active_feature;
		overlay.selected = sel_features.count(&feature) != 0;
	}
	aegisub::visual_vector_clip::DrawOverlay(context, {
														  .spline = spline,
														  .features = overlay_features,
														  .mode = static_cast<aegisub::visual_vector_clip::Mode>(mode),
														  .inverse = inverse,
														  .video_position = video_pos,
														  .video_size = video_res,
														  .pointer = mouse_pos,
														  .pointer_valid = static_cast<bool>(mouse_pos),
														  .holding = holding && static_cast<bool>(drag_start),
														  .drag_start = drag_start,
														  .line_colour = line_color_primary_opt->GetColor(),
														  .highlight_primary = highlight_color_primary_opt->GetColor(),
														  .highlight_secondary = highlight_color_secondary_opt->GetColor(),
														  .shaded_alpha = static_cast<float>(shaded_area_alpha_opt->GetDouble()),
													  });
}

void VisualToolVectorClip::MakeFeature(size_t idx) {
	auto const start = overlay_features.size();
	aegisub::visual_vector_clip::AppendFeatures(spline, idx, overlay_features);
	for (std::size_t i = start; i < overlay_features.size(); ++i) {
		auto const& source = overlay_features[i];
		auto feature = agi::make_unique<Feature>();
		feature->idx = source.curve_index;
		feature->point = source.point;
		feature->pos = source.position;
		feature->type = source.square ? DRAG_SMALL_SQUARE : DRAG_SMALL_CIRCLE;
		features.push_back(*feature.release());
	}
}

void VisualToolVectorClip::MakeFeatures() {
	overlay_features.clear();
	sel_features.clear();
	features.clear();
	active_feature = nullptr;
	for (size_t i = 0; i < spline.size(); ++i)
		MakeFeature(i);
}

std::string VisualToolVectorClip::BuildClipValue() const {
	return aegisub::visual_vector_clip::BuildClipValue(spline);
}

bool VisualToolVectorClip::Save(std::string const& value) {
	auto core = c->GetCore();
	bool changed = false;
	for (auto line : core.selectionController->GetSelectedSet()) {
		// This check is technically not correct as it could be outside of an
		// override block... but that's rather unlikely
		bool has_iclip = line->Text.get().find("\\iclip") != std::string::npos;
		std::string const previous_text = line->Text.get();
		SetOverride(line, has_iclip ? "\\iclip" : "\\clip", value);
		changed |= previous_text != line->Text.get();
	}
	return changed;
}

void VisualToolVectorClip::Commit(wxString message) {
	std::string const value = BuildClipValue();
	// Selection-only events must not rewrite other selected subtitle lines.
	if (value == last_committed_clip)
		return;

	auto const& selected = c->GetCore().selectionController->GetSelectedSet();
	bool const has_selection = !selected.empty();
	perf_trace::VideoUiDurationScope total_trace(
		"visual_tool.vector_clip.commit",
		mode,
		static_cast<int>(selected.size()));
	bool changed = false;
	{
		perf_trace::VideoUiDurationScope save_trace(
			"visual_tool.vector_clip.save",
			static_cast<int>(spline.size()),
			static_cast<int>(selected.size()));
		changed = Save(value);
	}
	if (changed) {
		VisualToolBase::Commit(message);
	}
	else {
		ClearChangedLines();
	}
	if (has_selection)
		last_committed_clip = value;
}

void VisualToolVectorClip::UpdateDrag(Feature *feature) {
	spline.MovePoint(spline.begin() + feature->idx, feature->point, feature->pos);
}

bool VisualToolVectorClip::InitializeDrag(Feature *feature) {
	if (mode != 5) return true;
	aegisub::visual_vector_clip::RemovePoint(spline, feature->idx, feature->point);
	active_feature = nullptr;
	MakeFeatures();
	Commit(_("delete control point"));
	return false;
}

bool VisualToolVectorClip::InitializeHold() {
	using namespace aegisub::visual_vector_clip;
	auto const submode = static_cast<Mode>(mode);
	if (submode == Mode::Drag) {
		box_added.clear();
		return true;
	}
	if (submode == Mode::Line || submode == Mode::Bicubic) {
		Append(spline, submode, mouse_pos);
		sel_features.clear();
		MakeFeature(spline.size() - 1);
		UpdateHold();
		return true;
	}
	if (submode == Mode::Convert || submode == Mode::Insert) {
		if (auto const closest = FindClosest(spline, mouse_pos)) {
			if (submode == Mode::Convert)
				Convert(spline, *closest);
			else
				Insert(spline, *closest);
		}
		MakeFeatures();
		Commit();
		return false;
	}
	if (submode == Mode::Freehand || submode == Mode::FreehandSmooth) {
		sel_features.clear();
		features.clear();
		overlay_features.clear();
		active_feature = nullptr;
		BeginFreehand(spline, mouse_pos);
		return true;
	}
	if (submode == Mode::Move) {
		Append(spline, submode, mouse_pos);
		MakeFeatures();
		Commit(_("append move point"));
	}
	return false;
}

void VisualToolVectorClip::UpdateHold() {
	// Box selection
	if (mode == 0) {
		std::set<Feature *> boxed_features;
		Vector2D p1 = drag_start.Min(mouse_pos);
		Vector2D p2 = drag_start.Max(mouse_pos);
		for (auto& feature : features) {
			if (aegisub::visual_vector_clip::InBox(p1, p2, feature.pos))
				boxed_features.insert(&feature);
		}

		// Keep track of which features were selected by the box selection so
		// that only those are deselected if the user is holding ctrl
		std::set_difference(boxed_features.begin(), boxed_features.end(), sel_features.begin(), sel_features.end(),
			std::inserter(box_added, end(box_added)));

		std::copy(boxed_features.begin(), boxed_features.end(), std::inserter(sel_features, end(sel_features)));

		std::vector<Feature *> to_deselect;
		std::set_difference(box_added.begin(), box_added.end(), boxed_features.begin(), boxed_features.end(), std::back_inserter(to_deselect));
		for (auto feature : to_deselect)
			sel_features.erase(feature);

		return;
	}

	auto const submode = static_cast<aegisub::visual_vector_clip::Mode>(mode);
	if (mode == 1 || mode == 2) {
		aegisub::visual_vector_clip::UpdateAppend(spline, submode, mouse_pos);
		if (mode == 1)
			features.back().pos = mouse_pos;
		else
			MakeFeatures();
	}
	else if (mode == 6 || mode == 7) {
		auto const original_size = spline.size();
		aegisub::visual_vector_clip::UpdateFreehand(spline, submode, mouse_pos, !holding);
		if (spline.size() != original_size)
			MakeFeature(spline.size() - 1);
		if (!holding) {
			SetMode(0);
			MakeFeatures();
		}
	}
}

void VisualToolVectorClip::DoRefresh() {
	if (!active_line) {
		spline.clear();
		last_committed_clip.clear();
		MakeFeatures();
		return;
	}

	spline.SetCoordinateSystem({.video_position = video_pos, .video_size = video_res, .script_size = script_res});
	int scale;
	std::string vect = GetLineVectorClip(active_line, scale, inverse);
	spline.SetScale(scale);
	spline.DecodeFromAss(vect);
	last_committed_clip = BuildClipValue();

	MakeFeatures();
}
