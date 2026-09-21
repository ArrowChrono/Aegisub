# Known audio issues

## PortAudio: incomplete final callback and abrupt stream completion

**Status:** Deferred; source-level finding only. No PortAudio runtime or device
reproduction has been performed. Do not change the backend until an appropriate
test environment is available. The configuration inspected on 2026-09-22 has
`WITH_PORTAUDIO=OFF` in `build-dir/CMakeCache.txt`.

### Conditions and source evidence

The affected code is `PortAudioPlayer::paCallback` in
`src/audio_player_portaudio.cpp`, currently lines 220-235. A playback selection
or the audio's final range can end partway through a callback:

```text
0 < end - current < framesPerBuffer
```

The callback writes only `lenAvailable` mono S16 samples, advances `current`,
and returns continue. It does not initialize the remainder of `outputBuffer`.
On the following callback, no samples remain and it returns `paAbort` rather
than requesting normal completion and draining queued output.

The unwritten tail could expose previous or otherwise unspecified buffer
contents. Aborting rather than draining could also cut off queued final audio.
These are consequences identified from the control flow, not observed playback
glitches or a measured device capture. This backend is disabled in the inspected
Windows build and does not explain the shared Opus timeline regression.

### Record and history

The original investigation was recorded in the ignored build artifact
`build-dir/artifacts/opus-timing/audio-chain-audit.md`, under "Static finding:
PortAudio leaves the final output block partly unwritten". This document is the
durable deferred-issue record; the build artifact need not exist in a checkout.

The current callback lineage traces to commit
`1b9f38747b3956f76ec3f9988df3f632ddda4de0` (2009-06-10), which replaced the
PortAudio player with the PortAudio2 implementation. Its partial-fill/continue
and subsequent `paAbort` behavior predates the recent provider, cache, and Opus
changes. The historical commit mentions issue #876 about the backend replacement;
that reference has not been verified as a ticket for this particular EOF defect.

### Reproduction and acceptance when a test host is available

1. Use a supported PortAudio-enabled build with a working output device. Record
   the PortAudio version, host API, callback sizes, sample rate, and output latency.
2. Play known nonzero PCM ending before, exactly at, and just after a callback
   boundary, including a selection shorter than one callback. Bound each process
   run to 30 seconds and treat a timeout as failure.
3. Use callback instrumentation with a nonzero sentinel in the output buffer to
   distinguish valid PCM from unwritten samples; also capture actual output to
   determine whether queued final samples are lost. Do not infer an audible
   failure solely from the static finding.
4. Acceptance for a future fix: every submitted output sample is initialized;
   samples beyond the requested range are silence; valid final samples are
   drained without truncation; playback reaches the stopped state; and replay
   starts without stale tail audio. Keep user-requested immediate stop separate
   from natural range completion.

No production fix, backend configuration change, or runtime validation is part
of this deferred record.
