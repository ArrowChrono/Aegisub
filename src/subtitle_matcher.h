// Copyright (c) 2026
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

/// @file subtitle_matcher.h
/// @brief Builds the matcher closure used by the find/replace engine.
///
/// Split out of search_replace_engine.cpp so that the matcher logic can be
/// unit-tested without pulling in the GUI/context dependencies that the rest
/// of the engine requires.

#pragma once

#include <functional>
#include <boost/regex/icu.hpp>
#include <string>
#include <vector>

class AssDialogue;

/// Result of a single match attempt against a dialogue line.
struct MatchState {
	/// Byte range in the original field text (after NFC/tag mapping).
	size_t start = static_cast<size_t>(-1);
	size_t end = static_cast<size_t>(-1);
	/// Exclusive end of this match on the searchable surface (NFC and/or
	/// tag-stripped). Full-line enumeration advances in this space so combining
	/// reordering cannot drop earlier original characters.
	size_t search_end = 0;
	/// Regex replacements are expanded while the successful match result and
	/// its ICU traits are still available. Replace Next and Replace All have
	/// different prefix/suffix semantics, so keep both expansions.
	bool has_regex_replacement = false;
	std::string match_only_replacement;
	std::string search_context_replacement;

	operator bool() const { return end != static_cast<size_t>(-1); }
};

enum class SubtitleMatchReplacementScope {
	MATCH_ONLY,
	SEARCH_CONTEXT
};

/// All options that govern how the find/replace engine matches text.
struct SearchReplaceSettings {
	enum class Field {
		TEXT = 0,
		STYLE,
		ACTOR,
		EFFECT
	};

	enum class Limit {
		ALL = 0,
		SELECTED
	};

	std::string find;
	std::string replace_with;

	Field field;
	Limit limit_to;

	bool match_case;
	bool use_regex;
	bool use_unicode_escapes;
	bool ignore_comments;
	bool skip_tags;
	bool exact_match;

	/// Styles to search within. Empty means search all rows.
	std::vector<std::string> match_styles;
};

/// Build a matcher closure for the given settings. The closure maps a
/// dialogue line and a starting byte offset to a MatchState.
///
/// Note on Unicode normalization: when match_case is set, neither the needle
/// nor the haystack is normalized, so the comparison is byte-exact. Otherwise
/// both are NFC-normalized so that visually identical strings in different
/// Unicode forms still match. MatchState offsets are always in the original
/// field text (NFC ranges are mapped back, including combining reordering).
std::function<MatchState(const AssDialogue *, size_t)>
MakeSubtitleMatcher(SearchReplaceSettings const &settings);

std::function<MatchState(const AssDialogue *, size_t)>
MakeSubtitleMatcher(SearchReplaceSettings const& settings, size_t search_after_empty);

/// Build a reusable whole-line enumerator. Unlike repeatedly calling
/// MakeSubtitleMatcher with advancing original offsets, this preserves
/// searchable-surface order across NFC reordering and Boost's empty-match
/// retry semantics. Regex patterns are compiled once when the enumerator is
/// created, then reused for every line.
using SubtitleMatchEnumerator = std::function<std::vector<MatchState>(AssDialogue const&)>;
SubtitleMatchEnumerator
MakeSubtitleMatchEnumerator(SearchReplaceSettings const& settings);

/// Enumerate every match on one line in searchable-surface order (NFC /
/// tagless left-to-right), with Boost-style empty-match handling
/// (retry same position with match_not_initial_null before advancing).
std::vector<MatchState>
EnumerateLineMatches(AssDialogue const& line, SearchReplaceSettings const& settings);

/// Return the replacement expanded by the matcher for the requested scope.
std::string ExpandSubtitleMatchReplacement(MatchState const& ms,
                                           SearchReplaceSettings const& settings,
                                           SubtitleMatchReplacementScope scope);
