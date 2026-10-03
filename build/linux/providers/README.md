# Additional Linux runtime provider recipes

These recipes build the runtime components that the Windows packaging manifest
obtains from LsmasSharp 202608 and SceneChangeSharp v0.1.0. They do not embed those
implementations into Aegisub or commit upstream source/binary bundles.

- [LsmasNative](lsmasnative/README.md): pinned HOE L-SMASH-Works, custom patched
  FFmpeg, dav1d and zlib; C provider API 1.1.0
- [SceneChange](scenechange/README.md): independent Xvid native and WWXD NativeAOT
  providers, public provider ABI 1.0.0

Use the individual prepared-source instructions, exact lock files and an
external writable build directory. Toolchain locations are parameters, not
cloud-workspace paths. Source acquisition and optional NuGet restore need their
normal network access; native build steps consume the pinned prepared sources.

## Aegisub integration

Configure the main application with:

```
-DWITH_LSMASNATIVE=ON -DWITH_SCENECHANGE=ON
```

Collect the following runtime outputs using the Linux package manifest:

```
bin/runtimes/liblsmasnative.so
bin/runtimes/libscenechange_xvid.so
bin/runtimes/libscenechange_wwxd.so
```

They may be relative links into the package's common lib directory. Preserve
provider ABI/export isolation and let the packager audit their complete ELF
dependency closure. In the tested build, LsmasNative adds dynamic libatomic;
SceneChange Xvid/WWXD need only host glibc/math/loader. Do not statically link glibc
or copy graphics drivers into the application package.

Build and run Aegisub's lsmas-provider-smoke and existing
scenechange-provider-smoke against actual fixtures. The latter supports expected
keyframe lists, backend names and cancellation. The provider recipes' tests
complement these application-boundary tests; they do not replace them.

## Provenance and differences

Keep the per-provider source lock, build/test results, notices and corresponding
sources with any packaged runtime. The source- and binary-containing release
archives from the local validation are deliverables, not files for this commit.

LsmasNative's FFmpeg is the custom patched GPL/version3 configuration, with
networking configured on. It differs deliberately from the reduced LGPL-only,
network-disabled FFMS2/Ragbag FFmpeg build. Local-media behavior was tested;
remote URL playback was not. Xvid carries GPL terms as well. Do not label the
combined set of providers LGPL-only.

Xvid preserves the upstream optimized patch queue and runtime AVX2 dispatch.
The Windows persistent worker pool is inactive on Linux; upstream's pthread
fallback is used. WWXD uses .NET 10 NativeAOT and does not need an installed
.NET runtime at execution time. An optional build-environment ILLink workaround
is documented by the SceneChange recipe without disabling trimming/AOT.

The original complete package was tested on Debian 13 x86-64. Observed provider
GLIBC symbol requirements were 2.38 (LsmasNative/Xvid) and 2.34 (WWXD); the complete
application package has its own stricter host baseline. These observations do
not certify all older distributions or musl.

Validation scope for normalized, commit-facing recipes is recorded beside each
recipe. Full dependency builds are not claimed when only a safe reuse/native
rebuild was performed. Original package QA included 2675 passing unit tests,
8 platform/environment skips, both SceneChange backends, real GUI audio/video,
keyframe generation and relocated provider loading. A separate optional headless
playback probe stalled in both the preserved FFMS2 baseline and new package;
that diagnostic was not counted as passing.
