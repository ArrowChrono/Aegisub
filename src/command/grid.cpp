// Copyright (c) 2005-2010, Niels Martin Hansen
// Copyright (c) 2005-2010, Rodrigo Braz Monteiro
// Copyright (c) 2010, Amar Takhar
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "command.h"

#include "../ass_dialogue.h"
#include "../ass_file.h"
#include "../audio_controller.h"
#include "../audio_timing.h"
#include "../base_grid.h"
#include "../compat.h"
#include "../frame_main.h"
#include "../include/aegisub/context.h"
#include "../include/aegisub/context_ui.h"
#include "../libresrc/libresrc.h"
#include "../options.h"
#include "../selection_controller.h"
#include "../subtitle_grid_ops.h"
#include "../subtitle_grid_folding.h"

#include <libaegisub/make_unique.h>

namespace {
	using cmd::Command;

struct grid_line_next final : public Command {
	CMD_NAME("grid/line/next")
	STR_MENU("Next Line")
	STR_DISP("Next Line")
	STR_HELP("Move to the next subtitle line")

	void operator()(agi::Context *c) override {
		if (c->ui && c->GetUI().subsGrid)
			c->GetUI().subsGrid->NextVisibleLine(1);
		else
			c->GetCore().selectionController->NextLine();
	}
};

struct grid_line_next_create final : public Command {
	CMD_NAME("grid/line/next/create")
	CMD_ICON(button_audio_commit)
	STR_MENU("Next Line")
	STR_DISP("Next Line")
	STR_HELP("Move to the next subtitle line, creating a new one if needed")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		AudioTimingController *tc = core.audioController->GetTimingController();
		if (tc)
			tc->Commit();

		AssDialogue *cur = core.selectionController->GetActiveLine();
		core.selectionController->NextLine();
		if (cur == core.selectionController->GetActiveLine()) {
			auto newline = aegisub::subtitle_grid_ops::CreateLineAfter(*cur, OPT_GET("Timing/Default Duration")->GetInt());

			auto pos = core.ass->iterator_to(*cur);
			core.ass->Events.insert(++pos, *newline.release());
			core.ass->Commit(from_wx(_("line insertion")), AssFile::COMMIT_DIAG_ADDREM);
			core.selectionController->NextLine();
		}
	}
};

struct grid_line_prev final : public Command {
	CMD_NAME("grid/line/prev")
	STR_MENU("Previous Line")
	STR_DISP("Previous Line")
	STR_HELP("Move to the previous line")

	void operator()(agi::Context *c) override {
		if (c->ui && c->GetUI().subsGrid)
			c->GetUI().subsGrid->NextVisibleLine(-1);
		else
			c->GetCore().selectionController->PrevLine();
	}
};

void activate_fold_start(agi::Context *c, int start, int end) {
	auto core = c->GetCore();
	auto *active = core.selectionController->GetActiveLine();
	if (!active || active->Row <= start || active->Row > end)
		return;
	for (auto& line : core.ass->Events) {
		if (line.Row == start) {
			core.selectionController->SetActiveLine(&line);
			return;
		}
	}
}

struct grid_fold_create final : public Command {
	CMD_NAME("grid/fold/create")
	STR_MENU("Create Fold Group")
	STR_DISP("Create Fold Group")
	STR_HELP("Group and collapse the real subtitle range between the first and last selected lines")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		auto core = c->GetCore();
		auto selection = core.selectionController->GetSortedSelection();
		return selection.size() >= 2 && core.ass->Folding().CanCreate(selection.front()->Row, selection.back()->Row);
	}

	void operator()(agi::Context *c) override {
		if (!Validate(c))
			return;
		auto core = c->GetCore();
		auto selection = core.selectionController->GetSortedSelection();
		int start = selection.front()->Row;
		int end = selection.back()->Row;
		activate_fold_start(c, start, end);
		if (core.ass->Folding().Create(*core.ass, start, end))
			core.ass->Commit(from_wx(_("create fold group")), AssFile::COMMIT_FOLD);
	}
};

struct validate_current_fold : public Command {
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		auto core = c->GetCore();
		auto *active = core.selectionController->GetActiveLine();
		return active && core.ass->Folding().GroupAt(active->Row);
	}
};

struct grid_fold_toggle final : public validate_current_fold {
	CMD_NAME("grid/fold/toggle")
	STR_MENU("Toggle Fold Group")
	STR_DISP("Toggle Fold Group")
	STR_HELP("Expand or collapse the active line's fold group")

	void operator()(agi::Context *c) override {
		if (!Validate(c))
			return;
		auto core = c->GetCore();
		auto& folding = core.ass->Folding();
		auto group = *folding.GroupAt(core.selectionController->GetActiveLine()->Row);
		if (!folding.IsCollapsed(group))
			activate_fold_start(c, group.start, group.end);
		if (folding.Toggle(*core.ass, group.start))
			core.ass->Commit(from_wx(_("toggle fold group")), AssFile::COMMIT_FOLD);
	}
};

struct grid_fold_expand final : public validate_current_fold {
	CMD_NAME("grid/fold/expand")
	STR_MENU("Expand Fold Group")
	STR_DISP("Expand Fold Group")
	STR_HELP("Expand the active line's fold group")

	void operator()(agi::Context *c) override {
		if (!Validate(c))
			return;
		auto core = c->GetCore();
		if (core.ass->Folding().SetCollapsed(*core.ass, core.selectionController->GetActiveLine()->Row, false))
			core.ass->Commit(from_wx(_("expand fold group")), AssFile::COMMIT_FOLD);
	}
};

struct grid_fold_collapse final : public validate_current_fold {
	CMD_NAME("grid/fold/collapse")
	STR_MENU("Collapse Fold Group")
	STR_DISP("Collapse Fold Group")
	STR_HELP("Collapse the active line's fold group")

	void operator()(agi::Context *c) override {
		if (!Validate(c))
			return;
		auto core = c->GetCore();
		auto& folding = core.ass->Folding();
		auto group = *folding.GroupAt(core.selectionController->GetActiveLine()->Row);
		activate_fold_start(c, group.start, group.end);
		if (folding.SetCollapsed(*core.ass, group.start, true))
			core.ass->Commit(from_wx(_("collapse fold group")), AssFile::COMMIT_FOLD);
	}
};

struct grid_fold_clear final : public validate_current_fold {
	CMD_NAME("grid/fold/clear")
	STR_MENU("Clear Fold Group")
	STR_DISP("Clear Fold Group")
	STR_HELP("Remove the active line's fold group without deleting subtitles")

	bool Validate(const agi::Context *c) override {
		if (validate_current_fold::Validate(c))
			return true;
		auto core = c->GetCore();
		auto *active = core.selectionController->GetActiveLine();
		if (!active)
			return false;
		for (auto const& entry : core.ass->GetExtradata(active->ExtradataIds)) {
			if (entry.key == "_aegi_folddata")
				return true;
		}
		return false;
	}

	void operator()(agi::Context *c) override {
		if (!Validate(c))
			return;
		auto core = c->GetCore();
		if (core.ass->Folding().Remove(*core.ass, core.selectionController->GetActiveLine()->Row))
			core.ass->Commit(from_wx(_("clear fold group")), AssFile::COMMIT_FOLD);
	}
};

struct validate_any_fold : public Command {
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		return !c->GetCore().ass->Folding().Groups().empty();
	}
};

struct grid_fold_expand_all final : public validate_any_fold {
	CMD_NAME("grid/fold/expand/all")
	STR_MENU("Expand All Fold Groups")
	STR_DISP("Expand All Fold Groups")
	STR_HELP("Expand every fold group in the subtitle file")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		if (core.ass->Folding().SetAllCollapsed(*core.ass, false))
			core.ass->Commit(from_wx(_("expand all fold groups")), AssFile::COMMIT_FOLD);
	}
};

struct grid_fold_collapse_all final : public validate_any_fold {
	CMD_NAME("grid/fold/collapse/all")
	STR_MENU("Collapse All Fold Groups")
	STR_DISP("Collapse All Fold Groups")
	STR_HELP("Collapse every fold group in the subtitle file")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		auto& folding = core.ass->Folding();
		if (auto *active = core.selectionController->GetActiveLine()) {
			if (auto const *group = folding.GroupAt(active->Row))
				activate_fold_start(c, group->start, group->end);
		}
		if (folding.SetAllCollapsed(*core.ass, true))
			core.ass->Commit(from_wx(_("collapse all fold groups")), AssFile::COMMIT_FOLD);
	}
};

struct grid_fold_clear_all final : public validate_any_fold {
	CMD_NAME("grid/fold/clear/all")
	STR_MENU("Clear All Fold Groups")
	STR_DISP("Clear All Fold Groups")
	STR_HELP("Remove all fold groups and invalid fold markers without deleting subtitles")

	bool Validate(const agi::Context *c) override {
		auto *ass = c->GetCore().ass.get();
		for (auto const& line : ass->Events) {
			for (auto const& entry : ass->GetExtradata(line.ExtradataIds)) {
				if (entry.key == "_aegi_folddata")
					return true;
			}
		}
		return false;
	}

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		if (core.ass->Folding().Clear(*core.ass))
			core.ass->Commit(from_wx(_("clear all fold groups")), AssFile::COMMIT_FOLD);
	}
};

struct grid_selection_back final : public Command {
	CMD_NAME("grid/selection/back")
	STR_MENU("Back in Selection History")
	STR_DISP("Back in Selection History")
	STR_HELP("Return to the previously selected or edited subtitle line")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		return c->GetCore().selectionController->CanNavigateSelectionBack();
	}

	void operator()(agi::Context *c) override {
		c->GetCore().selectionController->NavigateSelectionBack();
	}
};

struct grid_selection_forward final : public Command {
	CMD_NAME("grid/selection/forward")
	STR_MENU("Forward in Selection History")
	STR_DISP("Forward in Selection History")
	STR_HELP("Return to the subtitle line active before selection history moved back")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		return c->GetCore().selectionController->CanNavigateSelectionForward();
	}

	void operator()(agi::Context *c) override {
		c->GetCore().selectionController->NavigateSelectionForward();
	}
};

struct grid_selection_anchor final : public Command {
	CMD_NAME("grid/selection/anchor")
	STR_HELP("Anchor the active subtitle line, or return to and clear the current anchor")
	CMD_TYPE(COMMAND_VALIDATE | COMMAND_DYNAMIC_NAME)

	wxString StrMenu(const agi::Context *c) const override {
		auto anchor = c->GetCore().selectionController->GetSelectionAnchor();
		if (!anchor)
			return _("&Anchor Current Line");
		if (!anchor->available)
			return wxString::Format(_("&Clear Unavailable Anchor (line %d)"), anchor->row + 1);
		return wxString::Format(_("&Return to Anchor (line %d)"), anchor->row + 1);
	}

	wxString StrDisplay(const agi::Context *c) const override {
		auto anchor = c->GetCore().selectionController->GetSelectionAnchor();
		if (!anchor)
			return _("Anchor Current Line");
		if (!anchor->available)
			return wxString::Format(_("Clear Unavailable Anchor (line %d)"), anchor->row + 1);
		return wxString::Format(_("Return to Anchor (line %d)"), anchor->row + 1);
	}

	bool Validate(const agi::Context *c) override {
		auto const& controller = c->GetCore().selectionController;
		return controller->HasSelectionAnchor() || controller->GetActiveLine();
	}

	void operator()(agi::Context *c) override {
		auto result = c->GetCore().selectionController->ToggleSelectionAnchor();
		switch (result.action) {
			case SelectionController::AnchorAction::Pinned:
				c->ShowStatus(from_wx(wxString::Format(_("Anchored line %d"), result.row + 1)));
				break;
			case SelectionController::AnchorAction::Returned:
				c->ShowStatus(from_wx(wxString::Format(_("Returned to line %d; anchor cleared"), result.row + 1)));
				break;
			case SelectionController::AnchorAction::Cleared:
				c->ShowStatus(from_wx(wxString::Format(_("Anchor cleared from line %d"), result.row + 1)));
				break;
			case SelectionController::AnchorAction::Missing:
				c->ShowStatus(from_wx(wxString::Format(_("Anchor at line %d is unavailable; anchor cleared"), result.row + 1)));
				break;
			case SelectionController::AnchorAction::None:
				break;
		}
	}
};

struct grid_sort_actor final : public Command {
	CMD_NAME("grid/sort/actor")
	STR_MENU("&Actor Name")
	STR_DISP("Actor Name")
	STR_HELP("Sort all subtitles by their actor names")

	void operator()(agi::Context *c) override {
		auto *ass = c->GetCore().ass.get();
		ass->Sort(AssFile::CompActor);
		ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct validate_sel_multiple : public Command {
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		return c->GetCore().selectionController->GetSelectedSet().size() > 1;
	}
};

struct grid_sort_actor_selected final : public validate_sel_multiple {
	CMD_NAME("grid/sort/actor/selected")
	STR_MENU("&Actor Name")
	STR_DISP("Actor Name")
	STR_HELP("Sort selected subtitles by their actor names")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		core.ass->Sort(AssFile::CompActor, core.selectionController->GetSelectedSet());
		core.ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_effect final : public Command {
	CMD_NAME("grid/sort/effect")
	STR_MENU("&Effect")
	STR_DISP("Effect")
	STR_HELP("Sort all subtitles by their effects")

	void operator()(agi::Context *c) override {
		auto *ass = c->GetCore().ass.get();
		ass->Sort(AssFile::CompEffect);
		ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_effect_selected final : public validate_sel_multiple {
	CMD_NAME("grid/sort/effect/selected")
	STR_MENU("&Effect")
	STR_DISP("Effect")
	STR_HELP("Sort selected subtitles by their effects")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		core.ass->Sort(AssFile::CompEffect, core.selectionController->GetSelectedSet());
		core.ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_end final : public Command {
	CMD_NAME("grid/sort/end")
	STR_MENU("&End Time")
	STR_DISP("End Time")
	STR_HELP("Sort all subtitles by their end times")

	void operator()(agi::Context *c) override {
		auto *ass = c->GetCore().ass.get();
		ass->Sort(AssFile::CompEnd);
		ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_end_selected final : public validate_sel_multiple {
	CMD_NAME("grid/sort/end/selected")
	STR_MENU("&End Time")
	STR_DISP("End Time")
	STR_HELP("Sort selected subtitles by their end times")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		core.ass->Sort(AssFile::CompEnd, core.selectionController->GetSelectedSet());
		core.ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_layer final : public Command {
	CMD_NAME("grid/sort/layer")
	STR_MENU("&Layer")
	STR_DISP("Layer")
	STR_HELP("Sort all subtitles by their layer number")

	void operator()(agi::Context *c) override {
		auto *ass = c->GetCore().ass.get();
		ass->Sort(AssFile::CompLayer);
		ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_layer_selected final : public validate_sel_multiple {
	CMD_NAME("grid/sort/layer/selected")
	STR_MENU("&Layer")
	STR_DISP("Layer")
	STR_HELP("Sort selected subtitles by their layer number")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		core.ass->Sort(AssFile::CompLayer, core.selectionController->GetSelectedSet());
		core.ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_start final : public Command {
	CMD_NAME("grid/sort/start")
	STR_MENU("&Start Time")
	STR_DISP("Start Time")
	STR_HELP("Sort all subtitles by their start times")

	void operator()(agi::Context *c) override {
		auto *ass = c->GetCore().ass.get();
		ass->Sort();
		ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_start_selected final : public validate_sel_multiple {
	CMD_NAME("grid/sort/start/selected")
	STR_MENU("&Start Time")
	STR_DISP("Start Time")
	STR_HELP("Sort selected subtitles by their start times")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		core.ass->Sort(AssFile::CompStart, core.selectionController->GetSelectedSet());
		core.ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_style final : public Command {
	CMD_NAME("grid/sort/style")
	STR_MENU("St&yle Name")
	STR_DISP("Style Name")
	STR_HELP("Sort all subtitles by their style names")

	void operator()(agi::Context *c) override {
		auto *ass = c->GetCore().ass.get();
		ass->Sort(AssFile::CompStyle);
		ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_style_selected final : public validate_sel_multiple {
	CMD_NAME("grid/sort/style/selected")
	STR_MENU("St&yle Name")
	STR_DISP("Style Name")
	STR_HELP("Sort selected subtitles by their style names")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		core.ass->Sort(AssFile::CompStyle, core.selectionController->GetSelectedSet());
		core.ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_text final : public Command {
	CMD_NAME("grid/sort/text")
	STR_MENU("Te&xt")
	STR_DISP("Text")
	STR_HELP("Sort all subtitles by their text, including styling tags")

	void operator()(agi::Context *c) override {
		auto *ass = c->GetCore().ass.get();
		ass->Sort(AssFile::CompText);
		ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_text_selected final : public validate_sel_multiple {
	CMD_NAME("grid/sort/text/selected")
	STR_MENU("Te&xt")
	STR_DISP("Text")
	STR_HELP("Sort selected subtitles by their text, including styling tags")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		core.ass->Sort(AssFile::CompText, core.selectionController->GetSelectedSet());
		core.ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_text_stripped final : public Command {
	CMD_NAME("grid/sort/text_stripped")
	STR_MENU("Stri&pped Text")
	STR_DISP("Stripped Text")
	STR_HELP("Sort all subtitles by their stripped text")

	void operator()(agi::Context *c) override {
		auto *ass = c->GetCore().ass.get();
		ass->Sort(AssFile::CompTextStripped);
		ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_text_stripped_selected final : public validate_sel_multiple {
	CMD_NAME("grid/sort/text_stripped/selected")
	STR_MENU("Stri&pped Text")
	STR_DISP("Stripped Text")
	STR_HELP("Sort selected subtitles by their stripped text")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		core.ass->Sort(AssFile::CompTextStripped, core.selectionController->GetSelectedSet());
		core.ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_text_length final : public Command {
	CMD_NAME("grid/sort/text_length")
	STR_MENU("Text by &Length")
	STR_DISP("Text by Length")
	STR_HELP("Sort all subtitles by the length of their text, including styling tags")

	void operator()(agi::Context *c) override {
		auto *ass = c->GetCore().ass.get();
		ass->Sort(AssFile::CompTextLength);
		ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_text_length_selected final : public validate_sel_multiple {
	CMD_NAME("grid/sort/text_length/selected")
	STR_MENU("Text by &Length")
	STR_DISP("Text by Length")
	STR_HELP("Sort selected subtitles by the length of their text, including styling tags")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		core.ass->Sort(AssFile::CompTextLength, core.selectionController->GetSelectedSet());
		core.ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_text_stripped_length final : public Command {
	CMD_NAME("grid/sort/text_stripped_length")
	STR_MENU("Stripped Text by Lengt&h")
	STR_DISP("Stripped Text by Length")
	STR_HELP("Sort all subtitles by the length of their rendered text")

	void operator()(agi::Context *c) override {
		auto *ass = c->GetCore().ass.get();
		ass->Sort(AssFile::CompTextStrippedLength);
		ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_sort_text_stripped_length_selected final : public validate_sel_multiple {
	CMD_NAME("grid/sort/text_stripped_length/selected")
	STR_MENU("Stripped Text by Lengt&h")
	STR_DISP("Stripped Text by Length")
	STR_HELP("Sort selected subtitles by the length of their rendered text")

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		core.ass->Sort(AssFile::CompTextStrippedLength, core.selectionController->GetSelectedSet());
		core.ass->Commit(from_wx(_("sort")), AssFile::COMMIT_ORDER);
	}
};

struct grid_tag_cycle_hiding final : public Command {
	CMD_NAME("grid/tag/cycle_hiding")
	CMD_ICON(toggle_tag_hiding)
	STR_MENU("Cycle Tag Hiding Mode")
	STR_DISP("Cycle Tag Hiding Mode")
	STR_HELP("Cycle through tag hiding modes")

	void operator()(agi::Context *c) override {
		int tagMode = OPT_GET("Subtitle/Grid/Hide Overrides")->GetInt();

		// Cycle to next
		tagMode = (tagMode+1)%3;

		// Show on status bar
		wxString message;
		if (tagMode == 0) message = _("ASS Override Tag mode set to show full tags.");
		if (tagMode == 1) message = _("ASS Override Tag mode set to simplify tags.");
		if (tagMode == 2) message = _("ASS Override Tag mode set to hide tags.");
		c->ShowStatus(from_wx(message), 10000);

		// Set option
		OPT_SET("Subtitle/Grid/Hide Overrides")->SetInt(tagMode);
	}
};

struct grid_tags_hide final : public Command {
	CMD_NAME("grid/tags/hide")
	STR_MENU("&Hide Tags")
	STR_DISP("Hide Tags")
	STR_HELP("Hide override tags in the subtitle grid")
	CMD_TYPE(COMMAND_RADIO)

	bool IsActive(const agi::Context *) override {
		return OPT_GET("Subtitle/Grid/Hide Overrides")->GetInt() == 2;
	}

	void operator()(agi::Context *) override {
		OPT_SET("Subtitle/Grid/Hide Overrides")->SetInt(2);
	}
};

struct grid_tags_show final : public Command {
	CMD_NAME("grid/tags/show")
	STR_MENU("Sh&ow Tags")
	STR_DISP("Show Tags")
	STR_HELP("Show full override tags in the subtitle grid")
	CMD_TYPE(COMMAND_RADIO)

	bool IsActive(const agi::Context *) override {
		return OPT_GET("Subtitle/Grid/Hide Overrides")->GetInt() == 0;
	}

	void operator()(agi::Context *) override {
		OPT_SET("Subtitle/Grid/Hide Overrides")->SetInt(0);
	}
};

struct grid_tags_simplify final : public Command {
	CMD_NAME("grid/tags/simplify")
	STR_MENU("S&implify Tags")
	STR_DISP("Simplify Tags")
	STR_HELP("Replace override tags in the subtitle grid with a simplified placeholder")
	CMD_TYPE(COMMAND_RADIO)

	bool IsActive(const agi::Context *) override {
		return OPT_GET("Subtitle/Grid/Hide Overrides")->GetInt() == 1;
	}

	void operator()(agi::Context *) override {
		OPT_SET("Subtitle/Grid/Hide Overrides")->SetInt(1);
	}
};

struct grid_move_up final : public Command {
	CMD_NAME("grid/move/up")
	STR_MENU("Move line up")
	STR_DISP("Move line up")
	STR_HELP("Move the selected lines up one row")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		return c->GetCore().selectionController->GetSelectedSet().size() != 0;
	}

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		if (aegisub::subtitle_grid_ops::MoveSelectionUp(core.ass->Events, core.selectionController->GetSelectedSet()))
			core.ass->Commit(from_wx(_("move lines")), AssFile::COMMIT_ORDER);
	}
};

struct grid_move_down final : public Command {
	CMD_NAME("grid/move/down")
	STR_MENU("Move line down")
	STR_DISP("Move line down")
	STR_HELP("Move the selected lines down one row")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		return c->GetCore().selectionController->GetSelectedSet().size() != 0;
	}

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		if (aegisub::subtitle_grid_ops::MoveSelectionDown(core.ass->Events, core.selectionController->GetSelectedSet()))
			core.ass->Commit(from_wx(_("move lines")), AssFile::COMMIT_ORDER);
	}
};

struct grid_swap final : public Command {
	CMD_NAME("grid/swap")
	CMD_ICON(arrow_sort)
	STR_MENU("Swap Lines")
	STR_DISP("Swap Lines")
	STR_HELP("Swap the two selected lines")
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		return c->GetCore().selectionController->GetSelectedSet().size() == 2;
	}

	void operator()(agi::Context *c) override {
		auto core = c->GetCore();
		if (aegisub::subtitle_grid_ops::SwapSelection(core.selectionController->GetSelectedSet()))
			core.ass->Commit(from_wx(_("swap lines")), AssFile::COMMIT_ORDER);
	}
};

}

namespace cmd {
	void init_grid() {
		reg(agi::make_unique<grid_fold_create>());
		reg(agi::make_unique<grid_fold_toggle>());
		reg(agi::make_unique<grid_fold_expand>());
		reg(agi::make_unique<grid_fold_collapse>());
		reg(agi::make_unique<grid_fold_clear>());
		reg(agi::make_unique<grid_fold_expand_all>());
		reg(agi::make_unique<grid_fold_collapse_all>());
		reg(agi::make_unique<grid_fold_clear_all>());
		reg(agi::make_unique<grid_line_next>());
		reg(agi::make_unique<grid_line_next_create>());
		reg(agi::make_unique<grid_line_prev>());
		reg(agi::make_unique<grid_selection_anchor>());
		reg(agi::make_unique<grid_selection_back>());
		reg(agi::make_unique<grid_selection_forward>());
		reg(agi::make_unique<grid_sort_actor>());
		reg(agi::make_unique<grid_sort_effect>());
		reg(agi::make_unique<grid_sort_end>());
		reg(agi::make_unique<grid_sort_layer>());
		reg(agi::make_unique<grid_sort_start>());
		reg(agi::make_unique<grid_sort_style>());
		reg(agi::make_unique<grid_sort_text>());
		reg(agi::make_unique<grid_sort_text_stripped>());
		reg(agi::make_unique<grid_sort_text_length>());
		reg(agi::make_unique<grid_sort_text_stripped_length>());
		reg(agi::make_unique<grid_sort_actor_selected>());
		reg(agi::make_unique<grid_sort_effect_selected>());
		reg(agi::make_unique<grid_sort_end_selected>());
		reg(agi::make_unique<grid_sort_layer_selected>());
		reg(agi::make_unique<grid_sort_start_selected>());
		reg(agi::make_unique<grid_sort_style_selected>());
		reg(agi::make_unique<grid_sort_text_selected>());
		reg(agi::make_unique<grid_sort_text_stripped_selected>());
		reg(agi::make_unique<grid_sort_text_length_selected>());
		reg(agi::make_unique<grid_sort_text_stripped_length_selected>());
		reg(agi::make_unique<grid_move_down>());
		reg(agi::make_unique<grid_move_up>());
		reg(agi::make_unique<grid_swap>());
		reg(agi::make_unique<grid_tag_cycle_hiding>());
		reg(agi::make_unique<grid_tags_hide>());
		reg(agi::make_unique<grid_tags_show>());
		reg(agi::make_unique<grid_tags_simplify>());
	}
}
