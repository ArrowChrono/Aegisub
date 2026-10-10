# ASS override reuse boundary (E4)

## Status and sequence

The first E4 step relocates the existing wx per-line tag editor and coordinate
primitives into `subtitle_edit_ops`. Gac already had that relocation from
`03183e2db`; exp now supplies the same API without changing font-name policy.
This is not yet a shared selected-row batch or preview transaction.

The reviewed sequence is:

1. Align the per-line primitive foundation on exp, preserving current wx behavior.
2. Share data-only selected-position preparation, preserving the units below.
3. Evaluate immediate toggle/color preparation separately from font preview.
4. Keep preview, cancellation, identities, history and publication with hosts.

Do not introduce a generic callback-based batch/form framework merely to combine
loops with different lifecycle policies.

## Coordinate units

| Value | Unit / purpose |
| --- | --- |
| raw selection/caret | UTF-8 bytes in the complete ASS dialogue text |
| `NormalizePos` result / `SelectionPos::plain` | UTF-8 bytes in plain text before the raw position, with brace blocks removed |
| `DenormalizePos` argument | Plain-text UTF-8 byte offset, not a character ordinal |
| `CharacterPos` result | Visible Unicode codepoint count, ignoring blocks |
| `RemapPosForLine` argument | That codepoint count projected into another row |
| Gac native text positions | UTF-16 units, converted by the Gac adapter |

The active row keeps its exact raw endpoints even inside an override block.
Other rows are addressed by the same visible codepoint count, then mapped to
their own plain/raw byte positions. Copying active byte offsets across rows is
incorrect for differing prefixes or Unicode lengths. Gac's UTF-16 conversion
and wx text-control/STC adaptation stay outside these core primitives.

The former Gac comments calling `plain` a codepoint offset were imprecise;
the implementation always used plain-text bytes. Relocation does not change
the conversion algorithm or repair escaped-brace behavior.

## Existing per-line foundation

`ParsedLine` owns parsed blocks for a mutable dialogue. It retains its
`text_rewritten` state: raw positions can identify the first write precisely,
but whole-block serialization may change offsets beyond the written tag's
delta. Subsequent writes must retain the existing normalized-position route.
The writer can report the actual written block for caret placement.

Reset-aware scalar reads, fallback event styles, duplicate/alias handling,
block insertion and Boolean toggle range restoration remain the same.
`ToggleStyleTag` returns the first write's byte delta; the host still decides
how to restore its selection and commit the edited rows. Color/alpha reads and
writes are separate; exp's color transaction is not part of this first step.

Source comparison verified the old wx local bodies against Gac's relocated
primitives. The migration preserves read/write branches and coordinate results.
The new location resolves lint using reverse range iteration, designated
initializers and safe index comparison, not a new ASS editing algorithm.

## Do not flatten these host policies

- Toggle reads at selection start, writes inverse state there and restores the
  old start-state at a nonempty range's end.
- Font reads effective state at insertion point but writes at selection start.
  Those positions differ when the caret is at the end of a selected span.
- Color collapses selection after the written tag, preferring the writer's
  reported block over a potentially stale raw position.
- wx color amends a commit during preview and undoes on Cancel. Gac currently
  applies accepted color results, not the same preview transaction.
- Font preview is recalculated from pristine text, with distinct rendered and
  permanently changed sets. Gac additionally checks revision, row pointer/Id,
  selection and rendered text before restore/publication.
- Unknown named reset fallback for scalar reads and invalid font requests are
  existing distinct semantics. Do not incidentally unify them.
- Current exp font-name preservation from `ee1353a22` remains intact. Relocation
  does not port or normalize chooser family/variant policy between hosts.

## Exp foundation acceptance

Aegisub and gtest-run built in RelWithDebInfo. Full C++ passed 2,784 tests,
with two conditional skips and zero failures, after runtime fixture setup and
within a 180-second process limit. The result is
`build-dir/override-primitives-e4-cpp-results.xml`.

The existing font-name native scenario passed against the newly built wx host
under both localized-name preferences. It retains multi-row names, explicit
tags, resets, size-only changes, preview/Cancel, complete ASS/style/metadata
oracles, preference checks and normal exit. Fresh evidence is in
`build-dir/artifacts/override-primitives-e4-native/result.json`.
Primary review inspected current mixed-row size/preview and localized-name
dialogs. This is font-path coverage, not a newly executed native wx toggle or
color chooser scenario.

Full-rule clang-tidy ran through a separate compilation database for both
changed translation units. Relocated code introduces no modernize/performance
diagnostics; unrelated existing warnings remain outside scope. No new unit
tests were added for an unchanged relocation.

## Separate Gac-only finding

The legacy `ApplyFontOverride` method introduced by `72515628c2` mixes its
UTF-16 selection end with a UTF-8 insertion delta before converting back.
This must not become the coordinate contract of a shared helper. The production
font chooser uses `ApplyFontSelection`, so this finding is not evidence of a
chooser preview bug. Its repair and app-process regression are separate from
the exp primitive relocation and are recorded in Gac's reuse acceptance.
