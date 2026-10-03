#!/usr/bin/env bash
set -euo pipefail
# Preserve assert-based ABI/behavior checks even when the caller sets -O globally.
unset PYTHONOPTIMIZE
. "$(dirname -- "${BASH_SOURCE[0]}")/env.sh"
export FIXTURE_DIR="$BUILD_ROOT/tests/fixtures"
mkdir -p "$BUILD_ROOT/tests" "$ARTIFACTS/provenance"
"$RECIPE_ROOT/tests/make-fixtures.sh"
"$CC" -std=c11 -O2 -I"$ARTIFACTS/include" "$RECIPE_ROOT/tests/smoke.c" \
 -L"$ARTIFACTS/lib" -llsmasnative -Wl,-rpath,"$ARTIFACTS/lib" -o "$BUILD_ROOT/tests/smoke"
"$BUILD_ROOT/tests/smoke" "$FIXTURE_DIR"/{h264-aac.mp4,hevc-flac.mkv,vp9-opus.webm,av1-opus.mkv,ffv1-pcm.mkv} \
 | tee "$ARTIFACTS/provenance/native-smoke.log"
# Both upstream suites use temporary/build-root outputs. Suppress Python bytecode
# so importing their helper module does not write __pycache__ into source checkout.
PYTHONDONTWRITEBYTECODE=1 python3 "$SOURCE_ROOT/LsmasSharp/scripts/test_native_audio_reads.py" \
 --native-library "$ARTIFACTS/lib/liblsmasnative.so" --artifacts-dir "$BUILD_ROOT/tests/regression" \
 2>&1 | tee "$ARTIFACTS/provenance/upstream-audio-regression.log"
PYTHONDONTWRITEBYTECODE=1 python3 "$SOURCE_ROOT/LsmasSharp/scripts/test_native_build.py" \
 2>&1 | tee "$ARTIFACTS/provenance/upstream-build-identity-tests.log"
audit_args=()
if [[ -n "${AEGISUB_SOURCE:-}" ]]; then
 audit_args+=(--aegisub-symbols "$AEGISUB_SOURCE/vendor/lsmasnative/lsmas_native_api.functions.inc")
fi
python3 "$RECIPE_ROOT/tests/audit.py" --library "$ARTIFACTS/lib/liblsmasnative.so" \
 --header "$ARTIFACTS/include/lsmas_native.h" --report "$ARTIFACTS/provenance/abi-audit.json" "${audit_args[@]}"
