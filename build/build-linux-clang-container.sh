#!/usr/bin/env bash
set -euo pipefail

artifact_dir=$(mktemp -d /artifacts/run-XXXXXXXX)
trap 'printf "%s\n" "$?" > "$artifact_dir/exit-code.txt"' EXIT
printf 'Artifacts: %s\n' "$artifact_dir"
if [[ ! -f /source/include/ragbag/subtitle_plugin_api.h ]]; then
    echo 'Prepare include/ragbag/subtitle_plugin_api.h before running the container build' >&2
    exit 2
fi
useradd --create-home --uid 1001 aegisub-build
cd /source
git -c safe.directory=/source clone --no-hardlinks . /work/source
git -c safe.directory=/source ls-files -z | tar --exclude='.git' --null -T - -cf - | tar -xf - -C /work/source
cp build/build-linux-clang.sh build/build-linux-clang-container.sh /work/source/build/
mkdir -p /work/source/include/ragbag
cp include/ragbag/subtitle_plugin_api.h /work/source/include/ragbag/
if [[ -d include/stringzilla ]]; then
    cp -r include/stringzilla /work/source/include/
fi
cd /work/source
while IFS= read -r -d '' entry; do
    if [[ "$entry" == i/lf* && "$entry" == *w/crlf* ]]; then
        sed -i 's/\r$//' "${entry#*$'\t'}"
    fi
done < <(git ls-files --eol -z)
find . -path './.git' -prune -o -type f -name '*.sh' -exec sed -i 's/\r$//' {} +
chown -R aegisub-build:aegisub-build /work/source
{
    git -c safe.directory=/work/source rev-parse HEAD
    sha256sum include/ragbag/subtitle_plugin_api.h
    if [[ -f include/stringzilla/stringzilla.h ]]; then
        grep -E '^#define STRINGZILLA_H_VERSION_(MAJOR|MINOR|PATCH)' include/stringzilla/stringzilla.h
    fi
    clang --version
    cmake --version
    locale
    dpkg-query -W
} > "$artifact_dir/environment.txt"

set +e
timeout --kill-after=5s 1800s runuser -u aegisub-build -- env JOBS="${JOBS:-8}" BUILD_DIR=build-dir-linux-clang \
    bash build/build-linux-clang.sh all \
    -DWITH_LSMASNATIVE=OFF -DWITH_SCENECHANGE=OFF -DWITH_PLUGIN_BRIDGE=OFF \
    2>&1 | tee "$artifact_dir/build.log"
status=${PIPESTATUS[0]}
set -e
if [[ -f build-dir-linux-clang/aegisub ]]; then
    cp build-dir-linux-clang/aegisub "$artifact_dir/"
    (cd "$artifact_dir" && sha256sum aegisub > aegisub.sha256)
    ldd build-dir-linux-clang/aegisub > "$artifact_dir/aegisub-ldd.txt"
fi
if [[ -f build-dir-linux-clang/clang-unit-tests.xml ]]; then
    cp build-dir-linux-clang/clang-unit-tests.xml "$artifact_dir/"
fi
if [[ -f build-dir-linux-clang/gtest-run ]]; then
    cp build-dir-linux-clang/gtest-run "$artifact_dir/"
fi
if [[ -d build-dir-linux-clang/Testing ]]; then
    cp -r build-dir-linux-clang/Testing "$artifact_dir/"
fi
exit "$status"
