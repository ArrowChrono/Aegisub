// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
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

/// @file visual_tool.cpp
/// @brief Base class for visual typesetting functions
/// @ingroup visual_ts

#include "visual_tool.h"

#include "ass_compat.h"
#include "ass_dialogue.h"
#include "async_video_provider.h"
#include "ass_file.h"
#include "ass_file_app.h"
#include "ass_style.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "perf_trace.h"
#include "project.h"
#include "selection_controller.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_overlay_draw_context.h"
#include "visual_tool_clip.h"
#include "visual_tool_commit_policy.h"
#include "visual_tool_drag.h"
#include "visual_tool_vector_clip.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/format.h>
#include <libaegisub/of_type_adaptor.h>
#include <libaegisub/scope_exit.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

VisualToolBase::VisualToolBase(VideoDisplay *parent, agi::Context *context)
	: c(context), parent(parent), command_session(context->GetCore().ass.get()), frame_number(c->GetCore().videoController->GetFrameN()), highlight_color_primary_opt(OPT_GET("Colour/Visual Tools/Highlight Primary")), highlight_color_secondary_opt(OPT_GET("Colour/Visual Tools/Highlight Secondary")), line_color_primary_opt(OPT_GET("Colour/Visual Tools/Lines Primary")), line_color_secondary_opt(OPT_GET("Colour/Visual Tools/Lines Secondary")), shaded_area_alpha_opt(OPT_GET("Colour/Visual Tools/Shaded Area Alpha")), file_changed_connection(c->GetCore().ass->AddCommitListener(&VisualToolBase::OnCommit, this)), interaction_render_timer([this] { OnInteractionRenderTimer(); }) {
	auto core = c->GetCore();
	UpdateScriptResolution();
	UpdateLayoutResolution();
	active_line = GetActiveDialogueLine();
	connections.push_back(core.selectionController->AddActiveLineListener(&VisualToolBase::OnActiveLineChanged, this));
	connections.push_back(core.videoController->AddSeekListener(&VisualToolBase::OnSeek, this));
	parent->Bind(wxEVT_MOUSE_CAPTURE_LOST, &VisualToolBase::OnMouseCaptureLost, this);

	// Coalesce keyboard-nudge undos while keys are held/repeated; split after idle.
	// Explicit id so this handler does not receive every timer event on VideoDisplay.
	commit_id_reset_timer_id = wxNewId();
	commit_id_reset_timer.SetOwner(parent, commit_id_reset_timer_id);
	parent->Bind(wxEVT_TIMER, &VisualToolBase::OnCommitIdResetTimer, this, commit_id_reset_timer_id);
}

VisualToolBase::~VisualToolBase() {
	interaction_render_timer.Stop();
	commit_id_reset_timer.Stop();
	parent->Unbind(wxEVT_TIMER, &VisualToolBase::OnCommitIdResetTimer, this, commit_id_reset_timer_id);
	parent->Unbind(wxEVT_MOUSE_CAPTURE_LOST, &VisualToolBase::OnMouseCaptureLost, this);
	CancelInteraction(true);
}

void VisualToolBase::OnInteractionRenderTimer() {
	if (!IsInteracting() || !interaction_render_pacer.IsActive())
		return;

	auto const now = DeadlinePacingPolicy::Clock::now();
	auto const deadline = interaction_render_pacer.NextDeadline();
	if (deadline) {
		perf_trace::ObserveVideoUiDuration(
			"visual_tool.interaction_render.timer_lateness",
			std::chrono::duration<double, std::milli>(now - *deadline).count());
	}
	bool const allowed = interaction_render_pacer.OnTimer(now);
	perf_trace::ObserveVideoUiDuration(
		"visual_tool.interaction_render.timer_fire", 0.0, allowed ? 1 : 0, deadline ? 1 : 0);
	if (allowed)
		RenderInteractionFrame(1);
	else
		ArmInteractionRenderTimer();
}

void VisualToolBase::ArmInteractionRenderTimer() {
	if (interaction_render_timer.IsRunning())
		return;

	auto const deadline = interaction_render_pacer.NextDeadline();
	if (!deadline)
		return;

	auto const remaining = *deadline - DeadlinePacingPolicy::Clock::now();
	auto const delay = std::max<std::int64_t>(
		1,
		std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
	int const delay_ms = static_cast<int>(std::min<std::int64_t>(delay, std::numeric_limits<int>::max()));
	interaction_render_timer.StartAt(*deadline);
	perf_trace::ObserveVideoUiDuration("visual_tool.interaction_render.timer_arm", 0.0, delay_ms);
}

void VisualToolBase::RenderInteractionFrame(int reason) {
	perf_trace::ObserveVideoUiDuration("visual_tool.interaction_render", 0.0, reason);
	parent->RenderNow();
}

void VisualToolBase::BeginInteractionPacing(int selection_count) {
	if (interaction_render_pacer.IsActive())
		return;

	parent->CancelReleaseToolFeedback();
	interaction_render_pacer.Begin(DeadlinePacingPolicy::Clock::now());
	c->GetCore().videoController->BeginVisualSubtitleInteraction();
	interaction_trace_active = true;
	perf_trace::ObserveVideoUiDuration("visual_tool.interaction_begin", 0.0, selection_count);
}

void VisualToolBase::EndInteractionPacing(bool render_final, int selection_count) {
	if (!interaction_render_pacer.IsActive())
		return;

	interaction_render_timer.Stop();
	interaction_render_pacer.Force(DeadlinePacingPolicy::Clock::now());
	interaction_render_pacer.End();
	auto const final_interaction_id = c->GetCore().videoController->EndVisualSubtitleInteraction();
	if (interaction_trace_active) {
		perf_trace::ObserveVideoUiDuration("visual_tool.interaction_end", 0.0, selection_count);
		interaction_trace_active = false;
	}
	if (render_final)
		parent->RenderFinalToolFeedback(final_interaction_id);
}

void VisualToolBase::OnCommitIdResetTimer(wxTimerEvent &) {
	command_session.ResetCommitId();
}

void VisualToolBase::UpdateScriptResolution() {
	int script_w, script_h;
	auto core = c->GetCore();
	core.ass->GetResolution(ScriptResolutionType::PlayRes, script_w, script_h);
	script_res = Vector2D(script_w, script_h);
}

void VisualToolBase::UpdateLayoutResolution() {
	int lw, lh;
	c->GetCore().ass->GetLayoutResolution(lw, lh);
	if (lw <= 0 || lh <= 0) {
		// Fall back to video storage resolution (what libass uses when LayoutRes is unset)
		if (auto *provider = c->GetCore().project->VideoProvider()) {
			lw = provider->GetWidth();
			lh = provider->GetHeight();
		} else {
			// Last resort: use PlayRes
			lw = script_res.X();
			lh = script_res.Y();
		}
	}
	layout_res = Vector2D(lw, lh);
}

void VisualToolBase::OnCommit(int type, AssDialogue const* changed) {
	bool const local_commit = command_session.IsLocalCommitInProgress();
	if (local_commit && !command_session.ShouldObserveLocalCommit())
		return;

	bool const interaction_cancelled = !local_commit && CancelInteraction(true);

	auto *new_active_line = GetActiveDialogueLine();
	bool needs_render = false;
	bool const coordinate_system_changed = type == AssFile::COMMIT_NEW || type & AssFile::COMMIT_SCRIPTINFO;

	if (coordinate_system_changed) {
		active_line = new_active_line;
		UpdateScriptResolution();
		UpdateLayoutResolution();
		OnCoordinateSystemsChanged();
		needs_render = true;
	}

	bool const changed_line_relevant = visual_tool_commit_policy::UsesChangedLineFilter(type)
		&& changed
		&& (changed == active_line
			|| changed == new_active_line
			|| IsDisplayed(changed));
	bool const refresh_any_external_commit = !local_commit
		&& !coordinate_system_changed
		&& ShouldRefreshOnAnyExternalCommit();
	bool const needs_file_refresh = visual_tool_commit_policy::ShouldRefreshFile({
		type,
		local_commit,
		refresh_any_external_commit,
		changed != nullptr,
		changed_line_relevant,
	});

	if (needs_file_refresh) {
		active_line = new_active_line;
		OnFileChanged();
		needs_render = true;
	}

	if (needs_render || interaction_cancelled) {
		if (interaction_cancelled)
			parent->RenderNow();
		else
			parent->Render();
	}
}

void VisualToolBase::OnSeek(int new_frame) {
	parent->CancelReleaseToolFeedback();
	if (frame_number == new_frame) return;
	perf_trace::VideoUiDurationScope trace(
		"grid_select.visual.seek",
		frame_number,
		new_frame);

	frame_number = new_frame;
	OnFrameChanged();

	AssDialogue *new_line = GetActiveDialogueLine();
	if (new_line != active_line) {
		bool const interaction_cancelled = CancelInteraction(true);
		active_line = new_line;
		OnLineChanged();
		if (interaction_cancelled)
			parent->RenderNow();
	}
}

void VisualToolBase::OnMouseCaptureLost(wxMouseCaptureLostEvent &) {
	bool const was_interacting = CancelInteraction(false);
	OnFileChanged();
	if (was_interacting) {
		parent->RenderNow();
	}
	else {
		parent->Render();
	}
}

void VisualToolBase::OnActiveLineChanged(AssDialogue *new_line) {
	bool const displayed = IsDisplayed(new_line);
	perf_trace::VideoUiDurationScope trace(
		"grid_select.visual.active",
		new_line ? 1 : 0,
		displayed ? 1 : 0);
	if (!displayed)
		new_line = nullptr;

	bool const interaction_cancelled = CancelInteraction(true);
	if (new_line != active_line) {
		active_line = new_line;
		OnLineChanged();
		if (interaction_cancelled)
			parent->RenderNow();
		else
			parent->Render();
	}
	else if (interaction_cancelled) {
		parent->RenderNow();
	}
}

bool VisualToolBase::CancelInteraction(bool release_capture) {
	parent->CancelReleaseToolFeedback();
	bool const was_interacting = IsInteracting();
	holding = false;
	dragging = false;
	if (was_interacting) {
		command_session.ResetCommitId();
		EndInteractionPacing(false);
	}
	if (release_capture && parent->HasCapture())
		parent->ReleaseMouse();
	return was_interacting;
}

bool VisualToolBase::IsDisplayed(AssDialogue const* line) const {
	int frame = frame_number;
	if (frame < 0)
		frame = c->GetCore().videoController->GetFrameN();
	auto core = c->GetCore();
	return line
		&& !line->Comment
		&& core.videoController->FrameAtTime(line->Start, agi::vfr::START) <= frame
		&& core.videoController->FrameAtTime(line->End, agi::vfr::END) >= frame;
}

AssDialogue *VisualToolBase::GetCommitTargetLine() const {
	return changed_lines.size() == 1 ? single_changed_line : nullptr;
}

void VisualToolBase::Commit(wxString message) {
	auto const& selected = c->GetCore().selectionController->GetSelectedSet();
	if (changed_lines.empty()) {
		perf_trace::ObserveVideoUiDuration(
			"visual_tool.commit.skipped_no_change",
			0.0,
			static_cast<int>(selected.size()),
			0);
		return;
	}

	if (message.empty())
		message = _("visual typesetting");

	AssDialogue *target_line = GetCommitTargetLine();
	auto clear_changed_lines = agi::make_scope_exit([this] { ClearChangedLines(); });
	perf_trace::VideoUiDurationScope commit_trace(
		"visual_tool.commit",
		static_cast<int>(selected.size()),
		static_cast<int>(changed_lines.size()));
	command_session.Commit(
		from_wx(message),
		AssFile::COMMIT_DIAG_TEXT,
		command_session.GetCommitId(),
		target_line,
		changed_lines);
}

double VisualToolBase::GetNudgeStep(
	char const* option,
	char const* large_option,
	VisualNudgeMagnitude magnitude) {
	return OPT_GET(magnitude == VisualNudgeMagnitude::Large ? large_option : option)
		->GetDouble();
}

void VisualToolBase::CommitNudge(wxString message) {
	if (changed_lines.empty()) {
		auto const& selected = c->GetCore().selectionController->GetSelectedSet();
		perf_trace::ObserveVideoUiDuration(
			"visual_tool.commit.skipped_no_change",
			0.0,
			static_cast<int>(selected.size()),
			1);
		parent->Render();
		return;
	}

	if (message.empty())
		message = _("visual typesetting");

	AssDialogue *target_line = GetCommitTargetLine();
	auto clear_changed_lines = agi::make_scope_exit([this] { ClearChangedLines(); });
	command_session.Commit(
		from_wx(message),
		AssFile::COMMIT_DIAG_TEXT,
		command_session.GetCommitId(),
		target_line,
		changed_lines);
	// Key-repeat merges into one undo; a short idle starts a new group.
	commit_id_reset_timer.Start(500, wxTIMER_ONE_SHOT);
	parent->Render();
}

void VisualToolBase::CommitAndRefresh(wxString message) {
	if (changed_lines.empty()) {
		auto const& selected = c->GetCore().selectionController->GetSelectedSet();
		perf_trace::ObserveVideoUiDuration(
			"visual_tool.commit.skipped_no_change",
			0.0,
			static_cast<int>(selected.size()),
			2);
		return;
	}

	if (message.empty())
		message = _("visual typesetting");

	AssDialogue *target_line = GetCommitTargetLine();
	auto clear_changed_lines = agi::make_scope_exit([this] { ClearChangedLines(); });
	command_session.CommitWithFeedback(
		from_wx(message),
		AssFile::COMMIT_DIAG_TEXT,
		command_session.GetCommitId(),
		target_line,
		changed_lines,
		aegisub::LocalCommitFeedback::ObserveSelf);
}

AssDialogue* VisualToolBase::GetActiveDialogueLine() {
	auto core = c->GetCore();
	AssDialogue *diag = core.selectionController->GetActiveLine();
	if (IsDisplayed(diag))
		return diag;
	return nullptr;
}

void VisualToolBase::SetCanvasSize(int w, int h) {
	canvas_size = Vector2D(w, h);
}

void VisualToolBase::SetDisplayArea(int x, int y, int w, int h) {
	UpdateLayoutResolution();
	if (x == video_pos.X() && y == video_pos.Y() && w == video_res.X() && h == video_res.Y()) return;

	video_pos = Vector2D(x, y);
	video_res = Vector2D(w, h);

	bool const interaction_cancelled = CancelInteraction(true);
	OnDisplayAreaChanged();
	if (interaction_cancelled)
		parent->RenderNow();
}

Vector2D VisualToolBase::ToScriptCoords(Vector2D point) const {
	return (point - video_pos) * script_res / video_res;
}

Vector2D VisualToolBase::FromScriptCoords(Vector2D point) const {
	return (point * video_res / script_res) + video_pos;
}

template<class FeatureType>
VisualTool<FeatureType>::VisualTool(VideoDisplay *parent, agi::Context *context)
: VisualToolBase(parent, context)
{
}

template<class FeatureType>
void VisualTool<FeatureType>::OnMouseEvent(wxMouseEvent &event) {
	bool const interaction_was_active = holding || dragging;
	bool release_mouse = false;
	bool left_click = event.LeftDown();
	bool left_double = event.LeftDClick();
	auto render_tool_feedback = [&] {
		perf_trace::VideoUiDurationScope trace("visual_tool.feedback_request");
		if (trace.IsActive()) {
			int const mouse_flags = (left_click ? 1 : 0) | (left_double ? 2 : 0) | (event.LeftUp() ? 4 : 0) | (event.Moving() ? 8 : 0) | (event.Dragging() ? 16 : 0) | (event.Leaving() ? 32 : 0) | (event.Entering() ? 64 : 0) | (event.LeftIsDown() ? 128 : 0);
			int const interaction_flags = (interaction_was_active ? 1 : 0) | (holding ? 2 : 0) | (dragging ? 4 : 0);
			trace.SetDetails(mouse_flags, interaction_flags);
			perf_trace::ObserveVideoUiDuration("visual_tool.feedback_request.begin", 0.0, mouse_flags, interaction_flags);
		}
		parent->RenderToolFeedback();
	};
	if (left_click || left_double || event.LeftUp() || (interaction_was_active && (event.Moving() || event.Dragging()))) {
		char const *phase = "visual_tool.input.motion";
		if (left_click || left_double)
			phase = "visual_tool.input.down";
		else if (event.LeftUp())
			phase = "visual_tool.input.up";
		perf_trace::ObserveVideoUiDuration(
			phase, 0.0, event.LeftIsDown() ? 1 : 0, interaction_was_active ? 1 : 0);
	}
	shift_down = event.ShiftDown();
	ctrl_down = event.CmdDown();
	alt_down = event.AltDown();

	wxPoint pt = event.GetPosition(); mouse_pos = Vector2D(pt.x, pt.y);

	if (event.Leaving()) {
		mouse_pos = Vector2D();
		render_tool_feedback();
		return;
	}

	if (!dragging) {
		int max_layer = INT_MIN;
		active_feature = nullptr;
		for (auto& feature : features) {
			if (feature.IsMouseOver(mouse_pos) && feature.layer >= max_layer) {
				active_feature = &feature;
				max_layer = feature.layer;
			}
		}
	}

	if (dragging) {
		// continue drag
		if (event.LeftIsDown()) {
			for (auto sel : sel_features)
				sel->UpdateDrag(mouse_pos - drag_start, shift_down);
			for (auto sel : sel_features)
				UpdateDrag(sel);
			Commit();
			ScheduleInteractionRender();
		}
		// end drag
		else {
			dragging = false;

			// mouse didn't move, fiddle with selection
			if (active_feature && !active_feature->HasMoved()) {
				// Don't deselect stuff that was selected in this click's mousedown event
				if (!sel_changed) {
					if (ctrl_down)
						RemoveSelection(active_feature);
					else
						SetSelection(active_feature, true);
				}
			}

			active_feature = nullptr;
			release_mouse = true;
		}
	}
	else if (holding) {
		if (!event.LeftIsDown()) {
			holding = false;
			release_mouse = true;
		}

		UpdateHold();
		Commit();
		ScheduleInteractionRender();

	}
	else if (left_click) {
		// A pending keyboard-nudge idle timer must not ResetCommitId mid-drag.
		commit_id_reset_timer.Stop();
		drag_start = mouse_pos;
		auto core = c->GetCore();
		double_click_selection = core.selectionController->GetSelectedSet();

		// start drag
		if (active_feature) {
			if (!sel_features.count(active_feature)) {
				sel_changed = true;
				SetSelection(active_feature, !ctrl_down);
			}
			else
				sel_changed = false;
			double_click_selection = core.selectionController->GetSelectedSet();

			if (active_feature->line)
				core.selectionController->SetActiveLine(active_feature->line);

			if (InitializeDrag(active_feature)) {
				for (auto sel : sel_features) sel->StartDrag();
				dragging = true;
				parent->CaptureMouse();
			}
		}
		// start hold
		else {
			if (!alt_down && features.size() > 1) {
				sel_features.clear();
				core.selectionController->SetSelectedSet({ core.selectionController->GetActiveLine() });
			}
			if (active_line && InitializeHold()) {
				holding = true;
				parent->CaptureMouse();
			}
		}
	}

	if (active_line && left_double)
		OnDoubleClick();

	bool const interaction_is_active = holding || dragging;
	bool const interaction_started = !interaction_was_active && interaction_is_active;
	bool const interaction_ended = interaction_was_active && !interaction_is_active;
	if (interaction_ended) {
		EndInteractionPacing(true, static_cast<int>(sel_features.size()));
	}
	else if (interaction_started || !interaction_is_active) {
		render_tool_feedback();
	}

	if (interaction_started)
		BeginInteractionPacing(static_cast<int>(sel_features.size()));
	if (release_mouse) {
		parent->ReleaseMouse();
		parent->SetFocus();
	}

	// Only coalesce the changes made in a single drag
	if (!event.LeftIsDown())
		command_session.ResetCommitId();
}

template<class FeatureType>
void VisualTool<FeatureType>::DrawAllFeatures() {
	wxColour grid_color = to_wx(line_color_secondary_opt->GetColor());
	gl.SetLineColour(grid_color, 1.0f, 1);
	wxColour base_fill = to_wx(highlight_color_primary_opt->GetColor());
	wxColour active_fill = to_wx(highlight_color_secondary_opt->GetColor());
	wxColour alt_fill = to_wx(line_color_primary_opt->GetColor());
	for (auto& feature : features) {
		wxColour fill = base_fill;
		if (&feature == active_feature)
			fill = active_fill;
		else if (sel_features.count(&feature))
			fill = alt_fill;
		gl.SetFillColour(fill, 0.3f);
		feature.Draw(gl);
	}
}

template<class FeatureType>
void VisualTool<FeatureType>::DrawAllFeatures(VideoOverlayDrawContext &context) {
	wxColour const grid_color = to_wx(line_color_secondary_opt->GetColor());
	context.SetLineColour(grid_color, 1.0f, 1);
	wxColour const base_fill = to_wx(highlight_color_primary_opt->GetColor());
	wxColour const active_fill = to_wx(highlight_color_secondary_opt->GetColor());
	wxColour const alt_fill = to_wx(line_color_primary_opt->GetColor());
	for (auto& feature : features) {
		wxColour fill = base_fill;
		if (&feature == active_feature)
			fill = active_fill;
		else if (sel_features.count(&feature))
			fill = alt_fill;
		context.SetFillColour(fill, 0.3f);
		feature.Draw(context);
	}
}

template<class FeatureType>
void VisualTool<FeatureType>::RestoreDoubleClickSelection() {
	auto core = c->GetCore();
	core.selectionController->SetSelectedSet(double_click_selection);
	double_click_selection.clear();
}

template<class FeatureType>
void VisualTool<FeatureType>::SetSelection(FeatureType *feat, bool clear) {
	if (clear)
		sel_features.clear();

	if (sel_features.insert(feat).second && feat->line) {
		auto core = c->GetCore();
		Selection sel;
		if (!clear)
			sel = core.selectionController->GetSelectedSet();
		if (sel.insert(feat->line).second)
			core.selectionController->SetSelectedSet(std::move(sel));
	}
}

template<class FeatureType>
void VisualTool<FeatureType>::RemoveSelection(FeatureType *feat) {
	if (!sel_features.erase(feat) || !feat->line) return;
	for (auto sel : sel_features)
		if (sel->line == feat->line) return;

	auto core = c->GetCore();
	auto sel = core.selectionController->GetSelectedSet();

	// Don't deselect the only selected line
	if (sel.size() <= 1) return;

	sel.erase(feat->line);

	// Set the active line to an arbitrary selected line if we just
	// deselected the active line
	AssDialogue *new_active = core.selectionController->GetActiveLine();
	if (feat->line == new_active)
		new_active = *sel.begin();

	core.selectionController->SetSelectionAndActive(std::move(sel), new_active);
}

//////// PARSERS

typedef const std::vector<AssOverrideParameter> * param_vec;

// Find a tag's parameters in a line or return nullptr if it's not found
static param_vec find_tag(std::vector<std::unique_ptr<AssDialogueBlock>>& blocks, std::string const& tag_name) {
	for (auto ovr : blocks | agi::of_type<AssDialogueBlockOverride>()) {
		for (auto const& tag : ovr->Tags) {
			if (tag.Name == tag_name)
				return &tag.Params;
		}
	}

	return nullptr;
}

// Get a Vector2D from the given tag parameters, or Vector2D::Bad() if they are not valid
static Vector2D vec_or_bad(param_vec tag, size_t x_idx, size_t y_idx) {
	if (!tag ||
		tag->size() <= x_idx || tag->size() <= y_idx ||
		(*tag)[x_idx].omitted || (*tag)[y_idx].omitted)
	{
		return Vector2D();
	}
	return Vector2D((*tag)[x_idx].Get<float>(), (*tag)[y_idx].Get<float>());
}

Vector2D VisualToolBase::GetLinePosition(AssDialogue *diag) {
	auto blocks = diag->ParseTags();

	if (Vector2D ret = vec_or_bad(find_tag(blocks, "\\pos"), 0, 1)) return ret;
	if (Vector2D ret = vec_or_bad(find_tag(blocks, "\\move"), 0, 1)) return ret;

	// Get default position
	auto margin = diag->Margin;
	int align = 2;

	auto core = c->GetCore();
	if (AssStyle *style = core.ass->GetStyle(diag->Style)) {
		align = style->alignment;
		for (int i = 0; i < 3; i++) {
			if (margin[i] == 0)
				margin[i] = style->Margin[i];
		}
	}

	param_vec align_tag;
	int ovr_align = 0;
	if ((align_tag = find_tag(blocks, "\\an")))
		ovr_align = (*align_tag)[0].Get<int>(ovr_align);
	else if ((align_tag = find_tag(blocks, "\\a")))
		ovr_align = AssCompat::NormalizeLegacyAssAlignment(
			(*align_tag)[0].Get<int>(0), 0);

	if (ovr_align > 0 && ovr_align <= 9)
		align = ovr_align;

	// Alignment type
	int hor = (align - 1) % 3;
	int vert = (align - 1) / 3;

	// Calculate positions
	int x, y;
	if (hor == 0)
		x = margin[0];
	else if (hor == 1)
		x = (script_res.X() + margin[0] - margin[1]) / 2;
	else
		x = script_res.X() - margin[1];

	if (vert == 0)
		y = script_res.Y() - margin[2];
	else if (vert == 1)
		y = script_res.Y() / 2;
	else
		y = margin[2];

	return Vector2D(x, y);
}

Vector2D VisualToolBase::GetLineOrigin(AssDialogue *diag) {
	auto blocks = diag->ParseTags();
	return vec_or_bad(find_tag(blocks, "\\org"), 0, 1);
}

bool VisualToolBase::GetLineMove(AssDialogue *diag, Vector2D &p1, Vector2D &p2, int &t1, int &t2) {
	auto blocks = diag->ParseTags();

	param_vec tag = find_tag(blocks, "\\move");
	if (!tag)
		return false;

	p1 = vec_or_bad(tag, 0, 1);
	p2 = vec_or_bad(tag, 2, 3);
	// VSFilter actually defaults to -1, but it uses <= 0 to check for default and 0 seems less bug-prone
	t1 = (*tag)[4].Get<int>(0);
	t2 = (*tag)[5].Get<int>(0);

	return p1 && p2;
}

void VisualToolBase::GetLineRotation(AssDialogue *diag, float &rx, float &ry, float &rz) {
	rx = ry = rz = 0.f;

	auto core = c->GetCore();
	if (AssStyle *style = core.ass->GetStyle(diag->Style))
		rz = style->angle;

	auto blocks = diag->ParseTags();

	if (param_vec tag = find_tag(blocks, "\\frx"))
		rx = tag->front().Get(rx);
	if (param_vec tag = find_tag(blocks, "\\fry"))
		ry = tag->front().Get(ry);
	if (param_vec tag = find_tag(blocks, "\\frz"))
		rz = tag->front().Get(rz);
	else if ((tag = find_tag(blocks, "\\fr")))
		rz = tag->front().Get(rz);
}

void VisualToolBase::GetLineShear(AssDialogue *diag, float& fax, float& fay) {
	fax = fay = 0.f;

	auto blocks = diag->ParseTags();

	if (param_vec tag = find_tag(blocks, "\\fax"))
		fax = tag->front().Get(fax);
	if (param_vec tag = find_tag(blocks, "\\fay"))
		fay = tag->front().Get(fay);
}

void VisualToolBase::GetLineScale(AssDialogue *diag, Vector2D &scale) {
	float x = 100.f, y = 100.f;

	auto core = c->GetCore();
	if (AssStyle *style = core.ass->GetStyle(diag->Style)) {
		x = style->scalex;
		y = style->scaley;
	}

	auto blocks = diag->ParseTags();

	if (param_vec tag = find_tag(blocks, "\\fscx"))
		x = tag->front().Get(x);
	if (param_vec tag = find_tag(blocks, "\\fscy"))
		y = tag->front().Get(y);

	scale = Vector2D(x, y);
}

void VisualToolBase::GetLineClip(AssDialogue *diag, Vector2D &p1, Vector2D &p2, bool &inverse) {
	inverse = false;

	auto blocks = diag->ParseTags();
	param_vec tag = find_tag(blocks, "\\iclip");
	if (tag)
		inverse = true;
	else
		tag = find_tag(blocks, "\\clip");

	if (tag && tag->size() == 4) {
		p1 = vec_or_bad(tag, 0, 1);
		p2 = vec_or_bad(tag, 2, 3);
	}
	else {
		p1 = Vector2D(0, 0);
		p2 = script_res - 1;
	}
}

std::string VisualToolBase::GetLineVectorClip(AssDialogue *diag, int &scale, bool &inverse) {
	auto blocks = diag->ParseTags();

	scale = 1;
	inverse = false;

	param_vec tag = find_tag(blocks, "\\iclip");
	if (tag)
		inverse = true;
	else
		tag = find_tag(blocks, "\\clip");

	if (tag && tag->size() == 4) {
		return agi::format("m %d %d l %d %d %d %d %d %d"
			, (*tag)[0].Get<int>(), (*tag)[1].Get<int>()
			, (*tag)[2].Get<int>(), (*tag)[1].Get<int>()
			, (*tag)[2].Get<int>(), (*tag)[3].Get<int>()
			, (*tag)[0].Get<int>(), (*tag)[3].Get<int>());
	}
	if (tag) {
		scale = std::max((*tag)[0].Get(scale), 1);
		return (*tag)[1].Get<std::string>("");
	}

	return "";
}

void VisualToolBase::SetSelectedOverride(std::string const& tag, std::string const& value) {
	auto core = c->GetCore();
	for (auto line : core.selectionController->GetSelectedSet())
		SetOverride(line, tag, value);
}

void VisualToolBase::ClearChangedLines() {
	changed_lines.clear();
	changed_line_set.clear();
	single_changed_line = nullptr;
}

void VisualToolBase::ScheduleInteractionRender() {
	if (!IsInteracting() || !interaction_render_pacer.IsActive())
		return;

	bool const allowed = interaction_render_pacer.Request(DeadlinePacingPolicy::Clock::now());
	perf_trace::ObserveVideoUiDuration("visual_tool.interaction_render.request", 0.0, allowed ? 1 : 0);
	if (allowed) {
		interaction_render_timer.Stop();
		RenderInteractionFrame(0);
	}
	else {
		ArmInteractionRenderTimer();
	}
}

void VisualToolBase::SetOverride(AssDialogue* line, std::string const& tag, std::string const& value) {
	if (!line) return;

	perf_trace::VideoUiDurationScope override_trace("visual_tool.override");
	std::string const original_text = line->Text.get();

	std::string removeTag;
	if (tag == "\\1c") removeTag = "\\c";
	else if (tag == "\\frz") removeTag = "\\fr";
	else if (tag == "\\pos") removeTag = "\\move";
	else if (tag == "\\move") removeTag = "\\pos";
	else if (tag == "\\clip") removeTag = "\\iclip";
	else if (tag == "\\iclip") removeTag = "\\clip";

	// Get block at start
	auto blocks = line->ParseTags();
	AssDialogueBlock *block = blocks.front().get();

	if (block->GetType() == AssBlockType::OVERRIDE) {
		auto ovr = static_cast<AssDialogueBlockOverride*>(block);
		// Remove old of same
		for (size_t i = 0; i < ovr->Tags.size(); i++) {
			std::string const& name = ovr->Tags[i].Name;
			if (tag == name || removeTag == name) {
				ovr->Tags.erase(ovr->Tags.begin() + i);
				i--;
			}
		}
		ovr->AddTag(tag + value);

		line->UpdateText(blocks);
	}
	else
		line->Text = "{" + tag + value + "}" + line->Text.get();

	bool const text_changed = original_text != line->Text.get();
	if (override_trace.IsActive())
		override_trace.SetDetails(text_changed ? 1 : 0);

	if (text_changed && changed_line_set.insert(line).second) {
		single_changed_line = changed_lines.empty() ? line : nullptr;
		changed_lines.push_back(line);
	}
}

// If only export worked
template class VisualTool<VisualDraggableFeature>;
template class VisualTool<ClipCorner>;
template class VisualTool<VisualToolDragDraggableFeature>;
template class VisualTool<VisualToolVectorClipDraggableFeature>;
