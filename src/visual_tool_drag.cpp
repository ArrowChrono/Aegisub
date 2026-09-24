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

/// @file visual_tool_drag.cpp
/// @brief Position all visible subtitles by dragging visual typesetting tool
/// @ingroup visual_ts

#include "visual_tool_drag.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "perf_trace.h"
#include "project.h"
#include "selection_controller.h"
#include "utils.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_overlay_draw_context.h"
#include "video_overlay_helpers.h"
#include "visual_tool_drag_snapshot.h"

#include <libaegisub/format.h>
#include <libaegisub/make_unique.h>

#include <algorithm>
#include <optional>
#include <unordered_set>
#include <wx/toolbar.h>

static const DraggableFeatureType DRAG_ORIGIN = DRAG_BIG_TRIANGLE;
static const DraggableFeatureType DRAG_START = DRAG_BIG_SQUARE;
static const DraggableFeatureType DRAG_END = DRAG_BIG_CIRCLE;

std::shared_ptr<const VisualToolRenderSnapshot> VisualToolDrag::CaptureRenderSnapshot(
	std::shared_ptr<const VisualToolRenderContext> const& context) const {
	auto snapshot = std::make_shared<VisualToolDragSnapshot>(context);
	snapshot->grid_colour = to_wx(line_color_secondary_opt->GetColor()).GetRGB();
	snapshot->line_colour = to_wx(line_color_primary_opt->GetColor()).GetRGB();
	auto const base_fill = to_wx(highlight_color_primary_opt->GetColor()).GetRGB();
	auto const active_fill = to_wx(highlight_color_secondary_opt->GetColor()).GetRGB();
	for (auto const& feature : features) {
		auto const fill = &feature == active_feature                            ? active_fill
						  : sel_features.count(const_cast<Feature *>(&feature)) ? snapshot->line_colour
																				: base_fill;
		snapshot->features.push_back({.type = feature.type, .pos = feature.pos, .parent = feature.parent ? std::optional<Vector2D>{feature.parent->pos} : std::nullopt, .fill_colour = fill, .line_id = feature.line->Id, .layer = feature.layer});
	}
	return snapshot;
}

VisualToolDrag::VisualToolDrag(VideoDisplay *parent, agi::Context *context)
: VisualTool<VisualToolDragDraggableFeature>(parent, context)
{
	auto core = c->GetCore();
	connections.push_back(core.selectionController->AddSelectionListener(&VisualToolDrag::OnSelectedSetChanged, this));
	connections.push_back(core.project->AddTimecodesListener([this](agi::vfr::Framerate const&) {
		OnFileChanged();
		this->parent->Render();
	}));
	auto const& sel_set = core.selectionController->GetSelectedSet();
	selection = sel_set;
}

void VisualToolDrag::SetToolbar(wxToolBar *tb) {
	if (toolbar)
		toolbar->Unbind(wxEVT_TOOL, &VisualToolDrag::OnSubTool, this);
	toolbar = tb;
	const int icon_size = GetVideoUiIconSize(toolbar, OPT_GET("App/Toolbar Icon Size")->GetInt());
	toolbar->SetToolBitmapSize(wxSize(icon_size, icon_size));
	toolbar->AddSeparator();
	move_pos_button = toolbar->AddTool(wxID_ANY, _("Toggle between \\move and \\pos"),
		wxBitmapBundle::FromBitmap(CMD_ICON_GET(visual_move_conv_move, wxLayout_Default, icon_size)),
		_("Toggle between \\move and \\pos"), wxITEM_CHECK)->GetId();
	button_is_move = true;
	UpdateToggleButtons();
	toolbar->Realize();
	toolbar->Show(true);

	toolbar->Bind(wxEVT_TOOL, &VisualToolDrag::OnSubTool, this);
}

void VisualToolDrag::UpdateToggleButtons() {
	if (!toolbar) return;

	bool to_move = true;
	if (active_line) {
		Vector2D p1, p2;
		int t1, t2;
		to_move = !GetLineMove(active_line, p1, p2, t1, t2);
	}

	if (to_move != button_is_move) {
		const int icon_size = GetVideoUiIconSize(toolbar, OPT_GET("App/Toolbar Icon Size")->GetInt());
		if (to_move)
			toolbar->SetToolNormalBitmap(move_pos_button, wxBitmapBundle::FromBitmap(CMD_ICON_GET(visual_move_conv_move, wxLayout_Default, icon_size)));
		else
			toolbar->SetToolNormalBitmap(move_pos_button, wxBitmapBundle::FromBitmap(CMD_ICON_GET(visual_move_conv_pos, wxLayout_Default, icon_size)));
		button_is_move = to_move;
	}
	// The checked state represents the current line mode, while the icon
	// represents the conversion performed by the next click.
	toolbar->ToggleTool(move_pos_button, !to_move);
}

void VisualToolDrag::OnSubTool(wxCommandEvent &) {
	// Toggle \move <-> \pos
	auto core = c->GetCore();
	VideoController *vc = core.videoController.get();
	for (auto line : selection) {
		Vector2D p1, p2;
		int t1, t2;

		bool has_move = GetLineMove(line, p1, p2, t1, t2);

		if (has_move)
			SetOverride(line, "\\pos", p1.PStr());
		else {
			p1 = GetLinePosition(line);
			// Round the start and end times to exact frames
			int start = vc->TimeAtFrame(vc->FrameAtTime(line->Start, agi::vfr::START)) - line->Start;
			int end = vc->TimeAtFrame(vc->FrameAtTime(line->Start, agi::vfr::END)) - line->Start;
			SetOverride(line, "\\move", agi::format("(%s,%s,%d,%d)", p1.Str(), p1.Str(), start, end));
		}
	}

	CommitAndRefresh();
	UpdateToggleButtons();
}

void VisualToolDrag::OnLineChanged() {
	UpdateToggleButtons();
}

void VisualToolDrag::OnFileChanged() {
	parent->ResetToolPresentation();
	/// @todo it should be possible to preserve the selection in some cases
	features.clear();
	sel_features.clear();
	primary = nullptr;
	active_feature = nullptr;

	RebuildFrameVisibility();
	for (auto& diag : c->GetCore().ass->Events)
		if (frame_visibility.Visible().count(&diag))
			MakeFeatures(&diag);

	UpdateToggleButtons();
}

void VisualToolDrag::OnFrameChanged() {
	perf_trace::VideoUiDurationScope trace(
		"grid_select.visual.frame",
		static_cast<int>(frame_visibility.Visible().size()),
		static_cast<int>(features.size()));

	if (!frame_visibility_valid) {
		OnFileChanged();
		return;
	}

	std::vector<AssDialogue *> entered;
	std::vector<AssDialogue *> exited;
	frame_visibility.Advance(frame_visibility_frame, frame_number, entered, exited);
	frame_visibility_frame = frame_number;
	auto const changed_line_count = entered.size() + exited.size();
	RemoveFeatures(exited);
	AddFeatures(std::move(entered));
	trace.SetDetails(
		static_cast<int>(changed_line_count),
		static_cast<int>(features.size()));
}

void VisualToolDrag::RebuildFrameVisibility() {
	auto core = c->GetCore();
	std::vector<aegisub::visual_frame_visibility::Interval<AssDialogue>> intervals;
	intervals.reserve(core.ass->Events.size());
	for (auto& diag : core.ass->Events) {
		if (diag.Comment)
			continue;
		intervals.push_back({
			core.videoController->FrameAtTime(diag.Start, agi::vfr::START),
			core.videoController->FrameAtTime(diag.End, agi::vfr::END),
			&diag,
		});
	}
	frame_visibility.Rebuild(intervals, frame_number);
	frame_visibility_valid = true;
	frame_visibility_frame = frame_number;
}

void VisualToolDrag::RemoveFeatures(std::vector<AssDialogue *> const& lines) {
	if (lines.empty())
		return;

	std::unordered_set<AssDialogue *> removed_lines(lines.begin(), lines.end());
	if (primary && removed_lines.count(primary->line))
		primary = nullptr;
	for (auto feat = features.begin(); feat != features.end(); ) {
		if (!removed_lines.count(feat->line)) {
			++feat;
			continue;
		}
		if (&*feat == active_feature)
			active_feature = nullptr;
		sel_features.erase(&*feat);
		feat->line = nullptr;
		feat = features.erase(feat);
	}
}

void VisualToolDrag::AddFeatures(std::vector<AssDialogue *> lines) {
	if (lines.empty())
		return;

	std::sort(lines.begin(), lines.end(), [](AssDialogue const* left, AssDialogue const* right) {
		return left->Row < right->Row;
	});
	auto pos = features.begin();
	for (auto *diag : lines) {
		while (pos != features.end() && pos->line && pos->line->Row < diag->Row)
			++pos;
		MakeFeatures(diag, pos);
	}
}

void VisualToolDrag::OnSelectedSetChanged() {
	parent->ResetToolPresentation();
	auto core = c->GetCore();
	auto const& new_sel_set = core.selectionController->GetSelectedSet();
	perf_trace::VideoUiDurationScope trace(
		"grid_select.visual.selection",
		static_cast<int>(new_sel_set.size()),
		static_cast<int>(features.size()));
	std::set<AssDialogue *> selected_feature_lines;
	for (auto *feature : sel_features)
		if (feature->line)
			selected_feature_lines.insert(feature->line);

	bool any_changed = false;
	for (auto it = features.begin(); it != features.end(); ) {
		bool was_selected = selection.count(it->line) != 0;
		bool is_selected = new_sel_set.count(it->line) != 0;
		if (was_selected && !is_selected) {
			sel_features.erase(&*it++);
			any_changed = true;
		}
		else {
			if (is_selected && !was_selected && it->type == DRAG_START
				&& selected_feature_lines.insert(it->line).second) {
				sel_features.insert(&*it);
				any_changed = true;
			}
			++it;
		}
	}

	if (any_changed)
		parent->Render();
	selection = new_sel_set;
}

void VisualToolDrag::Draw() {
	DrawAllFeatures();

	// Load colors from options
	wxColour line_color = to_wx(line_color_primary_opt->GetColor());

	// Draw connecting lines
	for (auto& feature : features) {
		if (feature.type == DRAG_START) continue;

		Feature *p2 = &feature;
		Feature *p1 = feature.parent;

		// Move end marker has an arrow; origin doesn't
		bool has_arrow = p2->type == DRAG_END;
		int arrow_len = has_arrow ? 10 : 0;

		// Don't show the connecting line if the features are very close
		Vector2D direction = p2->pos - p1->pos;
		if (direction.SquareLen() < (20 + arrow_len) * (20 + arrow_len)) continue;

		direction = direction.Unit();
		// Get the start and end points of the line
		Vector2D start = p1->pos + direction * 10;
		Vector2D end = p2->pos - direction * (10 + arrow_len);

		if (has_arrow) {
			gl.SetLineColour(line_color, 0.8f, 2);

			// Arrow line
			gl.DrawLine(start, end);

			// Arrow head
			Vector2D t_half_base_w = Vector2D(-direction.Y(), direction.X()) * 4;
			gl.DrawTriangle(end + direction * arrow_len, end + t_half_base_w, end - t_half_base_w);
		}
		// Draw dashed line
		else {
			gl.SetLineColour(line_color, 0.5f, 2);
			gl.DrawDashedLine(start, end, 6);
		}
	}
}

void VisualToolDrag::DrawOverlay(VideoOverlayDrawContext &context) {
	DrawAllFeatures(context);

	wxColour const line_color = to_wx(line_color_primary_opt->GetColor());

	for (auto& feature : features) {
		if (feature.type == DRAG_START) continue;

		Feature *p2 = &feature;
		Feature *p1 = feature.parent;

		bool const has_arrow = p2->type == DRAG_END;
		int const arrow_len = has_arrow ? 10 : 0;

		Vector2D direction = p2->pos - p1->pos;
		if (direction.SquareLen() < (20 + arrow_len) * (20 + arrow_len))
			continue;

		direction = direction.Unit();
		Vector2D const start = p1->pos + direction * 10;
		Vector2D const end = p2->pos - direction * (10 + arrow_len);

		if (has_arrow) {
			context.SetLineColour(line_color, 0.8f, 2);
			context.DrawLine(start, end);

			Vector2D const t_half_base_w = Vector2D(-direction.Y(), direction.X()) * 4;
			context.DrawTriangle(end + direction * arrow_len, end + t_half_base_w, end - t_half_base_w);
		}
		else {
			context.SetLineColour(line_color, 0.5f, 2);
			video_overlay_helpers::DrawDashedLine(context, start, end, 6);
		}
	}
}

void VisualToolDrag::MakeFeatures(AssDialogue *diag) {
	MakeFeatures(diag, features.end());
}

void VisualToolDrag::MakeFeatures(AssDialogue *diag, feature_list::iterator pos) {
	Vector2D p1 = FromScriptCoords(GetLinePosition(diag));

	// Create \pos feature
	auto feat = agi::make_unique<Feature>();
	auto parent = feat.get();
	feat->pos = p1;
	feat->type = DRAG_START;
	feat->line = diag;

	if (selection.count(diag))
		sel_features.insert(feat.get());
	features.insert(pos, *feat.release());

	Vector2D p2;
	int t1, t2;

	// Create move destination feature
	if (GetLineMove(diag, p1, p2, t1, t2)) {
		feat = agi::make_unique<Feature>();
		feat->pos = FromScriptCoords(p2);
		feat->layer = 1;
		feat->type = DRAG_END;
		feat->time = t2;
		feat->line = diag;
		feat->parent = parent;

		parent->time = t1;
		parent->parent = feat.get();

		features.insert(pos, *feat.release());
	}

	// Create org feature
	if (Vector2D org = GetLineOrigin(diag)) {
		feat = agi::make_unique<Feature>();
		feat->pos = FromScriptCoords(org);
		feat->layer = -1;
		feat->type = DRAG_ORIGIN;
		feat->time = 0;
		feat->line = diag;
		feat->parent = parent;
		features.insert(pos, *feat.release());
	}
}

bool VisualToolDrag::InitializeDrag(Feature *feature) {
	primary = feature;

	// Set time of clicked feature to the current frame and shift all other
	// selected features by the same amount
	if (feature->type != DRAG_ORIGIN) {
		auto core = c->GetCore();
		int time = core.videoController->TimeAtFrame(frame_number) - feature->line->Start;
		int change = time - feature->time;

		for (auto feat : sel_features)
			feat->time += change;
	}
	return true;
}

void VisualToolDrag::UpdateDrag(Feature *feature) {
	if (feature->type == DRAG_ORIGIN) {
		SetOverride(feature->line, "\\org", ToScriptCoords(feature->pos).PStr());
		return;
	}

	Feature *end_feature = feature->parent;
	if (feature->type == DRAG_END)
		std::swap(feature, end_feature);

	if (!feature->parent)
		SetOverride(feature->line, "\\pos", ToScriptCoords(feature->pos).PStr());
	else
		SetOverride(feature->line, "\\move", agi::format("(%s,%s,%d,%d)"
			, ToScriptCoords(feature->pos).Str()
			, ToScriptCoords(end_feature->pos).Str()
			, feature->time , end_feature->time));
}

void VisualToolDrag::OnDoubleClick() {
	RestoreDoubleClickSelection();
	Vector2D d = ToScriptCoords(mouse_pos) - (primary ? ToScriptCoords(primary->pos) : GetLinePosition(active_line));

	auto core = c->GetCore();
	for (auto line : core.selectionController->GetSelectedSet()) {
		Vector2D p1, p2;
		int t1, t2;
		if (GetLineMove(line, p1, p2, t1, t2)) {
			if (t1 > 0 || t2 > 0)
				SetOverride(line, "\\move", agi::format("(%s,%s,%d,%d)", (p1 + d).Str(), (p2 + d).Str(), t1, t2));
			else
				SetOverride(line, "\\move", agi::format("(%s,%s)", (p1 + d).Str(), (p2 + d).Str()));
		}
		else
			SetOverride(line, "\\pos", (GetLinePosition(line) + d).PStr());

		if (Vector2D org = GetLineOrigin(line))
			SetOverride(line, "\\org", (org + d).PStr());
	}

	CommitAndRefresh(_("positioning"));
}
