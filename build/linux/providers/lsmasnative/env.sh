#!/usr/bin/env bash
# Sourced by recipe entry points. Outputs belong only to the explicit build root.
RECIPE_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
: "${SOURCE_ROOT:?Set SOURCE_ROOT to the prepared source parent containing LsmasSharp, dav1d and zlib}"
: "${BUILD_ROOT:?Set BUILD_ROOT to a separate writable build directory}"
SOURCE_ROOT=$(cd -- "$SOURCE_ROOT" && pwd)
mkdir -p -- "$BUILD_ROOT"
BUILD_ROOT=$(cd -- "$BUILD_ROOT" && pwd)
for protected in "$SOURCE_ROOT" "$RECIPE_ROOT"; do
    if [[ "$BUILD_ROOT" == "$protected" || "$BUILD_ROOT" == "$protected"/* || "$protected" == "$BUILD_ROOT"/* ]]; then
        echo 'BUILD_ROOT must be disjoint from source and recipe directories' >&2
        return 1
    fi
done
export RECIPE_ROOT SOURCE_ROOT BUILD_ROOT
export CC=${CC:-clang-19} CXX=${CXX:-clang++-19} JOBS=${JOBS:-3}
export PREFIX="$BUILD_ROOT/prefix" ARTIFACTS="$BUILD_ROOT/artifacts"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
unset PKG_CONFIG_SYSROOT_DIR
"$CC" --version | grep -m1 -E 'clang version 19\.' >/dev/null || { echo 'Clang 19 is required' >&2; return 1; }
mkdir -p "$BUILD_ROOT/logs" "$PREFIX" "$ARTIFACTS/provenance"
