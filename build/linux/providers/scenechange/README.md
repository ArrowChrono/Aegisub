# SceneChangeSharp v0.1.0 providers for Linux x86-64

Small, standalone build recipes for Aegisub's two loadable scene-change providers.
They use the same source release and Xvid feature selection as the Windows
`scenechange_wwxd-v0.1.0-win-x64.zip` and
`scenechange_xvid-v0.1.0-win-x64.zip` assets. No upstream sources, binaries,
download caches, generated media or build outputs are vendored here.

These are an adaptation of the successfully built and tested local Linux
recipes. The portable refactoring was checked with dry runs, command equivalence
and tests against the existing verified binaries; it has not received a fresh
end-to-end NativeAOT rebuild. It is not an empty-machine bootstrap or a promise
of bit-identical output.

## External sources and exact pins

Prepare a separate Git checkout yourself; these scripts do not fetch or install
anything. `SOURCE_ROOT` is the SceneChangeSharp checkout itself, not its parent.

```sh
git clone --branch v0.1.0 https://github.com/MIRIMIRIM/SceneChangeSharp.git /path/to/SceneChangeSharp
git -C /path/to/SceneChangeSharp checkout --detach 51566a64eb193f130c39731c5eacc92330b463b1
git -C /path/to/SceneChangeSharp submodule update --init --recursive
export SOURCE_ROOT=/path/to/SceneChangeSharp
export BUILD_ROOT=/path/to/build-scenechange-linux
```

`source-revisions.json` records repository URLs and the exact revisions:

- SceneChangeSharp tag `v0.1.0`, annotated tag object
  `7b405c6c6088a1d899809143665a6a4fb56b1d73`, commit
  `51566a64eb193f130c39731c5eacc92330b463b1`
- `vendor/xvidcore`: Xvid 1.3.7,
  `d7eeb1a838fb403123711d63acc0430aa3d9edf9`
- `vendor/vapoursynth-scxvid`:
  `48817c0b5dfa8aec68346439d4db8e1751c99876`
- `vendor/vapoursynth-wwxd`:
  `a5870862fd85138b5e4822a076377b96592c9cd2`

All four HEADs and the annotated tag are checked on every build/verification.
Tracked changes are rejected. Untracked files are ignored by the pin checks.
Archive-only source trees without Git metadata are deliberately unsupported.
The scripts neither patch nor generate files in the external checkout. Xvid
patches are applied to its private tracked-file export. WWXD publishes a private export
of main-repository tracked files, including the referenced C# project, so that
its `obj`/`bin` generation cannot affect the original checkout.

Choose a dedicated `BUILD_ROOT` outside both the external source checkout and
this recipe directory, with neither one an ancestor of the other. The scripts
recreate only their own Xvid variant/private source subdirectories; never point
`BUILD_ROOT` at unrelated files or a symlink-managed build tree.

Expected external layout:

```text
$SOURCE_ROOT/
  .git, .gitmodules
  patches/xvidcore-*.patch
  src/SceneChange/SceneChange.csproj
  src/native/abi/scenechange_provider.h
  src/native/wwxd/scenechange_wwxd.cs
  src/native/xvid/scxvid_bridge.cpp
  tests/fixtures/scxvid-golden-v1.json
  vendor/{xvidcore,vapoursynth-scxvid,vapoursynth-wwxd}/
```

## Build prerequisites and tool selection

Run natively on Linux x86-64 with glibc. Cross-compilation, musl and other
architectures are not supported by this recipe.

- Python 3.9+ (the parity test uses `random.Random.randbytes`), Bash, Git,
  GNU patch, binutils (`ar`, `nm`, `readelf`, `ldd`)
- Clang/Clang++ with C11/C++20 support, a GNU-compatible ELF linker, NASM,
  glibc/pthread development files, zlib development files for NativeAOT,
  and available static `libstdc++`/`libgcc` archives
- Official .NET SDK **10.0.401**, supplying NativeAOT/ILCompiler and ILLink
  **10.0.12**; the Linux build used Clang **19.1.7**, NASM **2.16.03** and
  GCC **14** runtime archives
- Access to official NuGet feeds for restore, or the required official packages
  already copied into `$BUILD_ROOT/wwxd/nuget-packages`

Set executable paths as needed. Each variable is one executable path/name,
not a command plus arguments. No sibling dependency directories are assumed.

```sh
export CC=/usr/bin/clang-19
export CXX=/usr/bin/clang++-19
export NASM=/usr/bin/nasm
export DOTNET=/path/to/dotnet/dotnet
export JOBS=2
./rebuild.sh --dry-run --parity --inprocess-illink
./rebuild.sh --verify
```

`CC`, `CXX`, `NASM`, `DOTNET`, `AR`, and `PATCH` default to `clang-19`, `clang++-19`,
`nasm`, `dotnet`, `ar`, and `patch` on PATH. `JOBS` defaults to 2. Provision any
additional compiler search paths (`LIBRARY_PATH`, for example) in your shell;
they are not guessed. WWXD adds private `clang`/`clang++` symlinks pointing to
the chosen compilers and uses a private `global.json` to select the exact SDK.
Its CLI home, package caches, XDG paths and temporary files remain in BUILD_ROOT.
No machine-level settings are changed.

`--dry-run` validates Git inputs and tool availability, then prints commands
without creating BUILD_ROOT, applying patches, restoring packages or compiling.
It does not establish that packages, system libraries or link dependencies are
present. `--verify` builds both providers and runs their standalone tests.
`--parity` also builds full Xvid and compares it to the optimized provider.
To select a single build, run `python3 build-xvid.py --variant optimized`,
`python3 build-xvid.py --variant full`, or `python3 build-wwxd.py`.

`--inprocess-illink` is optional and deliberately off by default. It imports the
local scoped `msbuild-inprocess-illink.targets` only into this WWXD publish. It
runs the same official ILLink task assemblies in-process for restricted hosts
that cannot create named-pipe task hosts. It does not disable trimming, AOT or
validation. Enable it only when that environment restriction applies.

## Windows option parity and Linux adaptations

Upstream `.github/workflows/native.yml` publishes the unchanged file-based WWXD
source as a shared NativeAOT library; this recipe changes its RID from `win-x64`
to `linux-x64`. PublishAot, NativeLib=Shared, unsafe code and invariant
globalization remain controlled by the upstream source directives.

Upstream `.github/workflows/scxvid-native.yml` packages the optimized Xvid
variant from `scripts/build_scxvid_native.ps1`. The Linux optimized build retains
`DetectorOnly`, `PersistentSmp`, `Avx2SadBatch`, and x86-64 assembly. It applies
exactly this unchanged upstream patch queue to a private Xvid copy:

1. `xvidcore-upstream-9304e6d1-image-setedges.patch`
2. `xvidcore-gmc-negative-shift.patch`
3. `xvidcore-scene-detect-only.patch`
4. `xvidcore-avx2-sad-batch.patch`
5. `xvidcore-persistent-smp.patch`

The full-Xvid comparison build receives only patches 1 and 2. No extra source
patches are applied. Generic C and x86 assembly lists come from pinned
`build/generic/sources.inc`. The optimized AVX2 source alone receives `-mavx2`;
upstream CPUID/OSXSAVE runtime checks remain intact. Linux uses PIC, ELF64 NASM
System V ABI, pthreads, and a hidden private Xvid archive. C++/GCC runtimes are
linked statically; the version script exports only `scenechange_provider_get_api`.

Important: upstream's persistent pool is Windows-only. Its unchanged Linux
stub returns NULL, so existing Xvid `pthread_create`/`pthread_join` workers are
used. Enabling the upstream flag does not provide a Linux persistent pool or
establish a Linux performance advantage.

## Outputs and optional verification

```text
$BUILD_ROOT/output/libscenechange_wwxd.so
$BUILD_ROOT/output/libscenechange_xvid.so
$BUILD_ROOT/wwxd/output/scenechange_wwxd.so     # original NativeAOT name + debug sidecars
$BUILD_ROOT/build/full/libscenechange_xvid.so # only with --parity/full variant
$BUILD_ROOT/{build,wwxd,logs,tests}/           # generated inputs, objects, caches, test binaries
```

`./verify.sh` checks the default output mapping above. To validate existing
libraries without rebuilding them, use explicit paths:

```sh
./verify.sh --wwxd-lib /existing/output/libscenechange_wwxd.so \
  --xvid-lib /existing/output/libscenechange_xvid.so \
  --parity --full-xvid-lib /existing/build/full/libscenechange_xvid.so
```

SOURCE_ROOT is still required for pin checks, the original golden fixture and
the public ABI header. BUILD_ROOT receives only compiled native test consumers
in this reuse mode; library files are read-only. All library paths are resolved
before invoking tests, so relative paths work from the caller's directory.

The preserved smoke/parity tests cover API negotiation and layout, extension
tails, invalid dimensions, frame ownership, hard cuts, progress cancellation,
reset, lifecycle churn, all six upstream golden scenarios across 1/4/8 threads
and slices off/on, motion consistency, and full-versus-optimized randomized YUV
parity. The two C++20 consumers check WWXD's public header ABI and lifecycle.
Runtime checks unset LD_LIBRARY_PATH and point .NET roots at nonexistent paths.
ELF exports, dependencies and symbol versions are printed for inspection.
These tests do not replace Aegisub application integration, the full managed
test suite, other CPU testing or cross-distribution validation.

Refactoring validation actually performed: Python/Bash syntax and MSBuild XML
parse checks; optimized, full and WWXD dry runs with no build directory created;
path-normalized equivalence to the original 80 optimized/78 full compilation
commands and both link commands; and the entire standalone suite above against
the existing verified libraries. Both C++ test sources are byte-identical to
the verified versions. Python test changes only parameterize fixture/library
locations. This validation deliberately did not rebuild the provider libraries.

## Distribution and compatibility limits

The original tested artifacts required at most GLIBC 2.34 (WWXD) and GLIBC 2.38
(Xvid). Those are observations from that host, not fixed requirements of future
builds or a promise of support on older distributions. Inspect your own outputs
with `readelf -V` and build on the oldest intended target sysroot when broader
compatibility is needed. NativeAOT additionally exports its standard
`DotNetRuntimeDebugHeader` symbol and a version node; it runs without an
installed .NET runtime or ICU in the tested invariant-globalization build.

Upstream labels SceneChangeSharp and its provider release assets GPL-2.0;
Xvid and reference sources retain their own copyright and license notices.
NativeAOT output incorporates .NET runtime components, and the Xvid output
incorporates GCC runtime archives. Before distributing built libraries, retain
the exact upstream/submodule corresponding source, patch queue, these build
instructions, applicable GPL license texts, .NET LICENSE/ThirdPartyNotices and
GCC runtime copyright/exception notices. This recipe-only directory is not a
complete corresponding-source or binary redistribution bundle.
