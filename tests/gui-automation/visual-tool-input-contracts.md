# Visual-tool native input contracts

Run from the repository root with a Windows .NET 10 SDK and a built Aegisub GUI-test host. These drivers are standalone executable E2E programs, not `dotnet test` projects. They use an isolated profile and generated ASS fixture; pass a repository-relative video path. Output must be a new artifact directory. Do not run multiple native-input drivers concurrently.

```powershell
dotnet build tests/gui-automation/visual-tool-drag-uia.cs -o build-dir/artifacts/input-contract-driver --nologo
& build-dir/artifacts/input-contract-driver/visual-tool-drag-uia.exe `
  --exe build-dir/RelWithDebInfo/Aegisub.exe `
  --video build-dir/artifacts/visual-tool-drag/baseline-640x480-square.mp4 `
  --artifacts build-dir/artifacts/input-contract-drag-pos-libass `
  --input-contract drag-pos --subtitle-provider libass --timeout-seconds 20
```

The video in this example is an existing generated fixture, not a checked-in asset. Supply another repository-relative 4:3 video if absent. The contract requires a 4:3 canvas of at least 320 by 240 pixels. The chosen executable must have its runtime dependencies available; CSRI additionally requires the user's existing compatible renderer, which the test does not install, rebuild, or replace.

## Cases and independent oracles

| Cases | Behavior checked |
|---|---|
| `drag-pos`, `drag-move-start`, `drag-move-end`, `drag-org`, `rotate-z-origin`, `rotate-xy-origin`, `rect-corner`, `vector-node` | Release with a unique final coordinate equals a reference containing that terminal motion; return to start; Shift constraint; no-motion feature click; exact one-Undo restoration |
| `hold-rotate-z`, `hold-rotate-xy`, `hold-scale`, `hold-rect` | Existing hold release semantics equal explicit terminal motion and restore with one Undo |
| `freehand`, `freehand-smooth` | A short release tail below the normal sampling distance still ends at the release coordinate, within one script unit of native pixel quantization; exact Undo |
| `rapid-` plus a paired feature case (all feature cases above except `vector-node`) | A displayed old handle selects the live semantic feature; a new relative delta accumulates on the latest model; two independent Undo boundaries |

Each ordinary case generates 48 ASS events, preserves unrelated tags, styles and dialogue bytes, and compares the saved style/event document. The edited tag may move within the override block, but it may not be removed or duplicated; reference-versus-gap and Undo comparisons remain exact. The fixture contains one active edited line, not a multi-selection stress test. It uses explicit tags; it does not directly establish implicit-position materialization behavior.

Rapid cases require an exact trace witness: startup and restored baseline displayed, the sequential reference Final displayed before its next press, and no packet from the first rapid gesture displayed before the next press. If the renderer finishes before that press, the result is `prerequisite-inconclusive`, **not pass and not a proven product failure**. Do not repeat until a lucky run passes. The initial top-level window PNG is setup diagnostics only, not a pixel-coherence oracle. Trace presents are application-side successful Swap markers, not physical screen latency.

Run both `libass` and `CSRI/pf-xy-vsfilter_textsub` explicitly. Saved provider selection must equal the requested provider. The shared driver has a total watchdog of `max(120, 6 * timeout-seconds)` seconds. A supervising process should add a small bounded cleanup allowance. A failed capture may indicate an unsolicited native motion; preserve and inspect trace rather than assume the intended feature was never captured. Raw window-message input does not suppress real system mouse messages.

## MotionTrack ROI

```powershell
dotnet run --file tests/gui-automation/motion-track-roi-input.cs -- `
  build-dir/RelWithDebInfo/Aegisub.exe `
  build-dir/artifacts/visual-tool-drag/baseline-640x480-square.mp4 `
  build-dir/artifacts/input-contract-roi-libass libass
```

The ROI driver uses actual native input and the MotionTrack dialog's UIA `RangeValuePattern` values. It checks Band, Move, crossing Resize, no-motion resize near a grip, return to start, release-capture and capture cancellation against independent exact rectangles. It verifies that the ASS document is unchanged; ROI edits are not subtitle Undo operations. It writes `result.json`, per-case dialog/window PNGs and UIA evidence, and has a 180-second total watchdog (30 seconds in `--inspect` mode).

## Scope

These tests establish model and input semantics, not smoothness. They do not replace paired performance measurement, real pointer-to-screen measurement, or all-tools manual acceptance. Custom Cross/Measure/Perspective handlers and cross-active-line multi-selection regrab are not directly exercised by these new cases. Existing C++ lifecycle tests and WGL smoke remain necessary for queued Final, invalidation and overlay pixel behavior.