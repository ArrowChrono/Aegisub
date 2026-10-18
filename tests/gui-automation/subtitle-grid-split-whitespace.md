# Subtitle grid split by whitespace GUI regression

This scenario dispatches `edit/line/split/by_whitespace` through the real GUI
command system. Lua validators compare the complete document with semantic
snapshots, excluding only the derived `raw` serialization field. They also
compare the exact selected indices and active index. The expected output is
constructed from explicitly specified fragments, not a second whitespace
splitter.

## Coverage

The fixture has twelve dialogue rows. Rows **2, 4, 5, 6, 8, 9, 10, and 11**
are selected, with row **8** active. `Select input` returns this selection out
of document order. Splitting produces exactly **42** dialogue rows, of which
**38** are selected; the original active row becomes row **35**. Four
unselected lines remain byte-for-byte unchanged even though they contain
spaces or tabs.

| Requirement | Evidence |
| --- | --- |
| Noncontiguous selection and original active line | `Input unchanged` checks all eight selected source rows and active row 8; `Split exact` checks all 38 selected output rows and active row 35. |
| Unicode White_Space separators | `Split exact` requires row 6 to become the 26 tokens `w01` through `w26`, separated in the input by all 25 White_Space code points listed below. |
| Chinese, English, emoji, leading/trailing and consecutive whitespace | `Split exact` requires row 2 to become exactly `你好`, `Hello`, and `😀`, with no empty fragments. |
| Simple text splitting rather than ASS-aware parsing | `Split exact` requires row 8 to become `{\fnComic`, `Sans}甲\N乙\h尾`, and `Z`. A space inside a tag splits it; literal `\N` and `\h` do not split. |
| Empty, whitespace-only, padded single-word, and non-whitespace Unicode no-ops | `No-op unchanged` compares all document fields and the exact selection for rows 4, 5, 9, and 10. Row 5 retains its leading/trailing whitespace; row 10 retains U+200B and U+FEFF without splitting. |
| Original times and metadata | `Split exact` compares each fragment's original start/end time, comment flag, layer, style, actor, effect, all margins, and ordinary ExtraData. Input times are intentionally not in row order. |
| Folding boundary ownership | `Split exact` requires the start marker on row 2 and end marker on row 11 to remain on their original first fragments, with no folding markers on any cloned fragment. Ordinary ExtraData is still copied. |
| Unselected dialogue and non-dialogue entries | `Split exact`, `Input unchanged`, and `No-op unchanged` compare the whole document, including unselected rows, both styles, and all script-info entries. |
| Single undo and redo | After a split and repeated no-op, one undo must satisfy `Input unchanged`; one redo must satisfy `Split exact`, including selection and active line. |
| No-op history preservation | After an initial no-op, one undo must satisfy `Initial restored` and disable further undo. Repeating the command on split output must not add an undo entry. A no-op while redo is available must keep `edit/redo` enabled and restore `Split exact` when redone. |
| Clean final restoration | Two final undo commands satisfy `No-op unchanged` and `Initial restored`, after which `edit/undo` must be disabled. |

The separator row covers U+0009 through U+000D, U+0020, U+0085, U+00A0,
U+1680, U+2000 through U+200A, U+2028, U+2029, U+202F, U+205F, and U+3000.
Control characters stay in fixture data and never appear in scenario IDs or
macro names. Both fold boundaries are expanded; this case checks marker
ownership, not collapsed-grid painting.

## Reproduce on Windows

Run from the repository root after building the `Aegisub` target. Keep the
existing build directory's configuration; this example uses `RelWithDebInfo`.
The fixture is the only autoload script in a fresh isolated profile, and
`am/reload/autoload` loads it synchronously before preparation.

```powershell
$ErrorActionPreference = 'Stop'
$profile = 'build-dir/split-whitespace-profile-' + (Get-Date -Format 'yyyyMMdd-HHmmssfff')
$artifacts = "$profile/artifacts"
New-Item -ItemType Directory -Path "$profile/user/automation/autoload", $artifacts | Out-Null
Copy-Item -LiteralPath tests/gui-automation/subtitle-grid-split-whitespace.lua -Destination "$profile/user/automation/autoload/subtitle-grid-split-whitespace.lua"
'{"Path":{"Automation":{"Autoload":"?user/automation/autoload/"}}}' | Set-Content -LiteralPath "$profile/user/config.json" -Encoding utf8

$arguments = @(
  '--gui-test', 'run',
  '--scenario', 'tests/gui-automation/subtitle-grid-split-whitespace.json',
  '--profile-dir', $profile,
  '--artifacts', $artifacts
)
$process = Start-Process -FilePath build-dir/RelWithDebInfo/Aegisub.exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput "$artifacts/process.stdout.log" -RedirectStandardError "$artifacts/process.stderr.log"
if (-not $process.WaitForExit(240000)) {
  if (-not $process.HasExited) {
    & taskkill.exe /PID $process.Id /T /F | Out-Null
  }
  throw 'Whitespace split GUI regression exceeded its 240 second process limit'
}
$process.WaitForExit()
if ($process.ExitCode -ne 0) {
  throw "Whitespace split GUI regression exited with code $($process.ExitCode)"
}
$result = Get-Content -LiteralPath "$artifacts/result.json" -Raw | ConvertFrom-Json
if ($result.passed -ne $true -or $result.timed_out -ne $false -or $result.exit_code -ne 0 -or $result.host -ne 'gui-test' -or $result.scenario -ne 'subtitle-grid-split-whitespace' -or $result.steps.Count -ne 40) {
  throw 'Whitespace split GUI regression did not produce a complete passing result'
}
if (@($result.steps | Where-Object { $_.status -ne 'passed' }).Count -ne 0) {
  throw 'Whitespace split GUI regression contains an unsuccessful step'
}
$evidence = Get-Content -LiteralPath "$artifacts/split-whitespace-evidence.txt"
$labels = @($evidence | Where-Object { $_ -match '^(prepared|no-op|split|repeat-no-op|undo|redo|initial-restored)$' })
if (($labels -join ',') -ne 'prepared,no-op,split,repeat-no-op,undo,redo,initial-restored') {
  throw 'Whitespace split GUI regression did not produce all evidence checkpoints'
}
$evidence
```

The **40 steps** each have a **5 second** timeout, for a **200 second** total
scenario budget. The independent **240 second** process limit includes startup
and shutdown. A timeout, missing report, partial result, failed step, or missing
evidence checkpoint is a failure. Do not retry indefinitely. The fresh profile
prevents a prior run's result or evidence file from being accepted as current.

## Artifacts and limits

The explicit artifacts directory contains `ready.json`, `result.json`, runtime
logs, redirected process logs, and `split-whitespace-evidence.txt`. The evidence
file records all dialogue texts, times, metadata, ExtraData, selection, and
active row at seven checkpoints: prepared, no-op, split, repeat-no-op, undo,
redo, and initial restoration. It uses lossless `\xHH` byte escapes for
non-ASCII bytes, controls, quotes, and backslashes, so tabs and line separators
cannot corrupt its record structure. All writes and the final close are
checked. The file contains no machine-specific paths and is reproducible from
the checked-in fixture.

The executable Lua predicates and the complete passing `result.json` are the
assertion evidence; a text dump alone does not prove success. No clipboard,
native mouse/keyboard input, focus changes, or screenshot matching is used.
This scenario verifies real command behavior and document/selection/history
state. It does not verify the right-click menu position, localized text,
collapsed-grid painting, or subtitle rendering. Lua's public dialogue tables
do not expose internal line IDs, so physical object identity is not directly
asserted; original marker ownership and active-row continuity are asserted.

Execution status: the `Aegisub` target built successfully in `build-dir` with
the `RelWithDebInfo` configuration. The actual `--gui-test run` execution
passed **40/40 steps**, exited with code **0**, and reported
`timed_out: false` with a scenario duration of **623.0796 ms**. Its artifacts
are in `build-dir/split-whitespace-validation/e2e-01/artifacts`; all seven
evidence checkpoints were inspected. The full C++ `test-aegisub` target also
completed with exit code **0**: **2834 passed**, **2 skipped**, and **0 failed**
out of **2836** tests, including all **58** `subtitle_edit_ops` tests. Its XML
report is `build-dir/split-whitespace-validation/unit-tests.xml`.
