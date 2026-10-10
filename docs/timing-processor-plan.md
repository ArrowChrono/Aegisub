# Timing Processor calculation boundary

`aegisub::PrepareTimingProcessor` prepares a detached sequence of start/end
times. It neither reads options nor changes subtitles. A host publishes the
complete result only after preparation succeeds.

## Ownership

- The host selects eligible lines, rejects negative durations and supplies its
  existing order, including the order of equal-start lines. The calculation
  does not sort, filter or retain dialogue pointers.
- The host snapshots timecodes and sorted keyframes, including its existing
  video-final-frame policy. The calculation does not load media or repair
  reference data.
- Only enabled parameter groups are validated. Enabled keyframe snapping needs
  loaded timecodes and nonempty keyframes. Invalid parameters throw
  `std::invalid_argument`; lead overflow throws `std::overflow_error`.
- Preferences, error translation, undo, document revision and no-op/commit
  policy remain host responsibilities. wx saves valid transferred preferences
  before processing, and commits successful nonempty processing even if times
  are unchanged. Failed wx input transfer leaves the original dialog open and
  does not save preferences or process subtitles.

## Preserved arithmetic

This extraction is not a timing-algorithm repair. It preserves lead-in,
lead-out, adjacency and keyframe order; centisecond collision comparison and
lead conversion; raw-millisecond adjacency; signed `distance * bias + 0.5`
conversion; previous-keyframe tie preference; and end-keyframe-minus-one
snapping. Both frame indices are computed before changing a line's start.
Lead limits are applied before checking the integer range. Construction of
`agi::Time` retains its existing negative-time clamp.

Equal-frame snapping retains the original raw time. ASS writer rounding is a
separate boundary, not an extra conversion inside preparation. Existing Exact
and frame-projection behavior is outside this work.

## Verification

The seven test-first cases in `tests/tests/timing_processor_plan.cpp` cover
collision limits, raw signed adjacency and pipeline order, supplied equal-start
order, CFR keyframe ties and endpoint thresholds, VFR/equal-frame preservation,
late overflow with unchanged input, and invalid enabled groups.

The representative private-desktop scenario in
`tests/gui-automation/timing-calculation-native.md` checks native Cancel,
eligibility, invalid input and unchanged preferences/document, then verifies
every event against literal expected output after combined leads and adjacency.
It preserves comments and non-time fields and requires normal process exit.
Keyframe arithmetic is covered by the calculation cases; this native fixture
does not claim to test video/keyframe loading.

Run the full existing C++ suite and this native scenario from the repository
root. Write fresh results under the build directory; retained screenshots and
ASS outputs are evidence for that run, not substitutes for rerunning changed
behavior.
