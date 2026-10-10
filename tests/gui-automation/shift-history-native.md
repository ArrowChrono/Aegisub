# Native wx numeric Shift history E2E

Run from the repository root with a freshly built wx host and a new artifact
directory:

```powershell
dotnet run --file tests/gui-automation/shift-history-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/shift-history-native
```

The .NET 10 Windows driver reuses PrivateDesktop, observed menu/button commands
and the existing GUI-test host. It uses an inactive private desktop without
switching the input desktop, global input or clipboard access. The driver has a
180-second watchdog and the shared supervisor a 210-second limit. Native calls
have one-second timeouts, ordinary waits ten seconds, startup twenty seconds and
normal process exit ten seconds. Run exclusively from other GUI scenarios.

The fixture has four dialogue rows, a comment, three styles, Unicode/tag/text
sentinels and complete metadata. Its isolated profile seeds old padded/comma
history, signed/padded frame history and decimal-second history. The observed
native ListBox must publish canonical labels. Actual native time/radio controls
select a 1230ms backward start-only shift. Cancel must preserve the entire saved
ASS and original history; OK must change only Start across all five event rows.
The literal full-file oracle also checks unrelated fields and headers.

New history must contain the actual canonical amount and one-based inclusive
selected blocks; the old frame entry must be normalized to the same frame count.
Bounded normal exit with native code zero must flush numeric preferences. A new
process using the same profile must reread the history and preferences.

A malformed second record must show one warning and no partial list. The driver
observes the SDK-hosted Windows TaskDialog's actual OK button and dismisses it
with its window-local TDM_CLICK_BUTTON command, not a guessed child control ID.
Applying a valid shift after this error must preserve the malformed file. Actual
Clear must remove it; reopening must have no warning/stale entries. Applying
again must produce one valid record and exactly another 1230ms shift. All normal
exits and final full-ASS comparisons are mandatory.

Artifacts contain result.json, driver.log, actions.ndjson, native control
inventories, current PNGs, complete baseline/actual/expected ASS files, history,
profile and normal-startup evidence. There is no source/executable hash manifest.

History double-click is not dynamically covered by this scenario. wx MSW's
GetMessagePos hit test rejects inactive-desktop messages lacking actual cursor
position. No production keyboard shortcut, extra button or hit-test bypass was
added merely to make automation pass. Direct numeric restoration is source
reviewed; this limitation must remain explicit in acceptance reports.
