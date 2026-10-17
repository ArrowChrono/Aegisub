# Font Collector

The command-line interface is part of the main `Aegisub` executable and is
invoked as the `fontcollector` subcommand. There is no standalone font
collector executable or second CMake project.

The main CMake build is the canonical integration. It provides:

- `aegisub_fontcollector_api`, the static facade linked into `Aegisub`;
- `aegisub_fontcollector`, the shared C API SDK target;
- installed `Aegisub::fontcollector_static` and `Aegisub::fontcollector`
  targets in the `AegisubCoreCAPI` package;
- `include/aegisub/fontcollector/fontcollector.h` in the CoreAPI install
  component.

Build Aegisub and both C API smoke executables from the repository root:

```powershell
cmake --build build-dir --config RelWithDebInfo --target `
  Aegisub aegisub_fontcollector fontcollector_api_smoke fontcollector_api_shared_smoke
```

Run the process and API smoke tests after building:

```powershell
ctest --test-dir build-dir -C RelWithDebInfo `
  -R "^fontcollector_(cli|api|api_shared)_smoke$" --output-on-failure
```

The `aegisub_fontcollector` shared library exposes the C API declared in
`include/aegisub/fontcollector/fontcollector.h`.

The main Aegisub build exposes the same implementation as
`Aegisub::fontcollector` (shared) and `Aegisub::fontcollector_static` (static)
through the CoreAPI install package. The GUI executable links the static
target, so the command and external API smoke tests exercise one core
implementation. The main build supplies CLI11 and links the command into
`Aegisub`.

When `--encoding` is omitted, BOMs and strictly valid UTF-8 are handled
deterministically before consulting the main build's charset detector for
legacy byte encodings. Use `--encoding` when an ambiguous legacy file needs a
specific interpretation.

## CLI

The preferred command form is:

```powershell
Aegisub.exe fontcollector list <file-or-dir> [more...] [--recursive] [--details] [--json]
Aegisub.exe fontcollector inventory <file-or-dir> [more...] [--recursive] [--low-frequency-threshold N] [--json]
Aegisub.exe fontcollector check <file-or-dir> [more...] [--recursive] [--details] [--json] [--strict]
Aegisub.exe fontcollector validate <file-or-dir> [more...] [--recursive] [--details] [--json]
Aegisub.exe fontcollector normalize <file-or-dir> [more...] [--target localized|english] [--recursive] [--details] [--json]
Aegisub.exe fontcollector collect <file-or-dir> [more...] --to <dir> [--recursive] [--details] [--json] [--strict]
Aegisub.exe fontcollector collect <file-or-dir> [more...] --to-script-dir [--recursive] [--details] [--json] [--strict]
```

On Windows, `Aegisub.exe` remains a GUI-subsystem executable so normal GUI
launches do not open a console. Automation which needs a reliable exit code
must use a wait-capable process API such as CMake `execute_process`, Win32
`CreateProcess` plus `WaitForSingleObject`, or PowerShell
`Start-Process -Wait -PassThru -NoNewWindow`. Direct PowerShell `&` invocation
does not wait for GUI-subsystem processes and therefore does not set
`$LASTEXITCODE` reliably.

All collection commands accept `--matcher platform|libass`. `platform` is the
default and uses the native provider:

| OS | platform matcher | libass matcher provider | Fontconfig linked? |
|----|------------------|-------------------------|--------------------|
| Windows | GDI/DirectWrite | DirectWrite | No |
| macOS | CoreText | Fontconfig (optional) | Optional - only for libass |
| Linux/Unix | Fontconfig | Fontconfig | Yes (required) |

`libass` uses the ported libass selector (family substitutions, attribute
scoring, and per-codepoint glyph checks). On Windows the catalog is DirectWrite
(not Fontconfig). On macOS Fontconfig is optional: without it, platform/CoreText
still works and `--matcher libass` fails with a clear error. On Linux both
matchers share the Fontconfig catalog but use different selection algorithms.
JSON reports include the selected `matcher` and resolved `provider`.
The provider is selected by the build platform; there is no user-selectable
provider cross-product. On macOS, Fontconfig remains an optional dependency used
only by the libass matcher.

The libass matcher also accepts `--additional-fonts` and
`--additional-fonts-recursive` to inspect font files which are not installed.
Use `--exclude-system-fonts` for a deterministic private-font catalog (requires
at least one additional font file). These options are intentionally restricted
to `--matcher libass`.

On Windows, a selected font can occasionally expose readable data through
DirectWrite/GDI without exposing a local file path. Such a face is reported as
`memory_only`, not missing. Collection commands dump its bytes using a
readable `<full-face-name>.<ext>` filename and an extension derived from the
font signature. A numeric suffix is added only when different font data would
otherwise use the same name; duplicate byte streams are collected only once.

JSON and `--details` output include the candidates considered by the libass
selector. Scores use libass semantics: lower is better. Candidate evidence
includes the name-match stage, provider order, glyph coverage, and codepoints
actually selected. An equal-score tie is reported as an ambiguous match; a
score is a ranking heuristic and is not proof that the renderer chose the same
face.

Except for `inventory`, directory inputs scan immediate `.ass` and `.ssa` children; add `--recursive` to include subdirectories. `validate` is strict by default and exits with `20` when fonts, styles, glyphs, or copies are missing.

`normalize` is read-only. It scans style font names and non-empty explicit `\fn` tags, then reports safe canonical-name changes and unsafe findings without writing the input file. `--target` defaults to `localized`; use `--target english` for verified English Win32 family names. Full, PostScript, and typographic names remain informational and are never treated as safe ASS family aliases.

Except for `inventory`, output defaults to concise human-readable text. It reports actionable issues with input file paths and source line numbers, then prints one summary line per input file. Use `--details` to include provider/cache/search events plus full ASS font usage and matched font details. Use `--json` for automation.

`list` is the legacy per-file font list, shaped after ACGrip's ListAssFonts output. Installed fonts are printed as `localized display name <ASS font name>` using the matched names reported by the native API; unresolved fonts are printed as their ASS font name and colored red when stdout is an interactive Windows console. Fonts with missing glyphs are colored blue. Missing glyph reports are written to stderr as `AssFontInfo 'Font,bold,italic' Dialogue #... is missing characters: ...`. `list --details` adds ASS weight/italic, install status, styles, source lines, matched family/full names, provider source, and matched font paths under each font.

### Aggregate font usage inventory

`inventory` is a read-only, one-shot scan of saved `.ass` files. File extensions
are matched case-insensitively; explicit `.ssa` inputs are not supported by this
command. Directory inputs scan only their immediate children unless
`--recursive` is given. Recursive scans do not follow directory symlinks or
junctions. Duplicate references to the same normalized file are analyzed once.
The GUI's Font Usage List uses the same analysis and aggregation core.

The command always writes one UTF-8 JSON document to stdout. `--json` is an
equivalent explicit spelling, not a different output mode. General logs and
progress belong on stderr. There is no watch mode, and `--details`, `--quiet`,
`--strict`, collection destinations and normalization targets are not accepted.
The existing matcher, encoding and libass private-font options described above
apply; the default matcher remains `platform`.

`--low-frequency-threshold` accepts a decimal integer from `0` through
`2147483647`, defaults to `5`, and marks counts strictly below the threshold.
`0` disables only low-frequency hints, not font errors or warnings. For example:

```powershell
Aegisub.exe fontcollector inventory samples/subtitles --recursive --low-frequency-threshold 6
Aegisub.exe fontcollector inventory samples/a.ass samples/b.ass --matcher libass --additional-fonts samples/fonts --exclude-system-fonts --json
```

Usage counts are not dialogue row counts. Each file contributes the number of
distinct exact `(start_ms, end_ms)` pairs that actually use a font. Duplicate
events at the same times, repeated font switches and confirmed family aliases
or bold/italic variants count once within that file. Different files contribute
independently. Unused styles, comments, empty text and pure drawing content do
not contribute. Names are localized using the system display language rather
than the application's interface language. Records are sorted deterministically
by English name, then available localized or requested name, with identity
evidence breaking ties.

The JSON contract is separate from the existing diagnostic schema:

```json
{
  "format": "font_usage_inventory",
  "schema_version": 1,
  "locale": "zh-CN",
  "matcher": "platform",
  "provider": "gdi-dwrite",
  "recursive": false,
  "low_frequency_threshold": 5,
  "complete": true,
  "result": 0,
  "inputs": [{"path": "samples/a.ass", "kind": "file"}],
  "files": [{"input": "samples/a.ass", "status": "ok"}],
  "fonts": [],
  "diagnostics": []
}
```

`locale` and `provider` above are illustrative; actual values describe the
language and provider used by the scan. Each `fonts[]` record includes
`ass_names`, nullable `localized_name` and `english_name`, `display_name`,
`usage_count`, `is_low_usage`, `missing_font`, `missing_codepoints`, `warnings`,
`display_state`, `reasons`, per-file `sources` and per-request `requests`. The sum of
`sources[].usage_count` equals the record's `usage_count`. Original ASS name
spellings are preserved; display-name fallbacks do not replace missing metadata
with fabricated names.

`requests[]` retains raw ASS tags as strings, normalized request attributes,
source styles/lines, matched names/face index and actual font-source paths,
including successful requests without warnings. Diagnostics use `file` for
their source filename, consistent with the existing collector reports.

`display_state` is one of `missing_font`, `missing_glyphs`, `warning`,
`low_usage` and `normal`, in that priority order. All applicable reasons are
retained, and `is_low_usage` remains independent of a higher-priority state.
Missing codepoints are sorted unique integers, an empty array after a successful
check with no missing glyphs, or `null` when unknown for a missing font.
Diagnostics and warning/reason objects include stable codes and readable
messages. Inventory reason messages use English independently of the GUI
language; `locale` selects font-name metadata, not the language of these messages.
Consumers should inspect codes rather than parse messages. Failed
files have `status: "error"` and structured diagnostics instead of fabricated
successful empty results. An empty directory and a successfully analyzed file
with no actual font usage are successful empty inventories.

When libass finds the requested family but none of the required glyphs, inventory
uses internal requested-family evidence to report missing glyphs rather than a
missing font. This does not change the old commands' output or the public C ABI.

Inventory distinguishes usage problems from variant metadata. The existing
`noncanonical_font_variant` and `implicit_variant_fallback` codes remain in
root `diagnostics` with their messages and source/request evidence, but their
inventory severity is `info`; they do not appear in a font row's `warnings` or
`reasons` and do not make it yellow. Intrinsic weight outside 400/700, CFF outline
format, or a different physical B/I role alone does not establish an incorrect
ASS font request. Synthetic bold/italic and an observed ambiguous match remain
warnings. This inventory-only policy does not alter schema-2 diagnostics,
severity, strict exits or collection behavior of the existing commands.

| Result | Exit code | `complete` |
| --- | --- | --- |
| Successful scan, including empty results and font findings | `0` | `true` |
| Invalid parsed options or unsupported explicit input type | `1` | `false` |
| Missing inputs, unreadable directories, or file read/parse failures | `5` | `false` |

Invalid parsed global options stop the scan. Otherwise mixed inputs retain
valid results and all diagnostics, using the fixed exit-code priority
`5 > 1 > 0`. Font findings never produce the strict audit exit code `20`.
CLI11 syntax errors retain normal CLI error handling and occur before JSON
execution, so they do not promise an inventory document. Existing `list --json`
and other command schemas remain unchanged.

## Batch Resolution

Multi-file CLI runs use the native session batch API. The collector first analyzes every ASS file, merges font requests by `(facename, bold, italic)`, unions their codepoints, resolves each merged request once, and then maps the result back to each input file. This keeps provider/cache work low while preserving per-file diagnostics.

For `list`, `check`, `validate` and `collect`, text output is grouped by input file. Their JSON output is a schema-versioned object:

```json
{
  "schema_version": 2,
  "ok": false,
  "result": 20,
  "summary": {},
  "matcher": "platform",
  "provider": "gdi-dwrite",
  "files": [],
  "diagnostics": []
}
```

Each `files[]` entry contains `input`, `ok`, CLI `result`, native `operation_result`, `error`, `summary`, `matcher`, `provider`, `diagnostics`, `events`, and `font_usage`. Top-level `diagnostics[]` is the flattened list from every file, suitable for CI/reporting. In each file report:

- `font_usage[].lines` contains source file line numbers using that ASS font request.
- `font_usage[].matched_font.missing_lines` contains source file line numbers containing missing glyphs.
- `diagnostics[].file` and `diagnostics[].lines` identify the exact ASS file and source lines for missing styles, missing fonts, missing glyphs, and collection failures.
- For in-memory `AssFile` callers without source line metadata, these fields fall back to dialogue row numbers.

The collection C ABI exposes one session/batch path:

```c
int aegisub_fontcollector_session_create_with_options(
    AegisubFontCollectorSessionOptions const *options,
    ...);

int aegisub_fontcollector_session_collect_batch(
    AegisubFontCollectorSession *session,
    AegisubFontCollectorBatchItem *items,
    size_t item_count,
    char *error_buffer,
    size_t error_buffer_size);
```

Each `AegisubFontCollectorBatchItem` owns its callbacks, summary pointer, error buffer, and result code. Invalid items report their own errors and do not prevent valid items in the same batch from being resolved.

`AegisubFontCollectorSessionOptions` is versioned with `struct_size`; its
additional font paths are UTF-8 files supplied to the libass provider.
`include_system_fonts = 0` creates a private-font-only catalog and requires
`additional_font_file_count > 0` (zero-init leaves this field at 0 — set it to
`1` unless you intentionally build a private catalog).
`collect_match_candidates = 1` enables the extra candidate scan. The CLI sets
it only for `--details` or `--json`; the GUI leaves it off.
Usage callbacks expose callback-scoped `match_candidates` evidence appended to
`AegisubFontCollectorFontUsage` when enabled.

Font-name normalization uses a separate versioned, read-only C API. Every new request, result, summary, and batch item begins with `struct_size`. Initialize it to `sizeof(struct)` for current headers; the public `*_V1_SIZE` constants describe the stable v1 prefixes accepted by newer libraries:

```c
int aegisub_fontcollector_build_normalization_plan(
    AegisubFontNameNormalizationRequest const *request,
    AegisubFontNameNormalizationCallback callback,
    void *user_data,
    AegisubFontNameNormalizationSummary *summary,
    char *error_buffer,
    size_t error_buffer_size);

int aegisub_fontcollector_build_normalization_plan_batch(
    AegisubFontNameNormalizationBatchItem *const *items,
    size_t item_count,
    char *error_buffer,
    size_t error_buffer_size);
```

The batch API takes an array of item pointers, constructs one immutable family catalog, and uses it for every input plan. Each item's request pointer must remain valid for the call. Callback strings are valid only for the callback duration. Neither function writes subtitle files.

Callers must inspect each `AegisubFontNameNormalizationBatchItem::result` (and that item's error buffer). The batch function return value is only for argument/catalog-level failures; individual input read or analysis failures do not make the batch return non-zero by themselves.

JSON output for `fontcollector normalize --json` uses `schema_version: 2`. The root `summary` aggregates counts across files and includes `catalog_available` (true only when every successfully analyzed file had a usable family catalog).

## GUI portability application safety

The CLI `normalize` command and the name-plan C API above remain report-only;
neither writes an ASS file. A suggested name in that report is not authority to
apply a combined GUI name/variant change without the GUI's physical-selection
checks.

The GUI stages selected B/I and name changes on ID-preserving subtitle copies.
Before committing anything, it replays every affected plaintext font state,
including inherited styles, later `\fn` families, named/empty `\r`, explicit
resets and text after drawing blocks. Changed effective requests must preserve
observed physical entity, role, realized weight and realized italic state.
Synthetic, unknown or incomplete evidence rejects the whole selection without
partial changes. A single-finding preflight sets unprovable proposals to
report-only; Apply independently checks the complete selected combination and
the current dependency text again.

Name aliases require separate live probes of the original and proposed raw
names (including a leading `@`); one family ID or one normalized localized-name
probe cannot establish that both names select the same entity. Without live
alias evidence the GUI does not apply a rename. Variant-only callers without a
live provider retain their captured Observed catalog-profile semantics; they
do not gain a guarantee about later system-font changes.

Font-affecting transforms and signed relative font sizes whose changed state
cannot be proved by the static profile are report-only. Color-only transforms
and isolated explicit/reset/drawing boundaries remain usable when the complete
proof succeeds. Style changes conservatively replay all events, so an invalid
style/reset elsewhere can prevent automatic application; span-only changes do
not reparse completely unchanged events when all styles are unchanged.
Planning still creates a candidate copy for each proposal; large-script
planning performance has not been measured. No cross-platform rendering or
pixel-equivalence guarantee is implied by local font-selection preservation.
