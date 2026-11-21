#pragma once

#include "font_family_selection_model.h"

#include <libaegisub/scope_exit.h>

#include <optional>
#include <utility>
#include <vector>

#include <wx/arrstr.h>
#include <wx/combobox.h>
#include <wx/timer.h>
#include <wx/wupdlock.h>

#ifdef __WXMSW__
#include <windows.h>
#include <commctrl.h>
#endif

namespace font_name_combo_box_detail {

inline wxString Fold(wxString value) {
	return value.Lower();
}

struct ContainsMatch {
	std::size_t index = 0;
	FontFamilyId family_id = 0;
};

/// Prefer the row currently highlighted by the open list. The tracked match
/// can lag behind native prefix matching or explicit keyboard navigation.
inline std::optional<std::size_t> ResolveCommitSelection(
	std::size_t choice_count,
	std::optional<std::size_t> list_caret,
	std::optional<std::size_t> tracked_match) noexcept
{
	if (list_caret && *list_caret < choice_count)
		return list_caret;
	if (tracked_match && *tracked_match < choice_count)
		return tracked_match;
	return std::nullopt;
}

/// First choice whose label starts with the query (case-insensitive).
inline std::optional<ContainsMatch> FindPrefixMatch(
	std::vector<std::pair<wxString, FontFamilyId>> const& choices,
	wxString const& query) noexcept
{
	if (query.empty())
		return std::nullopt;

	auto const folded_query = Fold(query);
	for (std::size_t index = 0; index < choices.size(); ++index) {
		auto const& [label, family_id] = choices[index];
		if (Fold(label).StartsWith(folded_query))
			return ContainsMatch{index, family_id};
	}
	return std::nullopt;
}

/// First choice whose label contains the query (case-insensitive).
/// Searches the supplied candidates in display order.
inline std::optional<ContainsMatch> FindContainsMatch(
	std::vector<std::pair<wxString, FontFamilyId>> const& choices,
	wxString const& query) noexcept
{
	if (query.empty())
		return std::nullopt;

	auto const folded_query = Fold(query);
	for (std::size_t index = 0; index < choices.size(); ++index) {
		auto const& [label, family_id] = choices[index];
		// family_id may be 0 for enumerator fallback choices; match by label.
		if (Fold(label).Find(folded_query) != wxNOT_FOUND)
			return ContainsMatch{index, family_id};
	}
	return std::nullopt;
}

/// Typed length after stripping a native auto-complete suffix, if any.
/// A fully selected value (selection from 0) is a committed choice, not an
/// empty typed prefix.
inline std::size_t TypedQueryLength(
	std::size_t value_length,
	long selection_start,
	long selection_end,
	long insertion_point) noexcept
{
	if (selection_start > 0
		&& selection_start < selection_end
		&& selection_end == static_cast<long>(value_length)
		&& insertion_point == selection_start)
		return static_cast<std::size_t>(selection_start);
	return value_length;
}

} // namespace font_name_combo_box_detail

/// Editable font selector with optional case-insensitive contains matching.
///
/// When contains matching is off, the control is a plain editable combo: native
/// prefix behaviour is left untouched.
///
/// When enabled, the intentional difference is the match predicate (contains
/// instead of prefix). Opening the list or typing while it is open jumps the
/// drop-down highlight to the first contains match in the current choices.
///
/// MSW note: moving the list caret can rewrite the edit field, and a later
/// native paint/message can clear our highlight. We therefore (1) restore the
/// edit only when it actually changed (avoid SetWindowText-driven native
/// prefix walk), (2) jump the list caret once per open/key with redraw frozen,
/// and (3) re-apply that same caret once after a short settle timer — not a
/// second full match pass (which looked like scrolling item-by-item when
/// typing first and opening the list second).
class FontNameComboBox final : public wxComboBox {
	std::vector<std::pair<wxString, FontFamilyId>> choices;
	std::optional<FontFamilyId> selected_family_id;
	/// Last prefix/contains match row while searching (not a committed selection).
	std::optional<std::size_t> highlight_index;
	/// User-typed query while searching; edit should show this until commit.
	wxString typed_query;
	/// Last query observed by the text handler; selection/copy notifications
	/// which leave it unchanged must not reopen a dismissed list.
	wxString last_text_query;
	std::size_t match_revision = 0;
	bool contains_matching = false;
	bool auto_expand_on_input = false;
	bool applying = false;
	bool committing_list_selection = false;
	bool suppress_keyboard_text_enter = false;
	bool drop_down_open = false;
	wxTimer settle_timer;
#ifdef __WXMSW__
	HWND guarded_list = nullptr;
#endif

	static wxArrayString ToWxChoices(std::vector<FontFamilyChoice> const& choices) {
		wxArrayString result;
		result.reserve(choices.size());
		for (auto const& choice : choices)
			result.Add(wxString::FromUTF8(choice.label.c_str()));
		return result;
	}

	bool IsDropDownOpen() const {
#ifdef __WXMSW__
		auto const hwnd = GetHWND();
		if (hwnd)
			return ::SendMessageW(reinterpret_cast<HWND>(hwnd), CB_GETDROPPEDSTATE, 0, 0) != 0;
#endif
		return drop_down_open;
	}

#ifdef __WXMSW__
	[[nodiscard]] HWND NativeList() const noexcept {
		COMBOBOXINFO info{};
		info.cbSize = sizeof(info);
		return ::GetComboBoxInfo(GetHWND(), &info) ? info.hwndList : nullptr;
	}
#endif

	std::optional<std::size_t> CurrentListCaret() const noexcept {
#ifdef __WXMSW__
		auto const list = NativeList();
		if (!list)
			return std::nullopt;
		auto const selection = ::SendMessageW(list, LB_GETCURSEL, 0, 0);
#else
		auto const selection = wxComboBox::GetSelection();
#endif
		if (selection < 0 || static_cast<std::size_t>(selection) >= choices.size())
			return std::nullopt;
		return static_cast<std::size_t>(selection);
	}

	std::optional<FontFamilyId> ExactListMatch(wxString const& value) const {
		std::optional<FontFamilyId> matched;
		for (auto const& [label, id] : choices) {
			if (label != value)
				continue;
			if (matched && *matched != id)
				return std::nullopt;
			matched = id;
		}
		return matched && *matched != 0 ? matched : std::nullopt;
	}

	[[nodiscard]] std::optional<std::size_t> ExactListIndex(wxString const& value) const {
		std::optional<std::size_t> first;
		for (std::size_t index = 0; index < choices.size(); ++index) {
			auto const& [label, id] = choices[index];
			if (label != value)
				continue;
			if (selected_family_id == id)
				return index;
			if (!first)
				first = index;
		}
		return first;
	}

	[[nodiscard]] wxString ReadEditQuery() const {
		auto const value = GetValue();
		long from = 0, to = 0;
		GetSelection(&from, &to);
		return value.Left(font_name_combo_box_detail::TypedQueryLength(
			value.length(), from, to, GetInsertionPoint()));
	}

	/// Write the edit field without selecting a list item (CB_SETCURSEL fills).
	void ForceEditText(wxString const& text) {
		const bool was_applying = std::exchange(applying, true);
		auto restore = agi::make_scope_exit([&] { applying = was_applying; });
#ifdef __WXMSW__
		auto const combo_hwnd = reinterpret_cast<HWND>(GetHWND());
		if (combo_hwnd) {
			COMBOBOXINFO info{};
			info.cbSize = sizeof(info);
			if (::GetComboBoxInfo(combo_hwnd, &info) && info.hwndItem)
				::SetWindowTextW(info.hwndItem, text.wc_str());
			::SetWindowTextW(combo_hwnd, text.wc_str());
		}
#endif
		if (GetValue() != text)
			ChangeValue(text);
		SetInsertionPoint(static_cast<long>(text.length()));
		last_text_query = text;
	}

	/// List highlight + scroll only (not CB_SETCURSEL — that commits the name).
	/// Redraw is frozen so a late native prefix step does not paint intermediate
	/// rows when we immediately correct the caret (type-then-open path).
	void ApplyListCaret(std::size_t index) {
		if (index >= choices.size())
			return;
#ifdef __WXMSW__
		auto const list = NativeList();
		if (!list)
			return;
		// Already on the target row and it is the top item — nothing to do.
		// Avoids a second LB_SETTOPINDEX looking like another scroll step.
		if (::SendMessageW(list, LB_GETCURSEL, 0, 0) == static_cast<LRESULT>(index)
			&& ::SendMessageW(list, LB_GETTOPINDEX, 0, 0) == static_cast<LRESULT>(index))
			return;

		const bool was_applying = std::exchange(applying, true);
		auto restore = agi::make_scope_exit([&] { applying = was_applying; });
		::SendMessageW(list, WM_SETREDRAW, FALSE, 0);
		::SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(index), 0);
		::SendMessageW(list, LB_SETTOPINDEX, static_cast<WPARAM>(index), 0);
		::SendMessageW(list, WM_SETREDRAW, TRUE, 0);
		::RedrawWindow(list, nullptr, nullptr,
			RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
#else
		wxUnusedVar(index);
#endif
	}

	void StopSettleTimer() {
		if (settle_timer.IsRunning())
			settle_timer.Stop();
	}

	void StartSettleTimer() {
		// One caret re-stick if native clears the highlight after open/paint.
		// Does not re-run matching; only re-applies highlight_index.
		if (!settle_timer.IsRunning())
			settle_timer.StartOnce(50);
	}

	void OnSettleTimer(wxTimerEvent&) {
		StopSettleTimer();
		if (!contains_matching || !highlight_index || !IsDropDownOpen())
			return;
		// Caret only — rewriting the edit here re-triggers native list search
		// and looks like the list scrolling through items one by one.
		ApplyListCaret(*highlight_index);
		if (!typed_query.empty() && GetValue() != typed_query)
			ForceEditText(typed_query);
	}

	void JumpToMatch(std::size_t index, wxString const& typed) {
		if (index >= choices.size())
			return;
		typed_query = typed;
		highlight_index = index;

		// Caret first (may rewrite edit). Only force the edit when native
		// actually changed it — SetWindowText while dropped runs prefix search
		// and paints intermediate rows (type-then-open "scroll through" feel).
		ApplyListCaret(index);
		if (GetValue() != typed_query)
			ForceEditText(typed_query);
		StartSettleTimer();
	}

	void UpdateContainsHighlight(bool force_jump) {
		if (!contains_matching || applying)
			return;

		auto index = ExactListIndex(typed_query);
		if (!index) {
			if (auto const match = font_name_combo_box_detail::FindContainsMatch(choices, typed_query))
				index = match->index;
		}
		highlight_index = index;
		if (index && (force_jump || IsDropDownOpen()))
			JumpToMatch(*index, typed_query);
		else {
			if (!index)
				StopSettleTimer();
			if (GetValue() != typed_query)
				ForceEditText(typed_query);
		}
	}

	void AutoExpandForInput() {
		if (!auto_expand_on_input || applying || IsDropDownOpen())
			return;

		auto const match = contains_matching
							   ? font_name_combo_box_detail::FindContainsMatch(choices, typed_query)
							   : font_name_combo_box_detail::FindPrefixMatch(choices, typed_query);
		if (!match) {
			highlight_index.reset();
			return;
		}

		highlight_index = match->index;
		Popup();
#ifdef __WXMSW__
		// Opening by keyboard does not generate mouse movement, so the newly shown
		// controls can retain a transparent cursor previously set by the video
		// canvas. Ask the Aegisub window under the pointer to choose its cursor.
		// Sending WM_MOUSEMOVE (or moving the pointer) would change the hovered row.
		POINT cursor_position{};
		if (::GetCursorPos(&cursor_position)) {
			auto const cursor_window = ::WindowFromPoint(cursor_position);
			DWORD process_id = 0;
			if (cursor_window)
				::GetWindowThreadProcessId(cursor_window, &process_id);
			if (process_id == ::GetCurrentProcessId()) {
				::PostMessageW(cursor_window, WM_SETCURSOR,
					reinterpret_cast<WPARAM>(cursor_window),
					MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
			}
		}
#endif
	}

	void OnKeyDown(wxKeyEvent& event) {
		auto const key = event.GetKeyCode();
		if (auto_expand_on_input && IsDropDownOpen()
			&& (key == WXK_RETURN || key == WXK_NUMPAD_ENTER)) {
			auto const selection = font_name_combo_box_detail::ResolveCommitSelection(
				choices.size(), CurrentListCaret(), highlight_index);
			if (!selection) {
				event.Skip();
				return;
			}
			suppress_keyboard_text_enter = true;
			DismissSilently();

			wxCommandEvent selected(wxEVT_COMBOBOX, GetId());
			selected.SetEventObject(this);
			selected.SetInt(static_cast<int>(*selection));
			selected.SetString(choices[*selection].first);
			ProcessWindowEvent(selected);
			return;
		}
		event.Skip();
	}

	void OnKeyUp(wxKeyEvent& event) {
		auto const key = event.GetKeyCode();
		if (key == WXK_RETURN || key == WXK_NUMPAD_ENTER)
			suppress_keyboard_text_enter = false;
		event.Skip();
	}

	void TrackTextInput() {
		auto const query = ReadEditQuery();
		if (query == last_text_query)
			return;
		typed_query = last_text_query = query;
		selected_family_id = ExactListMatch(query);
		ResetMatching();
		if (contains_matching || auto_expand_on_input) {
			auto const revision = match_revision;
			CallAfter([this, revision] {
				if (revision != match_revision)
					return;
				if (contains_matching)
					UpdateContainsHighlight(/*force_jump=*/IsDropDownOpen());
				AutoExpandForInput();
			});
		}
	}

	void OnDropDown(wxCommandEvent& event) {
		drop_down_open = true;
		if (contains_matching) {
			// Defer until the native list exists, and ignore this callback if
			// the choices changed or the drop-down closed in the meantime.
			auto const revision = match_revision;
			CallAfter([this, revision] {
				if (revision == match_revision && IsDropDownOpen())
					UpdateContainsHighlight(/*force_jump=*/true);
			});
		}
		event.Skip();
	}

	void OnCloseUp(wxCommandEvent& event) {
		drop_down_open = false;
		if (highlight_index && !typed_query.empty() && GetValue() != typed_query)
			ForceEditText(typed_query);
		ResetMatching();
		event.Skip();
	}

	void ResetMatching() {
		++match_revision;
		highlight_index.reset();
		StopSettleTimer();
	}

	void DismissSilently() {
		if (IsDropDownOpen()) {
			auto const value = GetValue();
			long from = 0, to = 0;
			GetSelection(&from, &to);
			const bool was_applying = std::exchange(applying, true);
			auto restore = agi::make_scope_exit([&] { applying = was_applying; });
			Dismiss();
			if (GetValue() != value)
				ChangeValue(value);
			SetSelection(from, to);
		}
		drop_down_open = false;
	}

	void StoreChoices(std::vector<FontFamilyChoice> const& new_choices) {
		choices.clear();
		choices.reserve(new_choices.size());
		for (auto const& choice : new_choices)
			choices.emplace_back(wxString::FromUTF8(choice.label.c_str()), choice.family_id);
	}

#ifdef __WXMSW__
	static LRESULT CALLBACK GuardListCapture(HWND list, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
		auto& self = *reinterpret_cast<FontNameComboBox *>(id);
		if (message == WM_NCDESTROY) {
			::RemoveWindowSubclass(list, GuardListCapture, id);
			self.guarded_list = nullptr;
		}
		if (message != WM_CAPTURECHANGED || self.applying || !self.IsDropDownOpen())
			return ::DefSubclassProc(list, message, wp, lp);

		auto const next_capture = reinterpret_cast<HWND>(lp);
		if (!next_capture || next_capture == list || next_capture == self.GetHWND() || self.ContainsHWND(next_capture))
			return ::DefSubclassProc(list, message, wp, lp);

		// Moving capture to another control can make Windows accept the current
		// highlight and rewrite the edit before the other control handles its click.
		auto const value = self.GetValue();
		long from = 0, to = 0;
		self.GetSelection(&from, &to);
		self.applying = true;
		auto restore = agi::make_scope_exit([&] { self.applying = false; });
		auto const result = ::DefSubclassProc(list, message, wp, lp);
		self.drop_down_open = self.IsDropDownOpen();
		self.ResetMatching();
		if (self.GetValue() != value)
			self.ChangeValue(value);
		self.SetSelection(from, to);
		return result;
	}

	bool MSWCommand(WXUINT param, WXWORD id) override {
		if (param != CBN_SELCHANGE && param != CBN_SELENDOK)
			return wxComboBox::MSWCommand(param, id);

		// wxComboBox can emit text updates during the same native list pick.
		auto const previous = std::exchange(committing_list_selection, true);
		auto restore = agi::make_scope_exit([&] { committing_list_selection = previous; });
		return wxComboBox::MSWCommand(param, id);
	}
#endif

	bool ProcessEvent(wxEvent& event) override {
		if (event.GetId() != GetId())
			return wxComboBox::ProcessEvent(event);
		auto const type = event.GetEventType();
		if (applying && (type == wxEVT_TEXT || type == wxEVT_COMBOBOX ||
						 type == wxEVT_COMBOBOX_DROPDOWN || type == wxEVT_COMBOBOX_CLOSEUP || type == wxEVT_TEXT_ENTER))
			return true;
		if (type == wxEVT_TEXT_ENTER && suppress_keyboard_text_enter) {
			suppress_keyboard_text_enter = false;
			return true;
		}
		if (type == wxEVT_TEXT && !committing_list_selection)
			TrackTextInput();
		if (type == wxEVT_COMBOBOX) {
			int const index = static_cast<wxCommandEvent&>(event).GetInt();
			if (index < 0 || static_cast<std::size_t>(index) >= choices.size())
				return true;
			auto const [label, id] = choices[static_cast<std::size_t>(index)];
			auto const previous = std::exchange(committing_list_selection, true);
			auto restore = agi::make_scope_exit([&] { committing_list_selection = previous; });
			SetExplicitSelection(label, id);
			return wxComboBox::ProcessEvent(event);
		}
		return wxComboBox::ProcessEvent(event);
	}

public:
	FontNameComboBox(
		wxWindow *parent,
		wxString const& value,
		wxSize const& size,
		std::vector<FontFamilyChoice> const& choices,
		bool contains_matching,
		bool auto_expand_on_input)
	: wxComboBox(parent, -1, value, wxDefaultPosition, size, ToWxChoices(choices),
		wxCB_DROPDOWN | wxTE_PROCESS_ENTER)
	, typed_query(value)
	, last_text_query(value)
	, contains_matching(contains_matching)
	, auto_expand_on_input(auto_expand_on_input)
	{
		StoreChoices(choices);
#ifdef __WXMSW__
		if (auto const list = NativeList(); list && ::SetWindowSubclass(list, GuardListCapture, reinterpret_cast<UINT_PTR>(this), 0))
			guarded_list = list;
#endif
		if (auto_expand_on_input) {
			Bind(wxEVT_KEY_DOWN, &FontNameComboBox::OnKeyDown, this);
			Bind(wxEVT_KEY_UP, &FontNameComboBox::OnKeyUp, this);
		}

		if (contains_matching) {
			settle_timer.SetOwner(this);
			Bind(wxEVT_TIMER, &FontNameComboBox::OnSettleTimer, this,
				settle_timer.GetId());
		}
#if wxCHECK_VERSION(3, 1, 0)
		if (contains_matching || auto_expand_on_input) {
			Bind(wxEVT_COMBOBOX_DROPDOWN, &FontNameComboBox::OnDropDown, this);
			Bind(wxEVT_COMBOBOX_CLOSEUP, &FontNameComboBox::OnCloseUp, this);
		}
#endif

		if (auto const exact = ExactListMatch(value))
			selected_family_id = exact;
	}

	~FontNameComboBox() override {
#ifdef __WXMSW__
		if (guarded_list)
			::RemoveWindowSubclass(guarded_list, GuardListCapture, reinterpret_cast<UINT_PTR>(this));
#endif
		StopSettleTimer();
	}

	void SetChoices(std::vector<FontFamilyChoice> const& new_choices) {
		auto const value = GetValue();
		long from = 0, to = 0;
		GetSelection(&from, &to);
		const bool was_applying = std::exchange(applying, true);
		auto restore = agi::make_scope_exit([&] { applying = was_applying; });
		ResetMatching();
		DismissSilently();
		wxWindowUpdateLocker repaint(this);
		wxComboBox::Set(ToWxChoices(new_choices));
		StoreChoices(new_choices);
		ChangeValue(value);
		SetSelection(from, to);
	}

	void SetExplicitSelection(wxString const& label, FontFamilyId family_id) {
		ResetMatching();
		if (!committing_list_selection)
			DismissSilently();
		typed_query = label;
		ForceEditText(label);
		selected_family_id = family_id == 0 ? std::nullopt : std::optional{family_id};
	}

	[[nodiscard]] bool IsUpdatingInternally() const { return applying; }

	/// Committed family only (explicit list pick or exact typed label).
	std::optional<FontFamilyId> SelectedFamilyId() const noexcept {
		return selected_family_id;
	}

	/// True while a native list pick is replacing the edit text and dispatching
	/// its combo event. Text handlers can preserve state across the bare label.
	bool IsCommittingListSelection() const noexcept {
		return committing_list_selection;
	}
};
