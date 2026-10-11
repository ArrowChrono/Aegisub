# Native local script reference E2E

Run from the repository root with the actual built wx application and a new
artifact subdirectory:

```powershell
dotnet run --file tests/gui-automation/local-script-references-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/local-script-references-native
```

The .NET 10 Windows driver creates its own inactive desktop through the existing
`PrivateDesktop` helper. It never switches the input desktop, sends global input,
or reads or writes the clipboard. Every native command targets an observed
window owned by the launched host; UI Automation reads and selects the manager's
real list rows. File dialogs use their observed native filename edit and action
button. The Automation menu is read directly from the actual window menu.

The driver has a 180-second total watchdog, supervised by the helper's
210-second process limit. Normal GUI waits take at most ten seconds, host startup
readiness twenty seconds, and text messages one second. Failures preserve the
artifacts and stop only the driver's owned host. Run after the executable build
finishes and do not replace the executable during verification.

| Case | Required behavior and evidence |
| --- | --- |
| `ReferenceDiagnosticsInInputOrder` | Opens an ASS containing whitespace, empty pipe segments, all three reference prefixes, an unknown prefix, a missing file, Lua initialization failure, and an unrecognized file. Checks the original warning/error messages in input order; `diagnostics.json` and four diagnostic PNGs retain their observed text and windows. |
| `TrimmedPrefixesPreserveLoadedOrder` | Checks the exact ordered six rows, local flags, retained failed/unknown descriptions, and the exact successful macros. Sources occupy document, automation base, and external directories. |
| `ManagerAddPublishesMacro` | Adds a fifth working script with the real Add/file dialog flow. Checks its appended list position and newly published macro. |
| `ReloadUsesChangedLua` | Changes a physical Lua script's name, description, and macro; selects the original manager row and clicks Reload. Checks the changed row and exact menu contents, including removal of the old macro. |
| `ManagerRemovePublishesReferences` | Removes failed, unrecognized, and the working tail script. Checks row removal, retained row order, and the exact retained macro set. |
| `SaveAndReopenReferences` | Saves through the real Save As dialog, checks the exact reference string containing both `~` and `$` references and every subtitle event field, opens an empty ASS to clear the manager, reopens the saved ASS, and saves again. Checks the restored list/menu and reference/event roundtrip. |
| `ReopenEmptyClearsMacros` | Opens the empty ASS again, checks no local rows and the explicit empty macro menu, then verifies normal host shutdown with exit code zero. |

Fixtures and the isolated English-language profile are generated inside the
artifact directory. The ASS contains two dialogue lines plus a comment with
Unicode, commas, override tags, distinct layers, times, actors, and an effect.
The automation base has an extra directory level so script-relative references
are observably shorter for sources beside the document. The `/` input reference
is constructed at runtime and is expected to become a shorter `~` reference on
save. No local machine paths are embedded in the checked-in scenario.

`result.json` contains seven stable case names and individual assertions. A pass
requires all seven cases to have successful nonempty evidence. The driver parses
its own completed result before returning zero. It also retains `driver.log`,
`actions.ndjson`, list/menu JSON observations, manager PNGs, generated Lua and
ASS files, saved/roundtripped ASS, and the helper's desktop/process identity
records. `failure.json` and failure window PNGs are produced when a live host can
still be inspected. Runtime artifacts may contain the fixture's resolved paths.

The scenario validates real wx manager behavior and ASS persistence. It does not
test interactive desktop focus, keyboard accelerators, Lua macro execution, or
the Automation engine's internal implementation.
