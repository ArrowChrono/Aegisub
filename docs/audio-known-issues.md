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

## Legacy cache preemption test synchronization (2026-10-20)

**Status:** Diagnosed and corrected in the tests. The observed failure is a
synchronization assumption about a legal scheduling order, not evidence of a
cache data race. Production cache behavior and cancellation policy are unchanged.
This conclusion is limited to the investigated failure chain, not a whole-module
race audit.

### Failure and introducing commit

The configured C++ run during Font Audit validation failed
`lagi_audio_display.spectrum_analysis_cache_visible_miss_preempts_remaining_prefetch_batch`:
provider calls were 3 rather than 2, block 1 was ready, prefetch builds were 3
rather than 1, and busy skips were 0. The complete original report remains in
`build-gacui-preferences-validation/artifacts/font-audit-safety/build-cpp.log`.

`56b004913d608b0c962c33ab067daa1effb5384e`
(`audio_display: optimize legacy caches and Skia retained rendering`) introduced
both background FFT batch building and this test, including its provider gate
and contention wait. The parent only issued prefetch hints and had no such
background FFT test. The analogous waveform test has the same lineage and
synchronization assumption. The Font Audit changes did not introduce the issue.

### Implementation contract versus the test assumption

In both `AudioSpectrumAnalysisCache::Get` and
`AudioWaveformSummaryCache::Get`, `visible_waiters` is nonzero only while waiting
for the build mutex. Once the visible request acquires the mutex, it decrements
the counter and builds while still owning that mutex. At each subsequent missing
block, prefetch yields if a current waiter exists or the build mutex cannot be
acquired immediately. It does not promise sticky cancellation merely because a
visible request waited earlier in the batch's lifetime.

The old contention wait correctly proved that the visible waiter had registered;
it was not a premature publication or a missing memory barrier. However, the
old `Release()` then unblocked every provider read. It allowed the following:

1. Prefetch publishes the first block and releases the build mutex.
2. The worker is descheduled before checking its next block.
3. The visible request acquires the mutex, clears its waiter, finishes and returns.
4. The worker resumes with no active waiter or busy build lock and fills the
   remaining missing blocks.

The test waited only for the visible future, then assumed the prefetch batch
had already observed busy and stopped. Its metrics could still be changing
between assertions. The original 3 provider calls and later 3 published prefetch
blocks reflect different observation instants, not an inconsistent locked cache
snapshot.

### Controlled native evidence

A fixed sixteen-iteration natural-schedule observation passed all sixteen;
that was retained as an observation, not used as proof of absence or repair.
CDB then reproduced the failure once without source or memory changes: pause
prefetch at the next-block check, let visible finish, resume the worker while
holding the assertion thread, then let assertions inspect the completed batch.
The old test failed with the same four predicates: 4 provider calls, middle block
present, prefetch builds 3 and busy skips 0. The different call count is expected
because the debugger made the worker finish before all assertions.

Raw logs, XML, exact inputs and source/binary binding are under
`build-dir/artifacts/audio-cache-preemption/`, indexed by `final-validation.json`.
`cdb-original.log` records `WORKER_BEFORE_NEXT_BLOCK`,
`VISIBLE_FINISHED_BEFORE_ASSERTIONS`, then `PREFETCH_BATCH_FINISHED`.
`cdb-original.xml` records one executed and failed test. Debugger thread pauses
are deliberate schedule control, not a claim of unperturbed execution.

### Correction and regression contract

Both tests now reuse the sequenced provider gate. They release only the first
read and keep the visible read blocked until the prefetch batch has actually
stopped. The existing ready callback, after this one batch's active-range cleanup,
provides that boundary; it is not generalized to a scheduler-wide idle signal.
The tests assert:

- Exact two provider reads in the expected order and at independent sample offsets.
- Prefetch completion while the visible future is still pending, with one
  prefetch build, zero visible builds and exactly one busy skip.
- Both middle blocks absent and the visible block not prematurely published.
- After the second gate is released, one visible build, both requested endpoint
  blocks present, no middle blocks, no extra reads and the same exact busy count.

Spectrum uses blocks 10 and 13 to avoid left-edge padding and adjacent rolling
window ambiguity. The expected provider starts are 768 and 1152 for hop 128 and
half-window 512. Scope-exit releases provider gates before the visible future and
cache are destroyed; the promise outlives cache shutdown. All waits retain finite
limits and the original ten-second scenario watchdog. No assertion was widened,
scenario removed, timeout increased or retry-until-pass loop introduced.

A second CDB schedule delayed the repaired test's worker until the visible read
was active; the one-test XML passed. Its first breakpoint's optimized local was
unavailable, so it acted as a one-shot first-publication stop; the subsequent
worker-block-11 and visible-start-1152 observations establish the intended order.
Some entry-frame pretty-printer reads also report unavailable state. Those values
are not used as proof: the test's explicit pending-phase assertions and XML are
the accepted metrics evidence. The complete diagnostic messages remain in
`cdb-fixed.log` rather than being silently removed.

The focused tests, related audio suites and complete configured C++ results are
recorded separately in the evidence index, with each actual source/configuration
and report. Validation is Windows x64 RelWithDebInfo; other platforms and race
sanitizers have not been run. A passing run does not establish that the complete
cache implementation has no other races.
