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

#include <array>
#include <boost/container/map.hpp>
#include <boost/flyweight/flyweight_fwd.hpp>
#include <optional>
#include <utility>
#include <vector>

#include <wx/combobox.h>
#ifdef __WXMSW__
#include <wx/odcombo.h>
#endif
#include <wx/panel.h>
#include <wx/timer.h>

#include <libaegisub/signal.h>

#include "subtitle_command_session.h"
#include "subtitle_edit_box_color_click.h"
#include "time_display_mode.h"

namespace agi { namespace vfr { class Framerate; } }
namespace agi { struct Context; }
namespace agi { class Time; }
class AssDialogue;
class AssStyle;
class SubsStyledTextEditCtrl;
class TimeEdit;
class wxButton;
class wxCheckBox;
class wxMouseEvent;
class wxRadioButton;
class wxSizer;
class wxSpinCtrl;
class wxStyledTextCtrl;
class wxStyledTextEvent;
class wxTextCtrl;
struct AssDialogueBase;

#ifdef __WXMSW__
using SubsEditStyleComboBox = wxOwnerDrawnComboBox;
#else
using SubsEditStyleComboBox = wxComboBox;
#endif

template<class Base> class Placeholder;

/// @brief Main subtitle edit box
///
/// Controls the text edit and all surrounding controls
class SubsEditBox final : public wxPanel {
	enum TimeField {
		TIME_START = 0,
		TIME_END,
		TIME_DURATION
	};

	std::vector<agi::signal::Connection> connections;

	/// Currently active dialogue line
	AssDialogue *line = nullptr;
	AssStyle *active_style = nullptr;

	/// Are the buttons currently split into two lines?
	bool button_bar_split = true;
	/// Are the controls currently enabled?
	bool controls_enabled = true;

	agi::Context *c;
	aegisub::SubtitleCommandSession command_session;

	agi::signal::Connection file_changed_slot;

	// Box controls
	wxCheckBox *comment_box;
	SubsEditStyleComboBox *style_box;
	wxButton *style_edit_button;
	Placeholder<wxComboBox> *actor_box;
	TimeEdit *start_time;
	TimeEdit *end_time;
	TimeEdit *duration;
	wxSpinCtrl *layer;
	std::array<wxSpinCtrl *, 3> margin;
	Placeholder<wxComboBox> *effect_box;
	wxRadioButton *by_ass;
	wxRadioButton *by_exact;
	wxRadioButton *by_frame;
	wxTextCtrl *char_count;
	wxCheckBox *split_box;
	bool code_mode = false;
	SubtitleTimeDisplayMode time_display_mode = SubtitleTimeDisplayMode::Ass;
	SubtitleTimeDisplayMode non_frame_display_mode = SubtitleTimeDisplayMode::Ass;
	SubtitleTimeDisplayMode file_default_display_mode = SubtitleTimeDisplayMode::Ass;

	wxSizer *top_sizer;
	wxSizer *middle_right_sizer;
	wxSizer *middle_left_sizer;
	wxSizer *bottom_sizer;

	void SetControlsState(bool state);
	/// @brief Update times of selected lines
	/// @param field Field which changed
	void CommitTimes(TimeField field);
	/// @brief Commits the current edit box contents
	/// @param desc Undo description to use
	void CommitText(wxString const& desc);

	/// Last used commit message to avoid coalescing different types of changes
	wxString last_commit_type;

	/// Last field to get a time commit, as they all have the same commit message
	int last_time_commit_type;

	/// Timer to stop coalescing changes after a break with no edits
	wxTimer undo_timer;
	/// Caps visual-tool text synchronization to roughly one edit-box update per frame.
	wxTimer visual_tool_text_sync_timer;
	bool visual_tool_text_sync_pending = false;

	/// Colour buttons resolve single vs double clicks themselves: a click only
	/// opens its dialog command once the double-click window passes, and the
	/// port-paired second press (wxEVT_LEFT_DCLICK) runs the pick-from-video
	/// command instead. Plain presses never reroute — see
	/// subtitle_edit_box_color_click.h for the per-port event sequences.
	wxTimer color_click_timer;
	aegisub::subtitle_edit_box_color_click::ColorClickSequencer color_click_sequencer;
	const char *pending_color_open_command = nullptr;

	bool IsVisualToolInteracting() const;
	void QueueVisualToolTextSync();
	void CancelVisualToolTextSync();
	void OnVisualToolTextSyncTimer(wxTimerEvent& event);

	/// The start and end times of the selected lines without changes made to
	/// avoid negative durations, so that they can be restored if future changes
	/// eliminate the negative durations
	boost::container::map<AssDialogue *, std::pair<agi::Time, agi::Time>> initial_times;

	// Constructor helpers
	wxSpinCtrl *MakeMarginCtrl(wxString const& tooltip, int margin, wxString const& commit_msg);
	TimeEdit *MakeTimeCtrl(wxString const& tooltip, TimeField field);
	void MakeButton(const char *cmd_name);
	void MakeColorButton(const char *open_cmd_name, const char *pick_cmd_name);
	wxButton *MakeBottomButton(const char *cmd_name);
	SubsEditStyleComboBox *MakeStyleComboBox(wxString const& initial_text, void (SubsEditBox::*handler)(wxCommandEvent&), wxString const& tooltip);
	wxRadioButton *MakeRadio(wxString const& text, bool start, wxString const& tooltip);

#ifdef WITH_WXSTC
	void OnChangeStc(wxStyledTextEvent &event);
#endif
	void OnChangeTc(wxCommandEvent& event);
	void OnKeyDown(wxKeyEvent &event);
	void OnMouse(wxMouseEvent &event);

	void OnActiveLineChanged(AssDialogue *new_line);
	void OnSelectedSetChanged();
	void OnLineInitialTextChanged(std::string const& new_text);

	void OnFrameTimeRadio(wxCommandEvent &event);
	void OnStyleChange(wxCommandEvent &event);
	void OnActorChange(wxCommandEvent &event);
	void OnLayerEnter(wxCommandEvent &event);
	void OnCommentChange(wxCommandEvent &);
	void OnEffectChange(wxCommandEvent &);
	void OnSize(wxSizeEvent &event);
#ifdef __WXMSW__
	WXLRESULT MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) override;
#endif
	void OnSplit(wxCommandEvent&);
	void DoOnSplit(bool show_original);

	void SetPlaceholderCtrl(wxControl *ctrl, wxString const& value);

	/// @brief Set a field in each selected line to a specified value
	/// @param set   Callable which updates a passed line
	/// @param desc  Undo description to use
	/// @param type  Commit type to use
	/// @param amend Coalesce sequences of commits of the same type
	template<class setter>
	void SetSelectedRows(setter set, wxString const& desc, int type, bool amend = false);

	/// @brief Set a field in each selected line to a specified value
	/// @param field Field to set
	/// @param value Value to set the field to
	/// @param desc  Undo description to use
	/// @param type  Commit type to use
	/// @param amend Coalesce sequences of commits of the same type
	template<class T>
	void SetSelectedRows(T AssDialogueBase::*field, T value, wxString const& desc, int type, bool amend = false);

	template<class T>
	void SetSelectedRows(T AssDialogueBase::*field, wxString const& value, wxString const& desc, int type, bool amend = false);

	/// @brief Reload the current line from the file
	/// @param type AssFile::COMMITType
	void OnCommit(int type, AssDialogue const* changed);

	void UpdateFields(int type, bool repopulate_lists);
	void UpdateCodeMode();

	/// Regenerate a dropdown list with the unique values of a dialogue field
	void PopulateList(wxComboBox *combo, boost::flyweight<std::string> AssDialogue::*field);

	/// @brief Enable or disable frame timing mode
	void UpdateFrameTiming(agi::vfr::Framerate const& fps);
	void ApplyTimeDisplayMode(SubtitleTimeDisplayMode mode, bool update_radio_buttons = true);
	void UpdateTimeDisplayModeFromFile(bool force_apply);

	/// Update the character count box for the given text
	void UpdateCharacterCount(std::string const& text);

	/// Call a command and optionally restore focus to the edit control
	/// (quick-pick entries skip the refocus so status hints stay visible).
	void CallCommand(const char *cmd_name, bool refocus_edit_control = true);
	/// Second mouse-down on a colour button inside the double-click window.
	void RunPendingColorQuickPick(const char *pick_cmd_name);
	/// Double-click window elapsed without a second press: fire the dialog.
	void ConfirmPendingColorClick();

	void SetDurationField();

#ifdef WITH_WXSTC
	const bool use_stc;
	SubsStyledTextEditCtrl *edit_ctrl_stc;
#endif
	wxTextCtrl* edit_ctrl_tc;
	wxTextCtrl *secondary_editor;

public:
	/// @brief Constructor
	/// @param parent Parent window
	SubsEditBox(wxWindow *parent, agi::Context *context);
	~SubsEditBox();

	/// Report whether the main subtitle text editor can currently take focus.
	bool CanFocusEditControl() const;
	/// Give keyboard focus to the main subtitle text editor.
	void FocusEditControl();
	/// Apply the latest deferred edit-box text when a visual interaction ends.
	void FlushVisualToolTextSync();
	/// Return the current selection in the main subtitle text editor.
	std::string GetEditControlSelectedText() const;
	/// Return the current caret/selection as 0-based character offsets [start, stop).
	/// No selection: start == stop.
	std::optional<std::pair<int, int>> GetEditControlCaret() const;
	/// Set the caret before or after the 1-based character index in the main editor.
	void SetEditControlCaret(int character_index, bool after);
	/// Set a selection range in the main editor as 0-based character offsets [start, stop).
	void SetEditControlSelection(int start, int stop);
};
