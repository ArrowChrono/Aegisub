// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "selection_controller.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "include/aegisub/context.h"
#include "subs_controller.h"

#include <algorithm>
#include <type_traits>

SelectionController::SelectionController(agi::Context *c) : context(c) { }

int SelectionController::GetActiveLineId() const {
	return active_line ? active_line->Id : 0;
}

AssDialogue *SelectionController::GetDialogueById(int line_id) const {
	if (line_id <= 0)
		return nullptr;

	auto core = context->GetCore();
	for (auto& line : core.ass->Events) {
		if (line.Id == line_id)
			return &line;
	}
	return nullptr;
}

bool SelectionController::IsLiveDialogueId(int line_id) const {
	return GetDialogueById(line_id) != nullptr;
}

aegisub::selection_anchor::Anchor::ResolveRow SelectionController::GetLineRowResolver() const {
	return [this](int line_id) -> std::optional<int> {
		if (auto *line = GetDialogueById(line_id))
			return line->Row;
		return std::nullopt;
	};
}

aegisub::selection_navigation_history::History::LineIdIsValid SelectionController::GetLiveLineValidator() const {
	return [this](int line_id) { return IsLiveDialogueId(line_id); };
}

void SelectionController::RecordActiveLineChange(AssDialogue *old_line, AssDialogue *new_line) {
	if (restoring_selection_history)
		return;

	selection_history.RecordTransition(old_line ? old_line->Id : 0, new_line ? new_line->Id : 0);
}

bool SelectionController::NavigateSelectionHistory(bool forward) {
	auto const is_live = GetLiveLineValidator();
	auto target_line_id = forward
		? selection_history.GoForward(GetActiveLineId(), is_live)
		: selection_history.GoBack(GetActiveLineId(), is_live);
	if (!target_line_id)
		return false;

	auto *target = GetDialogueById(*target_line_id);
	if (!target)
		return false;

	bool const old_restoring = restoring_selection_history;
	restoring_selection_history = true;
	SetSelectionAndActive({target}, target);
	restoring_selection_history = old_restoring;
	return true;
}

void SelectionController::SetSelectedSet(Selection new_selection) {
	if (selection == new_selection)
		return;
	selection = std::move(new_selection);
	AnnounceSelectedSetChanged();
}

void SelectionController::SetActiveLine(AssDialogue *new_line) {
	if (new_line != active_line) {
		RecordActiveLineChange(active_line, new_line);
		active_line = new_line;
		if (active_line) {
			auto core = context->GetCore();
			core.ass->Properties.active_row = active_line->Row;
		}
		AnnounceActiveLineChanged(new_line);
	}
}

SelectionController::PreparedSelection SelectionController::PrepareSelectionAndActive(
	Selection selected, AssDialogue *active) const {
	PreparedSelection prepared{.selected = std::move(selected), .active = active, .navigation = selection_history, .active_changed = active != active_line};
	if (prepared.active_changed && !restoring_selection_history)
		prepared.navigation.RecordTransition(active_line ? active_line->Id : 0, active ? active->Id : 0);
	return prepared;
}

void SelectionController::InstallSelection(PreparedSelection& prepared) noexcept {
	static_assert(std::is_nothrow_move_assignable_v<decltype(selection_history)>);
	selection.swap(prepared.selected);
	selection_history = std::move(prepared.navigation);
	active_line = prepared.active;
	if (active_line)
		context->GetCore().ass->Properties.active_row = active_line->Row;
}

void SelectionController::PublishSelection(PreparedSelection const& prepared) {
	AnnounceSelectedSetChanged();
	if (prepared.active_changed && active_line == prepared.active)
		AnnounceActiveLineChanged(active_line);
}

void SelectionController::SetSelectionAndActive(Selection new_selection, AssDialogue *new_line) {
	bool active_line_changed = new_line != active_line;
	if (active_line_changed)
		RecordActiveLineChange(active_line, new_line);
	selection = std::move(new_selection);
	active_line = new_line;
	if (active_line) {
		auto core = context->GetCore();
		core.ass->Properties.active_row = active_line->Row;
	}

	AnnounceSelectedSetChanged();
	if (active_line_changed)
		AnnounceActiveLineChanged(new_line);
}

std::vector<AssDialogue *> SelectionController::GetSortedSelection() const {
	std::vector<AssDialogue *> ret(selection.begin(), selection.end());
	sort(begin(ret), end(ret), [](AssDialogue *a, AssDialogue *b) { return a->Row < b->Row; });
	return ret;
}

void SelectionController::ClearSelectionHistory() {
	selection_history.Clear();
}

std::optional<aegisub::selection_anchor::Snapshot> SelectionController::GetSelectionAnchor() const {
	return selection_anchor.Peek(GetLineRowResolver());
}

std::optional<aegisub::selection_anchor::Snapshot> SelectionController::RefreshSelectionAnchor() {
	return selection_anchor.Refresh(GetLineRowResolver());
}

SelectionController::AnchorResult SelectionController::ToggleSelectionAnchor() {
	if (!selection_anchor.IsSet()) {
		if (!active_line)
			return {};

		selection_anchor.Set(active_line->Id, active_line->Row);
		AnnounceSelectionAnchorChanged();
		return {AnchorAction::Pinned, active_line->Row};
	}

	auto anchor = selection_anchor.Refresh(GetLineRowResolver());
	if (!anchor)
		return {};

	AnchorResult result{AnchorAction::Missing, anchor->row};
	if (anchor->available) {
		auto *target = GetDialogueById(anchor->line_id);
		if (target == active_line) {
			result.action = AnchorAction::Cleared;
		}
		else if (target) {
			// Anchor returns are ordinary navigation so Back can revisit the prior line.
			SetSelectionAndActive({target}, target);
			result.action = AnchorAction::Returned;
			result.row = target->Row;
		}
	}

	selection_anchor.Clear();
	AnnounceSelectionAnchorChanged();
	return result;
}

void SelectionController::ClearSelectionAnchor() {
	if (!selection_anchor.IsSet())
		return;

	selection_anchor.Clear();
	AnnounceSelectionAnchorChanged();
}

void SelectionController::RecordEditedLine(AssDialogue *line) {
	if (!restoring_selection_history && line)
		selection_history.RecordVisit(line->Id);
}

bool SelectionController::CanNavigateSelectionBack() const {
	return selection_history.CanGoBack(GetActiveLineId(), GetLiveLineValidator());
}

bool SelectionController::CanNavigateSelectionForward() const {
	return selection_history.CanGoForward(GetActiveLineId(), GetLiveLineValidator());
}

bool SelectionController::NavigateSelectionBack() {
	return NavigateSelectionHistory(false);
}

bool SelectionController::NavigateSelectionForward() {
	return NavigateSelectionHistory(true);
}

void SelectionController::PrevLine() {
	if (!active_line) return;
	auto core = context->GetCore();
	auto it = core.ass->iterator_to(*active_line);
	if (it != core.ass->Events.begin()) {
		--it;
		SetSelectionAndActive({&*it}, &*it);
	}
}

void SelectionController::NextLine() {
	if (!active_line) return;
	auto core = context->GetCore();
	auto it = core.ass->iterator_to(*active_line);
	if (++it != core.ass->Events.end())
		SetSelectionAndActive({&*it}, &*it);
}
