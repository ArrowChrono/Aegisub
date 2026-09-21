# Shared Matroska Opus audio alignment

## Reproduction and ownership

The GacUI complex-media fixture exposed a shared provider defect before any GUI
session, RAM cache, rendering or playback clock was involved. The same actual
decoder DLLs are used by `exp` and the GacUI worktree. The reproduction is now a
GUI-independent `audio-timeline-smoke` target on the mainline.

`tests/fixtures/media-seek/` and its independent sparse `reference.txt` are copied
unchanged from the original acceptance fixture. Do not shift the oracle, fit a
sample offset, widen tolerances, or skip a decoder to make this test pass.

For `vfr-opus.mkv`, raw EBML inspection gives a first audio Block at 234 ms,
CodecDelay of 6.5 ms, OpusHead pre-skip of 312 samples at 48 kHz, and SeekPreRoll
of 80 ms. The first video presentation timestamp is zero. Per
[RFC 9559](https://www.rfc-editor.org/rfc/rfc9559.html#section-5.1.4.1.25):

```text
first decoded packet origin = 234 ms - 6.5 ms = 227.5 ms
first retained PCM sample   = 227.5 ms + 312 / 48000 s = 234 ms
leading timeline samples    = 11232
decoded PCM samples         = 153600
total timeline samples      = 164832
```

The FFmpeg-demuxed packet PTS is 227 ms, not the original Block timestamp:
rescaling CodecDelay to the millisecond stream time base rounds 6.5 ms to 7 ms.
Adding 312 samples to that rounded PTS cannot recover the lost half millisecond.
SeekPreRoll is a decoding-history requirement, never another timeline offset.

The actual factory baseline was:

| Backend | Total samples | First nonzero sample | Advance |
| --- | ---: | ---: | ---: |
| Independent reference | 164832 | 11232 | 0 |
| FFmpegSource | 164496 | 10896 | 336 samples / 7 ms |
| LsmasNative | 164808 | 11208 | 24 samples / 0.5 ms |

AAC is an independent compatibility control: both providers produce 165088
samples for `cfr-aac.mp4`. Its timeline padding ends at 10464, while the first
nonzero decoded sample is 10471; these are distinct properties.

## Historical attribution, not an invented behavioral bisect

### FFmpegSource

Aegisub `bd7ac88f6b47dc5147c1114460d92e5ebaf32a5f` started delegating A/V delay
to FFMS in 2010. Upstream FFMS
`9f7f57102012d4a76bc01f2f9040946913645845` established the corresponding
packet-PTS-based delay calculation. FFMS 5.0 still associates that origin with
already pre-skipped PCM. It therefore places the retained samples at 227 ms,
accounting for the 336-sample advance.

The Aegisub `875456c803fe8bdd116ec779c0a750041bb6ff53` change from `-1` to
`FFMS_DELAY_FIRST_VIDEO_TRACK` is an equivalent constant rename, not the cause.
`6b827ebf0a93e5e6f7a35fdcdf0d6548480984b0` records the packaged FFMS 5.0
dependency. These facts identify the source assumption and dependency lineage;
they do **not** establish the first historically failing FFMS/FFmpeg binary pair.
No historical binary rebuild/bisect is claimed.

### LsmasNative

LsmasSharp `2f0617bf3f2d04d1eaa49eb9e9ecfadd00e00042` introduced
`av_gap += initial_padding` while correctly removing indexed codec padding.
For this container, the existing gap uses rounded 227 ms PTS, then adds the
exact 312 samples: `227 * 48 + 312 = 11208`. The remaining 24-sample discrepancy
is a mixed-precision conversion defect in that padding correction.

LsmasSharp `4c85eb4c68e3a96c18c0936652d663a30724d145` retained this arithmetic
while fixing independent MOV endpoint, fresh-seek priming and AAC rewind bugs.
Aegisub `284ee06b4c8a4d29f5ecccac4e16a1f384e91f2d` selected the 202608 asset;
`fc1393e9be39bc0ab68ee3997bff38ef77b7ccbc` corrected its release URL.

Do not revert those native changes wholesale. In particular, Aegisub
`d5e9741c23bfdaea778688c0a225399ba2e4a6ce` only corrected readiness to cover the
entire shifted timeline. It cannot move PCM; reverting it would restore the
unreadable positive-gap tail. Checked-read and cache-failure recovery changes
likewise address different defects and remain necessary.

## Actual decoder identity

Version APIs and SHA-256, rather than a release filename alone, identified the
executing libraries:

| Component | Identity |
| --- | --- |
| FFMS2 | 5.0.0.0 / `FFMS_GetVersion() == 0x05000000` |
| FFMS2 SHA-256 | `970d3f8d170fd13a2dd8a7e8a7f56b0ce5a1dc7eea79d1774bbb2c2cab47e3f0` |
| LsmasNative | `202607-2-g4c85eb4`, API 1.1.0, native `4c85eb4c68e3` |
| LsmasNative SHA-256 | `5ce705a0f7602a288177b7ebdbc2d7774285635fd439b84ad0d1e60804b5a047` |
| L-SMASH-Works | `7e65185d3f08` |
| Native FFmpeg | `39c24f36247f+mov-audio-end`, avcodec 62.36.101, avformat 62.19.101 |

## Shared alignment policy

With the default `AEGISUB_MATROSKA_PARSING=ON` configuration, select the actual
Opus audio track by audio ordinal, then read its raw first Block timestamp,
CodecDelay and OpusHead pre-skip. Determine video frame-zero origin from the
earliest presentation timestamp in the first video cluster, not packet order.
Read only block headers; support SimpleBlock, BlockGroup, signed relative
timestamps and unknown-size clusters.

For these files, open FFMS with `FFMS_DELAY_NO_SHIFT` and Lsmas with `av_sync=0`.
Both expose retained PCM without backend A/V padding or prefix clipping. Apply
one shared offset:

```text
round((audio_block_ns - video_origin_ns - codec_delay_ns) * rate / 1e9
      + opus_pre_skip * rate / 48000)
```

The wrapper pads positive offsets, trims negative offsets, preserves the final
decoded sample and publishes the complete timeline as ready before caching.
Checked reads still propagate failures. No runtime-version or observed-offset
heuristic is used. AAC, other containers/codecs and audio-only inputs retain
their existing provider policy.

The optional legacy Matroska parser does not expose the required metadata. Its
build remains compatible and retains the previous behavior; the exact-timing
fix and scanner tests require the default modern parser. This is an explicit
scope limitation, not a claim that legacy-parser Opus output is corrected.

## Additional native random-seek finding

After fixing the timeline, the unchanged strict seek oracle exposed another
pre-existing Lsmas limitation. At canonical sample 124800, native unsynchronized
sample 113568 and old synchronized sample 124776 produce byte-identical random
PCM with normalized residual 0.085501. Fresh and previously used handles agree;
the best diagnostic lag is zero. Sequential PCM remains within one S16 level.

The native patch already decodes at least 80 ms before Opus seeks. It does not
omit the codec's minimum preroll. FFmpeg's CELT energy prediction retains
history that converges after a reset; a minimum preroll is not a universal
promise of bit-exact PCM, or of a fixed 1% residual. FFMS uses substantially
more packet history and passes this fixture's unchanged seek contract.

The shared Lsmas provider therefore explicitly decodes and discards one
additional container-declared SeekPreRoll window (at least Opus's recommended
80 ms) before noncontiguous reads, then continues at the requested sample.
Sequential reads and cache fills do not pay this cost. Scratch storage is
bounded to 4096 sample frames; a failed warmup remains a decode failure rather
than publishing silence. This is an editing-accuracy policy, not a fitted
timeline shift or a claim that any finite preroll gives universally bit-exact
Opus seeks. For this fixture, the additional declared 80 ms reduces every
sparse-window normalized residual below 0.0061 without changing the 0.01 gate.
Exact repeatable random PCM is supplied by the sequentially populated caches.

## Validation entry points

Run commands from the repository root, preserving the build's configuration:

```powershell
cmake --build build-dir --config RelWithDebInfo --target Aegisub audio-timeline-smoke test-aegisub --parallel
ctest --test-dir build-dir -C RelWithDebInfo -R '^audio_timeline_smoke$' --output-on-failure
```

The smoke has a 90-second process limit and 10-second cache deadline. Both
providers must be available without fallback. It checks exact timeline length,
leading silence and EOF, strict sequential/reference PCM, the frozen 1% direct
seek criterion, and RAM/HD cache output against both independent reference and
exact cached slices. Unit tests cover raw metadata, negative offsets, format
preservation and checked failure propagation without native decoder DLLs.
Diagnostic baseline captures and logs are retained under
`build-dir/artifacts/opus-timing/`; normal smoke output uses
`build-dir/artifacts/audio-timeline/`.
