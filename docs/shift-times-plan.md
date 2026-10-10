# Shift Times detached preparation

`aegisub::PrepareShiftTimes` consumes an ordered sequence of start/end times
and selection flags, a numeric directed offset, scope/field settings and actual
timecodes. It returns row timing edits and the original selected blocks. It
does not mutate input or retain document pointers.

## Boundary

Supply the complete event sequence in file/UI row order, including comments.
Snapshot indices correspond to contiguous zero-based UI rows. Output edit rows
are indices into that same sequence, not persistent dialogue identities or a
new chronological order. Selected blocks are one-based inclusive ranges of
the original selection, independent of which rows the requested scope affects.

- Mode 0 affects all rows, mode 1 only selected rows, mode 2 everything from the
  first selected row onward. Without selection, modes 1 and 2 prepare no edits.
- Fields 0 affects both endpoints, 1 start only, 2 end only. Inverted ranges are
  allowed; no range normalization belongs to this operation.
- `amount` has already had direction applied. Parsing, signed frame input,
  backward INT_MIN rejection and the original history amount remain with hosts.
- Time-zero returns an empty plan. Frame-zero still converts START/END and may
  normalize times. Hosts retain their existing early-close and commit policies;
  an empty edit vector is not a new generic no-op rule.

Only shifted endpoints undergo the existing `agi::Time` integer conversion to
centiseconds, once. Untouched endpoints remain raw `agi::Time` copies. Output
construction clamps representable negative time to zero without a second
rounding pass. For example, start-only +7 ms on raw 105/205 yields 117/205.

Frame conversion uses explicit checked Framerate methods. Offset addition is
int64, but both shifted frame numbers and output times must fit int. Wide
Framerate internals preserve their stored v1 phase and existing midpoint rules.
No new Exact, rounding or average-FPS mapping is introduced.

Invalid mode/fields or missing frame timecodes throw `std::invalid_argument`.
Out-of-range shifts throw `std::overflow_error`; checked conversion failures
propagate. The complete local plan must succeed before any endpoint is
published, so a later failure cannot publish earlier candidates.

## Host responsibilities

wx owns event pointers, native input, direction, preferences, error translation
and Commit -> history save -> close. Preferences still persist in the dialog
destructor, including Cancel. The synchronous timecode update switches wx to
time mode when timecodes become unavailable; helper parameter validation is not
an alternate native input policy.

Gac owns guards, parsing, affected identities, bookmark/history mutation and
`RecordChange`. User-facing error rendering remains adapter-owned. History I/O
failure after successful subtitle publication remains a separate warning,
without rollback. There is no text-to-number-to-text loop inside preparation.

## Acceptance

Six necessary tests in `tests/tests/shift_times_plan.cpp` were written before
implementation. They cover raw partial endpoints, clamping, disjoint blocks in
all scopes, time-zero/frame-zero, literal CFR/VFR and large representable frame
shifts, later failure without input mutation, and invalid parameters/reference.
Raw precision needs calculation-level coverage because ASS serialization can
hide millisecond differences. These cases do not duplicate the general
Framerate round-trip suite.

The exp RelWithDebInfo full C++ run passed 2,784 tests with two conditional skips
and no failures, under a 180-second process limit after configured runtime
fixture setup. Results are in `build-dir/shift-preparation-e3-cpp-results.xml`.
The unchanged native scenario documented in
`tests/gui-automation/shift-overflow-native.md` passed against the fresh wx
adapter, with current artifacts in
`build-dir/artifacts/shift-preparation-e3-native`. It checks complete ASS/history,
comments, scopes, selected blocks, time/frame failures and same-dialog retries,
zero distinctions, raw untouched fields and normal process exit.

Primary review inspected the current native error, retained input, raw Exact
editor and final saved-result images. Full-rule clang-tidy succeeded for the
new calculation/tests and wx adapter through a separate compilation database;
there are no new modernize/performance diagnostics. Existing unrelated warnings
are outside this extraction.

Empty selection/input behavior is source-reviewed, not a separately executed
new test. The native scenario does not establish a reentrant timecode-loss race.
Gac host integration and its acceptance are recorded in its component-reuse
document, not inferred from wx or calculation tests.
