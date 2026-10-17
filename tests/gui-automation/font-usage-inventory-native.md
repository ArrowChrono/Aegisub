# Font usage inventory E2E

Run all commands from the repository root against a newly built Aegisub.
Do not run these scenarios concurrently with shared builds, other GUI scenarios,
clipboard users, or font-installation work. Each artifact directory must be new.
Neither successful compilation nor an old `result.json` is a behavioral pass.

## Deterministic CLI contract

```powershell
python tests/smoke/font_usage_inventory_e2e.py `
  build-dir/RelWithDebInfo/Aegisub.exe `
  build-dir/artifacts/font-usage-inventory-cli
```

This standard-library Python suite requires Windows for its real exclusive-share
read-lock scenario; it fails, rather than silently skipping, on another OS.
It uses `tests/fixtures/gdi-fonts`, libass
additional-font support and `--exclude-system-fonts`. It does not install fonts.
The deliberately missing-family check and legacy strict-audit check use the
default platform matcher against a dedicated absent name. The 900-second suite
budget and each 60-second process deadline fail rather than retry. A failure
leaves the input, stdout, stderr, process metadata and final `result.json`.
Input changes are limited to the copied runtime fixtures; the source fixtures
are never rewritten. The three-file source copy is also fingerprinted before
and after all CLI commands, including `collect` and `normalize`.

The independent oracle is `tests/fixtures/font-usage-inventory/expected.json`.
Its three ASS files deliberately contain equal-time events on different layers,
bold/italic switches, different end times, cross-file equal intervals, unused
styles, comments, empty text, tags-only text and drawing-only text. The known
font maps only space and `A`; `B` is a known absent glyph. A regular-only catalog
requires synthetic bold. Expected counts and glyphs are not learned from the
program's output. The generated family/fullname alias input uses the fixed
English family and full-name metadata of the existing regular fixture.
The existing collector also classifies a synthetic realized variant as
`noncanonical_font_variant`; its platform unresolved-font path retains that
warning for an unknown realized variant. The inventory assertions preserve
these complete legacy diagnostics instead of filtering them from warnings or
the GUI's full-copy reasons.

The fallback regression uses two ASS files under `fallback-inputs`, containing
`A` and `AB` at the same interval in different files. Its font directory passes
`00-fallback-b.ttf` before `zz-primary-a.ttf`; both have face index zero.
The fallback family is alphabetically before the primary family and contains
only space/B; the copied primary contains only space/A. The independent oracle
requires one primary-family row with two uses, one use from each ASS file,
primary English/display metadata and primary request names. The fixed primary
Win32 name table has only an `en-US` family name: localized metadata must be the
primary English family when the system UI language has primary language `en`,
and must be JSON `null` otherwise. The displayed name still has its independently
specified English fallback, not a fabricated localized field. The test obtains
the actual UI-language list through `GetUserPreferredUILanguages` and checks the
reported locale against its first entry; it does not derive the oracle solely
from inventory output. The AB
request must preserve both actual font paths. Selecting metadata from any
matching face-zero path would incorrectly split the A-only and AB rows and is
an explicit failure, not an alternative expected result.

Generate the dedicated CC0 fallback font from the existing regular fixture
using FontTools; test execution itself does not require FontTools:

```powershell
python tests/fixtures/font-usage-inventory/generate_fallback.py
```

| Exact CLI test | Direct evidence |
| --- | --- |
| `test_three_file_interval_deduplication_and_threshold_boundaries` | Nonrecursive 3, recursive 5; thresholds 0/5/6; exact reason codes and states |
| `test_default_json_is_equivalent_to_explicit_json` | One plain UTF-8 JSON document; optional `--json` equivalence and defaults |
| `test_multiple_sources_deduplicate_and_ignore_input_order` | Directory/file overlap, duplicate files and reversed input order |
| `test_single_file_keeps_same_time_variants_once` | Three same-time events and mixed styles contribute one interval |
| `test_requests_preserve_explicit_tags_style_variants_and_matching_evidence` | Raw bold/italic tags retain exact strings such as `"1"`, style evidence, effective 400/700 weights, matching paths and variants |
| `test_confirmed_family_and_fullname_alias_merge_without_losing_raw_names` | Same-entity alias merging, raw-name retention and English display metadata |
| `test_fallback_face_preserves_primary_family_and_all_font_paths` | A-only/AB files aggregate to one primary-family row with count two despite preceding face-zero fallback; English/display/request metadata remain primary, localized metadata follows the independently checked fixed `en-US` name table, and AB keeps both font paths |
| `test_empty_directory_and_comment_only_file_are_distinct_success` | Zero files versus one successfully parsed file with no usage |
| `test_exclusive_read_lock_publishes_partial_result_and_recovers` | Real Windows `CreateFileW` read handle with share mode zero; partial a=2/error b/exit 5, then complete count 3 after release |
| `test_missing_font_preserves_raw_name_and_unknown_glyph_null` | Long and short unknown GDI families retain their raw ASS names rather than platform substitutes; missing state, real JSON null semantics and success exit despite a font issue |
| `test_missing_glyphs_are_sorted_unique_and_override_low_usage` | Exact `[66]` missing set, red state, independent low-usage flag and complete reason list including duplicate detection |
| `test_warning_and_red_warning_keep_all_reasons` | Regular-only synthetic bold, red over yellow over blue, threshold-zero preservation of real errors and exact complete reason lists |
| `test_partial_failures_preserve_valid_rows_and_exit_priority` | One batch with valid, `.ssa`, absent and invalid-Dialogue-time inputs; each absent/malformed file has error status plus `file_error` keyed by diagnostic `file`, `.ssa` has `invalid_argument`, exit 5 and incomplete result |
| `test_unsupported_explicit_extension_exits_one` | Strict explicit `.ssa` rejection in inventory only |
| `test_invalid_global_options_do_not_scan_otherwise_bad_input` | Invalid global option wins before missing-source processing |
| `test_threshold_int_max_and_invalid_syntax` | Int32 maximum accepted; empty/negative/fraction/overflow/text return the parser's exact exit 64, option-specific stderr and no stdout |
| `test_inventory_rejects_write_only_options` | No collect/normalize/strict output options; exact parser exit 64 with the rejected option on stderr, no stdout or accidental output directory |
| `test_legacy_commands_retain_schema_two_and_strict_exit` | Existing list/check/validate/collect/normalize schema and strict exit 20 |

The common JSON assertion additionally checks field types, all allowed display
states, source-count sums, original-name deduplication, schema 1 and the actual
exit code. The legacy C API ABI and existing C++ tests must still be run through
their existing build/CTest commands; this process suite is not an ABI proof.

## Native GUI scenario

### Current contract and evidence boundary

The final user-approved UI is a scrolled list of independent, read-only native
name/status text fields. Text can be selected directly within either field.
`Copy` and `Select All` in a field's actual context menu operate only on that
field. The separate `Copy font names list` and `Copy full list` actions operate
on the complete current published inventory, preserving the tab-separated
name/count/all-reasons structure and platform CRLF. Count/reason formatting is
localized by the GUI, not hard-coded into the core JSON formatter.
There is no names-only preference checkbox, whole-row selection, Grid, STC, or
double-click editing mode. The test's replacement of those previous assumptions
is an explicit user contract change, not relaxed assertions for old failures.

The STC and Grid prototypes were never acceptance evidence for this final UI.
Historical failures, including hover-run1's selected-text input failure and the
old wrapped suffix, remain failures. The old normal SCI scenario is retired and
returns failure with an explanatory message; it is not a runnable acceptance
recommendation. Current runs require `--probe-controls` or
`--interaction-controls`. The explicit `--background-supported-inputs` option is
valid only with `--interaction-controls`; it selects the bounded background-input
sub-scope below, not complete GUI acceptance. Without that option the strict F5
case and its failure path remain unchanged.

The fields-probe-run4 actual native/UIA evidence established native `Edit`
controls in per-record row panels, with `ES_READONLY|ES_MULTILINE`, no border,
left-aligned name and `ES_RIGHT` status. Both expose UIA Document/TextPattern
with Single selection, **not ValuePattern**. The scenario therefore uses actual
native HWND/style/parent/value observations, not invented ValuePattern state,
SCI messages, captions used as data, or private Grid messages. Each publication
waits for its complete independent list-copy oracle and then exact name/status
pairs; merely finding two readonly Edits is not scan-completion evidence.

### Current English/600-DIP revision and red evidence

The GUI scenario writes an isolated profile with `App/Language=en_US`. It now
requires exact untranslated default English: `1 use` versus `n uses`, compact
`Not found` / `Missing glyphs` / `Warning` / `Low usage`, and full reasons joined
with `; `. Non-standard variants retain their warning code and explicitly say
`Warning: non-standard font variant (not a font match failure)`; this does not
reclassify that warning as a failed match. Existing count, ordering, CRLF,
fragment, field-value, selection/focus, watcher, source, Undo and font-set
oracles are otherwise unchanged. This is not a cross-language GUI execution.

The first phase measures the **outer** window, not its client area, and requires
exact 600 by 560 DIP converted at actual `GetDpiForWindow` scale. Root ran the
new assertion against the frozen old producer before rebuilding it:
`build-dir/artifacts/font-usage-inventory-localization-width-red3/default-window-size.json`
records DPI 120, expected 750 by 700 pixels, actual 700 by 700 pixels. The exact
600-DIP assertion failed before any clipboard or font operation, proving the
old 560-DIP default does not satisfy the new expectation. Earlier red1 setup
rejection and red2 desktop-permission failure remain unsuccessful setup evidence,
not behavioral width failures. The successful permission-elevated launch in red3
is the behavioral red result.

The C# revision compiled with `--no-dependencies`; command/log/result are in
`build-dir/artifacts/font-usage-inventory-localization-test-build4/`. Root then
executed the rebuilt localized GUI in
`build-dir/artifacts/font-usage-inventory-i18n-gui-run2/`: the bounded interaction
scope passed, while full acceptance remains false. The strengthened English CLI
suite also passed in `build-dir/artifacts/font-usage-inventory-i18n-cli-run1/`.
The exact results and remaining gaps are recorded below; compilation alone is
not used as behavioral evidence.

The existing CLI tests
`test_three_file_interval_deduplication_and_threshold_boundaries`,
`test_missing_font_preserves_raw_name_and_unknown_glyph_null`,
`test_missing_glyphs_are_sorted_unique_and_override_low_usage` and
`test_warning_and_red_warning_keep_all_reasons` also require complete exact
English `reasons[].message` lists, including `Not found`, `Low usage`,
`Missing glyphs: B U+0042`, synthetic-bold wording and the full non-standard
variant clarification. Their pre-existing code/state/count and duplicate-reason
oracles remain; no new unit tests or merely message-presence checks were added.

### Scope and exact phases

| Exact phase | Automatic evidence / independent oracle |
| --- | --- |
| `interaction_V01_menu_singleton_nonmodal` | Real File menu command twice yields one owned non-topmost inventory; main remains enabled; exact outer 600x560 DIP at actual native DPI; empty list has both full-list actions disabled without clipboard writes; defaults threshold 5/nonrecursive. |
| `interaction_V15_real_session_font_registration` | With the explicit opt-in, absent fixed family is missing with the original requested name, count 1 and exact missing/low reasons. Four actual session font files register, and automatic refresh resolves the same count without F5. |
| `interaction_V04_local_tip_observation_refresh` (strict) / `interaction_V04_local_tip_observation_watch_refresh` (explicit background sub-scope) | The user-approved local wxTipWindow retires the old native static-TOOL oracle. One real GetCursorPos capability observation gates an ordinary local mouse move and bounded new-owned-visible-window/PNG collection. Content, twelve-second persistence, leave closure and refresh closure are separate blocked requirements, not inferred from expected/HelpText/source/native TOOL strings. The same-content real watched write must still publish a complete enabled pair with both public identities changed before exact localized full-list copy. |
| `interaction_V04_native_fields_resize_selection_preserved` | Missing and resolved one-use cases at actual initial, +25%, narrow (65% but no smaller than native min) and restored widths. HWNDs, exact selection, full values and readonly state persist. Status `EM_GETLINECOUNT` is exactly 1. Full-list clipboard expectations do not change. Selected/unselected PNGs and actual bounds are separate manual right-alignment/color evidence. |
| `interaction_V02_V03_three_five_and_thresholds` | Exact 3/5 uses through threshold 0, 5, 6; equality at 5 is not low frequency; six makes five low frequency. Native name/status values and full-list count/reasons match independent constants. |
| `interaction_V07_native_fragment_and_list_copy_scopes` | In the dedicated-family variant, public `EM_SETSEL/EM_GETSEL` selects the proper substring `GDI`; native `WM_COPY` must return exactly `GDI`. Both whole-list context actions preserve that fragment selection and copy scope. Real name-field Select All/Copy returns only its full name. Collapsed selection leaves clipboard sequence unchanged; values/readonly remain unchanged. No whole-record copy can satisfy the fragment oracle. |
| `interaction_V07_multiple_records_exact_CRLF` | Two independent records produce exact names-only CRLF and exact full-list count/all-reasons CRLF. Native fields show compact highest-priority status, not the complete reasons. |
| `interaction_V14_nested_native_Edit_mousewheel_scroll` | A dedicated 32-family absent-font input has an independently ordered names-list oracle. A standard local `WM_MOUSEWHEEL` delivered to the actual nested Edit moves the first row upward in its containing list without changing its text; before/after PNG/coordinates are saved. No global pointer input. |
| `interaction_V08_same_text_watch_selection_focus_restoration` | Actual background native click establishes field focus (read through target-thread `GetGUIThreadInfo`); selection is set publicly. Writing identical saved ASS content causes watched publication with different enabled public field identities. Same key/full text restores exact selection and thread-local focus, and copies the same fragment. This is not physical-keyboard validation. |
| `interaction_V08_watch_write_atomic_create_rename_delete` | Real write, atomic replacement, create, rename out/in and delete yield exact counts 5→6→7→9→7→9→7 with unchanged full-list reasons, no F5. |
| `interaction_V09_deleted_root_F5_rebuild_and_later_write` (strict) / `interaction_V09_deleted_root_watch_rebuild_and_later_write` (explicit background sub-scope) | Real root deletion disables both list actions; strict F5 while missing remains required. The explicit background sub-scope records that F5 step blocked without posting it. Both retain exact rebuilding seven, later automatic write eight, and restored seven. |
| `interaction_V09_root_churn_recovers_without_modal` | After the original V09 scenario, eight predeclared root delete/restore cycles use the existing backup without sleeps or retries. A new complete enabled public field pair must retain the exact seven-use row before the unchanged exact full-list clipboard oracle. A later real saved-source append must automatically publish exact eight, and restoring that file must automatically return exact seven, proving the final subscription remains live rather than merely exposing a recovered snapshot. Owned visible `#32770` windows other than the exact main/inventory HWNDs fail rather than being dismissed; the exact owner windows must stay enabled and the inventory must answer bounded `WM_NULL`. `root-churn-source-events.json` records all sixteen completed source actions, with partial progress preserved on failure. This regression executed completely in recovery runs gui-green1 and gui-green2; see the recovery verification section below. |
| `interaction_V02_source_replacement_and_non_ASS_retention` | Single file gives two; invalid-only drop retains two and recursion is disabled for file-only input; mixed duplicate/invalid paths give five; directory replacement restores seven and remembered recursion. |
| `interaction_V01_unsaved_main_editor_edit_real_undo` | Actual writable main subtitle edit while inventory is open; strict F5 still requires disk-source seven. In the explicit background sub-scope, F5 is blocked/not posted and an identical-content real source write must publish different complete enabled field identities before the same disk-source-seven oracle. Saved subtitle text is unchanged; actual Edit/Undo restores original text and clean caption. |
| `interaction_V10_close_reopen_persists_settings_without_sources` | Threshold six and recursion survive close/reopen. Reopen has no source/current list and both list actions disabled. The removed checkbox is not tested as a preference. |
| `interaction_V15_real_session_font_removal` | Four actual owned session registrations remove; the same count three automatically becomes missing with raw requested name and exact missing/low reasons, without F5. Per-path success and outstanding balance survive cleanup failures. |
| `interaction_V01_owner_close_and_read_only_main_subtitle` | Inventory actions leave saved subtitle text and clean main caption unchanged; closing owner destroys inventory and exits normally. |

### Real context actions and native field selection

All list copying uses the actual owned `#32768` popup. The runner observes the
HMENU, exact item labels/enabled states and bounds, then sends at most the
observed item count of local DOWN presses until the exact target is highlighted.
Only then does it send ENTER. It never bypasses a menu using its observed command
ID. Disabled actions are dismissed with ESC without command execution. The
clipboard sequence is sampled before the menu action, not after it.

An enabled list-menu invocation remains pending with its original pre-command
clipboard sequence until that sequence changes. The runner does not re-open a
menu every polling interval and absorb a queued response into a new baseline.
Only after reading an owner-verified response may a mismatched old publication
start another invocation, still inside the original ten-second wait. Invocation,
pending sequence and exact response artifacts distinguish those states.

Full-list OLE responses use `ReadOwnedUnicodeTextAfter(invocationBefore, hostPid)`.
The clipboard STA first acquires `OpenClipboard` (the existing 500 ms open limit),
then samples owner/sequence under that lock. The owner PID must be the actual
host and sequence must differ from that invocation's original baseline. The
worker-thread pending sequence is diagnostic only, not a stale STA precondition.
wxMSW `SetData` uses `OleSetClipboard`, whose formats are delayed-rendered; hence
`GetClipboardData(CF_UNICODETEXT)` may legitimately advance the sequence while
materializing data. The snapshot records both locked pre-read and post-GetData
sequences, then strictly checks owner HWND/PID and post-GetData sequence remain
stable throughout the locked copy. It retains the bounded even UTF-16 allocation,
8 MiB cap, in-allocation terminator, checked memory unlock and clipboard close.
Only the snapshot's original payload can satisfy the unchanged exact oracle.
No owner exception is retried, no external owner is accepted, and no producer
`OleFlushClipboard` or clipboard mutation is introduced. After releasing the
lock, a later transaction does not invalidate an already completed locked
snapshot. The original `ReadUnicodeText(expectedSequence, hostPid)` contract and
native field-fragment call sites remain unchanged. Errors include stage and
expected/actual owner/sequence; successful response artifacts include the
independent locked snapshot evidence. See Microsoft's
[OleSetClipboard](https://learn.microsoft.com/en-us/windows/win32/api/ole2/nf-ole2-olesetclipboard)
and [GetClipboardData](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getclipboarddata)
contracts. The prior supported-run2 failure remains failure evidence: its missing
actual owner/sequence diagnostics leave external interference versus same-host
OLE activity unresolved. The new lock-scoped protocol removes the caller-to-STA
TOCTOU; one successful run does not dynamically establish that older failure's
specific cause.

During asynchronous changes, a bounded ten-second wait requires exact expected
CF_UNICODETEXT. Counts, reason strings and CRLF are fixture constants; only the
platform-dependent display name has the fixed-fixture metadata/CLI guard.
Successful full-list copy is followed by complete native field-pair equality.
Any visible group with other than two fields makes the entire snapshot explicitly
incomplete (`null`, not an empty list). Bounded publication waits cannot accept
the remaining good rows while omitting that group; persistent extra singletons
time out, and selection review outside convergence fails immediately.
Dynamic fields are re-discovered after publication; dead HWND ownership errors
are treated as transient only with the exact existing host-ownership error and
contemporaneous loss of live ownership while the host remains alive. Class
lookup error 1400 is accepted as a destroyed candidate only when `IsWindow` is
false. A field text-read error 1400 is accepted only while the host remains alive
and that actual HWND is already destroyed. Either narrow field-read lifetime
race records an incomplete snapshot and returns `null` for the existing bounded
publication wait to re-read the whole tree; no dead field becomes empty text or
an omitted good group. Host exit, still-live HWND errors and other native errors
continue to fail. These races are recorded; other failures propagate.

Native field selection uses public Edit messages. It does not depend on global
modifier state, foreground activation or SendInput. Name-field Select All/Copy
menu behavior is covered; the status field runs the full-list action, not its own
native Select All/Copy. **Physical Ctrl+A/C routing is not claimed**. Selected
native character ranges are automatically checked with EM_GETSEL, not inferred
from drag coordinates. The readonly style, full field values and one-line status
are public native observations, not pixel-color assertions.

Queued F5 is followed first by a bounded observation of actual content identity,
value or enabled-state transition, with no popup opened to drain the queue.
Public identity includes HWND, control ID, parent/parent ID, class/role style,
full text and enabled state. Only after that evidence may the unchanged exact
clipboard oracle establish completed publication. A numeric HWND can be reused;
IsWindow alone is neither destruction nor refresh-completion evidence.

The strict case is known unavailable with this background message backend:
fields-interaction-run2 and run3 remain failed artifacts. In this wxMSW build,
`wxEVT_CHAR_HOOK` comes from its thread `WH_KEYBOARD` hook, whereas ordinary
posted `WM_KEYDOWN` produces `wxEVT_KEY_DOWN`/possibly `CHAR`, not that hook event.
The dialog binds F5 through `CHAR_HOOK`. The Windows posted-input distinction is
also documented by [Microsoft](https://devblogs.microsoft.com/oldnewthing/20250319-00/?p=110979).
Static source evidence establishes this backend limitation; the two limited CDB
attempts did **not** complete the dynamic input chain and do not prove it.
Changing the PostMessage target or adding a producer-only testing route is not
an accepted substitute for real F5 input.

Only explicit `--background-supported-inputs` omits those known unsupported F5
posts. Every affected requirement is recorded blocked, never passed. Tooltip and
unsaved-main-editor refresh instead use real same-content watched writes and
require complete enabled replacement field identities before any copy popup;
deleted-root recovery retains its independent watcher/count assertions. This
scope does not claim real F5 or physical-keyboard coverage.

### Local tooltip observation and explicit gaps

The new user-approved policy is a window-local wxTipWindow that stays while the
pointer remains over the field and closes on leave or refresh. This is not the
old native auto-pop TOOL backend. Its custom view paints text itself, so ordinary
WM_GETTEXT/UIA cannot be assumed to expose the painted details. The former
registered-native-TOOL equality oracle is explicitly retired because of that
contract/backend change, not relabeled as a passing test. Its supported-run3
artifacts below remain historical evidence only.

The independent current absent-family details oracle is written to
`tooltip-independent-oracle.json`: original family, `1 use`,
`Reasons: Low usage; Not found; Warning: non-standard font variant (not a font match failure)`,
`ASS requested names:`, `GDI returned/matched names:`, `Sources:` and the known
source path plus ` · Line 11`. The GDI name is still guarded by the same-platform
CLI, not represented as rendered tooltip evidence. Tooltip line-range compression
and other plural source-line layouts are not proved by this single-line oracle.

The runner makes one GetCursorPos capability observation. If it fails,
`actual_cursor` is null and no fallback coordinates are used as evidence. Only a
successful real cursor read already inside the actual field permits one ordinary
window-local WM_MOUSEMOVE and at most two seconds of actual new-owned-visible
window collection. Candidate identities and original PNGs are evidence for root
review, not an invented static tooltip registration. No global cursor motion,
foreground activation, desktop switch, producer test hook or forced popup is
used. Microsoft requires the input desktop to be current for
[GetCursorPos](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getcursorpos);
wx's GetMessagePos fallback is not proof of a real continuous hover.

`V04_local_tip_visible_content`, `V04_local_tip_12_second_persistence`,
`V04_local_tip_leave_closure` and `V04_local_tip_refresh_closure` remain explicit
blocked partitions in this background scenario. It does not wait twelve seconds
and then claim persistence based only on text or elapsed time. Same-content watch
publication, different complete field identities and exact English full-list
output continue to be asserted independently of these tooltip gaps.

### Probe-only mode

`--probe-controls` collects baseline native/UIA/PNG, submits copy-multi.ass via the
actual inventory WM_DROPFILES route, then captures the early 500/1500ms samples.
The source acceptance guard reads only the stable baseline direct-child source
Static, verifying ownership and parent/class/control-ID before each read.
A source label is input-acceptance evidence, not scan-completion evidence.

Within ten seconds from acceptance, probe waits for at least a real readonly
native Edit pair and saves a final ready/timeout snapshot. This is only a
capability prerequisite: the actual run4 ready snapshot caught Render midway,
so it is expressly not used as the formal suite's complete-publication oracle.
Native styles/client bounds/read-only observations and supported UIA patterns are
recorded. Grid/Table/Selection metadata is read only if actually exposed; GetItem
has a shared cap of 64 calls per snapshot. Typed LegacyIAccessiblePattern is
absent from the managed UIA reference; no additional COM framework is invented.
Probe invokes no UIA pattern actions and uses no clipboard/fonts. Exit zero
means collection finished, while result.json keeps passed=false and
full_acceptance=false.

### Build and isolated execution

Compile only, from repository root:

```powershell
$env:NUGET_SCRATCH = Join-Path (Get-Location) `
  'build-dir/dotnet-artifacts/font-usage-inventory/nuget-scratch'
dotnet build tests/gui-automation/font-usage-inventory-native.cs `
  --configuration RelWithDebInfo `
  --artifacts-path build-dir/dotnet-artifacts/font-usage-inventory `
  --output build-dir/dotnet-artifacts/font-usage-inventory/RelWithDebInfo
```

Root alone may run the background scenario after freezing the matching producer
and driver. Use a **fresh, nonexistent** artifact directory (do not pre-create it).
The strict command intentionally retains the known unavailable F5 input case and
is expected to fail on this backend; it is a reproduction command, not the
recommended supported-input validation command:

```powershell
$process = Start-Process `
  build-dir/dotnet-artifacts/font-usage-inventory/RelWithDebInfo/font-usage-inventory-native.exe `
  -ArgumentList 'build-dir/RelWithDebInfo/Aegisub.exe', `
                'build-dir/artifacts/font-usage-inventory-fields-strict-f5-run1', `
                '--interaction-controls', '--allow-session-font-changes' `
  -WindowStyle Hidden -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "Strict inventory scenario failed (known F5 backend limitation)" }
```

The following records root's actual final localized GUI invocation, which exited
zero against the matching rebuilt producer. Its artifact directory now exists;
a reproduction must replace that argument with a fresh nonexistent directory.
This remains only the bounded supported-input scope. The prior supported-run3
used the same flags with its historical artifact directory and older expectations:

```powershell
$process = Start-Process `
  build-dir/dotnet-artifacts/font-usage-inventory/RelWithDebInfo/font-usage-inventory-native.exe `
  -ArgumentList 'build-dir/RelWithDebInfo/Aegisub.exe', `
                'build-dir/artifacts/font-usage-inventory-i18n-gui-run2', `
                '--interaction-controls', '--background-supported-inputs', `
                '--allow-session-font-changes' `
  -WindowStyle Hidden -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "Inventory background-supported-input sub-scope failed" }
```

Exit zero for the explicit sub-scope means its executed automatic checks did not
fail. It is **not** a complete GUI pass: both result files keep `passed=false`
and `full_acceptance=false`, while `interaction_passed` separately reports the
executed sub-scope. Metadata records the exact mode, `background_supported_inputs`
and `strict_f5_case_executed` (false in that sub-scope). The flag is rejected
without `--interaction-controls` and cannot combine with probe mode.

The font flag requires separate human authorization for real nonpersistent
session registration/removal in the isolated test environment. This scenario
uses only its four controlled CC0 fixture files, never installed user fonts.
Private desktops do not isolate the Windows clipboard, so clipboard use also
requires the already coordinated permission/exclusivity. No foreground test,
input-desktop switch, global pointer movement or SendInput is performed.

For capability collection only, replace all scenario flags with `--probe-controls` and
choose a fresh probe directory. Without font opt-in, actual font-set mutation and
the dedicated literal-GDI fragment variant are explicitly blocked, not faked.

The scenario retains one 120-second hard watchdog inside the existing
210-second private-desktop supervisor. Individual waits are ten seconds, native
messages one second, startup twenty seconds and a CLI comparison sixty seconds;
all are bounded by the total watchdog. Timeout/forced termination is failure.
Only successfully registered paths are removed, each outcome is logged, all
owned paths are attempted even after a removal error, and failures are reported
rather than hidden by cleanup. The original failure remains sticky during finally.

### Executed final i18n GUI/CLI results

The final localized GUI artifacts are in
`build-dir/artifacts/font-usage-inventory-i18n-gui-run2/`; root reported process
exit zero. Its `result.json` and `interaction-summary.json` agree on the following
bounded outcome:

| Actual result | Evidence |
| --- | --- |
| Interaction scope passed, not complete GUI acceptance | Both files have `interaction_passed=true`, `error=null`, `passed=false`, `full_acceptance=false`, `background_supported_inputs=true`, `strict_f5_case_executed=false`; mode is `interaction-controls/background-supported-inputs`. |
| 347 check records: 336 true checks (335 interaction + 1 startup), 0 false, 11 blocked; all 16 current interaction phases executed | Exact check/phase records in both result files. The startup check contributes to the 336 true total, but not to the 335 interaction assertions or 16 interaction phases; blocked records are not passes. |
| Exact default outer window 600 by 560 DIP | `default-window-size.json`: DPI 120, expected 750 by 700 pixels, actual 750 by 700 pixels, `measured_surface=outer_window_not_client_area`. This closes the default-width assertion that failed against the old producer. |
| The current en_US count/status/list-copy oracles and existing complex interaction assertions passed | The phase records include exact native fields, fragment and both full-list menu scopes, multi-record CRLF/reasons, resize, nested Edit wheel, same-text selection/focus restoration, saved-file watcher/root/source changes, real edit/Undo, owner close and session font registration/removal. This does not constitute another-language GUI or real-keyboard execution. |
| Real hover could not be established in the inactive-desktop environment | `local-tip-hover-capability.json`: `cursor_readable=false`, `native_error=5`, `actual_cursor=null`, `cursor_inside_field=false`, one attempt, no input-desktop switch or global pointer move. Failed/fallback coordinates were not treated as a hover. |
| Local-tip content, twelve-second persistence, leave closure and refresh closure were not measured | Separate `V04_local_tip_visible_content`, `V04_local_tip_12_second_persistence`, `V04_local_tip_leave_closure`, `V04_local_tip_refresh_closure` blocked records. No HelpText, source text, old TOOL registration or elapsed delay substitutes for these observations. |
| The strengthened English CLI contract suite passed | `build-dir/artifacts/font-usage-inventory-i18n-cli-run1/result.json`: `passed=true`, `tests_run=18`, no failures/errors, 45 process records. This includes the four exact reason-message cases named above without changing their warning-code policy. |

The eleven blocked records consist of the retired native static-TOOL oracle,
four unmeasured local-tooltip requirements, three real-F5 requirements,
foreground validation, controlled warning-yellow rendering, and the grouped
locale/identity/real-keyboard/debounce/slow-scan/permission/live-settings/DPI/theme
gaps. A retired backend record is not a passed tooltip test. In particular,
`GetCursorPos` error 5 is an environmental observation, not proof that the new
local tooltip appears, stays for twelve seconds, or closes correctly.

The first localized GUI attempt remains failed evidence in
`build-dir/artifacts/font-usage-inventory-i18n-gui-run1/`: its complete-field
oracle timed out after observing the mojibake status `1 use Â· Not found` instead
of the required `1 use · Not found`. The correct expected text was not weakened.
The earlier 600-DIP width-red artifacts and the retired old native TOOL oracle
also remain intact. Only the final i18n-gui-run2 result is the current bounded
interaction pass; it does not relabel those failures or establish the unmeasured
tooltip lifetime behavior.

### Historical supported-run3 evidence: prior 560-DIP/native-TOOL revision

Root executed the then-frozen final-fields scenario with all three flags above.
It used the previous 560-DIP default, hard-coded Chinese count/reason strings and
native static TOOL backend. The following results are not acceptance evidence
for the new English/600-DIP/local-wxTipWindow revision. Its actual artifacts are in
`build-dir/artifacts/font-usage-inventory-fields-supported-run3/`.

| Actual result | Evidence |
| --- | --- |
| Process exit 0; bounded automatic sub-scope passed, not complete GUI acceptance | `result.json` and `interaction-summary.json`: `interaction_passed=true`, `error=null`, `passed=false`, `full_acceptance=false`, `strict_f5_case_executed=false`; mode is `interaction-controls/background-supported-inputs`. |
| 347 check records: 339 true assertions, 0 false, 8 blocked; all 16 interaction phases in that historical revision executed | The two result files contain exact assertions/phases. Startup is not counted as an interaction phase; blocked records are not passed assertions. |
| Actual native `GDI` fragment, name-field Select All/Copy, both explicit full-list scopes and exact multi-record CRLF/count/all-reasons | `fragment-GDI-*.txt`, `fragment-preserved-through-*-oracle.json`, `multiple-names-exact-CRLF-oracle.json`, `multiple-full-exact-CRLF-and-reasons-oracle.json`. |
| 53 atomic menu snapshots; 42 exact replies and 11 earlier-publication replies during bounded convergence, with every final exact oracle satisfied | The 53 `*-response-*.json` files contain `clipboard_snapshot` and `exact_match`. All locked owner/sequence stability checks succeeded. This is not a claim that all 53 payloads were exact. |
| No measured sequence change during `GetClipboardData` in those 53 snapshots | Every snapshot has `SequenceAtOpen == SequenceAfterGetData == SequenceAfterCopy`. The API accounts for legal delayed rendering, but a GetData-time sequence transition was not empirically exercised by this run. |
| Static TOOL payloads and real same-content watched replacement fields | `tooltip-static-name.json`, `tooltip-static-status.json`, `static-tooltip-refresh-field-instances.json`, `static-tooltip-watch-refresh-copy-contract-oracle.json`. Registered payload evidence does not establish a visible bubble. |
| Default/wide/narrow/restored sizing and unchanged field selections/values; actual 32-row nested Edit wheel movement | `interaction-resize-*-bounds.json`, selected/unselected PNGs, `nested-Edit-wheel.json`, `interaction-long-list-before-wheel.png`, `interaction-long-list-after-wheel.png`. Pixel colors/alignment and original-PNG review remain separate root acceptance work. |
| Same-text selection/focus restoration, save/atomic-replace/create/rename/delete counts, root deletion/rebuild/later write, source replacement, real main-editor edit/Undo and owner close | Exact named phase assertions in `interaction-summary.json`; `same-text-watch-focus-selection.json` and the corresponding `*-oracle.json` files. No actual F5 was posted or claimed. |
| Four real controlled session registrations, four successful removals, final outstanding zero | `session-font-events.json`; independently launched fresh CLI in `root-font-cleanup-verification.json` confirms the fixture family is missing again, with `registered=4`, `removed=4`, `final_outstanding=0`. |

The eight blocked records are the two visible static-tooltip bubbles, three
actual-F5 requirements (tooltip refresh, missing root, unsaved main editor),
foreground validation, controlled warning-yellow rendering, and the grouped
locale/identity/real-keyboard/debounce/slow-scan/permission/live-settings/DPI/theme
gaps. The substituted same-content watcher phases passed; actual F5 did not run.

### Artifacts and honest gaps

`interaction-summary.json` and `result.json` deliberately retain
passed=false/full_acceptance=false because actual F5 and manual/environment
acceptance remain outstanding. Their separate `interaction_passed` reports only
the executed automatic scope, with mode/flag/strict-F5 execution metadata; a
blocked F5 is not included as a passed check. `rendering-review.json`
collects manual original-PNG review of red/blue/normal colors, name wrapping,
600-DIP default width, right-edge suffix alignment and clipping. Historical
native TOOL registration and current expected/HelpText/source strings do not close
local-bubble content, duration or closure coverage. Real keyboard/foreground,
warning-yellow variants, locale/identity variants, debounce blind-window and
slow-scan races, permissions/backend failures, live settings, multiple DPI/theme
variants remain explicit gaps rather than invented passes.

Native clipboard payload files are UTF-16LE **without BOM**; JSON is UTF-8 and
uses sanitized runtime paths. Exact equality uses original runtime values before
sanitization. Main-subtitle preservation checks compare decoded text, not
independent BOM/byte identity. The CLI fixture has separate SHA-256 checks.

The historical supported-run3 evidence applies only to its matching older
producer/driver and the explicit supported-input scope, not the current revised
localization, default width or local-tooltip lifetime. Strict
fields-interaction-run2/run3 and supported-run1/run2 remain failed artifacts;
they are not retroactively relabeled as passes. Earlier STC success, probe
collection, or compilation does not replace the executed evidence above.
Slopwatch was attempted for the C# changes but unavailable, not a passing
analysis. The historical i18n revision has actual i18n-gui-run2 bounded-interaction
and i18n-cli-run1 contract evidence, in addition to the preserved old-producer
width red. Its full GUI acceptance remains false, with the unmeasured tooltip,
real-input and environment boundaries above.
The stable product contract is summarized in
[the font usage memory](../../.github/Memories/font-usage-inventory.md).

## Warning-classification revision: preserved incomplete GUI validation

The later `font-usage-inventory-warning-safety-20261017` artifacts do not inherit
the historical GUI pass. Its `gui-green` run failed at the font-removal clipboard
oracle: the two actual replies still contained the registered-family name and
`Low usage`, while the exact expected payload required the original missing name
and `Not found; Low usage`. A third menu invocation produced no new clipboard
sequence before timeout. The producer log subsequently records a catalog rebuild
from 609 families/1023 faces to 608/1019; that is not proof of a published missing
row or a successful copy response.

The driver now captures the pre-removal public name/status identities and waits
for a new complete enabled pair with the exact original name and
`3 uses · Not found` before opening another copy popup. This preserves the exact
clipboard and missing-font assertions; it does not manufacture an observed row.
**This new wait was not reached by the final run and remains unvalidated.**

The bounded `gui-debug2` run attached CDB only to its owned host. It recorded the
font-debounce/GUI-completion functions but failed earlier at close/reopen, before
the removal phase. Pausing changes timing, so it did not establish the removal
refresh chain. The following `gui-final1` run, without CDB, failed at the same
close/reopen timeout. Both contain 341 check records: 332 true assertions,
9 blocked records, and a terminal timeout; neither interaction nor full GUI
acceptance passed. The close-timeout root cause remains unknown and must not be
attributed solely to CDB. The sandbox-only `gui-debug1` bootstrap failed to create
a desktop before an Aegisub host or session-font registration existed.

Both executed runs registered four fixture fonts and removed all four in
`finally`, with zero outstanding registrations. All recorded owned host/driver/CDB
PIDs were verified gone before releasing exclusivity. CDB was still waiting after
its host exited and was then ended by its recorded owned PID; no live process was
resurrected for another observation. In `gui-final1`, the independent fresh-process
`root-font-cleanup-verification.json` records exit zero and the controlled family
missing again, using the same executable fingerprint as the failed GUI run.
The earlier `gui-red` fixture-precondition failure and `gui-red2` actual
HelpText-unsupported failure remain preserved, not relabeled as content red/green
evidence. Actual visible tooltip content, matched-weight presentation and tooltip
duration/closure remain blocked; HelpText `NotSupported` is not a passing check.


## Native watcher recovery verification (2026-10-17)

Artifacts: `build-dir/artifacts/font-usage-inventory-gui-recovery-20261017/`.
The frozen harness includes the fixed eight-cycle `RootChurnInteraction` and a
subsequent real saved-file 7-to-8-to-7 transition. It preserves the original exact
clipboard, close/reopen, font-removal readiness/copy and owner-exit assertions.

- `gui-green1` and `gui-green2`: each exit 0, 368 true checks, zero false checks,
  twelve explicit blocked records, `interaction_passed=true`. Both completed all
  eight cycles, both later real writes, actual inventory destruction/reopen and
  actual font removal followed by exact missing-font clipboard output.
- Each run removed its four temporary session fonts with final balance zero.
  A fresh platform CLI process confirmed the fixture missing with count three;
  the recorded test-owned GUI/driver original process instances were gone.

The background reports keep `full_acceptance=false`. Background keyboard/tooltip,
other language/font/DPI/theme and native main/secondary reload limits remain as
recorded. The necessary shared invalidation unit had a genuine pre-fix behavior
failure, then passed in the complete 2826-test suite (2824 pass, two skips).
The shared watcher lifecycle and platform boundaries are summarized in
`.github/Memories/file-watcher-boundaries.md`. This background scope does not
claim the unexecuted keyboard, tooltip, provider-reload or platform checks above.
