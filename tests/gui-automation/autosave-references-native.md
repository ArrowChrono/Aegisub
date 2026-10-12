# Native autosave reference recovery E2E

Run from the repository root with the completed wx build and a fresh artifact
subdirectory. Do not replace the executable while the scenario is running.

```powershell
dotnet run --file tests/gui-automation/autosave-references-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/autosave-references-native
```

Compile the driver without starting the GUI:

```powershell
dotnet build tests/gui-automation/autosave-references-native.cs
```

The .NET 10 Windows driver uses the existing `PrivateDesktop` helper to launch
its owned application on an inactive desktop. It never switches the input
desktop, sends global input, or reads or writes the clipboard. Commands target
observed native menus and verified host windows. Native file dialogs use their
filename edits and buttons. UI Automation reads the linked-file confirmation;
the actual Automation menu supplies macro command evidence.

The generated English profile configures a two-second autosave interval and
disables backup-on-open and save-on-every-change, and asks before loading linked
files. GUI test startup intentionally disables automatic save, so the driver
opens the real Backup preferences page, verifies the disabled checkbox plus the
configured interval and path, enables it, and clicks Apply. Before/enabled/applied
JSON and PNG observations retain this real option-subscription flow. Autosaves go two directory levels away from the source document. Linked
media are deliberately path-resolution fixtures, not decodable media: the
native confirmation must name the original audio, video, timecodes, and keyframes
paths, then the driver chooses No. This tests recovery path resolution, not
media decoding. The secondary subtitle source is a valid ASS; its persisted
path is checked in each autosave and again after recovery plus a second timer autosave, without
claiming secondary video rendering coverage.

| Stable case | Required behavior |
| --- | --- |
| `AutosaveRelocatesSourceReferences` | Opens the source ASS, checks the exact native linked-file confirmation and three macros, then executes the local Lua macro. A real timer autosave must retain the edited dialogue and all other events; media, timecodes, keyframes, secondary subtitles, and local Lua paths must resolve to their original files. The base `$` Lua reference must remain exactly unchanged; all three retain order. |
| `AutosavePreservesLiveDocumentAndHistory` | Checks the live filename, dirty state, and untouched source text. A second live macro edit must autosave without compounding relocation. One Undo must retain the first edit and dirty state; the second Undo must reach the saved revision and produce an autosave with every original event. One Redo must restore precisely the first edit and dirty state. Every later autosave must preserve the same original reference targets. Timestamp gating prevents earlier autosaves from satisfying these observations. |
| `RecoveredAutosaveResolvesSourceReferences` | Discards only the live edit, opens the autosave from its recovery directory, checks the actual native linked-file paths and exact macros, executes the recovered local macro, and waits for a fresh real timer autosave of that recovered document. Its event data must contain the second edit; all five linked paths and all three script targets must still resolve to the original sources. |
| `DummyReferencesRemainOpaque` | Opens a separate document containing dummy audio and video references, checks the native confirmation, edits through Lua, and waits for its own timer autosave. Both complete dummy strings must remain exactly unchanged, alongside correct script references and events. Ends with normal owned-host shutdown and exit code zero. |

The ASS fixture has two dialogue lines and one comment, distinct layers, timing,
actors, an effect, Unicode text, commas, and override tags. Only the first
dialogue text is edited. Lua sources occupy the document directory, a separate
Automation base directory, and an external directory. The input uses `~`, `$`, and `/` references. The real manager normalizes the `/`
input to a shorter `~` reference when opening this same-drive fixture, so the
scenario verifies its target rather than claiming unchanged absolute-prefix
serialization through AutoSave. Absolute inputs are constructed at runtime; no machine-specific paths are embedded in this scenario.
The default scenario covers timer AutoSave only. A separate, explicit probe retains
all original strict cross-directory manual Save As assertions:

```powershell
dotnet run --file tests/gui-automation/autosave-references-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/autosave-cross-directory-probe --cross-directory-save-as
```

This probe executes the same recovery flow, then saves to the source documents
directory and requires script and secondary paths to resolve there. It currently
fails genuinely: manual wx Save As retains recovery-directory-relative Lua and
secondary strings, so those strings point elsewhere from the new destination.
`LocalScriptManager::SaveLoadedList` is registered for script collection changes;
the ordinary Save As path does not refresh that script list. Secondary path
refresh depends on a `SecondarySubtitleSession` UI instance and its property-update
listener; the exact runtime reason that its value remained unchanged in this
probe has not been established. The observed wrong secondary string is evidence,
not a claim that UI activation disconnected that listener. This separate
manual-save problem is outside the scoped AutoSave fix. The probe is not marked
expected-failure, does not suppress its failing assertion, and returns nonzero.
The original observed failure remains in the `autosave-references-native-final-07`
artifact directory. Normal Save As also rebuilds media properties from actual
loaded state, so the probe's manual-save assertions cover events, scripts, and
secondary subtitles rather than media that were intentionally declined.

The driver enforces a 180-second watchdog; the helper supervises it with a
210-second limit. Individual GUI and autosave waits are ten seconds, startup is
twenty seconds, and native text entry is one second. Existing autosaves are
rejected so stale output cannot qualify as a new run. The 40-millisecond sleep
only spaces predicate checks inside the ten-second deadline; it never establishes
success by elapsed delay. Transient autosave read failures are retained in
`autosave-read-pending.ndjson`, including the relative filename, exception, and
observation time, and never extend the deadline. Failure stops only the
owned host and retains diagnostics. Repeat using a new artifact subdirectory.

`result.json` contains all four stable case IDs, per-case status/counts, and
individual assertions. A successful driver parses the JSON and requires the
exact case set, nonempty successful evidence for every case, and no failed
assertions before returning zero. Other artifacts include generated ASS/Lua
fixtures, the isolated profile, actual autosaves in `recovery/autosaves`, frozen
ASS observations, native linked-dialog JSON and PNGs, menu JSON, recovered timer
autosaves (or the probe's manual saved ASS), `actions.ndjson`, `driver.log`, and desktop/process identity records.
On failure, accessible live windows are recorded in `failure.json` and PNGs.
Runtime artifacts can contain resolved fixture paths. Frozen ASS observations
are evidence copies; open the original recovery autosave for path resolution.
