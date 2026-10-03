#!/usr/bin/env bash
set -euo pipefail
# Preserve assert-based ABI/behavior checks even when the caller sets -O globally.
unset PYTHONOPTIMIZE
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export PYTHONDONTWRITEBYTECODE=1
exec python3 "$HERE/verify.py" "$@"
