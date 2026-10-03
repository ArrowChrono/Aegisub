#!/bin/sh
# Source this in the caller shell before configuring, building or running tests.
: "${AEGISUB_DEPENDENCY_ROOT:?Set AEGISUB_DEPENDENCY_ROOT to the prepared dependency workspace}"
AEGISUB_DEPENDENCY_ROOT=$(CDPATH= cd -- "$AEGISUB_DEPENDENCY_ROOT" && pwd)
DEPS=${DEPS:-$AEGISUB_DEPENDENCY_ROOT/deps/root}
export AEGISUB_DEPENDENCY_ROOT DEPS
export PATH="$DEPS/usr/bin:$PATH"
export LD_LIBRARY_PATH="${WX:-$AEGISUB_DEPENDENCY_ROOT/provider-wxwidgets/prefix}/lib:$DEPS/usr/lib/x86_64-linux-gnu:$DEPS/usr/lib/x86_64-linux-gnu/pulseaudio${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PKG_CONFIG_LIBDIR="$DEPS/usr/lib/x86_64-linux-gnu/pkgconfig:$DEPS/usr/lib/pkgconfig:$DEPS/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$DEPS"
export CMAKE_PREFIX_PATH="$DEPS/usr"
