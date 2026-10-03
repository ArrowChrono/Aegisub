#!/usr/bin/env bash
set -euo pipefail
. "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
python3 "$RECIPE_ROOT/prepare-sources.py" --sources "$SOURCE_ROOT" --build "$BUILD_ROOT"
# Existing output trees are incremental caches for this lock/toolchain. For changed
# compiler or configuration, use a new BUILD_ROOT; never share across toolchains.
cmake -S "$BUILD_ROOT/prepared/zlib" -B "$BUILD_ROOT/zlib" -G Ninja \
 -DCMAKE_C_COMPILER="$CC" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
 -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_C_FLAGS=-fvisibility=hidden > "$BUILD_ROOT/logs/zlib-configure.log" 2>&1
cmake --build "$BUILD_ROOT/zlib" --target zlibstatic -j "$JOBS" > "$BUILD_ROOT/logs/zlib-build.log" 2>&1
mkdir -p "$PREFIX/lib/pkgconfig" "$PREFIX/include"
cp "$BUILD_ROOT/zlib/libz.a" "$PREFIX/lib/"
cp "$BUILD_ROOT/prepared/zlib/zlib.h" "$BUILD_ROOT/zlib/zconf.h" "$PREFIX/include/"
cp "$BUILD_ROOT/zlib/zlib.pc" "$PREFIX/lib/pkgconfig/"
meson_args=()
[[ ! -f "$BUILD_ROOT/dav1d/build.ninja" ]] || meson_args+=(--reconfigure)
CFLAGS='-fPIC -fvisibility=hidden' meson setup "${meson_args[@]}" "$BUILD_ROOT/dav1d" "$SOURCE_ROOT/dav1d" \
 --prefix="$PREFIX" --libdir=lib --default-library=static --buildtype=release \
 -Denable_tools=false -Denable_tests=false > "$BUILD_ROOT/logs/dav1d-configure.log" 2>&1
ninja -C "$BUILD_ROOT/dav1d" -j "$JOBS" install > "$BUILD_ROOT/logs/dav1d-build.log" 2>&1
mkdir -p "$BUILD_ROOT/ffmpeg"
(
 cd "$BUILD_ROOT/ffmpeg"
 "$BUILD_ROOT/prepared/ffmpeg/configure" --prefix="$PREFIX" --arch=x86_64 --target-os=linux \
  --cc="$CC" --cxx="$CXX" --enable-gpl --enable-version3 --enable-static --disable-shared --enable-pic \
  --enable-avcodec --enable-avformat --enable-swscale --enable-swresample \
  --disable-programs --disable-avdevice --disable-avfilter --disable-encoders --disable-muxers --disable-doc --disable-debug \
  --disable-autodetect --enable-libdav1d --enable-zlib --pkg-config-flags=--static \
  --extra-cflags="-O2 -fPIC -fvisibility=hidden -I$PREFIX/include" --extra-ldflags="-L$PREFIX/lib" \
  > "$BUILD_ROOT/logs/ffmpeg-configure.log" 2>&1
 make -j"$JOBS" > "$BUILD_ROOT/logs/ffmpeg-build.log" 2>&1
 make install >> "$BUILD_ROOT/logs/ffmpeg-build.log" 2>&1
)
printf '#pragma once\n#define LSMAS_FFMPEG_MOV_AUDIO_END_FIXED 1\n' > "$PREFIX/include/lsmas_ffmpeg_contract.h"
"$RECIPE_ROOT/build-native.sh"
