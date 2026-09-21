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

#include <libaegisub/fs_fwd.h>
#include <libaegisub/signal.h>

#include <memory>
#include <vector>
#include <wx/panel.h>

namespace agi { struct Context; }
namespace agi { class OptionValue; }
class AsyncVideoProvider;
class SecondarySubtitleStrip;
class SecondarySubtitleSession;
class VideoDisplay;
class wxSpinCtrl;
class wxTextCtrl;
class wxStaticLine;

/// @class VideoBox
/// @brief The box containing the video display and associated controls
class VideoBox final : public wxPanel {
	std::vector<agi::signal::Connection> connections;
	agi::Context *context;     ///< Project context
	wxTextCtrl *VideoPosition; ///< Current frame time
	wxSpinCtrl *VideoFrameInput; ///< Current frame and frame jump input
	wxTextCtrl *VideoSubsPos;  ///< Time relative to the active subtitle line
	SecondarySubtitleStrip *secondarySubtitleStrip = nullptr;
	wxStaticLine *secondarySubtitleStripSeparator = nullptr;
	VideoDisplay *videoDisplay = nullptr;
	std::shared_ptr<SecondarySubtitleSession> secondarySubtitleSession;
	// The attached box owns provider activation; detached boxes are presenters
	// for the same ContextUiState session and only switch view visibility.
	bool ownsSecondarySubtitleSession = false;
	bool isDetached = false;
	bool hasVideoProvider = false;
	bool presentationAvailable = true;
	bool closing = false;
	int current_frame = -1;
	bool secondarySubtitleStripHeightDragActive = false;
	int secondarySubtitleStripHeightDragPreservedVideoHeight = 0;

	/// Update VideoPosition and VideoSubsPos
	void UpdateTimeBoxes();
	void JumpToInputFrame();
	void ApplyVideoProvider();
	void ApplyVideoProvider(AsyncVideoProvider *provider);
	void UpdateSecondarySubtitleStripGutter();
	void UpdateSecondarySubtitleStripVisibility();
	int GetSecondarySubtitleLayoutMinHeight() const;
	void RelayoutAfterSecondarySubtitleStripChange(int preserved_video_height, int preferred_client_height_delta);
	void OnCurrentFrameChanged(int frame_number);
	void OnSubtitlesCommit(int type);
	void OnVideoProviderChanged(AsyncVideoProvider *provider);
	void OnDetachedVideoChanged(agi::OptionValue const&);
	void OnSecondarySubtitleStripEnabledChanged(agi::OptionValue const&);
	void OnSize(wxSizeEvent &event);

public:
	VideoBox(
		wxWindow *parent,
		bool isDetached,
		agi::Context *context);
	void SyncToContextState();
	void SyncSecondarySubtitleStripVisibility();
	void SetSecondarySubtitlePresentationAvailable(bool available);
	void PrepareForDetachedClose();
	bool OpenSecondarySubtitlesFromPath(agi::fs::path const& path, bool show_errors = true);
	void OnSecondarySubtitleStripHeightChanged(int previous_height, int new_height);
	void BeginSecondarySubtitleStripHeightDrag();
	void PreviewSecondarySubtitleStripHeightChange();
	void CommitSecondarySubtitleStripHeightDrag();
};
