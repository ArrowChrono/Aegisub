# Shared visual-transform port and near-camera clipping

## Current integration status, 2026-11-08

The uncommitted convergence candidate is not accepted under the original
strict wx performance contract. The latest valid wx pair fails all four
Draw and Combined gates and passes all four Frame gates. Gac passes all
twelve Draw and full-render gates across the three tested renderers.
The detailed current pairs and bounded optimization decision are recorded
near the end of this document.

Both sessions' correctness evidence has now been reconciled against the
current source. The final wx ten-case boundary suite includes capture loss,
active-without-capture, the positive assertion-channel probe and exact Undo.
Gac's actual OpenGL workspace suite covers docked Z/XY and detached Scale/XY;
its dependent code is unchanged. Current three-renderer runs additionally
cover XY, origins/modifiers/folded sources, near clipping and following GL
state. No new original-scope GUI run is required solely by documentation
updates. This does not claim physical input or every workspace/renderer
combination.

No further small optimization is supported by the retained measurements.
Work proceeds to atomic integration preparation while preserving the wx
failure as an explicit merge decision. No branch has been rewritten or
merged. The historical acceptance below describes its recorded revision
and older gate, not acceptance of the current convergence candidate.

## Historical delivered baseline

Accepted on Windows x64 RelWithDebInfo on 2026-10-29 (Asia/Shanghai). This
follow-up selectively shares neutral code with exp and repairs the visible
near-camera fragment. It does not renew all prior Gac renderer certifications.

## Scope and ownership

Comparison parents are exp `301bc1619` and Gac `760ac1103`. The selected maximal
neutral closure is **39 common C++ files plus the identical common
`tests/gui-automation/driver/PrivateDesktop.cs` helper**. The external delivery
receipt records their final committed Git blob identities and branch revisions.
The accepted exp C++ 48-path patch is
`08EBA340562D89DAAC1454A05A048F94E18464E97DA8B996324D14F893E4BDBC`;
the Gac nine-path source patch is
`5DACDAE6E892993815C6E934C3E6868CA345A2A604F9A647EF972C1FD04B55B6`.
Documentation and the later wx observer-transport repair are bound separately.

Shared code covers neutral ASS reading/edit policy, geometry, immutable overlay
snapshots, drawing-context contracts and their related consumers/tests. wx keeps
native capture, live ASS edits, coalesced history, gesture retirement,
Drag/Clip/Linking ownership and renderer/fallback selection. Gac keeps its existing
actual-drawn snapshot, committed-baseline and Final-presentation owners. No Gac
UI/runtime/workspace/history architecture is imported into wx, and no second
clipping algorithm is maintained.

The wx observer is test-only and read-only: actual `AssDialogue.Id`, source
selection/fold projection and explicit viewport capture. It adds no normal-path
CPU readback or mutation/input-injection API. IDs are compared within the relevant
process/gesture, not numerically across fresh processes or document reloads.

## Root causes and corrected contract

- `21f1e6db3` introduced RGB-only shared Z state; `760ac1103` introduced analogous
  XY state. They discarded configured colour transparency before the direct
  overlay renderer boundary. Those states now retain `agi::Color`, keeping
  transparency and explicit stroke/fill opacity independent. Existing packed-RGB
  wx snapshot producers and legacy GL's explicit-alpha behavior are deliberately
  unchanged. The tests prove shared paint arguments, not GL pixel transparency.
- `760ac1103` rejected a complete XY segment if either projected endpoint was
  behind the camera, omitting its visible front fragment. One shared segment path
  now uses the same camera transform as point projection and clips crossing
  segments against the **inclusive denominator plane `1`**. Endpoint order and
  both-front float operation order are preserved; hidden/invalid segments are
  rejected. Four separately widened exact float-product terms and a bounded
  four-double grow-expansion handle the finite cancellation witness. There is no
  heap allocation or general numerical framework. Point/hit admission is unchanged.

The regression pose is `frx75`, `fry80`, `frz15`, `fax0.2`, `fay-0.1`,
`org(220,180)`, PlayRes `640x480`, LayoutResY `38400`, perspective scale `640`.
The missing stroke is the **negative-X secondary grid ray**, not a primary axis:
first-segment fade `.9375`, width `2`, with independently expected visible direction
`(220,180) -> (640,400.9604)`.

This is not exact raster equivalence. CPU segment clipping and eight-step fade
can differ from former raw-GL rasterization. All four fresh wx original image
pairs were inspected, including this ray; source geometry and positive pixels
support the image result, rather than a line count or timing pass alone.

## Evidence roots and requirement map

`$PortEvidence` and `$GacEvidence` below denote caller-selected external artifact
roots, not repository paths. Raw results, failures and runnable bundles remain
outside version control. Source/target/configuration/binary binding is retained
in their receipts; compiled-only targets and old artifacts are not new GUI passes.

| Requirement | Exact test or GUI case | Actual evidence |
|---|---|---|
| Visible front ray | `visual_transform_overlay.rotate_xy_near_plane_clipping_retains_visible_front_grid_ray` | Actual old-policy RED: one executed/one assertion failure; final focused passes on both consumers |
| Ordered clipping, inclusive boundary and cancellation | `visual_transform_overlay.rotate_xy_segment_preserves_front_order_and_clips_at_inclusive_near_plane` | Nine independent rows in both endpoint orders; final focused passes |
| Nonfinite input/state and representability | `visual_transform_overlay.rotate_xy_segment_rejects_nonfinite_input` | Both final focused XMLs pass |
| XY transparency separate from fade | `visual_transform_overlay.rotate_xy_preserves_transparency_and_independent_grid_fade` | Direct shared paint-argument checks pass |
| Z line/highlight transparency | `visual_transform_overlay.rotate_z_preserves_transparency_for_lines_and_highlight_fill` | Direct shared paint-argument checks pass |
| Per-source origins and display constraints | `visual_edit_controller.rotate_origin_uses_each_lines_fallback_and_anisotropic_viewport`; `visual_edit_controller.rotate_xy_origin_uses_per_source_fallback_and_display_axis_constraint` | Both final focused XMLs pass |
| XY wrapping/modifiers, Scale ratios/snap and exact returned source | `visual_transform_policy.rotate_xy_matches_wx_axis_snap_and_signed_wrap`; `visual_edit_controller.scale_preserves_selection_and_uses_display_delta_with_snap_and_ratio`; `visual_edit_controller.rotate_xy_modifiers_return_to_baseline_restores_exact_source` | Both final focused XMLs pass |
| Selected hidden sources | `visual_edit_controller.rotate_xy_uses_active_display_delta_for_selected_invisible_sources`; native `HostAndFoldSetup` | CPU passes; both fresh wx runs observe actual collapsed selected-source projection |
| Native wx hold/origin/modifiers/normalization | `RotateZHold`, `RotateXYHold`, `ScaleHold`, `RotateZOrigin`, `RotateXYOriginShift`, `RotateXYShiftControl`, `RotateZControl`, `ScaleShift`, `ScaleControl`, `RotateXYReturnNormalization` | Fresh baseline and corrected run: exact16/16 each |
| wx representative drawing/runtime | `XYDrawNeutral`, `XYDrawNormalXYZShear`, `XYDrawEdgeOn`, `XYDrawNearCamera`, `RuntimeAndTraceEvidence` | Four original after-PNG pairs inspected; exact controlled comparison and wall-time gates pass |
| Actual Gac near-camera raster | `VisualToolsNativeRotateXYNearCameraPreservesVisibleFrontRay` | Exact 1/1, actual OpenGL/libass; originalPNG inspected; independent RGBA recount matches all four bands |

wx runs are `$PortEvidence/runs/nearclip-wx-baseline-repaired` and
`runs/nearclip-wx-corrected`. They retain `result.json`, `junit.xml`, input/DPI/tool
journals, actual source IDs, saved ASS, captures and the trace. The comparison in
`evidence/nearclip-wx-comparison.json` checks **56 controlled Info/Styles/Events
checkpoints**, **53 identical directed mouse inputs** and **112 generated-version
comments** individually bound to their own ready-title version. Only the generated
comment is excluded from cross-equality. This is not arbitrary whole-ASS byte
identity or cross-process extradata-ID equality.

The Gac run is
`runs/nearclip-gac-directed/gacui-visual-tools-f05e8a5049ca4c8bbb77354ff632bc12`.
It retains ASS/Y4M, original PNG/BMP, plain/overlay RGBA, line/pointer/state TSV,
state JSON, report/JUnit and owned-process/supervisor receipts. The actual drawn
snapshot/context are nonnull and remain exact before/after readback and final
capture: revision6, frame0, version `(1,4,2)`. Four isolated, unclipped logical
bands at X offsets `80,160,240,320`, half extent `2`, require at least four new
green pixels (`G>=200`, channel advantages >=100), with zero plain green pixels.
Actual new-green counts are **11/10/9/9**. Replay is only a geometry witness;
independent framebuffer pixels establish raster visibility. The consumer uses
its observed allocation rather than a guessed forced host size.

The default Gac 16-case manifest is unchanged; `-NearCamera` selects only this one
scenario. The historical 760ac1103 three-renderer/native and workspace ownership
results remain in the Gac branch's `docs/gacui-rotatexy-batch.md`, including
`VisualToolsNativeRotateXYUsesProjectedPreviewAndExactHistory`,
`VisualToolsNativeTransformOriginsModifiersAndFoldedSources`,
`VisualToolsNativeRotateZPreservesPreviewIdentityAndExactHistory` and
`VisualToolsNativeScaleUsesInheritedPercentagesAndExactHistory`.
Those cases were not rerun by the new single directed scenario.

## CPU closure and retained failures

| Consumer | Focused | Full compiled discovery and XML | Static scope |
|---|---|---|---|
| exp/wx |78/78|2907 =2885 passed+22 skipped; no failures/errors/disabled/incomplete/timeouts|32TUs:31 valid unchanged analyses reused+1 new;27 production Skia-visual-OFF and5 static-only ON|
| Gac |78/78|2905 =2873 passed+32 skipped; no failures/errors/disabled/incomplete/timeouts|5TUs:4 valid unchanged analyses reused+1 new; all exit0|

Both focused selections contain38 shared cases,10 Scale cases,29 affected
existing cases and one boundary inventory. Final compiled registration and
unique XML name sets match exactly. The 22 exp skips are 17 unavailable xy-VSFilter,
3 unavailable AviSynth,1 Unix-only case and1 inverse unavailable-Skia-drawing
case. The 32 Gac skips are the same 17+3+1 plus 11 drawing-Skia-OFF cases. Skips are
not passes. Gac's six actual staged runtimes include both SceneChange DLLs;
exp has four and no SceneChange runtime. Staging is not new SceneChange GUI
operation acceptance.

Static analysis is **not warning-free**. The recorded changed-line gate has no new modernize/performance warnings
or analysis errors. Exp retains the inherited
parent301 `json::Object` finding. Gac retains 723 diagnostics, including 107
added-line readability/deprecation entries and one non-added-line `vector2d`
analyzer finding. No warnings were disabled to obtain the gate result.

CPU receipts: `$PortEvidence/archive/nearclip-accepted-candidate/receipt.json`,
`$GacEvidence/evidence/gac-cpu-closeout.json`, both final focused/full XMLs and
static summaries. The helper's actual legacy caller compile-only retry exits 0;
compile006 also exits 0. Matching key EXE/PDB and dependency bundles are archived.
The final test-only correction leaves the wx production executable unchanged;
its explicit reuse is source-bound, not represented as a fresh application build.

Failures are retained, not filtered away:

- The existing-API ray test was written first and actually failed against the
  old policy (`archive/nearclip-red`). A grouped numeric candidate was rejected
  analytically before compilation, not called a failed executable or green test.
- The first v2 focused run was 77/78. Its old `<1010` assertion encoded segment
  omission; it was replaced by independently expected ordered clipped-tail
  coordinates, not a larger count cap. The three new tests and nine boundary
  rows are unchanged. The complete failed source/XML/EXE/PDB remain archived.
- The original Gac multi-target invocation exited 1 at ambiguous int/float
  `Vector2D` smoke construction. The narrow `420.f` repair and byte-equivalent
  JSON literal changes leave predicates/geometry unchanged. Successful unchanged
  `Aegisub`/`AegisubGac` links are reused with proof; final smoke/gtest invocations
  pass. The failed invocation is not relabeled as overall success.
- Preparation-only cache-validator and file-based-MSBuild routing errors are
  preserved with their actual outcomes; they are not product test failures.

Static test review identified an unexecuted candidate residual-zeroing mutation
not directly protected by current rows and a finite `0<d<1` point-boundary gap.
Neither is an observed implementation defect. No exhaustive mutation or
finite-domain proof is claimed, and no post-implementation new unit test was added.

## GUI accounting and observer protocol repair

The earlier wx window remains exhausted 4/4. It retained the profile radio-group
selection failure, then driver-thread DPI virtualization failure; the profile-only
Cross radio and scoped/restored PMv2 context repaired those causes before the old
paired passes. Those passes did not settle the subsequently found missing ray.
The original Gac window remains 4/6 and expired. Neither record is reset or borrowed.

The independently approved clipping window is closed **4/4**:

1. Fresh wx baseline: setup failure, one failed case and 15 not-run cases.
2. Evidence-supported observer-protocol repair validation: fresh wx baseline16/16.
3. Corrected wx candidate with the same exact compile006 bundle:16/16.
4. Directed Gac near-camera consumer:1/1.

Start1 recorded Win32 1460 at initial capture. A 1071.63ms cold STC paint spanned
canvas selection while the readonly request used synchronous 1000ms transport.
Only state/capture observer requests now use checked owned-HWND `PostMessageW`,
followed by the original fresh-sequence receipt Wait (10s under the same150s
business deadline). Exact normalized reversal proves all other harness characters
unchanged: mutation/tool/mouse/paint1s transport, assertions, poses and timing gates.
The internal reason for the cold STC paint cost remains unknown; no stack was
captured before owned cleanup. This is not a product stall fix. No configured deadline or wait was extended; only observer request transport changed.

`evidence/nearclip-gui-budget.json` records every actual owned creation. The clock
is anchored **at the conservative pre-driver reservation, before actual driver/host creation**, with actual
host creation recorded separately and never moving it later. The window ran from
00:45:51.636 to01:10:32.174 on2026-10-29 local time: **24m40.538s**, before the
unchanged02:15:51.636 deadline. One prelaunch compiler-name guard refusal started
noGUI and consumed no start; its unowned idle MSBuild node was not terminated.
There are no remaining GUI starts in this authorization.

The compile006 repair source/main assembly and helper assembly identities are in
`internal/wx-harness/compile006-async-observer/compile006.final.json`. Its apphost
stub equals compile005's, so apphost hash alone does not identify the repaired
harness. All three binaries and both process/driver identities were checked.
No hidden trials, name-based kills, global input, clipboard use or current-desktop
switching are used. Owned kernel image-path/PID/start identities and kill-on-owner-
exit jobs bound cleanup; a failed observation aborts the case rather than proceeding
with a stale queued response.

## Timing: accepted gate, explicit cost

The four cohorts use16 warmup paints and96 requested measured paints. Actual
positive draw counts may be higher because incidental paints are retained. All
outliers are retained; captures/readback are outside the cohort. Quantiles are
nearest rank `sorted[ceil(N*p)-1]`. The unchanged per-pose P95 gate is
`candidate <= max(baseline*1.25, baseline+0.50ms)`.

Values below are milliseconds. Total is the sum of actual draw samples.

| Pose | Actual draws B/C | P50 B/C | P95 B/C | Max B/C | Total B/C | P95 limit |
|---|---:|---:|---:|---:|---:|---:|
| Neutral |191/192|0.0401/0.2841|0.0570/0.4094|0.0690/0.5624|7.8252/55.0483|0.5570|
| NormalXYZShear |191/191|0.0416/0.2941|0.0567/0.4307|0.0642/0.4991|8.1135/57.6358|0.5567|
| EdgeOn |191/192|0.0412/0.2948|0.0574/0.4175|0.0908/0.6460|8.1334/56.9934|0.5574|
| NearCamera |191/192|0.0456/0.1899|0.0633/0.2899|0.1155/0.4078|9.0659/37.8090|0.5633|

Candidate P95 increased **0.2266-0.3740ms (4.58-7.60x)**. Passing this gate does
not mean performance neutrality. Trace phase `video_display.tool_snapshot.draw`
is application wall time, including synchronous GL-driver blocking, not pure CPU,
GPU elapsed or scan-out. `video_display.render` may include swap; swap wall is
reported separately. Baseline Scale can legitimately use legacy drawing without
snapshot draw events; that absence is not a performance pass. The positive-sample
gate applies specifically to XY.

## Reproduction and limits

Commands run from the relevant repository root using caller-supplied paths. GUI
recipes do not authorize another launch; the budget owner alone uses the existing
root supervisor. No generated project, second input framework or package is needed.

```powershell
# SDK10 file-based wx driver; keep all output under the selected build root.
dotnet build tests/gui-automation/wx-transform-port-native.cs `
  --artifacts-path (Join-Path $BuildRoot 'driver-artifacts') `
  -o (Join-Path $BuildRoot 'driver/wx-transform-port') `
  --configuration Release -p:UseSharedCompilation=false `
  --nologo --disable-build-servers `
  "-bl:$(Join-Path $BuildRoot 'driver-{}.binlog')"

# Invoke the apphost, not dotnet DLL, so private-desktop self-relaunch is exact.
& (Join-Path $BuildRoot 'driver/wx-transform-port/wx-transform-port-native.exe') `
  $WxExecutable $FreshRunArtifacts $ArtifactsRoot $Label

# Gac branch, existing configured RelWithDebInfo build and isolated runner.
& $CMake --build $BuildDirectory --config RelWithDebInfo `
  --target gacui-visual-tools-smoke --parallel $Parallel

tests/smoke/run-gacui-runtime-smoke.ps1 `
  -Executable $GacSmokeExecutable -ArtifactsRoot $FreshArtifactsRoot `
  -Suite visual-tools -NearCamera -VideoRenderer opengl -TimeoutSeconds 180
```

The actual SDK was 10.0.403. Preserve the source directives (`net10.0-windows`,
WPF, nullable, implicit usings, `PublishAot=false`, existing driver project),
coordinate `$env:TEMP`/`TMP` under `$BuildRoot`, and use unique binlogs. File-based
build routing does not accept the attempted project-only `/m:2` and `/nr:false`
combination. Each wx business/native/supervisor/outer bound remains 150/170/180/240s;
Gac's corresponding bounds remain150/170/180/240s. The full outer window must fit
the unchanged global deadline; do not reset a per-case or total clock.

Only Windows x64 VS18 RelWithDebInfo and actual OpenGL/libass are newly exercised.
Native wx Alt, CSRI/xy-VSFilter, physical input/scan-out, other GPUs/platforms and
other Gac backends are not newly accepted. Production Skia visual tools are OFF;
five ON TUs received static analysis only, not ON-configuration build/runtime
certification. Exp drawing-Skia is ON and Gac drawing-Skia is OFF.

The external delivery/cleanup receipts bind committed source blobs to these
accepted binaries, preserve all named failures/runnable bundles, and record
managed archive outcomes. Committing changes generated version metadata, not the
accepted production source behavior; no metadata-only rebuild is claimed as GUI-
tested. Intermediate outputs are removed only after preserving needed EXE/PDB/DLL
and ignored evidence. Protected/active checkouts are never force-deleted; any
refusal is named in the final external report.

## Owned-pass boundary experiment, November 5, 2026

This experiment is uncommitted work above `322c35b59831b5ad2a7674f8cd9633a1d0a6ba1d`.
It is **not accepted for integration**. The historical acceptance described above
does not certify this candidate or satisfy its stricter performance limits.

This preceding candidate established a flat-shaded owned pass, captured the
entry clip equation, shade model and colour mask once, and restores them at pass
completion. Perspective batches retain the shared near-plane clipping and
three submissions, restore the known 2D baseline, and perform no GL state queries,
attribute-stack operations or error polling on the owned path. `Finish()` drains
submission errors before restoring state and checks restoration separately.
Exception unwinding also restores state; outstanding GL errors remain fatal on
that non-throwing cleanup path. The unowned context retains its original checks
and stack protection. The usual 248 grid vertices use stack colour storage;
larger batches retain a heap fallback. Clip-plane capability caching was not
introduced.

The focused WGL tests were changed before implementation. Their owned contract
now reports pending drawing errors at `Finish()` rather than rejecting the next
batch. External code must not change the pass-owned state between batches.
Arbitrary entry shade and clip-equation restoration remained required at pass
completion. Preserved coverage includes independent near-plane/gradient pixels,
full stacks, multiple batches, subsequent 2D drawing, invalid input rejection,
and unowned state preservation. `ValidateXYOwnedPassBoundaries` adds direct and
split-canvas entry-error rejection and C++ exception-unwind restoration.

Windows x64 RelWithDebInfo validation:

- `detached-visual-tools-smoke` passed, including the owned finish/deferred-error
  and unwind cases and existing renderer pixel checks. All 148 corresponding
  RGBA artifacts matched the retained preceding experiment byte for byte.
- `test-aegisub` selected 2,997 tests: 2,978 passed, 19 skipped, zero failures or
  errors. Skips are not runtime coverage.
- An independent Ninja compilation database was generated for clang-tidy.
  Both changed C++ translation units completed analysis without compiler errors.
  Existing warnings remain; the new boundary test's designated-initializer
  warnings were corrected without changing its assertions.
- The observed wx candidate completed the existing 16-case native fixture.
  Stage diagnostics were disabled at runtime. Its drawing code uses the same
  backend files as the candidate worktree.
- An earlier launch of the ordinary application could not answer the fixture's
  readonly observer protocol. That setup failure is retained, not treated as a
  performance result.
- A fresh old-wx run passed 12 cases, then timed out delivering a native message
  in `XYDrawNormalXYZShear`; three remaining cases did not run. It does not
  establish a complete fresh baseline. No retry or driver timeout change was
  used to turn that failure into a pass.

The screening comparison therefore uses the retained successful old-wx run of
the `33a2b455` implementation. Each new pose has 192 measured draws. Draw includes
`Finish()`. The combined metric is the measured per-frame span from pass
preparation through drawing and finish; it is not the sum of phase percentiles.

| Pose | Old draw P95 ms | Draw limit ms | Candidate draw P95 ms | Prepare P95 ms | Combined P95 ms | Frame P95 ms |
|---|---:|---:|---:|---:|---:|---:|
| Neutral | 0.0433 | 0.0533 | 0.0675 | 0.0377 | 0.1181 | 16.7190 |
| NormalXYZShear | 0.0607 | 0.0707 | 0.0669 | 0.0365 | 0.1189 | 16.7090 |
| EdgeOn | 0.0506 | 0.0606 | 0.0765 | 0.0463 | 0.1451 | 16.7888 |
| NearCamera | 0.0514 | 0.0614 | 0.0717 | 0.0375 | 0.1350 | 16.7328 |

Only one draw metric passes `max(old * 1.10, old + 0.01 ms)`, and all four combined
metrics fail that limit. All four frame metrics fit their historical
`max(old * 1.10, old + 0.05 ms)` limits. The increased preparation cost prevents
claiming that moving capture to pass entry solved the overall cost.
Per-request completion and trace ordering were checked; fresh cross-process
clock calibration and a complete fresh reference run remain unproven.

Evidence is retained in the external run bundle
`aegisub-xy-owned-boundary-20261105-001`, including both failed GUI runs, successful
candidate results, original trace data, XML, binary identities, patches, and the
reproducible `compare_screen.py` analysis. This experiment does not change
Measure semantics. Gac GUI/performance, its three renderers and docked/detached
workspace acceptance have not been rerun because the wx screening gate failed.
No commit, integration or push is certified by this result.

### Canonical owned-pass candidate, 2026-11-05

The next uncommitted candidate replaces arbitrary entry-state preservation with
an explicit window-overlay contract. `SetupCanvas` establishes the projection,
modelview and compatibility state. Between that setup and pass construction,
host guides must preserve the canvas baseline. During the pass, drawing owns
the clip, shading and array state; callers may not mutate those states behind
the pass. Its exit baseline is orthographic projection, identity modelview,
MODELVIEW mode, disabled clip plane 0 and client arrays, FLAT shading, and an
all-enabled colour mask. The equation of a disabled clip plane is unspecified.
The unowned drawing path still preserves its supported caller state.

The wx normal finish drains submission errors once and performs no redundant
state writes. Gac suppresses alpha writes during the pass, restores the mask,
and checks that actual restoration separately. Recoverable GL submission errors
trigger full canonical restoration before being thrown. C++ unwinding restores
the same baseline, including normal and texture-coordinate arrays. Outstanding
GL errors during non-throwing destruction remain visible and fatal.
Split-canvas pending errors are reported by `Finish()`; direct construction
still checks pending errors in `SetupCanvas`.

The full `GL_MAX_CLIP_PLANES` value is queried after a context becomes current.
wx stores it with `VideoDisplay::glContext` and clears it on unload. Gac queries
it with the surface's existing viewport-capability initialization and clears it
in `release_context`. Preparation consumes that value without another query,
a process-static cache, a context lookup, or an eight-plane cap.

The focused test changes preceded implementation. Against the preceding
implementation, canonical finish, split-canvas error timing and unwind
assertions failed. With this candidate:

| Requirement | Evidence |
|---|---|
| Near clipping, gradients, multiple batches and subsequent 2D | `ValidateXYOwnedPass` passed; all 148 existing RGBA files matched the preceding candidate byte for byte |
| Full stack operation without consuming stack entries | `ValidateXYOwnedPassStackBoundaries` passed |
| Deferred errors and canonical error recovery | `ValidateXYOwnedPassFinishError`, `ValidateXYOwnedBatchEntryChanges`, `ValidateXYOwnedPendingEntryError` passed |
| Direct/split entry errors and C++ unwind | `ValidateXYOwnedPassBoundaries` failed against the previous contract and passed with the canonical contract |
| Zero batches, ordinary lines, text and alpha protection | `ValidateXYOwnedWithoutPerspective` passed all six host/mode combinations; each checks the exit baseline, and the drawing modes also check pixels |
| Shared C++ regression coverage | `test-aegisub`: 2,997 selected, 2,978 passed, 19 skipped, zero failures/errors |

The ordinary wx application and observed wx application were rebuilt.
Clang-tidy completed for the shared OpenGL implementation, wx display and WGL
smoke translation units after disabling unavailable PCH artifacts in the
separate analysis build and generating its resource headers. Existing warnings
remain; no modernize/performance warning was found in the newly added functions.
At the initial A checkpoint, the Gac host translation unit had not yet been
built or analyzed. Post-test formatting was verified to change whitespace only.
The first attempts did not produce a complete fresh performance cohort:

- The unchanged acceptance driver failed in `RotateZOrigin` on a synchronous
  `WM_COMMAND` timeout before reaching the four timing poses.
- A diagnostic-only candidate run failed in `RotateXYShiftControl` waiting for
  viewport capture publication. Its trace contains a 7,929.28 ms scene-render
  interval before overlay preparation.
- A diagnostic-only old-fast-wx run failed in `ScaleHold` on viewport capture.
  The failure dump shows the UI thread in
  `VideoDisplay::ReadRenderedViewport -> glReadPixels -> nvoglv64`; a driver
  worker was in `NtGdiDdDDIGetDeviceState`. This establishes a failure in the old
  baseline too, but does not prove a common root cause for every retained
  message timeout or identify the underlying driver/device fault.

The diagnostic driver only captures a failure dump before normal failure
reporting and cleanup. It preserves the original input protocol, scenario
assertions and deadlines. Diagnostic runs are not acceptance runs.
No failed scenario was omitted, no deadline was extended, and no observer
overhead was subtracted. The strict draw, frame and combined gates are unchanged.

Evidence is retained in the external bundle
`aegisub-xy-canonical-20261105-001`: preceding and candidate patches, red/green
WGL logs, pixel comparison, unit-test XML, build receipts, all failed GUI
results, and the old-baseline failure dump and CDB stack report.
Static geometry/loop merging was deferred at that checkpoint until A could be
measured separately. The subsequent A/B results below supersede that progress
status; the failed runs remain retained.

### A/B follow-up and remaining gate, 2026-11-05

The unchanged native driver subsequently completed all 16 cases for the old
`33a2b455` implementation (`wx-reference-final`), A (`wx-a-final`), and B (`wx-b`).
Each pose has 191 or 192 measured draws. Completion/request/trace ordering and
snapshot/context identities were checked by `compare_fresh.py`; this remains
a screening comparison, without a newly proven cross-process clock calibration.
No failed attempt was discarded.

A removes the arbitrary entry-state queries and uses the context-owned clip
limit. B additionally moves the existing 248/6/18 vertices into one immutable
`static constexpr` aggregate and combines grid validation with colour packing.
All vertices are still validated before GL state is changed; the overflow bound
still uses double arithmetic. The owned restore retains its scope guard.

| Pose | Fresh old draw P95 ms | Strict draw/combined limit ms | A draw P95 ms | A prepare P95 ms | A combined P95 ms |
|---|---:|---:|---:|---:|---:|
| Neutral | 0.0506 | 0.0606 | 0.0525 | 0.0009 | 0.0665 |
| NormalXYZShear | 0.0509 | 0.0609 | 0.0696 | 0.0013 | 0.0874 |
| EdgeOn | 0.0447 | 0.0547 | 0.0662 | 0.0011 | 0.0857 |
| NearCamera | 0.0391 | 0.0491 | 0.0506 | 0.0008 | 0.0939 |

| Pose | B draw P95 ms | B prepare P95 ms | B combined P95 ms | B frame P95 ms |
|---|---:|---:|---:|---:|
| Neutral | 0.0578 | 0.0011 | 0.0795 | 16.7033 |
| NormalXYZShear | 0.0654 | 0.0013 | 0.0824 | 16.7065 |
| EdgeOn | 0.0812 | 0.0015 | 0.1513 | 16.7772 |
| NearCamera | 0.0749 | 0.0013 | 0.1309 | 16.6844 |

Both A and B pass only one of four draw gates, zero combined gates, and all
four frame gates. The original historical reference remains retained separately;
it is not replaced to obtain a passing result. B has not demonstrated a P95
improvement over A.

The combined interval still starts before pass construction and ends after
`Finish()` and the existing draw-scope recording. It includes preparation
recording, snapshot selection/identity bookkeeping, and draw recording. The
per-frame value `combined - prepare - draw`, computed before taking percentiles,
has B P95 values of 0.0234/0.0220/0.0660/0.0715 ms in pose order. These values
locate work outside the two inner intervals; they are not a measurement of GL
overhead or of trace overhead alone and are not subtracted from acceptance.
The two trace writes take a mutex, update summary data, construct JSON and can
flush the bounded trace buffer synchronously. The retained event format does
not identify which individual writes flushed or waited for the mutex. A
measurement-only recording redesign remains a proposal, not an implemented
change or a reason to waive the existing gates.

The initial B run had only before/after GPU readings: 90/92 degrees C and
300 MHz. Those readings do not establish thermal throttling throughout that
run or explain every retained timeout. A separate readiness observation from
23:22:14 to 23:24:09 local time captured 24 samples over approximately 115
seconds: 88-90 degrees C, 300 MHz, software thermal slowdown active in all
24 samples, and its cumulative counter increasing by 115,021,851 microseconds.
Hardware thermal slowdown was inactive in those samples. This proves the
environment was unsuitable for another clean timing comparison at that time;
the existing gate failures remain failures. No additional GUI performance run
was started during this readiness observation.

B verification:

- `detached-visual-tools-smoke` exited successfully; all 148 corresponding
  RGBA outputs matched A, with no missing or differing files.
- The wx RelWithDebInfo build completed `Aegisub` and `test-aegisub`:
  2,997 selected, 2,978 passed, 19 skipped, zero failures/errors.
- The Gac-enabled RelWithDebInfo configuration, with libplacebo enabled,
  built `gacui-visual-tools-smoke` and the full `Aegisub` application.
  Its `test-aegisub` run selected 3,011 cases: 2,978 passed, 33 skipped,
  zero failures/errors. The skips include the prior 19 plus one placebo
  runtime case, one L-SMASH availability case and 12 compressed-audio cases;
  these were initially not runtime coverage. After staging the existing
  libplacebo and L-SMASH DLLs into the build's application-local `runtimes`
  directory, the complete suite was run again: 3,011 selected, 2,992 passed,
  19 skipped, zero failures/errors. The placebo load test and all 13 L-SMASH
  cases then completed. This validates library loading and those CPU tests,
  not actual Gac renderer/device behavior. Both XML results are retained.
- Clang-tidy completed for the B OpenGL implementation/XY translation units
  and the Gac video surface. No modernize/performance diagnostic was emitted
  in the changed implementation functions; existing diagnostics remain.
- An independent A-to-B source/evidence review found no blocking correctness
  issue in the static geometry, validation ordering, or restore adjustment.
  It did not claim performance acceptance or review every earlier dirty file.

The same evidence bundle now includes `candidate-b.patch`, `canonical-b-review.md`,
`wgl-b`, `full-b.xml`, `full-gac-b.xml`, the A/B comparison JSON, the directly
computed residual comparison, `measurement-review-b.md`, `gpu-readiness-01.json`,
the runtime staging receipt and `full-gac-b-runtimes.xml`. The remaining work is
a bounded old/candidate comparison in a stable environment, analysis of any
remaining measured cost without changing the gates or timing boundaries, then
the actual Gac renderer/workspace acceptance. Gac GUI and performance have not
been run for B. The candidate remains uncommitted and is not ready to merge or
push.

### Cooled reference/B pair, 2026-11-06

The first readiness observation on November 6 still showed software thermal
slowdown in all 24 samples, so no GUI lane was launched. After cooling, the
next 115-second readiness observation remained at 46-47 degrees C with neither
thermal flag active and no thermal-counter increase. The one remaining
reference/B pair was then executed with unchanged production source, observer,
driver, scenarios, timeouts and binary identities. Only artifact destinations
changed. Each lane completed all 16 cases, exited successfully and preserved
the expected completion/request/snapshot/context associations. No retry was
performed. This uses the second allowed B pair.

During the pair, 19 telemetry samples all reported 46 degrees C, neither
thermal flag was active, and both thermal counters stayed constant. Sampled
graphics clocks were 1080 MHz except for one 1350 MHz sample. This removes
the observed thermal-throttling blocker for this pair; it does not establish
identical scheduling or driver execution costs.

The comparison applies the fresh reference formulas and retains both earlier
draw/frame caps. The effective limit is the minimum of those limits, so a
slower fresh baseline cannot relax acceptance. Units below are milliseconds.

| Pose | Fresh old draw P95 | Effective draw/combined limit | B draw P95 | B prepare P95 | B combined P95 | B frame P95 |
|---|---:|---:|---:|---:|---:|---:|
| Neutral | 0.0423 | 0.0523 | 0.0747 | 0.0015 | 0.0977 | 16.6852 |
| NormalXYZShear | 0.0477 | 0.0577 | 0.0856 | 0.0020 | 0.1201 | 16.7708 |
| EdgeOn | 0.0614 | 0.0547 | 0.0317 | 0.0013 | 0.0428 | 16.6304 |
| NearCamera | 0.0554 | 0.0491 | 0.0304 | 0.0014 | 0.0817 | 16.6312 |

Reference sample counts are 192 per pose; candidate counts are
192/189/190/192, all above the original minimum. Draw passes 2/4, Combined
1/4, and Frame 4/4, including the retained caps. The candidate still fails
acceptance. Preparation is small; neither cooling nor the B CPU reductions
have established overall compliance.

Existing per-frame trace evidence also shows a change in where presentation
time was spent, without any source or timing-boundary change:

| Candidate pose | Scene P50 | Swap P50 | Samples with swap greater than 8 ms |
|---|---:|---:|---:|
| Neutral | 16.0244 | 0.0420 | 0/192 |
| NormalXYZShear | 16.0085 | 0.0376 | 33/189 |
| EdgeOn | 0.0553 | 16.2604 | 190/190 |
| NearCamera | 0.0540 | 16.2454 | 192/192 |

Every measured scene interval uses `video_display.scene_cache.direct.policy`.
The reference's scene P50 remains 16.0662-16.1828 ms and its swap P50 remains
0.0226-0.0326 ms. Both report native OpenGL presentation, swap interval 1 and
60 Hz nominal refresh. Thus the shorter candidate Draw intervals for the last
two poses coincide with presentation time appearing in SwapBuffers instead
of the earlier scene interval. The trace does not identify the specific driver
call or scheduling cause; this correlation is not proof of an intrinsic
geometry speedup or of work deliberately moved by the implementation.
Full-frame measurements remain part of acceptance.

For B, same-frame `combined - prepare - draw` P95 is
0.0247/0.0363/0.0112/0.0596 ms. These residuals remain diagnostic only.
Whole-session update accounting is also retained: override P95 increases from
0.0122 to 0.0203 ms (1,482 events each), and commit P95 from 0.2737 to
0.3689 ms (469 events each). Those are not pose-specific measurements and do
not establish a new update regression's cause; they cannot be omitted when
assessing total cost. Finish remains inside Draw, without a separate duration
measurement.

Evidence is in `aegisub-xy-recheck-20261106-002`: `identity.json`, both complete
GUI lanes, `gpu-readiness-01.json`, `gpu-pair.json`, `comparison.json`,
`strict-gates.json`, and reproducible `phase-accounting.py`/JSON. All previous
failures remain retained. Further identical acceptance reruns are not planned;
the next diagnostic must distinguish driver/presentation waiting and trace
recording costs before choosing another implementation or measurement change.
No Gac GUI run, commit, merge or push was performed.

### Observer v2 and bounded CPU/GL convergence, 2026-11-06

The cooled trace's periodic residual was investigated before changing product
timing. Replaying the buffer policy places the inferred 64-entry flush on the
Prepare observation, not on the following Draw observation. The earlier trace
does not directly measure flush duration, CPU wakeup costs or the exact driver
call responsible for scene/Swap waiting. Those explanations must not be treated
as established hardware causes.

The experimental observed builds now share a bounded, thread-local raw duration
recorder. It captures the original phase timestamps, duration, owned phase text,
details and immediate flag, then serializes records after the existing Combined
endpoint. The same recorder is used in the old wx reference. Explicit drain,
its own reporting tail and receipts remain before Swap and inside Frame.
Global flush policy is unchanged. Product selection, drawing, GL submission
checks and pass Finish retain their previous timing endpoints.

The recorder has capacity 16, rejects nesting, wrong-thread Finish, repeated
Finish and replacement sessions, and reports overflow or incomplete unwinding
as failed receipts. Noexcept duration-scope overflow is reported at explicit
Finish rather than terminating before retaining evidence. The trace reader
checks every matched warmup and measured receipt, original ordering and
timestamps, exact reference/candidate record counts, failure fields and drain
accounting. It rejects detailed stage/query traces. This is a versioned
measurement change, not an uninstrumented product speedup. Recorder changes
remain in the experimental observed trees, outside the primary product patch.

Four old/candidate pairs were executed: observer-only B, separated CPU loops,
one diagnosed CPU correction, and the final GL candidate. The eight-start
budget is exhausted; no failed samples or poses were removed and no automatic
retry was performed. Each pair completed the original 16 cases on both sides.
Fresh readiness and pair telemetry remained at 46 degrees C with no observed
thermal flag or counter changes.

Product changes relative to B are confined to the shared GL backend:

- All vertex finite/opacity checks precede a separate colour packing loop.
  Generated code first exposed an additional per-vertex initializer-list max
  helper; replacing it with three binary max comparisons removes calls from
  the successful validation loop. Colour packing is scalar unrolling, not
  claimed SIMD.
- Only Owned passes with depth at least 1 omit projection flattening and its
  normal restore. The composed eye Z is -1, and the near plane accepts
  homogeneous W at least 1, so the canonical depth clip is redundant for this
  branch. Owned depths below 1 and Unowned contexts retain flattening.
- Colour-array ownership removes the duplicate successful disable while
  retaining scope cleanup and the existing full pass error restoration.
  Finish error checking remains inside Draw.

The depth optimization was preceded by additional preservation cases in
`ValidateXYOwnedPass`: a depth 0.5 host and denominator 1.5. Its 18 host/case
combinations preserve both near-boundary directions, gradient witnesses,
multiple batches, following 2D drawing, invalid-input handling and pass state.
Before/after WGL runs pass; all 173 RGBA files are byte-identical. The final
test-initializer cleanup was rebuilt and the WGL test rerun.

The final strict decision retains the minimum of every preceding historical
and v2 Draw/Frame cap. In particular, the CPU pair's Neutral cap of 0.0512 ms
still applies. The final pair's original comparison retained the three older
historical sets; `final-strict-gates.json` adds the intervening v2 caps and is
the authoritative cumulative result. Units are milliseconds.

| Pose | Cumulative Draw/Combined cap | Draw P95 | Prepare P95 | Combined P95 | Frame P95 | Draw / Combined / Frame |
|---|---:|---:|---:|---:|---:|---|
| Neutral | 0.0512 | 0.0495 | 0.0008 | 0.0521 | 16.7037 | pass / fail / pass |
| NormalXYZShear | 0.0501 | 0.0516 | 0.0009 | 0.0546 | 16.6936 | fail / fail / pass |
| EdgeOn | 0.0534 | 0.0470 | 0.0007 | 0.0485 | 16.7184 | pass / pass / pass |
| NearCamera | 0.0491 | 0.0488 | 0.0008 | 0.0511 | 16.6705 | pass / fail / pass |

The final Frame caps are 18.31346/18.31775/18.32886/18.31951 ms.
Draw passes 3/4, Combined 1/4 and Frame 4/4. NormalXYZShear Draw exceeds its cap
by 1.5 microseconds. Combined exceeds the cap by 0.9/4.5/2.0 microseconds for
Neutral/NormalXYZShear/NearCamera. These are failures, even where margins are
small. The candidate remains unaccepted.

Final same-frame residual P95 is 2.5/4.1/1.8/2.5 microseconds. Candidate drain
P95 is 21.6/21.0/20.2/65.9 microseconds, with its reporting tail still charged
to Frame. No residual or drain cost was subtracted. Whole-lane override P95
is 0.0131 ms reference versus 0.0197 ms candidate, and commit P95 is 0.3055
versus 0.3301 ms, with respectively 1,482 and 469 observations on each side.
These increases remain reported without assigning an unsupported cause or
inventing a separate update gate. In this final pair all scene groups contain
the dominant presentation wait; no Swap sample exceeds 8 ms.

The final primary wx C++ suite selects 2,997 tests: 2,978 pass and 19 skip by
configuration. Before the CPU/GL experiments, the separate observer suite
selected 3,008: 2,989 passed and the same 19 skipped, including all 11 new
recorder tests. Replacement-session handling
has executed failing-before/passing-after evidence. Its initial test build
failure and stale observed-tree test failures remain recorded; the latter
were corrected by restoring the primary arrow endpoint assertions, using the
existing main-thread interface in the experimental input hook, and registering
the diagnostic header in the exact surface inventory. Missing test runtimes
were staged rather than accepting additional skips.

The final Gac configuration builds the application, `gacui-visual-tools-smoke`
and C++ test binary. Its C++ suite selects 3,011 tests: 2,992 pass and 19 skip.
This is build/unit coverage only. Real Gac renderer, performance and
docked/detached workspace acceptance remain pending the wx gate.

Evidence is grouped under `aegisub-xy-observer-v2-20261106-001`, with separate
`cpu`, `cpu-correction` and `gl` records. Each preserves build/test outcomes,
source/binary identities, trace/cohort input, telemetry and comparisons. The
final review, cumulative gates, preparation/drain/update/scene accounting and
pixel comparison are in `gl`.

An evidence-path mistake during v2 readiness preparation overwrote the older
`aegisub-xy-recheck-20261106-002/gpu-readiness-01.json` with fresh v2 samples.
The original older readiness file is unavailable; it has not been restored
or reconstructed. Both evidence directories carry the incident notice.
The earlier pair telemetry, traces and timing results were not overwritten.
Only the fresh timestamped v2 file supports v2 readiness.

No Measure semantics changed. No commit, merge or push was performed.
The candidate and observed trees remain retained for the unresolved performance
decision. Further work requires an attributed cost diagnosis using the frozen
final source and all cumulative caps, followed by a separately bounded
experiment; another identical acceptance rerun is not a solution.

### Timestamp, local GL state and update-path iteration, 2026-11-06

The next bounded experiment used three successive reference/candidate pairs
(six wx starts, no automatic retries). Its evidence group is
`aegisub-xy-local-state-20261106-001`, with `observer`, `gl` and `update`
subdirectories. Every pair passed the unchanged recording protocol and all
16 native scenario checks. None passed the cumulative strict performance gate.
The failed pairs and all original samples remain retained.

The observer-only change samples the scope end once and uses that endpoint
for both duration and record timestamp. The ordinary emission path accepts
the same timestamp internally. Recorder phase text remains owned; session,
overflow, drain and flush contracts are unchanged. This instrumentation is
confined to the observed candidate and reference sources. Product scope
lifetimes, snapshot selection and Prepare-through-Finish boundaries did not
move.

The shared GL change omits two MODELVIEW selections only for the Owned
depth-at-least-one path that never switches projection. Between grid and
axes, it applies the primary current colour without repeating the unchanged
blend, line width and smoothing settings. GL_FLAT restoration is necessary
and remains. Generic vertex validation, near clipping, three submissions,
projection fallback, error checks and recovery remain. No persistent colour
cache or caller-supplied trusted-geometry certificate was added.

The update change makes `SetOverride(AssDialogue&, ...)` the shared mutation
implementation, with the string overload delegating to it. wx mutates its
existing dialogue, and `PlanGeometryEdit` mutates only its local candidate.
XY, Z and Scale refresh and geometry snapshot capture each read one local
shared `Line` value. Text ownership, style defaults, move/origin fallback and
Commit semantics remain unchanged. The former extra construction and repeated
full reads came from the shared migrations in `cce3f883c2e`, `21f1e6db396`
and `760ac110360`; they are separate from the original wx GL regression.

Final raw results are milliseconds. Caps are the minimum of all previous
valid pair caps and each new pair's limits; the three new pairs did not
loosen or further tighten them.

| Pose | Draw/Combined cap | Draw P95 | Prepare P95 | Combined P95 | Frame P95 | Draw / Combined / Frame |
|---|---:|---:|---:|---:|---:|---|
| Neutral | 0.0512 | 0.0656 | 0.0011 | 0.0697 | 16.7130 | fail / fail / pass |
| NormalXYZShear | 0.0501 | 0.0630 | 0.0011 | 0.0658 | 16.7314 | fail / fail / pass |
| EdgeOn | 0.0534 | 0.0644 | 0.0012 | 0.0681 | 16.6663 | fail / fail / pass |
| NearCamera | 0.0491 | 0.0516 | 0.0010 | 0.0542 | 16.7119 | fail / fail / pass |

Final measured candidate counts are 192/192/192/191. The unchanged Frame
caps are 18.31346/18.31775/18.32886/18.31951 ms. Residual P95 is
4.6/4.0/4.6/2.4 microseconds, and drain P95 is 26.9/29.6/28.5/60.9
microseconds. Drain and its reporting tail remain charged to Frame; none
was subtracted from Draw or Combined. All three pairs have zero measured
Swap intervals above 8 ms and retain predominantly long Scene intervals.
Pair telemetry records 42–44 C, varying graphics clocks and no active thermal
flags or increasing thermal counters. This does not exclude CPU, driver or
scheduling effects, and cross-pair differences are not causal timing proof.

Whole-lane final Commit has 469 observations per side: reference P50/P95
131.4/329.8 microseconds, candidate 129.3/428.2. The ordinary local Commit
uses SkipSelf and therefore does not execute its own DoRefresh. Source
comparison found no new commit-listener registration topology; existing
traces do not identify the uninstrumented listener or wait responsible for
the tail. The parse-once change is not claimed as a demonstrated Commit
tail fix.

Final override P95 is 12.9 microseconds reference and 12.1 candidate, but
counts differ (1,482 versus 1,770). These whole-lane populations cannot be
used alone to claim an equivalent-work performance improvement. The final
trace confirms 1,458 text-changing observations on each side and 24 versus
312 unchanged observations. Within the text-changing stratum, reference
P50/P95 is 4.9/12.9 microseconds and candidate is 5.1/12.3; this descriptive
stratum does not replace the full-lane accounting or prove a causal gain.
Driver input inventories match at 513 entries per side. The final trace and
case-level count review remain part of the evidence.

Validation for this source:

- WGL state/raster smoke passed; all 173 RGBA outputs match the previous
  final candidate byte for byte, including small-depth fallback, near-plane
  visibility, gradients, following 2D drawing and error recovery.
- Shared update/controller/policy tests: 36 passed. Four new tests cover
  literal override/alias results, dialogue identity and metadata, owned text
  and style geometry, and explicit move/origin/transform fields.
- Final primary wx suite: 3,001 selected, 2,982 passed, 19 skipped.
  Final observed suite: 3,012 selected, 2,993 passed, 19 skipped.
  Gac configuration: 3,015 selected, 2,996 passed, 19 skipped.
- Application and smoke targets built for wx and Gac configurations.
  Changed production/test translation units were checked with the independent
  clang-tidy databases; final changed lines have no new modernize/performance
  warning. Independent source and test-quality reviews found no blocking
  defect in this iteration.

The first new metadata test incorrectly compared rounded integer conversion
against raw milliseconds. It was corrected to assert the same literal
1,234/5,678 values through `GetMillisecond`, without changing product time
behavior. Both original full-suite failures are retained. A later concurrent
wx full run failed `lagi_audio.wave64_truncated` with an unexpected exception
type. The two configurations share that test's fixed system-temp filename;
this is a concrete interference risk, but the actual exception type was not
captured. One subsequent serial wx full run passed. No audio code or assertion
was changed, and future full suites must run serially or isolate temp roots.
Initial static-analysis command setup failures also remain retained.

The six-start budget is exhausted. Real Gac renderer/performance and
docked/detached final acceptance remain unrun because wx still fails.
Measure behavior is unchanged. No commit, merge or push was made. The primary
candidate, observed tree and corresponding builds are retained to diagnose
the unresolved performance gap; this iteration is implemented and checked
for the stated scope, but is not accepted for integration.

### Captured boundaries, immutable geometry and Commit diagnostics, 2026-11-06–07

The evidence group is `aegisub-xy-input-static-20261106-001`. Product changes
remain on the uncommitted `322c35b` candidate. This iteration is not accepted
for integration: the final wx pair still fails Neutral, the complete new
input suite did not pass, and real Gac renderer/workspace acceptance remains
unrun.

Generic tools now ignore Enter/Leave only while an interaction is active
and their VideoDisplay owns capture. This happens before coordinates,
modifiers, commit identity or feedback state change. The host consults the
same tool policy before overwriting its pointer. Idle, Motion, LeftUp and
uncaptured paths retain their behavior; capture-lost cancellation remains
separate. Measure uses the base policy and was not changed.

The old Enter fallthrough predates shared-transform migration. History
inspection identifies captured holding in `a1050db3f347`, the Leave early
return in `bd8b8ce4c51f`, and the current template/Vector2D handling in
`be77dc8307d5`. The current old-handler control reproduces the chain:
captured Enter with a false left-button flag changes three selected rows,
advances document revision 8 to 9, and releases capture. This does not prove
the native cause of the earlier unsolicited Enter/Leave events.

The corrected fixed run completed `HostAndFoldSetup`,
`CapturedXYHoldBoundary`, `CapturedXYOriginBoundary`,
`CapturedZHoldBoundary`, `CapturedScaleHoldBoundary` and `IdleHoverBoundary`.
All sixteen captured Enter/Leave receipts preserve document/history/selection,
capture and host/tool pointer state. Their trace intervals contain no Commit
or feedback request, with 23 ordinary Commit and 41 feedback records elsewhere
as positive witnesses. The cases include folded hidden selected rows,
same-coordinate explicit wx Motion with changed modifiers, distinct native
LeftUp, independent final geometry and one Undo.

The input suite as a whole still exits unsuccessfully. Its first two runs
failed before boundary injection because the driver assumed native wx Motion
at the same screen coordinate would be dispatched, then compared an unordered
selected-ID set by enumeration order after Undo. Local wx 3.3.1 filters the
former before the tool handler. After those corrections, the fixed run failed
at capture-loss because GetRawText compared top-level and nested JSON
indentation. Parsed fields and the receipt show capture loss cancels without
changing document/history, but subsequent late Motion/Up, capture-loss Undo
and normal final shutdown were not executed. The structural comparison fix
builds in a separate unrun driver output; it is not a passing rerun.
All failures remain retained.

The declared budget was explicitly revised from eight to ten actual wx
application starts after the first two driver failures: four input runs, one
static pair, one final pair and one diagnostic pair. No performance pair was
retried or sample excluded. A later rejected diagnostic preflight started no
driver or application and is retained separately. The boundary test hook was
initially inserted into a stale observation tree; only that addition was
removed and transferred to the correct tree before any corresponding build
or launch. Existing hooks were preserved.

Static XY geometry now has an owning immutable type whose consteval
constructor validates vertices/opacity and derives its private coordinate
bound. A prepared packet owns vertices and packed colours; it can only be
constructed from that validated geometry. Generic caller-provided spans
retain per-vertex checks. Both entry points use one GL submission function
and retain dynamic transform, scale, width and overflow checks.

wx tools and the shared controller hold the same one-entry colour cache.
Snapshots retain immutable packets across recolouring, Reset and producer
destruction. The key includes full RGBA; opacity semantics remain unchanged.
A miss allocates one shared object and four vectors, with 8,320 payload bytes.
Those costs stay within the existing capture/update or first live Draw path.
The current trace does not identify individual cache misses or recolours, so
their isolated timing benefit or cost is not established.

The static pair passed protocol and all sixteen existing native cases per
side, but Draw/Combined passed 0/4 and Frame 4/4. Candidate Draw P95 was
0.0627/0.0762/0.0728/0.0630 ms; Combined was
0.0675/0.0870/0.0789/0.0675 ms. The final pair also passed protocol and native
cases; its raw results are:

| Pose | Draw/Combined cap | Draw P95 | Prepare P95 | Combined P95 | Frame P95 | Draw / Combined / Frame |
|---|---:|---:|---:|---:|---:|---|
| Neutral | 0.0512 | 0.0771 | 0.0017 | 0.0844 | 16.6455 | fail / fail / pass |
| NormalXYZShear | 0.0501 | 0.0336 | 0.0017 | 0.0370 | 16.5847 | pass / pass / pass |
| EdgeOn | 0.0534 | 0.0341 | 0.0016 | 0.0370 | 16.5909 | pass / pass / pass |
| NearCamera | 0.0491 | 0.0373 | 0.0020 | 0.0401 | 16.5781 | pass / pass / pass |

Final measured candidate counts are 165/191/191/191. Swap exceeds 8 ms in
62/190/190/191 of those frames; the static pair has no such measured frame.
The last three improved distributions therefore do not isolate the cache or
input change from the synchronization-phase change. All original cohorts,
drain costs and cumulative caps remain intact. No CPU-frequency mechanism is
inferred. Whole-lane final Commit P50/P95 is 144.4/418.5 microseconds reference
and 150.6/455.9 candidate, with 469 samples each. Both have 1,482 overrides.

Commit diagnostics are confined to observation sources. Raw stage scopes
preserve PushState, AnnounceCommitDetails, AnnounceCommit order and the outer
timing endpoints. Original trace locking and flushing remain in place; raw
lock/flush counters do not recursively log. Receipt publication occurs after
the original endpoint and reports its own partial processing cost separately.
The accepted launcher initially rejected the diagnostic variable before
starting an application. A diagnostic-only launcher then allowed exactly
`AEGISUB_COMMIT_DIAGNOSTICS=1`, rejecting absent, wrong or extra overrides
while retaining identity, ownership, deadline and native-case checks.

Both diagnostic lanes completed all sixteen native cases. Each has 469
ordinary Commit records, 469 complete ordered receipts and 469 processing
records, with no invalid/overflow/aborted receipt. Both record 451 history
amends, 18 full snapshots, 459 deferred and 10 synchronous editor branches.
The following diagnostic values are microseconds and are not acceptance data:

| Scope | Reference P50 / P95 / Max | Candidate P50 / P95 / Max |
|---|---:|---:|
| Whole Commit | 148.8 / 671.2 / 1774.5 | 152.6 / 565.7 / 1278.1 |
| PushState | 7.2 / 10.9 / 21.8 | 7.3 / 11.1 / 20.9 |
| AnnounceCommitDetails | 56.3 / 109.6 / 210.3 | 56.8 / 112.8 / 256.2 |
| AnnounceCommit | 75.0 / 596.1 / 1690.8 | 76.4 / 461.6 / 1210.7 |

The dominant tail stage is AnnounceCommit. Trace-lock wait P95 there is
18.2/14.3 microseconds, and synchronous trace flush occurs in 6/8 commits;
AnnounceCommitDetails flushes in 29/31. Logging interference is observed but
does not explain the full tail. Candidate sequence 244 spends 1,210.7
microseconds in AnnounceCommit, with no trace flush, 31.0 microseconds of trace
lock wait and a 486.5-microsecond STC freeze on its synchronous editor path.
Reference sequence 21 instead takes the deferred path and spends 1,526.1
microseconds in AnnounceCommit with no flush or measured trace-lock wait.
Individual remaining listeners and non-trace waits remain unlocalized.
Stage, lock and flush intervals overlap; they were not added or subtracted
to manufacture corrected performance results.

Validation and limits:

- Final prepared/controller tests: 37 passed. New cases include
  `visual_transform_overlay.prepared_geometry_owns_vertices_and_packed_colours`,
  `visual_transform_overlay.canonical_geometry_bound_and_cache_preserve_retained_colours`
  and
  `visual_edit_controller.rotate_xy_snapshots_reuse_prepared_geometry_without_recolouring_retained_frames`.
- WGL smoke passed with 200 RGBA witnesses: all prior 173 remain byte-identical,
  plus 27 prepared/generic, retained-colour and independent-gradient witnesses.
  Invalid generic and prepared dynamic inputs still reject without GL mutation.
- Final primary full suite: 3,004 executed, 2,985 passed, 19 skipped.
  Gac configuration: 3,018 executed, 2,999 passed, 19 skipped. Application and
  Gac smoke targets build, but no real Gac GUI/smoke run is claimed.
- Diagnostic observed full suite: 3,020 executed, 3,001 passed, 19 skipped.
  After enum-width and designated-initializer corrections, the target rebuilt
  and `PerfTrace.*` passed 27/27, including all five new CommitDiagnostic
  cases. The full diagnostic suite predates those representation-only changes.
- Changed product and diagnostic lines have no new modernize/performance
  warning after targeted clang-tidy closure. Readability findings remain
  recorded rather than suppressed.

No commit, merge or push was performed. The candidate and observation sources,
builds, frozen acceptance binaries and failed evidence remain retained for
the unresolved performance and input-suite decision. The current observation
binaries include Commit diagnostics and must not be reused as acceptance
binaries. Gac renderer/performance, docked/detached workspace acceptance,
active-without-capture input coverage and other platforms remain unverified.

### Input suite completion and bounded localizations, 2026-11-07

The corrected input suite passes all eight cases, including
CaptureLossCancelsWithoutExtraCommit and RuntimeAndTraceEvidence. Late
Motion/Up, retained preview, one exact Undo, selection preservation,
sixteen clean captured-boundary intervals, 23 ordinary Commit and 44
feedback positive witnesses were checked. Artifacts remain in
`input/fixed-semantic/run2`; original loader/preflight failures are
retained. The old-handler red fails at capture before the document
comparison, so the semantic comparator correction does not weaken it.

The passing run used frozen final candidate `ae594beb…700002f`, semantic
driver DLL `2b3def3f…c41a59`, and an assembled runtime layout recorded in
its manifest. Older DLL timestamps alone do not establish historical
byte identity; the result binds to that assembled layout. Current
observer output was already diagnostic and was not reused as acceptance.

Budget audit: the revision preregistered one added input launch, total11
and input5. A loader failure (0xC0000135, missing companion DLLs) consumed
that slot. A driver preflight rejection created no application; the
subsequent successful launch was one extra unregistered start, giving
actual12/input6. The ledger now records this overrun explicitly and does
not retroactively authorize it. No further start is authorized by that
artifact group's plan.

Offline analysis correction uses nearest-rank percentiles and the exact
`video_display.scene_cache.direct.policy` scope duration. Final Neutral:

| Scene-duration stratum | Frames | Draw P50 / P95 ms | Draw above 0.0512 ms |
|---|---:|---:|---:|
| Reference > 8 ms | 191 | 0.0400 / 0.0592 | 18 |
| Candidate > 8 ms | 102 | 0.0504 / 0.0914 | 49 |
| Candidate <= 8 ms | 63 | 0.0306 / 0.0445 | 2 |

All16 candidate Draw values above0.065ms occur in the long-Scene stratum.
The short-Scene stratum passes P95, not every frame. Full Neutral remains
failed at Draw0.0771/Combined0.0844ms. Sequence256 has Scene19.8379ms and
Swap12.3037ms; the next frame has Scene0.1185ms and Swap15.5597ms. Later
candidate measured poses have no long Scene and pass P95, while the
reference predominantly remains long-Scene. These are descriptive
phases; no idle/busy pipeline, CPU frequency, or VSync mechanism is
established. Scene policy wraps framebuffer setup and RenderBackendScene,
not a provider-frame wait. Waiting duration alone does not explain the
difference, nor is an interaction between waiting location and candidate
commands excluded.

The static pair's long-Scene candidate Draw P50 remains above reference
across all poses. That observation and final phase changes do not isolate
cache savings or a fixed CPU cost. Traced upload.full spans intersect
zero of165 exact Neutral Draw intervals. The165 versus191 render records
are actual cohort sizes; the driver requests96 Paint operations, not
exactly192 renders. Equal run-wide observer receipts do not prove26
missing tail frames. Reproducible corrected output:
`final/analyse-neutral-draw.py`, `final/neutral-draw-corrected.json`,
`final/neutral-draw-analysis.md`.

Commit analysis now parses worker pipeline markers independently and
uses a temporal leaf partition of observed UI-named scopes. Crossing
scopes remain ambiguous; its sum exactly conserves UI union6512.6us
(reference)/6505.4us(candidate), correcting prior nested double counting.
Ordinary scope records lack thread IDs, so this is not exclusive listener
CPU time. Paired worker render/reuse envelopes overlap41919.4/36296.4us
of AC in aggregate; these include observation overhead and run
concurrently, so they are never subtracted from UI duration.

Each side has10 synchronous-editor receipts, only5 above1ms; the other5
range182.2–554.3us across both sides. Candidate seq244 has AC1210.7us,
text1014.0us (83.75%), and all-observed-UI union1072.0us. Earlier86–94%
figures described union, not a single listener chain. Deferred receipts
above600us number9 candidate (620.2–1036.9us) and18 reference
(600.4–1526.1us). Some have nonzero logger waits/flushes, including
candidate seq302 lock41.2us and seq334 flush89.1us. Seq341 specifically
has no positive-duration observed UI scope and zero trace wait/flush;
its worker envelope overlap288.9us differs from the282.6us render payload.
Missing scopes cannot distinguish uninstrumented work, other waiting, or
preemption.

The source-derived listener inventory includes previously omitted
VideoDisplay::OnSubtitlesCommit and
AudioTimingControllerDialogue::OnFileChanged, alongside BaseGrid,
VideoBox, FrameMain, SubsEditBox, VisualToolBase, search-replace/edit
callbacks and instantiated AudioKaraoke. Source registrations do not
prove the active runtime set. Future probes should identify actual
listener instances and thread IDs, preserving outer Commit endpoints
and publishing afterwards. Corrected script, every synchronous receipt,
all deferred slow receipts and conservation results are retained in
`commit/analyse-announce-listeners.py`, `commit/announce-listeners.json`
and `commit/announce-listener-analysis.md`.

The local code-testing skill is Polyglot; C++ is not a reason to exclude
it. Existing-test-only runs can fall outside its test-editing trigger.
This offline correction used its Python guidance with tests written
before the analysis fix. No native source, GL call, acceptance cap, or
original trace was changed by this correction.

No commit, merge or push was performed. Real Gac renderer/performance,
docked/detached acceptance, physical event origins and
active-without-capture coverage remain unrun. The open targets are Draw
internals in the long-Scene phase and uncovered AnnounceCommit listener
time; neither root cause is yet established.

### Bounded Draw stage diagnosis, 2026-11-07

The diagnostic-only Draw runs completed all 16 native scenarios on both
wx implementations under comparison: old fast wx and the shared candidate.
Each side supplied 895 selected XY frames (768 measured, 127 warmup).
Original Draw endpoints, observer drain order, publication order and
stage trees were validated against raw records. Candidate frames all
used the prepared geometry path. No existing GL command, error check or
outer timing boundary changed for these probes.

| Pose | Reference Draw P50 / P95 us | Candidate Draw P50 / P95 us | Candidate Finish P50 / P95 us |
|---|---:|---:|---:|
| Neutral | 30.8 / 49.4 | 37.5 / 56.3 | 7.6 / 15.2 |
| NormalXYZShear | 30.4 / 43.4 | 35.0 / 47.4 | 7.4 / 13.1 |
| EdgeOn | 30.6 / 47.3 | 36.7 / 53.8 | 7.5 / 13.6 |
| NearCamera | 39.3 / 56.4 | 40.4 / 62.9 | 7.7 / 13.7 |

These independent nearest-rank percentiles are not additive. Per-frame
interval unions and exclusive partitions conserve the original Draw
duration. Candidate grid distributions were close to or below reference;
dynamic validation P95 was 0.3–0.4 us. The recurring additional candidate
region is the whole `pass.Finish()` call, whose normal wx path retains
error collection but does not restore GL state. This localizes elapsed
time without identifying an individual GL-call latency or its cause.

The tails have different sources. Candidate Neutral sequence 287 spent
60.1 us in Finish out of 89.9 us Draw, while EdgeOn sequence 875 spent
69.2 us in grid out of 84.2 us Draw. NearCamera sequence 1244 spent
143.1 us in grid and 25.3 us in Finish out of 179.5 us Draw. Reference
also had grid and snapshot-residual outliers. These observations do not
justify dropping or relocating error detection.

The pair was interrupted after reference by a launcher timestamp-parser
error on the native seven-digit fractional timestamp. Reference had
completed successfully; its original failed controller result remains.
The parser was repaired with isolated tests, its existing process
receipt reconciled, and only the previously unused candidate slot ran.
The recorded gap was 976.969 seconds. Both telemetry segments remained
44–45 C without thermal slowdown flags or counter changes, but these
are not uninterrupted controlled performance runs.

All measured reference frames and 767 of 768 candidate frames had Scene
durations above 8 ms. No measured or warmup frame had a Swap duration
above 8 ms, so this pair does not explain the earlier Scene/Swap phase
transition. Diagnostic setup, observer drain and publication remain in
Frame; no cost was subtracted. No acceptance cap was evaluated or changed.
The previous strict wx failure, and the Gac/workspace obligations, remain.

The diagnostic builds and reviewed tests passed: 34 PerfTrace cases;
full observed C++ suite 3008 passed, 19 skipped, zero failed. Seven
Draw-specific tests include actual scope endpoint copying and unwind,
thread/session, capacity and disabled-recording checks. Independent
source and raw-evidence reviews found no remaining measurement blocker.
Four targeted clang-tidy runs completed; eight diagnostic-only modernize
suggestions remain, with no new performance warning. Other configurations
and reference-specific static analysis are not claimed covered.

Reproducible artifacts are retained in the bounded-probes iteration:
`draw/analysis/stage-attribution.md`, `draw/analysis/analysis.json`,
`draw-evidence-review.md`, frozen source/binaries and both raw traces.
Whole-trace receipts include 129 reference and 132 candidate incomplete
records outside the required cohorts; no selected frame was omitted.
All receipts have false invalid, overflow and aborted flags.

### Actual AnnounceCommit listener diagnosis, 2026-11-07

The subsequent diagnostic pair completed all 16 native scenarios per
side, with consecutive reference/candidate launches. Both runs contain
469 ordinary Commit scopes, 469 complete listener receipts and 469
processing records, uniquely associated by session, sequence and original
outer endpoints. There are no invalid receipts. The existing signal
implementation and ordinary logger paths were not modified; labeled
registration adapters capture actual synchronous callback entry/exit
into a bounded 64-record array and publish after the original Commit end.

Every captured Commit called eight listeners in this observed order:
VisualTool, VideoBox, VideoDisplay, SubsEditBox, BaseGrid,
FrameSelectionAnchor, FrameTitle and SearchEngineReset. Each side has
3752 invocation records. This is the fixture's runtime inventory, not a
promise that other dialogs or audio listeners can never be instantiated.
Per-AssFile registration identifiers are not global or cross-run IDs.

| Scope | Reference P50 / P95 us | Candidate P50 / P95 us |
|---|---:|---:|
| Outer Commit | 152.7 / 340.2 | 150.4 / 318.9 |
| AnnounceCommit | 90.5 / 236.8 | 89.6 / 232.9 |
| SubsEditBox listener | 31.7 / 72.6 | 31.3 / 82.4 |
| BaseGrid listener | 21.1 / 87.2 | 20.7 / 87.3 |
| FrameTitle listener | 22.3 / 96.8 | 22.9 / 97.2 |
| AnnounceCommit outside listener union | 6.4 / 8.1 | 6.1 / 7.5 |
| Receipt processing | 90.0 / 115.1 | 90.4 / 131.2 |

Each side has 10 synchronous-editor and 459 deferred-editor submissions.
Four synchronous samples per side exceed 1 ms in AnnounceCommit; the
largest synchronous intervals are predominantly SubsEditBox. No deferred
AnnounceCommit exceeds 600 us in either run. Their maxima are 548.9 us
reference and 572.5 us candidate. Therefore the earlier unexplained
deferred 600–1500 us population was not reproduced; these new records
cannot assign that historical population to BaseGrid or another listener,
nor establish that it has been fixed. The current deferred tails do
identify BaseGrid and FrameTitle as substantial measured regions.

Two different candidate outliers must remain visible. Commit sequence 405
(ordinary/receipt/processing lines 42015/42016/42017) took 18.6867 ms.
AnnounceCommitDetails took 18.5463 ms and recorded one 64-entry,
19114-byte FlushLocked interval of 18.4708 ms. Its subsequent
AnnounceCommit was 131.7 us. The flush probe encloses writing, stream
flush, buffer cleanup and bookkeeping; it measures elapsed time and
cannot establish pure disk latency or exclude scheduling. Sequence 235
(receipt line 24779) likewise took 16.6498 ms overall, with a 16.4685 ms
FlushLocked interval inside 16.5299 ms AnnounceCommitDetails. Both full
outliers remain in all distributions without logger subtraction.

Both the interval union and exclusive invocation partitions preserve
all samples. Remaining AnnounceCommit residual contains dispatch and
probe overhead, not a claim of thread inactivity. Logger counters are
associated with stages but lack per-flush endpoints in the receipt;
they cannot be assigned exactly to an individual listener. Diagnostic
publication changes overhead and flush phase, so these values do not
replace the prior uninstrumented acceptance results.

Both applications built. The first test build exposed a new test helper
copying noncopyable JSON elements; moving the owned payload corrected
the helper without weakening assertions. The original failure remains.
The corrected focused suite passed 113 tests; full observed C++ passed
3014, skipped 19 and failed zero. Six new listener tests use real
AssFile callbacks, covering arities/order, connection/self-disconnection,
empty/duplicate registrations, exception/nesting, capacity and disabled
capture. Three targeted clang-tidy runs completed, with three new
diagnostic-only modernize suggestions and no new performance warning.
Unrun registration-only and Gac static-analysis configurations remain
explicitly listed in the verification report.

This bounded phase used exactly four actual application starts:
two Draw and two listener runs. No automatic application retry, merge
or push occurred. Raw traces, failures, frozen binaries/source and
`listener/analysis/listener-analysis.json` remain retained. Strict wx
acceptance, Gac renderer/performance and workspace acceptance are still
open; no further GUI launch is covered by this phase's exhausted budget.

### Final bounded observer correction, 2026-11-07

The subsequent authorized iteration used two additional application starts:
one continuous old-fast reference/candidate pair, with no retries. This was
an observer validation and diagnostic screen, not product acceptance.
Shared GL and the primary candidate's product code were not changed.

The Finish review found no supported remaining reduction. Normal wx success
already performs one required GL error check, with no GL restoration calls,
allocation or error-string construction. The preceding stage measurements
did not isolate material cost in its remaining C++ bookkeeping. Removing,
sampling or moving the check would change the error/timing contract.

The observed sources instead add opt-in
`AEGISUB_COMMIT_TRACE_FLUSH_DEFERRAL=1`. During an actual Commit, the session
defers physical trace flushing for both UI and worker appends. Formatting,
summary updates, locking and event order remain. The bound includes pending
entries and is fixed at 256 entries / 512 KiB; overflow retains its prefix,
reports rejection and fails rather than flushing inside the scope.
Immediate requests are delayed until this opt-in scope finishes.

Original Commit begin/end samples are copied without resampling. Explicit
Finish drains after the original endpoint and before synchronous input
returns. The receipt reports the actual physical-flush counter delta before
ownership release. A separate observed-total interval includes setup through
receipt append, but excludes its own metric emission and lock-release tail.
The unchanged external Send interval encloses those remaining operations;
it excludes the driver's own subsequent duration-record append. Lower inner
Commit time is observer isolation, not a product speedup.

Both applications built from the restored, reviewed observer revision.
Initial PerfTrace coverage passed 35 tests, including eight new cases.
A single buildable counterfactual restored synchronous threshold flushing:
both selected tests failed their actual file-visibility assertions before
Finish. Exact source restoration, rebuild and the same two tests then
passed. The full C++ suite passed 3009 tests, skipped 19 and failed zero.
Three targeted clang-tidy runs found no new modernize/performance warning;
changed-line readability diagnostics remain recorded. The analyzer passed
17 tests after independent review corrected its supplementary completion
validation to match the fixed driver's actual version/interaction rules.

The real pair completed all 16 native scenarios per side. Both traces have
469 ordinary Commit scopes, 469 complete observer receipts and 469 totals,
with exact endpoint association. All in-scope physical-flush deltas and
rejected-entry/byte counts are zero. Boundary drain occurred in 53 reference
and 37 candidate scopes. Peak storage was respectively 89 / 84 entries and
20722 / 19448 bytes. Each side retains all 513 external Send records and
464 supplementary samples. Supplementary native windows associate 448
observer receipts; the remaining 21 per side are retained without inventing
a unique cross-clock per-Send mapping.

| Interval | Reference P50 / P95 ms | Candidate P50 / P95 ms |
|---|---:|---:|
| Original Commit | 0.1383 / 0.3812 | 0.1343 / 0.3543 |
| Observer total through receipt append | 0.1713 / 0.4325 | 0.1656 / 0.4060 |
| Full synchronous Send | 0.5829 / 1.3799 | 0.6043 / 1.7317 |
| Boundary drain, all 469 scopes | 0.0001 / 0.0685 | 0.0001 / 0.0604 |

These are separately measured distributions, not percentile sums or
subtractions. All warmup, measured move, setup, down, final and restore
samples, along with whole-update intervals, remain in the report.
Prior listener-instrumented timings are not a product speed baseline.
Driver-observed measured-move maxima also remain visible: reference/candidate
dispatch reaches 128.1899 / 143.6205 ms and the full observed interval reaches
199.9474 / 154.8220 ms. These intervals include driver work; the measurements
do not assign their tails to product code, disk or scheduling.

| Pose | Frozen Draw/Combined cap ms | Reference Draw P95 ms | Candidate Draw P95 ms | Candidate Combined P95 ms |
|---|---:|---:|---:|---:|
| Neutral | 0.0512 | 0.0662 | 0.0677 | 0.0740 |
| NormalXYZShear | 0.0501 | 0.0639 | 0.0744 | 0.0798 |
| EdgeOn | 0.0534 | 0.0524 | 0.0775 | 0.0840 |
| NearCamera | 0.0491 | 0.0567 | 0.0556 | 0.0595 |

All four candidate Draw and Combined screens fail the unchanged frozen
caps. Frame P95 passes for both sides in all poses; candidate values are
16.6913–16.7290 ms. Three reference Draw values also exceed the frozen
caps, which does not authorize raising them or discarding candidate
failures. The pair's telemetry recorded 44–45 C, P0/P3 and no thermal
slowdown flags or counter changes; it does not establish a CPU/driver
mechanism or identical clock residency.

Evidence is retained in the `aegisub-xy-final-convergence-20261107-001`
iteration: `finish-review.md`, `verification-report.md`,
`observer/analysis.json`, `observer/results.md`, raw traces, source patches, frozen binaries,
the counterfactual failures and application-start ledger. The final
independent runtime review recomputed the raw associations and distributions
without disagreement; its remaining limits are recorded in
`observer-runtime-review.md`. The final
bounded optimization iteration stops here with wx still unaccepted.
Gac renderer/performance, docked/detached workspace and remaining input
coverage are still outstanding. No commit, merge or push occurred.

### 2026-11-08 — current product performance pairs

The performance-first iteration combined the accepted input and fixture repairs
from the other session with this session's unchanged strict gates. The tested
product remained HEAD `322c35b` plus tracked patch
`c4dc1d7213dfe9bd5d7f93410b8e0d212a739fa9bb07261847700112d68e318a`
and the bound required untracked sources. Only this documentation changed in
the product worktree during the iteration. Measurement changes were confined
to isolated sources and external readers. Shared GL, Finish, interaction rules
and product timing endpoints were not changed.

The preregistered envelope allowed ten starts: wx A1/B1/B2/A2 and one
reference/candidate pair for each of three Gac renderer configurations.
Eight actual starts completed, with no retries. The first wx pair failed,
so B2/A2 were not run or reassigned. Gac proceeded independently.

#### wx: valid first-pair strict failure

Both minimal measured roles passed the fixed sixteen-case native driver and
exited normally. The reference retains the old-fast `33a2b455` three-batch
implementation. Candidate Prepare starts before pass construction, Finish
remains inside Draw, and recording drain remains inside Frame.

The first reader returned HOLD because its older helper expected completion
diagnostics and canvas identity inside viewport captures. The current native
protocol publishes completion diagnostics in independent incremental
`visual-state` replies. The external reader was repaired after reproducing
the exact missing-field failure; original captures, traces and HOLD were
preserved. Request/PID/HWND, state sequence, actual capture publication and
typed allocation/provider/document continuity are validated. The existing
completion-matching predicate, recording/drain/cohort checks, quantiles and
gates remain unchanged. Ten new cases plus the previous twenty-nine reader
cases passed; a request-binding counterfactual was detected.

Each side supplied 1,059 state replies. Reference/candidate had 1,512/1,511
native completion records. Actual measured counts were 191/192/192/192 and
192/192/191/191; incidental paints and outliers were retained.

| Pose | Reference Draw P95 ms | Candidate Prepare P95 ms | Candidate Draw P95 ms | Candidate Combined P95 ms | Effective Draw/Combined cap ms | Candidate Frame P95 ms |
|---|---:|---:|---:|---:|---:|---:|
| Neutral | 0.0554 | 0.0012 | 0.0567 | 0.0609 | 0.0512 | 16.7414 |
| NormalXYZShear | 0.0472 | 0.0011 | 0.0579 | 0.0612 | 0.0501 | 16.7047 |
| EdgeOn | 0.0504 | 0.0011 | 0.0566 | 0.0608 | 0.0534 | 16.7319 |
| NearCamera | 0.0524 | 0.0011 | 0.0540 | 0.0586 | 0.0491 | 16.7439 |

Draw and Combined fail all four effective gates; Frame passes all four.
Fresh paired Draw limits are 0.0654/0.0572/0.0604/0.0624 ms. Even without
the retained caps, NormalXYZShear Draw and Combined and EdgeOn Combined
exceed their fresh limits. The two reference poses above retained caps do
not authorize raising caps or discarding candidate results.

Both external-cost readers validate all 513 Send records, four complete
supplementary update sequences, 469 ordinary commits and zero Commit-deferral
receipts. Reference/candidate full Send P95 is 1.3327/1.4043 ms, Commit P95
0.3069/0.3547 ms and override P95 0.0124/0.0130 ms. The candidate's
36.2952 ms maximum Send remains in the evidence. No numeric gate was invented
for these auxiliary costs and no tail was attributed to CPU frequency,
disk or scheduling without evidence. All warmup, stage and whole-update
costs are retained; different clocks are not subtracted. Both runs sampled
44 C with no thermal flags or counter increases.

#### Gac: three renderer pairs pass

Ordinary OpenGL uses the authenticated archived F6 reference. Its effective
gates take the minimum of the fresh pair and both retained historical strict
limits. F6 cannot execute the complete other-renderer fixture, so libplacebo
and forced-fixed use an explicitly identified reconstruction of original
`322c35b` with the same renderer-selectable measurement fixture. They use
their own fresh paired gates; OpenGL thresholds are not transferred.

All six native runs passed the exact nine-case fixture and exited normally:
27 candidate and 27 reference case executions. Each pose has sixteen actual
warmups and ninety-six actual measured draws, with 448 ordered update rows
per run. Native process/image/start/exit, selected loaded modules, actual
backend and fixed-pipeline initialization logs are bound to the executed
source and binary. Module observation occurred before fixed-pose cohorts.

| Renderer | Candidate Draw P95 range ms | Candidate full-render P95 range ms | Draw gates | Full-render gates |
|---|---:|---:|---:|---:|
| OpenGL | 0.0514–0.0550 | 0.4042–0.4611 | 4/4 | 4/4 |
| libplacebo | 0.0373–0.0476 | 0.5265–0.6917 | 4/4 | 4/4 |
| Forced fixed-function | 0.0365–0.0559 | 0.4305–0.5285 | 4/4 | 4/4 |

Pass setup, initialization, drawing and Finish remain charged to Draw.
Full-render is the original OnRendering interval through context, interop,
D2D submission and restoration; it excludes parent Present, GPU completion
and scanout. All twelve candidate update-to-draw P95 values are below their
own reference values. Counts, totals, P50, P95 and maxima remain available;
cached draws were not observed in these windows, rather than assigned an
invented duration. Handler-only timing remains unavailable.

Candidate and reconstructed reference builds passed complete C++ suites:
2,999/2,988 passed respectively, nineteen skipped each, zero failures.
All three affected C++ translation units were checked on both sides.
Six new test-probe initializer warnings were corrected and the candidate
smoke rebuilt; final new modernize/performance warnings are zero. Two
incomplete-submodule configure attempts and one incorrectly prepared test
runtime remain recorded as failures. They were preparation defects, and no
product code or assertion was weakened to conceal them.

The external Gac contract tests passed 19/19, inherited timing tests 30/30,
and launcher tests 11/11. Counterfactuals detected selecting only the looser
historical cap and accepting forced cleanup as a normal exit. Candidate
canonical GL_FLAT and legacy-reference GL_SMOOTH expectations remain
explicit; the native API rejection and following-pixel assertions were
preserved when adapting to the current batch API.

Evidence is retained under `aegisub-xy-perf-final-20261108-001`, including
`wx/result.md`, repaired-reader raw replay, both external-cost reports,
`gac/result.md`, three Gac comparisons, native identities, raw timing and
raster artifacts, independent reviews, source/build releases and the
eight-start ledger. The earlier wx boundary suite and docked/detached
workspace results keep their original version and coverage boundaries;
this performance phase does not claim a fresh complete workspace or
physical-input matrix.

Independent runtime review reconstructed all 2,688 Gac warmup/measured
draw/full pairs and ordered update rows directly from raw trace/TSV, checked
all 48 role/pose Draw and full-render P95 values, and reproduced the
gradient, near-camera isolation and following-primitive pixel assertions.
No blocking discrepancy was found. The original wx data and repaired
reader's failed strict result were independently checked as well.

Overall merge acceptance remains false because wx strict performance still
fails. These Gac passes cannot compensate for it. Neither `exp` nor
`exp-probe-gac` was rewritten, committed, merged or pushed. Related
intermediate changes still require atomic replacement/reorganization after
the remaining performance and integration requirements are satisfied.

### 2026-11-08: final bounded optimization candidate rejected

The sole new proposal was to compose the existing seven local matrix
operations on the CPU inside Draw and submit one matrix. Independent
algebra and cost reviews found it mathematically feasible, but not a
well-supported way to close the remaining strict wx gap. Old fast wx
already uses the same seven transform operations. The retained diagnostic
candidate's entire GL state/matrix region has P95 1.6–2.5 us, including
required identity and near-plane setup. The latest minimal-observer
Combined excess is 7.4–11.1 us. These separate runs and quantiles cannot
be subtracted to estimate an improvement.

CPU composition would still cost time and introduce numerical verification
for large finite angles, near-plane rounding and nonidentity unowned
modelview composition. No measured downstream grid or Finish improvement
supports accepting that extra implementation. The proposal was rejected
before implementation; this does not prove that further optimization is
impossible. The normal wx Finish error check remains required, and static
geometry validation and prepared colours are already active.

This review phase changed no product code or tests and executed no builds,
tests or native GUI starts. It did not activate its conditional launch
budget or create another experimental checkout. Evidence is retained under
`aegisub-xy-one-candidate-20261108-001`: `matrix-review.md`,
`cost-review.md`, `root-evidence-check.json` and `decision.md`.

Performance acceptance is unchanged: wx Draw 0/4, Combined 0/4, Frame 4/4;
Gac Draw 12/12 and full OnRendering 12/12 across the three tested renderers.
The bounded micro-optimization iteration is closed. A further attempt
requires new evidence of removable work or a separately agreed broader
design. Strict gates, measured boundaries and all retained failures remain
unchanged; overall merge acceptance is still false.

### 2026-11-08: validated integration and accepted wx performance exception

The final implementation was reconstructed directly on wx parent
`33a2b4553a3087b4b122a9037f7a555f2a406a20` and Gac parent
`e3bf38baf61e78107d17751dbe2367bc69592af5`. Final Windows x64
RelWithDebInfo builds passed. Complete C++ suites reported wx 2,849 passed
and Gac 2,999 passed, with 19 skipped and no failures in each lane. Final
changed-line modernize/performance checks had no new findings; other
existing diagnostics remain recorded.

The reconstructed wx native integration completed three registered runs
without retry: direct WGL preservation, the original 16-case transform
driver and the 10-case capture/boundary driver. All 200 WGL raster witnesses
were byte-identical to the retained corpus. Transform validation passed
654 checks, including ten exact Undo/Redo/restored file sets; boundary
validation passed 715 checks, seven exact Undo pairs and six late-input
sequences. The positive assertion probe produced its one expected
assertion, and all 15 ordinary boundaries reported none. Source, fixtures,
actual process identity, loaded wx libraries and normal exit were verified.

Gac's earlier three-renderer correctness/performance and real docked /
detached workspace evidence retains its recorded source-dependency scope.
It is not relabeled as a fresh GUI/performance run of these reconstructed
Gac binaries. No physical hardware input or additional platform coverage
is inferred. Integration evidence is retained under
`aegisub-xy-integration-20261108-001`.

The user explicitly accepted the remaining wx performance gap as a merge
exception on 2026-11-08. This supersedes the earlier merge-blocking status,
not the measured failures or the strict thresholds:

| Pose | Draw P95 (ms) | Combined P95 (ms) | Effective cap (ms) |
|---|---:|---:|---:|
| Neutral | 0.0567 | 0.0609 | 0.0512 |
| NormalXYZShear | 0.0579 | 0.0612 | 0.0501 |
| EdgeOn | 0.0566 | 0.0608 | 0.0534 |
| NearCamera | 0.0540 | 0.0586 | 0.0491 |

The retained paired result remains wx Draw 0/4, Combined 0/4 and Frame 4/4;
Gac remains Draw 12/12 and full OnRendering 12/12. No sample was excluded,
measurement boundary moved or threshold increased. The authorized next
step is validated atomic local-history replacement; publication is not
part of this acceptance.
