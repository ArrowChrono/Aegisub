// Copyright (c) 2005-2007, Rodrigo Braz Monteiro
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

#pragma once

#include "async_video_provider_host.h"
#include "deadline_pacing_policy.h"
#include "video_render_packet.h"
#include "video_subtitle_update_policy.h"

#include <libaegisub/signal.h>
#include <libaegisub/vfr.h>

#include <chrono>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "ui_dispatch.h"

class AssDialogue;
class AsyncVideoProvider;
class UiDeadlineTimer;
class VideoControllerTimer;
struct AssFileCommitDetails;

namespace agi {
	struct Context;
	class OptionValue;
}

enum class AspectRatio {
	Default = 0,
	Fullscreen,
	Widescreen,
	Cinematic,
	Custom
};

/// Manage stuff related to video playback
class VideoController final {
	/// Navigation target frame changed (new frame number)
	agi::signal::Signal<int> Seek;
	/// Continuous playback advanced to a new frame (new frame number)
	agi::signal::Signal<int> PlaybackFrameAdvanced;
	/// A render packet is ready to be presented.
	agi::signal::Signal<VideoRenderPacket const&, double> FrameReady;
	/// A frame was presented by the video display (new frame number).
	agi::signal::Signal<int> FramePresented;
	/// Aspect ratio was changed (type, value)
	agi::signal::Signal<AspectRatio, double> ARChange;

	agi::Context *context;
	agi::ui::UiActivationScope ui_activation;

	/// The video provider owned by the threaded frame source, or nullptr if no
	/// video is open
	AsyncVideoProvider *provider = nullptr;

	/// Last seen script color matrix
	std::string color_matrix;

	/// Playback timer used to periodically check if we should go to the next
	/// frame while playing video
	std::unique_ptr<VideoControllerTimer> playback_timer;
	std::unique_ptr<UiDeadlineTimer> visual_subtitle_update_timer;
	/// Debounce timer for idle prefetch of the frames ahead of the playhead
	std::unique_ptr<VideoControllerTimer> paused_prefetch_timer;
	// Match the tool's interactive display budget instead of producing faster than it can present.
	DeadlinePacingPolicy visual_subtitle_update_pacer{std::chrono::milliseconds(17)};
	video_subtitle_update_policy::UpdateCoalescer pending_visual_subtitle_updates;
	video_subtitle_update_policy::UpdateCoalescer final_visual_subtitle_updates;
	bool visual_subtitle_interaction_active = false;
	std::uint64_t visual_subtitle_interaction_id = 0;

	/// Time when playback was last started
	std::chrono::steady_clock::time_point playback_start_time;

	/// The start time of the first frame of the current playback; undefined if
	/// video is not currently playing
	int start_ms = 0;

	/// One past the last frame to play if video is currently playing
	int end_frame = 0;
	enum class PlaybackMode {
		None,
		ToEnd,
		LineRange
	};
	PlaybackMode playback_mode = PlaybackMode::None;
	int playback_end_ms = 0;
	bool playback_uses_audio_authority = false;
	/// Playback is armed but its first frame has not been delivered yet. The
	/// new audio clock starts with that frame; an ongoing seek may keep the old
	/// audio playing until then, while starting from pause waits silently.
	bool playback_start_pending = false;
	/// The frame the playback-start gate is waiting for
	int playback_start_frame = -1;
	std::chrono::steady_clock::time_point playback_pending_since;
	bool playback_start_wait_notice_shown = false;

	/// The frame number which was last requested from the video provider,
	/// which may not be the same thing as the currently displayed frame
	int frame_n = 0;
	/// The frame number which was last presented by the video display.
	int presented_frame_n = -1;
	/// Preview navigation may present an already-rendered in-flight frame. When
	/// that happens the controller treats the presented frame as the current one.
	bool accept_late_preview_frames = false;
	std::set<int> acceptable_late_preview_frames;
	bool inspection_step_in_flight = false;
	int inspection_step_frame = -1;
	int inspection_step_anchor_frame = -1;
	bool has_pending_inspection_step = false;
	int pending_inspection_step_frame = -1;
	bool pending_inspection_step_play_audio = false;
	int pending_inspection_step_delta = 0;
	enum class InteractiveSeekPhase {
		None,
		Pressed,
		Previewing
	};
	InteractiveSeekPhase interactive_seek_phase = InteractiveSeekPhase::None;
	PlaybackMode interactive_seek_preview_resume_mode = PlaybackMode::None;
	int interactive_seek_preview_resume_end_ms = 0;

	/// The picture aspect ratio of the video if the aspect ratio has been
	/// overridden by the user
	double ar_value = 1.;

	/// The current AR type
	AspectRatio ar_type = AspectRatio::Default;

	/// Cached option for audio playing when frame stepping
	const agi::OptionValue* playAudioOnStep;

	std::deque<VideoRenderPacket> recent_render_packets;

	void OnPlayTimer();
	void OnPausedPrefetchTimer();
	void SchedulePausedFramePrefetch();
	void OnVisualSubtitleUpdateTimer();
	void ArmVisualSubtitleUpdateTimer();
	void FlushPendingVisualSubtitleUpdate();
	void FlushDueVisualSubtitleUpdateBeforeFrameRequest();
	void SubmitSubtitleUpdate(
		video_subtitle_update_policy::CoalescedUpdate update,
		int reason);
	void ResetVisualSubtitleInteraction() noexcept;

	void HandleVideoError(std::string const& message);
	void HandleSubtitlesError(std::string const& message);
	void DeliverFrameReady(VideoRenderPacket packet, double time);
	void RememberRecentRenderPacket(VideoRenderPacket const& packet);
	void ClearRecentRenderPacketCache();
	bool TrySeekAndDeliverRecentRenderPacket(int frame);

	void OnSubtitlesCommit(AssFileCommitDetails commit);
	void OnNewVideoProvider(AsyncVideoProvider *provider);
	void OnActiveLineChanged(AssDialogue *line);
	void OnTimecodesChanged(agi::vfr::Framerate const&);

	void RequestFrame();
	void RequestFrame(bool supersede_in_flight);
	void RequestFrameImmediate();
	void RequestFramePreview(int target_frame, bool trace, bool supersede_in_flight);
	void ClearLatePreviewFrameAcceptance();
	void ClearInspectionStepState();
	void ClearInteractiveSeekPreviewState();
	int GetInspectionStepAnchorFrame() const;
	void StepFrames(int delta, bool play_audio_on_inspection);
	void HandleInspectionStepTarget(int target, bool immediate_request, bool play_audio, int delta);
	void RequestInspectionStepTarget(int target, bool immediate_request, bool play_audio, int delta);
	void PlayInspectionStepAudio(bool play_audio, int delta);
	void RequestPendingInspectionStepTarget();
	void StepSingleFrame(int delta);
	void StopPlayback(bool clear_interactive_seek_preview, bool schedule_paused_prefetch = true);
	bool StopIfAudioEnded();
	void StartPlayback(PlaybackMode mode, int range_end_ms = 0);
	bool PreparePlayback(PlaybackMode mode, int start_frame, int range_end_ms = 0, bool keep_audio_playing = false);
	void StartPlaybackTimer();
	void PrimeNextPlaybackFrame();
	void ResolvePendingPlaybackStart();
	bool FrameAlreadyDelivered(int frame) const;
	void ShowPlaybackStartWaitNotice();
	void HidePlaybackStartWaitNotice();
	void ResetPlaybackState();

public:
	static constexpr agi::vfr::Time DefaultJumpToTimeMode = agi::vfr::START;

	VideoController(agi::Context *context);
	~VideoController();

	/// Is the video currently playing?
	bool IsPlaying() const;
	bool PlaybackUsesAudioAuthority() const { return playback_uses_audio_authority; }

	/// Get the current frame number
	int GetFrameN() const { return frame_n; }
	/// Get the last presented frame number, or -1 if nothing has been presented yet
	int GetPresentedFrameN() const { return presented_frame_n; }

	/// Notify the controller that the display presented a new frame
	void NotifyFramePresented(int frame_number);
	/// Drop cached render packets after external video render pipeline changes
	void InvalidateRenderPacketCache();
	/// Begin coalescing subtitle-provider updates produced by a legacy visual tool.
	void BeginVisualSubtitleInteraction();
	/// Submit the final state; return its interaction id, or zero if none was submitted.
	std::uint64_t EndVisualSubtitleInteraction();

	/// Get the actual aspect ratio from a predefined AR type
	double GetARFromType(AspectRatio type) const;

	/// Override the aspect ratio of the currently loaded video
	void SetAspectRatio(double value);

	/// Override the aspect ratio of the currently loaded video
	/// @param type Predefined type to set the AR to. Must not be Custom.
	void SetAspectRatio(AspectRatio type);

	/// Get the current AR type
	AspectRatio GetAspectRatioType() const { return ar_type; }

	/// Get the current aspect ratio of the video
	double GetAspectRatioValue() const { return ar_value; }

	/// @brief Jump to the beginning of a frame
	/// @param n Frame number to jump to
	void JumpToFrame(int n);
	/// @brief Preview-seek to the beginning of a frame
	///
	/// Used for high-frequency navigation (drag/seek preview) where delivering
	/// an in-flight frame is better than dropping it under heavy decoder load.
	void PreviewToFrame(int n);
	/// Preview-seek while keeping only the newest requested frame.
	void PreviewToFrameLatest(int n);
	/// Warm one source frame in the provider cache without presenting it.
	void PrefetchFrame(int frame) noexcept;
	/// Capture the seek gesture's playback intent; playback continues until a preview.
	void BeginInteractiveSeekPreview();
	/// Commit an interactive seek preview and resume prior playback once if needed.
	void CommitInteractiveSeekPreviewToTime(int ms, agi::vfr::Time end = DefaultJumpToTimeMode);
	/// Cancel an interactive seek preview and resume prior playback once if needed.
	void CancelInteractiveSeekPreview();
	/// @brief Jump to a time
	/// @param ms Time to jump to in milliseconds
	/// @param end Type of time
	void JumpToTime(int ms, agi::vfr::Time end = DefaultJumpToTimeMode);

	/// Navigate by a relative number of frames (paused only).
	///
	/// Designed for hotkey repeat scenarios (e.g. prev/next-large): at most one
	/// request is in flight while later repeat input is coalesced to the newest target.
	void NavigateByFrames(int delta);
	/// Navigate to an absolute frame while paused.
	void NavigateToFrame(int frame);
	/// Navigate to the previous or next keyframe while paused, accumulating repeat
	/// input against the newest queued target.
	void NavigateToKeyframe(std::vector<int> const& keyframes, int direction);

	/// Starting playing the video
	void Play();
	/// Play the next frame then stop
	void NextFrame();
	/// Play the previous frame then stop
	void PrevFrame();
	/// Seek to the beginning of the current line, then play to the end of it
	void PlayLine();
	/// Stop playing
	void Stop();

	DEFINE_SIGNAL_ADDERS(Seek, AddSeekListener)
	DEFINE_SIGNAL_ADDERS(PlaybackFrameAdvanced, AddPlaybackFrameAdvancedListener)
	DEFINE_SIGNAL_ADDERS(FrameReady, AddFrameReadyListener)
	DEFINE_SIGNAL_ADDERS(FramePresented, AddFramePresentedListener)
	DEFINE_SIGNAL_ADDERS(ARChange, AddARChangeListener)
	agi::ui::WeakLifetime GetAsyncUiLifetime() const { return ui_activation.GetLifetime(); }
	AsyncVideoProviderEventSink CreateAsyncVideoProviderEventSink();

	int TimeAtFrame(int frame, agi::vfr::Time type = agi::vfr::EXACT) const;
	int FrameAtTime(int time, agi::vfr::Time type = agi::vfr::EXACT) const;
};
