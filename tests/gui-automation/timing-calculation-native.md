# Native wx timing calculation E2E

Run from the repository root with the built wx application and a **new**
artifact subdirectory:

```powershell
dotnet run --file tests/gui-automation/timing-calculation-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/timing-calculation-native
```

This file-based .NET 10 Windows scenario reuses `PrivateDesktop`,
`ObservedWindowsCommand`, `AutomationProtocol` and the established
`timing-preferences-native.cs` native-dialog identification. It runs the real
wx host on an inactive private desktop without switching the input desktop,
global input or clipboard use. Run exclusively from other GUI tests and heavy
builds. The driver watchdog is 180 seconds within the helper supervisor's
210-second limit; normal GUI waits are ten seconds, startup readiness twenty
seconds, native requests one second, and normal exit ten seconds. Expiration
or forced cleanup is failure, not normal shutdown.

## Interaction and independent oracle

The generated fixture has ten Dialogue rows across Default, Alternate and Third
styles plus an interleaved Comment. File order is intentionally not chronological.
It includes distinct actors, layers, margins, effects, Unicode, override tags,
comma-containing text and Script Info metadata sentinels. A native File Save As
first establishes writer-normalized headers, styles and project properties.
Every baseline event must match the full `SeedEvents` literal in original file
order before any processing can pass.

The driver observes the actual **Timing -> Timing Post-Processor...** native
menu command. It identifies all eight native Edit controls from their class,
neighboring named label and row bounds rather than enumeration order or numeric
values. The two Enable checkboxes are distinguished by Button class and their
enclosing named group bounds. These are ordinary Edit controls, not the subtitle
grid's TimeEdit controls. Input goes to the exact owned control HWND.

First it reads the initial values (110/120ms leads, 30/40ms gap/overlap, bias
37%, adjacency disabled), edits the available controls, changes lead-in and
selection-only, captures the actual dialog and presses its Cancel button. The
entire subsequent saved ASS must equal the baseline and the document must not
be dirty. Reopening must recover the initial values. The native None button
must disable OK and All must restore it, exercising style eligibility without
injecting checklist state. All three styles remain selected for processing;
selection-only is unchecked.

Before the final valid apply, it enters the target valid values below, then uses
`Native.SetText` on the enabled lead-in Edit to enter **not-an-integer**. It does
not force focus into that Edit or deliberately cause wx focus-loss normalization.
It verifies the actual text and posts the real OK command. A visible native
`#32770` popup must belong to the same owned process and private desktop, and its
observed `GW_OWNER` must be the still-open Timing Post-Processor HWND. The
processor's observed owner must in turn be the main window. Thus the expected
Invalid timing title is never sufficient to select an unrelated process/window.

The popup must contain native DirectUI content, and its **actual UIA Text** must
exactly equal **Timing values must be nonnegative integers. No subtitles were
changed.** The unique actual UIA Button named **OK** must have a nonzero native HWND that
`IsChild` proves lies inside this popup, and the owned host PID. The popup must
expose exactly one native UIA command button. Its observed AutomationId must be
**CommandButton_2**, and the parsed logical ID must equal **2**. This is specific
to the wxMSW only-OK TaskDialog protocol used by this `wxOK | wxICON_ERROR`
notification: wx 3.3.1 `src/msw/msgdlg.cpp:725-739` adds the custom OK button as
`IDCANCEL` with `TDCBF_CANCEL_BUTTON` when wxCANCEL is absent; lines 540-547 map
the returned IDCANCEL to wxID_OK. The actual UIA evidence independently exposes
that custom command as CommandButton_2. The driver sends `TDM_CLICK_BUTTON` with
this verified logical ID; it does not guess native HWND IDs, try a 1/2 fallback,
or use global input or focus changes. `InvokePattern` support is recorded but
not used because an inactive TaskDialog can reject Invoke despite supporting
the pattern. The processor must remain open, re-enabled, and retain
**not-an-integer** verbatim in the original Edit. The
actual document must remain clean. Cancel and a native Save As must preserve the
entire baseline ASS. Reopening must recover all eight initial values, bias and
applicable booleans, proving the rejected transfer did not save partially read
preferences. The original final valid pipeline then runs without dropping any
literal event case.

Next it enters lead-in **100ms**, lead-out **200ms**, maximum gap **200ms**,
maximum overlap **600ms**, bias **25%**, and enables both leads and adjacency.
After capturing the actual values it presses the native OK button, verifies the
document becomes dirty and saves through the actual File Save As dialog.

| Actor | Seed Start-End | Literal expected Start-End | Distinguishing behavior |
| --- | --- | --- | --- |
| zero | 0.05-0.50 | 0.00-0.75 | Negative lead-in clamps to zero; boundary gap joins A |
| a-clamp | 1.00-2.00 | 0.75-2.05 | Joins zero; lead-out stops at updated follower start |
| b-clamp | 2.15-3.00 | 2.05-3.20 | Lead-in leaves 50ms gap, then predecessor fills it |
| c-gap | 10.00-11.00 | 9.90-11.23 | Positive 100ms post-lead gap joins at 25% |
| d-gap | 11.40-12.00 | 11.23-12.20 | Same joined boundary, serialized rounding |
| e-overlap | 20.00-21.00 | 19.90-21.08 | Already colliding lines can expand |
| f-overlap | 20.80-22.00 | 21.08-22.20 | Negative 500ms post-lead overlap joins at 25% |
| g-tie | 30.00-32.00 | 29.90-32.20 | Equal starts and ends, oversized overlap stays |
| h-tie | 30.00-32.00 | 29.90-32.20 | Tie result independent of sort order |
| i-far | 40.00-41.00 | 39.90-41.20 | Large separation does not join |
| comment | 1.90-2.90 | 1.90-2.90 | Ignored even where it intersects processed lines |

The zero/A post-lead gap is exactly 200ms, equal to the inclusive maximum gap.
Their joined boundary is 700 + int(200 * 0.25 + 0.5) = 750ms. Consequently zero's
end and A's start are both 0.75s; 0.70s/0.90s are only the intermediate lead
results, not the combined-processing oracle. This adjacency does not change
A's 2.05s end or the touching A/B pair.

The complete sorted adjacency chain after the lead passes is checked below.
G/H have identical times, so either tie order produces the same result. Later
pairs use the preceding joins' updated values.

| Pair | Raw distance (ms) | Adjacency decision |
| --- | ---: | --- |
| zero/A | 200 | Join at 750ms; inclusive gap boundary |
| A/B | 0 | Already touching; no adjustment |
| B/C | 6700 | Greater than 200ms gap |
| C/D | 100 | Join at 11225ms |
| D/E | 7700 | Greater than 200ms gap |
| E/F | -500 | Join at 21076ms |
| F/first tie | 7700 | Greater than 200ms gap |
| first tie/second tie | -2300 | Greater than 600ms overlap |
| second tie/I | 7700 | Greater than 200ms gap |

The C/D common raw boundary is 11225ms and ASS serialization rounds it to
11.23s. The E/F boundary is 21076ms, including the existing signed truncation
behavior for negative distance, and serializes to 21.08s. The A/B limit reads
the follower's already updated 2050ms start, not its original start or the
predecessor's 2000ms end. These values are independently hand-derived, not generated by production timing
functions. The first baseline run exposed a missed zero/A adjacency in the
original hand calculation; its failure artifacts are retained. The corrected
literal accounts for the full sorted adjacency chain above without relaxing
assertions or removing a scenario.

`ExpectedEvents` stores **complete literal ASS event lines**, not computed or
parsed timestamp expectations. The full expected file uses those events in
original file order and every non-event baseline line unchanged. Exact full-line
comparison checks all headers, styles, project properties and event fields;
there is no metadata filtering, timestamp-calculation oracle or hash assertion.
Finally a focused PNG captures the saved main window and main-window-local
WM_CLOSE must finish normally with native exit code zero before success.

## Coverage boundary

No video, timecodes or keyframes are loaded. The keyframe checkbox must be
disabled and unchecked, and all four keyframe numeric controls disabled. They
are read but never enabled or injected. This scenario covers wx lead safety,
zero clamping, original row mapping, collision handling and signed adjacency
through the actual native dialog, **not** keyframe snapping or selection-only
application. The latter is only toggled during Cancel.

Available keyframes are covered separately by the GacHTTP scenario at
`tests/smoke/run-gacui-timing-processor-http-smoke.ps1` and fixture
`tests/smoke/gacui_timing_processor_http_smoke.cpp`, target
`gacui-timing-processor-http-smoke` / CTest
`gacui_timing_processor_http_smoke`, together with the relevant core mathematics
tests. Those Gac paths belong to the Gac probe checkout and are not prerequisites
of this standalone wx driver. This document does not claim those tests or this
scenario have been run merely because the harness exists or compiles.

## Artifacts and acceptance

A fresh run writes `result.json` with every check and final status, `driver.log`,
`actions.ndjson`, actual dialog/control inventories and helper process/desktop
identity records. Seven real window PNGs show initial dialog, canceled edits,
invalid Edit input, the actual Invalid timing TaskDialog, processor state after
error dismissal, values before valid OK and saved main window. The invalid case
also retains the native window/control inventory, observed owner-chain handles
and verified process identity in `invalid-timing-owner.json`, and actual UIA
text/control/automation-ID evidence in `invalid-timing-accessibility.json`.
`invalid-timing-dismissal.json` records the actual UIA AutomationId, native HWND ID,
OK HWND/process ID, actual popup-child relationship, owner-chain handles and
observed InvokePattern support, verified logical command ID, selected dismissal
message and wxMSW source protocol used for dismissal.
`baseline.ass`, `after-cancel.ass`, `after-invalid-cancel.ass`,
`after-ok.ass` and `expected.ass` make the complete oracle inspectable. The
fixture, isolated profile, initial preference snapshot and launch ready artifact
remain available to reproduce the run. No hash manifest is generated.

Acceptance requires a newly generated `passed: true`, exact complete ASS
comparison, verified native values, exact owner-bound invalid-input rejection with unchanged
ASS/preferences, and a bounded zero-code normal exit. Inspect
inventories and focused screenshots on layout failure; do not weaken matching,
retry indefinitely or drop cases. Compilation, old artifacts and forced cleanup
are not substitutes for real behavior verification.
