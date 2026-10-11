# Native Automation runtime E2E

Run from the repository root after the actual wx executable has finished
building. The artifact argument must be a new subdirectory of
`build-dir/artifacts`:

```powershell
dotnet run --file tests/gui-automation/automation-neutral-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/automation-neutral-native
```

This .NET 10 Windows scenario launches the real application with a private
profile and the existing `PrivateDesktop` helper. The driver and application
remain on their own inactive desktop. Every native action targets an observed
window of the launched process; the scenario does not use global input,
activate the desktop, or operate on the clipboard. The saved
`private-profile-settings.json` records the two paste command types removed
from the private profile's normal `menu.json`. Normal menu refresh validates
all commands, and these unrelated paste validators otherwise probe the shared
window-station clipboard. Automation and Undo retain normal menu behavior.
The default application configuration and user profile are untouched.

The driver has a 180-second watchdog and the helper supervises it with a
210-second process limit. Normal waits have ten-second deadlines, host
readiness has a twenty-second deadline, native synchronous messages have
one-second deadlines, and the Lua cancellation loop fails after twenty-five
seconds. A timeout fails the run and terminates only the owned processes.
The run requires exclusive access to its executable and artifact directory;
do not rebuild or replace the executable while it is running.

The checked-in fixture contains four complete ASS events: three dialogue
events and a comment, with Unicode, commas, override tags, different layers,
times, actors, margins, and effects. The Lua script registers seven real
macros and an export filter through the application runtime. Expected accepted
and exported event files are explicit fixtures, independent of the driver.

| Case | Required evidence |
| --- | --- |
| `ValidationAndToggle` | Baseline save equals every original event field. Real menu refresh disables the validation macro and clears the Lua toggle. Highlighting the observed command displays the Lua validation help in the native status bar. No undo history exists. |
| `DialogAcceptAndSelection` | Edits string, multiline string, integer, float, checkbox, and alpha controls in a real mixed-control Lua dialog. Lua verifies their returned types and values, the dropdown selection, and the RGBA default. Save matches the exact accepted events and active-row property. A second real macro inspects the two selected rows and active row before showing an acknowledgement. Validation, dynamic help, toggle, and the named undo entry all change. |
| `DialogCancelRollsBack` | Lua changes the first event, stages a named undo point, changes the tail, and opens a parameter dialog. The driver edits its text and clicks the actual Cancel button. Lua requires a false dialog result and cancels the invocation. Saved events, dirty state, selected/active rows, and the prior undo entry remain intact. |
| `ProgressCancelRollsBack` | Lua stages the same changes and reports title, task, and progress while polling a bounded cancellation loop. The driver waits for the actual task text and clicks its Cancel button once. The complete event, selection, dirty-state, and undo comparisons repeat. |
| `RuntimeFailureRollsBack` | Lua stages changes and raises a distinct error. The actual progress log must contain that error before the driver closes it. The complete rollback comparisons repeat. |
| `UndoAndSave` | Exactly one ordinary Undo consumes the accepted macro entry. Save restores every original event field; validation/help/toggle revert. Another real Lua invocation inspects the restored data and presents its success acknowledgement. |
| `ExportConfigurationAndExecution` | The real Export dialog edits the fixture filter's delimiter-rich UTF-8 prefix, integer, and boolean. First export matches all expected event fields and preserves the comment. Saving the live document proves copy isolation and unchanged undo history. Reopening Export reconstructs the dialog from serialized accepted settings; the controls and a second identical export prove readback. Final save remains identical to the original live events, then the host exits normally with code zero. |

`result.json` has these seven stable names, explicit case outcomes, and the
individual checks. A zero driver exit requires every expected case to contain
at least one check and pass; the driver parses the completed result before
returning zero. Previous completed cases remain distinguishable from an
incomplete later case if the run fails. The result also records scope limits.

Artifacts retain the copied Lua/input/expected fixtures, isolated profile,
all saved and exported ASS documents, event comparisons, menu/help observations,
runtime-error text, action log, driver log, screenshots of real dialogs, and
desktop/process identity records. On failure, `failure.json` and available
window screenshots preserve the observed host state. An unexpected completed
progress log encountered while finishing a macro is captured in
`unexpected-progress-log.json` and its screenshot and fails immediately with
the actual Lua diagnostic. Runtime artifacts may
contain resolved fixture paths, while checked-in sources use repository-relative
paths. No content hashes or integrity manifests are generated.

The native scenario does not exercise Workspace active-session or script
replacement guards. Modal disabling alone does not prove those contracts;
the separate neutral runtime E2E owns those checks. Existing
`local-script-references-native.cs` owns loading failure, metadata, manager
add/remove/reload, feature publication, and reference save/reopen behavior.
Dropdown and color controls retain nontrivial initial values here; their Lua
readback is checked, but the native color picker is not manipulated.

Export serialization is verified within the same live document by destroying
and reconstructing the real Export dialog. This scenario does not claim
settings persist through an ASS save/reopen: the current ASS writer does not
serialize `ProjectProperties::automation_settings`. The final saved ASS and
the restored control observations make that boundary visible.

The small native declarations follow the existing driver's `DllImport`
convention without adding an AOT requirement. HWND/HMENU values are borrowed
from the owned application and never released by the scenario. UTF-16 calls,
explicit Win32 BOOL marshalling, pointer-sized message parameters/results,
bounded synchronous messages, and rooted enumeration callbacks are used.
