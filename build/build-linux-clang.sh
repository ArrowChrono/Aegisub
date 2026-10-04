#!/usr/bin/env bash
set -euo pipefail

mode=${1:-all}
if (($#)); then shift; fi
case "$mode" in configure|build|test|all) ;; *) echo 'Usage: build/build-linux-clang.sh [configure|build|test|all] [CMake options...]' >&2; exit 2 ;; esac
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
[[ "$PWD" == "$repo_root" ]] || { echo 'Run this script from the repository root' >&2; exit 2; }
build_dir=${BUILD_DIR:-build-dir-linux-clang}
build_type=${BUILD_TYPE:-RelWithDebInfo}
jobs=${JOBS:-$(nproc)}
test_timeout=${TEST_TIMEOUT_SECONDS:-900}
if ! [[ "$test_timeout" =~ ^[1-9][0-9]{0,3}$ ]] || ((test_timeout > 7200)); then
    echo 'TEST_TIMEOUT_SECONDS must be an integer from 1 to 7200' >&2; exit 2;
fi

if [[ -f "$build_dir/CMakeCache.txt" ]]; then
    cached_cxx=$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$build_dir/CMakeCache.txt")
    if ! grep -q '^CMAKE_GENERATOR:INTERNAL=Ninja$' "$build_dir/CMakeCache.txt" ||
       ! "$cached_cxx" --version | grep -qi clang; then
        echo 'Existing cache is not a Ninja clang build; use a separate BUILD_DIR' >&2; exit 2;
    fi
fi
if [[ "$mode" == configure || "$mode" == all ]]; then
    "${CC:-clang}" --version | grep -qi clang
    "${CXX:-clang++}" --version | grep -qi clang
    if [[ -f "$build_dir/CMakeCache.txt" ]]; then
        if ! grep -Fxq "CMAKE_BUILD_TYPE:STRING=$build_type" "$build_dir/CMakeCache.txt"; then
            echo 'Existing cache has a different configuration; use a separate BUILD_DIR' >&2; exit 2;
        fi
        for language in C CXX; do
            compiler=$(sed -n "s/^CMAKE_${language}_COMPILER:[^=]*=//p" "$build_dir/CMakeCache.txt")
            requested=${CC:-clang}
            if [[ "$language" == CXX ]]; then requested=${CXX:-clang++}; fi
            if [[ "$(readlink -f "$compiler")" != "$(readlink -f "$(command -v "$requested")")" ]]; then
                echo 'Existing cache uses a different compiler; use a separate BUILD_DIR' >&2; exit 2;
            fi
        done
    fi
    bash build/version.sh .
    stringzilla_args=()
    if [[ -f include/stringzilla/stringzilla.h ]]; then
        stringzilla_args+=("-DSTRINGZILLA_INCLUDE_DIRS=$repo_root/include")
    fi
    cmake -S . -B "$build_dir" -G Ninja \
        -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
        -DCMAKE_BUILD_TYPE="$build_type" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=OFF \
        -DCLANG_TIDY:FILEPATH= -DCMAKE_CXX_CLANG_TIDY= \
        -DWITH_AVISYNTH=OFF -DWITH_LIBPLACEBO=OFF -DWITH_TEST=ON -DBUILD_TESTING=ON \
        "${stringzilla_args[@]}" "$@"
fi
if [[ "$mode" == build || "$mode" == all ]]; then
    cmake --build "$build_dir" --parallel "$jobs" --target Aegisub
fi
if [[ "$mode" == test || "$mode" == all ]]; then
    cmake --build "$build_dir" --parallel "$jobs" --target gtest-run font-collector-state-test
    timeout --kill-after=5s "${test_timeout}s" ctest --test-dir "$build_dir" -L gtest --no-tests=error --timeout 60 \
        --stop-on-failure --output-on-failure --output-junit clang-unit-tests.xml
fi
