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

#include <memory>
#include <string>
#include <wx/frame.h>
#include <wx/timer.h>
#include <wx/splitter.h>

#include "ui_dispatch.h"

#include <libaegisub/signal.h>

class AegisubApp;
class AsyncVideoProvider;
class AudioBox;
class VideoBox;
class LuaWorkspaceFrame;
class wxPanel;
class wxSplitterWindow;
class wxToolBar;
namespace agi { class AudioProvider; }
namespace agi { struct Context; class OptionValue; }

class FrameMain : public wxFrame {
	friend class AegisubApp;

	std::unique_ptr<agi::Context> context;
	LuaWorkspaceFrame *lua_workspace = nullptr;
	agi::ui::UiActivationScope ui_activation;

    // XXX: Make Freeze()/Thaw() noops on GTK, this seems to be buggy
#ifdef __WXGTK__
    void Freeze(void) {}
    void Thaw(void) {}
#endif

	bool showVideo = false; ///< Is the video display shown?
	bool showAudio = false; ///< Is the audio display shown?
	bool pending_video_open_ui_sync = false;
	bool pending_audio_open_ui_sync = false;
	wxPanel *contentsPanel = nullptr;
	wxTimer StatusClear;   ///< Status bar timeout timer
	agi::signal::Connection selection_changed_connection;
#ifdef _WIN32
	wxTimer FontChangeDebounce; ///< Debounces WM_FONTCHANGE bursts before refreshing subtitles
	wxTimer AudioOutputRecoveryDebounce; ///< Debounces session/device-change bursts before rebuilding XAudio2 output
	bool session_notifications_registered = false;
	std::string pending_audio_output_recovery_reason;
#endif

	void InitContents();
	void EnsureVideoBoxCreated();
	void EnsureAudioBoxCreated();
	void SyncAudioOpenUi();
	void SyncVideoOpenUi();

	void OnKeyDown(wxKeyEvent &event);
	void OnMouseWheel(wxMouseEvent &evt);

	void OnStatusClear(wxTimerEvent &event);
	void OnSelectedSetChanged();
	void UpdateSelectionAnchorStatus();
#ifdef _WIN32
	void OnFontChangeDebounce(wxTimerEvent &event);
	void OnAudioOutputRecoveryDebounce(wxTimerEvent &event);
	void QueueAudioOutputRecovery(std::string reason);
	void RegisterSessionNotifications();
	void UnregisterSessionNotifications();
#endif
	void OnCloseWindow (wxCloseEvent &event);

	void OnAudioOpen(agi::AudioProvider *provider);
	void OnVideoOpen(AsyncVideoProvider *provider);
	void OnVideoDetach(agi::OptionValue const& opt);
	void OnSubtitlesOpen();
	void OnSubtitleCommandToolbarVisibleChanged(agi::OptionValue const& opt);
	void OnEditGridSplitterSashPosChanged(wxSplitterEvent& event);
	void OnEditGridSplitterSashPosChanging(wxSplitterEvent& event);
	void QueueEditGridSplitterMinimumUpdate();
	int GetEditGridSplitterMinimumPosition();
	int UpdateEditGridSplitterMinimumPosition();
	void UpdateEditGridSplitterMinimum();

	void EnableToolBar(agi::OptionValue const& opt);

	AudioBox *audioBox = nullptr;      ///< The audio area
	VideoBox *videoBox = nullptr;      ///< The video area
	wxWindow *subtitleCommandToolbar = nullptr; ///< Configurable command buttons below the edit box

	wxSizer *MainSizer;  ///< Arranges things from top to bottom in the window
	wxSizer *TopSizer;   ///< Arranges video box and tool box from left to right
	wxSizer *ToolsSizer; ///< Arranges audio and editing areas top to bottom
	wxSplitterWindow *editGridSplitter = nullptr; ///< Splitter between edit area and grid
	wxPanel *editAreaPanel = nullptr; ///< Panel containing the edit area (top pane of splitter)
	int edit_grid_splitter_minimum_position = 0;
	bool pending_edit_grid_splitter_minimum_update = false;
	bool updating_edit_grid_splitter_sash = false;

public:
	FrameMain();
	~FrameMain();

#ifdef _WIN32
	WXLRESULT MSWWindowProc(WXUINT message, WXWPARAM wParam, WXLPARAM lParam) override;
#endif

	/// Set the status bar text
	/// @param text New status bar text
	/// @param ms Time in milliseconds that the message should be visible
	void StatusTimeout(wxString text,int ms=10000);

	/// Set the last executed command name on the status bar
	/// @param text Command display name
	void SetLastCommand(wxString text);

	/// @brief Set the video and audio display visibility
	/// @param video -1: leave unchanged; 0: hide; 1: show
	/// @param audio -1: leave unchanged; 0: hide; 1: show
	void SetDisplayMode(int showVid,int showAudio);
	/// Recalculate the edit/grid splitter after edit area contents change size
	void UpdateEditGridSplitterForContentChange();

	/// Update the window title to reflect current filename and modified state
	void UpdateTitle();
	LuaWorkspaceFrame *GetLuaWorkspace(bool create = true);
	bool PrepareLuaWorkspaceForClose();
	[[nodiscard]] bool NeedsLuaWorkspaceCloseDecision() const;
	void FinishLuaWorkspaceClose(bool discard);

	bool IsVideoShown() const { return showVideo; }
	bool IsAudioShown() const { return showAudio; }
	agi::ui::WeakLifetime GetAsyncUiLifetime() const { return ui_activation.GetLifetime(); }

	DECLARE_EVENT_TABLE()
};
