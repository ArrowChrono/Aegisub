# Subtitle grid join with line breaks GUI regression

This scenario exercises `edit/line/join/with_line_breaks` through real GUI
command dispatch, rather than directly invoking the text helper. Lua validators
compare the complete current document against semantic snapshots, excluding
only the derived `raw` serialization field. A document, metadata, time,
selection, or active-line mismatch fails the scenario.

## Coverage

The fixture contains eight dialogue lines. Rows **2, 4, 5, and 7** are selected,
with row **7** active; the macro returns the selection out of document order.
The scenario verifies this noncontiguous selection before joining. The input
includes Chinese and English, ASS override tags, existing literal `\N`
escapes, an empty selected row, and out-of-order start and end times.

The merged text must follow grid order, retain the original text and tags, and
insert exactly one literal `\N` at every selected-line boundary. The empty row
therefore produces consecutive `\N` escapes; it is not skipped. The first
selected row survives with its original comment flag, layer, style, actor,
effect, margins, and ExtraData. Its **5200 ms** start is retained even though
other selected rows start earlier. Its end becomes **11200 ms**, the maximum
selected end, which belongs to the empty row. An unselected row ends at
**15000 ms** and must not affect that result. All four unselected lines, all
styles, and all script-info entries must remain unchanged.

Joining and redo must leave only the surviving first selected row selected and
active. Undo restores every original line and all document metadata and times.
The existing join contract updates selection before committing, so undo leaves
the first selected row selected and active; it does **not** restore the former
four-line selection or active row 7. The validator asserts that existing
behavior explicitly, without changing it as part of this feature.

The scenario also checks that the new command is disabled for a single-line
selection, both before and after joining. It then reselects the same four input
rows and exercises the existing `edit/line/join/concatenate` command. That
command must still insert ordinary spaces, including the two consecutive
spaces caused by the empty row, without replacing the pre-existing `\N`
escapes. Final undo steps restore the clean initial document and verify that no
earlier undo state remains.

## Reproduce on Windows

Run all commands from the repository root after building the `Aegisub` target.
Use the configuration already present in the build directory; the example uses
the normal `RelWithDebInfo` executable. The Lua fixture is the only autoload
script in a new isolated profile. `am/reload/autoload` loads it synchronously
before fixture preparation, avoiding the asynchronous startup-loader race.

```powershell
$profile = 'build-dir/join-line-breaks-profile-' + (Get-Date -Format 'yyyyMMdd-HHmmssfff')
$artifacts = "$profile/artifacts"
New-Item -ItemType Directory -Force -Path "$profile/user/automation/autoload", $artifacts | Out-Null
Copy-Item -LiteralPath tests/gui-automation/subtitle-grid-join-line-breaks.lua -Destination "$profile/user/automation/autoload/subtitle-grid-join-line-breaks.lua"
'{"Path":{"Automation":{"Autoload":"?user/automation/autoload/"}}}' | Set-Content -LiteralPath "$profile/user/config.json" -Encoding utf8

$arguments = @(
  '--gui-test', 'run',
  '--scenario', 'tests/gui-automation/subtitle-grid-join-line-breaks.json',
  '--profile-dir', $profile,
  '--artifacts', $artifacts
)
$process = Start-Process -FilePath build-dir/RelWithDebInfo/Aegisub.exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$artifacts/process.stdout.log" -RedirectStandardError "$artifacts/process.stderr.log"
if (-not $process.WaitForExit(200000)) {
  if (-not $process.HasExited) {
    & taskkill.exe /PID $process.Id /T /F | Out-Null
  }
  throw 'Join line breaks GUI regression exceeded its 200 second process limit'
}
$process.WaitForExit()
if ($process.ExitCode -ne 0) {
  throw "Join line breaks GUI regression exited with code $($process.ExitCode)"
}
$result = Get-Content -LiteralPath "$artifacts/result.json" -Raw | ConvertFrom-Json
if (-not $result.passed -or $result.timed_out -or $result.host -ne 'gui-test' -or $result.steps.Count -ne 31) {
  throw 'Join line breaks GUI regression did not pass all 31 steps'
}
Get-Content -LiteralPath "$artifacts/join-line-breaks-evidence.txt"
```

The **31 steps** use a **5 second** per-step timeout and a **155 second** total
scenario budget. The independent **200 second** process limit includes startup
and shutdown. A timeout is a failure; do not rerun indefinitely or accept a
partial result as success. The unique profile prevents old result files from
being mistaken for evidence from this run.

## Artifacts and limits

The explicit artifacts directory contains `ready.json`, `result.json`, runtime
logs, redirected process logs, and `join-line-breaks-evidence.txt`. The semantic
evidence file records every dialogue's text, times, metadata, and ExtraData,
plus selection and active dialogue-row numbers at six checkpoints: prepared,
line-break join, undo, redo, old concatenate, and clean initial restoration.
It contains no machine-specific paths and is regenerated by the scenario.
The checked-in Lua predicates and `result.json` provide the matching executable
assertions; a text dump alone is not a passing result.

This scenario does not use clipboard operations, native input, focus changes,
or screenshots. It verifies command behavior and real document/GUI selection
state, not submenu placement, localized menu text, painted grid geometry, or
visual rendering of the merged subtitle. No screenshot is offered as evidence
of those untested properties.

Execution status: the `Aegisub` target built successfully in `build-dir` with
the `RelWithDebInfo` configuration. The actual `--gui-test run` execution
passed **31/31 steps**, exited with code **0**, and reported
`timed_out: false` with a scenario duration of **753.3771 ms**. Its artifacts
are in `build-dir/join-line-breaks-validation/e2e-01/artifacts`; all six
semantic evidence checkpoints were inspected. This result verifies the command
scenario, not native menu clicks or screenshot-based visual acceptance. The
full C++ `test-aegisub` target also completed in `RelWithDebInfo` with exit
code **0**: **2739 passed**, **19 skipped**, and **0 failed** out of **2758**
tests, including all **56** `subtitle_edit_ops` tests. Its XML report is
`build-dir/join-line-breaks-validation/unit-tests.xml`.
