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

#include "search_replace_engine.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "selection_controller.h"
#include "text_selection_controller.h"

#include <libaegisub/exception.h>
#include <libaegisub/string_utils.h>

#include <tuple>
#include <utility>

namespace {
static const size_t bad_pos = -1;
static const MatchState bad_match{0, bad_pos};

auto get_dialogue_field(SearchReplaceSettings::Field field) -> decltype(&AssDialogueBase::Text) {
	switch (field) {
		case SearchReplaceSettings::Field::TEXT: return &AssDialogueBase::Text;
		case SearchReplaceSettings::Field::STYLE: return &AssDialogueBase::Style;
		case SearchReplaceSettings::Field::ACTOR: return &AssDialogueBase::Actor;
		case SearchReplaceSettings::Field::EFFECT: return &AssDialogueBase::Effect;
	}
	throw agi::InternalError("Bad field for search");
}

template<typename Iterator, typename Container>
Iterator circular_next(Iterator it, Container& c) {
	++it;
	if (it == c.end())
		it = c.begin();
	return it;
}

}

SearchReplaceEngine::SearchReplaceEngine(agi::Context *c)
: context(c)
{
	continuation_connections = agi::signal::make_vector({
		c->ass->AddCommitListener([this] { last_empty_match.reset(); }),
		c->selectionController->AddActiveLineListener([this] { last_empty_match.reset(); }),
		c->selectionController->AddSelectionListener([this] { last_empty_match.reset(); }),
		c->textSelectionController->AddSelectionListener([this] {
			if (last_empty_match &&
			    (!std::cmp_equal(context->textSelectionController->GetSelectionStart(), last_empty_match->pos) ||
			     !std::cmp_equal(context->textSelectionController->GetSelectionEnd(), last_empty_match->pos)))
				last_empty_match.reset();
		})
	});
}

void SearchReplaceEngine::Replace(AssDialogue *diag, MatchState &ms) {
	auto& diag_field = diag->*get_dialogue_field(settings.field);
	auto text = diag_field.get();

	std::string replacement = ExpandSubtitleMatchReplacement(
		ms, settings, SubtitleMatchReplacementScope::MATCH_ONLY);

	agi::util::strings::replace_range_inplace(text, ms.start, ms.end, replacement);
	diag_field = std::move(text);
	ms.end = ms.start + replacement.size();
}

bool SearchReplaceEngine::FindReplace(bool replace) {
	if (!initialized)
		return false;

	// Find/Replace Next do not produce a report for the results panel.
	last_matches.clear();
	last_replacements.clear();

	auto core = context->GetCore();
	if (replace)
		last_empty_match.reset();
	bool continuing_empty = last_empty_match.has_value();
	auto matches = continuing_empty
		? MakeSubtitleMatcher(settings, last_empty_match->search_pos)
		: GetMatcher(settings);

	AssDialogue *line = core.selectionController->GetActiveLine();
	auto it = core.ass->iterator_to(*line);
	size_t pos = 0;

	auto replace_ms = bad_match;
	if (replace) {
		if (settings.field == SearchReplaceSettings::Field::TEXT)
			pos = core.textSelectionController->GetSelectionStart();

		if (aegisub::subtitle_match_report::LineIsEligible(*line, settings) &&
		    (replace_ms = matches(line, pos))) {
			size_t end = bad_pos;
			if (settings.field == SearchReplaceSettings::Field::TEXT)
				end = core.textSelectionController->GetSelectionEnd();

			if (end == bad_pos || (pos == replace_ms.start && end == replace_ms.end)) {
				Replace(line, replace_ms);
				pos = replace_ms.end;
				core.ass->Commit(from_wx(_("replace")), AssFile::COMMIT_DIAG_TEXT);
			}
			else {
				// The current line matches, but it wasn't already selected,
				// so the match hasn't been "found" and displayed to the user
				// yet, so do that rather than replacing
				core.textSelectionController->SetSelection(replace_ms.start, replace_ms.end);
				return true;
			}
		}
	}
	// Search from the end of the selection to avoid endless matching the same thing
	else if (settings.field == SearchReplaceSettings::Field::TEXT)
		pos = core.textSelectionController->GetSelectionEnd();
	// For non-text fields we just look for matching lines rather than each
	// match within the line, so move to the next line
	else if (settings.field != SearchReplaceSettings::Field::TEXT)
		it = circular_next(it, core.ass->Events);

	auto const& sel = core.selectionController->GetSelectedSet();
	bool selection_only = sel.size() > 1 && settings.limit_to == SearchReplaceSettings::Limit::SELECTED;

	do {
		if (continuing_empty && &*it != line) {
			matches = GetMatcher(settings);
			continuing_empty = false;
		}
		if (selection_only && !sel.count(&*it)) continue;
		if (!aegisub::subtitle_match_report::LineIsEligible(*it, settings)) continue;

		if (MatchState ms = matches(&*it, pos)) {
			if (selection_only)
				// We're cycling through the selection, so don't muck with it
				core.selectionController->SetActiveLine(&*it);
			else
				core.selectionController->SetSelectionAndActive({ &*it }, &*it);

			if (settings.field == SearchReplaceSettings::Field::TEXT)
				core.textSelectionController->SetSelection(ms.start, ms.end);

			last_empty_match.reset();
			if (!replace && settings.use_regex && settings.field == SearchReplaceSettings::Field::TEXT && ms.start == ms.end)
				last_empty_match = EmptyMatch{.pos = ms.start, .search_pos = ms.search_end};

			return true;
		}
	} while (pos = 0, &*(it = circular_next(it, core.ass->Events)) != line);

	// Replaced something and didn't find another match, so select the newly
	// inserted text
	if (replace_ms && settings.field == SearchReplaceSettings::Field::TEXT)
		core.textSelectionController->SetSelection(replace_ms.start, replace_ms.end);

	return true;
}

bool SearchReplaceEngine::FindAll() {
	if (!initialized)
		return false;

	last_matches.clear();
	last_replacements.clear();
	last_empty_match.reset();

	auto core = context->GetCore();

	std::vector<AssDialogue const *> selection;
	if (settings.limit_to == SearchReplaceSettings::Limit::SELECTED) {
		auto const& sel = core.selectionController->GetSelectedSet();
		selection.assign(sel.begin(), sel.end());
	}

	last_matches = aegisub::subtitle_match_report::FindAll(core.ass->Events, settings, selection);

	if (last_matches.empty())
		context->ShowInfo(from_wx(_("No matches found.")));

	return true;
}

bool SearchReplaceEngine::ReplaceAll() {
	if (!initialized)
		return false;

	last_matches.clear();
	last_replacements.clear();
	last_empty_match.reset();

	auto core = context->GetCore();
	auto enumerate = MakeSubtitleMatchEnumerator(settings);

	auto const& sel = core.selectionController->GetSelectedSet();
	bool selection_only = settings.limit_to == SearchReplaceSettings::Limit::SELECTED;
	auto field = get_dialogue_field(settings.field);
	std::vector<std::pair<AssDialogue *, decltype(AssDialogueBase::Text)>> pending;
	std::vector<aegisub::subtitle_match_report::ReplacementHit> replacements;

	for (auto& diag : core.ass->Events) {
		if (selection_only && !sel.count(&diag)) continue;
		if (!aegisub::subtitle_match_report::LineIsEligible(diag, settings)) continue;

		AssDialogue prepared(static_cast<AssDialogueBase const&>(diag));
		if (aegisub::subtitle_match_report::ReplaceInLine(prepared, settings, enumerate, replacements))
			pending.emplace_back(&diag, prepared.*field);
	}

	for (auto const& [diag, value] : pending)
		diag->*field = value;
	last_replacements = std::move(replacements);

	if (!last_replacements.empty()) {
		core.ass->Commit(from_wx(_("replace")), AssFile::COMMIT_DIAG_TEXT);
		// Hits go to the results panel; do not show a modal success box first.
	}
	else {
		context->ShowInfo(from_wx(_("No matches found.")));
	}

	return true;
}

void SearchReplaceEngine::Configure(SearchReplaceSettings const& new_settings) {
	auto query = [](SearchReplaceSettings const& s) {
		return std::tie(s.find, s.replace_with, s.field, s.limit_to, s.match_case,
			s.use_regex, s.use_unicode_escapes, s.ignore_comments, s.skip_tags,
			s.exact_match, s.match_styles);
	};
	if (!initialized || query(settings) != query(new_settings))
		last_empty_match.reset();
	settings = new_settings;
	initialized = true;
}
