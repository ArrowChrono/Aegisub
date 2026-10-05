# Aegisub

For binaries and general information see [the homepage](https://arrowchrono.github.io/aegisite/).

Changelogs: [Stable](https://arrowchrono.github.io/aegisite/changelog/), [Beta](https://arrowchrono.github.io/aegisite/changelog/beta/)

## Development and building

Aegisub uses C++20 and CMake, with bundled LuaJIT. Run commands from the
repository root and initialize submodules with `git submodule update --init --recursive`.
Use separate build directories for different compilers/generators; keep an
existing directory's configuration when rebuilding.

### Dependencies and configuration

- [vcpkg.json](vcpkg.json): vcpkg dependencies, optional features and pinned baseline.
- [CMakeLists.txt](CMakeLists.txt): dependency discovery and `WITH_*` / `AEGISUB_*` options.
- [CMakePresets.json](CMakePresets.json) and [cmake](cmake): Windows presets and vcpkg triplets.
- [build/package-win-portable.cs](build/package-win-portable.cs) and
  [its dependency manifest](build/package-win-portable.thirdparty.json): preparation
  of external headers and runtimes not supplied by vcpkg.
- [CI workflow](.github/workflows/gha-ci.yml): toolchain setup, build, test and packaging commands.

### Windows (MSVC)

The checked-in presets use Visual Studio 2026, CMake 3.25+, PowerShell 7 and
vcpkg. Install the C++ workload and Windows SDK, bootstrap vcpkg at the manifest
baseline, and set `VCPKG_ROOT`. Use a developer shell with CMake available
(Visual Studio includes it). Dependency preparation requires .NET SDK 10.

```powershell
dotnet build/package-win-portable.cs -- --mode prepare-dependency --cache-dir .cache/package-win-portable
pwsh -File build/version.ps1 .
cmake --preset release-x64
cmake --build --preset relwithdebinfo-x64
```

The configure preset `release-x64` uses `build-dir`; the build preset
`relwithdebinfo-x64` selects `RelWithDebInfo`.
For a scripted MSVC build, see [build/build-win-vcpkg.ps1](build/build-win-vcpkg.ps1)
(`-Configuration RelWithDebInfo` selects the same build configuration).

### Clang, Linux

[Building with Clang](docs/build-clang.md) covers prerequisites, dependency setup,
Windows clang-cl and Linux clang scripts, tests, and WSLC containers. Start there
rather than reusing the Windows MSVC presets on Linux.
For macOS, see the [platform notes](https://github.com/wangqr/Aegisub/wiki/Special-notice-for-macOS).

### macOS

TODO. Need tests.

### Tests

Test targets and CTest registration are defined in
[CMakeLists.test.txt](CMakeLists.test.txt) and
[CMakeLists.smoke.txt](CMakeLists.smoke.txt). The Clang scripts' `test` mode builds
and runs C++ unit tests with bounded timeouts and JUnit output; see
[the build guide](docs/build-clang.md) for commands and artifacts.
GUI/provider E2E automation is documented in
[GUI automation protocol](docs/gui-automation-protocol.md).

## License

All files in this repository are licensed under various GPL-compatible BSD-style licenses; see LICENCE and the individual source files for more information.
~~The official Windows build is GPLv2 due to including fftw3.~~
