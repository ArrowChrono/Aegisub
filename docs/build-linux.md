# Linux Clang build and separate runtime providers

This records the Linux x86-64 build based on `exp` commit
`82e31f01d42df763ab894609d18c9738e33c7400` and this branch's source fixes.
It is a **prepared-dependency workflow**, not a clean-machine bootstrap,
fully static application, or all-distribution release.

The providers are built by their own projects as shared libraries with private
static implementation dependencies. Aegisub consumes their C ABI through its
existing dynamic loaders. Do not fold FFmpeg, LsmasNative, Xvid, WWXD, libass,
libplacebo or AviSynth implementations into the application executable.
Generated translations, downloaded vendor trees, build caches and binaries do
not belong in this commit.

## Verified toolchain and dependencies

- Debian 13 development libraries, Clang/Clang++ 19.1.7 using libstdc++ 14,
  CMake 3.31.6, Ninja 1.12.1; Release, PIC, without IPO/LTO
- wxWidgets 3.3.1 (`wxWidgets/wxWidgets`, commit
  `49c6810948f40c457e3d0848b9111627b5b61de5`), shared GTK3 build with STC and
  GL canvas; webview, mediactrl, samples, and upstream tests disabled
- vcpkg baseline `58950f88544e4637524dbd6a01d0317cf4cb77fc`, static Release
  dependencies with dynamic platform CRT, built with Clang 19
  - Skia 146, commit `50841da4a7b7064b3cea8a851e60ef921c87a103`, baseline
    patches unchanged; only `fontconfig`, `freetype`, `harfbuzz`, `icu`, and
    `gl` features enabled, with default features disabled
  - PFFFT 1.0.0; Boost 1.90.0 with the repository's eight Boost packages,
    including ICU features for locale and regex
  - ICU 78.2, HarfBuzz 13.0.1, Fontconfig 2.17.1, FreeType 2.13.3,
    Expat 2.7.4, zlib 1.3.1, libpng 1.6.55, Brotli 1.2.0, and bzip2
- libass 0.17.5, shared provider embedding its own static dependencies:
  FreeType 2.14.3, HarfBuzz 14.2.1, FriBidi 1.0.16, libpng 1.6.55,
  zlib 1.3.2, Fontconfig 2.17.1, and Expat 2.8.3
- FFMS2 5.0 (`FFMS/ffms2`, commit
  `7ed5e4d039ca9a6236bd2ebdfdd656c4304fbe04`) and RagbagSubtitle v0.2.0
  (`MIRIMIRIM/RagbagSubtitle`, commit
  `ce859c54ada2c3b0a3b40737e357dd81b169b9e8`), shared providers with static
  FFmpeg 7.1.5 (`3a0867c2bfda4a4d4309ca1a8cbdc6175e67f587`), dav1d 1.5.4
  (`54706fc6bc0cdecab7e9593974a4039cc038fca7`), and zlib 1.3.2
- libplacebo 7.360.1 (`haasn/libplacebo`, commit
  `cee9b076f2c63104ccfd497fa79c39a867293ec4`), Vulkan loader 1.4.341,
  and libdovi 3.3.2; the external provider recipe includes Linux library-search
  and shaderc build adaptations
- AviSynth+ 3.7.5 (`AviSynth/AviSynthPlus`, commit
  `6c7c26617a6675eec89e4d4a3565ed709df6511f`), shared with bundled plugins,
  CUDA off; its external recipe patches Linux Clang to link libstdc++
  rather than libc++; optional SoundTouch/TimeStretch was not built
- AegisubBridge `dc-v0.1.0` (`MIRIMIRIM/AegisubBridge`, commit
  `5e718e4402485b800618a4d2b6d199ac0c3fd4fe`), .NET SDK 10.0.401 and
  app-local runtime 10.0.12, Linux x64 NativeAOT dependency-control library

- LsmasNative 202608 from `MIRIMIRIM/LsmasSharp`, commit
  `4c85eb4c68e3a96c18c0936652d663a30724d145`, API 1.1.0. Its own DSO embeds
  patched FFmpeg `39c24f36247f370864ce86ebdf2aa936151f4bfc`, HOE
  `7e65185d3f08ba4ad191e9a5cbba3e2c6fd3bb67`, xxHash
  `e626a72bc2321cd320e953a0ccf1584cad60f363`, dav1d 1.5.3 and zlib 1.3.1
- SceneChangeSharp v0.1.0, commit
  `51566a64eb193f130c39731c5eacc92330b463b1`, produces separate
  `libscenechange_xvid.so` and `libscenechange_wwxd.so`. Xvid uses the pinned
  1.3.7 source and original five-patch queue; WWXD uses .NET 10 NativeAOT
  (SDK 10.0.401, compiler/runtime packs 10.0.12)

Other development packages (GTK3, OpenGL, audio backends, CLI11, pugixml,
Hunspell, uchardet, FFTW, Highway, GoogleTest, and StringZilla) must already be
available. This is a mixed dependency layout, not a claim that the full root
`vcpkg.json` manifest has been validated on Linux. The packaged build evidence
contains full provider locks, package versions, and build logs.

## Prepared workspace and configuration

Initialize repository submodules and use a full-history clone for version
metadata. The checked-in `build/linux/configure.sh` translates the verified
configuration into a relocatable wrapper. It expects these prepared siblings
under `AEGISUB_DEPENDENCY_ROOT`:

- `deps/root`: extracted Debian development/toolchain prefix
- `deps/stringzilla`: StringZilla headers
- `deps-extra/installed/x64-linux-clang-release`: selected static vcpkg libraries
- `provider-wxwidgets/prefix`, `provider-libass/prefix`,
  `provider-placebo/prefix`, `provider-avisynth/prefix`
- `provider-media/artifacts`: FFMS2/Ragbag headers and runtime outputs
- `provider-bridge/sdk` and `provider-bridge/runtime`: host pack and bridge files
- `provider-lsmasnative/artifacts` and `provider-scenechange/output`: new providers

These are artifact-layout names, not downloads performed by the wrapper. See
`build/linux/providers/README.md` for the new providers' exact source pins,
prepared-source layout, patches, build options and commands. The original
full-package build evidence carries the older provider recipes and complete
source/license inventories; their prerequisites remain external here.

```sh
export AEGISUB_DEPENDENCY_ROOT=/path/to/prepared-dependencies
export AEGISUB_BUILD_DIR="$PWD/build-linux"
. ./build/linux/env.sh
./build/linux/configure.sh
cmake --build "$AEGISUB_BUILD_DIR" --parallel
cmake --build "$AEGISUB_BUILD_DIR" --target gtest-run lsmas-provider-smoke scenechange-provider-smoke
```

`AEGISUB_SOURCE_DIR` overrides the source checkout. `DEPS`, `EXTRA`, `WX`,
`CC`, `CXX` and `AEGISUB_DOTNET_HOST_PACK_NATIVE_DIR` can override their
corresponding prefixes/tools. Additional CMake arguments are passed last.
`env.sh` must be sourced in the caller shell so CMake/Ninja and their extracted
shared libraries remain available for subsequent build/test commands. The
configure wrapper also sources it for direct use, but a child shell cannot
export this setup back to its caller. Keep the chosen build directory separate from a build configured against a
different checkout. Inspect `CMakeCache.txt` to confirm the selected paths.
The wrapper retains the extracted Debian pkg-config sysroot; a system-installed
or differently arranged dependency environment needs corresponding adaptation.

The wrapper enables LsmasNative, both SceneChange backends, FFMS2, AviSynth,
libplacebo, FFTW/PFFFT, LuaSocket, Skia drawing/audio/grid, and the plugin bridge.
Enabling a compile option does not install its runtime library.

`AEGISUB_PREFER_STATIC_DEPS` defaults to `OFF`. When enabled, it selects
available static pugixml and correct ICU/Boost imported-target dependencies.
It does not make every dependency static. GTK, wxWidgets, GL and system
libraries remain shared.

### Header ordering and ELF symbol isolation

`primary-includes.cmake` puts pinned static/provider headers before broad
Debian include roots, preventing ICU/Boost/libplacebo header/library mismatch.
Keep the configure wrapper's targeted `--exclude-libs` list: exported static
HarfBuzz/FreeType/Fontconfig/image symbols previously interposed on GTK/Pango's
shared libraries and caused GUI crashes. The executable deliberately does not
use `--exclude-libs,ALL`, which could hide intended Lua/core exports. Each
provider instead controls its own private archive visibility and ABI exports.

## Runtime staging and packaging

`build/linux/runtimes.json` lists all eight separately built native runtime
boundaries. Input paths are relative to the prepared workspace; destination
names are the existing application loader names under `bin/runtimes`.
`stage-runtimes.py` requires Python 3.11 or later, checks ELF architecture and
all destination conflicts first, refuses overwrites, and emits input hashes.

```sh
python3 build/linux/stage-runtimes.py \
  --base "$AEGISUB_DEPENDENCY_ROOT" --validate
# STAGE is a new application layout, not the build or provider source tree.
STAGE=/path/to/new-staging-directory
mkdir -p "$STAGE/bin"
cp "$AEGISUB_BUILD_DIR/aegisub" "$STAGE/bin/"
python3 build/linux/stage-runtimes.py \
  --base "$AEGISUB_DEPENDENCY_ROOT" --output "$STAGE" > runtime-inputs.json
```

This helper only consumes provider outputs. It does **not** create a complete
portable package or resolve transitive shared libraries. Before distributing,
the packaging step must also:

1. Stage automation/data/translations under `share/aegisub`, GTK resources,
   licenses and corresponding source/build recipes
2. Stage bridge managed/native files under `bin/plugins`, its app-local .NET
   runtime under `bin/.dotnet`, and bridge automation under `share/aegisub/automation`
3. Preserve AviSynth plugins under `share/aegisub/runtimes/avs-plugins`
4. Collect the trusted ELF `DT_NEEDED` closure plus explicit dlopen dependencies
   (including .NET's ICU/OpenSSL); preserve required SONAME aliases. Keep the
   chosen Vulkan loader consistent and leave GL/Vulkan drivers to the host
5. Set relative RUNPATHs on staged copies only and use `build/linux/launcher.sh`
   as the package's top-level `aegisub`; preserve original provider outputs
6. Audit unresolved dependencies/exports and test the relocated package from a
   path containing spaces without build-environment library paths

The existing verified archive includes its complete manifest-driven packager
and detailed evidence under `build-info/packaging`; that larger workspace
collector is not presented here as a general distribution packaging system.

On Linux, a nonempty `AEGISUB_DATA_DIR` overrides `?data`; unset/empty retains
compiled `P_DATA`. The launcher sets it to its own `share/aegisub`, sets the
package library/data paths, and selects its app-local .NET. Managed assemblies
remain `.dll`; the Linux NativeAOT bridge uses `.so`. Host font configuration,
display/audio services and suitable graphics drivers remain requirements.

### Licensing and runtime limits

LsmasNative preserves the Windows release's GPL/version3 FFmpeg options and
upstream MOV/audio-read patches. It is not the LGPL-only configuration used by
the separate FFMS2/Ragbag providers. Preserve its GPL-3.0-or-later source and
notices. SceneChange/Xvid are GPL-2.0 builds; WWXD also needs the .NET notices.
A dynamically loaded provider boundary does not remove redistribution duties.
The source archives and license inventories in the tested package document the
actual selected inputs; this repository does not vendor those input trees.

The FFMS2/Ragbag FFmpeg build targets local software decoding, without network,
hardware decoding, encoding/muxing, filter/device support or broad optional
codec libraries. LsmasNative instead follows its upstream release options:
network/protocols and native decoder/demuxer breadth retain upstream defaults;
FFmpeg programs, devices, filters, muxers, encoders and documentation are disabled.
Do not infer identical codec or protocol support across these provider builds.
Xvid retains runtime AVX2 dispatch, but its persistent worker
pool is Windows-only; Linux uses the upstream pthread fallback. LsmasNative and
Xvid outputs require at least GLIBC 2.38; WWXD at least 2.34. The full package's
maximum glibc requirement must be audited independently. No older-distribution,
musl or Windows-feature-parity claim is made.

## Verification

The 17 source/CMake changes in this branch match the provider-enabled Clang 19
source used for the verified package. Rerunning the prepared configuration and
build before this commit produced 2,683 unit tests: **2,675 passed, 8 skipped,
zero failed**, including all three 2,048-byte logger regressions. Nine selected
core/API/provider/Lua/font-collector/CLI smoke tests also passed.

Run unit tests from the configured runtime data directory using the CMake target:

```sh
cmake --build "$AEGISUB_BUILD_DIR" --target test-aegisub
python3 build/linux/test-stage-runtimes.py
```

Provider-dependent tests need real runtime files discoverable beside the test
executables. The newly added `lsmas-provider-smoke` is `EXCLUDE_FROM_ALL` and
is not registered with CTest. Build it explicitly, then run it with a video
fixture containing non-silent audio (at least 4,096 samples):

```sh
# For a staged layout, copy the test executable next to bin/aegisub so it uses
# bin/runtimes. Supply the same prepared shared-library environment as the build.
cp "$AEGISUB_BUILD_DIR/lsmas-provider-smoke" "$STAGE/bin/"
"$STAGE/bin/lsmas-provider-smoke" /path/to/video-with-audio.mkv
```

The smoke asserts actual LsmasNative selection, decoded video/audio, and stable
first-frame/sample results across random seeks. Silent fixtures are rejected by
design. Existing `scenechange-provider-smoke` accepts a fixture, output keyframe
path, expected frame list, backend (`scxvid` or `wwxd-provider`) and optional
cancellation frame. Its optional CTest registration is controlled by
`AEGISUB_SCENECHANGE_SMOKE_VIDEO`, `..._EXPECTED` and `..._BACKEND`.

Previously completed package verification also covered:

- Five LsmasNative codec pairs: H.264/AAC, HEVC/FLAC, VP9/Opus, AV1/Opus, FFV1/PCM
- Both SceneChange backends finding exact cuts, cancellation and odd-size fallback
- Managed/CoreCLR/NativeAOT bridge lifecycle; all eight runtime boundaries loaded
- GUI Chinese subtitle display, audio spectrum, keyframe generation and seeks
- Relocation with build paths removed; all archive checksums and relative links
- Standalone LsmasNative clean-source rebuild, 16 upstream audio and 13 identity
  tests; Xvid golden sequences and full/optimized parity

The new staging helper's eight planning tests pass, all eight staged provider
files match the verified inputs byte-for-byte, and the staged LsmasNative and
SceneChange integration smokes pass. The normalized configure wrapper was run
against the existing build; this separate review worktree was not rebuilt from
scratch, and the normalized provider recipes are separately qualified in their
README. Cross-distribution, Windows and macOS builds were not run.

The optional headless playback diagnostic stalled after video-open/first seek
in **both** the preserved baseline and new provider package (15-second timeout).
No pass is claimed for it. Direct integration and actual GUI decode/seek passed
independently. No physical audio-output verification is possible on this host.
wxWidgets 3.2 and old-Fontconfig fallback paths remain from earlier attempts;
the final build uses wxWidgets 3.3.1/Fontconfig 2.17.1, so those fallback paths do
not have final-build coverage.
