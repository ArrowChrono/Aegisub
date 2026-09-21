// Copyright (c) 2019, Qirui Wang
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

#ifdef WITH_XAUDIO2
#include "include/aegisub/audio_player.h"

#include "audio_player_xaudio2_buffer_slots.h"
#include "audio_player_xaudio2_playback.h"
#include "options.h"
#include "perf_trace.h"

#include <libaegisub/audio/provider.h>
#include <libaegisub/scoped_ptr.h>
#include <libaegisub/log.h>
#include <libaegisub/make_unique.h>

#include <atomic>
#include <chrono>
#include <mutex>

#ifndef XAUDIO2_REDIST
#include <xaudio2.h>
#else
#include <xaudio2redist.h>
#endif

namespace {
class XAudio2Thread;

/// @class XAudio2Player
/// @brief XAudio2-based audio player
///
/// The core design idea is to have a playback thread that performs all playback operations, and use the player object as a proxy to send commands to the playback thread.
class XAudio2Player final : public AudioPlayer {
	/// The playback thread
	std::unique_ptr<XAudio2Thread> thread;

	/// Desired length in milliseconds to write ahead of the playback cursor
	int WantedLatency;

	/// Multiplier for WantedLatency to get total buffer length
	int BufferLength;

	/// @brief Tell whether playback thread is alive
	/// @return True if there is a playback thread and it's ready
	bool IsThreadAlive();

public:
	/// @brief Constructor
	XAudio2Player(agi::AudioProvider* provider);
	/// @brief Destructor
	~XAudio2Player() = default;

	/// @brief Start playback
	/// @param start First audio frame to play
	/// @param count Number of audio frames to play
	void Play(int64_t start, int64_t count);

	/// @brief Stop audio playback
	/// @param timerToo Whether to also stop the playback update timer
	void Stop();

	/// @brief Tell whether playback is active
	/// @return True if audio is playing back
	bool IsPlaying();

	/// @brief Get playback end position
	/// @return Audio frame index
	///
	/// Returns 0 if playback is stopped or there is no playback thread
	int64_t GetEndPosition();
	/// @brief Get approximate playback position
	/// @return Index of audio frame user is currently hearing
	///
	/// Returns 0 if playback is stopped or there is no playback thread
	int64_t GetCurrentPosition();

	/// @brief Change playback end position
	/// @param pos New end position
	void SetEndPosition(int64_t pos);

	/// @brief Change playback volume
	/// @param vol Amplification factor
	void SetVolume(double vol);
};

/// @brief RAII support class to init and de-init the COM library
struct COMInitialization {

	/// Flag set if an inited COM library is managed
	bool inited = false;

	/// @brief Destructor, de-inits COM if it is inited
	~COMInitialization() {
		if (inited) CoUninitialize();
	}

	/// @brief Initialise the COM library as single-threaded apartment if isn't already inited by us
	bool Init() {
		if (!inited && SUCCEEDED(CoInitialize(nullptr)))
			inited = true;
		return inited;
	}
};

struct ReleaseCOMObject {
	void operator()(IUnknown* obj) {
		if (obj) obj->Release();
	}
};

struct DestroyXAudio2Voice {
	void operator()(IXAudio2Voice* voice) {
		if (voice) voice->DestroyVoice();
	}
};

/// @brief RAII wrapper around Win32 HANDLE type
struct Win32KernelHandle final : public agi::scoped_holder<HANDLE, BOOL(__stdcall*)(HANDLE)> {
	/// @brief Create with a managed handle
	/// @param handle Win32 handle to manage
	Win32KernelHandle(HANDLE handle = 0) :scoped_holder(handle, CloseHandle) {}

	Win32KernelHandle& operator=(HANDLE new_handle) {
		scoped_holder::operator=(new_handle);
		return *this;
	}
};

/// @class XAudio2Thread
/// @brief Playback thread class for XAudio2Player
///
/// Not based on wxThread, but uses Win32 threads directly
class XAudio2Thread :public IXAudio2VoiceCallback {
	/// @brief Win32 thread entry point
	/// @param parameter Pointer to our thread object
	/// @return Thread return value, always 0 here
	static unsigned int __stdcall ThreadProc(void* parameter);
	/// @brief Thread entry point
	void Run();

	/// @brief Check for error state and throw exception if one occurred
	void CheckError();

	/// Win32 handle to the thread
	Win32KernelHandle thread_handle;

	/// Event object, world to thread, set to start playback
	Win32KernelHandle event_start_playback;

	/// Event object, world to thread, set to stop playback
	Win32KernelHandle event_stop_playback;

	/// Event object, world to thread, set if playback end time was updated
	Win32KernelHandle event_update_end_time;

	/// Event object, world to thread, set if the volume was changed
	Win32KernelHandle event_set_volume;

	/// Event object, world to thread, set if the thread should end as soon as possible
	Win32KernelHandle event_buffer_end;

	/// Event object, XAudio2 callback to thread, set after the EOS buffer has played
	Win32KernelHandle event_stream_end;

	/// Event object, XAudio2 callback to thread, set if the source voice reports an error
	Win32KernelHandle event_voice_error;

	/// Event object, world to thread, set if the thread should end as soon as possible
	Win32KernelHandle event_kill_self;

	/// Event object, thread to world, set when the thread has entered its main loop
	Win32KernelHandle thread_running;

	/// Event object, thread to world, set when playback is ongoing
	Win32KernelHandle is_playing;

	/// Event object, thread to world, set when a play request has been handled
	Win32KernelHandle playback_request_done;

	/// Monotonic request IDs prevent a timed-out start from running later
	std::atomic<uint64_t> playback_request_generation{0};
	std::atomic<uint64_t> cancelled_playback_generation{0};
	std::atomic<uint64_t> completed_playback_generation{0};

	/// Event object, thread to world, set if an error state has occurred (implies thread is dying)
	Win32KernelHandle error_happened;

	/// Statically allocated error message text describing reason for error_happened being set
	const char* error_message = nullptr;

	/// Playback volume, 1.0 is "unchanged"
	std::atomic<double> volume{1.0};

	/// Audio frame to start playback at
	std::atomic<int64_t> start_frame{0};

	/// Audio frame to end playback at
	std::atomic<int64_t> end_frame{0};

	/// Desired length in milliseconds to write ahead of the playback cursor
	int wanted_latency;

	/// Device-consumed progress with bounded high-resolution interpolation.
	std::mutex playback_clock_mutex;
	aegisub::xaudio2::PlaybackClock playback_clock;

	/// Audio provider to take sample data from
	agi::AudioProvider* provider;

	/// Two banks retain flushed audio until its callbacks release the slots.
	XAudio2BufferSlots buffer_slots;

	/// HRESULT supplied by the most recent OnVoiceError callback
	std::atomic<HRESULT> voice_error{S_OK};

public:
	/// @brief Constructor, creates and starts playback thread
	/// @param provider       Audio provider to take sample data from
	/// @param WantedLatency Desired length in milliseconds to write ahead of the playback cursor
	/// @param BufferLength  Multiplier for WantedLatency to get total buffer length
	XAudio2Thread(agi::AudioProvider* provider, int WantedLatency, int BufferLength);
	/// @brief Destructor, waits for thread to have died
	~XAudio2Thread();

	// IXAudio2VoiceCallback
	void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 BytesRequired) override {}
	void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
	void STDMETHODCALLTYPE OnStreamEnd() override {
		SetEvent(event_stream_end);
	}
	void STDMETHODCALLTYPE OnBufferStart(void* pBufferContext) override {}
	void STDMETHODCALLTYPE OnBufferEnd(void* pBufferContext) override {
		intptr_t i = reinterpret_cast<intptr_t>(pBufferContext);
		if (i >= 0 && i < buffer_slots.SlotCount()) {
			buffer_slots.Release(static_cast<int>(i));
		}
		SetEvent(event_buffer_end);
	}
	void STDMETHODCALLTYPE OnLoopEnd(void* pBufferContext) override {}
	void STDMETHODCALLTYPE OnVoiceError(void* pBufferContext, HRESULT Error) override {
		voice_error.store(Error, std::memory_order_release);
		SetEvent(event_voice_error);
	}

	/// @brief Start audio playback
	/// @param start Audio frame to start playback at
	/// @param count Number of audio frames to play
	void Play(int64_t start, int64_t count);

	/// @brief Stop audio playback
	void Stop();

	/// @brief Change audio playback end point
	/// @param new_end_frame New last audio frame to play
	///
	/// Playback stops instantly if new_end_frame is before the current playback position
	void SetEndFrame(int64_t new_end_frame);

	/// @brief Change audio playback volume
	/// @param new_volume New playback amplification factor, 1.0 is "unchanged"
	void SetVolume(double new_volume);

	/// @brief Tell whether audio playback is active
	/// @return True if audio is being played back, false if it is not
	bool IsPlaying();

	/// @brief Get approximate current audio frame being heard by the user
	/// @return Audio frame index
	///
	/// Returns 0 if not playing
	int64_t GetCurrentFrame();

	/// @brief Get audio playback end point
	/// @return Audio frame index
	int64_t GetEndFrame();

	/// @brief Tell whether playback thread has died
	/// @return True if thread is no longer running
	bool IsDead();
};

unsigned int __stdcall XAudio2Thread::ThreadProc(void* parameter) {
	auto *thread = static_cast<XAudio2Thread*>(parameter);
	try {
		thread->Run();
	}
	catch (...) {
		ResetEvent(thread->is_playing);
		thread->error_message = "XAudio2Thread: unhandled exception in playback thread";
		SetEvent(thread->error_happened);
	}
	ResetEvent(thread->thread_running);
	return 0;
}

/// Macro used to set error_message, error_happened and end the thread
#define REPORT_ERROR(msg) \
{ \
	ResetEvent(is_playing); \
	error_message = "XAudio2Thread: " msg; \
	SetEvent(error_happened); \
	return; \
}

void XAudio2Thread::Run() {
	COMInitialization COM_library;
	if (!COM_library.Init()) {
		REPORT_ERROR("Could not initialise COM")
	}
	IXAudio2* xaudio2_raw = nullptr;
	HRESULT hr;
	if (FAILED(hr = XAudio2Create(&xaudio2_raw, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
		REPORT_ERROR("Failed initializing XAudio2")
	}
	std::unique_ptr<IXAudio2, ReleaseCOMObject> xaudio2(xaudio2_raw);
	auto *pXAudio2 = xaudio2.get();

	IXAudio2MasteringVoice* master_voice_raw = nullptr;
	if (FAILED(hr = pXAudio2->CreateMasteringVoice(&master_voice_raw))) {
		REPORT_ERROR("Failed initializing XAudio2 MasteringVoice")
	}
	std::unique_ptr<IXAudio2MasteringVoice, DestroyXAudio2Voice> master_voice(master_voice_raw);
	auto *pMasterVoice = master_voice.get();

	// Describe the wave format
	WAVEFORMATEX wfx;
	wfx.nSamplesPerSec = provider->GetSampleRate();
	wfx.cbSize = 0;
	bool original = true;
	wfx.wFormatTag = provider->AreSamplesFloat() ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
	wfx.nChannels = provider->GetChannels();
	wfx.wBitsPerSample = provider->GetBytesPerSample() * 8;
	wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

	// The source voice must be destroyed before storage still referenced by
	// flushed buffers is released, including on error/early-return paths.
	std::vector<std::vector<BYTE>> buff(buffer_slots.SlotCount());
	IXAudio2SourceVoice* source_voice_raw = nullptr;
	if (FAILED(hr = pXAudio2->CreateSourceVoice(&source_voice_raw, &wfx, 0, 2, this))) {
		if (hr == XAUDIO2_E_INVALID_CALL) {
			// Retry with 16bit mono
			original = false;
			wfx.wFormatTag = WAVE_FORMAT_PCM;
			wfx.nChannels = 1;
			wfx.wBitsPerSample = sizeof(int16_t) * 8;
			wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
			wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
			source_voice_raw = nullptr;
			if (FAILED(hr = pXAudio2->CreateSourceVoice(&source_voice_raw, &wfx, 0, 2, this))) {
				REPORT_ERROR("Failed initializing XAudio2 SourceVoice")
			}
		}
		else {
			REPORT_ERROR("Failed initializing XAudio2 SourceVoice")
		}
	}
	std::unique_ptr<IXAudio2SourceVoice, DestroyXAudio2Voice> source_voice(source_voice_raw);
	auto *pSourceVoice = source_voice.get();

	// Now we're ready to roll!
	SetEvent(thread_running);
	bool running = true;

	enum EventIndex {
		EventStartPlayback,
		EventStopPlayback,
		EventUpdateEndTime,
		EventSetVolume,
		EventBufferEnd,
		EventStreamEnd,
		EventVoiceError,
		EventKillSelf,
		EventCount
	};
	HANDLE events_to_wait[EventCount] = {
		event_start_playback,
		event_stop_playback,
		event_update_end_time,
		event_set_volume,
		event_buffer_end,
		event_stream_end,
		event_voice_error,
		event_kill_self
	};

	int64_t next_input_frame = 0;
	int64_t playback_begin_frame = 0;
	int64_t playback_end_frame = 0;
	bool playback_should_be_running = false;
	bool playback_start_pending = false;
	bool playback_has_buffer = false;
	uint64_t playback_generation = 0;
	uint64_t playback_sample_base = 0;
	const int wanted_frames = std::max(1, wanted_latency * static_cast<int>(wfx.nSamplesPerSec) / 1000);
	const DWORD wanted_latency_bytes = wanted_frames * wfx.nBlockAlign;
	for (auto& i : buff)
		i.resize(wanted_latency_bytes);

	XAUDIO2_VOICE_DETAILS mastering_details{};
	pMasterVoice->GetVoiceDetails(&mastering_details);
	bool const trace_audio_output = perf_trace::IsCategoryEnabled(perf_trace::Category::Audio);
	bool output_starved = false;
	auto consumed_frame = [&](XAUDIO2_VOICE_STATE const& state) {
		return aegisub::xaudio2::ConsumedFrame(playback_begin_frame, next_input_frame,
											   playback_sample_base, state.SamplesPlayed,
											   !playback_start_pending && next_input_frame >= playback_end_frame && state.BuffersQueued == 0);
	};
	auto publish_position = [&](bool advancing = true) {
		XAUDIO2_VOICE_STATE state{};
		pSourceVoice->GetState(&state);
		std::scoped_lock lock(playback_clock_mutex);
		playback_clock.Publish(consumed_frame(state), next_input_frame, std::chrono::steady_clock::now(),
							   advancing && !playback_start_pending && state.BuffersQueued > 0);
	};
	auto emit_audio_output = [&](char const* reason, int64_t submitted_frames = -1,
		int64_t submitted_bytes = -1, double fill_duration_ms = -1.0,
		bool track_queue_health = false) {
		if (!trace_audio_output)
			return;

		XAUDIO2_VOICE_STATE voice_state{};
		pSourceVoice->GetState(&voice_state);
		XAUDIO2_PERFORMANCE_DATA performance{};
		pXAudio2->GetPerformanceData(&performance);

		bool const starved = track_queue_health
			&& playback_should_be_running
			&& next_input_frame < playback_end_frame
			&& voice_state.BuffersQueued == 0;
		bool const recovered = track_queue_health
			&& output_starved
			&& voice_state.BuffersQueued > 0;
		if (starved)
			output_starved = true;
		else if (recovered)
			output_starved = false;

		perf_trace::AudioOutputSnapshot snapshot;
		snapshot.backend_name = "xaudio2";
		snapshot.reason = reason ? reason : "";
		snapshot.queued_buffers = voice_state.BuffersQueued;
		snapshot.queued_ms = static_cast<double>(voice_state.BuffersQueued)
			* static_cast<double>(wanted_frames) * 1000.0
			/ static_cast<double>(wfx.nSamplesPerSec);
		snapshot.submitted_buffers = submitted_frames > 0 ? 1 : 0;
		snapshot.submitted_frames = submitted_frames;
		snapshot.submitted_bytes = submitted_bytes;
		snapshot.submitted_ms = submitted_frames >= 0
			? static_cast<double>(submitted_frames) * 1000.0 / static_cast<double>(wfx.nSamplesPerSec)
			: -1.0;
		snapshot.fill_duration_ms = fill_duration_ms;
		snapshot.played_frames = static_cast<int64_t>(voice_state.SamplesPlayed);
		snapshot.engine_latency_frames = performance.CurrentLatencyInSamples;
		snapshot.glitch_count = performance.GlitchesSinceEngineStarted;
		snapshot.source_rate_hz = static_cast<int>(wfx.nSamplesPerSec);
		snapshot.mastering_rate_hz = static_cast<int>(mastering_details.InputSampleRate);
		snapshot.low_water = voice_state.BuffersQueued <= 1 && next_input_frame < playback_end_frame;
		snapshot.starved = starved;
		snapshot.recovered = recovered;
		snapshot.end_of_stream = next_input_frame >= playback_end_frame && voice_state.BuffersQueued == 0;
		perf_trace::ObserveAudioOutputSnapshot(snapshot);
	};

	while (running) {
		DWORD wait_result = WaitForMultipleObjects(sizeof(events_to_wait) / sizeof(HANDLE), events_to_wait, FALSE,
												   playback_should_be_running ? 5 : INFINITE);

		switch (wait_result) {
		case WAIT_OBJECT_0 + EventStartPlayback:
			// Old slots stay owned by XAudio2 until their asynchronous OnBufferEnd
			// callbacks arrive. Prepare this request in the other bank so a normal
			// seek need not block the UI for that callback/engine processing pass.
			emit_audio_output("restart_begin");
			ResetEvent(is_playing);
			// Stop alone is asynchronous. Freezing this player's private engine
			// commits pending operations and makes the stop/flush counter reset
			// synchronous before a new stream is submitted.
			pXAudio2->StopEngine();
			if (FAILED(hr = pSourceVoice->Stop()))
				REPORT_ERROR("Failed stopping XAudio2 SourceVoice before playback")
			if (FAILED(hr = pSourceVoice->FlushSourceBuffers()))
				REPORT_ERROR("Failed flushing XAudio2 SourceVoice before playback")
			ResetEvent(event_stream_end);
			buffer_slots.BeginPlayback();
			emit_audio_output("restart_flushed");

			playback_begin_frame = start_frame.load(std::memory_order_acquire);
			next_input_frame = playback_begin_frame;
			playback_end_frame = end_frame.load(std::memory_order_acquire);
			playback_generation = playback_request_generation.load(std::memory_order_acquire);
			playback_should_be_running = playback_end_frame > next_input_frame;
			playback_start_pending = playback_should_be_running;
			playback_has_buffer = false;
			{
				std::scoped_lock lock(playback_clock_mutex);
				playback_clock.Reset(playback_begin_frame, std::chrono::steady_clock::now());
			}
			if (!playback_should_be_running) {
				emit_audio_output("start_empty");
				completed_playback_generation.store(playback_generation, std::memory_order_release);
				SetEvent(playback_request_done);
				break;
			}
			goto do_fill_buffer;

		case WAIT_OBJECT_0 + EventStopPlayback:
		stop_playback:
			// Stop playing and invalidate any pending start before flushing.
			ResetEvent(is_playing);
			playback_should_be_running = false;
			playback_start_pending = false;
			playback_has_buffer = false;
			if (FAILED(hr = pSourceVoice->Stop()))
				REPORT_ERROR("Failed stopping XAudio2 SourceVoice")
			if (FAILED(hr = pSourceVoice->FlushSourceBuffers()))
				REPORT_ERROR("Failed flushing XAudio2 SourceVoice")
			emit_audio_output("stop");
			completed_playback_generation.store(playback_generation, std::memory_order_release);
			SetEvent(playback_request_done);
			break;

		case WAIT_OBJECT_0 + EventUpdateEndTime:
			if (!playback_should_be_running)
				break;
			{
				auto const new_end = end_frame.load(std::memory_order_acquire);
				XAUDIO2_VOICE_STATE state{};
				pSourceVoice->GetState(&state);
				auto const action = aegisub::xaudio2::DecideEndUpdate(consumed_frame(state), next_input_frame, playback_end_frame, new_end);
				if (action == aegisub::xaudio2::EndUpdate::Unchanged)
					break;
				if (action == aegisub::xaudio2::EndUpdate::Stop) {
					playback_end_frame = new_end;
					goto stop_playback;
				}
				if (action == aegisub::xaudio2::EndUpdate::Rebuild) {
					pXAudio2->StopEngine();
					if (FAILED(hr = pSourceVoice->Stop()))
						REPORT_ERROR("Failed stopping XAudio2 SourceVoice for end update")
					pSourceVoice->GetState(&state);
					auto const resume_frame = consumed_frame(state);
					playback_end_frame = new_end;
					if (resume_frame >= new_end)
						goto stop_playback;
					// Do not publish a stopped transport while replacing queued PCM.
					// The controller treats a transient !IsPlaying as completion.
					{
						std::scoped_lock lock(playback_clock_mutex);
						playback_clock.Publish(resume_frame, new_end, std::chrono::steady_clock::now(), false);
					}
					if (FAILED(hr = pSourceVoice->FlushSourceBuffers()))
						REPORT_ERROR("Failed flushing XAudio2 SourceVoice for end update")
					ResetEvent(event_stream_end);
					buffer_slots.BeginPlayback();
					playback_begin_frame = next_input_frame = resume_frame;
					playback_start_pending = true;
					playback_has_buffer = false;
					emit_audio_output("end_rebuild");
				}
				else
					playback_end_frame = new_end;
			}
			goto do_fill_buffer;

		case WAIT_OBJECT_0 + EventSetVolume:
			if (FAILED(hr = pSourceVoice->SetVolume(volume.load(std::memory_order_acquire))))
				REPORT_ERROR("Failed setting XAudio2 SourceVoice volume")
			break;

		case WAIT_TIMEOUT:
			goto do_fill_buffer;

		case WAIT_OBJECT_0 + EventBufferEnd: {
			// Auto-reset events may coalesce callbacks; scanning every atomic slot
			// recovers all completed buffers in one pass.
			emit_audio_output("buffer_end", -1, -1, -1.0, true);
		do_fill_buffer:
			if (!playback_should_be_running)
				break;
			if (!playback_start_pending)
				publish_position();

			// A flush may still be reflected in GetState until its callbacks run.
			// Respect the API's queue limit even with a maximum-sized old bank.
			XAUDIO2_VOICE_STATE queue_state{};
			pSourceVoice->GetState(&queue_state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
			for (UINT32 queued = queue_state.BuffersQueued; queued < XAUDIO2_MAX_QUEUED_BUFFERS; ++queued) {
				int64_t const remaining_frames = playback_end_frame - next_input_frame;
				if (remaining_frames <= 0)
					break;
				int const i = buffer_slots.TryAcquire();
				if (i < 0) {
					break;
				}

				int const fill_len = static_cast<int>(std::min<int64_t>(remaining_frames, wanted_frames));
				auto const fill_started = trace_audio_output
					? std::chrono::steady_clock::now()
					: std::chrono::steady_clock::time_point{};
				if (original)
					provider->GetAudio(buff[i].data(), next_input_frame, fill_len);
				else
					provider->GetInt16MonoAudio(reinterpret_cast<int16_t*>(buff[i].data()), next_input_frame, fill_len);
				double const fill_duration_ms = trace_audio_output
					? static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
						std::chrono::steady_clock::now() - fill_started).count()) / 1000.0
					: -1.0;

				int64_t const buffer_end_frame = next_input_frame + fill_len;
				XAUDIO2_BUFFER xbf{};
				xbf.Flags = buffer_end_frame == playback_end_frame ? XAUDIO2_END_OF_STREAM : 0;
				xbf.AudioBytes = fill_len * wfx.nBlockAlign;
				xbf.pAudioData = buff[i].data();
				xbf.pContext = reinterpret_cast<void*>(static_cast<intptr_t>(i));
				if (FAILED(hr = pSourceVoice->SubmitSourceBuffer(&xbf))) {
					buffer_slots.Release(i);
					REPORT_ERROR("Failed submitting XAudio2 source buffer")
				}
				next_input_frame = buffer_end_frame;
				playback_has_buffer = true;
				emit_audio_output("submit", fill_len, xbf.AudioBytes, fill_duration_ms, true);
				if (!playback_start_pending)
					publish_position();
			}

			if (playback_start_pending && playback_has_buffer) {
				if (cancelled_playback_generation.load(std::memory_order_acquire) >= playback_generation)
					goto stop_playback;
				XAUDIO2_VOICE_STATE initial_state{};
				pSourceVoice->GetState(&initial_state);
				playback_sample_base = initial_state.SamplesPlayed;
				if (FAILED(hr = pSourceVoice->Start()))
					REPORT_ERROR("Failed starting XAudio2 SourceVoice")
				if (FAILED(hr = pXAudio2->StartEngine()))
					REPORT_ERROR("Failed restarting XAudio2 engine")
				if (cancelled_playback_generation.load(std::memory_order_acquire) >= playback_generation)
					goto stop_playback;
				playback_start_pending = false;
				publish_position();
				SetEvent(is_playing);
				emit_audio_output("start_prefilled");
				completed_playback_generation.store(playback_generation, std::memory_order_release);
				SetEvent(playback_request_done);
			}
			else if (playback_start_pending) {
				emit_audio_output("start_waiting_for_buffer");
			}
			if (!playback_start_pending)
				goto check_stream_end;
			break;
		}

		case WAIT_OBJECT_0 + EventStreamEnd: {
		check_stream_end:
			XAUDIO2_VOICE_STATE voice_state{};
			pSourceVoice->GetState(&voice_state);
			if (playback_should_be_running && !playback_start_pending
				&& next_input_frame >= playback_end_frame && voice_state.BuffersQueued == 0) {
				emit_audio_output("stream_end");
				publish_position(false);
				ResetEvent(is_playing);
				playback_should_be_running = false;
				playback_has_buffer = false;
			}
			break;
		}

		case WAIT_OBJECT_0 + EventVoiceError:
			LOG_E("audio/player/xaudio2") << "XAudio2 SourceVoice error HRESULT "
				<< static_cast<long>(voice_error.load(std::memory_order_acquire));
			REPORT_ERROR("XAudio2 SourceVoice reported an error")

		case WAIT_OBJECT_0 + EventKillSelf:
			// Voice RAII objects are destroyed before the engine, after Run exits.
			running = false;
			ResetEvent(is_playing);
			playback_should_be_running = false;
			playback_start_pending = false;
			pSourceVoice->Stop();
			pSourceVoice->FlushSourceBuffers();
			break;

		default:
			REPORT_ERROR("Something bad happened while waiting on events in playback loop, either the wait failed or an event object was abandoned.")
				break;
		}
	}
}

#undef REPORT_ERROR

void XAudio2Thread::CheckError()
{
	try {
		switch (WaitForSingleObject(error_happened, 0))
		{
		case WAIT_OBJECT_0:
			throw error_message;

		case WAIT_ABANDONED:
			throw "The XAudio2Thread error signal event was abandoned, somehow. This should not happen.";

		case WAIT_FAILED:
			throw "Failed checking state of XAudio2Thread error signal event.";

		case WAIT_TIMEOUT:
		default:
			return;
		}
	}
	catch (...) {
		ResetEvent(is_playing);
		ResetEvent(thread_running);
		throw;
	}
}

XAudio2Thread::XAudio2Thread(agi::AudioProvider* provider, int WantedLatency, int BufferLength)
	: event_start_playback(CreateEvent(0, FALSE, FALSE, 0))
	, event_stop_playback(CreateEvent(0, FALSE, FALSE, 0))
	, event_update_end_time(CreateEvent(0, FALSE, FALSE, 0))
	, event_set_volume(CreateEvent(0, FALSE, FALSE, 0))
	, event_buffer_end(CreateEvent(0, FALSE, FALSE, 0))
	, event_stream_end(CreateEvent(0, FALSE, FALSE, 0))
	, event_voice_error(CreateEvent(0, FALSE, FALSE, 0))
	, event_kill_self(CreateEvent(0, FALSE, FALSE, 0))
	, thread_running(CreateEvent(0, TRUE, FALSE, 0))
	, is_playing(CreateEvent(0, TRUE, FALSE, 0))
	, playback_request_done(CreateEvent(0, FALSE, FALSE, 0))
	, error_happened(CreateEvent(0, TRUE, FALSE, 0))
	, wanted_latency(WantedLatency)
	, provider(provider)
	, buffer_slots(std::min(BufferLength, XAUDIO2_MAX_QUEUED_BUFFERS))
{
	if (!(thread_handle = (HANDLE)_beginthreadex(0, 0, ThreadProc, this, 0, 0))) {
		throw AudioPlayerOpenError("Failed creating playback thread in XAudio2Player. This is bad.");
	}

	HANDLE running_or_error[] = { thread_running, error_happened };
	switch (WaitForMultipleObjects(2, running_or_error, FALSE, INFINITE)) {
	case WAIT_OBJECT_0:
		// running, all good
		return;

	case WAIT_OBJECT_0 + 1:
		// error happened, we fail
		throw AudioPlayerOpenError(error_message ? error_message : "Failed wait for thread start or thread error in XAudio2Player. This is bad.");

	default:
		throw AudioPlayerOpenError("Failed wait for thread start or thread error in XAudio2Player. This is bad.");
	}
}

XAudio2Thread::~XAudio2Thread() {
	SetEvent(event_kill_self);
	WaitForSingleObject(thread_handle, INFINITE);
}

void XAudio2Thread::Play(int64_t start, int64_t count)
{
	CheckError();

	start_frame.store(start, std::memory_order_release);
	end_frame.store(start + count, std::memory_order_release);
	uint64_t const generation = playback_request_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
	ResetEvent(playback_request_done);
	SetEvent(event_start_playback);

	// Bound the synchronous hand-off so a provider or callback failure cannot
	// leave the UI blocked indefinitely. The playback thread separately reports
	// success (including an empty range) and terminal errors.
	HANDLE events_to_wait[] = { playback_request_done, error_happened, thread_handle };
	ULONGLONG const deadline = GetTickCount64() + 10000;
	while (true) {
		ULONGLONG const now = GetTickCount64();
		DWORD const remaining = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
		switch (WaitForMultipleObjects(3, events_to_wait, FALSE, remaining)) {
		case WAIT_OBJECT_0 + 0:
			if (completed_playback_generation.load(std::memory_order_acquire) < generation)
				continue;
			LOG_D("audio/player/xaudio2") << "Playback request handled";
			return;
		case WAIT_OBJECT_0 + 1:
			throw error_message ? error_message : "XAudio2 playback thread failed while starting playback";
		case WAIT_OBJECT_0 + 2:
			CheckError();
			throw "XAudio2 playback thread exited while starting playback";
		case WAIT_TIMEOUT:
			cancelled_playback_generation.store(generation, std::memory_order_release);
			SetEvent(event_stop_playback);
			throw "Timed out waiting for XAudio2 playback to start";
		default:
			throw agi::InternalError("Unexpected result from WaitForMultipleObjects in XAudio2Thread::Play");
		}
	}
}

void XAudio2Thread::Stop() {
	CheckError();

	SetEvent(event_stop_playback);
}

void XAudio2Thread::SetEndFrame(int64_t new_end_frame) {
	CheckError();

	end_frame.store(new_end_frame, std::memory_order_release);
	SetEvent(event_update_end_time);
}

void XAudio2Thread::SetVolume(double new_volume) {
	CheckError();

	volume.store(new_volume, std::memory_order_release);
	SetEvent(event_set_volume);
}

bool XAudio2Thread::IsPlaying() {
	CheckError();

	switch (WaitForSingleObject(is_playing, 0))
	{
	case WAIT_ABANDONED:
		throw "The XAudio2Thread playback state event was abandoned, somehow. This should not happen.";

	case WAIT_FAILED:
		throw "Failed checking state of XAudio2Thread playback state event.";

	case WAIT_OBJECT_0:
		return true;

	case WAIT_TIMEOUT:
	default:
		return false;
	}
}

int64_t XAudio2Thread::GetCurrentFrame() {
	CheckError();
	if (!IsPlaying()) return 0;
	std::scoped_lock lock(playback_clock_mutex);
	return playback_clock.Position(std::chrono::steady_clock::now(), provider->GetSampleRate());
}

int64_t XAudio2Thread::GetEndFrame() {
	CheckError();
	return end_frame.load(std::memory_order_acquire);
}

bool XAudio2Thread::IsDead() {
	switch (WaitForSingleObject(thread_running, 0))
	{
	case WAIT_OBJECT_0:
		return false;
	default:
		return true;
	}
}

XAudio2Player::XAudio2Player(agi::AudioProvider* provider) :AudioPlayer(provider) {
	// The buffer will hold BufferLength times WantedLatency milliseconds of audio
	WantedLatency = OPT_GET("Player/Audio/DirectSound/Buffer Latency")->GetInt();
	BufferLength = OPT_GET("Player/Audio/DirectSound/Buffer Length")->GetInt();

	// sanity checking
	if (WantedLatency <= 0)
		WantedLatency = 100;
	if (BufferLength <= 0)
		BufferLength = 5;

	try {
		thread = agi::make_unique<XAudio2Thread>(provider, WantedLatency, BufferLength);
	}
	catch (const char* msg) {
		LOG_E("audio/player/xaudio2") << msg;
		throw AudioPlayerOpenError(msg);
	}
}

bool XAudio2Player::IsThreadAlive() {
	if (thread && thread->IsDead())
		thread.reset();
	return static_cast<bool>(thread);
}

void XAudio2Player::Play(int64_t start, int64_t count) {
	try {
		thread->Play(start, count);
	}
	catch (const char* msg) {
		LOG_E("audio/player/xaudio2") << msg;
	}
}

void XAudio2Player::Stop() {
	try {
		if (IsThreadAlive()) thread->Stop();
	}
	catch (const char* msg) {
		LOG_E("audio/player/xaudio2") << msg;
	}
}

bool XAudio2Player::IsPlaying() {
	try {
		if (!IsThreadAlive()) return false;
		return thread->IsPlaying();
	}
	catch (const char* msg) {
		LOG_E("audio/player/xaudio2") << msg;
		return false;
	}
}

int64_t XAudio2Player::GetEndPosition() {
	try {
		if (!IsThreadAlive()) return 0;
		return thread->GetEndFrame();
	}
	catch (const char* msg) {
		LOG_E("audio/player/xaudio2") << msg;
		return 0;
	}
}

int64_t XAudio2Player::GetCurrentPosition() {
	try {
		if (!IsThreadAlive()) return 0;
		return thread->GetCurrentFrame();
	}
	catch (const char* msg) {
		LOG_E("audio/player/xaudio2") << msg;
		return 0;
	}
}

void XAudio2Player::SetEndPosition(int64_t pos) {
	try {
		if (IsThreadAlive()) thread->SetEndFrame(pos);
	}
	catch (const char* msg) {
		LOG_E("audio/player/xaudio2") << msg;
	}
}

void XAudio2Player::SetVolume(double vol) {
	try {
		if (IsThreadAlive()) thread->SetVolume(vol);
	}
	catch (const char* msg) {
		LOG_E("audio/player/xaudio2") << msg;
	}
}
}

std::unique_ptr<AudioPlayer> CreateXAudio2Player(agi::AudioProvider* provider, AudioPlayerHost const&) {
	return agi::make_unique<XAudio2Player>(provider);
}

#endif // WITH_DIRECTSOUND
