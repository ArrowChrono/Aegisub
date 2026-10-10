# Native wx Shift Times overflow E2E

Run from the repository root with a freshly built wx executable and a **new**
artifact directory:

```powershell
dotnet run --file tests/gui-automation/shift-overflow-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/shift-overflow-native
```

The .NET 10 Windows file-based driver reuses `PrivateDesktop`,
`ObservedWindowsCommand`, `AutomationProtocol` and the established native file
picker and wx TaskDialog interaction. It uses an inactive private desktop without
switching the input desktop, global input, clipboard access, injected application
state or production test-only controls. Run exclusively from other GUI scenarios
and heavy builds. Its watchdog is 180 seconds inside the shared supervisor's
210-second limit. Native calls have one-second timeouts, ordinary waits ten
seconds, startup readiness twenty seconds and normal exit ten seconds. Expiry or
forced termination is failure, never accepted as normal exit.

## Fixture and actual interaction

The Shift Times contract is transactional: the complete operation must have
representable signed-int shifted frame numbers and output times before any row
is published or history is written. `CheckedFrameAtTime` and
`CheckedTimeAtFrame` use wide intermediate arithmetic and preserve the existing
v1 extrapolation, signed truncation and START/END midpoint formulas; their final
time/frame result must fit `int`, while the old conversion APIs remain unchanged.
For valid representable negative time results, assigning `agi::Time` still
clamps to zero. This UI scenario tests the application contract rather than
claiming exhaustive coverage of every checked conversion formula.

The opt-in APIs accept `int64_t` input and return `int`; a final result outside
that range throws `std::overflow_error`. Their internal fixed-width `int256_t`
arithmetic also covers full-width rational numerator/denominator products. Only
the completed conversion is narrowed: an out-of-range exact neighbor may still
produce a valid START/END midpoint. `CheckedTimeAtFrame` retains the unloaded
framerate's zero sentinel. Shift Times additionally requires its offset frame
number to fit `int` before converting it back to a timestamp.

The generated ASS has six event rows, including an interleaved Comment, three
styles, Unicode, tags, commas, distinct layers, actors, margins and effects, and
metadata sentinels. Its input deliberately contains raw millisecond timestamps
which are not all exact centiseconds. The final row is near signed 32-bit time's
upper boundary, with raw End `596:31:22.637` (2,147,482,637 ms).

The actual **Video -> Open Timecodes File...** picker loads a generated v1 file
containing `Assume 25`. No video decoder, enormous video or unusual framerate is
needed. The observed Open picker is the native common-dialog layout: the filename
Edit is the child of the filename `ComboBoxEx32` (both observed control ID 1148),
next to the actual filename label (1090). Parent/child ownership and their row
bounds are checked, the entered path is read back, and the actual enabled Open
button (1) is used. This is not the modern Save picker's UIA Edit ID 1001; the
first run exposed that distinction and its failure artifacts remain available.
The actual **Subtitle -> Select Lines...** dialog selects actors
containing `pick-`, including both dialogues and comments: rows **2–3 and 5**.
Those are the original selection blocks throughout the scenario, not a list of
all rows affected by the chosen scope.

A native Save As establishes the complete writer-normalized baseline. The six
baseline event lines must match the independent `SeedEvents` literals before
any scenario proceeds. Exact main-editor mode reads raw timestamps, separately
from ASS's centisecond serialization. The active selected line is identified by
its unique complete text sentinel; its expected Start and End are independent
literal values, not recalculated from production output.

| Phase | Native input and scope | Required result |
| --- | --- | --- |
| Zero time | `0:00:00.00`, all rows, both endpoints | Close, no dirty state/commit, unchanged full ASS and history bytes |
| Late overflow | `0:00:02.00` forward, all rows, both endpoints | Error at the final row's End; no earlier row or the final Start may be published |
| Cancel after error | Dismiss error, then Cancel in the retained dialog | Complete ASS and history unchanged, clean document, raw selected endpoints unchanged |
| Large time | `596:31:23.00` forward, all rows, both endpoints | Exact range error; first row is representable but a later row is not |
| Time retry | Correct that same dialog to `0:00:00.10` backward, all rows, Start only | Success; negative first Start clamps to zero; Comment participates; End retains its original raw milliseconds |
| Large frames | `60000000` forward, selected rows, both endpoints | Exact range error using ordinary 25 fps; no subtitle/history publication |
| Frame retry | Correct that same dialog to `1`, selected rows, both endpoints | Success; only rows 2–3 and 5 change, with their different START/END frame boundaries |
| Zero frames | `0`, selection onward, End only | Still maps/commits; row 6 End changes from serialized `596:31:22.64` to `596:31:22.62`; rows before first selection stay unchanged |

Each rejection locates a real native `#32770` window whose actual `GW_OWNER` is
the exact existing Shift Times HWND. That dialog's owner must be the owned main
window, and every target is verified against the owned live process and private
desktop. This matters because the error popup and original dialog both have the
title **Shift Times**.

The actual TaskDialog UIA Text must exactly equal:

> The shifted time is outside the supported range. No subtitles were changed.

The unique native UIA OK button must be an actual popup child in the owned
process. The only-OK wxMSW TaskDialog must expose `CommandButton_2`; its observed
logical ID is sent using `TDM_CLICK_BUTTON`. This follows wx 3.3.1's only-wxOK
IDCANCEL mapping (`src/msw/msgdlg.cpp:725–739`, mapped back at `540–547`), as in
the existing native timing-calculation driver. It does not guess a native child
ID, try fallback button IDs or invoke global input. The driver then verifies
that the original dialog HWND, amount text, every radio choice and history-list
count remain intact, the dialog is enabled again, and the document is still clean.

## Independent oracles

The expected event arrays contain **complete literal ASS lines** for each
accepted operation, not timestamp calculations or parsed-time comparisons.
Every non-event line is retained from the verified native baseline, so the
comparison covers the entire saved file including all headers, styles, project
properties, unrelated event fields and original file order. The expected full
ASS is written beside each actual file for inspection. Nothing is filtered out.

The key expected serialized times are:

| Row | Baseline Start–End | After -100 ms Start only | After selected +1 frame | After onward 0 frames End only |
| --- | --- | --- | --- | --- |
| 1 | `0.05–0.50` | `0.00–0.50` | `0.00–0.50` | `0.00–0.50` |
| 2 Comment | `10.01–10.51` | `9.91–10.51` | `9.94–10.54` | `9.94–10.54` |
| 3 | `20.00–22.57` | `19.90–22.57` | `19.94–22.62` | `19.94–22.62` |
| 4 gap | `30.01–32.78` | `29.91–32.78` | `29.91–32.78` | `29.91–32.78` |
| 5 | `40.01–42.90` | `39.91–42.90` | `39.94–42.94` | `39.94–42.94` |
| 6 late | `596:31:20.01–596:31:22.64` | `596:31:19.91–596:31:22.64` | `596:31:19.91–596:31:22.64` | `596:31:19.91–596:31:22.62` |

At 25 fps, each frame is 40 ms and frame-boundary midpoint timestamps are 20 ms
away from exact frame timestamps. For example, selected Comment Start 9910 ms
belongs to START frame 248; shifting to frame 249 gives 9940 ms. Its raw End
10509 ms is read through the existing centisecond conversion as 10510 ms;
END frame 262 plus one gives midpoint 10540 ms. Zero frames still applies that
mapping: the final raw End 2147482637 ms reads as 2147482640 ms, whose END midpoint
is 2147482620 ms. These are hand-derived fixed oracle values.

The main editor's **Exact** radio also verifies the active selected line's raw
End (`10.509`, `22.568` or `42.903`) survives the Start-only operation, while its
Start becomes the independently specified `9.910`, `19.900` or `39.910`. The same
read before and after rejected/canceled work checks that failure did not silently
round those raw values. ASS-only comparison would not detect this distinction.

Whole history arrays are compared structurally against complete independent
JSON literals, including every record and field; failed operations additionally
require byte-for-byte preservation. The old sentinel record remains last. Every
new record must retain `[{"start":2,"end":3},{"start":5,"end":5}]` even for all
rows and selection onward, and record the actual canonical amount, direction,
endpoint type, scope and current subtitle basename. Zero time adds no record;
zero frames adds a `"0"` record. After the final save, main-window-local WM_CLOSE
must exit normally within ten seconds with native exit code zero. History is
checked once more after that exit.

## Artifacts and coverage boundary

A fresh run retains `result.json`, `driver.log`, `actions.ndjson`, generated
fixture/profile/timecodes, launch readiness and process/desktop identity evidence,
all complete actual and expected ASS files, whole actual and expected history
JSON, native control inventories, each error popup's actual UIA inventory and
owner/button dismissal evidence. Current PNGs show selection, zero inputs,
oversized inputs, actual errors, retained dialogs, corrected inputs, exact raw
editor values and the final saved main window. No source/executable hash manifest
is generated.

Acceptance requires the newly generated `passed: true`, all complete ASS/history
comparisons, exact error ownership/text and retained-input checks, and normal
zero-code exit. Merely creating or compiling this driver, old artifacts and
forced cleanup are not acceptance. Native layout/identification failures must be
fixed using their inventories and screenshots, never weakened assertions,
unbounded retries or deleted cases.

This scenario covers the real wx Shift Times path at ordinary CFR 25 fps. It does
not claim arbitrary VFR mathematics, every numeric boundary, history double-click
restoration, history I/O failures, every raw endpoint in every row through Exact
UI, video playback, or other subtitle formats. Existing targeted C++ tests and
the separate native shift-history scenario cover their respective contracts;
their existence does not imply they ran with this scenario.
