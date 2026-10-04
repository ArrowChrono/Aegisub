# Building with Clang

Run all commands from the repository root. Use separate build directories for
MSVC, clang-cl and Linux clang; CMake cannot switch compiler or generator safely
in an existing cache. The scripts do not delete build directories. They stop on
configure, build or test failures.

## Windows: clang-cl, Ninja and vcpkg

This is an MSVC-ABI build, not a MinGW build. It still needs MSVC headers,
libraries, the Windows SDK resource tools and MASM. Both the application and
vcpkg ports use clang-cl. The `x64-windows-clang-cl` triplet preserves the
static/dynamic library choices and release-only dependencies of
`x64-windows-release`, but enables compiler tracking for the clang dependency
cache. Do not share its installed directory with the MSVC triplet.

### Prerequisites

- Visual Studio 2022/2026 or Build Tools with Desktop development with C++,
  C++ Clang tools for Windows, CMake/Ninja tools, and Windows SDK 10.0.26100.0.
- PowerShell 7, Git, Python 3 and gettext tools (`msgfmt` for translations).
- vcpkg with the baseline from `vcpkg.json`. Bootstrap it before configuring.
  Set `VCPKG_ROOT` or pass `-VcpkgRoot`; otherwise the script uses VS's bundled
  vcpkg. The selected checkout must be able to resolve that baseline.
- The external headers/runtime assets used by the Windows build. The existing
  dependency preparation entry point is:

  ```powershell
  dotnet build/package-win-portable.cs -- --mode prepare-dependency --cache-dir .cache/package-win-portable
  ```

  This requires the .NET SDK specified by that packaging script. The vcpkg
  manifest alone does not supply libass, FFMS2 or AviSynth+ headers. Prepared
  files belong under the repository's `include` and runtime staging directories;
  do not put machine-local paths into checked-in scripts.
- RagBag's public plugin ABI header at `include/ragbag/subtitle_plugin_api.h`.
  This header is required by the core secondary-subtitle decoder even without
  a RagBag runtime installed; the dependency preparation above supplies it from
  the RagBag release listed in `build/package-win-portable.thirdparty.json`.
  Alternatively set `Ragbag_INCLUDE_DIR` to an include root
  containing `ragbag/subtitle_plugin_api.h` during configuration.

### Commands

```powershell
pwsh -File ./build/build-win-clang.ps1 -Mode all -Jobs 8

# Separate stages; build/test do not explicitly reconfigure the cache.
pwsh -File ./build/build-win-clang.ps1 -Mode configure
pwsh -File ./build/build-win-clang.ps1 -Mode build -Jobs 8
pwsh -File ./build/build-win-clang.ps1 -Mode test -Jobs 8
```

`all` configures, builds `Aegisub`, builds the C++ unit-test executables, and runs
CTest's `gtest` label. `test` is not a GUI/provider E2E run. It emits
`clang-unit-tests.xml` inside the build directory; CTest's logs are under
`Testing/Temporary`. Tests have a 60-second per-test timeout and a 15-minute total
CTest timeout (`-TestTimeoutSeconds`), stop on the first failure, and fail if no
tests are discovered.

The script finds VS through `vswhere`, initializes its x64 developer environment,
and uses the LLVM/CMake/Ninja bundled with that installation. `-VsInstallPath`
selects another installation without editing the script. All SDK include roots,
including WinRT, come from that developer environment and are passed through to
vcpkg port builds. `-WindowsSdkVersion` selects an installed SDK; the default
26100 version also avoids the OpenGL port's SDK whitelist problem observed with
the newer 28000 SDK. The SDK selected for dependency builds is not silently
different from the application SDK.

Ninja is single-config: `-Configuration` is passed to `CMAKE_BUILD_TYPE` at
configure time, not selected by `cmake --build --config`. Defaults reproduce the
existing `build-dir-clang` **Release** configuration. For a new debug-symbol build:

```powershell
pwsh -File ./build/build-win-clang.ps1 -Mode all -BuildDir build-dir-clang-rwdi -Configuration RelWithDebInfo
```

Dependencies remain release-only for either optimized configuration. The script
supports Release and RelWithDebInfo, not Debug. A Debug build needs a separate
triplet/installed tree with appropriate debug dependencies.

Additional configure options can be passed from PowerShell:

```powershell
./build/build-win-clang.ps1 -Mode configure -BuildDir build-dir-clang-new -CMakeArguments @('-DWITH_PLUGIN_BRIDGE=OFF')
```

### Settings carried over from build-dir-clang

- `Ninja`, `Release`, `x64-windows-clang-cl` for both target and host triplets.
- Shared bundled LuaJIT, LuaSocket, AviSynth+, FFMS2, LsmasNative, SceneChange,
  libplacebo, FFTW3/PFFFT, Matroska parsing and Plugin Bridge enabled.
- Skia drawing/audio/grid enabled, Skia visual tools disabled. This is not an
  effort to maintain the deprecated visual tools.
- Release IPO/LTO disabled. CMake checks Release IPO even when configuring
  another build type; it must not become an accidental toolchain prerequisite.
- `CLANG_TIDY` and `CMAKE_CXX_CLANG_TIDY` empty. This project automatically enables
  tidy when `CLANG_TIDY` is found, so clearing only `CMAKE_CXX_CLANG_TIDY` is not
  sufficient. Use a separate analysis directory for static analysis.
- No `/MP`; Ninja owns compilation parallelism. `CMAKE_EXPORT_COMPILE_COMMANDS`
  is now enabled for inspection and separate clang-tidy runs.
- `CMAKE_NINJA_CMCLDEPS_RC=0` and the direct `rc.exe` rule are applied to both
  application and port builds. CMake's `cmcldeps` can otherwise feed the RC-only
  `/c65001` switch to clang-cl's preprocessing step and fail. This bypasses the
  dependency-scanning wrapper, not resource compilation: `rc.exe` still produces
  resources for icons, version metadata and manifests. The tradeoff is reduced
  automatic tracking of files included by `.rc`; after changing an included
  header/icon, make sure the resource object is rebuilt (or use a clean build).

The old generated `build-dir-clang/configure-clang.cmd` and local triplets are
not source-controlled and contain machine-specific paths. Keep an existing
working cache for incremental `build`/`test` (pass its SDK version), or configure
a new directory with the checked-in script. Moving from those local triplets to
the checked-in triplet can change vcpkg ABI hashes and rebuild dependencies;
this is not a no-cost cache migration. Do not overwrite another build's settings
just to reuse its directory.

Use the same PowerShell executable/version when reusing a Windows dependency
cache. vcpkg's ABI inputs include PowerShell, not only the compiler; changing
the selected PowerShell can invalidate packages and their dependent helper
ports even if the CMake version, triplet and port sources are unchanged.
`VCPKG_DISABLE_COMPILER_TRACKING` does not disable these other tool inputs.
Check the actual shell and the resolved `pwsh.exe`, rather than assuming that
an app-bundled PowerShell and the WindowsApps entry point are interchangeable.

### Existing compatibility commits

The build procedure assumes the compatibility changes already on this branch:

- `18d66e031`: cross-platform compiler/header compatibility, including wxWidgets
  3.2 header handling and platform-specific APIs.
- `f7520c8ac`: dependency target corrections and LsmasNative provider smoke
  coverage, including optional static-dependency target handling.
- `25763a607`: clang-cl incompatibilities exposed by the Windows build, including
  Boost stable_vector destructor handling and bundled LuaJIT intrinsic/header
  handling.

These were unpushed relative to `origin/exp` when this procedure was consolidated.
They are prerequisites already in the source, not extra patches to apply during
every build. Do not replace clang-cl with `clang++` while retaining these Windows
MSVC flags/triplets.

## Linux: clang/clang++ and distro dependencies

Use the native clang drivers, not clang-cl, and do not inherit the Windows
presets/triplets. The default script uses distro packages, Ninja and
`RelWithDebInfo`; it does not invoke vcpkg. Bundled LuaJIT is built from source;
system Lua/LuaJIT is not a replacement for it.
The script disables AviSynth by default because the distro package baseline
does not include AviSynth+ development headers. To enable it, install those
headers and pass `-DWITH_AVISYNTH=ON` with `-DAviSynth_INCLUDE_DIR` as needed.
The distro baseline also disables libplacebo: Ubuntu 24.04's libplacebo 6 headers
do not provide `PL_ALPHA_NONE`, which the current renderer uses. With compatible
newer headers and runtime, pass `-DWITH_LIBPLACEBO=ON` explicitly. A successful
baseline build does not validate this optional renderer.

For Debian/Ubuntu with wxWidgets 3.2, a starting dependency set is:

```sh
sudo apt-get update
sudo apt-get install clang g++ cmake ninja-build git pkg-config gettext python3 \
    libass-dev libboost-container-dev libboost-locale-dev libboost-regex-dev \
    libicu-dev zlib1g-dev libfontconfig1-dev \
    libwxgtk3.2-dev libgl1-mesa-dev libhwy-dev libcli11-dev libpugixml-dev \
    libebml-dev libmatroska-dev libgtest-dev libasound2-dev \
    libhunspell-dev libuchardet-dev libffms2-dev libfftw3-dev libplacebo-dev
```

Package availability/names vary by release. In particular CMake needs Boost
container/locale/regex config packages, Highway, CLI11, pugixml and the wxWidgets
`adv base core gl propgrid stc` components. Fontconfig is required on Linux.
Boost Spirit is header-only and does not need a `Boost::spirit` CMake component.
Keep the default libstdc++ toolchain; selecting libc++ is a separate dependency
ABI decision. PFFFT can be supplied separately; missing optional packages do not
prove those features were built.

For the default `AEGISUB_USE_STRINGZILLA=ON` feature/test profile, provide the
StringZilla 4.6.0 headers matching the vcpkg baseline. A no-StringZilla build
compiles, but currently fails `utf8_icase_searcher_can_be_reused`: the cached
helper's ASCII-only fallback does not satisfy that test's Unicode contract.
That configuration is not claimed to pass. The Linux script recognizes headers
in `include/stringzilla`, or you can pass `STRINGZILLA_INCLUDE_DIRS` explicitly.
Do not remove the failed test to validate a no-StringZilla build.

For a checkout with Windows vcpkg dependencies already installed, copy the
`stringzilla` include directory from the selected triplet into `include`.
For a native Linux checkout, the matching headers are under `include/stringzilla`
in the [StringZilla v4.6.0 source archive](https://github.com/ashvardanian/StringZilla/archive/refs/tags/v4.6.0.tar.gz).
Only the headers are needed by this project.

Also prepare RagBag's public plugin ABI header in
`include/ragbag/subtitle_plugin_api.h`, or configure `Ragbag_INCLUDE_DIR` to
another include root containing that file. It is not a distro/vcpkg package.
The core compiles the plugin-loading interface unconditionally, even when
Plugin Bridge is disabled; Linux needs this platform-neutral header, not a
Windows DLL. CMake now discovers this dependency explicitly rather than finding
it accidentally through Windows' libass include root.

The Windows dependency preparation already downloads that header. For a native
Linux checkout, extract only `subtitle_plugin_api.h` from the
[RagBag v0.2.0 release archive](https://github.com/MIRIMIRIM/RagbagSubtitle/releases/download/v0.2.0/ragbag-subtitle-ffmpeg-windows-x64-v0.2.0.zip)
listed in `build/package-win-portable.thirdparty.json`. The header itself is a
cross-platform C ABI; do not use the archive's Windows DLL on Linux. For example:

```sh
python3 - <<'PY'
from pathlib import Path
from urllib.request import urlretrieve
from zipfile import ZipFile

archive = Path('build-dir-linux-clang/downloads/ragbag-v0.2.0.zip')
archive.parent.mkdir(parents=True, exist_ok=True)
urlretrieve('https://github.com/MIRIMIRIM/RagbagSubtitle/releases/download/v0.2.0/ragbag-subtitle-ffmpeg-windows-x64-v0.2.0.zip', archive)
header = Path('include/ragbag/subtitle_plugin_api.h')
header.parent.mkdir(parents=True, exist_ok=True)
with ZipFile(archive) as package:
    header.write_bytes(package.read('subtitle_plugin_api.h'))
PY
```

```sh
export LANG=C.UTF-8 LC_ALL=C.UTF-8
git submodule update --init --recursive
bash build/build-linux-clang.sh all \
    -DWITH_LSMASNATIVE=OFF -DWITH_SCENECHANGE=OFF -DWITH_PLUGIN_BRIDGE=OFF

# Incremental stages
bash build/build-linux-clang.sh build
bash build/build-linux-clang.sh test

# Another compiler version/build type must use another build directory.
CC=clang-18 CXX=clang++-18 BUILD_DIR=build-dir-linux-clang18 BUILD_TYPE=Release \
    bash build/build-linux-clang.sh all \
    -DWITH_LSMASNATIVE=OFF -DWITH_SCENECHANGE=OFF -DWITH_PLUGIN_BRIDGE=OFF
```

`BUILD_DIR`, `BUILD_TYPE`, `JOBS`, `CC`, `CXX`, and `TEST_TIMEOUT_SECONDS` configure the script. Extra
arguments after the mode go to CMake during `configure`/`all`. `build`/`test`
reuse the configured directory. Unit-test selection, timeout and JUnit artifact
match the Windows script. Lua automation tests (`test-automation`) additionally
need LuaRocks/Busted/MoonScript and are not included in this C++ test stage.
`TEST_TIMEOUT_SECONDS` must be an integer from 1 to 7200. Linux timeout commands
allow at most five additional seconds for SIGTERM handling before forcing SIGKILL.

The explicit runtime-provider switches above give a distro-only baseline.
Enable those providers only with their Linux runtime dependencies available;
Windows NativeAOT DLLs are not Linux binaries. If optional Skia renderers are
needed, supply a Linux Skia package and enable the corresponding options
explicitly. `AEGISUB_PREFER_STATIC_DEPS=ON` is optional, not a clang requirement;
use it only when the intended static libraries exist, including ICU dependencies.

For manual configuration equivalent to the script's compiler selection:

```sh
export LANG=C.UTF-8 LC_ALL=C.UTF-8
bash build/version.sh .
cmake -S . -B build-dir-linux-clang -G Ninja \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_TEST=ON -DBUILD_TESTING=ON \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE=OFF \
    -DCLANG_TIDY:FILEPATH= -DCMAKE_CXX_CLANG_TIDY= \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DSTRINGZILLA_INCLUDE_DIRS="$PWD/include" \
    -DWITH_AVISYNTH=OFF -DWITH_LIBPLACEBO=OFF \
    -DWITH_LSMASNATIVE=OFF -DWITH_SCENECHANGE=OFF -DWITH_PLUGIN_BRIDGE=OFF
cmake --build build-dir-linux-clang --parallel --target Aegisub gtest-run font-collector-state-test
timeout --kill-after=5s 900s ctest --test-dir build-dir-linux-clang -L gtest --no-tests=error --timeout 60 \
    --stop-on-failure --output-on-failure --output-junit clang-unit-tests.xml
```

Do not infer Linux build success from a Windows clang-cl build. Validate on the
target distro, and retain the actual configure/build logs and JUnit output.

### Linux containers with WSLC (no Docker installation)

WSL 3's `wslc` CLI can build Dockerfile-format images and run containers natively;
it does not require Docker Engine or Docker Desktop. Confirm that `wslc version`
works before starting. `build/Dockerfile.linux-clang` supplies the distro
dependency baseline without installing development packages into the normal
WSL distro.
The image sets `LANG` and `LC_ALL` to `C.UTF-8`: the existing Boost.Locale tests
initialize the system locale and require UTF-8 Unicode case-folding. A bare
rootfs with the `C`/ASCII locale is not an equivalent test environment.

From the repository root in Windows PowerShell:

```powershell
New-Item -ItemType Directory -Force build-dir-linux-clang-wslc | Out-Null
wslc build -t aegisub-linux-clang:ubuntu24.04 -f build/Dockerfile.linux-clang --iidfile build-dir-linux-clang-wslc/image-id.txt build
if ($LASTEXITCODE -ne 0) { throw 'WSLC image build failed' }
wslc run --rm -e JOBS=8 `
    --mount "type=bind,source=$($PWD.Path),target=/source,readonly" `
    --mount "type=bind,source=$($PWD.Path)/build-dir-linux-clang-wslc,target=/artifacts" `
    aegisub-linux-clang:ubuntu24.04 bash /source/build/build-linux-clang-container.sh
if ($LASTEXITCODE -ne 0) { throw 'Linux clang build/tests failed' }
```

The container runner clones Git history into its Linux filesystem, overlays
tracked working-tree files, and includes the new Linux build scripts. Other
untracked/ignored source inputs are not copied except the required RagBag public
ABI header at `include/ragbag/subtitle_plugin_api.h` and a prepared
`include/stringzilla` header directory if present. Source is mounted read-only;
existing Windows products and private `CMakeLists.user.txt` are not imported.
Tracked files stored as LF in Git have Windows CRLF converted back to LF only
in the container copy, including resource manifests. Shell files are also
normalized, and `.gitattributes` keeps shell entry points LF on new checkouts. Builds and tests
run as a non-root user, which matters for permission-related unit tests.
`environment.txt` (including effective locale), `build.log`, the compiled `aegisub` with checksum/linkage
information (if built), `clang-unit-tests.xml` (if tests ran) and `Testing`
are exported to a fresh `run-XXXXXXXX` subdirectory in the host artifact directory,
with `exit-code.txt` recording this run's final status. Old successful outputs
are never mixed with a newer failed run. The runner preserves failure status
through `tee` and limits the build/test stage to 30 minutes plus a five-second
forced-termination grace period.
The `gtest-run` executable is also exported when built, for focused reruns in
the same validation image without recompiling.

Each run starts a fresh build tree inside the container; artifacts survive
`--rm`, but object files do not. This container baseline validates compilation
and headless C++ tests, not GUI launch, GPU rendering or audio-device behavior.
Those need a separate display/device-enabled E2E environment.

If the session cannot reach Docker Hub, `wslc import <rootfs-tarball>
aegisub-ubuntu-base:24.04` can import a verified Ubuntu official rootfs. Supply
`--build-arg BASE_IMAGE=aegisub-ubuntu-base:24.04` to `wslc build`. Verify the
download against Ubuntu's published SHA256SUMS before importing. This avoids
the registry fetch, but apt still needs network access. If a local proxy is
required, pass its container-reachable address using the standard `http_proxy`
and `https_proxy` build arguments; host loopback is exposed by WSLC as
`host.wslc.internal`, not the container's `127.0.0.1`. Do not check personal proxy
addresses or credentials into the Dockerfile.

The Dockerfile fixes the distro release, not every apt package version or image
digest. Record the built image ID and compiler versions with validation results;
strict bit-for-bit reproducibility requires additionally pinning the base digest
and package repository snapshot. Do not claim a container run succeeded merely
because the Dockerfile exists.
