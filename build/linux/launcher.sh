#!/bin/sh
set -eu
SELF=$(readlink -f -- "$0")
APPDIR=$(CDPATH= cd -- "$(dirname -- "$SELF")" && pwd)
export AEGISUB_DATA_DIR="$APPDIR/share/aegisub"
export LD_LIBRARY_PATH="$APPDIR/lib:$APPDIR/bin/runtimes${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export XDG_DATA_DIRS="$APPDIR/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
if [ -f "$APPDIR/share/glib-2.0/schemas/gschemas.compiled" ]; then
    export GSETTINGS_SCHEMA_DIR="$APPDIR/share/glib-2.0/schemas"
fi
# Host source probes <executable>/.dotnet; this explicit value also prevents a
# caller's unrelated SDK installation from changing the chosen bridge runtime.
if [ -d "$APPDIR/bin/.dotnet" ]; then
    export AEGISUB_DOTNET_ROOT="$APPDIR/bin/.dotnet"
fi
exec "$APPDIR/bin/aegisub" "$@"
