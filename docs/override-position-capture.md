# Override position capture

`OverridePosition` in `src/subtitle_edit_ops.h` captures a selection endpoint
before a selected-row operation starts rewriting text. It is a data value, not
a document transaction, row owner or preview session.

## Units and lookup

- `original.raw` is the exact input UTF-8 byte position in the complete ASS text.
- `original.plain` is the existing normalized plain-text UTF-8 byte position.
- `characters` is the existing ICU character-boundary/grapheme count used for
  other-row projection. It is not a byte, UTF-16-unit or codepoint count.
- `CaptureOverridePosition` computes normalization and counting once.
- `ForLine(line, true)` returns the captured original pair. It does not re-read
  or normalize already-rewritten active text.
- `ForLine(line, false)` delegates to existing `RemapPosForLine`, including its
  short-target clamping and plain/raw conversion rules.

Hosts convert their native positions before capture and convert the final
operation-specific result back afterward. The helper does not move an active
caret to a visible-text anchor, mutate rows or decide which row is active.

The independent closed-block counting repair from `b9070bc61` remains in
`CharacterPos`. Capture consumes that corrected result without adding another
parser or changing escaped-brace, drawing or rendered-text semantics.

## Adapter boundary

The wx immediate toggle path captures both range endpoints before its row loop.
Color preparation captures its insertion endpoint before its row loop. Their
existing writes, alpha/reset handling, final selection/caret, commit amendment,
Cancel and Undo policies remain local.

Font insertion-point reads, pristine preview snapshots, identity/lifetime checks,
rendered-versus-committed changes and preview restoration are not unified by this
value. No callback-based batch framework or additional revision state is added.

## Acceptance

The original three capture tests were written before implementation. Their
interior-block projection literals were updated for the separately accepted
counting repair; active identity, frozen capture, Unicode byte units, target
clamping and non-mutation checks remain strict.

RelWithDebInfo builds of `Aegisub` and `gtest-run` passed. Full C++ ran after
fixture setup on an inactive private desktop within a 180-second process limit:
2,789 passes, two conditional skips and zero failures. Evidence is
`build-dir/tests-runtime/override-capture-restored-full-results.xml`.

Both native multirow scenarios passed with complete independent ASS metadata,
actual selection ranges, preview/Cancel, Undo/Redo and normal exit:
`build-dir/artifacts/override-capture-restored-normal` and
`build-dir/artifacts/override-capture-restored-interior`. They do not establish
rendering or glyph correctness solely from serialized text. Changed translation
units received full-rule clang-tidy through the separate `build-tidy` database;
new code introduces no modernize/performance diagnostics.
