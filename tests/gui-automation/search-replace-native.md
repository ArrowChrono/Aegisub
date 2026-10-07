# Native wx Search/Replace E2E

Run from the repository root with the built wx application:

```powershell
dotnet run --file tests/gui-automation/search-replace-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/search-filters filters
```

Use a new artifact directory for each run. The driver launches the real GUI on
an owned, inactive private desktop, uses observed native controls, and never
switches the input desktop or touches the clipboard. The existing desktop
supervisor imposes a 210-second deadline; the scenario has a 180-second watchdog
and each GUI wait is bounded. It creates an isolated GUI-test profile with the
supported native text editor (`Subtitle/Use STC = false`) so selections can be
observed through native Edit messages and converted back to UTF-8 byte ranges.

The `filters` scenario covers a selected comment match made ineligible by
Skip Comments, a selected match excluded by the checked-style filter, and an
ineligible Actor-field replacement. Mixed styles, comment rows, override tags,
accented text, CJK, supplementary Unicode, and untouched sentinel metadata are
included. It checks no premature dirty state, the eligible next match, the saved
ASS fields, and one-step Undo.

Artifacts include the producer source, executable/source hashes, native window
observations, generated input and saved ASS files, per-assertion results, and
private-desktop/process identities. A nonzero exit or timeout is a failure;
`result.json` records the first failed assertion rather than retrying the scene.
