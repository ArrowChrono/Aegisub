#!/usr/bin/env bash
# Separate entry point also supports validating the native recipe against an
# already prepared, matching static media prefix without rebuilding FFmpeg.
set -euo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
python3 "$RECIPE_ROOT/prepare-sources.py" --sources "$SOURCE_ROOT" --verify-only
cmake -S "$RECIPE_ROOT" -B "$BUILD_ROOT/native" -G Ninja \
 -DCMAKE_C_COMPILER="$CC" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$ARTIFACTS" \
 -DLSMAS_SOURCE="$SOURCE_ROOT/LsmasSharp" \
 -DLSW_SOURCE="${LSW_SOURCE:-$BUILD_ROOT/prepared/hoe}" \
 -DFFMPEG_PREFIX="${FFMPEG_PREFIX:-$PREFIX}" > "$BUILD_ROOT/logs/native-configure.log" 2>&1
cmake --build "$BUILD_ROOT/native" -j "$JOBS" > "$BUILD_ROOT/logs/native-build.log" 2>&1
cmake --install "$BUILD_ROOT/native" >> "$BUILD_ROOT/logs/native-build.log" 2>&1
strip --strip-unneeded -o "$ARTIFACTS/lib/liblsmasnative.so.new" "$ARTIFACTS/lib/liblsmasnative.so"
mv "$ARTIFACTS/lib/liblsmasnative.so.new" "$ARTIFACTS/lib/liblsmasnative.so"
python3 "$RECIPE_ROOT/collect-metadata.py"
