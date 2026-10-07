# Native wx Search/Replace E2E

Run from the repository root with the built wx application:

```powershell
dotnet run --file tests/gui-automation/search-replace-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/search-filters filters
dotnet run --file tests/gui-automation/search-replace-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/search-atomic atomic
dotnet run --file tests/gui-automation/search-replace-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/search-zero zero
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

The `atomic` scenario first creates a real Redo entry. Replace All can prepare
several early-row replacements before `needle|^(a+)+$` reaches a late row of
100 `a` characters followed by `!`. The scene requires Boost's real complexity
error, unchanged dirty state and live ASS data (observed with Save As rather
than the disabled Save command), and preservation of the prior Redo entry.
A corrected query must then replace all rows in one independent Undo/Redo step.

The `zero` scenario checks repeated `(?=a)` in `aa`, same-position nonempty
alternatives, supplementary Unicode, decomposed/composed accents, skipped ASS
tags, end-of-line traversal, query/caret/Undo invalidation, and the existing
singleton Selected Rows traversal relaxation. A first-hit scene has a real
catastrophic regex suffix after two valid early matches: Find Next must return
those matches without evaluating the later suffix. Unicode fixtures have an
explicit UTF-8 BOM to keep charset guessing out of the search assertion.

## Matcher continuation boundary

The original `MakeSubtitleMatcher(settings)` interface and All enumeration are
unchanged. The overload taking `search_after_empty` is for resuming a regex on
the **same, unchanged searchable field**, using the previous empty match's
`MatchState::search_end` (NFC/tagless byte coordinates, not original-field
coordinates). It rejects only an empty match at that initial position, retains
full lookbehind context, and returns at the first hit. The GUI engine discards
this cursor when the document, active row, row selection, text selection, or
query changes; a new row always uses the ordinary matcher.

Artifacts include the producer source, executable/source hashes, native window
observations, generated input and saved ASS files, per-assertion results, and
private-desktop/process identities. A nonzero exit or timeout is a failure;
`result.json` records the first failed assertion rather than retrying the scene.
