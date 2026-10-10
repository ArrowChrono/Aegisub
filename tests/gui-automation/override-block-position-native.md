# Native closed-override-block position E2E

Run from the repository root against the newly built **wx** application with a
new artifact directory:

~~~powershell
dotnet run --file tests/gui-automation/override-block-position-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/override-block-position-native
~~~

This file-based .NET 10 Windows scenario uses the existing `PrivateDesktop`,
`ObservedWindowsCommand`, and `AutomationProtocol` infrastructure. Both the
driver and application remain on an inactive private desktop. The scenario
never switches the input desktop, injects global keyboard/mouse input, accesses
the clipboard, or adds a production automation hook. Run it exclusively, not
during a shared build or another GUI scenario.

The total watchdog is **180 seconds**, inside the driver's **210-second
supervisor**. Native requests have one-second timeouts, ordinary GUI waits ten
seconds, startup readiness twenty seconds, and normal exit ten seconds. A
timeout, forced termination, ambiguous control, unexpected ASS difference, or
nonzero normal exit is failure, not a skip. There are no unbounded retries.

## Closed-block boundary and fixture

The active line is exactly:

~~~text
中😀{\b1}{\i1}éZ
~~~

The observed native EDIT receives UTF-16 selection **[11,11]**. This is UTF-8
raw position **15**, before the `1` in the second complete override block. The
prefix before that raw position contains an opening brace without its closing
brace, even though the actual subtitle contains the complete `{\i1}` block.
The cross-line ordinary-character ordinal must therefore be **2** (`中`, `😀`),
not a count that treats the truncated override prefix as ordinary text.

This scenario deliberately preserves the active raw anchor inside the second
block. Its expected active-line results do not move the operation to a plain
text boundary or merge the two adjacent blocks. Only the cross-line count is
at issue; the active raw/plain positions and target remapping remain unchanged.
The related boundary `中{\b1}{\i1}😀é` at UTF-8 raw position 11 must count 1,
but that second active-text fixture is not executed by this scenario.

The regression was introduced by
`2447e907ed56298d2d879a08a4baeacc6e487d0d`: counting a prefix discarded the
closing brace before calling `CharacterCount(IGNORE_BLOCKS)`. The repair finds
complete brace blocks in the original text and moves only the counting boundary
back to the containing block's opening brace. Truly unterminated text still
counts literally, and the existing ICU grapheme counting remains in use.
The focused C++ boundary tests additionally cover all interior offsets of
`ABC{\b1}{\i1}DEF`, both Unicode fixtures, combining marks, literal unclosed
text and clamping; they were written and observed failing before the repair.

This is not a change to rendered-text semantics. Escaped opening braces,
comments, drawing text and grapheme joining across removed blocks are not
normalized through `GetStrippedText`; agreement between those separate
grammars is outside this repair. Active raw/plain anchors and `RemapPosForLine`
are unchanged.

Four selected dialogues and one interleaved, unselected comment contain
Unicode/emoji and distinct layers, timestamps, actors, effects, margins and
styles. All selected rows have effective explicit bold/italic 1 at their
operation anchors, so a size-only edit does not ask this scenario to test
unrelated font-variant propagation. The other selected texts are:

~~~text
{\bord3\c&H010203&\b1\i1}é界XY😀 tail
{\shad12\alpha&H40&\b1\i1}中🙂uv café
{\fnArial\fs24\b1\i1}Q
~~~

Their leading overrides have different lengths. Ordinal 2 means the position
before `X`, before `u`, and the end of the short `Q` target, respectively. The
short target verifies existing end clamping. The comment, all nontext event
fields, Script Info and four style definitions are sentinels. Rendering uses
Arial; no installed localized alias pair or font-candidate selection is needed.

`Seed`, `Bold`, and `Font` are complete handwritten event arrays. Every preview
also has a handwritten active-text literal. No production parser, position
mapper, tag helper, or transformed actual result generates expected text. A
native Save As baseline accounts for ordinary header/style/project writer
serialization, but its five event records must first match `Seed` exactly.
Each persisted checkpoint substitutes only the corresponding literal event
array into that baseline, writes the complete expected ASS, and compares every
saved line in order. Metadata is not filtered out of comparisons.

## Actual native operations

1. Open **Subtitle → Select Lines...** through its observed native menu command.
   Match actors containing `pick-`, choose Matches/Contains/Actor/Set selection,
   include dialogues and exclude comments with the actual native controls.
   Capture the selection dialog and save the baseline.
2. Set the observed active EDIT to [11,11] with `EM_SETSEL` and read it back
   through `EM_GETSEL`. A window-local Shift key-up, without a preceding key-down
   or global input, lets the existing selection controller observe the range.
   Save `anchor.json` and an actual range screenshot.
3. Invoke the actual **Toggle Bold** button. The active result must be
   `中😀{\b1}{\i1\b0}éZ`, with native caret [14,14]. The other rows insert
   `{\b0}` at their mapped ordinal-2 anchors, including after `Q`. Save and
   compare complete ASS. One actual **Undo toggle bold** restores all original
   rows and [11,11]; one **Redo toggle bold** restores all literal results and
   [14,14]. Undo once more and compare the pristine font baseline.
4. Open the actual **Font Face** button at [11,11]. **Select Font** must show
   Arial and size 24. Replace only the observed size EDIT with 37 and then 39.
   Each actual live preview must equal its complete literal active text, such
   as `中😀{\b1}{\i1\fs37}éZ`, and retain caret [16,16]. Both the native font
   EDIT and read-only UIA ValuePattern must still say Arial. Capture both
   previews. **Cancel** must restore the exact active text and [11,11], followed
   by an exact complete-ASS comparison of all rows and metadata.
5. Reopen at the same original [11,11] anchor and preview size 35, then size 41.
   Require the same exact text, font-family and [16,16] checks on both previews.
   **OK** must retain only the final result: active
   `中😀{\b1}{\i1\fs41}éZ`, with `{\fs41}` before `X`, before `u`, and after
   `Q` on the other selected rows. The short row's earlier `\fs24` remains
   unchanged because the operation belongs at the mapped end anchor. Compare
   the complete saved ASS against `Font`.
6. One **Undo set font** must restore the original five events and [11,11], not
   an intermediate preview. One **Redo set font** must restore only final size
   41 at all original anchors and [16,16]. Compare complete ASS after each.
7. Capture the final saved state, then require normal `WM_CLOSE` completion and
   native exit code zero. Forced cleanup is only failure containment.

The font dialog remains modal during previews, so those checkpoints inspect the
actual active text/range and input controls; the complete multirow persisted
oracle is checked after Cancel, Accept, Undo, and Redo. Repeated preview and
cancel assertions are intentionally strict: a stale native text-control cache,
duplicate preview tag, caret drift, or incomplete restoration must fail rather
than being accepted as part of the character-count correction.

## Observed controls and isolation

All HWNDs are checked against the application's PID and private desktop.
The Font Face button must be a unique native Button with its exact label, and
the Select Font owner must be the observed main window. The editable font
ComboBox is discovered through read-only UIA metadata; its native EDIT is
obtained with `GetComboBoxInfo`. Font size is the unique native Edit to the
right of the ComboBox on its actual row, inside the actual labeled **Font**
group. No guessed font IDs, enumeration-order fallback, or fixed coordinates
are used. UIA patterns are never used for input.

Text entry uses `EM_SETSEL` plus `EM_REPLACESEL` on the actual owned EDIT HWND,
including size, line-selection criteria, and the Save As filename. It does not
send a synthetic `EN_CHANGE` or write directly to the document model. Save As
uses the existing file-picker UIA metadata to identify its native filename
EDIT and Save button.
Window discovery and UIA preparation share one ten-second deadline. Each
readiness check confirms that the current unique Save As window is the same
observed owned HWND, then takes a fresh UIA descendant/metadata snapshot.
Missing controls or `ElementNotAvailableException` mean not ready yet; timeout
is failure. Duplicate controls, a replacement dialog, and all other errors fail
immediately. Filename entry and Save activation are performed once, after both
unique native handles are ready; the save operation or scenario is not retried.

Undo/Redo first opens the actual Edit popup using its native mnemonic, which
refreshes dynamic history labels. The observed menu inventory and PNG are
saved; the exact expected label must be enabled and agree with its discovered
command ID before bounded window-local keyboard navigation highlights and
activates that item. Only `ERROR_INVALID_WINDOW_HANDLE` (1400) is treated as a
popup destroyed during class-name enumeration; other native errors propagate.

Top-level caption discovery uses cached `GetWindowTextLengthW` /
`GetWindowTextW` after ownership checks, avoiding synchronous caption queries
while a dialog is being created. Child-control values still use the bounded
native text request. The scenario thread uses PerMonitorV2 DPI awareness to
match the host, without changing the input desktop.

## Artifacts and acceptance limits

The new artifact directory retains the fixture, isolated profile, readiness and
private-desktop/process identity evidence; `driver.log`, `actions.ndjson`, and
`result.json`; selection/font/menu inventories and focused PNGs; original
anchor and actual editor-range JSON; a separate preview-completion JSON for
each size; and full actual/expected ASS pairs for each persisted checkpoint.
The result is marked incomplete until every assertion and normal exit passes.
Ordinary failures also retain the actual failing window inventory and PNG.

Acceptance requires a fresh run with `passed: true`. Building the driver or
application, reusing old artifacts, or observing only the active line is not a
substitute. Coverage is the wx Toggle Bold and Font Face cross-line ordinal
mapping from a complete override-block interior, plus the established preview,
cancel and history contracts on that anchor. Skia, unclosed/malformed blocks,
the second raw-11 fixture, complex grapheme clusters, color editing, font
candidate selection, and rendered glyph correctness are not covered.
