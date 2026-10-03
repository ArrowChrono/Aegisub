#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export PYTHONDONTWRITEBYTECODE=1
verify=0
parity=0
dry_run=()
illink=()
for arg in "$@"; do
    case "$arg" in
        --verify) verify=1 ;;
        --parity) verify=1; parity=1 ;;
        --dry-run) dry_run=(--dry-run) ;;
        --inprocess-illink) illink=(--inprocess-illink) ;;
        --help|-h)
            echo 'Usage: rebuild.sh [--verify] [--parity] [--dry-run] [--inprocess-illink]'
            echo 'Required: SOURCE_ROOT, BUILD_ROOT. Tools: CC, CXX, NASM, AR, PATCH, DOTNET. JOBS defaults to 2.'
            exit 0 ;;
        *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done
python3 "$HERE/build-xvid.py" --variant optimized "${dry_run[@]}"
python3 "$HERE/build-wwxd.py" "${dry_run[@]}" "${illink[@]}"
if (( parity )); then
    python3 "$HERE/build-xvid.py" --variant full "${dry_run[@]}"
fi
if (( verify )) && (( ${#dry_run[@]} == 0 )); then
    if (( parity )); then
        "$HERE/verify.sh" --parity
    else
        "$HERE/verify.sh"
    fi
fi
