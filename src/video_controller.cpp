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

#include "video_controller.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "audio_controller.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "perf_trace.h"
#include "project.h"
#include "selection_controller.h"
#include "time_range.h"
#include "ui_deadline_timer.h"
#include "async_video_provider.h"
#include "async_video_trace.h"
#include "utils.h"
#include "video_controller_timer.h"
#include "video_navigation_ops.h"
#include "video_subtitle_update_policy.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace {
void AddSubtitleCommit(
	video_subtitle_update_policy::UpdateCoalescer& updates,
	video_subtitle_update_policy::UpdateMode mode,
	AssDialogueCommitSpan changed_lines) {
	if (mode == video_subtitle_update_policy::UpdateMode::FullReload) {
		updates.AddFullReload();
		return;
	}

	std::vector<int> rows;
	rows.reserve(changed_lines.size());
	for (auto const* line : changed_lines)
		rows.push_back(line ? line->Row : -1);
	updates.AddIncrementalRows(rows);
}

/// How long the playback-start gate waits before hinting at the wait in the
/// status bar
constexpr int PlaybackStartWaitNoticeMs = 300;
/// How long the playback-start gate waits for the first frame before giving
/// up and letting the audio clock run (the picture snaps forward once the
/// stalled read completes)
constexpr int PlaybackStartFallbackMs = 10000;
/// How long to stay idle after a presentation before prefetching ahead
constexpr int PausedPrefetchDebounceMs = 250;
/// How far ahead of the playhead idle prefetch warms the frame cache
constexpr int PausedPrefetchLookaheadMs = 500;
constexpr int PausedPrefetchMaxFrames = 16;
}

VideoController::VideoController(agi::Context *c)
	: context(c), playback_timer(CreateVideoControllerTimer([this] { OnPlayTimer(); })), visual_subtitle_update_timer(std::make_unique<UiDeadlineTimer>([this] { OnVisualSubtitleUpdateTimer(); })), paused_prefetch_timer(CreateVideoControllerTimer([this] { OnPausedPrefetchTimer(); })), playAudioOnStep(OPT_GET("Audio/Plays When Stepping Video")) {
	auto core = context->GetCore();
	ui_activation.AddConnections(
		core.ass->AddCommitDetailsListener(&VideoController::OnSubtitlesCommit, this),
		core.project->AddVideoProviderListener(&VideoController::OnNewVideoProvider, this),
		core.project->AddTimecodesListener(&VideoController::OnTimecodesChanged, this),
		core.selectionController->AddActiveLineListener(&VideoController::OnActiveLineChanged, this));
}

VideoController::~VideoController() {
	ui_activation.Deactivate();
	ResetVisualSubtitleInteraction();
}

void VideoController::ResetPlaybackState() {
	playback_mode = PlaybackMode::None;
	playback_end_ms = 0;
	playback_uses_audio_authority = false;
	playback_start_pending = false;
	playback_start_frame = -1;
	playback_start_wait_notice_shown = false;
}

void VideoController::OnNewVideoProvider(AsyncVideoProvider *new_provider) {
	paused_prefetch_timer->Stop();
	StopPlayback(true, false);
	ResetVisualSubtitleInteraction();
	provider = new_provider;
	presented_frame_n = -1;
	ClearLatePreviewFrameAcceptance();
	ClearInspectionStepState();
	ClearInteractiveSeekPreviewState();
	ClearRecentRenderPacketCache();
	color_matrix = provider ? provider->GetColorSpace() : "";
	ResetPlaybackState();
}

void VideoController::OnSubtitlesCommit(AssFileCommitDetails commit) {
	if (!provider) return;
	auto core = context->GetCore();
	ClearInspectionStepState();
	ClearInteractiveSeekPreviewState();
	ClearRecentRenderPacketCache();

	if ((commit.type & AssFile::COMMIT_SCRIPTINFO) || commit.type == AssFile::COMMIT_NEW) {
		auto new_matrix = core.ass->GetScriptInfo("YCbCr Matrix");
		if (!new_matrix.empty() && new_matrix != color_matrix) {
			color_matrix = new_matrix;
			provider->SetColorSpace(new_matrix);
		}
	}

	auto const update_mode = video_subtitle_update_policy::SelectUpdateMode(
		commit.type, commit.changed_lines);
	if (visual_subtitle_interaction_active) {
		AddSubtitleCommit(pending_visual_subtitle_updates, update_mode, commit.changed_lines);
		AddSubtitleCommit(final_visual_subtitle_updates, update_mode, commit.changed_lines);
		bool const allowed = visual_subtitle_update_pacer.Request(DeadlinePacingPolicy::Clock::now());
		perf_trace::ObserveVideoUiDuration("video_controller.subtitle_update.request.commit", 0.0, allowed ? 1 : 0);
		if (allowed) {
			visual_subtitle_update_timer->Stop();
			FlushPendingVisualSubtitleUpdate();
		}
		else {
			ArmVisualSubtitleUpdateTimer();
		}
		return;
	}

	bool const incremental = update_mode == video_subtitle_update_policy::UpdateMode::IncrementalLines;
	perf_trace::ObserveVideoUiDuration(
		"video_controller.subtitle_update.immediate",
		0.0,
		static_cast<int>(commit.changed_lines.size()),
		incremental ? 1 : 0);
	perf_trace::VideoUiDurationScope subtitle_update_trace(
		"video_controller.subtitle_update",
		static_cast<int>(commit.changed_lines.size()),
		incremental ? 1 : 0);
	if (incremental)
		provider->UpdateSubtitles(core.ass.get(), commit.changed_lines);
	else
		provider->LoadSubtitles(core.ass.get());
}

void VideoController::BeginVisualSubtitleInteraction() {
	if (visual_subtitle_interaction_active)
		return;

	visual_subtitle_update_timer->Stop();
	pending_visual_subtitle_updates.Clear();
	final_visual_subtitle_updates.Clear();
	if (++visual_subtitle_interaction_id == 0)
		++visual_subtitle_interaction_id;
	visual_subtitle_update_pacer.Begin(DeadlinePacingPolicy::Clock::now());
	visual_subtitle_interaction_active = true;
}

std::uint64_t VideoController::EndVisualSubtitleInteraction() {
	if (!visual_subtitle_interaction_active)
		return 0;

	visual_subtitle_interaction_active = false;
	visual_subtitle_update_timer->Stop();
	visual_subtitle_update_pacer.Force(DeadlinePacingPolicy::Clock::now());
	visual_subtitle_update_pacer.End();
	pending_visual_subtitle_updates.Clear();
	auto final_update = final_visual_subtitle_updates.Take();
	if (final_update && provider) {
		SubmitSubtitleUpdate(std::move(*final_update), 2);
		return visual_subtitle_interaction_id;
	}
	return 0;
}

void VideoController::OnVisualSubtitleUpdateTimer() {
	if (!visual_subtitle_interaction_active)
		return;

	auto const now = DeadlinePacingPolicy::Clock::now();
	auto const deadline = visual_subtitle_update_pacer.NextDeadline();
	if (deadline) {
		perf_trace::ObserveVideoUiDuration(
			"video_controller.subtitle_update.timer_lateness",
			std::chrono::duration<double, std::milli>(now - *deadline).count());
	}
	bool const allowed = visual_subtitle_update_pacer.OnTimer(now);
	perf_trace::ObserveVideoUiDuration(
		"video_controller.subtitle_update.timer_fire", 0.0, allowed ? 1 : 0, deadline ? 1 : 0);
	if (allowed)
		FlushPendingVisualSubtitleUpdate();
	else
		ArmVisualSubtitleUpdateTimer();
}

void VideoController::ArmVisualSubtitleUpdateTimer() {
	if (visual_subtitle_update_timer->IsRunning())
		return;

	auto const deadline = visual_subtitle_update_pacer.NextDeadline();
	if (!deadline)
		return;

	auto const remaining = *deadline - DeadlinePacingPolicy::Clock::now();
	auto const delay = std::max<std::int64_t>(
		1,
		std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
	int const delay_ms = static_cast<int>(std::min<std::int64_t>(delay, std::numeric_limits<int>::max()));
	visual_subtitle_update_timer->StartAt(*deadline);
	perf_trace::ObserveVideoUiDuration("video_controller.subtitle_update.timer_arm", 0.0, delay_ms);
}

void VideoController::FlushPendingVisualSubtitleUpdate() {
	auto update = pending_visual_subtitle_updates.Take();
	if (update)
		SubmitSubtitleUpdate(std::move(*update), 1);
}

void VideoController::FlushDueVisualSubtitleUpdateBeforeFrameRequest() {
	if (!visual_subtitle_interaction_active || pending_visual_subtitle_updates.Empty())
		return;

	bool const allowed = visual_subtitle_update_pacer.Request(DeadlinePacingPolicy::Clock::now());
	perf_trace::ObserveVideoUiDuration("video_controller.subtitle_update.request.frame", 0.0, allowed ? 1 : 0);
	if (allowed) {
		visual_subtitle_update_timer->Stop();
		FlushPendingVisualSubtitleUpdate();
	}
	else {
		ArmVisualSubtitleUpdateTimer();
	}
}

void VideoController::SubmitSubtitleUpdate(
	video_subtitle_update_policy::CoalescedUpdate update,
	int reason) {
	if (!provider)
		return;

	auto core = context->GetCore();
	std::vector<AssDialogue const*> changed_lines;
	if (update.mode == video_subtitle_update_policy::UpdateMode::IncrementalLines) {
		changed_lines.reserve(update.rows.size());
		auto row = update.rows.begin();
		for (auto const& line : core.ass->Events) {
			if (row == update.rows.end())
				break;
			if (line.Row == *row) {
				changed_lines.push_back(&line);
				++row;
			}
		}
		if (changed_lines.size() != update.rows.size()) {
			update.mode = video_subtitle_update_policy::UpdateMode::FullReload;
			changed_lines.clear();
		}
	}

	bool const incremental = update.mode == video_subtitle_update_policy::UpdateMode::IncrementalLines;
	char const* phase = reason == 2
		? "video_controller.subtitle_update.final"
		: "video_controller.subtitle_update.paced";
	perf_trace::ObserveVideoUiDuration(
		phase,
		0.0,
		static_cast<int>(changed_lines.size()),
		incremental ? 1 : 0);
	perf_trace::VideoUiDurationScope subtitle_update_trace(
		"video_controller.subtitle_update",
		static_cast<int>(changed_lines.size()),
		incremental ? 1 : 0);
	if (incremental)
		provider->UpdateSubtitles(core.ass.get(), changed_lines, {
			reason == 1 ? VideoRenderDeliveryClass::VisualSubtitleIntermediate : VideoRenderDeliveryClass::VisualSubtitleFinal,
			visual_subtitle_interaction_id,
			reason == 2});
	else
		provider->LoadSubtitles(core.ass.get(), {
			reason == 1 ? VideoRenderDeliveryClass::VisualSubtitleIntermediate : VideoRenderDeliveryClass::VisualSubtitleFinal,
			visual_subtitle_interaction_id,
			reason == 2});
}

void VideoController::ResetVisualSubtitleInteraction() noexcept {
	if (visual_subtitle_update_timer)
		visual_subtitle_update_timer->Stop();
	visual_subtitle_update_pacer.End();
	pending_visual_subtitle_updates.Clear();
	final_visual_subtitle_updates.Clear();
	visual_subtitle_interaction_active = false;
}

void VideoController::OnTimecodesChanged(agi::vfr::Framerate const&) {
	ClearInspectionStepState();
	ClearInteractiveSeekPreviewState();
	ClearRecentRenderPacketCache();
}

void VideoController::OnActiveLineChanged(AssDialogue *line) {
	perf_trace::VideoUiDurationScope trace(
		"grid_select.video.seek",
		(line && OPT_GET("Video/Subtitle Sync")->GetBool()) ? 1 : 0,
		provider ? 1 : 0);
	if (line && provider && OPT_GET("Video/Subtitle Sync")->GetBool()) {
		Stop();
		JumpToTime(line->Start);
	}
}

void VideoController::RequestFrame() {
	RequestFrame(true);
}

void VideoController::RequestFrame(bool supersede_in_flight) {
	FlushDueVisualSubtitleUpdateBeforeFrameRequest();
	auto core = context->GetCore();
	core.ass->Properties.video_position = frame_n;
	if (supersede_in_flight)
		ClearLatePreviewFrameAcceptance();
	auto const frame_time = TimeAtFrame(frame_n);
	perf_trace::ObserveFrameRequest(frame_n, frame_time, false);
	provider->RequestFrame(frame_n, frame_time, supersede_in_flight);
}

void VideoController::RequestFrameImmediate() {
	FlushDueVisualSubtitleUpdateBeforeFrameRequest();
	auto core = context->GetCore();
	ClearInspectionStepState();
	ClearInteractiveSeekPreviewState();
	core.ass->Properties.video_position = frame_n;
	ClearLatePreviewFrameAcceptance();
	auto const frame_time = TimeAtFrame(frame_n);
	perf_trace::ObserveFrameRequest(frame_n, frame_time, true);

	try {
		provider->CancelPendingFrameRequests();

		// Synchronous callers favor deterministic display over latest-only coalescing.
		int const requested_frame = frame_n;
		auto packet = provider->GetRenderPacket(frame_n, frame_time);
		perf_trace::ObserveFrameResult(frame_n, frame_time, true, true);
		DeliverFrameReady(std::move(packet), frame_time);
		if (presented_frame_n != requested_frame)
			NotifyFramePresented(requested_frame);
	}
	catch (AsyncVideoProviderVideoError const& err) {
		HandleVideoError(err.GetMessage());
	}
	catch (AsyncVideoProviderSubtitlesError const& err) {
		HandleSubtitlesError(err.GetMessage());
	}
}

void VideoController::RequestFramePreview(int target_frame, bool trace, bool supersede_in_flight) {
	if (!provider)
		return;

	ClearInspectionStepState();
	ClearInteractiveSeekPreviewState();
	if (!supersede_in_flight && !accept_late_preview_frames)
		provider->CancelPendingFrameRequests();

	frame_n = mid(0, target_frame, provider->GetFrameCount() - 1);
	accept_late_preview_frames = !supersede_in_flight;
	if (accept_late_preview_frames)
		acceptable_late_preview_frames.insert(frame_n);
	else
		acceptable_late_preview_frames.clear();
	if (trace)
		perf_trace::TraceSeek(frame_n, false);
	RequestFrame(supersede_in_flight);
	Seek(frame_n);
}

void VideoController::ClearLatePreviewFrameAcceptance() {
	accept_late_preview_frames = false;
	acceptable_late_preview_frames.clear();
}

void VideoController::ClearInspectionStepState() {
	inspection_step_in_flight = false;
	inspection_step_frame = -1;
	inspection_step_anchor_frame = -1;
	has_pending_inspection_step = false;
	pending_inspection_step_frame = -1;
	pending_inspection_step_play_audio = false;
	pending_inspection_step_delta = 0;
}

void VideoController::ClearInteractiveSeekPreviewState() {
	interactive_seek_phase = InteractiveSeekPhase::None;
	interactive_seek_preview_resume_mode = PlaybackMode::None;
	interactive_seek_preview_resume_end_ms = 0;
}

int VideoController::GetInspectionStepAnchorFrame() const {
	if ((inspection_step_in_flight || has_pending_inspection_step) && inspection_step_anchor_frame >= 0)
		return inspection_step_anchor_frame;
	return frame_n;
}

void VideoController::HandleInspectionStepTarget(int target, bool immediate_request, bool play_audio, int delta) {
	if (!provider)
		return;

	if (inspection_step_in_flight) {
		if (target == inspection_step_anchor_frame)
			return;

		inspection_step_anchor_frame = target;
		if (target == inspection_step_frame) {
			has_pending_inspection_step = false;
			pending_inspection_step_frame = -1;
			pending_inspection_step_play_audio = false;
			pending_inspection_step_delta = 0;
		}
		else {
			has_pending_inspection_step = true;
			pending_inspection_step_frame = target;
			pending_inspection_step_play_audio = play_audio;
			pending_inspection_step_delta = delta;
		}
		return;
	}

	if (target == frame_n)
		return;

	RequestInspectionStepTarget(target, immediate_request, play_audio, delta);
}

void VideoController::RequestInspectionStepTarget(int target, bool immediate_request, bool play_audio, int delta) {
	frame_n = target;
	inspection_step_in_flight = true;
	inspection_step_frame = frame_n;
	inspection_step_anchor_frame = frame_n;
	has_pending_inspection_step = false;
	pending_inspection_step_frame = -1;
	pending_inspection_step_play_audio = false;
	pending_inspection_step_delta = 0;
	ClearLatePreviewFrameAcceptance();
	perf_trace::TraceSeek(frame_n, false);
	bool const delivered_from_cache = immediate_request && TrySeekAndDeliverRecentRenderPacket(frame_n);
	if (!delivered_from_cache) {
		RequestFrame();
		Seek(frame_n);
	}

	PlayInspectionStepAudio(play_audio, delta);
}

void VideoController::PlayInspectionStepAudio(bool play_audio, int delta) {
	if (!play_audio)
		return;

	auto core = context->GetCore();
	if (delta > 0) {
		core.audioController->PlayRange(TimeRange(TimeAtFrame(frame_n - 1), TimeAtFrame(frame_n)));
	}
	else if (delta < 0) {
		core.audioController->PlayRange(TimeRange(TimeAtFrame(frame_n), TimeAtFrame(frame_n + 1)));
	}
}

void VideoController::StepFrames(int delta, bool play_audio_on_inspection) {
	if (!provider || IsPlaying())
		return;

	int const frame_count = provider->GetFrameCount();
	if (frame_count <= 0)
		return;

	int const target = mid(0, GetInspectionStepAnchorFrame() + delta, frame_count - 1);
	HandleInspectionStepTarget(target, true, play_audio_on_inspection, delta);
}

void VideoController::NavigateToFrame(int target_frame) {
	if (!provider || IsPlaying())
		return;

	int const frame_count = provider->GetFrameCount();
	if (frame_count <= 0)
		return;

	int const target = mid(0, target_frame, frame_count - 1);
	HandleInspectionStepTarget(target, true, false, target - GetInspectionStepAnchorFrame());
}

void VideoController::NavigateToKeyframe(std::vector<int> const& keyframes, int direction) {
	if (!provider || IsPlaying())
		return;

	int const anchor_frame = GetInspectionStepAnchorFrame();
	int const target = direction < 0
		? aegisub::video_navigation_ops::ComputePreviousKeyframe(keyframes, anchor_frame)
		: aegisub::video_navigation_ops::ComputeNextKeyframe(keyframes, anchor_frame, provider->GetFrameCount() - 1);
	HandleInspectionStepTarget(target, true, false, direction);
}

void VideoController::JumpToFrame(int n) {
	if (!provider) return;
	ClearInspectionStepState();
	ClearInteractiveSeekPreviewState();

	bool was_playing = IsPlaying();
	auto resume_mode = playback_mode;
	auto resume_end_ms = playback_end_ms;

	frame_n = mid(0, n, provider->GetFrameCount() - 1);
	ClearLatePreviewFrameAcceptance();
	perf_trace::TraceSeek(frame_n, was_playing);
	bool const delivered_from_cache = !was_playing && TrySeekAndDeliverRecentRenderPacket(frame_n);
	if (!delivered_from_cache) {
		RequestFrame();
		Seek(frame_n);
	}

	if (was_playing) {
		if (!PreparePlayback(resume_mode, frame_n, resume_end_ms, true)) {
			Stop();
			return;
		}
	}
}

void VideoController::PreviewToFrame(int n) {
	if (!provider) return;
	ClearInspectionStepState();
	ClearInteractiveSeekPreviewState();

	bool const already_accepting_late_preview = accept_late_preview_frames;
	if (!already_accepting_late_preview)
		ClearLatePreviewFrameAcceptance();

	bool was_playing = IsPlaying();
	auto resume_mode = playback_mode;
	auto resume_end_ms = playback_end_ms;
	if (was_playing) {
		Stop();
		provider->CancelPendingFrameRequests();
	}
	else if (!already_accepting_late_preview) {
		provider->CancelPendingFrameRequests();
	}

	frame_n = mid(0, n, provider->GetFrameCount() - 1);
	accept_late_preview_frames = true;
	acceptable_late_preview_frames.insert(frame_n);
	perf_trace::TraceSeek(frame_n, was_playing);
	RequestFrame(false);
	Seek(frame_n);

	if (was_playing && PreparePlayback(resume_mode, frame_n, resume_end_ms))
		StartPlaybackTimer();
}

void VideoController::PreviewToFrameLatest(int n) {
	if (!provider) return;
	StopIfAudioEnded();

	ClearInspectionStepState();
	ClearLatePreviewFrameAcceptance();

	bool const keep_playback_paused_for_preview = interactive_seek_phase != InteractiveSeekPhase::None;
	if (keep_playback_paused_for_preview)
		interactive_seek_phase = InteractiveSeekPhase::Previewing;
	bool was_playing = IsPlaying();
	auto resume_mode = playback_mode;
	auto resume_end_ms = playback_end_ms;
	if (was_playing)
		StopPlayback(!keep_playback_paused_for_preview);

	int const target_frame = mid(0, n, provider->GetFrameCount() - 1);
	// Audio positions have finer resolution than video frames. Small mouse
	// movements within one frame must not supersede its pending preview.
	if (keep_playback_paused_for_preview && target_frame == frame_n)
		return;
	frame_n = target_frame;
	perf_trace::TraceSeek(frame_n, was_playing);
	if (!TrySeekAndDeliverRecentRenderPacket(frame_n)) {
		RequestFrame(true);
		Seek(frame_n);
	}

	if (was_playing && !keep_playback_paused_for_preview && PreparePlayback(resume_mode, frame_n, resume_end_ms))
		StartPlaybackTimer();
}

void VideoController::BeginInteractiveSeekPreview() {
	StopIfAudioEnded();
	if (!provider || interactive_seek_phase != InteractiveSeekPhase::None)
		return;

	paused_prefetch_timer->Stop();
	provider->CancelFramePrefetch();
	interactive_seek_phase = InteractiveSeekPhase::Pressed;
	interactive_seek_preview_resume_mode = IsPlaying() ? playback_mode : PlaybackMode::None;
	interactive_seek_preview_resume_end_ms = playback_end_ms;
}

void VideoController::CommitInteractiveSeekPreviewToTime(int ms, agi::vfr::Time end) {
	if (!provider) {
		ClearInteractiveSeekPreviewState();
		return;
	}

	StopIfAudioEnded();
	int const target_frame = mid(0, FrameAtTime(ms, end), provider->GetFrameCount() - 1);
	if (IsPlaying()) {
		// A click without a preview keeps audio running and seeks exactly once.
		JumpToFrame(target_frame);
		return;
	}

	bool const resume_playback = interactive_seek_preview_resume_mode != PlaybackMode::None;
	auto const resume_mode = interactive_seek_preview_resume_mode;
	int const resume_end_ms = interactive_seek_preview_resume_end_ms;

	if (interactive_seek_phase != InteractiveSeekPhase::None && target_frame == frame_n) {
		// Releasing at the preview target commits the request already in flight.
		// Reissuing it would invalidate its result and make the playback-start
		// gate wait for a second render of the very same frame.
		ClearInteractiveSeekPreviewState();
	}
	else
		JumpToFrame(target_frame);
	if (resume_playback && PreparePlayback(resume_mode, frame_n, resume_end_ms))
		StartPlaybackTimer();
	else
		SchedulePausedFramePrefetch();
}

void VideoController::CancelInteractiveSeekPreview() {
	StopIfAudioEnded();
	if (interactive_seek_phase == InteractiveSeekPhase::None)
		return;

	bool const resume_playback = interactive_seek_phase == InteractiveSeekPhase::Previewing && interactive_seek_preview_resume_mode != PlaybackMode::None;
	auto const resume_mode = interactive_seek_preview_resume_mode;
	int const resume_end_ms = interactive_seek_preview_resume_end_ms;
	ClearInteractiveSeekPreviewState();

	if (provider && resume_playback && PreparePlayback(resume_mode, frame_n, resume_end_ms))
		StartPlaybackTimer();
	else
		SchedulePausedFramePrefetch();
}

void VideoController::JumpToTime(int ms, agi::vfr::Time end) {
	if (!provider) return;

	JumpToFrame(FrameAtTime(ms, end));
}

void VideoController::NavigateByFrames(int delta) {
	StepFrames(delta, false);
}

void VideoController::StepSingleFrame(int delta) {
	StepFrames(delta, playAudioOnStep->GetBool());
}

void VideoController::NextFrame() {
	StepSingleFrame(1);
}

void VideoController::PrevFrame() {
	StepSingleFrame(-1);
}

bool VideoController::PreparePlayback(PlaybackMode mode, int start_frame, int range_end_ms, bool keep_audio_playing) {
	if (!provider || mode == PlaybackMode::None)
		return false;

	paused_prefetch_timer->Stop();
	provider->CancelFramePrefetch();
	auto core = context->GetCore();
	start_ms = TimeAtFrame(start_frame);
	playback_mode = mode;
	playback_end_ms = range_end_ms;
	if (mode == PlaybackMode::LineRange) {
		end_frame = FrameAtTime(playback_end_ms, agi::vfr::END) + 1;
		if (start_ms >= playback_end_ms) {
			ResetPlaybackState();
			return false;
		}
	}
	else {
		end_frame = provider->GetFrameCount();
	}

	// Start the new audio clock only after its first video frame arrives. An
	// ongoing playback seek can keep the old audio running during that wait;
	// ResolvePendingPlaybackStart replaces it at the delivered frame's time.
	// Starting from pause still waits silently so cold reads cannot desync it.
	if (!keep_audio_playing && core.audioController->IsPlaying()) {
		core.audioController->Stop();
	}
	playback_uses_audio_authority = false;
	// Clamp to a frame the provider can actually deliver: PlayLine start
	// frames extrapolate past the video range for lines timed outside it,
	// and waiting on an undeliverable frame would hold the gate until the
	// fallback. Seek paths clamp the same way before requesting.
	playback_start_frame = mid(0, start_frame, provider->GetFrameCount() - 1);
	playback_start_pending = true;
	playback_pending_since = std::chrono::steady_clock::now();
	playback_start_wait_notice_shown = false;
	if (FrameAlreadyDelivered(playback_start_frame))
		ResolvePendingPlaybackStart();
	return true;
}

void VideoController::StartPlaybackTimer() {
	playback_timer->Start(10);
}

void VideoController::PrefetchFrame(int frame) noexcept {
	if (!provider || frame < 0 || frame >= provider->GetFrameCount())
		return;
	provider->PrefetchFrames(frame, 1);
}

void VideoController::PrimeNextPlaybackFrame() {
	if (playback_start_pending
		|| !provider
		|| frame_n != playback_start_frame
		|| presented_frame_n != playback_start_frame)
		return;

	int const next_frame = frame_n + 1;
	if (next_frame >= end_frame || next_frame >= provider->GetFrameCount())
		return;

	// Warm only the provider's source cache. Advancing frame_n here would make
	// the next playback tick request the start frame again until the audio clock
	// crosses its frame boundary.
	provider->PrefetchFrames(next_frame, 1);
	perf_trace::ObserveVideoUiDuration(
		"video_controller.playback_prime",
		0.0,
		playback_start_frame,
		next_frame);
}

void VideoController::ResolvePendingPlaybackStart() {
	if (!playback_start_pending)
		return;
	playback_start_pending = false;

	auto const now = std::chrono::steady_clock::now();
	auto const waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		now - playback_pending_since).count();
	HidePlaybackStartWaitNotice();

	auto core = context->GetCore();
	start_ms = TimeAtFrame(playback_start_frame);
	if (playback_mode == PlaybackMode::LineRange)
		core.audioController->PlayRange(TimeRange(start_ms, playback_end_ms));
	else
		core.audioController->PlayToEnd(start_ms);
	playback_start_time = std::chrono::steady_clock::now();
	playback_uses_audio_authority = core.audioController->IsPlaying();
	perf_trace::TracePlayStart(playback_start_frame, start_ms);
	perf_trace::ResetVideoPlaybackInterval();
	perf_trace::ObserveVideoUiDuration(
		"video_controller.playback_start_gate",
		static_cast<double>(waited_ms),
		playback_uses_audio_authority ? 1 : 0);
	PrimeNextPlaybackFrame();
}

bool VideoController::FrameAlreadyDelivered(int frame) const {
	// A recent packet is reusable for a paused seek, but it is not evidence
	// that a frame requested while playing has reached the display. Treating
	// the packet cache as delivered starts audio before the seek result arrives;
	// the late result then moves the controller back to the target frame.
	return presented_frame_n == frame;
}

void VideoController::ShowPlaybackStartWaitNotice() {
	playback_start_wait_notice_shown = true;
	context->ShowStatus(from_wx(_("Waiting for video frames...")), 3000);
}

void VideoController::HidePlaybackStartWaitNotice() {
	if (!playback_start_wait_notice_shown)
		return;
	playback_start_wait_notice_shown = false;
	context->ShowStatus("", 1000);
}

void VideoController::StartPlayback(PlaybackMode mode, int range_end_ms) {
	if (mode == PlaybackMode::ToEnd && presented_frame_n >= 0)
		frame_n = presented_frame_n;

	ClearInspectionStepState();
	ClearInteractiveSeekPreviewState();
	if (provider)
		provider->CancelPendingFrameRequests();
	ClearLatePreviewFrameAcceptance();

	if (!PreparePlayback(mode, frame_n, range_end_ms))
		return;

	if (playback_start_pending)
		RequestFrame();
	StartPlaybackTimer();
}

void VideoController::Play() {
	if (IsPlaying()) {
		Stop();
		return;
	}

	StartPlayback(PlaybackMode::ToEnd);
}

void VideoController::PlayLine() {
	Stop();
	auto core = context->GetCore();

	AssDialogue *curline = core.selectionController->GetActiveLine();
	if (!curline) return;

	// Round-trip conversion to convert start to exact
	int startFrame = FrameAtTime(curline->Start, agi::vfr::START);
	if (provider)
		provider->CancelPendingFrameRequests();
	if (!PreparePlayback(PlaybackMode::LineRange, startFrame, curline->End))
		return;

	if (playback_start_pending) {
		JumpToFrame(startFrame);
	}
	else {
		// PreparePlayback resolved the gate from the frame already on screen.
		// Keep the navigation notification, but do not request that frame again.
		frame_n = mid(0, startFrame, provider->GetFrameCount() - 1);
		context->GetCore().ass->Properties.video_position = frame_n;
		perf_trace::TraceSeek(frame_n, false);
		Seek(frame_n);
	}
	StartPlaybackTimer();
}

void VideoController::StopPlayback(bool clear_interactive_seek_preview, bool schedule_paused_prefetch) {
	ClearInspectionStepState();
	if (clear_interactive_seek_preview)
		ClearInteractiveSeekPreviewState();
	ClearLatePreviewFrameAcceptance();
	if (IsPlaying()) {
		perf_trace::TracePlayStop(frame_n);
		perf_trace::ResetVideoPlaybackInterval();
		playback_timer->Stop();
		playback_uses_audio_authority = false;
		auto core = context->GetCore();
		core.audioController->Stop();
	}
	HidePlaybackStartWaitNotice();
	ResetPlaybackState();
	if (schedule_paused_prefetch)
		SchedulePausedFramePrefetch();
}

void VideoController::Stop() {
	StopPlayback(true);
}

bool VideoController::StopIfAudioEnded() {
	auto core = context->GetCore();
	if (!IsPlaying() || !playback_uses_audio_authority || core.audioController->IsPlaying())
		return false;
	// Preserve a held gesture across natural completion, but honor explicit stops.
	StopPlayback(!core.audioController->IsPlaybackComplete());
	return true;
}

bool VideoController::IsPlaying() const {
	return playback_timer && playback_timer->IsRunning();
}

void VideoController::OnPlayTimer() {
	using namespace std::chrono;
	auto core = context->GetCore();
	if (playback_start_pending) {
		auto const waited = duration_cast<milliseconds>(
								steady_clock::now() - playback_pending_since)
								.count();
		if (!playback_start_wait_notice_shown && waited >= PlaybackStartWaitNoticeMs)
			ShowPlaybackStartWaitNotice();
		if (waited >= PlaybackStartFallbackMs)
			ResolvePendingPlaybackStart();
		return;
	}

	int authority_time_ms = start_ms + duration_cast<milliseconds>(steady_clock::now() - playback_start_time).count();
	if (playback_uses_audio_authority) {
		if (StopIfAudioEnded()) {
			return;
		}
		authority_time_ms = core.audioController->GetPlaybackPosition();
	}

	int next_frame = FrameAtTime(authority_time_ms);
	perf_trace::ObserveVideoPlaybackTick(next_frame);

	bool const reached_end = next_frame >= end_frame;
	if (reached_end)
		next_frame = end_frame - 1;

	if (next_frame != frame_n) {
		frame_n = next_frame;
		RequestFrame();
		Seek(frame_n);
		PlaybackFrameAdvanced(frame_n);
	}

	if (reached_end)
		StopPlayback(false);
}

void VideoController::SchedulePausedFramePrefetch() {
	if (!provider || IsPlaying() || playback_start_pending || interactive_seek_phase != InteractiveSeekPhase::None)
		return;

	paused_prefetch_timer->Stop();
	paused_prefetch_timer->StartOnce(PausedPrefetchDebounceMs);
}

void VideoController::OnPausedPrefetchTimer() {
	if (!provider || IsPlaying() || playback_start_pending || interactive_seek_phase != InteractiveSeekPhase::None)
		return;

	// Only warm ahead of a frame that actually made it to the display, so a
	// still-in-flight seek cannot prefetch from a stale position.
	if (presented_frame_n != frame_n)
		return;

	int const frame_count = provider->GetFrameCount();
	if (frame_n + 1 >= frame_count)
		return;

	int const lookahead_frame = FrameAtTime(TimeAtFrame(frame_n) + PausedPrefetchLookaheadMs);
	int const count = std::min(
		std::min(lookahead_frame, frame_count - 1) - frame_n,
		PausedPrefetchMaxFrames);
	if (count > 0)
		provider->PrefetchFrames(frame_n + 1, count);
}

double VideoController::GetARFromType(AspectRatio type) const {
	switch (type) {
		case AspectRatio::Default:    return (double)provider->GetWidth()/provider->GetHeight();
		case AspectRatio::Fullscreen: return 4.0/3.0;
		case AspectRatio::Widescreen: return 16.0/9.0;
		case AspectRatio::Cinematic:  return 2.35;
        default: throw agi::InternalError("Bad AR type");
	}
}

void VideoController::SetAspectRatio(double value) {
	ar_type = AspectRatio::Custom;
	ar_value = mid(.5, value, 5.);
	auto core = context->GetCore();
	core.ass->Properties.ar_mode = (int)ar_type;
	core.ass->Properties.ar_value = ar_value;
	ARChange(ar_type, ar_value);
}

void VideoController::SetAspectRatio(AspectRatio type) {
	ar_value = mid(.5, GetARFromType(type), 5.);
	ar_type = type;
	auto core = context->GetCore();
	core.ass->Properties.ar_mode = (int)ar_type;
	core.ass->Properties.ar_value = ar_value;
	ARChange(ar_type, ar_value);
}

int VideoController::TimeAtFrame(int frame, agi::vfr::Time type) const {
	auto core = context->GetCore();
	return core.project->Timecodes().TimeAtFrame(frame, type);
}

int VideoController::FrameAtTime(int time, agi::vfr::Time type) const {
	auto core = context->GetCore();
	return core.project->Timecodes().FrameAtTime(time, type);
}

void VideoController::HandleVideoError(std::string const& message) {
	if (playback_start_pending || IsPlaying())
		StopPlayback(true, false);
	else {
		ClearInspectionStepState();
		ClearInteractiveSeekPreviewState();
	}
	ClearRecentRenderPacketCache();
	LOG_E("video_controller") << "Failed seeking video. The video file may be corrupt or incomplete. Error: " << message;
}

void VideoController::HandleSubtitlesError(std::string const& message) {
	if (playback_start_pending || IsPlaying())
		StopPlayback(true, false);
	else {
		ClearInspectionStepState();
		ClearInteractiveSeekPreviewState();
	}
	ClearRecentRenderPacketCache();
	LOG_E("video_controller") << "Failed rendering subtitles. Error: " << message;
}

void VideoController::RememberRecentRenderPacket(VideoRenderPacket const& packet) {
	if (packet.frame_number < 0)
		return;

	for (auto it = recent_render_packets.begin(); it != recent_render_packets.end(); ++it) {
		if (it->frame_number == packet.frame_number) {
			recent_render_packets.erase(it);
			break;
		}
	}

	recent_render_packets.push_front(packet);
	constexpr size_t max_recent_packets = 8;
	while (recent_render_packets.size() > max_recent_packets)
		recent_render_packets.pop_back();
}

void VideoController::ClearRecentRenderPacketCache() {
	recent_render_packets.clear();
}

bool VideoController::TrySeekAndDeliverRecentRenderPacket(int frame) {
	for (auto it = recent_render_packets.begin(); it != recent_render_packets.end(); ++it) {
		if (it->frame_number != frame)
			continue;

		perf_trace::ObserveVideoRenderPacketCacheLookup(frame, true, "recent_render_packet");
		auto packet = *it;
		recent_render_packets.erase(it);
		recent_render_packets.push_front(packet);
		double const packet_time = packet.time;
		context->GetCore().ass->Properties.video_position = frame;
		if (provider) {
			provider->CancelPendingFrameRequests();
			provider->SetCurrentFrameContext(frame, packet_time);
		}
		Seek(frame);
		DeliverFrameReady(std::move(packet), packet_time);
		return true;
	}

	perf_trace::ObserveVideoRenderPacketCacheLookup(frame, false, "recent_render_packet");
	return false;
}

void VideoController::DeliverFrameReady(VideoRenderPacket packet, double time) {
	if (packet.frame_number != frame_n) {
		if (!accept_late_preview_frames || !acceptable_late_preview_frames.count(packet.frame_number))
			return;
		acceptable_late_preview_frames.erase(packet.frame_number);
		frame_n = packet.frame_number;
		context->GetCore().ass->Properties.video_position = frame_n;
		Seek(frame_n);
	}
	RememberRecentRenderPacket(packet);
	FrameReady(packet, time);
	// Audio startup synchronously prepares output buffers and notifies the
	// spectrum display. Present the ready video before waiting for that work.
	if (playback_start_pending && packet.frame_number == playback_start_frame)
		ResolvePendingPlaybackStart();
}

void VideoController::NotifyFramePresented(int frame_number) {
	presented_frame_n = frame_number;
	FramePresented(frame_number);
	PrimeNextPlaybackFrame();
	if (inspection_step_in_flight && frame_number == inspection_step_frame) {
		inspection_step_in_flight = false;
		inspection_step_frame = -1;
		RequestPendingInspectionStepTarget();
	}
	SchedulePausedFramePrefetch();
}

void VideoController::RequestPendingInspectionStepTarget() {
	if (!has_pending_inspection_step) {
		inspection_step_anchor_frame = frame_n;
		return;
	}

	int const target = pending_inspection_step_frame;
	bool const play_audio = pending_inspection_step_play_audio;
	int const delta = pending_inspection_step_delta;
	has_pending_inspection_step = false;
	pending_inspection_step_frame = -1;
	pending_inspection_step_play_audio = false;
	pending_inspection_step_delta = 0;

	if (!provider || IsPlaying() || target == frame_n) {
		inspection_step_anchor_frame = frame_n;
		return;
	}

	RequestInspectionStepTarget(target, true, play_audio, delta);
}

void VideoController::InvalidateRenderPacketCache() {
	ClearInspectionStepState();
	ClearInteractiveSeekPreviewState();
	ClearRecentRenderPacketCache();
}

AsyncVideoProviderEventSink VideoController::CreateAsyncVideoProviderEventSink() {
	return CreateAsyncVideoProviderMainThreadSink(
		GetAsyncUiLifetime(),
		{.on_frame_ready = [this](VideoRenderPacket packet, double time) {
			 aegisub::async_video_trace::ObservePipelineEvent({.stage = "gui_receive", .version = packet.delivery_version, .delivery_class = packet.delivery_class, .visual_interaction_id = packet.visual_interaction_id, .frame = packet.frame_number});
			 if (!provider || !provider->IsCurrent(packet.delivery_version)) {
				 aegisub::async_video_trace::ObservePipelineEvent({.stage = "gui_stale", .version = packet.delivery_version, .delivery_class = packet.delivery_class, .visual_interaction_id = packet.visual_interaction_id, .frame = packet.frame_number});
				 perf_trace::ObserveVideoUiDuration(
					 "video_controller.frame_ready_stale",
					 0.0,
					 packet.frame_number);
				 return;
			 }
			 aegisub::async_video_trace::ObservePipelineEvent({.stage = "gui_accept", .version = packet.delivery_version, .delivery_class = packet.delivery_class, .visual_interaction_id = packet.visual_interaction_id, .frame = packet.frame_number});
			 DeliverFrameReady(std::move(packet), time); },
		 .on_video_error = [this](std::string const& message) { HandleVideoError(message); },
		 .on_subtitles_error = [this](std::string const& message) { HandleSubtitlesError(message); }},
		AsyncVideoFrameDeliveryMode::VisualSubtitleBatches);
}
