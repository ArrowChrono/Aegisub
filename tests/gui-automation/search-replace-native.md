# Native wx Search/Replace E2E

Run from the repository root with the built wx application:

```powershell
dotnet run --file tests/gui-automation/search-replace-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/search-filters filters
dotnet run --file tests/gui-automation/search-replace-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/search-atomic atomic
dotnet run --file tests/gui-automation/search-replace-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/search-zero zero
dotnet run --file tests/gui-automation/search-replace-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/search-grouped grouped
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

The `grouped` scenario uses `x|ééé` → `界` on two subtitle lines containing
ASS tags, CJK, an emoji, and accents. Its five replacements both lengthen and
shorten UTF-8 text. It checks two rendered result rows with three/two separate
orange and green highlights, the actual five-replacement status, complete saved
lines, double-click navigation to the first replacement on each line, and
original UTF-8 selection coordinates after Undo. A manual edit must disable
only that historical result while the other line remains navigable. Find All
must still produce one result per match. Additional launches check grouped
deletions (adjacent zero-length green markers may form one continuous region),
separated deletions with three/two individually visible green markers, zero-width
insertions, style/comment filtering, and zero-width navigation after Undo.
Independent `$` and `(?=x)|$` Find All queries require visible orange markers at
nonempty line ends, including the final marker alongside an interior match.

Grouped result dialogs request a 1320×520 size and are captured with PrintWindow
on the private desktop (DPI rounding may change the captured size by a pixel).
PNG analysis counts exact orange/green background segments within the observed
UIA ListItem row bounds; it does not recreate the application's replacement
algorithm. The driver records native labels, accessible result line numbers,
accessible descendants, screenshot
geometry, and detected highlight ranges alongside each PNG. Saved ASS fields,
editor text, status labels, and editor selection are asserted independently.
The `visual-review.json` artifact lists expected column strings for manual PNG
review: line numbers, ordered Match/Replacement fragments, complete Context and
Replaced line text. Pixel counts alone do not establish glyph semantics, and
`result.json` passing means the automated checks passed, not that this manual
review was performed. No Copy Selected Lines action or clipboard is used.

## Matcher continuation boundary

The original `MakeSubtitleMatcher(settings)` interface and All enumeration are
unchanged. The overload taking `search_after_empty` is for resuming a regex on
the **same, unchanged searchable field**, using the previous empty match's
`MatchState::search_end` (NFC/tagless byte coordinates, not original-field
coordinates). It rejects only an empty match at that initial position, retains
full lookbehind context, and returns at the first hit. The GUI engine discards
this cursor when the document, active row, row selection, text selection, or
query changes; a new row always uses the ordinary matcher.

Artifacts include the producer source, executable/source hashes (including the
result dialog and match report sources), native window
observations, generated input and saved ASS files, per-assertion results, and
private-desktop/process identities. A nonzero exit or timeout is a failure;
`result.json` records the first failed assertion rather than retrying the scene.
