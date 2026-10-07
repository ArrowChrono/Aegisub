# Native wx karaoke Start boundary E2E

Run from the repository root with the built wx application, using a **new**
artifact directory for each invocation:

```powershell
dotnet run --file tests/gui-automation/karaoke-start-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/karaoke-start-rejected rejected
dotnet run --file tests/gui-automation/karaoke-start-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/karaoke-start-restart restart
dotnet run --file tests/gui-automation/karaoke-start-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/karaoke-start-no-pair no-pair
dotnet run --file tests/gui-automation/karaoke-start-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/karaoke-start-empty-file empty-file
```

The existing `PrivateDesktop` driver owns the host process and runs on a private,
inactive WinSta0 desktop. It never switches the input desktop, sends global
input, or uses the clipboard. Each invocation has a 180-second watchdog inside
the supervisor's 210-second process deadline; individual GUI waits are bounded.
Coordinate GUI and heavy-build runs with other sessions instead of extending
timeouts or retrying failed scenarios.

The harness opens the actual **Timing → Kanji Timer...** default menu command.
It observes the native read-only style dropdowns, chooses existing styles with
control-local Home/Down key messages, unchecks the actual interpolation checkbox,
and uses the actual Start, Link, Accept Line, warning OK, and Close controls.
It never invokes dialog internals, injects staged rows, or assigns matching state.
The three explicit Link decisions per pair consume three source syllables and
three destination graphemes; Accept Line stages the result without committing it.

## Scenarios and contracts

- **`rejected`:** Start with no initial style selection must show the existing
  missing-style warning. Close and a native Save As must preserve the entire ASS.
  Reopen, select Source/Destination, then Link and Accept two separate pairs.
  Change the destination dropdown to Source and press Start. After accepting the
  existing same-style warning, Close must still commit both staged pairs.
  A single Undo restores the full baseline, and a single Redo restores both pairs.
- **`restart`:** Stage the same two pairs, then select Alternate Source/Alternate
  Destination and successfully Start again. Link and Accept the alternate pair,
  then Close. Only the alternate destination must change: both earlier staged
  destination rows must remain exactly as originally loaded. One Undo/Redo checks
  the complete successful-restart transaction.
- **`no-pair`:** Stage the same two primary pairs, select the existing Source and
  Unused styles, and Start. Unused has no matching event: the dialog must remain
  enabled with no additional modal, Accept Line on the empty display is a safe
  no-op, and Close must leave the entire baseline ASS unchanged and add no Undo
  entry. This retains the existing successful-Start reset semantics even when no
  pair is available. Reopen, Start Source/Destination, and stage both pairs again;
  Close and full-file Undo/Redo must still work. Initial and no-pair display PNGs
  are saved for visual verification of the empty display; pixel equality is not
  asserted because focus changes the label color.
- **`empty-file`:** Open an ASS containing all six styles but zero input Events.
  The native document controller inserts one empty Default event during load.
  The harness explicitly requires zero input events and exactly one saved empty
  Default event before Start. Source/Destination therefore have no matching pair;
  Start/Accept/Close must remain safe, show no extra modal, preserve the entire
  loaded ASS, and add no Undo entry. This is a native empty-*file* scenario, **not
  a test of an internally empty Events container**.

The nonempty scenarios use a ten-event, six-style ASS file with three primary pairs, an
interleaved alternate pair, a comment and a non-target sentinel, Unicode text,
override tags, distinct layers, timestamps, actors, margins, and effects.
Source durations sum exactly to their event durations; no frame/exact-timing
rounding behavior is changed or tested by this harness.

The oracle compares **every line of the complete saved ASS**, including Script
Info, styles, project metadata, and every event field. The baseline is obtained
with native Save As to account for the normal ASS writer's initial serialization.
Expected output is that baseline with only explicitly named destination Text
fields replaced by fixed expected karaoke strings. Subsequent native Save As
also works if a defective Start has lost all staged work and disabled Save.
There is no metadata filtering, partial-row comparison, or expected-failure mode.

History-state observations open the actual Edit popup with a control-local
`WM_SYSCOMMAND/SC_KEYMENU` message and require its native menu window to appear.
Only then does the harness inspect the refreshed dynamic menu label and native
enabled/disabled state; reading cached labels from a closed menu is insufficient.
No-pair cases require disabled `Nothing to undo`. Committed matching cases require
enabled `Undo kanji timing`, then enabled `Redo kanji timing` after Undo, in
addition to the complete-file history oracle. The popup is dismissed with a
window-local `WM_CANCELMODE` post and a bounded disappearance wait.

## Empty Events boundary

Production `OnStart` maps an empty Events container to null source/destination
pointers instead of dereferencing its end iterator, skips their Style access,
and follows the existing empty-pair reset path. Normal native document loading
still supplies a default event. The four E2E scenarios therefore verify normal
user-facing behavior, not the genuinely empty internal container branch. They
do not establish a native empty-list reproduction or instrumented verification
of invalid memory access.

## Evidence and limits

Artifacts include generated `host/fixture.ass`, the full `baseline.ass`,
`expected.ass`, `after-close.ass`, `undo.ass`, and `redo.ass`; native-window JSON,
dialog PNGs at actual linking/staging/warning boundaries; control action records;
opened Edit popup PNGs and refreshed label/enable-state JSON;
producer source; executable/source hashes; per-assertion `result.json`; and
process/private-desktop identities. A failure preserves the current observations
and any already saved ASS outputs, returns nonzero, and does not retry the scene.

The staged rejection case intentionally uses **identical existing styles**.
The controls are read-only, so a user cannot clear a previously selected style
to reproduce missing-style rejection after staging. Missing selection is covered
only at initial dialog open; the harness does not fake it with control selection
reset messages. The harness does not cover an internally empty Events list,
automatic Japanese interpolation, or other matching algorithms. The no-pair
scenario directly exercises a missing destination; the empty-file scenario has
neither a source nor a destination match. Audible bell behavior is not captured.
