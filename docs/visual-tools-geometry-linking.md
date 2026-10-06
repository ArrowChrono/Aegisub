# Visual-tool geometry linking (phase one)

## User contract

The **Geometry Linking** toolbar toggle and `video/tool/geometry_linking`
command enable an optional shared state. It starts **off** for a video display,
and survives switches between Drag, Scale, Rotate Z, and Rotate XY. No existing
Ctrl/Alt/Shift mouse gesture is reassigned. Clip tools continue to edit clips
only; the toggle does not make a clip edit drive subtitle text.
During a captured gesture, the toggle remains enabled and retains its selected
appearance. Invoking its command during the gesture does not change the linking
mode; the gesture must be released before the mode can be changed.

The tools retain their existing main-parameter semantics. This is not a
multi-line transform around a common center:

- Dragging a static position translates its existing origin and clip/iclip by
  the same script-space delta.
- **Drag double-click** is the complete-trajectory translation entry point.
  For a move it translates both endpoints and the existing origin/clip, retaining
  the move times. A single move-endpoint edit cannot generally keep an attached
  clip correct for the complete trajectory. That operation is rejected with a
  specific message; use double-click translation or turn linking off instead.
  Without attached geometry, endpoint edits retain their existing time behavior.
- Static Scale and Rotate Z preserve the ordinary tool writes, including the
  same absolute values applied to selected lines. Each line's clip follows its
  own before/after text transform. Positions and origins are not transformed
  again as clip side effects.
- Scale normalization retains each selected line's own ratio. Existing Scale
  nudges, Rotate Z horizontal/vertical nudges, origin handles, and Drag
  double-click use the same linked-edit contract.
- Changing an origin alone moves the clip by the actual change of the text
  transform, not by the origin-handle displacement. With no rotation, changing
  an origin need not move the clip at all.

Linking follows the static ASS geometry pose; it does not re-solve automatic
wrapping or collision-avoidance layout. Integer implicit-anchor coverage below
is not a claim that arbitrary unpositioned complex layouts are supported.

The static affine model accounts for Z rotation, nonuniform scale, origin
relative to position, static shear, and PlayRes/LayoutRes pixel aspect. For
example, with square pixels, `pos(300,200)`, `org(260,180)`, `frz90`, and
`fscx100/fscy100`, the displayed scaling anchor is `(280,140)`. Changing scales
to `200/50` maps `clip(260,100,340,180)` to `clip(270,60,310,220)` while
leaving position and origin unchanged.

Nonempty rectangles keep rectangle tags for positive axis-aligned mappings whose
output coordinates are integers within numerical tolerance. Empty rectangular
clips stay empty. Other mappings use vector clips, including some
axis-aligned rotations such as 90 or 180 degrees; conversion alongside other
clip tags is rejected if it would change renderer precedence. Vector clips
preserve polarity, drawing-scale geometry, contours, cubic and spline commands.
Output vector coordinates are normalized to scale 6 with integer coordinates;
the maximum component rounding error is `1/64` of a script pixel. Empty static
clips remain empty. Color, alpha, font size/spacing, border, shadow, blur,
drawing scale/body, and baseline offset are not opportunistically scaled.

## Explicit unsupported boundaries

Capability restrictions are checked only when an operation actually needs to
link attached geometry. Merely enabling the toggle does not block ordinary
Rotate XY edits without clips, or perspective origin edits with no attached
clip. Whole-position/origin translation can handle static perspective poses.

Clip linking for perspective changes, or for existing nonzero frx/fry during
nontranslation edits, is unsupported in this phase. Geometry animation,
including clips appearing only inside `t`, moving lines during a static
scale/rotation edit, different visible runs without a single static pose,
ambiguous duplicate pose tags/style resets, mixed pos/move source tags,
Banner/Scroll effects, and vertical `@` text-font poses during nontranslation
edits are rejected. Pure drawing runs do not acquire text-font restrictions. A trailing drawing-mode reset without a
subsequent visible run is not a mixed-run transform. Implicit positions with
integer default anchors can be linked without materializing a pos tag during
Scale. A fractional default center, unknown default style, or style reset/
ambiguous alignment that prevents proving the default anchor is rejected when
attached geometry needs linking; set an explicit pos first or turn linking off.
The existing tools' default-position getter and their ordinary off-mode writes
are unchanged.

Malformed geometry, unsupported drawing coordinates/scales, non-positive scales,
singular shear, out-of-range results, and incompatible selected-feature edits
are explicit failures rather than approximations. Ordinary decimal drawing
coordinates are accepted when libass nearest rounding and VSFilter truncation
produce the same source fixed-point value, and the linked transform uses that
common value. Renderer-divergent inputs are rejected rather than guessed. The operation does not split
events, freeze animation, or partially rewrite the selected set.

## Interaction, failure and undo

Each gesture snapshots source text and pose. Drawing coordinates are prepared
once per gesture, and each event recalculates from that source rather than the
previous preview. All main and attached edits are planned before mutation.
On a rejected update **all selected lines retain their last valid preview**.
An immediate status message is followed by a complete warning after release or
cancellation; keyboard/normalization warnings are deferred until their mutation
method returns. The warning provides the reason and does not run inside a
captured drag's mutation loop.

One Undo restores the complete main/attached change for every edited selected
line, and Redo restores that edit. No-motion clicks preserve the original ASS
bytes. Returning a gesture to its starting point also restores the original
text; an Undo history item may remain if intermediate previews were already
committed, consistently with the existing tools' undo-coalescing behavior.

## Reproducible Windows verification

Run all commands from the repository root, in a Visual Studio developer shell
with CMake, .NET 10, and FFmpeg available. Preserve an existing build directory's
configuration. The commands below use the usual RelWithDebInfo x64 build.
Geometry cases always launch a separate, non-input Win32 desktop in WinSta0;
the private driver and both actual/expected GUI hosts use that same explicit
desktop. Creation or identity failures abort the run, with no fallback to the
user's input desktop. The geometry path does not send global input, request
foreground focus, switch desktops, or read/write the shared clipboard.

```powershell
cmake --build build-dir --config RelWithDebInfo --target Aegisub test-aegisub --parallel
New-Item build-dir/artifacts/geometry-linking -ItemType Directory -Force | Out-Null
ffmpeg -v error -f lavfi -i 'color=c=0x16202e:s=640x480:r=24:d=10' `
  -c:v libx264 -pix_fmt yuv420p -y build-dir/artifacts/geometry-linking/video.mp4
dotnet build tests/gui-automation/visual-tool-drag-uia.cs `
  -o build-dir/artifacts/geometry-linking/driver --nologo
```

Deploy the usual libass runtime next to the executable. For CSRI, use an
existing compatible x64 xy-VSFilter DLL in the executable's `csri` directory.
The driver checks the actual selected provider; absent or unavailable renderers
are not a passing result. The repository's renderer test gate accepts an
explicit DLL and exact version:

```powershell
$env:AEGISUB_XY_VSF_DLL = (Resolve-Path build-dir/RelWithDebInfo/csri/VSFilter.dll).Path
$env:AEGISUB_XY_VSF_REQUIRED = '1'
$env:AEGISUB_XY_VSF_VERSION = '3.2.0.810'
cmake --build build-dir --config RelWithDebInfo --target test-aegisub --parallel
```

Use a new artifact directory for each run; directories are not overwritten.
The complete E2E matrix is:

```powershell
$cases = @(
  'translate','off','move','scaled-curve','scale','scale-shear','normalize',
  'rotate','origin','origin-xy','nudge-origin','reject-animation','reject-mixed',
  'reject-xy','xy-no-clip','empty','reject-animated-only','reject-endpoint',
  'endpoint-no-geometry','mixed-static-endpoint','reject-perspective-scale','drawing-tail','scale-layout',
  'reject-vertical-font','reject-reset','reject-multiclip','reject-zero-scale',
  'reject-malformed','no-motion','return-to-start','clip-only','scale-nudge','scale-curve',
  'decimal-curve','reject-decimal-curve','reject-effect','reject-pos-move',
  'reject-duplicate-xy','drawing-tail-vertical','last-valid-preview','reject-animated-spaced',
  'implicit-integer','reject-implicit-fractional','reject-implicit-reset','reject-malformed-position',
  'implicit-no-geometry'
)
foreach ($provider in @('libass','CSRI/pf-xy-vsfilter_textsub')) {
  $renderer = if ($provider -eq 'libass') { 'libass' } else { 'xy-vsfilter' }
  foreach ($case in $cases) {
    & build-dir/artifacts/geometry-linking/driver/visual-tool-drag-uia.exe `
      --exe build-dir/RelWithDebInfo/Aegisub.exe `
      --video build-dir/artifacts/geometry-linking/video.mp4 `
      --artifacts "build-dir/artifacts/geometry-final/$renderer/$case" `
      --input-contract "geometry-$case" --subtitle-provider $provider `
      --timeout-seconds 20
    if ($LASTEXITCODE -ne 0) { throw "Failed $renderer/$case" }
  }
}
```

Each case has a finite 180-second total watchdog and a 210-second launcher
limit, in addition to bounded UIA, startup, dialog and capture waits. Native
desktop/window-station identity and owned PID/start-time checks guard each
targeted GUI message; the original input desktop must remain unchanged.
It generates 48 events, selects two genuinely
different visible lines, and checks unrelated tags/styles/text and the other
46 dialogue events. Main-tag assertions use a 0.001 numeric tolerance for the existing three-decimal
serialization, while clip-coordinate assertions use a 0.016 script-pixel
component tolerance for normalized scale-6 integer output. These geometric
oracles are distinct from rendered-pixel evidence. Scale cases include rotated text,
nonuniform scale and distinct per-line old scales; drawing cases retain
multiple contours, cubic and spline commands with scaled clip/iclip input.
The mixed static-position/bare-move case clicks the selected move start at
frame zero: the static line's origin and clip translate with its position,
while the bare move keeps ordinary endpoint retiming and leaves its end/time
unchanged.

Artifacts include saved `before.ass`, `after.ass`, `undo.ass`, `redo.ass`,
rendered `before-frame.png`/`after-frame.png` obtained through the real
`video/frame/save` command and its native save dialog, precise HWND window
screenshots, and `geometry-result.json`. Every frame path must be a new file
inside its case artifacts. Rejection cases save
`rejected.ass`, the complete visible warning text, and a warning screenshot.
Each rejection also checks the expected semantic reason, not just a generic
failure or atomicity message.
The `last-valid-preview` case first independently verifies and saves a completed
legal reference gesture, then restores the original events. A second gesture
makes the same valid two-line preview before an invalid zero-scale update,
without using UIA or Save while captured. It verifies that the invalid update
retains the independently checked preview atomically, and checks Undo/Redo
against the original and last-valid documents. The `scale`
case additionally renders an independently authored fixed
`expected.ass` and checks exact renderer pixels against `expected-frame.png`;
its `pixel-oracle.json` records the result. Actual and expected pixel-oracle
frames must contain non-background image content; two empty or black frames
are not a passing oracle. Window screenshots document UI
state; they are not substituted for frame/geometry correctness evidence.

The isolated profile copies the default menu and adds a test menu exposing
registered Normalize, Nudge, Save, Undo, Redo and Save Frame commands. The
driver observes their runtime native menu IDs and posts each command once to
the owned private host; it neither invokes global shortcuts nor changes the
ASS model directly. UIA reads only that host's fresh toolbar targets/states.
Each target's
observed center is validated against its native toolbar client rectangle and
host process, then receives one native click. Select Visible, initial tool
entry, tool switching, and the Geometry Linking toggle use this same route;
there are no action retries. Fresh radio/toggle and two-line-selection assertions
remain mandatory. Native toolbar actuation avoids an unreliable MSW UIA
DefaultAction/TogglePattern dispatch while preserving real visible UI entry.
Rejection warnings are found through private-desktop, PID-limited native window
enumeration, not the disabled main window's accessibility tree. The driver
reads real Static messages where available. For the observed DirectUI TaskDialog
structure without Static text, it reads Text elements from that exact verified
dialog HWND's UIA provider only. The actual route and text are recorded, and the
observed native OK Button HWND receives one posted BM_CLICK so its own window
procedure sends the notification; no dynamic TaskDialog control ID is guessed,
and no UIA focus or invocation is used.
Native save-dialog captions must be recognized unambiguously; unsupported
captions fail rather than selecting a button by position. Desktop, toolbar,
menu-command, dialog and exported-frame evidence are saved as JSON/NDJSON.
Clip-only checks that editing clip geometry does not change text
position/origin/pose while linking is enabled.

The `translate`, `off`, `scale`, `origin`, `origin-xy`, and `clip-only` cases
also check the Geometry Linking toolbar button before press, while pressed,
after a held motion, after one real linking command while held, and after
release. Before capture, the driver observes the button's native command ID
from UIA and verifies it against its native toolbar. While captured it reads
only `TB_GETSTATE`, not UIA or Save, and checks that the canvas keeps capture.
Each phase waits 200 ms for ordinary idle UI updates and saves a
`link-<phase>-window.png` screenshot and a record in
`link-state-evidence.ndjson`. The button must remain enabled and its checked
bit must equal the intended on/off mode throughout. The one linking command
uses the observed test-menu ID and must not change that mode, including the
off case. These checks retain the existing two-line geometry, unrelated-event,
and Undo/Redo oracles; screenshots do not replace those assertions.

The older non-geometry driver scenarios are not covered by this private-desktop
launcher and must only be used in a separately authorized interactive test
session. For additional unchanged-input coverage there, run the existing `drag-pos`,
`drag-move-start`, `drag-move-end`, `drag-org`, `hold-scale`, `hold-rotate-z`,
`hold-rotate-xy`, `rotate-z-origin`, and `rotate-xy-origin` contracts described in
[visual-tool input contracts](../tests/gui-automation/visual-tool-input-contracts.md).
