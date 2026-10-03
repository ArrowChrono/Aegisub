# LsmasNative Linux provider recipe

This is build orchestration for the separate upstream provider. Aegisub keeps
its dynamic ABI boundary; none of the provider implementation is vendored or
compiled into the main executable. Output: `$BUILD_ROOT/artifacts/lib/liblsmasnative.so`.

## Locked source inputs

`source-lock.json` records exact Git commits and patch/header SHA-256 hashes.
The Windows asset used to select them was LsmasSharp release `202608`,
`lsmasnative-win64-hoe_202608.zip`. Its native API is 1.1.0.

Prepare this layout outside the Aegisub repository and build tree:

```
$SOURCE_ROOT/
  LsmasSharp/                      # 4c85eb4c68e3a96c18c0936652d663a30724d145
    third_party/l-smash-works/     # 7e65185d3f08ba4ad191e9a5cbba3e2c6fd3bb67
      FFmpeg/                     # 39c24f36247f370864ce86ebdf2aa936151f4bfc
      xxHash/                     # e626a72bc2321cd320e953a0ccf1584cad60f363
  dav1d/                          # 1.5.3, b546257f770768b2c88258c533da38b91a06f737
  zlib/                           # 1.3.1, 51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf
```

Example acquisition commands (the build itself makes no network requests):

```sh
mkdir -p "$SOURCE_ROOT"
git clone https://github.com/MIRIMIRIM/LsmasSharp.git "$SOURCE_ROOT/LsmasSharp"
git -C "$SOURCE_ROOT/LsmasSharp" checkout --detach 4c85eb4c68e3a96c18c0936652d663a30724d145
git -C "$SOURCE_ROOT/LsmasSharp" submodule update --init third_party/l-smash-works
git -C "$SOURCE_ROOT/LsmasSharp/third_party/l-smash-works" submodule update --init FFmpeg xxHash
git clone https://github.com/videolan/dav1d.git "$SOURCE_ROOT/dav1d"
git -C "$SOURCE_ROOT/dav1d" checkout --detach b546257f770768b2c88258c533da38b91a06f737
git clone https://github.com/madler/zlib.git "$SOURCE_ROOT/zlib"
git -C "$SOURCE_ROOT/zlib" checkout --detach 51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf
```

Source checkout HEADs, tracked cleanliness and release input hashes are checked.
The required FFmpeg MOV audio-end and HOE audio-read patches are read from that
pinned checkout, hash-verified and applied only to private build copies. Git
repository discovery and inherited Git state are isolated during patching. All
five patched files are checked against known-good hashes before marking a tree
prepared and again on reuse, so skipped or altered patches fail closed. zlib is
also copied before CMake, whose upstream configuration otherwise renames zconf.h
in the original source tree. Do not substitute stock FFmpeg or the separate
FFMS2/Ragbag dependency prefix; its patch contract and versions differ.

## Build

Prerequisites: Linux x86-64 with normal development headers/CRT, Clang 19,
CMake 3.20+, Ninja, Make, Python 3.9+, Git, pkg-config, Meson and NASM.
No Zig or managed .NET runtime is needed for this C provider. The Windows
manifest's Zig version is provenance, not a Linux build requirement.

```sh
export SOURCE_ROOT=/absolute/path/to/prepared-sources
export BUILD_ROOT=/absolute/path/to/lsmasnative-build
export CC=clang-19 CXX=clang++-19 JOBS=3
./build.sh
AEGISUB_SOURCE=/absolute/path/to/aegisub ./verify.sh
```

Set PATH and any toolchain loader paths yourself when using extracted tools.
SOURCE_ROOT and BUILD_ROOT must be disjoint. Use a fresh BUILD_ROOT after changing
compiler/configuration or the lock. Outputs, prepared sources and caches stay
under BUILD_ROOT; source checkouts are not edited. Verification suppresses Python
bytecode writes to external source trees.

`build-native.sh` is a narrow rebuild entry point. It accepts `FFMPEG_PREFIX`
and `LSW_SOURCE` overrides for an already prepared *matching* static media prefix
and patched HOE source. This advanced reuse path cannot certify how external
archives were built; callers must retain their provenance. The default full
build.sh path prepares the locked inputs itself.

## Preserved options and verification

- PIC static FFmpeg, dav1d, zlib and xxHash inside a replaceable shared provider
- FFmpeg GPL/version3 enabled, encoders/muxers/programs/avfilter/avdevice disabled,
  autodetection disabled, explicit dav1d/zlib; original configuration otherwise
  follows the verified Linux adaptation of the Windows recipe
- FFmpeg networking remains configured on (CONFIG_NETWORK=1), unlike the reduced
  FFMS2/Ragbag configuration. Remote URL playback was not tested
- Provider-local CMake source graph mirrors upstream build.zig; local ELF export
  map exposes only lsmas_* and excludes embedded archive symbols
- verify.sh generates five small fixtures using a separate system FFmpeg, tests
  fresh/cache-reloaded video and audio random seeks, all six video outputs,
  upstream 16 audio regressions and 13 build-identity tests, then checks 39 public
  exports, API version and static media linkage
- AEGISUB_SOURCE enables an additional check against that checkout's required
  and optional API symbols; when omitted it is explicitly reported not-run
- Fixture generation needs system FFmpeg encoders libx264, libx265, libvpx-vp9,
  libaom-av1, AAC, Opus, FLAC, FFV1 and PCM. FFmpeg/FFprobe are only test tools

The original complete dependency recipe and clean-source rebuild passed all
of these tests. The normalized commit recipe was revalidated by rebuilding only
the native provider against the previously verified pinned dependency prefix,
then executing its tests; its entire dependency stack was not rebuilt again.
See the accompanying normalization validation report for exact scope.

## Packaging and limits

Collect `artifacts/lib/liblsmasnative.so`, `artifacts/licenses/`,
`artifacts/build-manifest.json`, `artifacts/source-lock.json` and test reports.
Package the DSO as `bin/runtimes/liblsmasnative.so` or a relative link to the
package library directory. Enable Aegisub WITH_LSMASNATIVE and WITH_SCENECHANGE
when collecting the corresponding SceneChange providers.

This is a GPL-3.0-or-later configuration, unlike the LGPL-only FFmpeg used by
FFMS2/Ragbag. Retain the upstream notices and provide exact corresponding
sources/build instructions when distributing covered binaries. A source URL
alone is not a source offer. collect-metadata.py gathers notices from the pinned
sources; it does not provide a blanket redistribution certification.

The tested original artifact dynamically required libc, libm, libatomic and the
ELF loader, with observed maximum GLIBC 2.38. It was tested on Debian 13 x86-64;
no universal older-distro or musl compatibility is promised. Package libatomic
through the normal dependency closure while keeping glibc/loader host-provided.
