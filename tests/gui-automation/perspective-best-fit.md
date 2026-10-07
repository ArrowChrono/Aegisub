# Perspective Best Fit GUI end-to-end check

Run from the repository root on Windows with .NET 10. The executable must include
GUI automation host support. The driver creates a new private desktop and never
switches the interactive input desktop. It uses an isolated Aegisub profile and
only closes processes that it created.

```powershell
New-Item -ItemType Directory -Force build-dir/artifacts/perspective-best-fit | Out-Null
ffmpeg -hide_banner -loglevel error -f lavfi -i color=c=0x30343b:s=640x480:r=24 -t 12 -pix_fmt yuv420p build-dir/artifacts/perspective-best-fit/video.mp4
dotnet run --file tests/gui-automation/perspective-best-fit-uia.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/perspective-best-fit/video.mp4 build-dir/artifacts/perspective-best-fit/run
```

The run directory must not already contain files. Use a fresh directory name for
a repeated run; the driver does not erase previous evidence. The whole driver has
a 180-second watchdog, the private-desktop launcher has a 210-second deadline,
and individual waits and native messages are bounded. Timeout is a failure.

## Covered behavior

The input contains two styles, a multiline active subtitle with existing scale,
shear, rotation, position, origin, colour, alpha, border and shadow, and an
unselected event whose text and metadata must remain unchanged. Mouse gestures
create and edit a convex target with corners `(140,150)`, `(510,180)`, `(465,325)`,
`(180,285)` in a 640×480 video canvas.

- Best Fit defaults off when its key is absent from the isolated configuration.
- Toolbar and Preferences changes synchronize in both directions; the checked
  state survives a clean restart without overwriting ignored settings.
- Best Fit produces different tags from the constrained reduced mode and can
  write perspective axes despite a saved Fax + Frz Only setting. Its output
  contains exactly one of each `pos`, `fscx`, `fscy`, `fax`, `fay`, `frx`, `fry`,
  and `frz`, including zero-valued fields. At least one generated numeric field
  uses the requested four decimal places instead of shortened digits.
- With Best Fit enabled, changing Fax + Frz Only and Shape Tolerance does not
  change output for the same source and target.
- Fit Text off preserves both original scales.
- Two decimal places limits every generated geometry tag; zero decimal places
  still permits Apply and writes only integer geometry. The zero-decimal source
  uses `fax0`, since the multiline source shear is a preserved semantic constraint
  rather than a freely generated value.
- A vertical `@Microsoft YaHei` source with a long Chinese string, inherited
  style angle `12.34`, and no explicit rotation tag remains applicable with
  Best Fit and zero decimals. The style angle is unchanged and no rounded
  `fr`/`frz` override is introduced.
- Text, unrelated override tags, event metadata, unselected dialogue and style
  definitions remain unchanged. Undo restores the original active dialogue.

## Evidence and limits

`result.json` contains every behavioral assertion and the drawn target.
`gestures.ndjson` records each actual mouse gesture, native button IDs/states,
and live help text; every gesture must leave a valid, applicable target before
the next gesture begins. Native controls with negative wx IDs are queried through
public toolbar/tooltip control messages because Windows UIA omits their names. Each
output mode has a saved ASS file and window screenshots, including the target
before Apply. `input.ass` is the untouched input; private-desktop identity records
and the driver log make process ownership and execution reproducible. Profile and
host logs are generated runtime evidence and may contain machine-local paths;
they are not source fixtures and should not be committed.

`best-fit-frame.png` and `vertical-zero-decimals-frame.png` are actual rendered
640×480 subtitle frames exported with Aegisub's **Save Frame** command, not
`PrintWindow` captures. Export waits for the complete PNG IEND chunk and verifies
dimensions and non-black image content.

The ASS comparisons verify the public UI-to-solver-to-serialization path, not a
copy of the solver algorithm. Numerical geometric fit quality should additionally
be checked by projecting the saved ASS against the recorded target. A private
OpenGL canvas may not be present in a `PrintWindow` screenshot; window captures
alone must not be claimed as rendered subtitle pixel verification.

## Recorded verification

The final run used the existing 640×480 video fixture and the RelWithDebInfo
executable, with no foreground desktop input:

```powershell
dotnet run --file tests/gui-automation/perspective-best-fit-uia.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/geometry-linking/video.mp4 build-dir/artifacts/perspective-best-fit/final
```

Result: exit code 0, **93 checks passed**, approximately **30.19 seconds** from
private driver startup to `result.json` publication. The pure-colour video in the
first command block is an independently regenerable replacement for this existing
fixture. Use a new run directory when repeating either command.

`--inspect` may be appended for a bounded tool-selection-only diagnostic. It
writes `probe.json` rather than claiming that the complete behavioral suite ran.
The suite does not directly exercise held-edge selection or independently prove
global minimum geometric error. Saved ASS files and the recorded target support
separate numerical projection checks; neither an ASS difference nor a window
screenshot is such a proof.
