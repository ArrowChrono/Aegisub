#!/bin/bash
set -euo pipefail
: "${FIXTURE_DIR:?Set FIXTURE_DIR to a writable test-output directory}"
mkdir -p "$FIXTURE_DIR"
cd "$FIXTURE_DIR"
FFMPEG=${FFMPEG:-ffmpeg}
# System FFmpeg is a fixture generator only; these executables are not bundled.
common=(-hide_banner -loglevel error -y -f lavfi -i testsrc2=size=96x64:rate=12:duration=2 -f lavfi -i sine=frequency=440:sample_rate=48000:duration=2 -threads 2 -shortest)
"$FFMPEG" "${common[@]}" -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac h264-aac.mp4
"$FFMPEG" "${common[@]}" -c:v libx265 -preset ultrafast -x265-params pools=1:frame-threads=1:log-level=error -c:a flac hevc-flac.mkv
"$FFMPEG" "${common[@]}" -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -c:a libopus vp9-opus.webm
"$FFMPEG" "${common[@]}" -c:v libaom-av1 -cpu-used 8 -row-mt 0 -crf 45 -c:a libopus av1-opus.mkv
"$FFMPEG" "${common[@]}" -c:v ffv1 -c:a pcm_s16le ffv1-pcm.mkv
