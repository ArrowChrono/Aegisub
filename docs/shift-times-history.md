# Numeric Shift Times history

History records use numeric values in memory. `amount` is the actual integer
millisecond shift in time mode, or the signed integer frame amount in frame mode,
before the separate forward/backward direction is applied. The existing JSON
schema retains a string `amount`; parsing and formatting occur at that boundary.
Localized labels and control text are presentation, not a second source of truth.

## Compatibility and precision

Legacy time amounts accept ASCII decimal seconds or hours:minutes:seconds,
optional dot/comma fractions of one to three digits, surrounding spaces/tabs and
zero padding. Minute/second components in the colon form must be below sixty.
Garbage characters, negative times, more than three decimal digits and values
above the signed integer millisecond range are rejected rather than discarded
or clamped. Frame amounts accept decimal integers with an optional sign and
surrounding spaces/tabs, within the full signed int range.

Time amounts are normalized once using the existing `agi::Time` integer
conversion, matching the wx shift's actual execution value. This does not change
the shared rounding algorithm:

| Legacy amount | Actual shift in milliseconds | Canonical history text |
| --- | ---: | --- |
| `00:00:01,234` | 1230 | `0:00:01.23` |
| `0:00:01.235` | 1240 | `0:00:01.24` |
| `0:00:59.995` | 60000 | `0:01:00.00` |
| `0:59:59.995` | 3600000 | `1:00:00.00` |
| `596:31:23.647` | 2147483647 | `596:31:23.647` |

The terminal integer value is an exceptional saturated result of the existing
rounding rule. Its three decimal places are retained on write so a round trip
cannot turn the actual value into 2147483640. Normal accepted time records use
two decimal places. New wx records come from the amount used by processing,
not from reparsing the original control text.

Selected blocks are ordered, nonoverlapping, one-based inclusive ranges. Fields
and selection mode are integers in 0..2. Required fields must exist with the
correct JSON types; missing/null fields are not implicitly inserted or coerced.
Malformed records reject the complete load. Unknown extra fields are not part of
the numeric DTO and are not retained on a successful subsequent write.

## Publication and failures

Decode builds a temporary result without mutating the source JSON. wx builds
all localized labels before publishing the resulting records and list. A missing
history file means no history. Other read/validation errors produce a warning,
leave the list empty and prevent subsequent accepted shifts from overwriting the
unreadable file. Explicit successful Clear re-enables recording. Cancel preserves
the history file; ordinary Shift preferences still save when the dialog ends.

History remains auxiliary: a load error does not prevent a subtitle shift, and a
write error reports that subtitles shifted but history did not save. A write
error does not roll back subtitle changes. Successful shifts prepend an entry
and retain the existing fifty-entry limit. The timing and direction algorithms,
zero-time early return and subtitle commit policy are unchanged.

## Verification

Five necessary parser/codec tests were authored before implementation. They
cover old precision and carry boundaries, strict rejection, signed frame limits,
terminal time identity and malformed required fields/ranges. They supplement
native E2E because file-format arithmetic and integer extremes are not fully
observable through the ordinary time control's centisecond display.

The native wx scenario is documented in
`tests/gui-automation/shift-history-native.md`. It checks actual canonical labels,
native amount/radio input, full ASS content, cancellation, normal shutdown,
restart, all-or-nothing malformed loading, preservation during subsequent
application and explicit-clear recovery. It does not dynamically validate
history double-click: wx MSW uses `GetMessagePos()` for hit testing, which is not
controlled by inactive-desktop posted control messages. No input-desktop switch,
global pointer move or SDK hit-test bypass is used. The existing double-click
entry remains connected to direct numeric restoration; source review is not
reported as dynamic coverage.
