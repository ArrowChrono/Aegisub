# Tests

- `tests/` contains the GoogleTest suite built as `gtest-run` and executed by
  `test-aegisub`.
- `smoke/` contains native process and integration checks. The available
  targets are conditional on platform and optional backends; graphical checks
  require a usable Windows desktop and are intentionally separate from the
  fast unit-test gate.
- `plugin-bridge-smoke/` contains versioned automation scenarios and their
  native process runners.
- `gui-automation/` contains the Windows UIA correctness and specialized Skia
  drivers, plus their shared black-box driver support.
- `fixtures/` contains source fixtures. `setup.ps1`, `setup.bat`, and
  `setup.sh` copy them into the build-tree test runtime before unit tests run.
- `support/` contains shared GoogleTest fixtures and test-only stubs.

Generated runtime data belongs under `build-dir/tests-runtime`; the source
tree should contain only test sources and reusable fixtures.

## Font usage inventory

After building Aegisub, run the real-process inventory contract suite from the
repository root, using a fresh artifact directory:

```powershell
python tests/smoke/font_usage_inventory_e2e.py `
  build-dir/RelWithDebInfo/Aegisub.exe `
  build-dir/artifacts/font-usage-inventory-cli
```

The suite uses distributable fixture fonts without installing them, an
independent three-file interval-count oracle, real Windows exclusive read locks,
and bounded subprocesses. It requires Windows; source fixtures remain read-only.
See the [native inventory E2E guide](gui-automation/font-usage-inventory-native.md)
for GUI capabilities, repeatable commands, session-font authorization and known
unproved acceptance partitions. wxStyledTextCtrl is not a native Scintilla HWND;
the unsupported SCI-message proposal is not a passing GUI test.

## Perspective renderers

Perspective's Windows renderer checks use xy-VSFilter through its CSRI DLL.
Place the matching architecture's DLL at `artifacts/xy-vsfilter/VSFilter.dll`
and run the following from the repository root after configuring `build-dir`:

```powershell
$env:AEGISUB_XY_VSF_DLL = (Resolve-Path artifacts/xy-vsfilter/VSFilter.dll).Path
$env:AEGISUB_XY_VSF_REQUIRED = '1'
$env:AEGISUB_XY_VSF_VERSION = '3.2.0.810'
cmake --build build-dir --config RelWithDebInfo --target test-aegisub --parallel
```

The version pin identifies the DLL used to establish the current renderer
regressions; set it to the exact file version under evaluation. The checks
also verify the CSRI renderer identity and report its version, without falling
back to another renderer. An absent DLL skips these checks in ordinary unit
runs; `AEGISUB_XY_VSF_REQUIRED=1` makes absence a failure. A configured but
unloadable DLL, wrong renderer identity, or version mismatch always fails.
Without an explicit DLL, discovery checks the executable's `csri` directory.
Each renderer test runs in its own child process with a 30-second deadline;
a timeout fails the test and terminates the child with a bounded cleanup wait.
The drawing matrix covers `\p2`, positive and negative `\pbo`, alignment,
and unequal script/output resolutions. The text matrix covers Latin, CJK,
vertical CJK, explicit line breaks, and spacing; each case verifies its requested
GDI face and reports an explicit font skip if that face is unavailable.
The text cases also cover unequal X/Y output scales with unrotated and rotated
glyphs, keeping glyph size and character spacing under their distinct renderer
scaling rules. Use
`--gtest_filter=*perspective_vsfilter*` when running only these checks, since
the parameterized suites include an `xy/` prefix.
The existing `perspective_libass_render` cases provide secondary compatibility
coverage. Neither renderer's pixel checks claim subpixel text layout accuracy.

Run the Windows UIA correctness driver directly after building Aegisub:

```powershell
dotnet run --file tests/gui-automation/gui-automation-uia-correctness.cs `
  --artifacts-path build-dir/dotnet-artifacts/gui-automation-uia/RelWithDebInfo `
  -- `
  --exe build-dir/RelWithDebInfo/Aegisub.exe `
  --open tests/plugin-bridge-smoke/trim-selected-input.ass `
  --artifacts build-dir/gui-automation-uia-correctness/RelWithDebInfo
```

The entry point runs UIA in a supervised worker process. `--timeout-seconds`
also bounds worker stalls (clamped to 10-30 seconds); a stalled worker and the
Aegisub process it started are terminated together.
