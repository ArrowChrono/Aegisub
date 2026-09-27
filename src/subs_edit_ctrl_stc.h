// Copyright (c) 2005, Rodrigo Braz Monteiro
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

#include "subtitle_character_markers.h"
#include "subtitle_edit_ops.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <wx/stc/stc.h>

class Thesaurus;
namespace agi {
	class SpellChecker;
	struct Context;
	namespace ass { struct DialogueToken; }
}

/// @class SubsStyledTextEditCtrl
/// @brief A Scintilla control with spell checking and syntax highlighting
class SubsStyledTextEditCtrl final : public wxStyledTextCtrl {
#if wxUSE_DRAG_AND_DROP
	class DropTarget;
#endif

#ifdef __WXMSW__
	struct PendingPaintTiming {
		bool pending = false;
		std::uint64_t update_id = 0;
		int text_bytes = 0;
		std::int64_t requested_ns = 0;
	};

	PendingPaintTiming pending_paint_timing;
	std::uint64_t next_paint_timing_id = 0;

	WXLRESULT MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) override;
#endif

	/// Backend spellchecker to use
	std::unique_ptr<agi::SpellChecker> spellchecker;

	/// Backend thesaurus to use
	std::unique_ptr<Thesaurus> thesaurus;

	/// Project context, for splitting lines
	agi::Context *context;
	bool code_mode = false;
	bool InsertCodeText(std::string const& text);

	/// The word right-clicked on, used for spellchecker replacing
	std::string currentWord;

	/// The beginning of the word right-clicked on, for spellchecker replacing
	std::pair<int, int> currentWordPos;

	/// Spellchecker suggestions for the last right-clicked word
	std::vector<std::string> sugs;

	/// Thesaurus suggestions for the last right-clicked word
	std::vector<std::string> thesSugs;

	/// Text of the currently shown calltip, to avoid flickering from
	/// pointlessly reshowing the current tip
	std::string calltip_text;

	/// Position of the currently show calltip
	size_t calltip_position = 0;

	/// Cursor position which the current calltip is for
	int cursor_pos;

	/// The last seen line text, used to avoid reparsing the line for syntax
	/// highlighting when possible
	std::string line_text;

	/// Tokenized version of line_text
	std::vector<agi::ass::DialogueToken> tokenized_line;
	/// Styling context cached with line_text so same text can be reused safely.
	bool style_context_valid = false;
	bool last_template_line = false;

	/// Tag name armed for whole-tag selection on the next double-click.
	std::pair<int, int> repeat_tag_name_bounds{-1, 0};

	/// Last configured face reported missing by SetStyles; dedupes the LOG_D
	/// across the many option subscriptions that trigger a restyle.
	wxString last_missing_font_logged;

	std::string drag_source_text;
	int drag_source_start = 0;
	int drag_source_end = 0;
	int drag_preview_drop = 0;
	bool drag_preview_active = false;
	bool drag_preview_copy = false;
	bool drag_preview_changed = false;

	void OnContextMenu(wxContextMenuEvent &);
	void OnDoubleClick(wxStyledTextEvent&);
	void OnUseSuggestion(wxCommandEvent &event);
	void OnSetDicLanguage(wxCommandEvent &event);
	void OnSetThesLanguage(wxCommandEvent &event);
	void OnLoseFocus(wxFocusEvent &event);
	void OnChar(wxKeyEvent &event);
	void OnKeyDown(wxKeyEvent &event);

	/// Nudge the ASS block under the caret one step through the line.
	/// Returns false when the caret has nothing movable under it.
	bool MoveBlockUnderCaret(aegisub::subtitle_edit_ops::BlockMoveDirection direction);

	void OnStartDrag(wxStyledTextEvent &event);
	void OnDragOver(wxStyledTextEvent &event);
	void OnDoDrop(wxStyledTextEvent &event);

	void CancelTextDragPreview();
	int MapTextDragPreviewPosition(int position) const;
	void SetTextDragPreview(std::string const& text, int selection_start, int selection_end);

	void SetSyntaxStyle(int id, wxFont const& font, std::string const& name, wxColor const& default_background);
	void Subscribe(std::string const& name);

	void UpdateCallTip();
	void UpdateBraceHighlight();
	void SetStyles();
	/// Apply experimental MSW STC rendering technology (DirectWrite opt-in).
	/// Does not re-apply styles; call SetStyles() after technology changes.
	void ApplyScintillaTuning();
#ifdef __WXMSW__
	/// DirectWrite family for StyleSetFaceName (must not go through wxFont::SetFaceName).
	wxString directwrite_face;
	/// Non-zero when DirectWrite styles should use this weight after StyleSetBold.
	int directwrite_style_weight = 0;
	/// True when the resolved DirectWrite face is a software-italic face
	/// (StyleSetFont would flatten it to upright once the face is remapped).
	bool directwrite_style_italic = false;
	/// Cache key for the last successful GDI→DWrite face resolve.
	wxString directwrite_resolve_request_face;
	/// Long-lived system DWrite bridge for edit-box face resolve (lazy).
	std::unique_ptr<class DWriteBridge> dwrite_face_bridge;
#endif

	void UpdateStyle();

	/// Cached colour-swatch spans for the current line_text
	std::vector<aegisub::subtitle_edit_ops::ColorSpan> color_swatch_spans;

	void UpdateColorSwatches();
	void ClearColorSwatchIndicators();
	void OnIndicatorRelease(wxStyledTextEvent& event);

	/// Cached character-marker spans for the current line_text
	std::vector<aegisub::CharacterMarkerSpan> character_marker_spans;
	/// Encoded characters currently installed via SetRepresentation for this feature
	std::set<std::string> installed_character_representations;
	/// True while a character-marker calltip owns the calltip UI
	bool marker_calltip_active = false;
	/// Coalesce multiple option callbacks into one CallAfter refresh
	bool character_marker_refresh_queued = false;

	aegisub::CharacterMarkerShowConfig ReadCharacterMarkerShowConfig() const;
	aegisub::CharacterMarkerErrorConfig ReadCharacterMarkerErrorConfig() const;
	void SubscribeCharacterMarkerOptions();
	void QueueCharacterMarkerRefresh();
	void ApplyCharacterMarkerSettings();
	void UpdateCharacterMarkers();
	void ClearCharacterMarkerIndicators();
	void OnCharacterMarkerDwellStart(wxStyledTextEvent& event);
	void OnCharacterMarkerDwellEnd(wxStyledTextEvent& event);
	wxString BuildCharacterMarkerTooltip(aegisub::CharacterMarkerSpan const& span) const;

	/// Add the thesaurus suggestions to a menu
	void AddThesaurusEntries(wxMenu &menu);

	/// Add the spell checker suggestions to a menu
	void AddSpellCheckerEntries(wxMenu &menu);

	/// Generate a languages submenu from a list of locales and a current language
	/// @param base_id ID to use for the first menu item
	/// @param curLang Currently selected language
	/// @param lang Full list of languages
	wxMenu *GetLanguagesMenu(int base_id, wxString const& curLang, wxArrayString const& langs);

public:
	SubsStyledTextEditCtrl(wxWindow* parent, wxSize size, long style, agi::Context *context);
	~SubsStyledTextEditCtrl();

	void SetTextTo(std::string const& text);
	void SetCodeMode(bool enabled);
	void Paste() override;

	std::pair<int, int> GetBoundsOfWordAtPosition(int pos);

	DECLARE_EVENT_TABLE()
};
