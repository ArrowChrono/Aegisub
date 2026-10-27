// Copyright (c) 2009-2010, Niels Martin Hansen
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

#include "audio_controller.h"

#include "audio_controller_timer.h"
#include "audio_timing.h"
#include "include/aegisub/audio_player.h"
#include "include/aegisub/context.h"
#include "include/aegisub/context_ui.h"
#include "options.h"
#include "perf_trace.h"
#include "project.h"
#include "ui_services.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/log.h>

#include <algorithm>

AudioController::AudioController(agi::Context *context)
: context(context)
, playback_timer(CreateAudioControllerTimer([this] { OnPlaybackTimer(); }))
, provider_connection(context->GetCore().project->AddAudioProviderListener(&AudioController::OnAudioProvider, this))
{
#ifdef wxHAS_POWER_EVENTS
	Bind(wxEVT_POWER_SUSPENDED, &AudioController::OnComputerSuspending, this);
	Bind(wxEVT_POWER_RESUME, &AudioController::OnComputerResuming, this);
#endif

	OPT_SUB("Audio/Player", &AudioController::OnAudioPlayerChanged, this);
}

AudioController::~AudioController()
{
	Stop();
}

void AudioController::OnPlaybackTimer() {
	if (!player) return;
	perf_trace::AudioUiDurationScope trace("audio_controller.playback_timer");

	int64_t pos = player->GetCurrentPosition();
	int pos_ms = -1;
	auto const trace_mode = playback_mode;
	if (trace.IsActive() && provider) {
		pos_ms = MillisecondsFromSamples(pos);
		perf_trace::ObserveAudioUiTimerPosition(pos_ms);
	}
	if (!player->IsPlaying() ||
		(playback_mode != PM_ToEnd && pos >= player->GetEndPosition()+200))
	{
		// The +200 is to allow the player to end the sound output cleanly,
		// otherwise a popping artifact can sometimes be heard.
		StopPlayback(player->NeedsDeviceRecovery() ? PM_NotPlaying : PM_Completed);
	}
	else
	{
		if (pos_ms < 0)
			pos_ms = MillisecondsFromSamples(pos);
		AnnouncePlaybackPosition(pos_ms);
	}

	trace.SetDetails(pos_ms, static_cast<int>(trace_mode));
}

#ifdef wxHAS_POWER_EVENTS
void AudioController::OnComputerSuspending(wxPowerEvent &)
{
	Stop();
	player.reset();
}

void AudioController::OnComputerResuming(wxPowerEvent &)
{
	OnAudioPlayerChanged();
}
#endif

void AudioController::OnAudioPlayerChanged()
{
	if (!provider) return;

	Stop();
	player.reset();
	auto core = context->GetCore();

	try
	{
		if (core.audioPlayerFactoryService)
			player = core.audioPlayerFactoryService->CreateAudioPlayer(provider);
	}
	catch (...)
	{
		LOG_W("audio/controller") << "OnAudioPlayerChanged caught exception from CreateAudioPlayer";
		/// @todo This really shouldn't be just swallowing all audio player open errors
		core.project->CloseAudio();
	}
	if (!player) {
		LOG_W("audio/controller") << "OnAudioPlayerChanged no player; closing audio";
		core.project->CloseAudio();
		return;
	}
	AnnounceAudioPlayerOpened();
}

void AudioController::OnAudioProvider(agi::AudioProvider *new_provider)
{
	provider = new_provider;
	Stop();
	player.reset();
	OnAudioPlayerChanged();
}

void AudioController::SetTimingController(std::unique_ptr<AudioTimingController> new_controller)
{
	timing_controller = std::move(new_controller);
	if (timing_controller)
		timing_controller->AddUpdatedPrimaryRangeListener(&AudioController::OnTimingControllerUpdatedPrimaryRange, this);

	AnnounceTimingControllerChanged();
}

void AudioController::OnTimingControllerUpdatedPrimaryRange()
{
	if (playback_mode == PM_PrimaryRange)
		player->SetEndPosition(SamplesFromMilliseconds(timing_controller->GetPrimaryPlaybackRange().end()));
}

void AudioController::PlayRange(const TimeRange &range)
{
	if (player && player->NeedsDeviceRecovery())
		RecoverAudioPlayerAfterDeviceChange();
	if (!player || !provider) return;
	perf_trace::AudioUiDurationScope trace("audio_controller.play_range");

	int64_t const start_sample = SamplesFromMilliseconds(range.begin());
	int64_t const sample_count = SamplesFromMilliseconds(range.length());
	if (sample_count <= 0) {
		trace.Cancel();
		Stop();
		return;
	}

	perf_trace::ResetAudioUiTimerInterval();
	player->Play(start_sample, sample_count);
	playback_mode = PM_Range;
	playback_timer->Start(20);

	AnnouncePlaybackPosition(range.begin());
	trace.SetDetails(range.length(), range.begin());
}

void AudioController::PlayPrimaryRange()
{
	PlayRange(GetPrimaryPlaybackRange());
	if (playback_mode == PM_Range)
		playback_mode = PM_PrimaryRange;
}

void AudioController::PlayToEndOfPrimary(int start_ms)
{
	auto range = GetPrimaryPlaybackRange();
	if (start_ms >= range.end()) {
		Stop();
		return;
	}

	PlayRange(TimeRange(start_ms, range.end()));
	if (playback_mode == PM_Range)
		playback_mode = PM_PrimaryRange;
}

void AudioController::PlayToEnd(int start_ms)
{
	if (player && player->NeedsDeviceRecovery())
		RecoverAudioPlayerAfterDeviceChange();
	if (!player || !provider) return;
	perf_trace::AudioUiDurationScope trace("audio_controller.play_to_end");

	int64_t start_sample = SamplesFromMilliseconds(start_ms);
	int64_t sample_count = provider->GetNumSamples() - start_sample;
	if (sample_count <= 0) {
		trace.Cancel();
		Stop();
		return;
	}

	perf_trace::ResetAudioUiTimerInterval();
	player->Play(start_sample, sample_count);
	playback_mode = PM_ToEnd;
	playback_timer->Start(20);

	AnnouncePlaybackPosition(start_ms);
	trace.SetDetails(start_ms);
}

void AudioController::Stop() {
	StopPlayback(PM_NotPlaying);
}

void AudioController::StopPlayback(PlaybackMode stopped_mode) {
	if (!player) return;
	perf_trace::AudioUiDurationScope trace("audio_controller.stop");

	player->Stop();
	playback_mode = stopped_mode;
	playback_timer->Stop();
	perf_trace::ResetAudioUiTimerInterval();

	AnnouncePlaybackStop();
}

bool AudioController::IsPlaying()
{
	return player && playback_mode != PM_NotPlaying && playback_mode != PM_Completed;
}

void AudioController::RecoverAudioPlayerAfterDeviceChange() {
	if (!provider) {
		LOG_D("audio/player/xaudio2/recovery") << "Ignoring queued XAudio2 recovery because no audio provider is loaded.";
		return;
	}
	if (!player || !player->NeedsDeviceRecovery()) {
		LOG_D("audio/player/xaudio2/recovery") << "Ignoring queued XAudio2 recovery because the output has no critical error.";
		return;
	}

	LOG_I("audio/player/xaudio2/recovery") << "Rebuilding XAudio2 audio player after a critical output error.";
	OnAudioPlayerChanged();
}

int AudioController::GetPlaybackPosition()
{
	if (!IsPlaying()) return 0;

	return MillisecondsFromSamples(player->GetCurrentPosition());
}

int AudioController::GetDuration() const
{
	if (!provider) return 0;
	return (provider->GetNumSamples() * 1000 + provider->GetSampleRate() - 1) / provider->GetSampleRate();
}

TimeRange AudioController::GetPrimaryPlaybackRange() const
{
	if (timing_controller)
		return timing_controller->GetPrimaryPlaybackRange();
	else
		return TimeRange{0, 0};
}

void AudioController::SetVolume(double volume)
{
	if (!player) return;
	player->SetVolume(volume);
}

int64_t AudioController::SamplesFromMilliseconds(int64_t ms) const
{
	if (!provider) return 0;
	return (ms * provider->GetSampleRate() + 999) / 1000;
}

int64_t AudioController::MillisecondsFromSamples(int64_t samples) const
{
	if (!provider) return 0;
	return samples * 1000 / provider->GetSampleRate();
}
