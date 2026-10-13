# Static shear GUI end-to-end check

Run from the repository root on Windows with the existing .NET 10 runtime. The
executable must include GUI automation host support and the Shear main visual
tool. The default mode runs in the background on a private desktop, with an
isolated profile and verified host/driver identities. It dispatches the tool
commands corresponding to the arrow and reset operations through the existing
GUI menu dispatcher, and sends mouse events to the owned canvas. It leaves the
input desktop, cursor and foreground focus unchanged. It reuses the shared GUI
driver project and does not install packages.

Add `--real-input` to verify physical keyboard routing and capture the OpenGL
canvas from the actual screen. This explicit mode briefly displays the private
desktop during keyboard inputs and captures. Each lease restores the original
input desktop, cursor and focus in `finally`; the driver watchdog and launcher
provide restoration on failure or timeout. Reserve the desktop while running
this mode. Both modes execute the same 39 behavior cases and subtitle assertions.

```powershell
New-Item -ItemType Directory -Force build-dir/artifacts/shear-tool | Out-Null
ffmpeg -hide_banner -loglevel error -f lavfi -i color=c=0x30343b:s=640x480:r=24 -t 12 -pix_fmt yuv420p build-dir/artifacts/shear-tool/video.mp4
dotnet run --file tests/gui-automation/shear-static-uia.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/shear-tool/video.mp4 build-dir/artifacts/shear-tool/run3
if ($LASTEXITCODE -ne 0) { throw "Shear GUI scenario failed" }
$result = Get-Content -Raw build-dir/artifacts/shear-tool/run3/result.json | ConvertFrom-Json
$validation = Get-Content -Raw build-dir/artifacts/shear-tool/run3/report-validation.json | ConvertFrom-Json
if (-not $result.passed -or -not $validation.passed -or $result.completed_case_count -ne 39 -or $validation.parsed_case_count -ne 39 -or $validation.parsed_configuration_count -ne 4) { throw "Shear GUI evidence is incomplete" }
if ($result.input_method -ne 'command-dispatch' -or $result.physical_keyboard_verified) { throw "Expected the background command-dispatch mode" }
```

The optional real-input command uses its own fresh artifact directory:

```powershell
dotnet run --file tests/gui-automation/shear-static-uia.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/shear-tool/video.mp4 build-dir/artifacts/shear-tool/run-real-input --real-input
```

Use a new run directory for every execution. The private-desktop helper rejects
an existing directory with files, so previous evidence cannot be mistaken for a
new result. The whole scenario has a 180-second watchdog; its launcher has a
210-second deadline. Each host startup, wait, native message and shutdown has a
finite deadline. Timeout or any failed assertion returns a nonzero exit code.

The isolated configuration uses libass, the OpenGL renderer, disabled Skia
visual tools, a 640×480 canvas at 100% zoom, and visible visual-tool controls.
Its initial `hotkey.json` is the repository default with `Visual Shear` removed,
which matches the legacy default at revision `4d41f1d56`. The Video context adds
custom next-frame keys `Alt-Right`/`Ctrl-Alt-Right` and zoom-in keys
`Ctrl-Alt-Up`/`Up`, in the canonical order used by hotkey persistence. The legacy
profile marks all previous migrations complete and lacks only
`visual_tool_shear`, so unrelated historical migrations cannot alter the Video
fixture during the Shear upgrade. `legacy-config-input.json` and the fixture
metadata preserve that prior migration list. The fixture uses no personal
configuration.
Each host loads a fixture with two styles and two events. The first event is
active; the second has different metadata, tags and multiline text and remains
unselected.

The active static text event includes two visible lines, an additional override
block, `pos`, `org`, both scale tags, all three rotation tags, nonzero `fax` and
`fay`, a rectangular clip, colours, alpha, border and shadow. A second static
fixture replaces the visible text with a drawing containing line and cubic
segments plus a caption. Two further fixtures contain animated `fax` or `fay`
inside `t`, together with an unrelated animated colour or border.

Arrow and reset case names describe the requested operation. Default mode
verifies the corresponding GUI tool command; `--real-input` additionally
verifies the actual keyboard hook and hotkey route. Default mode reports
`input_method=command-dispatch` and `physical_keyboard_verified=false`; a
successful real-input run reports `physical_keyboard_verified=true`.

| Requirement | Evidence |
| --- | --- |
| Main toolbar order is clip → vector clip → shear → measure | `main-tool-order` observes the native button indices in one toolbar; `main-toolbar.json` preserves their names, command IDs and bounds. |
| Right/left edit only `fax` by ±0.01 | `static-text-arrow-right`, `static-text-arrow-left` each assert the exact numeric delta and complete single-axis preservation. |
| Down/up edit only `fay` by ±0.01 | `static-text-arrow-down`, `static-text-arrow-up` each assert the exact numeric delta and complete single-axis preservation. |
| Alt + each arrow uses ±0.1 | `static-text-alt-arrow-right`, `static-text-alt-arrow-left`, `static-text-alt-arrow-down`, `static-text-alt-arrow-up`. |
| Ctrl + Home resets only `fax`; Ctrl + End resets only `fay` | `static-text-reset-x`, `static-text-reset-y` start from 0.25 and −0.1 and require zero on the edited axis. |
| Each drag locks one axis, ignoring the diagonal component | `static-text-drag-x`, `static-text-drag-y`, `static-drawing-drag-x`, `static-drawing-drag-y`. |
| Every static edit preserves the other shear axis, all other tags, text, event metadata, both styles and every unselected event | Every static case has separate `axis-value`, `single-axis-preservation`, `other-axis`, `unselected-events` and `styles` assertions. Only the edited tag is removed for the full active-event comparison. |
| Save, undo and redo preserve exact event contents | Each static case saves its result, undoes once to the complete prior events, redoes once to the complete edited events, and undoes again before the next independent operation. Its `undo`, `redo` and `baseline` assertions compare every event and style. |
| Reset and a following nudge are separate undo entries | `static-text-reset-nudge-undo-boundary` resets `fax`, waits at least 600 ms, then nudges right. The first undo must retain `fax=0`; the second must restore the full baseline. Two redos must separately restore the reset and nudge. |
| A transform containing either animated shear axis refuses edits | `reject-animated-fax-*` and `reject-animated-fay-*` each execute all 12 inputs and independently assert clean state, unchanged axis value, and exact preservation of every event and style. |
| An old saved hotkey configuration acquires the new context | `legacy-hotkeys-installed` requires exactly the ten normal, Alt-arrow and reset bindings. `legacy-hotkeys-video-custom-preserved` compares the entire customized Video context. `legacy-hotkeys-marker-persisted` requires one `visual_tool_shear` marker after clean shutdown. |
| Restart preserves the upgrade without duplication | `legacy-hotkeys-restart-idempotent` compares the complete Shear context, customized Video context and migration list after the second existing host startup. |
| An existing custom Shear layout survives first migration | After all 39 behavior cases, `existing-shear-custom-layout-preserved` starts with only `video/tool/nudge/left/large=Shift-Left`, without the Shear or Alt-large migration markers, but with the earlier nudge marker. It requires exactly that one key to survive; the missing nine defaults are not added. Marker and Video preservation have separate assertions. |
| User unbinding survives a later startup | `existing-shear-unbound-preserved` removes the entire Shear context after migration and requires it to stay absent on startup. The complete custom Video context and migration list must remain unchanged. |

These four configuration checks reuse the first two behavior hosts and add two
short startup/shutdown checks after the behavior scenarios. All 39 behavior
cases run before custom or unbound Shear layouts are installed. Each check saves
the actual `hotkey.json` and `config.json` into its artifact directory, and the
report parser rereads the exported contexts and migration list. The custom and
unbound input fixtures are also retained. The hotkey map cannot distinguish an
explicitly empty context from an absent context before first migration; that
case is outside these checks. Configuration persistence checks verify the
upgrade data, and do not establish physical keyboard routing under that data.

The source position is `(320,280)`. At the initial coefficients `fax=0.25` and
`fay=-0.1`, the coefficient controls are at canvas positions `(340,180)` and
`(420,272)`. The X drag ends at `(372,202)` and must add 0.4 to `fax`; the Y drag
ends at `(443,296)` and must add 0.3 to `fay`. Their diagonal components also
exercise the axis lock. Every operation begins from the same source values.

In `--real-input` mode, keyboard input uses `SendInput` after the verified private desktop becomes the
input desktop and the owned canvas receives foreground focus. The driver
attaches its input queue to the owned GUI thread to observe keyboard state. It
requires that the GUI thread consume both the key/modifier press and release
before restoring the original desktop, including for rejected edits. The
unchanged subtitle assertions independently verify the resulting operation.
This exercises the wxWidgets keyboard hook and visual-tool hotkey route.
`keyboard-evidence.ndjson` records each consumed input; it does not infer
processing from `SendInput` merely returning success. The Windows API describes
attached threads as sharing keyboard state in its
[GetKeyboardState documentation](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getkeyboardstate).

Default screenshots use `PrintWindow` in the background and preserve the GUI
control state. These bitmaps do not establish OpenGL canvas or overlay rendering
coverage. `command-input-evidence.ndjson` records the actual dispatched command
IDs; it does not claim that physical arrow keys were pressed.

Real-input screenshots use real screen capture while the private desktop is displayed,
after repainting the canvas and waiting for desktop composition. This includes
the OpenGL canvas and overlay. `PrintWindow` is not used for these snapshots,
because its window bitmap does not provide reliable OpenGL rendering evidence.
Screenshot dimension and visible-pixel validation verifies the saved artifact;
the actual images remain available for visual review of the controls and text.
In real-input mode, `input-desktop-evidence.ndjson` records activation and restoration for each lease
and the final watchdog/launcher paths. The launcher identity records whether
any desktop switch occurred and whether the original input desktop restored.

`result.json` records all 39 stable case names, the four configuration snapshots,
every assertion, expected and
actual coefficient deltas, and before/after events. Static cases also include
undo, redo and restored snapshots. Each snapshot references a saved ASS export
and a validated window PNG. The driver reparses the report, rejects missing or
duplicate cases, rereads every exported event/style, and checks the referenced
PNG dimensions and nonblack pixel counts before writing
`report-validation.json` and exiting successfully. `driver.log`, fixture inputs,
host readiness records and desktop identity evidence remain available for
inspection. An unsuccessful run records the completed portion and failure
diagnostic in `result.json`, with a failure screenshot when capture is possible.

Appending `--inspect` runs only tool selection, toolbar order and a control
screenshot. It writes `probe.json`, `tool-probe-uia.json` and `tool-probe.png`,
then exits. It does not create a full behavior report or establish the 39-case
scenario result.
