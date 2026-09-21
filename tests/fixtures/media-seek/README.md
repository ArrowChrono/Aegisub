# Complex seek fixtures and independent oracles

These are small, entirely generated A/V files for the GacUI complex-media
acceptance smoke. No captured media, personal metadata, external subtitle,
user configuration or private path is included. The committed media and
`reference.txt` are test inputs; Python, FFmpeg and FFprobe are optional
regeneration tools, not build or test runtime dependencies.

## Video identity and timestamp contract

Both files contain ordinary lossy H.264 `yuv420p`, 320 x 180, with a closed
48-frame GOP, scene-cut insertion disabled, and three configured B frames.
Lossless x264 mode is deliberately not used: it disables the B frames needed
by this acceptance case. Both committed streams have keyframes exactly at
frame indices 0 and 48; all other frames are actual inter frames.

| File | Frames | I / P / B | Frame PTS in integer milliseconds |
| --- | ---: | --- | --- |
| `cfr-aac.mp4` | 80 | 2 / 20 / 58 | `40 * n` |
| `vfr-opus.mkv` | 64 | 2 / 16 / 46 | `200 * (n / 4) + [0, 20, 80, 120][n % 4]`, integer division |

The VFR intervals repeat 20, 60, 40 and 80 ms. PTS are exact integer
milliseconds, avoiding an unrelated difference between provider timestamp
rounding conventions. The final frame starts at 3160 ms for CFR and 3120 ms
for VFR. Do not replace these PTS with the container's average frame rate.

The original image is grayscale RGB:

- Rows `0 <= y < 100` contain eight 40-pixel-wide vertical barcode blocks.
  Block `i`, from left to right, is 224 if `(frame >> i) & 1`, otherwise 32.
  The fixed probes are `(20 + 40*i, 60)` for `0 <= i < 8`.
- Rows `100 <= y < 180` are source identity 96 for CFR/AAC and 160 for
  VFR/Opus. The fixed source probe is `(160, 140)`.
- Every RGB channel must match the original value within **4 levels**.
  Regeneration checks all nine probes in every decoded frame; the committed
  files' largest error is 1 level. The tolerance permits codec/color-conversion
  rounding, not a wrong barcode bit or replacement-source identity.

## Audio timeline, codec delay and source signal

Both audio tracks are 48 kHz mono. The AAC track uses 128 kbit/s with
perceptual noise substitution disabled. The Opus track uses 96 kbit/s and
declares **6.5 ms CodecDelay** and **80 ms SeekPreRoll** in its actual Matroska
TrackEntry. The generator reads and validates those EBML elements.

The source has 153600 signed-16 samples. For `t = i / 48000`, its value is:

```text
round(sign * (11000*sin(2*pi*(997*t + 137*t*t)) + 4000*sin(2*pi*1511*t)))
```

`sign` is +1 for AAC and -1 for Opus. The chirp prevents a stale periodic
block from masquerading as the requested range. The generated signal is not
used as a sample-exact oracle for lossy audio.

Encoding requests a positive 240 ms input offset, but the contract is the
**actual committed container and independent decoded timeline**, not that
command-line request. Codec priming, edit lists and Matroska millisecond
timestamps make the resulting values different:

| File | Leading timeline silence | Sequential timeline samples | Original signal origin |
| --- | ---: | ---: | ---: |
| `cfr-aac.mp4` | 10464 (218 ms) | 165088 | 11488 = 10464 + 1024 AAC priming samples |
| `vfr-opus.mkv` | 11232 (234 ms) | 164832 | 11232; declared Opus preskip is already removed |

The sequential oracle is independently decoded with FFmpeg and
`aresample=async=1:first_pts=0`, converting media timestamps to the zero-based
video timeline and padding its leading silence. The generator checks the
first decoded PTS and total sample count against the table, instead of fitting
an offset from provider output. It also compares the decoded interior with
the mathematical signal at the fixed origin: RMS error must be at most 100
and peak error at most 1000, excluding codec edge transients. Measured RMS/peak
are 60.06/519 for AAC and 47.53/246 for Opus. **These source-signal limits are
not tolerances for the host test's sparse decoded reference.**

The audio extends beyond the last video frame. A provider must not silently
truncate audio to video duration, discard declared priming incorrectly, or
invent successful silence inside the valid signal. A difference from this
fixed independent oracle must be investigated, not corrected by shifting the
reference to whatever a provider happens to return.

## `reference.txt` format

The file is whitespace-delimited, with no machine-specific information:

```text
MEDIA_SEEK 1
FILE filename frame_count timeline_sample_count delay_samples cached_sample_tolerance
PTS <frame_count integer millisecond values>
AUDIO timeline_start_sample sample_count
<sample_count signed-16 values>
... more AUDIO blocks ...
END
... next FILE ...
```

The per-sample cached-reference tolerance is **2 signed-16 levels**, reserved
for integer/floating conversion rounding. It is less than 0.0062% of full
scale and does not permit a sample-index shift, decoder delay compensation,
resampling or substituted silence. Samples outside the declared timeline and
before its leading-silence boundary must be exactly zero.

Each file has eight **960-sample (20 ms)** blocks, starting at exact integer
milliseconds. The last block crosses the independently decoded EOF and is
zero-padded only after that boundary:

- AAC: 0, 208, 238, 960, 1400, 2000, 2600 and 3430 ms.
- Opus: 0, 224, 254, 960, 1400, 2000, 2600 and 3420 ms.

The order of reading those blocks in the smoke need not be sequential. The
reference includes leading silence, a delay-boundary crossing, post-priming
signal, different interior regions and the actual tail, without committing
full decoded PCM files.

## Fresh uncached codec seek tolerance

Cache slicing and codec random access are distinct contracts. The normal RAM
cache is checked against the independent sequential blocks at the two-level
per-sample tolerance. Fresh uncached codec seeks additionally use:

```text
residual = max(0, abs(actual - reference) - 2)
sqrt(sum(residual^2) / sum(reference^2)) <= 0.01
```

The two-level subtraction retains the cached check's fixed sample-quantization
allowance; it is not a time-alignment adjustment. In particular, the nonzero
AAC priming block at 208 ms has RMS amplitude only 5.574, so a legal one-level
rounding difference must not become a large relative codec-state error. An
all-zero reference block and out-of-timeline padding still require exact
zeros, with no two-level allowance. No correlation search, offset fitting or
post-failure tolerance change is permitted. The **1% normalized RMS limit and
two-level residual predicate were frozen before measuring any PCM from a
provider under test**. Separate FFmpeg invocations with input-side `-ss`
decoded each range: AAC matched exactly; Opus's largest legitimate seek-state
difference was 0.0018028 at 2600 ms, with raw peak difference 43. The generator
records its residual normalized error and verifies the same 0.01 limit. This is an
actual fresh demuxer/decoder seek experiment, not an assertion that the CLI
always decodes exactly 80 ms of preroll.

The generator also proves the tolerance rejects mutations of the full
independent PCM, considering all 14 non-silent reference blocks:

| Mutation | Smallest normalized RMS error |
| --- | ---: |
| Shift back one sample | 0.07392 |
| Shift forward one sample | 0.07415 |
| Shift back 20 ms | 0.56237 |
| Shift forward 20 ms | 0.46888 |
| Replace signal with silence | 0.83699 |
| Remove the last 128 valid signal samples | 0.29206 |
| Read the other source at the same timeline index | 0.83699 |

All exceed 0.01. A wholly silent block cannot identify a sample shift by
itself; the onset, interior and tail checks supply that evidence.

## Reproduction and retained evidence

Run from the repository root with Python 3.9+ and FFmpeg/FFprobe on PATH:

```powershell
python tests/fixtures/media-seek/generate.py
```

The optional `--ffmpeg` / `--ffprobe` arguments select executables. All
subprocesses have a **30-second timeout**, encodes/decodes use `-nostdin`, and
every exit code is checked. Encoding strips inherited metadata. Intermediate
RGB, WAV and decoded data stay in a temporary directory beneath
`build-dir/artifacts/gacui-media-seek/fixtures` and are removed afterwards.
`validation.json` there retains timestamps, frame types, hashes, source-signal
alignment, independent seeks and mutation results.

The committed files use FFmpeg `git-2022-08-12-f6a36c7cf`. Repeated generation
produced identical bytes for both media and `reference.txt`. New encoder or
decoder versions may require an explicit reviewed fixture revision; the
generator must not silently recalibrate the frozen timing or tolerances.
The reference is whitespace-delimited text, so Git's platform-specific line
ending normalization does not change its numeric contract. Only the binary
media hashes are listed below.

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| `cfr-aac.mp4` | 58467 | `5705be9dd458028c9d2eb41401b5156b358fabaac80d13d5afc9b13e590f8800` |
| `vfr-opus.mkv` | 59993 | `c7765d5eb4a433dc66a584b08287a4b5897eb58dc3e55cfe18be6483e0d8ed01` |
