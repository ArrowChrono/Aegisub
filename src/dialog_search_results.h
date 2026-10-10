/// @file dialog_search_results.h
/// @brief Results panel for Find All / Replace All.

#pragma once

#include "subtitle_match_report.h"

#include <libaegisub/signal.h>
#include <wx/dialog.h>

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace agi { struct Context; }
struct AssFileCommitDetails;
class AssDialogue;
class wxDataViewCtrl;
class wxDataViewEvent;
class wxStaticText;
class wxButton;

/// Modeless list of matches or replacements produced by SearchReplaceEngine.
/// Not registered with DialogManager: construction needs the hit vectors, not
/// only an agi::Context*. Closed via Destroy (not Hide) so commit listeners
/// cannot accumulate.
class DialogSearchResults final : public wxDialog {
	class ResultsModel;

	/// Display rows: MatchHit fields plus optional replacement / post-edit range.
	struct DisplayHit {
		int line_id = 0;
		int row = -1;
		std::string start_time;
		std::string style;
		std::string matched;
		std::shared_ptr<std::string const> line_text;
		/// Field text captured when the report was generated. Never mutated by
		/// a recompute, so TryReconcileAfterDocumentRebuild can tell an undo
		/// returned the line to its original text (and the original match
		/// offsets are valid again) even after edits re-pointed `line_text`.
		std::shared_ptr<std::string const> original_line_text;
		std::string replacement;
		std::string replaced_line;
		std::vector<std::pair<std::size_t, std::size_t>> original_ranges;
		std::vector<std::pair<std::size_t, std::size_t>> replacement_ranges;
		SearchReplaceSettings::Field field = SearchReplaceSettings::Field::TEXT;
		/// Byte range in `line_text` (pre-replacement for replace reports).
		/// Mutable: a recompute moves these to the live match's offsets so
		/// jump/refresh use current coordinates.
		std::size_t match_start = 0;
		std::size_t match_end = 0;
		/// Byte range captured when the report was generated, into
		/// `original_line_text`. Never mutated by a recompute. After an undo
		/// returns the live field to the original text, these are the only
		/// valid offsets again, so TryReconcileAfterDocumentRebuild restores
		/// match_start/match_end/jump_* from them.
		std::size_t original_match_start = 0;
		std::size_t original_match_end = 0;
		/// Range used for jump; post-replacement while the edit still applies,
		/// otherwise the same as match_start/match_end.
		std::size_t jump_start = 0;
		std::size_t jump_end = 0;
		/// Per-row staleness from a targeted recompute (see RecomputeChangedLine).
		/// Set when this hit no longer lines up with the live text; the row is
		/// greyed and JumpToHit refuses it, while other rows stay usable.
		bool per_hit_stale = false;
	};

	agi::Context *c;
	/// Whether the Replacement column is shown (historical report shape).
	bool has_replacement = false;
	/// Whether this panel came from Replace All. Drives RecomputeChangedLine:
	/// replace reports grey a changed row instead of re-running substitution.
	bool is_replace_report = false;
	/// Whether the document still has those replacements applied. Cleared when
	/// undo restores pre-replacement field text so status/jumps stay honest.
	bool replacements_applied = false;
	bool stale = false;
	/// Original search settings, kept so a changed row can be re-matched in
	/// place rather than invalidating the whole report.
	SearchReplaceSettings settings;
	/// Compiled matcher built once from `settings`; reused for every recompute.
	/// Empty when the panel is a replace report or the matcher failed to build.
	aegisub::subtitle_match_report::MatchEnumerator enumerator;

	wxDataViewCtrl *list = nullptr;
	ResultsModel *model = nullptr;
	wxStaticText *status = nullptr;
	wxButton *copy_button = nullptr;
	agi::signal::Connection file_changed_slot;

	std::vector<DisplayHit> hits;
	/// Hit indices grouped by line Id, for O(1) commit intersection and O(k)
	/// access to the k rows affected by one changed subtitle line.
	std::unordered_map<int, std::vector<std::size_t>> hit_indices_by_line;

	void InitCommon(bool replace_mode);
	/// (Re)build the per-panel matcher from `settings`. For a Find report this
	/// uses a copy with `replace_with` cleared so the enumerator's per-match
	/// format expansion short-circuits (the panel never reads replacements on
	/// the recompute path). No-op for replace reports. Catches construction
	/// failures (bad regex) and leaves `enumerator` empty.
	void RebuildEnumerator();
	void PopulateList();
	/// Refresh one result row from its DisplayHit (text + grey state) in
	/// place, preserving selection/scroll. Used after a per-line recompute so a
	/// large report does not get fully rebuilt on every keystroke.
	void UpdateListRow(std::size_t hit_index);
	void UpdateStatusBar();
	void MarkStale();
	/// Undo/redo COMMIT_NEW keeps AssDialogue::Id values. Find reports re-match
	/// every resolved report line against the rebuilt document; replace reports
	/// restore their historical coordinates only when all original field text is
	/// back. A missing line or failed match marks the report stale.
	bool TryReconcileAfterDocumentRebuild();
	/// Re-match one changed line's hits against its live text (Find reports) or
	/// grey them outright (replace reports). Per-hit-stale rows stay listed but
	/// become non-jumpable; the rest of the panel is unaffected. Appends the
	/// indices in `hits` that were touched to `touched`, so the caller re-renders
	/// only those rows without a per-line vector allocation+copy. Returns false
	/// if matching failed and the report had to be marked stale.
	bool RecomputeChangedLine(AssDialogue const& line, std::vector<std::size_t>& touched);
	void OnCommit(AssFileCommitDetails commit);
	void OnActivate(wxDataViewEvent& evt);
	void OnCopySelected(wxCommandEvent&);
	/// Close a user-dismissed results dialog and return keyboard focus to the
	/// still-visible Find/Replace dialog which launched it.
	void CloseAndReturnFocus();
	/// `hit_index` is the row/index in the virtual result model and `hits`.
	void JumpToHit(std::size_t hit_index);

	DialogSearchResults(agi::Context *c, SearchReplaceSettings settings,
	                    std::vector<aegisub::subtitle_match_report::MatchHit> matches);
	DialogSearchResults(agi::Context *c, SearchReplaceSettings settings,
	                    std::vector<aegisub::subtitle_match_report::ReplacementHit> replacements);

public:
	static void Show(agi::Context *context, SearchReplaceSettings settings,
	                 std::vector<aegisub::subtitle_match_report::MatchHit> matches);
	static void Show(agi::Context *context, SearchReplaceSettings settings,
	                 std::vector<aegisub::subtitle_match_report::ReplacementHit> replacements);
	/// Close any open results panel (e.g. after a search with zero hits).
	static void Dismiss(agi::Context *context);

	~DialogSearchResults();
};
