# Native multirow override-position E2E

Run from the repository root against the newly built **wx** application with a
new artifact directory:

~~~powershell
dotnet run --file tests/gui-automation/override-position-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/override-position-native
~~~

This file-based .NET 10 Windows driver uses the existing PrivateDesktop,
ObservedWindowsCommand and AutomationProtocol infrastructure. Driver and
application remain on an inactive private desktop. It never switches the input
desktop, injects global keyboard/mouse input or accesses the clipboard. Run
exclusively, not during a shared build or another GUI scenario.

There is a 180-second total watchdog inside the existing 210-second supervisor.
Native messages time out after one second, ordinary waits after ten seconds,
startup readiness after twenty seconds, and normal exit after ten seconds.
Missing/ambiguous controls, unexpected complete-file differences, a watchdog,
forced termination or nonzero normal exit are failures, not skips.

## Fixture and independent oracle

Three dialogue rows and one interleaved untouched comment have Unicode/emoji,
distinct actors/layers/times/margins/effects, different leading override blocks
and three styles with different nontext properties. Default/Alternate are
regular; Third is bold. Primary alpha is 40 on all dialogue rows, with the third
also carrying a global alpha override. The second has a different explicit RGB
override. Rendering uses Arial; no font alias or font-selector behavior is
required.

The active text is:

~~~text
{\i1}A😀BCdé tail
~~~

Native UTF-16 selection [8,10] selects BC, after a closed leading block and the
entire emoji. Corresponding ordinary grapheme ordinals [2,4] select XY and uv
on the other rows, despite different UTF-8/leading-tag lengths. No selection is
inside a tag or surrogate pair. This does not claim to fix the legacy
tag-interior character-count quirk.

Seed, Bold, final Color and final Font arrays contain complete handwritten event literals.
No production parser, mapper, tag helper or output transformation generates
expected text. A native Save As baseline supplies the writer's header/style/
project metadata; its events must first equal the independent Seed array.
Subsequent comparisons substitute literal events into that baseline and compare
the complete saved ASS, including every nontext field and untouched comment.

## Actual native operations

1. Open Subtitle → Select Lines... using its observed menu command. Match
   actors containing pick-, set the selection, include dialogues and exclude
   comments using observed native radios/checks. Capture the actual selection
   dialog and save the native-opened baseline.
2. Set the observed native Edit range with EM_SETSEL and read back EM_GETSEL.
   Send a window-local Shift key-up without a key-down or global input:
   TextSelectionController observes key-up/left-up to refresh its cached range.
   Capture the nonzero range after the emoji.
3. Invoke the observed Toggle Bold button. Default/Alternate receive b1/b0
   spans; bold Third receives b0/b1. The active range remains on BC at UTF-16
   [13,15]. Compare complete ASS; one actual Undo restores all rows and one
   Redo restores all spans.
4. Undo Bold for an independent color baseline. Open Primary Color at the same
   nonzero range. Require the actual Select Color owner to be the observed main
   window. Verify initial HTML #332211, ASS &H112233& and alpha &H40&.
   Change HTML to #123456 using its observed native Edit. Capture actual input
   and live preview. Color inserts at grapheme ordinal 2 on all selected rows;
   active caret parks at UTF-16 [20,20], past the color tag before its closing
   brace. Cancel must restore exact original ASS and active [8,10] range.
5. Reopen at nonzero caret [8,8]. Preview #654321 then #123456, capturing both
   actual input states. RGB-only input preserves chooser alpha 40. Since all
   selected rows already have effective alpha 40, existing styles/global alpha
   remain unchanged and no new alpha tag is expected. Accept and compare all
   literal event records. One Undo must remove both amended previews; one Redo
   restores only the final color.
6. Undo Color and compare the complete pristine Seed again. Keep all three
   dialogues selected and set the actual active range to UTF-16 [8,10]. Open
   **Font Face** and require Arial / size 24. Change only the size EDIT to 37,
   then 39. Each actual live preview must equal its handwritten active-text
   literal, such as `{\i1}A😀{\fs37}BCdé tail`, and retain the actual native
   selection [15,17] on BC. Both the native font EDIT and read-only UIA value
   must remain Arial. Cancel must restore exact original text and [8,10], then
   the complete saved ASS must equal Seed. This exercises the font revert path
   that restores the entry selection before refreshing the shorter text.
7. Reopen Font Face with the same original [8,10] selection. Preview size 35,
   then 41, requiring exact literal text and [15,17] after each. Accept and
   compare the complete ASS against Font. One **Undo set font** must remove the
   entire operation and restore Seed / [8,10], rather than an intermediate
   preview. One **Redo set font** must restore only final size 41 / [15,17].
   Compare the complete ASS after each history operation.
8. Capture saved final state and require normal WM_CLOSE/native exit code zero.
   Forced cleanup is only failure containment.

The font operation changes only the size input; it does not click a font
family, weight, italic, or other variant control. Its oracle explicitly retains
the existing selected-line variant propagation: the active line is regular
italic, the second row has no explicit variant, and the third row's style is
bold with no explicit variant. Thus the final insertions are `{\fs41}` on the
active line, `{\fs41\i1}` on the second, and `{\fs41\b0\i1}` on the third.
That propagation is not changed by this scenario or the native-control refresh
fix. Font Face writes from the selection start without adding end-restoration
tags; unlike Toggle Bold, it does not append `\fs24`, `\i0`, or a similar tag
at the end of the selected BC/XY/uv range. The active [15,17] selection is the
original [8,10] shifted by the seven UTF-16 characters in `{\fs41}`.

While the font dialog is modal, preview checks inspect the actual active text,
selection, size and family controls. Complete multirow persisted oracles are
checked after Cancel, Accept, Undo, and Redo. Duplicate tags, stale text-control
state, caret drift and incomplete cancellation remain failures; no preview or
range assertion is relaxed to accommodate them.

Every HWND is checked against the application's PID and private desktop. Color
fields require unique observed Static labels inside RGB color group bounds,
native Edit class, actual row geometry and native parent. HTML/ASS must each
have one adjacent Edit; Alpha must have its hex/numeric Edit pair ordered
left-to-right, with the hex field directly parented by the chooser. No guessed
color IDs, fallback coordinates, UIA Invoke or new production hooks are used.
History actions first open the actual Edit popup through its observed mnemonic,
which triggers the existing menu-open dynamic-label refresh. The complete
refreshed menu inventory and popup PNG are captured. An exact expected label,
enabled state and matching observed command ID are required before finite
window-local keyboard navigation highlights and activates that exact item.
There are no stale-label probes or guessed command IDs.

Save As uses established file-picker UIA metadata to locate the actual filename
Edit and Save button, then operates on owned native HWNDs.
Window discovery and UIA preparation share one ten-second deadline. Each
readiness check confirms that the current unique Save As window is the same
observed owned HWND, then takes a fresh UIA descendant/metadata snapshot.
Missing controls or `ElementNotAvailableException` mean not ready yet; timeout
is failure. Duplicate controls, a replacement dialog, and all other errors fail
immediately. Filename entry and Save activation are performed once, after both
unique native handles are ready; the save operation or scenario is not retried.

Font Face is the unique observed native Button with that label; the actual
Select Font dialog must be owned by the main window. Its editable font
ComboBox is identified with read-only UIA metadata, and `GetComboBoxInfo`
provides its real native EDIT. The size field is uniquely the native Edit to
the right on the ComboBox's actual row, inside the labeled Font group's bounds.
There are no guessed font IDs, enumeration-order or coordinate fallbacks, or
UIA input actions. Size edits use `EM_SETSEL` and `EM_REPLACESEL`, with no
synthetic `EN_CHANGE` notification. Each preview has a separate completion JSON
and dialog/main-window PNG pair.

## Artifacts and limits

The native reload regression originated when
`c39d3c5d11e336b8ac3249f4f944de3f37ac00b4` connected the wxTextCtrl edit box
through `UpdateFields::SetValue`. The controller generalized by
`3d7b8c4d2ffaa58a4d9c30a4fb9746d23952e34b` skipped native selection writes
when its cached endpoints matched, even though SetValue had reset the control.
Repeated previews could therefore have the correct text and cached position
but an actual native caret at zero. The fix replays explicit selection requests
and restores the cached UTF-8 range after native text reload, converting against
the new text. It is shared by ordinary editing and both preview dialogs, not a
color-command workaround. SetInsertionPoint keeps its existing behavior because
native wxTextCtrl and STC differ in whether it collapses a selected range.

The new artifact directory contains fixture/profile/readiness evidence,
driver.log, actions.ndjson, result.json, observed-window inventories, actual
selection/color/font/input/result PNGs, native-editor text/range and font-preview
completion JSON, and complete actual/expected ASS pairs for accepted, cancelled,
undone and redone states.
Artifacts must come from the actual run: source compilation alone is not
behavioral proof. No executable/source hashes or local absolute-path literals
are checked into this driver, fixture or documentation.

Coverage is wx update_lines(toggle), PrepareColorEdit and Font Face multirow
ordinal mapping through native UI, including repeated live previews, Cancel,
and history selection restoration. Skia, font-family/candidate selection,
video quick-pick, differing-alpha propagation, complex grapheme clusters,
tag-interior anchors and rendering correctness are not covered.
