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

#include "subs_edit_ctrl_stc.h"

#include "ass_dialogue.h"
#include "command/command.h"
#include "compat.h"
#include "format.h"
#include "options.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "include/aegisub/spellchecker.h"
#include "perf_trace.h"
#include "selection_controller.h"
#include "automation/lua_source_tools.h"
#include "stc_compat.h"
#include "text_selection_controller.h"
#include "thesaurus.h"
#include "subtitle_character_markers.h"
#include "subtitle_edit_ops.h"
#include "utils.h"

#include <libaegisub/ass/dialogue_parser.h>
#include <libaegisub/calltip_provider.h>
#include <libaegisub/character_count.h>
#include <libaegisub/make_unique.h>
#include <libaegisub/log.h>
#include <libaegisub/spellchecker.h>
#include <libaegisub/string_utils.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <string_view>
#include <utility>

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dnd.h>
#include <wx/intl.h>
#include <wx/log.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include <wx/utils.h>

#ifdef __WXMSW__
#include <windows.h>
#include "font_file_lister_dwrite.h"
#endif

// Maximum number of languages (locales)
// It should be above 100 (at least 242) and probably not more than 1000
#define LANGS_MAX 1000

namespace {
	constexpr int BRACE_HIGHLIGHT_INDICATOR = 2;
	constexpr int BRACE_BAD_INDICATOR = 3;
	// Character markers own indicators 4–9; keep numbers centralized.
	// Primary/alt pairs share style so consecutive same-kind ranges paint as
	// separate units (Scintilla merges runs of a single indicator).
	constexpr int CHAR_MARKER_SPACE_INDICATOR = 4;           // U+0020 / U+3000 dashed box
	constexpr int CHAR_MARKER_SPACE_ALT_INDICATOR = 7;
	constexpr int CHAR_MARKER_OTHER_SPACE_INDICATOR = 5;     // other whitespace dashed underline
	constexpr int CHAR_MARKER_OTHER_SPACE_ALT_INDICATOR = 8;
	constexpr int CHAR_MARKER_ERROR_INDICATOR = 6;           // solid error box
	constexpr int CHAR_MARKER_ERROR_ALT_INDICATOR = 9;
	// Colour swatch indicator over the hex digits of \c-family parameters:
	// a box stroked and tinted in the tag's colour. The colour comes from the
	// per-range indicator value (INDICFLAG_VALUEFORE); the value must carry
	// wxSTC_INDICVALUEBIT or a pure-black swatch would vanish.
	constexpr int COLOR_SWATCH_BOX_INDICATOR = 10;
	constexpr int CHAR_MARKER_DWELL_MS = 500;

	bool IsTemplateLine(agi::Context *context) {
		auto *diag = context ? context->GetCore().selectionController->GetActiveLine() : nullptr;
		return diag
			&& diag->Comment
			&& agi::util::strings::istarts_with(diag->Effect.get(), "template");
	}

#ifdef __WXMSW__
	std::int64_t PaintTimingNowNs() noexcept {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	double PaintTimingDurationMs(std::int64_t started_ns, std::int64_t finished_ns) noexcept {
		return static_cast<double>(finished_ns - started_ns) / 1'000'000.0;
	}

	const char *FaceSourceName(DWriteBridge::TextFormatFace::Source source) {
		switch (source) {
			case DWriteBridge::TextFormatFace::Source::None: return "none";
			case DWriteBridge::TextFormatFace::Source::Raw: return "raw";
			case DWriteBridge::TextFormatFace::Source::LogFont: return "logfont";
			case DWriteBridge::TextFormatFace::Source::Typographic: return "typographic";
			case DWriteBridge::TextFormatFace::Source::GdiFallback: return "gdi-fallback";
		}
		return "?";
	}
#endif

	void ConfigureCharacterMarkerIndicator(wxStyledTextCtrl* ctrl, int indicator, int style, wxColour const& colour, int fill_alpha, int outline_alpha) {
		ctrl->IndicatorSetStyle(indicator, style);
		ctrl->IndicatorSetForeground(indicator, colour);
		ctrl->IndicatorSetUnder(indicator, true);
		ctrl->IndicatorSetAlpha(indicator, fill_alpha);
		ctrl->IndicatorSetOutlineAlpha(indicator, outline_alpha);
	}

	std::string Utf8EncodeCodepoint(char32_t cp) {
		std::string out;
		if (cp <= 0x7F) {
			out.push_back(static_cast<char>(cp));
		} else if (cp <= 0x7FF) {
			out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else if (cp <= 0xFFFF) {
			out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		} else {
			out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
		}
		return out;
	}

	wxString KindTitle(aegisub::CharacterMarkerKind kind, char32_t cp) {
		using K = aegisub::CharacterMarkerKind;
		if ((cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF))
			return _("Variation Selector");
		switch (cp) {
		case 0x0020: return _("Space");
		case 0x3000: return _("Ideographic Space");
		case 0x00A0: return _("No-Break Space");
		case 0x0009: return _("Tab");
		case 0x000A: return _("Line Feed");
		case 0x000D: return _("Carriage Return");
		case 0x200B: return _("Zero Width Space");
		case 0x200C: return _("Zero Width Non-Joiner");
		case 0x200D: return _("Zero Width Joiner");
		case 0x200E: return _("Left-to-Right Mark");
		case 0x200F: return _("Right-to-Left Mark");
		case 0xFEFF: return _("Byte Order Mark");
		case 0x2060: return _("Word Joiner");
		default:
			break;
		}
		switch (kind) {
		case K::Space: return _("Space");
		case K::IdeographicSpace: return _("Ideographic Space");
		case K::NoBreakSpace: return _("No-Break Space");
		case K::UnicodeWhitespace: return _("Unicode Whitespace");
		case K::CarriageReturn: return _("Carriage Return");
		case K::LineFeed: return _("Line Feed");
		case K::Tab: return _("Tab");
		case K::Control: return _("Control Character");
		case K::BidiControl: return _("Bidirectional Control Character");
		case K::JoinControl: return _("Join Control Character");
		case K::Invisible: return _("Invisible Character");
		}
		return _("Invisible Character");
	}

	wxString CategoryDescription(std::string const& code) {
		if (code == "Zs") return _("Space Separator (Zs)");
		if (code == "Zl") return _("Line Separator (Zl)");
		if (code == "Zp") return _("Paragraph Separator (Zp)");
		if (code == "Cc") return _("Control (Cc)");
		if (code == "Cf") return _("Format (Cf)");
		if (code == "Cn") return _("Unassigned (Cn)");
		if (code == "Co") return _("Private Use (Co)");
		if (code == "Cs") return _("Surrogate (Cs)");
		if (!code.empty())
			return wxString::Format(_("General Category (%s)"), to_wx(code).wx_str());
		return _("General Category");
	}

	bool IsHighlightableBrace(int character) {
		return character == '{' || character == '}' || character == '(' || character == ')';
	}

	int ScintillaColourOf(agi::Color const& color) {
		// Scintilla colours are 0x00BBGGRR.
		return (color.b << 16) | (color.g << 8) | color.r;
	}

	/// Sibling of subtitle_edit_ops.cpp's is_escaped_open_brace: same ParseTags
	/// rule (a '{' after an odd number of backslashes is plain text), different
	/// container. This one reads the live control a byte at a time because brace
	/// highlighting runs on every UPDATEUI and must not copy the whole buffer;
	/// the ops one takes the string_view the swatch pass already holds. Keep the
	/// odd/even test in step if either changes.
	bool IsEscapedOpenBrace(wxStyledTextCtrl const& ctrl, int position) {
		if (ctrl.GetCharAt(position) != '{')
			return false;

		int slash_count = 0;
		for (int pos = position - 1; pos >= 0 && ctrl.GetCharAt(pos) == '\\'; --pos)
			++slash_count;
		return (slash_count & 1) != 0;
	}

	int FindMatchingCurlyBrace(wxStyledTextCtrl const& ctrl, int brace_position) {
		int const direction = ctrl.GetCharAt(brace_position) == '{' ? 1 : -1;
		int const brace = ctrl.GetCharAt(brace_position);
		int const opposite = brace == '{' ? '}' : '{';
		int depth = 1;

		for (int pos = brace_position + direction; pos >= 0 && pos < ctrl.GetTextLength(); pos += direction) {
			int const character = ctrl.GetCharAt(pos);
			if (character == '{' && IsEscapedOpenBrace(ctrl, pos))
				continue;
			if (character == brace)
				++depth;
			else if (character == opposite && --depth == 0)
				return pos;
		}

		return wxSTC_INVALID_POSITION;
	}
}

/// Event ids
// Check menu.h for id range allocation before editing this enum
enum {
	EDIT_MENU_SPLIT_PRESERVE = (wxID_HIGHEST + 1) + 4000,
	EDIT_MENU_SPLIT_ESTIMATE,
	EDIT_MENU_SPLIT_VIDEO,
	EDIT_MENU_CUT,
	EDIT_MENU_COPY,
	EDIT_MENU_PASTE,
	EDIT_MENU_SELECT_ALL,
	EDIT_MENU_ADD_TO_DICT,
	EDIT_MENU_REMOVE_FROM_DICT,
	EDIT_MENU_SUGGESTION,
	EDIT_MENU_SUGGESTIONS,
	EDIT_MENU_THESAURUS = (wxID_HIGHEST + 1) + 5000,
	EDIT_MENU_THESAURUS_SUGS,
	EDIT_MENU_DIC_LANGUAGE = (wxID_HIGHEST + 1) + 6000,
	EDIT_MENU_DIC_LANGS,
	EDIT_MENU_THES_LANGUAGE = EDIT_MENU_DIC_LANGUAGE + LANGS_MAX,
	EDIT_MENU_THES_LANGS
};

static bool GetAutoCloseCharacterKey(wxKeyEvent const& event, aegisub::subtitle_edit_ops::AutoCloseKey& key) {
	if (event.CmdDown() || event.AltDown())
		return false;

	int unicode_key = event.GetUnicodeKey();
	if (unicode_key == WXK_NONE)
		return false;

	switch (unicode_key) {
	case '{':
		key = aegisub::subtitle_edit_ops::AutoCloseKey::OpenBrace;
		return true;
	case '}':
		key = aegisub::subtitle_edit_ops::AutoCloseKey::CloseBrace;
		return true;
	case '(':
		key = aegisub::subtitle_edit_ops::AutoCloseKey::OpenParen;
		return true;
	case ')':
		key = aegisub::subtitle_edit_ops::AutoCloseKey::CloseParen;
		return true;
	default:
		return false;
	}
}

static bool GetAutoCloseKeyDownKey(wxKeyEvent const& event, aegisub::subtitle_edit_ops::AutoCloseKey& key) {
	if (event.CmdDown() || event.AltDown())
		return false;

	if (event.GetKeyCode() == WXK_BACK) {
		key = aegisub::subtitle_edit_ops::AutoCloseKey::Backspace;
		return true;
	}

	return false;
}

void ApplyAutoCloseEdit(wxStyledTextCtrl *ctrl, aegisub::subtitle_edit_ops::AutoCloseEdit const& edit) {
	if (edit.replace_start != edit.replace_end || !edit.replacement.empty()) {
		ctrl->BeginUndoAction();
		ctrl->SetSelection(edit.replace_start, edit.replace_end);
		ctrl->ReplaceSelection(wxString::FromUTF8Unchecked(edit.replacement.c_str()));
		ctrl->SetSelection(edit.caret, edit.caret);
		ctrl->EndUndoAction();
	}
	else {
		ctrl->SetSelection(edit.caret, edit.caret);
	}
}

#if wxUSE_DRAG_AND_DROP
class SubsStyledTextEditCtrl::DropTarget final : public wxTextDropTarget {
	SubsStyledTextEditCtrl *ctrl;

public:
	explicit DropTarget(SubsStyledTextEditCtrl *ctrl) : ctrl(ctrl) { }

	bool OnDropText(wxCoord x, wxCoord y, wxString const& data) override {
		if (ctrl->code_mode && (data.Find('\r') != wxNOT_FOUND || data.Find('\n') != wxNOT_FOUND)) {
			ctrl->CancelTextDragPreview();
			int const position = ctrl->PositionFromPoint(wxPoint(x, y));
			if (position < 0)
				return false;
			int const anchor = ctrl->GetAnchor(), caret = ctrl->GetCurrentPos();
			ctrl->SetSelection(position, position);
			if (ctrl->InsertCodeText(from_wx(data)))
				return true;
			ctrl->SetAnchor(anchor);
			ctrl->SetCurrentPos(caret);
			return false;
		}
		return ctrl->DoDropText(x, y, data);
	}

	wxDragResult OnEnter(wxCoord x, wxCoord y, wxDragResult result) override {
		return ctrl->DoDragEnter(x, y, result);
	}

	wxDragResult OnDragOver(wxCoord x, wxCoord y, wxDragResult result) override {
		return ctrl->DoDragOver(x, y, result);
	}

	void OnLeave() override {
		ctrl->CancelTextDragPreview();
		ctrl->DoDragLeave();
	}
};
#endif

SubsStyledTextEditCtrl::SubsStyledTextEditCtrl(wxWindow* parent, wxSize wsize, long style, agi::Context *context)
: wxStyledTextCtrl(parent, -1, wxDefaultPosition, wsize, style)
, spellchecker(SpellCheckerFactory::GetSpellChecker())
, thesaurus(agi::make_unique<Thesaurus>())
, context(context)
{
	aegisub::stc::ConfigureWindowsSelectionRendering(this);
	ApplyScintillaTuning();

#if wxUSE_DRAG_AND_DROP
	SetDropTarget(new DropTarget(this));
#endif

	// Set properties
	SetWrapMode(wxSTC_WRAP_WORD);
	SetMarginWidth(1,0);
#if wxCHECK_VERSION (3, 1, 0)
	UsePopUp(wxSTC_POPUP_NEVER);
#else
	UsePopUp(false);
#endif
	SetStyles();

	// Set hotkeys
#if wxCHECK_VERSION (3, 1, 0)
	CmdKeyClear(wxSTC_KEY_RETURN, wxSTC_KEYMOD_CTRL);
	CmdKeyClear(wxSTC_KEY_RETURN, wxSTC_KEYMOD_SHIFT);
	CmdKeyClear(wxSTC_KEY_RETURN, wxSTC_KEYMOD_NORM);
	CmdKeyClear(wxSTC_KEY_TAB, wxSTC_KEYMOD_NORM);
	CmdKeyClear(wxSTC_KEY_TAB, wxSTC_KEYMOD_SHIFT);
	CmdKeyClear('D', wxSTC_KEYMOD_CTRL);
	CmdKeyClear('L', wxSTC_KEYMOD_CTRL);
	CmdKeyClear('L', wxSTC_KEYMOD_CTRL | wxSTC_KEYMOD_SHIFT);
	CmdKeyClear('T', wxSTC_KEYMOD_CTRL);
	CmdKeyClear('T', wxSTC_KEYMOD_CTRL | wxSTC_KEYMOD_SHIFT);
	CmdKeyClear('U', wxSTC_KEYMOD_CTRL);
	CmdKeyClear(wxSTC_KEY_HOME, wxSTC_KEYMOD_NORM);
	CmdKeyClear(wxSTC_KEY_HOME, wxSTC_KEYMOD_SHIFT);
	CmdKeyClear(wxSTC_KEY_END, wxSTC_KEYMOD_NORM);
	CmdKeyClear(wxSTC_KEY_END, wxSTC_KEYMOD_SHIFT);
#else
	CmdKeyClear(wxSTC_KEY_RETURN,wxSTC_SCMOD_CTRL);
	CmdKeyClear(wxSTC_KEY_RETURN,wxSTC_SCMOD_SHIFT);
	CmdKeyClear(wxSTC_KEY_RETURN,wxSTC_SCMOD_NORM);
	CmdKeyClear(wxSTC_KEY_TAB,wxSTC_SCMOD_NORM);
	CmdKeyClear(wxSTC_KEY_TAB,wxSTC_SCMOD_SHIFT);
	CmdKeyClear('D',wxSTC_SCMOD_CTRL);
	CmdKeyClear('L',wxSTC_SCMOD_CTRL);
	CmdKeyClear('L',wxSTC_SCMOD_CTRL | wxSTC_SCMOD_SHIFT);
	CmdKeyClear('T',wxSTC_SCMOD_CTRL);
	CmdKeyClear('T',wxSTC_SCMOD_CTRL | wxSTC_SCMOD_SHIFT);
	CmdKeyClear('U',wxSTC_SCMOD_CTRL);
	CmdKeyClear(wxSTC_KEY_HOME,wxSTC_SCMOD_NORM);
	CmdKeyClear(wxSTC_KEY_HOME,wxSTC_SCMOD_SHIFT);
	CmdKeyClear(wxSTC_KEY_END,wxSTC_SCMOD_NORM);
	CmdKeyClear(wxSTC_KEY_END,wxSTC_SCMOD_SHIFT);
#endif

	using std::bind;

	Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
		int const key = event.GetKeyCode();
		bool const paste_shortcut =
			((key == 'V' || key == 'v' || key == WXK_CONTROL_V) && event.GetModifiers() == wxMOD_CMD) ||
			((key == WXK_INSERT || key == WXK_NUMPAD_INSERT) && event.GetModifiers() == wxMOD_SHIFT);
		if (paste_shortcut) {
			if (code_mode || CanPaste())
				Paste();
		}
		else
			event.Skip();
	});
	Bind(wxEVT_CHAR_HOOK, &SubsStyledTextEditCtrl::OnKeyDown, this);
	Bind(wxEVT_CHAR, &SubsStyledTextEditCtrl::OnChar, this);
	Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
		int const pos = PositionFromPoint(event.GetPosition());
		if (pos < repeat_tag_name_bounds.first ||
			pos >= repeat_tag_name_bounds.first + repeat_tag_name_bounds.second)
			repeat_tag_name_bounds = {-1, 0};
		event.Skip();
	});

	Bind(wxEVT_MENU, bind(&SubsStyledTextEditCtrl::Cut, this), EDIT_MENU_CUT);
	Bind(wxEVT_MENU, bind(&SubsStyledTextEditCtrl::Copy, this), EDIT_MENU_COPY);
	Bind(wxEVT_MENU, bind(&SubsStyledTextEditCtrl::Paste, this), EDIT_MENU_PASTE);
	Bind(wxEVT_MENU, bind(&SubsStyledTextEditCtrl::SelectAll, this), EDIT_MENU_SELECT_ALL);

	if (context) {
		Bind(wxEVT_MENU, bind(&cmd::call, "edit/line/split/preserve", context), EDIT_MENU_SPLIT_PRESERVE);
		Bind(wxEVT_MENU, bind(&cmd::call, "edit/line/split/estimate", context), EDIT_MENU_SPLIT_ESTIMATE);
		Bind(wxEVT_MENU, bind(&cmd::call, "edit/line/split/video", context), EDIT_MENU_SPLIT_VIDEO);
	}

	Bind(wxEVT_CONTEXT_MENU, &SubsStyledTextEditCtrl::OnContextMenu, this);
	Bind(wxEVT_IDLE, std::bind(&SubsStyledTextEditCtrl::UpdateCallTip, this));
	Bind(wxEVT_STC_UPDATEUI, [this](wxStyledTextEvent& event) {
		UpdateBraceHighlight();
		event.Skip();
	});
	Bind(wxEVT_STC_DOUBLECLICK, &SubsStyledTextEditCtrl::OnDoubleClick, this);
	Bind(wxEVT_STC_INDICATOR_RELEASE, &SubsStyledTextEditCtrl::OnIndicatorRelease, this);
	Bind(wxEVT_STC_START_DRAG, &SubsStyledTextEditCtrl::OnStartDrag, this);
	Bind(wxEVT_STC_DRAG_OVER, &SubsStyledTextEditCtrl::OnDragOver, this);
	Bind(wxEVT_STC_DO_DROP, &SubsStyledTextEditCtrl::OnDoDrop, this);
	Bind(wxEVT_STC_STYLENEEDED, [=](wxStyledTextEvent&) {
		if (code_mode)
			return;
		{
			std::string text = GetTextRaw().data();
			if (text == line_text)
				return;
			repeat_tag_name_bounds = {-1, 0};
			line_text = move(text);
		}

		UpdateStyle();
	});

	OPT_SUB("Subtitle/Edit Box/Font Face", &SubsStyledTextEditCtrl::SetStyles, this);
	OPT_SUB("Subtitle/Edit Box/Font Size", &SubsStyledTextEditCtrl::SetStyles, this);
	OPT_SUB("Subtitle/Edit Box/Use DirectWrite", [this](agi::OptionValue const&) {
		ApplyScintillaTuning();
		SetStyles();
	});
	Subscribe("Normal");
	Subscribe("Comment");
	Subscribe("Drawing Command");
	Subscribe("Drawing X");
	Subscribe("Drawing Y");
	OPT_SUB("Colour/Subtitle/Syntax/Underline/Drawing Endpoint", &SubsStyledTextEditCtrl::SetStyles, this);
	Subscribe("Brackets");
	Subscribe("Slashes");
	Subscribe("Tags");
	Subscribe("Error");
	Subscribe("Parameters");
	Subscribe("Line Break");
	Subscribe("Karaoke Template");
	Subscribe("Karaoke Variable");

	OPT_SUB("Colour/Subtitle/Background", &SubsStyledTextEditCtrl::SetStyles, this);
	OPT_SUB("Subtitle/Highlight/Syntax", [this](agi::OptionValue const&) {
		if (code_mode)
			SetStyles();
		UpdateStyle();
	});
	OPT_SUB("Subtitle/Highlight/Color Swatches", &SubsStyledTextEditCtrl::UpdateStyle, this);
	OPT_SUB("App/Call Tips", &SubsStyledTextEditCtrl::UpdateCallTip, this);

	SubscribeCharacterMarkerOptions();
	SetMouseDwellTime(CHAR_MARKER_DWELL_MS);
	Bind(wxEVT_STC_DWELLSTART, &SubsStyledTextEditCtrl::OnCharacterMarkerDwellStart, this);
	Bind(wxEVT_STC_DWELLEND, &SubsStyledTextEditCtrl::OnCharacterMarkerDwellEnd, this);
	ApplyCharacterMarkerSettings();

	Bind(wxEVT_MENU, [=](wxCommandEvent&) {
		if (spellchecker) spellchecker->AddWord(currentWord);
		UpdateStyle();
		SetFocus();
	}, EDIT_MENU_ADD_TO_DICT);

	Bind(wxEVT_MENU, [=](wxCommandEvent&) {
		if (spellchecker) spellchecker->RemoveWord(currentWord);
		UpdateStyle();
		SetFocus();
	}, EDIT_MENU_REMOVE_FROM_DICT);
}

SubsStyledTextEditCtrl::~SubsStyledTextEditCtrl() {
}

void SubsStyledTextEditCtrl::SetCodeMode(bool enabled) {
	if (code_mode == enabled)
		return;
	CancelTextDragPreview();
	code_mode = enabled;
	style_context_valid = false;
	tokenized_line.clear();
	repeat_tag_name_bounds = {-1, 0};
	CallTipCancel();
	calltip_text.clear();
	calltip_position = static_cast<size_t>(-1);
	cursor_pos = -1;
	marker_calltip_active = false;
	SetIndicatorCurrent(0);
	IndicatorClearRange(0, GetTextLength());
	ClearCharacterMarkerIndicators();
	character_marker_spans.clear();
	ApplyCharacterMarkerSettings();
	ClearColorSwatchIndicators();
	color_swatch_spans.clear();
	BraceHighlight(wxSTC_INVALID_POSITION, wxSTC_INVALID_POSITION);
	SetLexer(code_mode ? wxSTC_LEX_LUA : wxSTC_LEX_CONTAINER);
	SetKeyWords(0, to_wx(code_mode ? "and break do else elseif end false for function goto if in local nil not or repeat return then true until while" : ""));
	SetStyles();
	line_text = GetTextRaw().data();
	UpdateStyle();
	UpdateBraceHighlight();
}

#ifdef __WXMSW__
WXLRESULT SubsStyledTextEditCtrl::MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) {
	if (message == WM_SIZE) {
		perf_trace::VideoUiDurationScope trace("subs_edit_stc.resize");
		return wxStyledTextCtrl::MSWWindowProc(message, wParam, lParam);
	}
	if (message != WM_PAINT)
		return wxStyledTextCtrl::MSWWindowProc(message, wParam, lParam);

	perf_trace::VideoUiDurationScope trace("subs_edit_stc.paint");
	auto const timing = pending_paint_timing;
	pending_paint_timing.pending = false;
	auto const paint_started_ns = timing.pending ? PaintTimingNowNs() : 0;
	auto const result = wxStyledTextCtrl::MSWWindowProc(message, wParam, lParam);
	if (!timing.pending)
		return result;

	auto const paint_finished_ns = PaintTimingNowNs();
	auto const technology = GetTechnology();
	LOG_I("subtitle/editbox/stc_paint_timing")
		<< "update_id=" << timing.update_id
		<< " phase=first_paint_complete"
		<< " text_bytes=" << timing.text_bytes
		<< " technology=" << (technology == wxSTC_TECHNOLOGY_DIRECTWRITE ? "directwrite" : "default")
		<< " technology_code=" << technology
		<< " paint_queue_wait_ms=" << PaintTimingDurationMs(timing.requested_ns, paint_started_ns)
		<< " paint_ms=" << PaintTimingDurationMs(paint_started_ns, paint_finished_ns);
	return result;
}
#endif

void SubsStyledTextEditCtrl::ApplyScintillaTuning() {
#ifdef __WXMSW__
	// Experimental: default remains GDI (wxSTC_TECHNOLOGY_DEFAULT). DirectWrite
	// was tried then reverted on Win10 (fac78e6a9); keep it opt-in and log the
	// actual technology in case SetTechnology silently fails to initialise D2D.
	bool const want_directwrite = OPT_GET("Subtitle/Edit Box/Use DirectWrite")->GetBool();
	int const requested = want_directwrite
		? wxSTC_TECHNOLOGY_DIRECTWRITE
		: wxSTC_TECHNOLOGY_DEFAULT;
	SetTechnology(requested);
	int const actual = GetTechnology();
	if (perf_trace::IsCategoryEnabled(perf_trace::Category::Log) ||
	    (want_directwrite && actual != wxSTC_TECHNOLOGY_DIRECTWRITE)) {
		LOG_I("subtitle/editbox/stc_technology")
			<< "requested=" << (want_directwrite ? "directwrite" : "default")
			<< " actual=" << (actual == wxSTC_TECHNOLOGY_DIRECTWRITE ? "directwrite" : "default")
			<< " actual_code=" << actual;
	}
#endif
}

void SubsStyledTextEditCtrl::Subscribe(std::string const& name) {
	OPT_SUB("Colour/Subtitle/Syntax/" + name, &SubsStyledTextEditCtrl::SetStyles, this);
	OPT_SUB("Colour/Subtitle/Syntax/Background/" + name, &SubsStyledTextEditCtrl::SetStyles, this);
	OPT_SUB("Colour/Subtitle/Syntax/Bold/" + name, &SubsStyledTextEditCtrl::SetStyles, this);
}

BEGIN_EVENT_TABLE(SubsStyledTextEditCtrl,wxStyledTextCtrl)
	EVT_KILL_FOCUS(SubsStyledTextEditCtrl::OnLoseFocus)

	EVT_MENU_RANGE(EDIT_MENU_SUGGESTIONS,EDIT_MENU_THESAURUS-1,SubsStyledTextEditCtrl::OnUseSuggestion)
	EVT_MENU_RANGE(EDIT_MENU_THESAURUS_SUGS,EDIT_MENU_DIC_LANGUAGE-1,SubsStyledTextEditCtrl::OnUseSuggestion)
	EVT_MENU_RANGE(EDIT_MENU_DIC_LANGS,EDIT_MENU_THES_LANGUAGE-1,SubsStyledTextEditCtrl::OnSetDicLanguage)
	EVT_MENU_RANGE(EDIT_MENU_THES_LANGS,EDIT_MENU_THES_LANGS+LANGS_MAX,SubsStyledTextEditCtrl::OnSetThesLanguage)
END_EVENT_TABLE()

void SubsStyledTextEditCtrl::OnLoseFocus(wxFocusEvent &event) {
	repeat_tag_name_bounds = {-1, 0};
	CallTipCancel();
	if (marker_calltip_active) {
		marker_calltip_active = false;
		// Allow syntax calltips to resume after focus returns.
		calltip_position = static_cast<size_t>(-1);
		calltip_text.clear();
		cursor_pos = -1;
	}
	event.Skip();
}

void SubsStyledTextEditCtrl::OnChar(wxKeyEvent &event) {
	if (code_mode) {
		event.Skip(event.GetKeyCode() != WXK_RETURN);
		return;
	}
	aegisub::subtitle_edit_ops::AutoCloseKey auto_close_key;
	if (!GetAutoCloseCharacterKey(event, auto_close_key)) {
		event.Skip();
		return;
	}

	wxCharBuffer old = GetTextRaw();
	auto const edit = aegisub::subtitle_edit_ops::BuildAutoCloseEdit(
		std::string_view(old.data(), old.length()),
		GetSelectionStart(),
		GetSelectionEnd(),
		auto_close_key);

	if (!edit.handled) {
		event.Skip();
		return;
	}

	ApplyAutoCloseEdit(this, edit);
}

void SubsStyledTextEditCtrl::OnKeyDown(wxKeyEvent &event) {
	repeat_tag_name_bounds = {-1, 0};
	event.Skip();
	if (code_mode) {
		if (event.GetKeyCode() == WXK_TAB && !event.CmdDown() && !event.AltDown()) {
			if (event.ShiftDown())
				CmdKeyExecute(wxSTC_CMD_BACKTAB);
			else
				ReplaceSelection(wxS("\t"));
			event.Skip(false);
		}
		else if ((event.GetKeyCode() == WXK_HOME || event.GetKeyCode() == WXK_END) && !event.CmdDown() && !event.AltDown()) {
			int const position = event.GetKeyCode() == WXK_HOME ? 0 : GetTextLength();
			if (event.ShiftDown())
				SetCurrentPos(position);
			else
				SetSelection(position, position);
			event.Skip(false);
		}
		else if ((event.GetKeyCode() == WXK_LEFT || event.GetKeyCode() == WXK_RIGHT) && event.GetModifiers() == wxMOD_ALT)
			event.Skip(false);
		return;
	}

	// Smart Home: navigate backward through ASS text blocks.
	if (event.GetKeyCode() == WXK_HOME && !event.CmdDown() && !event.AltDown()) {
		bool shift = event.ShiftDown();
		int anchor = GetAnchor();
		int pos = GetCurrentPos();
		int target = aegisub::subtitle_edit_ops::GetPreviousBlockStart(tokenized_line, pos);

		// Use CallAfter to set selection after all CHAR_HOOK handlers have run,
		// since the parent SubsEditBox::OnKeyDown re-skips the event via hotkey::check.
		if (shift)
			CallAfter([this, anchor, target] {
				SetAnchor(anchor);
				SetCurrentPos(target);
			});
		else
			CallAfter([this, target] { SetSelection(target, target); });

		event.Skip(false);
		return;
	}

	// Smart End: navigate forward through ASS text blocks.
	if (event.GetKeyCode() == WXK_END && !event.CmdDown() && !event.AltDown()) {
		bool shift = event.ShiftDown();
		int anchor = GetAnchor();
		int pos = GetCurrentPos();
		int target = aegisub::subtitle_edit_ops::GetNextBlockEnd(tokenized_line, pos);

		// Use CallAfter to set selection after all CHAR_HOOK handlers have run,
		// since the parent SubsEditBox::OnKeyDown re-skips the event via hotkey::check.
		if (shift)
			CallAfter([this, anchor, target] {
				SetAnchor(anchor);
				SetCurrentPos(target);
			});
		else
			CallAfter([this, target] { SetSelection(target, target); });

		event.Skip(false);
		return;
	}

	// Alt+Left/Right: walk the block under the caret through the line one step
	// at a time. Alt+Shift+arrow is Scintilla's rectangular selection, so this
	// requires Alt alone.
	if ((event.GetKeyCode() == WXK_LEFT || event.GetKeyCode() == WXK_RIGHT)
		&& event.GetModifiers() == wxMOD_ALT)
	{
		if (MoveBlockUnderCaret(event.GetKeyCode() == WXK_LEFT
			? aegisub::subtitle_edit_ops::BlockMoveDirection::Left
			: aegisub::subtitle_edit_ops::BlockMoveDirection::Right))
		{
			event.Skip(false);
			return;
		}
	}

	aegisub::subtitle_edit_ops::AutoCloseKey auto_close_key;
	if (GetAutoCloseKeyDownKey(event, auto_close_key)) {
		wxCharBuffer old = GetTextRaw();
		auto const edit = aegisub::subtitle_edit_ops::BuildAutoCloseEdit(
			std::string_view(old.data(), old.length()),
			GetSelectionStart(),
			GetSelectionEnd(),
			auto_close_key);

		if (edit.handled) {
			ApplyAutoCloseEdit(this, edit);
			event.Skip(false);
			return;
		}
	}

	// Workaround for wxSTC eating tabs.
	if (event.GetKeyCode() == WXK_TAB)
		Navigate(event.ShiftDown() ? wxNavigationKeyEvent::IsBackward : wxNavigationKeyEvent::IsForward);
	else if (event.GetKeyCode() == WXK_RETURN && event.GetModifiers() == wxMOD_SHIFT) {
		auto sel_start = GetSelectionStart(), sel_end = GetSelectionEnd();
		wxCharBuffer old = GetTextRaw();
		std::string data(old.data(), sel_start);
		data.append("\\N");
		data.append(old.data() + sel_end, old.length() - sel_end);
		SetTextRaw(data.c_str());

		SetSelection(sel_start + 2, sel_start + 2);
		event.Skip(false);
	}
}

bool SubsStyledTextEditCtrl::MoveBlockUnderCaret(aegisub::subtitle_edit_ops::BlockMoveDirection direction) {
	// Tokenize the live text rather than reusing tokenized_line: that pair is
	// refreshed from STYLENEEDED at paint time, and an edit spliced at byte
	// offsets from tokens a keystroke older would cut the wrong bytes.
	wxCharBuffer const buffer = GetTextRaw();
	std::string const text(buffer.data(), buffer.length());
	auto const tokens = agi::ass::TokenizeDialogueBody(text, IsTemplateLine(context));

	int const anchor = GetAnchor();
	int const caret = GetCurrentPos();
	auto const edit = aegisub::subtitle_edit_ops::MoveBlockAtPosition(
		text, tokens, caret, direction);
	if (!edit.handled)
		return false;

	auto in_block = [&](int position) {
		return position >= edit.block_start && position <= edit.block_end;
	};
	// A selection wholly inside the block rides along with it, so a selected
	// tag stays selected across a run of nudges; anything wider collapses to
	// the caret, which the block carries either way.
	bool const keep_selection = in_block(anchor) && in_block(caret);
	int const new_caret = in_block(caret) ? caret + edit.delta : caret;
	int const new_anchor = keep_selection ? anchor + edit.delta : new_caret;

	BeginUndoAction();
	SetSelection(edit.replace_start, edit.replace_end);
	ReplaceSelection(wxString::FromUTF8Unchecked(edit.replacement.c_str()));
	SetAnchor(new_anchor);
	SetCurrentPos(new_caret);
	EndUndoAction();
	return true;
}

void SubsStyledTextEditCtrl::OnStartDrag(wxStyledTextEvent &event) {
	CancelTextDragPreview();

	drag_source_start = GetSelectionStart();
	drag_source_end = GetSelectionEnd();
	if (drag_source_start == drag_source_end) {
		event.Skip();
		return;
	}

	wxCharBuffer text = GetTextRaw();
	drag_source_text.assign(text.data(), text.length());
	drag_preview_drop = drag_source_start;
	drag_preview_active = true;
	drag_preview_copy = false;
	drag_preview_changed = false;
	event.Skip();
}

int SubsStyledTextEditCtrl::MapTextDragPreviewPosition(int position) const {
	return aegisub::subtitle_edit_ops::MapTextDragPreviewPosition(
		position,
		static_cast<int>(drag_source_text.size()),
		drag_source_start,
		drag_source_end,
		drag_preview_drop,
		drag_preview_copy,
		drag_preview_changed);
}

void SubsStyledTextEditCtrl::SetTextDragPreview(std::string const& text, int selection_start, int selection_end) {
	int const mod_event_mask = GetModEventMask();
	bool const collecting_undo = GetUndoCollection();
	bool const event_handler_enabled = GetEvtHandlerEnabled();

	// Preview edits are transient UI state, not subtitle edits or undo steps.
	SetModEventMask(0);
	SetEvtHandlerEnabled(false);
	if (collecting_undo)
		SetUndoCollection(false);

	SetTargetRange(0, GetTextLength());
	ReplaceTargetRaw(text.data(), static_cast<int>(text.size()));
	SetSelection(selection_start, selection_end);

	if (collecting_undo)
		SetUndoCollection(true);
	SetEvtHandlerEnabled(event_handler_enabled);
	SetModEventMask(mod_event_mask);

	line_text = text;
	UpdateStyle();
	UpdateBraceHighlight();
	Refresh(false);
}

void SubsStyledTextEditCtrl::CancelTextDragPreview() {
	if (!drag_preview_active)
		return;

	SetTextDragPreview(drag_source_text, drag_source_start, drag_source_end);
	drag_preview_active = false;
	drag_preview_changed = false;
}

void SubsStyledTextEditCtrl::OnDragOver(wxStyledTextEvent &event) {
	if (!drag_preview_active) {
		event.Skip();
		return;
	}

	int const drop_position = MapTextDragPreviewPosition(event.GetPosition());
	bool const copy = event.GetDragResult() == wxDragCopy;
	bool const move = event.GetDragResult() == wxDragMove;
	if ((move || copy) && drop_position == drag_preview_drop && copy == drag_preview_copy) {
		event.SetPosition(drop_position);
		event.Skip();
		return;
	}
	if (!move && !copy) {
		SetTextDragPreview(drag_source_text, drag_source_start, drag_source_end);
		drag_preview_drop = drop_position;
		drag_preview_copy = copy;
		drag_preview_changed = false;
		event.Skip();
		return;
	}

	auto preview = aegisub::subtitle_edit_ops::BuildTextDragPreview(
		drag_source_text,
		drag_source_start,
		drag_source_end,
		drop_position,
		copy);
	SetTextDragPreview(preview.text, preview.selection_start, preview.selection_end);
	drag_preview_drop = drop_position;
	drag_preview_copy = copy;
	drag_preview_changed = preview.changed;
	event.SetPosition(drop_position);
	event.Skip();
}

void SubsStyledTextEditCtrl::OnDoDrop(wxStyledTextEvent &event) {
	if (!drag_preview_active) {
		event.Skip();
		return;
	}

	int const drop_position = MapTextDragPreviewPosition(event.GetPosition());
	SetTextDragPreview(drag_source_text, drag_source_start, drag_source_end);
	drag_preview_active = false;
	drag_preview_changed = false;
	event.SetPosition(drop_position);
	event.Skip();
}

void SubsStyledTextEditCtrl::SetSyntaxStyle(int id, wxFont const& font, std::string const& name, wxColor const& default_background) {
	// Size/encoding come from a valid GDI-facing wxFont. DirectWrite family
	// names must not pass through wxFont::SetFaceName — IsValidFacename only
	// knows GDI EnumFontFamiliesEx names and UnRef()s the font on failure.
	bool const syntax_bold = OPT_GET("Colour/Subtitle/Syntax/Bold/" + name)->GetBool();
#ifdef __WXMSW__
	if (!directwrite_face.empty()) {
		// One StyleSetFontAttr instead of StyleSetFont (GDI face) followed by
		// StyleSetFaceName — each StyleSetFaceName sends SCI_STYLESETFONT with
		// its own InvalidateStyleRedraw. resolved.italic is threaded through so
		// an italic edit-box face is not flattened to upright by the remap.
		StyleSetFontAttr(id, font.GetPointSize(), directwrite_face,
		                 syntax_bold, directwrite_style_italic, false,
		                 font.GetEncoding());
		// StyleSetBold inside StyleSetFontAttr writes 400/700 only. Restore the
		// resolved weight for non-bold styles so Medium (500) etc. are kept;
		// bold styles keep 700 and let DirectWrite pick or synthesize a bold face.
		if (directwrite_style_weight > 0 && !syntax_bold)
			StyleSetWeight(id, directwrite_style_weight);
	}
	else
#endif
	{
		StyleSetFont(id, font);
		StyleSetBold(id, syntax_bold);
	}
	StyleSetForeground(id, to_wx(OPT_GET("Colour/Subtitle/Syntax/" + name)->GetColor()));
	const agi::OptionValue *background = OPT_GET("Colour/Subtitle/Syntax/Background/" + name);
	if (background->GetType() == agi::OptionType::Color)
		StyleSetBackground(id, to_wx(background->GetColor()));
	else
		StyleSetBackground(id, default_background);
}

void SubsStyledTextEditCtrl::SetStyles() {
	wxFont font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
	font.SetEncoding(wxFONTENCODING_DEFAULT); // this solves problems with some fonts not working properly
	wxString fontname = FontFace("Subtitle/Edit Box");
	if (!fontname.empty() && !font.SetFaceName(fontname)) {
		// The configured face was uninstalled or renamed. SetFaceName failed
		// and UnRef'd the shared refdata; the next SetPointSize would then
		// AllocExclusive a fresh default and silently drop the explicit
		// encoding above. Rebuild from the system default instead.
		// Many option subscriptions call SetStyles(); only log when the
		// missing face actually changes, not on every restyle.
		if (fontname != last_missing_font_logged) {
			last_missing_font_logged = fontname;
			LOG_D("subtitle/editbox/font")
				<< "configured edit-box font face is unavailable, using system default: "
				<< from_wx(fontname);
		}
		fontname.clear();
		font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
		font.SetEncoding(wxFONTENCODING_DEFAULT);
	}
	else
		last_missing_font_logged.clear();
	font.SetPointSize(OPT_GET("Subtitle/Edit Box/Font Size")->GetInt());

#ifdef __WXMSW__
	// Map GDI face strings onto DirectWrite CreateTextFormat family+weight.
	// Do not write the DWrite family back into wxFont (SetFaceName would UnRef
	// when the name is not a GDI-enumerated facename).
	if (GetTechnology() != wxSTC_TECHNOLOGY_DIRECTWRITE || fontname.empty()) {
		directwrite_face.clear();
		directwrite_style_weight = 0;
		directwrite_style_italic = false;
		directwrite_resolve_request_face.clear();
	}
	else if (directwrite_resolve_request_face != fontname) {
		if (!dwrite_face_bridge)
			dwrite_face_bridge = std::make_unique<DWriteBridge>(DWriteBridgeMode::SystemOnly);
		auto resolved = dwrite_face_bridge->ResolveTextFormatFaceFromGdiFace(
			from_wx(fontname), FW_NORMAL, false);
		directwrite_resolve_request_face = fontname;
		// GdiFallback returns the unproven GDI face together with weight/italic
		// measured from GDI's own substitution for it — DirectWrite substitutes
		// independently, so applying those to the same face name would be worse
		// than not remapping at all. Fall back to the plain GDI branch.
		bool const use_dwrite_face = resolved.ok &&
			resolved.source != DWriteBridge::TextFormatFace::Source::GdiFallback;
		if (use_dwrite_face) {
			directwrite_face = to_wx(resolved.family);
			directwrite_style_weight = resolved.weight;
			directwrite_style_italic = resolved.italic;
			if (perf_trace::IsCategoryEnabled(perf_trace::Category::Log)) {
				// gdi_selected only exists on the HDC probe, which the raw
				// short-circuit skips; omit it there so the log does not look
				// like a dropped field.
				std::string const gdi_log = resolved.gdi_selected_face.empty()
					? std::string()
					: " gdi_selected=" + resolved.gdi_selected_face;
				LOG_I("subtitle/editbox/stc_dwrite_face")
					<< "requested=" << from_wx(fontname)
					<< " source=" << FaceSourceName(resolved.source)
					<< gdi_log
					<< " dwrite_family=" << resolved.family
					<< " weight=" << resolved.weight
					<< " italic=" << (resolved.italic ? 1 : 0);
			}
		}
		else if (resolved.ok) {
			// DirectWrite present but the face name is not provable in the
			// system collection; keep GDI rendering for it.
			directwrite_face.clear();
			directwrite_style_weight = 0;
			directwrite_style_italic = false;
			if (perf_trace::IsCategoryEnabled(perf_trace::Category::Log)) {
				std::string const gdi_log = resolved.gdi_selected_face.empty()
					? std::string()
					: " gdi_selected=" + resolved.gdi_selected_face;
				LOG_I("subtitle/editbox/stc_dwrite_face")
					<< "requested=" << from_wx(fontname)
					<< " source=" << FaceSourceName(resolved.source)
					<< gdi_log
					<< " dwrite_face=unused";
			}
		}
		else {
			// ok == false ⟺ no DirectWrite at all: every resolvable branch
			// fills a family, so there is no second failure mode to report.
			directwrite_face.clear();
			directwrite_style_weight = 0;
			directwrite_style_italic = false;
			if (perf_trace::IsCategoryEnabled(perf_trace::Category::Log)) {
				LOG_I("subtitle/editbox/stc_dwrite_face")
					<< "requested=" << from_wx(fontname)
					<< " dwrite=unavailable";
			}
		}
	}
#endif

	auto default_background = to_wx(OPT_GET("Colour/Subtitle/Background")->GetColor());

	namespace ss = agi::ass::SyntaxStyle;
	// STYLE_DEFAULT participates in line metrics (ascent/descent, tab width).
	// Keep it on the same face as syntax styles so row height matches the edit
	// font. Same single StyleSetFontAttr as SetSyntaxStyle — StyleSetFaceName
	// after StyleSetFont would send a second SCI_STYLESETFONT with its own
	// InvalidateStyleRedraw. Fields are set/cleared together inside the guard.
#ifdef __WXMSW__
	if (!directwrite_face.empty()) {
		StyleSetFontAttr(wxSTC_STYLE_DEFAULT, font.GetPointSize(), directwrite_face,
		                 false, directwrite_style_italic, false,
		                 font.GetEncoding());
		if (directwrite_style_weight > 0)
			StyleSetWeight(wxSTC_STYLE_DEFAULT, directwrite_style_weight);
	}
	else
#endif
	{
		StyleSetFont(wxSTC_STYLE_DEFAULT, font);
	}
	SetSyntaxStyle(ss::NORMAL, font, "Normal", default_background);
	SetSyntaxStyle(ss::COMMENT, font, "Comment", default_background);
	SetSyntaxStyle(ss::DRAWING_CMD, font, "Drawing Command", default_background);
	SetSyntaxStyle(ss::DRAWING_X, font, "Drawing X", default_background);
	SetSyntaxStyle(ss::DRAWING_Y, font, "Drawing Y", default_background);
	SetSyntaxStyle(ss::DRAWING_ENDPOINT_X, font, "Drawing X", default_background);
	SetSyntaxStyle(ss::DRAWING_ENDPOINT_Y, font, "Drawing Y", default_background);
	StyleSetUnderline(ss::DRAWING_ENDPOINT_X, OPT_GET("Colour/Subtitle/Syntax/Underline/Drawing Endpoint")->GetBool());
	StyleSetUnderline(ss::DRAWING_ENDPOINT_Y, OPT_GET("Colour/Subtitle/Syntax/Underline/Drawing Endpoint")->GetBool());
	SetSyntaxStyle(ss::OVERRIDE, font, "Brackets", default_background);
	SetSyntaxStyle(ss::PUNCTUATION, font, "Slashes", default_background);
	SetSyntaxStyle(ss::TAG, font, "Tags", default_background);
	SetSyntaxStyle(ss::ERROR, font, "Error", default_background);
	SetSyntaxStyle(ss::PARAMETER, font, "Parameters", default_background);
	SetSyntaxStyle(ss::LINE_BREAK, font, "Line Break", default_background);
	SetSyntaxStyle(ss::KARAOKE_TEMPLATE, font, "Karaoke Template", default_background);
	SetSyntaxStyle(ss::KARAOKE_VARIABLE, font, "Karaoke Variable", default_background);
	if (code_mode) {
		for (int style = wxSTC_LUA_DEFAULT; style <= wxSTC_LUA_WORD8; ++style) {
			SetSyntaxStyle(style, font, "Normal", default_background);
			StyleSetUnderline(style, false);
		}
		if (OPT_GET("Subtitle/Highlight/Syntax")->GetBool()) {
			for (int style : {wxSTC_LUA_COMMENT, wxSTC_LUA_COMMENTLINE, wxSTC_LUA_COMMENTDOC})
				SetSyntaxStyle(style, font, "Comment", default_background);
			for (int style : {wxSTC_LUA_STRING, wxSTC_LUA_CHARACTER, wxSTC_LUA_LITERALSTRING})
				SetSyntaxStyle(style, font, "Karaoke Variable", default_background);
			SetSyntaxStyle(wxSTC_LUA_WORD, font, "Tags", default_background);
			SetSyntaxStyle(wxSTC_LUA_NUMBER, font, "Parameters", default_background);
			SetSyntaxStyle(wxSTC_LUA_OPERATOR, font, "Brackets", default_background);
			SetSyntaxStyle(wxSTC_LUA_STRINGEOL, font, "Error", default_background);
		}
	}

	SetCaretForeground(StyleGetForeground(ss::NORMAL));
	StyleSetBackground(wxSTC_STYLE_DEFAULT, default_background);

	// Misspelling indicator
	IndicatorSetStyle(0,wxSTC_INDIC_SQUIGGLE);
	IndicatorSetForeground(0,wxColour(255,0,0));

	// IME pending text indicator
	IndicatorSetStyle(1, wxSTC_INDIC_PLAIN);
	IndicatorSetUnder(1, true);

	// Matching brace indicators. Keep them separate from syntax and spelling
	// styles so the active pair does not overwrite the existing ASS colours.
	IndicatorSetStyle(BRACE_HIGHLIGHT_INDICATOR, wxSTC_INDIC_ROUNDBOX);
	IndicatorSetForeground(BRACE_HIGHLIGHT_INDICATOR, to_wx(OPT_GET("Colour/Subtitle/Syntax/Brackets")->GetColor()));
	IndicatorSetAlpha(BRACE_HIGHLIGHT_INDICATOR, 40);
	IndicatorSetOutlineAlpha(BRACE_HIGHLIGHT_INDICATOR, 180);
	IndicatorSetUnder(BRACE_HIGHLIGHT_INDICATOR, true);
	BraceHighlightIndicator(true, BRACE_HIGHLIGHT_INDICATOR);

	IndicatorSetStyle(BRACE_BAD_INDICATOR, wxSTC_INDIC_SQUIGGLE);
	IndicatorSetForeground(BRACE_BAD_INDICATOR, to_wx(OPT_GET("Colour/Subtitle/Syntax/Error")->GetColor()));
	IndicatorSetUnder(BRACE_BAD_INDICATOR, true);
	BraceBadLightIndicator(true, BRACE_BAD_INDICATOR);

	// Character marker indicators (independent of spelling/brace indicators).
	auto marker_colour = to_wx(OPT_GET("Colour/Subtitle/Character Marker")->GetColor());
	auto error_colour = to_wx(OPT_GET("Colour/Subtitle/Character Marker Error")->GetColor());

	ConfigureCharacterMarkerIndicator(this, CHAR_MARKER_SPACE_INDICATOR, wxSTC_INDIC_DOTBOX, marker_colour, 0, 200);
	ConfigureCharacterMarkerIndicator(this, CHAR_MARKER_SPACE_ALT_INDICATOR, wxSTC_INDIC_DOTBOX, marker_colour, 0, 200);
	// Bottom dash style distinguishes NBSP/other whitespace from U+0020/U+3000 boxes.
	ConfigureCharacterMarkerIndicator(this, CHAR_MARKER_OTHER_SPACE_INDICATOR, wxSTC_INDIC_DASH, marker_colour, 0, 200);
	ConfigureCharacterMarkerIndicator(this, CHAR_MARKER_OTHER_SPACE_ALT_INDICATOR, wxSTC_INDIC_DASH, marker_colour, 0, 200);
	ConfigureCharacterMarkerIndicator(this, CHAR_MARKER_ERROR_INDICATOR, wxSTC_INDIC_STRAIGHTBOX, error_colour, 40, 220);
	ConfigureCharacterMarkerIndicator(this, CHAR_MARKER_ERROR_ALT_INDICATOR, wxSTC_INDIC_STRAIGHTBOX, error_colour, 40, 220);

	// Colour swatch indicator. With INDICFLAG_VALUEFORE the box stroke and
	// fill come from each range's indicator value rather than a static
	// foreground, so every swatch carries its own colour and no static
	// IndicatorSetForeground is needed (Indicator::Draw overwrites it from the
	// range value). The parameter text keeps its syntax colour; the light tint
	// plus stroke is the swatch.
	IndicatorSetStyle(COLOR_SWATCH_BOX_INDICATOR, wxSTC_INDIC_STRAIGHTBOX);
	IndicatorSetFlags(COLOR_SWATCH_BOX_INDICATOR, wxSTC_INDICFLAG_VALUEFORE);
	// Under the text, like every other indicator here: the tint then sits
	// between the selection background and the glyphs instead of washing over
	// them (indicators draw after the selection background either way).
	IndicatorSetUnder(COLOR_SWATCH_BOX_INDICATOR, true);
	IndicatorSetAlpha(COLOR_SWATCH_BOX_INDICATOR, 40);
	IndicatorSetOutlineAlpha(COLOR_SWATCH_BOX_INDICATOR, 220);
}

void SubsStyledTextEditCtrl::UpdateStyle() {
	if (code_mode) {
		style_context_valid = true;
		last_template_line = IsTemplateLine(context);
		Colourise(0, -1);
		return;
	}
	auto const text_bytes = static_cast<int>(std::min(
		line_text.size(),
		static_cast<size_t>(std::numeric_limits<int>::max())));
	bool const template_line = IsTemplateLine(context);
	last_template_line = template_line;
	style_context_valid = true;
	{
		perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.style.tokenize", text_bytes);
		tokenized_line = agi::ass::TokenizeDialogueBody(line_text, template_line);
		agi::ass::SplitWords(line_text, tokenized_line);
	}

	cursor_pos = -1;
	{
		perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.style.calltip", text_bytes);
		UpdateCallTip();
	}

	{
		perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.style.begin", text_bytes);
#if wxCHECK_VERSION (3, 1, 0)
		StartStyling(0);
#else
		StartStyling(0,255);
#endif
	}

	if (!OPT_GET("Subtitle/Highlight/Syntax")->GetBool()) {
		{
			perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.style.plain", text_bytes);
			SetStyling(line_text.size(), 0);
		}
		// Character markers are independent of syntax highlighting.
		{
			perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.style.markers", text_bytes);
			UpdateCharacterMarkers();
		}
		// Colour swatches are independent of syntax highlighting too.
		{
			perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.style.swatches", text_bytes);
			UpdateColorSwatches();
		}
		return;
	}

	if (line_text.empty()) {
		{
			perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.style.markers", text_bytes);
			UpdateCharacterMarkers();
		}
		{
			perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.style.swatches", text_bytes);
			UpdateColorSwatches();
		}
		return;
	}

	{
		perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.style.syntax", text_bytes, template_line ? 1 : 0);
		SetIndicatorCurrent(0);
		size_t pos = 0;
		for (auto const& style_range : agi::ass::SyntaxHighlight(line_text, tokenized_line, spellchecker.get())) {
			if (style_range.type == agi::ass::SyntaxStyle::SPELLING) {
				SetStyling(style_range.length, agi::ass::SyntaxStyle::NORMAL);
				IndicatorFillRange(pos, style_range.length);
			}
			else {
				SetStyling(style_range.length, style_range.type);
				IndicatorClearRange(pos, style_range.length);
			}
			pos += style_range.length;
		}
	}

	{
		perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.style.markers", text_bytes);
		UpdateCharacterMarkers();
	}

	{
		perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.style.swatches", text_bytes);
		UpdateColorSwatches();
	}
}

void SubsStyledTextEditCtrl::UpdateBraceHighlight() {
	if (code_mode) {
		int const position = GetCurrentPos() - 1;
		int const character = position >= 0 ? GetCharAt(position) : 0;
		if (character > 0 && std::string_view("()[]{}").find(static_cast<char>(character)) != std::string_view::npos) {
			int const match = BraceMatch(position);
			if (match >= 0)
				BraceHighlight(position, match);
			else
				BraceBadLight(position);
		}
		else
			BraceHighlight(wxSTC_INVALID_POSITION, wxSTC_INVALID_POSITION);
		return;
	}
	int const caret = GetCurrentPos();
	int brace = wxSTC_INVALID_POSITION;
	if (caret > 0 && IsHighlightableBrace(GetCharAt(caret - 1)) && !IsEscapedOpenBrace(*this, caret - 1))
		brace = caret - 1;
	else if (caret < GetTextLength() && IsHighlightableBrace(GetCharAt(caret)) && !IsEscapedOpenBrace(*this, caret))
		brace = caret;

	if (brace == wxSTC_INVALID_POSITION) {
		BraceHighlight(wxSTC_INVALID_POSITION, wxSTC_INVALID_POSITION);
		return;
	}

	int const match = GetCharAt(brace) == '{' || GetCharAt(brace) == '}'
		? FindMatchingCurlyBrace(*this, brace)
		: BraceMatch(brace);
	if (match == wxSTC_INVALID_POSITION)
		BraceBadLight(brace);
	else
		BraceHighlight(brace, match);
}

void SubsStyledTextEditCtrl::UpdateCallTip() {
	if (code_mode)
		return;
	// Marker tooltips own the calltip while active; do not overwrite them.
	if (marker_calltip_active)
		return;

	if (!OPT_GET("App/Call Tips")->GetBool()) return;

	int pos = GetCurrentPos();
	if (pos == cursor_pos) return;
	cursor_pos = pos;

	agi::Calltip new_calltip = agi::GetCalltip(tokenized_line, line_text, pos);

	if (!new_calltip.text) {
		CallTipCancel();
		return;
	}

	if (!CallTipActive() || calltip_position != new_calltip.tag_position || calltip_text != new_calltip.text)
		CallTipShow(new_calltip.tag_position, wxString::FromUTF8Unchecked(new_calltip.text));

	calltip_position = new_calltip.tag_position;
	calltip_text = new_calltip.text;

	CallTipSetHighlight(new_calltip.highlight_start, new_calltip.highlight_end);
}

void SubsStyledTextEditCtrl::SetTextTo(std::string const& text) {
	wxCharBuffer const buffer = GetTextRaw();
	std::string const current_text(buffer.data(), buffer.length());
	bool const template_line = IsTemplateLine(context);
	if (text == current_text && text == line_text && style_context_valid && template_line == last_template_line)
		return;

	repeat_tag_name_bounds = {-1, 0};
	bool const event_handler_enabled = GetEvtHandlerEnabled();
	auto const text_bytes = static_cast<int>(std::min(
		text.size(),
		static_cast<size_t>(std::numeric_limits<int>::max())));

	{
		perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.freeze", text_bytes);
		SetEvtHandlerEnabled(false);
		Freeze();
	}

	size_t old_pos = 0;
	aegisub::subtitle_edit_ops::TextChangeRange text_change;
	{
		perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.caret_capture", text_bytes);
		auto insertion_point = GetInsertionPoint();
		// Styling is deferred, so line_text can still describe the previous edit.
		old_pos = agi::CharacterCount(current_text.begin(), current_text.begin() + insertion_point, 0);
		text_change = aegisub::subtitle_edit_ops::FindMinimalTextChange(current_text, text);
	}

	if (text_change.changed) {
		auto const removed_bytes = text_change.old_end - text_change.old_begin;
		auto const inserted_bytes = text_change.new_end - text_change.new_begin;
		perf_trace::VideoUiDurationScope trace(
			"grid_select.editbox.stc.replace_text_range",
			static_cast<int>(std::min(removed_bytes, static_cast<size_t>(std::numeric_limits<int>::max()))),
			static_cast<int>(std::min(inserted_bytes, static_cast<size_t>(std::numeric_limits<int>::max()))));

		int const mod_event_mask = GetModEventMask();
		bool const collecting_undo = GetUndoCollection();
		SetModEventMask(0);
		if (collecting_undo)
			SetUndoCollection(false);

		SetTargetRange(
			static_cast<int>(text_change.old_begin),
			static_cast<int>(text_change.old_end));
		auto const replacement = std::string_view(text).substr(
			text_change.new_begin,
			inserted_bytes);
		ReplaceTargetRaw(replacement.data(), static_cast<int>(replacement.size()));

		if (collecting_undo)
			SetUndoCollection(true);
		SetModEventMask(mod_event_mask);
	}
	line_text = text;

	{
		perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.selection_restore", text_bytes);
		auto pos = agi::IndexOfCharacter(text, old_pos);
		// Range replacement moves the native selection even when the controller's
		// cached selection already equals the position we want to restore.
		SetSelection(pos, pos);
		if (context)
			context->GetCore().textSelectionController->SetSelection(pos, pos);
	}

	{
		perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.sync", text_bytes);
		SetEvtHandlerEnabled(event_handler_enabled);
		// Events were disabled during the range replacement, so force a style refresh.
	}
	{
		perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.style", text_bytes);
		UpdateStyle();
	}
	{
		perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.brace", text_bytes);
		UpdateBraceHighlight();
	}
	{
		perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.thaw", text_bytes);
		Thaw();
	}

#ifdef __WXMSW__
	if (perf_trace::IsCategoryEnabled(perf_trace::Category::Log)) {
		pending_paint_timing = {
			true,
			++next_paint_timing_id,
			text_bytes,
			PaintTimingNowNs(),
		};
	}
	else {
		pending_paint_timing.pending = false;
	}
#endif
}

bool SubsStyledTextEditCtrl::InsertCodeText(std::string const& text) {
	wxCharBuffer const buffer = GetTextRaw();
	std::string const original(buffer.data(), buffer.length());
	std::string candidate = original.substr(0, GetSelectionStart()) + text + original.substr(GetSelectionEnd());
	int caret = GetSelectionStart() + static_cast<int>(text.size());
	if (candidate.find_first_of("\r\n") != std::string::npos) {
		auto serialized = Automation4::SerializeLuaSource(candidate);
		if (!serialized.Succeeded()) {
			wxMessageBox(to_wx(serialized.diagnostic->message) + _("\n\nOpen the Lua Workspace (Shift+Enter) to edit multiline code."), _("Cannot paste Lua code"), wxOK | wxICON_ERROR, this);
			return false;
		}
		candidate = std::move(serialized.source);
		caret = static_cast<int>(candidate.size());
	}
	if (candidate != original) {
		BeginUndoAction();
		SetTargetRange(0, GetTextLength());
		ReplaceTargetRaw(candidate.data(), static_cast<int>(candidate.size()));
		EndUndoAction();
	}
	SetSelection(caret, caret);
	line_text = candidate;
	UpdateStyle();
	return true;
}

void SubsStyledTextEditCtrl::Paste() {
	wxTextDataObject text;
	auto *clipboard = wxClipboard::Get();
	wxLogNull disable_logging;
	bool read = false;
	for (int attempt = 0; attempt < 5 && !read; ++attempt) {
		if (clipboard->Open()) {
			read = clipboard->GetData(text);
			clipboard->Close();
		}
		if (!read && attempt < 4)
			wxMilliSleep(20);
	}
	if (!read) {
		if (code_mode)
			wxMessageBox(_("Could not read text from the clipboard. The code line was not changed."), _("Cannot paste Lua code"), wxOK | wxICON_ERROR, this);
		return;
	}
	std::string data = from_wx(text.GetText());
	if (code_mode) {
		InsertCodeText(data);
		return;
	}

	agi::util::strings::replace_all_inplace(data, "\r\n", "\\N");
	agi::util::strings::replace_all_inplace(data, "\n", "\\N");
	agi::util::strings::replace_all_inplace(data, "\r", "\\N");

	wxCharBuffer old = GetTextRaw();
	data.insert(0, old.data(), GetSelectionStart());
	int sel_start = data.size();
	data.append(old.data() + GetSelectionEnd());

	SetTextRaw(data.c_str());

	SetSelectionStart(sel_start);
	SetSelectionEnd(sel_start);

	line_text = GetTextRaw().data();
	UpdateStyle();
}

void SubsStyledTextEditCtrl::OnContextMenu(wxContextMenuEvent &event) {
	repeat_tag_name_bounds = {-1, 0};
	wxPoint pos = event.GetPosition();
	int activePos;
	if (pos == wxDefaultPosition)
		activePos = GetCurrentPos();
	else
		activePos = PositionFromPoint(ScreenToClient(pos));

	if (!code_mode) {
		currentWordPos = GetBoundsOfWordAtPosition(activePos);
		currentWord = line_text.substr(currentWordPos.first, currentWordPos.second);
	}

	wxMenu menu;
	if (!code_mode && spellchecker) {
		AddSpellCheckerEntries(menu);

		// Append language list
		menu.Append(-1, _("Spell checker language"), GetLanguagesMenu(
			EDIT_MENU_DIC_LANGS,
			to_wx(OPT_GET("Tool/Spell Checker/Language")->GetString()),
			to_wx(spellchecker->GetLanguageList())));
		menu.AppendSeparator();
	}

	if (!code_mode)
		AddThesaurusEntries(menu);

	// Standard actions
	menu.Append(EDIT_MENU_CUT,_("Cu&t"))->Enable(GetSelectionStart()-GetSelectionEnd() != 0);
	menu.Append(EDIT_MENU_COPY,_("&Copy"))->Enable(GetSelectionStart()-GetSelectionEnd() != 0);
	menu.Append(EDIT_MENU_PASTE,_("&Paste"))->Enable(CanPaste());
	menu.AppendSeparator();
	menu.Append(EDIT_MENU_SELECT_ALL,_("Select &All"));

	// Split
	if (context && !code_mode) {
		menu.AppendSeparator();
		menu.Append(EDIT_MENU_SPLIT_PRESERVE, _("Split at cursor (preserve times)"));
		menu.Append(EDIT_MENU_SPLIT_ESTIMATE, _("Split at cursor (estimate times)"));
		cmd::Command *split_video = cmd::get("edit/line/split/video");
		menu.Append(EDIT_MENU_SPLIT_VIDEO, split_video->StrMenu(context))->Enable(split_video->Validate(context));
	}

	PopupMenu(&menu);
}

void SubsStyledTextEditCtrl::OnDoubleClick(wxStyledTextEvent &evt) {
	if (code_mode) {
		evt.Skip();
		return;
	}
	int pos = evt.GetPosition();
	auto const previous_tag_name_bounds = repeat_tag_name_bounds;
	repeat_tag_name_bounds = {-1, 0};
	if (pos == -1 && !tokenized_line.empty()) {
		auto tok = tokenized_line.back();
		SetSelection(line_text.size() - tok.length, line_text.size());
	}
	else {
		// ASS text escapes are one editing unit even though Scintilla's word
		// selection treats the backslash as punctuation.
		auto escape_bounds = aegisub::subtitle_edit_ops::GetBoundsOfEscapeAtPosition(tokenized_line, pos);
		if (escape_bounds.second != 0) {
			SetSelection(escape_bounds.first, escape_bounds.first + escape_bounds.second);
			return;
		}
		// Position tags use two-stage selection: name first, then the whole tag
		// when the same name is double-clicked again.
		auto const tag_plan = aegisub::subtitle_edit_ops::PlanTagDoubleClick(
			line_text, tokenized_line, pos, previous_tag_name_bounds);
		if (tag_plan.selection.second != 0) {
			SetSelection(tag_plan.selection.first, tag_plan.selection.first + tag_plan.selection.second);
			repeat_tag_name_bounds = tag_plan.repeat_tag_name_bounds;
			return;
		}
		// Otherwise fall back to a plain WORD token (used by the spell checker).
		auto bounds = GetBoundsOfWordAtPosition(pos);
		if (bounds.second != 0)
			SetSelection(bounds.first, bounds.first + bounds.second);
		else
			evt.Skip();
	}
}

void SubsStyledTextEditCtrl::AddSpellCheckerEntries(wxMenu &menu) {
	if (currentWord.empty()) return;

	if (spellchecker->CanRemoveWord(currentWord))
		menu.Append(EDIT_MENU_REMOVE_FROM_DICT, fmt_tl("Remove \"%s\" from dictionary", currentWord));

	sugs = spellchecker->GetSuggestions(currentWord);
	if (spellchecker->CheckWord(currentWord)) {
		if (sugs.empty())
			menu.Append(EDIT_MENU_SUGGESTION,_("No spell checker suggestions"))->Enable(false);
		else {
			auto subMenu = new wxMenu;
			for (size_t i = 0; i < sugs.size(); ++i)
				subMenu->Append(EDIT_MENU_SUGGESTIONS+i, to_wx(sugs[i]));

			menu.Append(-1, fmt_tl("Spell checker suggestions for \"%s\"", currentWord), subMenu);
		}
	}
	else {
		if (sugs.empty())
			menu.Append(EDIT_MENU_SUGGESTION,_("No correction suggestions"))->Enable(false);

		for (size_t i = 0; i < sugs.size(); ++i)
			menu.Append(EDIT_MENU_SUGGESTIONS+i, to_wx(sugs[i]));

		// Append "add word"
		menu.Append(EDIT_MENU_ADD_TO_DICT, fmt_tl("Add \"%s\" to dictionary", currentWord))->Enable(spellchecker->CanAddWord(currentWord));
	}
}

void SubsStyledTextEditCtrl::AddThesaurusEntries(wxMenu &menu) {
	if (currentWord.empty()) return;

	auto results = thesaurus->Lookup(currentWord);

	thesSugs.clear();

	if (results.size()) {
		auto thesMenu = new wxMenu;

		int curThesEntry = 0;
		for (auto const& result : results) {
			// Single word, insert directly
			if (result.second.empty()) {
				thesMenu->Append(EDIT_MENU_THESAURUS_SUGS+curThesEntry, to_wx(result.first));
				thesSugs.push_back(result.first);
				++curThesEntry;
			}
			// Multiple, create submenu
			else {
				auto subMenu = new wxMenu;
				for (auto const& sug : result.second) {
					subMenu->Append(EDIT_MENU_THESAURUS_SUGS+curThesEntry, to_wx(sug));
					thesSugs.push_back(sug);
					++curThesEntry;
				}

				thesMenu->Append(-1, to_wx(result.first), subMenu);
			}
		}

		menu.Append(-1, fmt_tl("Thesaurus suggestions for \"%s\"", currentWord), thesMenu);
	}
	else
		menu.Append(EDIT_MENU_THESAURUS,_("No thesaurus suggestions"))->Enable(false);

	// Append language list
	menu.Append(-1,_("Thesaurus language"), GetLanguagesMenu(
		EDIT_MENU_THES_LANGS,
		to_wx(OPT_GET("Tool/Thesaurus/Language")->GetString()),
		to_wx(thesaurus->GetLanguageList())));
	menu.AppendSeparator();
}

wxMenu *SubsStyledTextEditCtrl::GetLanguagesMenu(int base_id, wxString const& curLang, wxArrayString const& langs) {
	auto languageMenu = new wxMenu;
	languageMenu->AppendRadioItem(base_id, _("Disable"))->Check(curLang.empty());

	for (size_t i = 0; i < langs.size(); ++i)
		languageMenu->AppendRadioItem(base_id + i + 1, LocalizedLanguageName(langs[i]))->Check(langs[i] == curLang);

	return languageMenu;
}

void SubsStyledTextEditCtrl::OnUseSuggestion(wxCommandEvent &event) {
	std::string suggestion;
	int sugIdx = event.GetId() - EDIT_MENU_THESAURUS_SUGS;
	if (sugIdx >= 0)
		suggestion = thesSugs[sugIdx];
	else
		suggestion = sugs[event.GetId() - EDIT_MENU_SUGGESTIONS];

	size_t pos;
	while ((pos = suggestion.rfind('(')) != std::string::npos) {
		// If there's only one suggestion for a word it'll be in the form "(noun) word",
		// so we need to trim the "(noun) " part
		if (pos == 0) {
			pos = suggestion.find(')');
			if (pos != std::string::npos) {
				if (pos + 1< suggestion.size() && suggestion[pos + 1] == ' ') ++pos;
				suggestion.erase(0, pos + 1);
			}
			break;
		}

		// Some replacements have notes about their usage after the word in the
		// form "word (generic term)" that we need to remove (plus the leading space)
		suggestion.resize(pos - 1);
	}

	// line_text needs to get cleared before SetTextRaw to ensure it gets reparsed
	std::string new_text;
	std::swap(line_text, new_text);
	SetTextRaw(new_text.replace(currentWordPos.first, currentWordPos.second, suggestion).c_str());

	SetSelection(currentWordPos.first, currentWordPos.first + suggestion.size());
	SetFocus();
}

void SubsStyledTextEditCtrl::OnSetDicLanguage(wxCommandEvent &event) {
	std::vector<std::string> langs = spellchecker->GetLanguageList();

	int index = event.GetId() - EDIT_MENU_DIC_LANGS - 1;
	std::string lang;
	if (index >= 0)
		lang = langs[index];

	OPT_SET("Tool/Spell Checker/Language")->SetString(lang);

	UpdateStyle();
}

void SubsStyledTextEditCtrl::OnSetThesLanguage(wxCommandEvent &event) {
	if (!thesaurus) return;

	std::vector<std::string> langs = thesaurus->GetLanguageList();

	int index = event.GetId() - EDIT_MENU_THES_LANGS - 1;
	std::string lang;
	if (index >= 0) lang = langs[index];
	OPT_SET("Tool/Thesaurus/Language")->SetString(lang);

	UpdateStyle();
}

std::pair<int, int> SubsStyledTextEditCtrl::GetBoundsOfWordAtPosition(int pos) {
	int len = 0;
	for (auto const& tok : tokenized_line) {
		if (len + (int)tok.length > pos) {
			if (tok.type == agi::ass::DialogueTokenType::WORD)
				return {len, tok.length};
			return {0, 0};
		}
		len += tok.length;
	}

	return {0, 0};
}

aegisub::CharacterMarkerShowConfig SubsStyledTextEditCtrl::ReadCharacterMarkerShowConfig() const {
	aegisub::CharacterMarkerShowConfig cfg;
	cfg.space = OPT_GET("Subtitle/Edit Box/Character Markers/Show/Space")->GetBool();
	cfg.ideographic_space = OPT_GET("Subtitle/Edit Box/Character Markers/Show/Ideographic Space")->GetBool();
	cfg.unicode_whitespace = OPT_GET("Subtitle/Edit Box/Character Markers/Show/Unicode Whitespace")->GetBool();
	cfg.line_endings = OPT_GET("Subtitle/Edit Box/Character Markers/Show/Line Endings")->GetBool();
	cfg.control_characters = OPT_GET("Subtitle/Edit Box/Character Markers/Show/Control Characters")->GetBool();
	cfg.invisible_characters = OPT_GET("Subtitle/Edit Box/Character Markers/Show/Invisible Characters")->GetBool();
	return cfg;
}

aegisub::CharacterMarkerErrorConfig SubsStyledTextEditCtrl::ReadCharacterMarkerErrorConfig() const {
	aegisub::CharacterMarkerErrorConfig cfg;
	cfg.enabled = OPT_GET("Subtitle/Edit Box/Character Markers/Error/Enabled")->GetBool();
	cfg.space = OPT_GET("Subtitle/Edit Box/Character Markers/Error/Space")->GetBool();
	cfg.ideographic_space = OPT_GET("Subtitle/Edit Box/Character Markers/Error/Ideographic Space")->GetBool();
	cfg.no_break_space = OPT_GET("Subtitle/Edit Box/Character Markers/Error/No-Break Space")->GetBool();
	cfg.other_unicode_whitespace = OPT_GET("Subtitle/Edit Box/Character Markers/Error/Other Unicode Whitespace")->GetBool();
	cfg.line_endings = OPT_GET("Subtitle/Edit Box/Character Markers/Error/Line Endings")->GetBool();
	cfg.tab = OPT_GET("Subtitle/Edit Box/Character Markers/Error/Tab")->GetBool();
	cfg.other_control_characters = OPT_GET("Subtitle/Edit Box/Character Markers/Error/Other Control Characters")->GetBool();
	cfg.bidi_controls = OPT_GET("Subtitle/Edit Box/Character Markers/Error/Bidi Controls")->GetBool();
	cfg.join_controls = OPT_GET("Subtitle/Edit Box/Character Markers/Error/Join Controls")->GetBool();
	cfg.other_invisible_characters = OPT_GET("Subtitle/Edit Box/Character Markers/Error/Other Invisible Characters")->GetBool();
	cfg.join_control_context_policy = static_cast<aegisub::JoinControlContextPolicy>(
		OPT_GET("Subtitle/Edit Box/Character Markers/Error/Join Control Context Policy")->GetInt());
	cfg.variation_selector_context_policy = static_cast<aegisub::VariationSelectorContextPolicy>(
		OPT_GET("Subtitle/Edit Box/Character Markers/Error/Variation Selector Context Policy")->GetInt());
	return cfg;
}

void SubsStyledTextEditCtrl::SubscribeCharacterMarkerOptions() {
	auto queue = [this](agi::OptionValue const&) { QueueCharacterMarkerRefresh(); };

	OPT_SUB("Subtitle/Edit Box/Character Markers/Show/Space", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Show/Ideographic Space", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Show/Unicode Whitespace", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Show/Line Endings", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Show/Control Characters", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Show/Invisible Characters", queue);

	OPT_SUB("Subtitle/Edit Box/Character Markers/Error/Enabled", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Error/Space", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Error/Ideographic Space", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Error/No-Break Space", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Error/Other Unicode Whitespace", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Error/Line Endings", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Error/Tab", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Error/Other Control Characters", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Error/Bidi Controls", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Error/Join Controls", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Error/Other Invisible Characters", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Error/Join Control Context Policy", queue);
	OPT_SUB("Subtitle/Edit Box/Character Markers/Error/Variation Selector Context Policy", queue);

	OPT_SUB("Colour/Subtitle/Character Marker", queue);
	OPT_SUB("Colour/Subtitle/Character Marker Error", queue);
}

void SubsStyledTextEditCtrl::QueueCharacterMarkerRefresh() {
	if (character_marker_refresh_queued)
		return;
	character_marker_refresh_queued = true;
	CallAfter([this] {
		character_marker_refresh_queued = false;
		// Re-apply indicator colours/styles and representations, then rescan.
		SetStyles();
		ApplyCharacterMarkerSettings();
		UpdateCharacterMarkers();
	});
}

void SubsStyledTextEditCtrl::ApplyCharacterMarkerSettings() {
	// Representations depend on the current line's context. Clear owned mappings
	// here; UpdateCharacterMarkers() rebuilds them from scanned spans only.
	for (auto const& encoded : installed_character_representations)
		ClearRepresentation(wxString::FromUTF8(encoded));
	installed_character_representations.clear();
}

void SubsStyledTextEditCtrl::ClearCharacterMarkerIndicators() {
	int const length = GetTextLength();
	if (length <= 0)
		return;

	for (int indicator : {
		CHAR_MARKER_SPACE_INDICATOR,
		CHAR_MARKER_SPACE_ALT_INDICATOR,
		CHAR_MARKER_OTHER_SPACE_INDICATOR,
		CHAR_MARKER_OTHER_SPACE_ALT_INDICATOR,
		CHAR_MARKER_ERROR_INDICATOR,
		CHAR_MARKER_ERROR_ALT_INDICATOR,
	}) {
		SetIndicatorCurrent(indicator);
		IndicatorClearRange(0, length);
	}
}

void SubsStyledTextEditCtrl::UpdateCharacterMarkers() {
	if (code_mode)
		return;
	auto const show = ReadCharacterMarkerShowConfig();
	auto const error = ReadCharacterMarkerErrorConfig();
	if (!aegisub::CharacterMarkersEnabled(show, error)) {
		if (!character_marker_spans.empty() || !installed_character_representations.empty()) {
			for (auto const& encoded : installed_character_representations)
				ClearRepresentation(wxString::FromUTF8(encoded));
			installed_character_representations.clear();
			ClearCharacterMarkerIndicators();
		}
		character_marker_spans.clear();
		if (marker_calltip_active) {
			CallTipCancel();
			marker_calltip_active = false;
			calltip_position = static_cast<size_t>(-1);
			calltip_text.clear();
			cursor_pos = -1;
		}
		return;
	}

	{
		auto const text_bytes = static_cast<int>(std::min(
			line_text.size(),
			static_cast<size_t>(std::numeric_limits<int>::max())));
		perf_trace::VideoUiDurationScope trace("grid_select.editbox.stc.style.markers.scan", text_bytes);
		character_marker_spans = aegisub::ScanCharacterMarkers(line_text);
	}
	auto const render_plan = aegisub::BuildCharacterMarkerRenderPlan(character_marker_spans, show, error);

	// Rebuild representations exactly from current-line spans with real context.
	// Error-only zero-width characters get a compact blob so the error indicator
	// and dwell hit target have non-zero width.
	std::set<std::string> needed;
	for (auto const& representation : render_plan.representations) {
		auto encoded = Utf8EncodeCodepoint(representation.codepoint);
		needed.insert(encoded);
		if (installed_character_representations.count(encoded))
			continue;
		SetRepresentation(wxString::FromUTF8(encoded), to_wx(representation.label));
	}

	for (auto const& encoded : installed_character_representations) {
		if (!needed.count(encoded))
			ClearRepresentation(wxString::FromUTF8(encoded));
	}
	installed_character_representations = std::move(needed);

	ClearCharacterMarkerIndicators();

	for (auto const& span : render_plan.indicators) {
		int indicator;
		switch (span.style) {
		case aegisub::CharacterMarkerIndicatorStyle::Error:
			indicator = CHAR_MARKER_ERROR_INDICATOR;
			break;
		case aegisub::CharacterMarkerIndicatorStyle::ErrorAlt:
			indicator = CHAR_MARKER_ERROR_ALT_INDICATOR;
			break;
		case aegisub::CharacterMarkerIndicatorStyle::OtherWhitespace:
			indicator = CHAR_MARKER_OTHER_SPACE_INDICATOR;
			break;
		case aegisub::CharacterMarkerIndicatorStyle::OtherWhitespaceAlt:
			indicator = CHAR_MARKER_OTHER_SPACE_ALT_INDICATOR;
			break;
		case aegisub::CharacterMarkerIndicatorStyle::SpaceAlt:
			indicator = CHAR_MARKER_SPACE_ALT_INDICATOR;
			break;
		case aegisub::CharacterMarkerIndicatorStyle::Space:
		default:
			indicator = CHAR_MARKER_SPACE_INDICATOR;
			break;
		}

		SetIndicatorCurrent(indicator);
		IndicatorFillRange(static_cast<int>(span.byte_start), static_cast<int>(span.byte_length));
	}
}

wxString SubsStyledTextEditCtrl::BuildCharacterMarkerTooltip(aegisub::CharacterMarkerSpan const& span) const {
	auto const title = KindTitle(span.kind, span.codepoint);
	auto const icu_name = aegisub::CharacterMarkerIcuName(span.codepoint);
	auto const cat_code = aegisub::CharacterMarkerGeneralCategoryCode(span.codepoint);
	auto const cat_desc = CategoryDescription(cat_code);

	wxString codepoint_line;
	if (!icu_name.empty())
		codepoint_line = wxString::Format(wxS("U+%04X %s"), static_cast<unsigned>(span.codepoint), to_wx(icu_name));
	else
		codepoint_line = wxString::Format(wxS("U+%04X"), static_cast<unsigned>(span.codepoint));

	auto const error = ReadCharacterMarkerErrorConfig();
	bool const is_error = aegisub::IsCharacterMarkerError(span, error);
	wxString status = is_error
		? _("Status: marked as error by current settings")
		: _("Status: shown as character marker");

	return wxString::Format(wxS("%s\n%s\n%s: %s\n%s"),
		title,
		codepoint_line,
		_("Category"),
		cat_desc,
		status);
}

void SubsStyledTextEditCtrl::OnCharacterMarkerDwellStart(wxStyledTextEvent& event) {
	if (code_mode) {
		event.Skip();
		return;
	}
	int const pos = event.GetPosition();
	if (pos < 0) {
		event.Skip();
		return;
	}

	auto const* span = aegisub::FindCharacterMarkerAtByte(character_marker_spans, static_cast<std::size_t>(pos));
	if (!span) {
		event.Skip();
		return;
	}

	auto const show = ReadCharacterMarkerShowConfig();
	auto const error = ReadCharacterMarkerErrorConfig();
	if (!aegisub::CharacterMarkerNeedsVisual(*span, show, error)) {
		event.Skip();
		return;
	}

	marker_calltip_active = true;
	CallTipShow(static_cast<int>(span->byte_start), BuildCharacterMarkerTooltip(*span));
	event.Skip(false);
}

void SubsStyledTextEditCtrl::OnCharacterMarkerDwellEnd(wxStyledTextEvent& event) {
	if (marker_calltip_active) {
		CallTipCancel();
		marker_calltip_active = false;
		// Invalidate syntax calltip cache so the next idle pass can restore it.
		calltip_position = static_cast<size_t>(-1);
		calltip_text.clear();
		cursor_pos = -1;
	}
	event.Skip();
}

void SubsStyledTextEditCtrl::ClearColorSwatchIndicators() {
	int const length = GetTextLength();
	if (length <= 0)
		return;

	SetIndicatorCurrent(COLOR_SWATCH_BOX_INDICATOR);
	IndicatorClearRange(0, length);
}

void SubsStyledTextEditCtrl::UpdateColorSwatches() {
	ClearColorSwatchIndicators();
	color_swatch_spans.clear();

	if (code_mode || !OPT_GET("Subtitle/Highlight/Color Swatches")->GetBool())
		return;

	color_swatch_spans = aegisub::subtitle_edit_ops::FindColorSpans(line_text, tokenized_line);

	for (auto const& span : color_swatch_spans) {
		// Alpha swatches wait for an alpha write path; \t-nested tags have no
		// set_tag target either, and a swatch that cannot be clicked would
		// misrepresent the tag as editable here.
		if (span.is_alpha || span.nested)
			continue;

		// The swatch covers only the hex digits, so the &H prefix and trailing
		// & keep their syntax colour and stay free for caret placement.
		auto const value_bounds = aegisub::subtitle_edit_ops::GetColorValueBounds(line_text, span);
		if (value_bounds.second <= 0)
			continue;

		SetIndicatorCurrent(COLOR_SWATCH_BOX_INDICATOR);
		SetIndicatorValue(wxSTC_INDICVALUEBIT | ScintillaColourOf(span.rgb()));
		IndicatorFillRange(value_bounds.first, value_bounds.second);
	}
}

void SubsStyledTextEditCtrl::OnIndicatorRelease(wxStyledTextEvent& event) {
	event.Skip();

	int const pos = event.GetPosition();
	// The notification arrives for every indicator at the released position
	// (spelling, IME, brace, character markers); only the swatch box acts.
	// Requiring the release point to still be on the swatch also discards
	// presses that dragged a selection off the value.
	if (pos < 0 || !(IndicatorAllOnFor(pos) & (1 << COLOR_SWATCH_BOX_INDICATOR)))
		return;
	if (!context)
		return;
	if (GetSelectionStart() != GetSelectionEnd())
		return;

	aegisub::subtitle_edit_ops::ColorSpan const *clicked = nullptr;
	for (auto const& span : color_swatch_spans) {
		if (span.is_alpha || span.nested)
			continue;
		auto const value_bounds = aegisub::subtitle_edit_ops::GetColorValueBounds(line_text, span);
		if (pos >= value_bounds.first && pos < value_bounds.first + value_bounds.second) {
			clicked = &span;
			break;
		}
	}
	// Mirror UpdateColorSwatches' paint filter: unclicked spans can't fire.
	if (!clicked)
		return;

	char const *command = nullptr;
	switch (clicked->slot) {
		case 1: command = "edit/color/primary"; break;
		case 2: command = "edit/color/secondary"; break;
		case 3: command = "edit/color/outline"; break;
		case 4: command = "edit/color/shadow"; break;
		default: return;
	}

	int const arg_start = clicked->byte_start;
	int const arg_length = clicked->byte_length;
	int const arg_slot = clicked->slot;
	// Select the whole clicked parameter, not just a caret inside it. The colour
	// commands resolve the override block from the selection start, which is the
	// parameter start either way, and the selected range is the value the picker
	// is about to rewrite — the same unit a double click on the value selects.
	// It also gives the picker's cancel path (which restores the selection as it
	// was on entry) the range back instead of a bare caret.
	// CallAfter keeps the picker out of Scintilla's mouse-button handling.
	CallAfter([this, command, arg_start, arg_length, arg_slot] {
		if (!context)
			return;
		// TextSelectionController::SetSelection skips the control entirely when
		// its own cached range already equals the requested one, and its cache
		// only resyncs from the control on STC_UPDATEUI, which Scintilla defers.
		// The picker's cancel path restores the range this handler set on entry,
		// which primes that cache to exactly what the re-selection below asks
		// for, so going through the controller alone would be swallowed and the
		// control would keep whatever selection it really had. Write the control
		// first, then let the controller record it and announce the change.
		auto select = [this](int start, int end) {
			SetSelection(start, end);
			// SetSelection already moves the insertion point; calling
			// SetInsertionPoint too would just announce the change twice.
			context->GetCore().textSelectionController->SetSelection(start, end);
		};
		select(arg_start, arg_start + arg_length);
		cmd::call(command, context);

		// Every COMMIT_DIAG_TEXT goes through SetTextTo, which collapses the
		// selection to the insertion point, so the range above is gone by the
		// time the picker returns and the rewritten value would sit unselected.
		// Cancelling instead restores the entry range, but through the same
		// controller whose cache the restore just primed, so the control can be
		// left behind either way. Re-select from the spans the commit rebuilt,
		// matching the span of the same slot that still spans where we clicked:
		// an in-place value replacement keeps the parameter roughly put, but it
		// can shift a byte or two — re-serializing the block drops a blank the
		// old text had between the tag name and its value — so this cannot
		// require an exact start. Leave the caret alone if nothing matches
		// rather than guess.
		for (auto const& span : color_swatch_spans) {
			if (span.is_alpha || span.nested || span.slot != arg_slot)
				continue;
			if (arg_start < span.byte_start || arg_start > span.byte_start + span.byte_length)
				continue;
			select(span.byte_start, span.byte_start + span.byte_length);
			return;
		}
	});
}
