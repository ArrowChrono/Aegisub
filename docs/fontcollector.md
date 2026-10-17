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

Directory inputs scan immediate `.ass` and `.ssa` children; add `--recursive` to include subdirectories. `validate` is strict by default and exits with `20` when fonts, styles, glyphs, or copies are missing.

`normalize` is read-only. It scans style font names and non-empty explicit `\fn` tags, then reports safe canonical-name changes and unsafe findings without writing the input file. `--target` defaults to `localized`; use `--target english` for verified English Win32 family names. Full, PostScript, and typographic names remain informational and are never treated as safe ASS family aliases.

Output defaults to concise human-readable text. It reports actionable issues with input file paths and source line numbers, then prints one summary line per input file. Use `--details` to include provider/cache/search events plus full ASS font usage and matched font details. Use `--json` for automation.

`list` is the font inventory mode, shaped after ACGrip's ListAssFonts output. Installed fonts are printed as `localized display name <ASS font name>` using the matched names reported by the native API; unresolved fonts are printed as their ASS font name and colored red when stdout is an interactive Windows console. Fonts with missing glyphs are colored blue. Missing glyph reports are written to stderr as `AssFontInfo 'Font,bold,italic' Dialogue #... is missing characters: ...`. `list --details` adds ASS weight/italic, install status, styles, source lines, matched family/full names, provider source, and matched font paths under each font.

## Batch Resolution

Multi-file CLI runs use the native session batch API. The collector first analyzes every ASS file, merges font requests by `(facename, bold, italic)`, unions their codepoints, resolves each merged request once, and then maps the result back to each input file. This keeps provider/cache work low while preserving per-file diagnostics.

Text output is grouped by input file. JSON output is always a schema-versioned object:

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
