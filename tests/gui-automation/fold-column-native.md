# Native folding-column visibility E2E

Run from the repository root with the newly built wx `Aegisub` target and a
**new** artifact subdirectory:

```powershell
dotnet build tests/gui-automation/fold-column-native.cs
dotnet run --file tests/gui-automation/fold-column-native.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/fold-column-native
```

The file-based .NET 10 Windows driver reuses `PrivateDesktop`,
`ObservedWindowsCommand`, `ScreenCapture`, and `AutomationProtocol`. The real
wx host runs on an inactive private desktop. The driver does not switch the
input desktop, use global mouse/keyboard input, or access the clipboard.
Run exclusively from other GUI scenarios and heavy builds.

The driver has a **180-second** watchdog within `PrivateDesktop`'s **210-second**
supervisor. Each native synchronous request has a one-second timeout. Normal
GUI waits and normal process exit have ten-second limits; startup readiness has
a twenty-second limit. Timeout or forced termination is failure, not a normal
shutdown or evidence of persistence. Use a fresh artifact directory for each
run rather than reusing prior output.

Native popup HWNDs can disappear between window enumeration and class-name
inspection. The scenario treats only Win32 error `1400` with a confirmed
`IsWindow == false` as an already-destroyed window; all other class-name errors
still fail. This does not add retries or relax menu-state assertions.

## Scenario

The isolated profile deliberately omits `Subtitle/Grid/Show Folding Column`,
so the first actual header popup must show the default checked preference.
Two generated fixtures contain nine events, two styles, distinct actors,
layers, margins, effects, an interleaved comment, Unicode, comma-containing
text, override tags, Script Info sentinels, and unrelated linked ExtraData.
The grouped fixture has two separate three-event groups: the first is collapsed
and the second expanded. The otherwise equivalent plain fixture has no fold
boundaries.

The grid is discovered from its actual unique native `ScrollBar` child and its
wx parent. Header and row context menus are opened with window-local
`WM_CONTEXTMENU`. The driver reads the actual native popup's `HMENU`, item
positions, labels, enabled state and checkmarks. It requires **Folding column**
to be the first header-menu item. Changing visibility clicks that exact observed
item using window-local keyboard messages. Bounded Home/Down navigation must
highlight that exact observed item before Enter; it
does not manufacture a main-window command ID for the local wx menu.

The run checks these actual interactions and transitions:

1. Default-enabled mixed groups show the folding column. Hiding it must not
   dirty the subtitle document or alter the entire native Save As serialization.
2. With the column hidden, the existing row-context **Fold Groups** submenu
   remains present. Its actual **Collapse All Fold Groups** and **Expand All
   Fold Groups** items are enabled and operate on both saved groups.
3. Re-enabling the column does not alter any document field, and the column
   remains visible when all groups are expanded.
4. Native **File → Open Subtitles...** switches to the ungrouped fixture, which
   has no column even when the preference is checked. **Subtitle → Select All**
   followed by the original context-menu **Create Fold Group** creates a group
   spanning all nine events and shows the column immediately. Clearing that
   last group removes it. Switching back to the grouped fixture restores it.
5. A normal `WM_CLOSE` with exit code zero flushes the disabled Boolean. A fresh
   host using that same profile must display an unchecked header item and no
   column despite loading groups; switching to the plain file keeps it hidden.
6. Enabling the preference while the plain document has no groups must not
   reserve an empty column. A second bounded normal shutdown and a third actual
   host process must restore the checked preference and visible column when
   loading the grouped fixture.
7. In that third host, **Expand All Fold Groups** first expands both groups and
   the column remains visible. **Clear All Fold Groups** removes the last groups
   and immediately removes the column while leaving the independent preference
   checked. Actual Edit menu **Undo** and **Redo** restore and remove the groups
   and column together. A final Undo and Save As leave a clean document before
   normal exit.

File switching and history are separate phases: no file-open operation follows
the history phase's reverted commit head. This keeps the scenario's strict
clean-after-open precondition independent of the existing subtitle-controller
saved-revision bookkeeping after Undo. Every file switch still requires the
actual requested filename and a clean document; no dirty-state or fold/history
assertion is removed.

File dialog text entry targets the actual native filename Edit. Save As uses
its observed modern UIA Edit `1001`; Open uses the observed Edit `1148`, requires
its native ComboBox → ComboBoxEx32 filename parent chain, and checks adjacency
to the native filename label. Acceptance targets the actual Open/Save button
observed through UIA. Context-menu fold operations and
history operations select actual native popup items rather than dispatching
hard-coded IDs. File and selection commands use IDs read from the actual
main-window menus. No Automation macro or production folding algorithm is used
as an assertion oracle.

## Geometry and document evidence

The profile uses a distinctive grid-line color, `rgb(13,24,37)`, solely to make
the actual painted header boundaries objectively measurable. PNGs are captured
with `PrintWindow` and validated by `ScreenCapture`. The driver finds the first
six distinct vertical lines spanning five pixels near the top of the grid's
client header, records their coordinates and current native window DPI, and
requires at least five measurable edges.

The visible screenshot's first positive edge must be exactly **20 DIP** from
the client origin. The next five actual column edges must shift left by that
exact gutter width in the hidden screenshot. Subsequent screenshots must match
the established visible or hidden edge array for their state. This measurement
uses painted output, not the preference value or a production layout query.
It detects an empty reserved gutter, failure to re-layout when the first group
appears or last group disappears, and a gutter incorrectly tied to whether any
group is collapsed. It does not assert arrow glyph shape or exact row selection
border rasterization.

Native Save As outputs provide independent document evidence. The driver
associates each event's `{=id}` references with its serialized ASS ExtraData,
checks each expected pair of boundaries, exact collapse flags, matching group
identity, distinct group identities, and absence of unexpected linked markers.
All nine complete serialized event fields and text must match the initial
native writer baseline after stripping only ExtraData reference tags. The
unrelated ExtraData sentinel must remain linked to its original event. The two
visibility-only changes additionally compare **every saved ASS line** exactly,
including metadata, styles and fold data. Configuration is read only after
bounded normal exit and complements, rather than replaces, observed UI state
after restart.

## Artifacts and acceptance

`result.json` records the final result and each actual assertion. `driver.log`
and `actions.ndjson` preserve the interaction chain. Menu inventories record
actual item order, labels, enabled states and checkmarks; focused menu PNGs
show header and existing folding submenu UI. State-specific `*-grid.png` and
`*-geometry.json` record the painted grid, capture validation, client origin,
DPI and measured boundaries. The generated ASS fixtures and each native Save
As output preserve the complete document evidence. The isolated profile's
`config.json`, post-exit Boolean snapshots, launch readiness artifacts, and
private-desktop/process identity records allow the run to be audited.

This scenario uses English UI labels. Simplified Chinese **折叠栏** translation
and other locale layouts are not validated by this English scenario. No
success claim is made merely from compilation or an older artifact. Acceptance
requires a newly generated `passed: true`, geometry matching the observed
visibility state, correct saved fold/document data, and all three bounded
normal exits. If native menu input or layout matching fails, inspect the saved
actual menu inventory and screenshot and repair the interaction or layout
assumption; do not relax assertions, add unbounded retries, or count cleanup
termination as persistence evidence.

The complete English scenario was verified against the actual wx
`RelWithDebInfo` application on 2026-10-10, with **202/202 assertions passed**,
driver exit code **0**, and all three bounded normal host exits returning **0**.
The fresh run artifact directory was
`build-dir/artifacts/fold-column-native-20261010-r5`. At native window DPI 120,
the visible header boundaries were `25,44,61,134,207,240` pixels and the hidden
boundaries `19,36,109,182,215,265`: the 25-pixel gutter equals 20 DIP, and the
next five corresponding columns shift by exactly 25 pixels. Initial failed
attempts remain separate artifacts; they are not counted as passing runs.
