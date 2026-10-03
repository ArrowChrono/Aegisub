#!/bin/sh
set -eu
# Prepared-prefix configuration, not a dependency bootstrap.
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SOURCE=${AEGISUB_SOURCE_DIR:-$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)}
: "${AEGISUB_DEPENDENCY_ROOT:?Set AEGISUB_DEPENDENCY_ROOT to the prepared dependency workspace}"
BASE=$(CDPATH= cd -- "$AEGISUB_DEPENDENCY_ROOT" && pwd)
BUILD=${AEGISUB_BUILD_DIR:-$SOURCE/build-linux}
DEPS=${DEPS:-$BASE/deps/root}
EXTRA=${EXTRA:-$BASE/deps-extra/installed/x64-linux-clang-release}
WX=${WX:-$BASE/provider-wxwidgets/prefix}
HOST=${AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR:-$BASE/provider-bridge/sdk/packs/Microsoft.NETCore.App.Host.linux-x64/10.0.12/runtimes/linux-x64/native}
CC=${CC:-$DEPS/usr/bin/clang-19}
CXX=${CXX:-$DEPS/usr/bin/clang++-19}
. "$SCRIPT_DIR/env.sh"
cmake -S "$SOURCE" -B "$BUILD" -G Ninja \
 -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_C_COMPILER="$CC" \
 -DCMAKE_CXX_COMPILER="$CXX" \
 -DCMAKE_INSTALL_PREFIX=/usr \
 -DCMAKE_PROJECT_INCLUDE="$SCRIPT_DIR/primary-includes.cmake" \
 -DAEGISUB_PRIMARY_DEPENDENCY_INCLUDE="$EXTRA/include;$BASE/provider-libass/prefix/include;$BASE/provider-placebo/prefix/include;$BASE/provider-media/artifacts/include;$BASE/provider-media/artifacts/include/ffms2" \
 -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
 -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=OFF \
 -DCMAKE_PREFIX_PATH="$EXTRA;$WX;$DEPS/usr" \
 -DCMAKE_LIBRARY_PATH="$EXTRA/lib;$WX/lib;$DEPS/usr/lib/x86_64-linux-gnu" \
 -DCMAKE_INCLUDE_PATH="$EXTRA/include;$DEPS/usr/include" \
 -DCMAKE_EXE_LINKER_FLAGS="-L$DEPS/usr/lib/x86_64-linux-gnu -Wl,--exclude-libs,libharfbuzz.a:libharfbuzz-subset.a:libfreetype.a:libfontconfig.a:libpng16.a:libz.a:libexpat.a:libbrotlicommon.a:libbrotlidec.a:libbz2.a" \
 -DwxWidgets_CONFIG_EXECUTABLE="$WX/bin/wx-config" \
 -DBoost_USE_STATIC_LIBS=ON -DAEGISUB_PREFER_STATIC_DEPS=ON \
 -DZLIB_LIBRARY_RELEASE="$EXTRA/lib/libz.a" -DZLIB_INCLUDE_DIR="$EXTRA/include" \
 -DHUNSPELL_LIBRARIES="$DEPS/usr/lib/x86_64-linux-gnu/libhunspell-1.7.a" \
 -Duchardet_LIBRARIES="$DEPS/usr/lib/x86_64-linux-gnu/libuchardet.a" \
 -Dass_INCLUDE_DIR="$BASE/provider-libass/prefix/include" \
 -Dass_RUNTIME_LIBRARY="$BASE/provider-libass/prefix/lib/libass.so.9" \
 -DFFMS2_INCLUDE_DIR="$BASE/provider-media/artifacts/include/ffms2" \
 -DLibPlacebo_INCLUDE_DIR="$BASE/provider-placebo/prefix/include" \
 -DAviSynth_INCLUDE_DIR="$BASE/provider-avisynth/prefix/include/avisynth" \
 -DSTRINGZILLA_INCLUDE_DIRS="$BASE/deps/stringzilla/include" \
 -DAEGISUB_USE_STRINGZILLA=ON -DLUA_WITH_LUASOCKET=ON \
 -DWITH_AVISYNTH=ON -DWITH_FFMS2=ON -DWITH_LIBPLACEBO=ON \
 -DWITH_PFFFT=ON -DWITH_FFTW3=ON \
 -DWITH_DRAWING_SKIA=ON -DWITH_SKIA_AUDIO_DISPLAY=ON -DWITH_SKIA_SUBTITLE_GRID=ON \
 -DWITH_PLUGIN_BRIDGE=ON \
 -DAEGISUB_DOTNET_HOST_PACK_NATIVE_DIR="$HOST" \
 -DAEGISUB_PLUGIN_BRIDGE_RUNTIME_DIR="$BASE/provider-bridge/runtime" \
 -DWITH_LSMASNATIVE=ON -DWITH_SCENECHANGE=ON \
 -DWITH_TEST=ON -DWITH_SMOKE=ON \
 "$@"
