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

#include "subtitle_match_report.h"

#include <libaegisub/signal.h>

#include <optional>
#include <string>
#include <vector>

namespace agi { struct Context; }
class AssDialogue;

class SearchReplaceEngine {
	agi::Context *context;
	bool initialized = false;
	SearchReplaceSettings settings;
	struct EmptyMatch {
		size_t pos;
		size_t search_pos;
	};
	std::optional<EmptyMatch> last_empty_match;
	std::vector<agi::signal::Connection> continuation_connections;

	std::vector<aegisub::subtitle_match_report::MatchHit> last_matches;
	std::vector<aegisub::subtitle_match_report::ReplacementHit> last_replacements;

	bool FindReplace(bool replace);
	void Replace(AssDialogue *line, MatchState &ms);

public:
	bool FindNext() { return FindReplace(false); }
	bool ReplaceNext() { return FindReplace(true); }
	bool ReplaceAll();
	bool FindAll();

	void Configure(SearchReplaceSettings const& new_settings);

	/// The settings that produced the current last_matches/last_replacements.
	/// Prefer this copy over the live dialog settings: the report panel needs
	/// the settings that were in effect when its hits were generated, so an
	/// in-place recompute matches against the same query that listed them.
	SearchReplaceSettings const& GetSettings() const { return settings; }

	std::vector<aegisub::subtitle_match_report::MatchHit> const& GetLastMatches() const {
		return last_matches;
	}
	std::vector<aegisub::subtitle_match_report::ReplacementHit> const& GetLastReplacements() const {
		return last_replacements;
	}

	static std::function<MatchState (const AssDialogue*, size_t)> GetMatcher(SearchReplaceSettings const& settings) {
		return MakeSubtitleMatcher(settings);
	}

	SearchReplaceEngine(agi::Context *c);
};
