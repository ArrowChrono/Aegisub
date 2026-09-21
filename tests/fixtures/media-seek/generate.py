"""Generate and independently validate the synthetic complex-seek fixtures."""

import argparse
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import wave


WIDTH, HEIGHT, SAMPLE_RATE = 320, 180, 48000
SOURCE_SAMPLES, BLOCK_SAMPLES = 153600, 960
PIXEL_TOLERANCE, PCM_TOLERANCE = 4, 2
DIRECT_NORMALIZED_RMS_LIMIT = 0.01
FIXTURES = (
    {"name": "cfr-aac.mp4", "frames": 80, "replacement": False,
     "delay": 10464, "priming": 1024, "samples": 165088,
     "blocks_ms": (0, 208, 238, 960, 1400, 2000, 2600, 3430)},
    {"name": "vfr-opus.mkv", "frames": 64, "replacement": True,
     "delay": 11232, "priming": 0, "samples": 164832,
     "blocks_ms": (0, 224, 254, 960, 1400, 2000, 2600, 3420)},
)


def run(command):
    return subprocess.run(command, check=True, capture_output=True, timeout=30)


def relative_directory(value, root):
    path = Path(value)
    if path.is_absolute() or not path.resolve().is_relative_to(root):
        raise ValueError("Output directories must remain repository-relative")
    path.mkdir(parents=True, exist_ok=True)
    return path


def source_sample(index, replacement):
    t = index / SAMPLE_RATE
    value = 11000 * math.sin(2 * math.pi * (997 * t + 137 * t * t))
    value += 4000 * math.sin(2 * math.pi * 1511 * t)
    return round(-value if replacement else value)


def block_at(samples, start):
    return [samples[i] if 0 <= i < len(samples) else 0
            for i in range(start, start + BLOCK_SAMPLES)]


def normalized_error(actual, expected):
    energy = sum(value * value for value in expected)
    if not energy:
        if any(actual):
            raise ValueError("A silent reference block acquired signal")
        return 0.0
    squared_error = sum(max(0, abs(a - b) - PCM_TOLERANCE) ** 2
                        for a, b in zip(actual, expected))
    return math.sqrt(squared_error / energy)


def pts_ms(index, replacement):
    if replacement:
        return (index // 4) * 200 + (0, 20, 80, 120)[index % 4]
    return index * 40


def create_inputs(directory, spec):
    raw = directory / (Path(spec["name"]).stem + ".rgb")
    with raw.open("wb") as stream:
        for frame in range(spec["frames"]):
            row = b"".join(bytes((224 if frame & (1 << bit) else 32,)) * 120
                           for bit in range(8))
            stream.write(row * 100)
            stream.write(bytes((160 if spec["replacement"] else 96,)) * (WIDTH * 80 * 3))
    pcm = directory / (Path(spec["name"]).stem + ".wav")
    with wave.open(str(pcm), "wb") as stream:
        stream.setparams((1, 2, SAMPLE_RATE, 0, "NONE", "not compressed"))
        stream.writeframes(b"".join(struct.pack("<h", source_sample(i, spec["replacement"]))
                                  for i in range(SOURCE_SAMPLES)))
    return raw, pcm


def encode(ffmpeg, raw, pcm, output, spec):
    command = [ffmpeg, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
               "-f", "rawvideo", "-pixel_format", "rgb24", "-video_size", "320x180",
               "-framerate", "25", "-i", str(raw), "-itsoffset", "0.240", "-i", str(pcm),
               "-map", "0:v:0", "-map", "1:a:0"]
    if spec["replacement"]:
        timestamps = ("settb=1/1000,setpts=floor(N/4)*200+"
                      "if(eq(mod(N\\,4)\\,0)\\,0\\,if(eq(mod(N\\,4)\\,1)\\,20\\,"
                      "if(eq(mod(N\\,4)\\,2)\\,80\\,120)))")
        command += ["-vf", timestamps, "-fps_mode", "passthrough", "-enc_time_base:v", "1:1000"]
    command += ["-c:v", "libx264", "-preset", "medium", "-qp", "12", "-pix_fmt", "yuv420p",
                "-x264-params", "keyint=48:min-keyint=48:scenecut=0:bframes=3:b-adapt=0:open-gop=0",
                "-threads", "1"]
    if spec["replacement"]:
        command += ["-c:a", "libopus", "-b:a", "96k", "-application", "audio"]
    else:
        command += ["-c:a", "aac", "-b:a", "128k", "-aac_pns", "0"]
    command += ["-fflags", "+bitexact", "-flags:v", "+bitexact", "-flags:a", "+bitexact",
                "-map_metadata", "-1", str(output)]
    run(command)


def opus_timing_metadata(data):
    """Read the actual Matroska TrackEntry timing elements, not byte-pattern guesses."""
    def vint(offset, keep_marker):
        first = data[offset]
        length = 1
        marker = 128
        while not first & marker:
            length += 1
            marker >>= 1
            if length > 8:
                raise ValueError("Invalid EBML integer")
        value = first if keep_marker else first & (marker - 1)
        for byte in data[offset + 1:offset + length]:
            value = value * 256 + byte
        return value, offset + length

    found = {}

    def visit(start, end):
        while start < end:
            identifier, offset = vint(start, True)
            size, offset = vint(offset, False)
            stop = min(offset + size, end)
            if identifier in (0x18538067, 0x1654AE6B, 0xAE):
                visit(offset, stop)
            elif identifier in (0x56AA, 0x56BB):
                found[identifier] = int.from_bytes(data[offset:stop], "big")
            start = stop

    visit(0, len(data))
    if found != {0x56AA: 6500000, 0x56BB: 80000000}:
        raise ValueError("Opus must declare 6.5 ms CodecDelay and 80 ms SeekPreRoll")
    return {"codec_delay_ns": found[0x56AA], "seek_preroll_ns": found[0x56BB]}


def validate(ffmpeg, ffprobe, output, directory, spec):
    probe = json.loads(run([ffprobe, "-v", "error", "-show_streams", "-show_format",
                           "-show_frames", "-show_entries",
                           "frame=key_frame,pict_type,best_effort_timestamp_time,nb_samples",
                           "-of", "json", str(output)]).stdout)
    if len(probe["streams"]) != 2:
        raise ValueError("Each fixture must have exactly two streams")
    video_stream, audio_stream = probe["streams"]
    expected_codec = "opus" if spec["replacement"] else "aac"
    if (video_stream["codec_name"], video_stream["width"], video_stream["height"],
            audio_stream["codec_name"], int(audio_stream["sample_rate"]), audio_stream["channels"]) != (
            "h264", WIDTH, HEIGHT, expected_codec, SAMPLE_RATE, 1):
        raise ValueError("Unexpected fixture codecs or dimensions")
    video = [frame for frame in probe["frames"] if "pict_type" in frame]
    audio = [frame for frame in probe["frames"] if "nb_samples" in frame]
    expected_pts = [pts_ms(i, spec["replacement"]) for i in range(spec["frames"])]
    actual_pts = [Fraction(frame["best_effort_timestamp_time"]) * 1000 for frame in video]
    if actual_pts != expected_pts:
        raise ValueError("Video PTS differ from the independent integer-millisecond formula")
    keys = [i for i, frame in enumerate(video) if frame["key_frame"]]
    types = {kind: sum(frame["pict_type"] == kind for frame in video) for kind in ("I", "P", "B")}
    if keys != [0, 48] or types["B"] < spec["frames"] // 2 or not types["P"]:
        raise ValueError("Fixture lost its two long GOPs or real P/B frames")
    first_audio = Fraction(audio[0]["best_effort_timestamp_time"]) * SAMPLE_RATE
    if first_audio != spec["delay"]:
        raise ValueError("First decoded audio PTS changed; do not silently fit a new alignment")

    rgb = directory / (output.stem + "-decoded.rgb")
    run([ffmpeg, "-nostdin", "-v", "error", "-y", "-i", str(output), "-map", "0:v:0",
         "-an", "-fps_mode", "passthrough", "-pix_fmt", "rgb24", "-f", "rawvideo", str(rgb)])
    pixels = rgb.read_bytes()
    frame_bytes = WIDTH * HEIGHT * 3
    if len(pixels) != spec["frames"] * frame_bytes:
        raise ValueError("Full decode changed the video frame count")
    pixel_max_error = 0
    for frame in range(spec["frames"]):
        points = [(20 + 40 * bit, 60, 224 if frame & (1 << bit) else 32) for bit in range(8)]
        points.append((160, 140, 160 if spec["replacement"] else 96))
        for x, y, expected in points:
            offset = frame * frame_bytes + (y * WIDTH + x) * 3
            pixel_max_error = max(pixel_max_error, *(abs(pixels[offset + c] - expected) for c in range(3)))
    if pixel_max_error > PIXEL_TOLERANCE:
        raise ValueError("Decoded barcode/source centers exceed the fixed four-level pixel tolerance")

    decoded = directory / (output.stem + "-timeline.s16")
    run([ffmpeg, "-nostdin", "-v", "error", "-y", "-i", str(output), "-map", "0:a:0",
         "-af", "aresample=async=1:first_pts=0", "-ac", "1", "-ar", str(SAMPLE_RATE),
         "-c:a", "pcm_s16le", "-f", "s16le", str(decoded)])
    data = decoded.read_bytes()
    if len(data) != spec["samples"] * 2:
        raise ValueError("Independent timeline decode sample count changed")
    samples = [value[0] for value in struct.iter_unpack("<h", data)]
    if any(samples[:spec["delay"]]) or not any(samples[spec["delay"]:spec["delay"] + 1024]):
        raise ValueError("Timeline prefix/onset does not match its declared positive delay")
    # A fixed codec/container-derived origin, not a fitted correlation or an
    # alignment copied from either provider under test. Exclude codec edge transients.
    origin = spec["delay"] + spec["priming"]
    errors = [samples[origin + i] - source_sample(i, spec["replacement"])
              for i in range(4096, SOURCE_SAMPLES - 5120)]
    rms = math.sqrt(sum(error * error for error in errors) / len(errors))
    peak = max(map(abs, errors))
    if rms > 100 or peak > 1000:
        raise ValueError("Lossy PCM is not aligned to the independent source signal")
    timing = opus_timing_metadata(output.read_bytes()) if spec["replacement"] else {"encoder_priming_samples": 1024}
    blocks = []
    seek_results, mutations = [], []
    for milliseconds in spec["blocks_ms"]:
        start = milliseconds * 48
        values = block_at(samples, start)
        blocks.append((start, values))
        # This is a separate fresh decoder opened with input-side seeking, not
        # a slice of the sequential decode or output from a provider under test.
        seek_file = directory / (output.stem + "-seek.s16")
        run([ffmpeg, "-nostdin", "-v", "error", "-y", "-ss", f"{milliseconds / 1000:.3f}",
             "-i", str(output), "-map", "0:a:0", "-af",
             "aresample=async=1:first_pts=0,apad=whole_len=960,atrim=end_sample=960",
             "-ar", str(SAMPLE_RATE), "-ac", "1", "-c:a", "pcm_s16le", "-f", "s16le", str(seek_file)])
        sought = [value[0] for value in struct.iter_unpack("<h", seek_file.read_bytes())]
        if len(sought) != BLOCK_SAMPLES:
            raise ValueError("Independent random seek returned an incomplete PCM block")
        if any(value for i, value in enumerate(sought)
               if start + i < spec["delay"] or start + i >= len(samples)):
            raise ValueError("Independent random seek changed exact timeline padding")
        error = normalized_error(sought, values)
        if error > DIRECT_NORMALIZED_RMS_LIMIT:
            raise ValueError("Independent codec seek exceeds the frozen normalized error limit")
        seek_results.append({"start_ms": milliseconds, "normalized_rms": error,
                             "max_error": max(abs(a - b) for a, b in zip(sought, values))})
        if any(values):
            mutation = {f"shift_{delta}_samples": normalized_error(block_at(samples, start + delta), values)
                        for delta in (-960, -1, 1, 960)}
            mutation["silence"] = normalized_error([0] * BLOCK_SAMPLES, values)
            last = max(i for i, value in enumerate(values) if value) + 1
            truncated = values[:max(0, last - 128)] + [0] * (BLOCK_SAMPLES - max(0, last - 128))
            mutation["last_128_signal_samples_missing"] = normalized_error(truncated, values)
            if min(mutation.values()) <= DIRECT_NORMALIZED_RMS_LIMIT:
                raise ValueError("A shifted, silent or truncated oracle mutation would pass")
            mutations.append({"start_ms": milliseconds, "normalized_errors": mutation})
    report = {
        "file": output.name, "bytes": output.stat().st_size,
        "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "frame_count": len(video), "pts_ms": expected_pts, "keyframes": keys, "picture_types": types,
        "pixel_tolerance": PIXEL_TOLERANCE, "pixel_max_error": pixel_max_error,
        "timeline_samples": len(samples), "leading_silence_samples": spec["delay"],
        "source_origin_samples": origin, "source_alignment_rms": rms, "source_alignment_peak": peak,
        "source_validation_limits": {"rms": 100, "peak": 1000},
        "sparse_reference_tolerance": PCM_TOLERANCE, "blocks_ms": list(spec["blocks_ms"]),
        "block_samples": BLOCK_SAMPLES, "codec_timing": timing,
        "timeline_decode_sha256": hashlib.sha256(data).hexdigest(),
        "direct_normalized_rms_limit": DIRECT_NORMALIZED_RMS_LIMIT,
        "direct_error_definition": "max(0, abs(actual-reference)-2) before RMS normalization; exact silence/padding",
        "independent_fresh_seeks": seek_results, "oracle_mutations": mutations,
    }
    return report, expected_pts, blocks, samples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--output-dir", default="tests/fixtures/media-seek")
    parser.add_argument("--artifacts-dir", default="build-dir/artifacts/gacui-media-seek/fixtures")
    args = parser.parse_args()
    root = Path.cwd().resolve()
    if not (root / "CMakeLists.txt").is_file():
        raise ValueError("Run this generator from the repository root")
    output = relative_directory(args.output_dir, root)
    artifacts = relative_directory(args.artifacts_dir, root)
    report = {"ffmpeg": run([args.ffmpeg, "-version"]).stdout.decode().splitlines()[0],
              "ffprobe": run([args.ffprobe, "-version"]).stdout.decode().splitlines()[0], "fixtures": []}
    reference = ["MEDIA_SEEK 1"]
    decoded_by_name = {}
    with tempfile.TemporaryDirectory(prefix="complex-av-", dir=artifacts) as temporary:
        working = Path(temporary).resolve().relative_to(root)
        for spec in FIXTURES:
            raw, pcm = create_inputs(working, spec)
            target = output / spec["name"]
            encode(args.ffmpeg, raw, pcm, target, spec)
            result, timestamps, blocks, samples = validate(args.ffmpeg, args.ffprobe, target, working, spec)
            decoded_by_name[spec["name"]] = samples
            report["fixtures"].append(result)
            reference.append(f'FILE {spec["name"]} {spec["frames"]} {spec["samples"]} {spec["delay"]} {PCM_TOLERANCE}')
            reference.append("PTS " + " ".join(map(str, timestamps)))
            for start, values in blocks:
                reference.append(f"AUDIO {start} {len(values)}")
                reference.extend(" ".join(map(str, values[i:i + 32])) for i in range(0, len(values), 32))
            reference.append("END")
        for spec, other in ((FIXTURES[0], FIXTURES[1]), (FIXTURES[1], FIXTURES[0])):
            ratios = []
            for milliseconds in spec["blocks_ms"]:
                values = block_at(decoded_by_name[spec["name"]], milliseconds * 48)
                if any(values):
                    ratios.append(normalized_error(block_at(decoded_by_name[other["name"]], milliseconds * 48), values))
            if min(ratios) <= DIRECT_NORMALIZED_RMS_LIMIT:
                raise ValueError("A stale replacement-source block would pass")
            report.setdefault("stale_source_mutations", []).append({"expected_file": spec["name"],
                                                                    "wrong_file": other["name"],
                                                                    "minimum_normalized_error": min(ratios)})
    (output / "reference.txt").write_text("\n".join(reference) + "\n", encoding="utf-8")
    (artifacts / "validation.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
