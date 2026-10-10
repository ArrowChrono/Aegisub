# Native wx timing preferences E2E

Run from the repository root with the built wx application and a **new**
artifact subdirectory:

```powershell
dotnet run --file tests/gui-automation/timing-preferences-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/timing-preferences-native
```

The file-based .NET 10 Windows driver reuses `PrivateDesktop`,
`ObservedWindowsCommand`, and `AutomationProtocol`. It runs the actual wx host
on an inactive private desktop without switching the input desktop, global input
or clipboard use. The driver has a 180-second watchdog within the helper's
210-second supervisor limit. Individual native requests have a one-second
timeout, normal GUI waits ten seconds, and host startup readiness twenty seconds.
Run it exclusively from other GUI tests and heavy builds.

## Scenario

The generated isolated profile starts with these preferences:

| Field | Initial | After OK |
| --- | ---: | ---: |
| Lead IN | 110 | 120 |
| Lead OUT | 120 | 230 |
| Adjacent Gap | 30 | 34 |
| Adjacent Overlap | 40 | 56 |
| Key Start Before | 50 | 50 |
| Key Start After | 60 | 60 |
| Key End Before | 70 | 70 |
| Key End After | 80 | 80 |
| Adjacent Bias | 0.37 | 0.63 |
| Enable Lead IN/OUT | true/true | true/true |
| Enable Adjacent | false | true |
| Enable Keyframe (stored) | true | true |
| Only Selection | false | false |

The harness opens **Timing → Timing Post-Processor...** through the observed
native menu. Each of the eight numeric controls is identified from its Edit
class, row bounds and neighboring named label, not enumeration order or its
initial numeric value. The two identically displayed Enable checkboxes are
distinguished by Button class and enclosing labeled group bounds.

First, it reads all native values, edits the enabled controls, toggles lead-in
and selection-only, and presses the actual Cancel button. A native Save As must
preserve the entire loaded document. Reopening the actual dialog must restore
all initial values, not the canceled values. Next, it sets the target enabled
numeric fields, bias and booleans and presses the actual OK button.

No video, timecodes or keyframes are loaded. The keyframe checkbox must therefore
be disabled and unchecked, and its four numeric controls disabled. The harness
only **reads** these unavailable controls: it never enables them or injects text
into them. The profile's true keyframe preference and all four initial threshold
values must survive OK, normal shutdown and restart. This wx scenario does not
cover editing keyframe thresholds when keyframes are available.

The fixture contains four dialogues spanning three styles, plus an interleaved
comment, Unicode, override tags, comma-containing text, distinct actors, layers,
margins and effects, and Script Info sentinels. Every dialogue is separated by
several seconds, so enabling adjacency with these small thresholds cannot join
any pair. All styles stay selected. The independent oracle adjusts each saved
Dialogue start by -12 centiseconds and end by +23 centiseconds using integer
ASS timestamp arithmetic; it does not invoke the production timing algorithm or
perform floating-point timestamp rounding. The Comment and every non-time
field must remain unchanged.

A baseline native Save As accounts for the normal ASS writer's initial
serialization. Complete saved ASS lines are compared, including headers,
styles, project metadata and every event field, without filtering metadata or
replacing the assertion with hashes.

The host then receives a main-window-local WM_CLOSE. Exit must complete within
ten seconds with code zero **before** reading the flushed profile.
The exit code is read through `PrivateDesktop.ExitCode`, which queries the
native process handle retained by the helper's CreateProcess ownership rather
than a `Process.GetProcessById` object's managed `ExitCode` property. All eight
numeric fields, bias and five booleans are checked. The same profile is reopened
in a fresh actual host process; the dialog must read back the saved values and
retain the unavailable-keyframe UI state. Cancel and a second normal WM_CLOSE
must preserve the complete stored preference set. A watchdog or cleanup kill is
always failure, never evidence that shutdown flushed preferences.

## Artifacts and acceptance

`result.json` records each actual check and final success or failure;
`driver.log` and `actions.ndjson` record the interaction chain. Native control
inventories include class, label, bounds, enabled state and observed control IDs.
Four focused PNGs show initial, canceled edits, target values before OK and the
restarted dialog. `baseline.ass`, `after-cancel.ass`, `after-ok.ass` and
`expected.ass` preserve the full-file oracle. Initial and post-exit preference
snapshots and the isolated profile's full `config.json` show stored values;
each launch also retains its ready artifact and helper process/desktop identity
evidence. No executable/source/file hash manifest is generated.

Successful acceptance requires a newly run `passed: true` result, exact full-ASS
comparison and both bounded normal exits. Build success or stale artifacts are
not a substitute for running this scenario. Inspect the native inventories and
focused screenshots if a changed wx layout makes control matching fail; do not
weaken the matching or hide a failure with retries.
