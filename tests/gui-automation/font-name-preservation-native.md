# Native ASS font-name preservation E2E

Run from the repository root with the newly built wx application and a **new**
artifact subdirectory:

```powershell
dotnet run --file tests/gui-automation/font-name-preservation-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/font-name-preservation-native
```

The file-based .NET 10 Windows scenario uses the existing `PrivateDesktop`,
`ObservedWindowsCommand`, and `AutomationProtocol` driver. The
application and helper run on an inactive private desktop; the scenario
does not switch the input desktop, inject global input, or use the clipboard.
Do not run concurrently with other GUI scenarios or shared builds.

There is one **180-second total watchdog**, inside the driver's **210-second
supervisor**. Native messages time out after one second, ordinary GUI waits
after ten seconds, and each startup ready artifact after twenty seconds. A
watchdog, forced termination, missing installed font, ambiguous control, or
unexpected complete-file difference is failure. There are no unbounded retries.

Top-level dialog-title discovery and top-level inventory captions use native
`GetWindowTextLengthW` / `GetWindowTextW` after the private desktop owner check.
For these cross-process window captions Windows reads its cached title instead
of sending synchronous `WM_GETTEXT` to a GUI thread that may still be creating
a font dialog. A top-level window destroyed during enumeration contributes an
empty caption; a live-window ownership or caption error remains failure.
Actual child-control values still use the driver's one-second bounded native
text request, and dialog discovery still has the same ten-second total wait.

## Environment and fixture

The scenario requires the real installed `Microsoft YaHei` / `微软雅黑` alias
pair. It does not substitute an English-only font or skip the alias checks when
the pair is unavailable. On a Chinese Windows installation, the true preference
must expose `微软雅黑` and the false preference `Microsoft YaHei`. Each candidate
list must contain the preferred bare name and omit the other bare alias as a
duplicate. The complete observed candidate lists are saved for inspection.

Two separate profiles run the entire scenario with **Prefer Localized Family
Names = true** and **false**, respectively. Both use the custom selectors,
contains matching and automatic list expansion; compact vertical-list mode is
disabled so the existing `@` text is tested directly. Profiles disable updates,
autosave, backup, and linked-file loading. The flushed configuration is checked
after a bounded normal exit to prove the preference was not changed by editing.

The helper's scenario thread explicitly uses PerMonitorV2 DPI awareness to
match the wx host's manifest. This is a thread-local setting, not a change to
the user's desktop or another process. Previous/current context identity and
the PerMonitorV2 equality check are saved in `dpi-context.json`.

UIA is used only to read values and control metadata. Pattern availability does
not imply that an action such as `Invoke`, `Expand`, or `SelectionItem.Select`
works on an inactive desktop; all user actions use observed native HWNDs and
window-local messages instead.

The generated ASS has six styles: English and localized YaHei, the two `@`
forms, an uninstalled Unicode name, and Arial. Distinct colors, scale, spacing,
angle, outline, shadow and margins provide nonfont sentinels. Four dialogue rows
and an interleaved comment contain explicit `\fn`, `\fs`, border/shadow/color,
position, italic/underline and reset tags, Unicode, comma-containing text,
different layers, actors, effects and margins. Script Info and Project Garbage
also contain sentinels.

The three selected dialogue rows use `Microsoft YaHei`, `微软雅黑`, and `Arial`
respectively, all with size 24 and the same regular/nonitalic/nonunderline
state. A comment and a fourth vertical-font dialogue are never selected for
font edits and must remain identical. The scenario is intentionally not a
minimal one-style/one-line proof.

## Actual GUI operations

### Style Editor

The Styles Manager is opened through the actual **Subtitle → Styles Manager...**
native menu. Its command ID is discovered from that menu and dispatched to the
owning main window using `ObservedWindowsCommand`, not a hard-coded ID or a
UIA toolbar Invoke that may depend on the input desktop. The current-script ListBox and its
**Edit** / **Copy** buttons are distinguished from storage controls by the
actual **Current script** group bounds. The native ListBox's actual item texts
are read and saved, and the requested style must match exactly one item. Its
real item rectangle receives window-local mouse-down/mouse-up; native selection
queries must then show exactly one selection at the requested item before the
actual Edit or Copy button is invoked. There is no UIA selection action or
direct model mutation.

For each of the six styles, the actual editable font ComboBox must show the
original ASS name exactly in both its native EDIT child and UIA `ValuePattern`.
Pressing the actual **OK**, without changing any field, followed by native
**Save Subtitles as...**, must preserve the entire ASS. In the Default style's
no-op case, expanding and collapsing the real dropdown must preserve its raw
English input even when the preferred candidate spelling is localized.

Further operations, each followed by a separate full-file comparison, are:

1. Change only Default's size to 29; its original English font remains.
2. Copy the unknown-name style through the actual **Copy** command. The new
   `Unknown - Copy` must be an exact style copy apart from its new name; all
   existing styles and events remain unchanged.
3. Copy Default while its font is still the known English `Microsoft YaHei`
   alias and its updated size is 29. Its Copy dialog must show both values;
   `Default - Copy` must be identical to its source apart from the new name,
   including when the candidate preference is localized.
4. Explicitly choose the profile's preferred YaHei spelling from the real font
   candidate list. The chosen candidate spelling is the written `Fontname`.
5. Type `Typed Style Face café 世界` into the font edit using window-local
   `WM_CHAR` keystrokes and change size to 30. The exact typed text is written,
   without replacing it with a catalog spelling.

### Select Font and heterogeneous selected lines

The actual **Subtitle → Select Lines...** dialog selects dialogues by their
fixture actor prefix. The scenario verifies and sets **Matches**, **Contains**,
**Actor**, **Set selection**, dialogues enabled and comments disabled, using
the actual native radio/checkbox controls. No production selection API is added
or used.

The actual **Font Face** bitmap button opens **Select Font**. It is identified
uniquely from the main window's native Button class and exact label. Its real
control ID and parent are observed at runtime; `ObservedWindowsCommand` posts
the button's window-local command to that parent. The command ID is recorded
in the action log, without treating UIA's `6000` identifier as a native ID or
relying on UIA Invoke to click an inactive-desktop button. The writable font ComboBox must support `ValuePattern` and
`ExpandCollapsePattern`. Size is identified uniquely as the native Edit on the
font row to the right of the combo **inside the actual labeled Font group's
bounds**, not by enumeration order or the initial number. The Font group must
also contain the font ComboBox. In Style Editor this excludes the unrelated
Scale X / Scale Y edits in the neighboring Miscellaneous group even when they
share the same screen row; both dialogs must still yield exactly one size Edit.

The following operations must pass in both profiles:

1. Select all three heterogeneous dialogue rows. The active line's English
   explicit `\fn` is displayed unchanged. Expanding/collapsing candidates and
   confirming without changes must preserve the complete ASS, including each
   other selected row's own alias/family.
2. Select only the localized row, verify exact `微软雅黑` display, and confirm
   without editing. The only allowed saved change is the deliberate Active
   Line metadata change from row 0 to row 1. Select the first row and then the
   three-row group again; expected Active Line returns to row 0.
3. Set size to 37. Wait until the actual active subtitle edit contains the
   live-preview `\fs37` and still contains the raw English `\fn`. Cancel must
   restore the exact active edit text and the entire saved ASS.
4. Reopen and set only size to 41. The three selected rows' existing `\fs24`
   tags must become `\fs41`; each original `\fn` alias/family remains exact.
   No comment, unselected event, style, timestamp, or other field may change.
5. Keep all three dialogues selected and explicitly choose the real preferred
   YaHei candidate. All three existing `\fn` values must become the candidate's
   exact spelling; size 41 and every other field remain unchanged. In the
   English-preferred profile the active line already displays `Microsoft YaHei`:
   explicitly choosing that same candidate must still replace the other lines'
   localized alias and Arial family. This distinguishes a real selection event
   from opening/confirming without choosing a font.
6. Select only the first dialogue and explicitly choose its current preferred
   YaHei candidate again. Its exact spelling remains in the active edit and
   confirming must preserve the complete ASS, including both unselected lines.
7. Type `Typed Line Face 世界` and set size to 43. Only that first dialogue's
   existing `\fn` and `\fs` values may change; the exact typed name survives.

Explicit candidate choice uses window-local mouse-down/mouse-up on the actual
native dropdown ListBox item after locating its real text and item rectangle.
It therefore traverses the real selection notification path; writing text into
the edit is never used as evidence of candidate selection. Manual typing uses
the combo's actual EDIT child and remains separate from list selection.
Dropdown opening and closing use `CB_SHOWDROPDOWN` on the actual observed combo
HWND, followed by `CB_GETDROPPEDSTATE` verification; UIA Expand/Collapse actions
are not used. UIA font value and pattern metadata remain independent read-only
checks, and expanding/collapsing without selection must still preserve the exact
original font text.

Each candidate choice first only observes the opening-side dropdown layout,
client area and caret until they are stable for 200 milliseconds, without
scrolling or choosing an item. It then scrolls the actual dropdown **once** and
observes its top index, item rectangle and `LB_ITEMFROMPOINT` result. Both
stability waits share one existing ten-second preparation deadline; there is
no second ten-second allowance or repeated scroll. The item must be wholly in the client area, its click point
must hit exactly the requested index, and the layout must remain unchanged for
200 milliseconds before a single mouse-down/up action. This allows the normal
asynchronous dropdown opening/highlight settle to finish without retrying a
wrong selection. The hit and geometry are checked again immediately before
that one action. `-selection-before.json` and `-selection-after.json` retain the
wanted label/index, top/caret indexes, item/client rectangles, click point,
raw/decoded hit index, outside flag, dropped state, window DPI and actual
selected index/label. Every read-only layout observation updates
`-selection-layout-latest.json`, and a preparation timeout also writes
`-selection-layout-timeout.json` containing the last observation and failure.
Thus a scroll overwritten by asynchronous opening, a rectangle extending into
a boundary, or unexpected ComboLBox hit-test behavior leaves specific evidence
instead of an unexplained stable-wait failure. The complete-rectangle visibility
and actual point-hit requirements remain mandatory. A correctly targeted
click that nevertheless selects a neighbor remains failure requiring event
analysis; the scenario never clicks repeatedly until the expected name appears.

## Full-file oracle and artifacts

A baseline native Save As accounts for ordinary ASS writer serialization.
Baseline style fonts must still equal the generated original names. Expected
files are derived independently using explicit style field replacement, exact
existing override-value replacement, and appending the copied style with only
its name changed. The deliberate Active Line transitions are represented in
the expected Project Garbage section, including omission of row 0 according
to normal ASS serialization. Metadata is not stripped from actual files,
filtered out of comparisons, or replaced with a hash. All saved lines must
match expected lines exactly and in order.

Every checkpoint writes both its full saved `.ass` and full `-expected.ass`
before reporting its comparison. The result is based on persisted ASS, not
solely the dialog text or preview. The size-only mixed-font checkpoint also
reads each of the three names back from the actual saved ASS.

Artifacts are retained on success and failure:

- `result.json`: each profile, phase, assertion and final status. During a run
  it explicitly reports that acceptance is not yet complete.
- `driver.log` and `actions.ndjson`: the actual interaction chain.
- Separate `localized` and `english` profiles, fixtures, initial preference
  snapshots, launch `ready.json`, and flushed configurations.
- Native control inventories with handles, labels, classes, bounds, enabled
  state and observed IDs for every opened Style Editor, Select Font and Select
  Lines dialog; real candidate-list text inventories and before/after click-hit
  evidence for every explicit candidate selection.
- Focused PNGs for each font operation, selection setup, baseline and failure.
- Baseline and checkpoint complete ASS/expected pairs. On ordinary failure,
  the scenario first records the failing modal UI, then attempts to cancel it
  and save `failure-current.ass` beside `failure-current-expected.ass` without
  replacing the original error or treating that cleanup as a passed check.
- Driver/launcher process and private-desktop identity evidence from the
  shared driver.

Acceptance requires a freshly executed `passed: true`, exact complete-ASS
checks in **both** profiles, and two normal `WM_CLOSE` exits within ten seconds
with native exit code zero. Compilation, stale artifacts, or forced cleanup
are not substitutes for running this E2E. This scenario covers spelling
preservation and deliberate font edits, not compact-vertical toggle behavior,
font installation changes, or native Windows font-dialog variant behavior.
