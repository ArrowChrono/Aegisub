# Motion Apply publication fault native probe

Run from the repository root with an empty artifacts directory:

```powershell
dotnet run --file tests/gui-automation/motion-apply-publication-fault.cs -- build-dir/RelWithDebInfo/Aegisub.exe build-dir/artifacts/motion-apply-publication-fault
```

The driver starts one real isolated GUI-test FrameMain process per fault case,
loads dummy video/audio and a two-row subtitle document, and sends a native
WM_COMMAND to the existing wx boundary host. Each process has a 60-second total
supervisor. Individual startup and publication waits are also bounded. Test-only
startup enables the existing wx assertion channel.

- `motion-apply-early`: Exact replacement installs twenty target events; the
  latest-connected commit observer throws before normal commit observers run.
- `motion-apply-selection`: a successful Exact Apply first establishes the
  twenty-row group; Compact replaces it with one target event, normal commit
  observers run, then the latest-connected selection observer throws.

Each case also runs with Karaoke timing enabled (`-karaoke` suffix).

Two additional `-grid` variants run the same publication boundaries with pending
Grid capture instead of AudioDisplay capture.

All six cases call the same private installation/publication function used by the
real Motion Track dialog. The test seam supplies deterministic planner input,
not a duplicate apply algorithm. It establishes the real grid, edit box, Drag,
managed modeless Styling Assistant and AudioKaraoke. A deferred edit-text timer
is queued before the fault. A native audio marker mouse-down establishes an
AudioDisplay marker interaction borrowing timing-controller markers. After
retirement, ordinary native mouse-up must release capture; the test does not
force ReleaseCapture to conceal an incomplete owner cleanup. Borrowed row addresses are compared against live
Events membership without dereferencing retired addresses. A null borrow is
safe; a stale nonnull borrow fails independently of any proposed ready API.
The receipt also records whether the stale edit control can still take focus,
whether pending text survives, committed row count and undo availability.

After the fault, the native driver sends bounded EM_SETSEL/EM_REPLACESEL messages
to the real multiline subtitle editor, and WM_SETTEXT to Styling followed by
Styling Enter. EM_REPLACESEL generates the native edit change notification;
WM_SETTEXT alone does not do so for this multiline editor. This exercises native
message editing, not physical keyboard input. A fresh
sequence/token receipt proves text arrived while document rows and revision
remain unchanged. Focus eligibility and ready flags are diagnostic only, not a
substitute for that behavior check.

An optional case name argument runs one of the six cases in isolation for
diagnosis without changing the process deadline or assertions.

The driver observes the native Undo menu command, posts it, and checks that
borrowers rebind to live rows. Native subtitle input must then commit to the
rebound row and advance document revision. Undo restores the original clean
fixture before a normal close request; exit code zero is required. Forced
termination is used only for failure cleanup, not accepted shutdown evidence.

The scenario uses the native text editor (STC disabled), both Dialogue and
Karaoke timing modes, a pending AudioDisplay marker interaction, Drag, and the
real modeless Styling Assistant. It does not claim STC-specific input coverage. Drag feature-line pointers, selected-feature references,
cached selection, double-click selection, primary/active features and the
frame-visibility index are checked against live Events membership; populated
feature, selected-feature, cached-selection and visibility state is required
before each fault. AudioKaraoke presence is required,
not optional passing coverage. Those limitations must remain explicit until
representative native interactions and direct observations are added.

Artifacts retain all case requests/receipts, isolated profiles, startup logs and
`results.json`. Driver exit 1 means a behavioral invariant failed; setup errors
produce a case failure and retain failure.json rather than fabricating a pass.

## Captured Grid shutdown failure

The preserved pre-fix run in
`build-dir/artifacts/motion-apply-fault-final-06/motion-apply-selection-grid/host`
reported `grid_capture=true` in receipts 1 through 7. Receipt 2 follows the
ordinary native mouse-up; receipt 7 follows Undo restoring the clean fixture.
Retirement had cleared the Grid drag state, but normal commit publication rebuilt
its row caches before the selection observer threw. The ready mouse handler then
skipped its `holding`-conditional release, leaving native capture owned after the
interaction was retired. The early-commit Grid variant did not rebuild before
mouse-up and therefore exercised a different cleanup branch.

The isolated diagnostic retained
`build-dir/motion-fault-grid-debug-cdb-02.log`. Its main-thread stack shows
`FrameMain` teardown destroying `BaseGrid`, followed by a
`wxWindowBase` destructor assertion and a modal wx assertion dialog. Reentrant
native resize/layout during that partially destroyed-window state reaches
`wxWindowBase::DoGetBestSize`, `_purecall`, and `abort`. Together with the fresh
capture receipts, this identifies the stranded capture as the owning cleanup
failure; the clean-close timeout was not evidence of a slow background job.
The stack capture did not finish all threads before the immutable process
supervisor expired, so that diagnostic has no completed `results.json` and is not
an accepted shutdown run. Failure evidence is retained rather than replaced by
later acceptance output.
## Six-case validation before metadata correction

The isolated `motion-apply-selection-grid` case in
`build-dir/artifacts/motion-apply-fault-grid-final-01` passes after the Grid owner
moves retired-interaction mouse-up cleanup ahead of the row-readiness gate.
The subsequent full six-case run in
`build-dir/artifacts/motion-apply-fault-final-07` reports every case passed with
unchanged retired document state, delivered native text, successful rebind,
committed recovered input, clean Undo and exit code zero. No test-side capture
release or timeout extension is used.

The first receipt in each case still records two setup assertions from the
pre-existing Motion Track dialog grid-sizer declarations (`5 > 2*2`). Those
assertions are retained in the artifacts and are outside this row-borrow
retirement change. Passing these six cases does not claim an assertion-free
application or coverage of STC, actual Skia interactive input, non-Windows
platforms, or every visual-tool mode.

## File-load metadata recovery

`motion-apply-file-rebind` is an independent lifecycle case. After the early
publication fault and ordinary capture release, it loads a second two-row ASS
file through the normal `Project::LoadSubtitles` production path. The host seam
only bypasses the file chooser; this is not physical keyboard File Open input.
The loaded file has independent styles (`RebindOnly`, `RebindAlternate`), actor
(`LoadedActor`) and effect (`LoadedEffect`). The probe reads each real combobox's
`GetStrings()` candidate array and current value. Assertions require exact
candidate sets, active values, live borrows, ready owners, recovered native EDIT
commit, Undo to the clean loaded file and normal process exit.

The preserved RED baseline is
`build-dir/artifacts/motion-apply-metadata-red-01`: its load receipt
`motion-apply-file-rebind/host/wx-boundary-4.json` shows ready row owners and the
new actor/effect values, but the old style candidates and empty actor/effect
candidate arrays. `results.json` fails only `metadataRebound`; all other named
behavior checks pass and the process exits zero. The source/binary identity is
retained in that directory's `binding.json`, patch and driver copy. The six-case
acceptance above predates this metadata correction and is not a claim of having
rerun those cases against a later revision.


After the metadata owner correction, the same case passes in
`build-dir/artifacts/motion-apply-metadata-green-01`. Its load receipt
`motion-apply-file-rebind/host/wx-boundary-4.json` contains exactly the two loaded
styles and unique actor/effect candidates with the active loaded values.
`metadataRebound`, live/ready rebind, recovered native commit, clean Undo and
exit-zero checks all pass. This post-correction run intentionally executes only
the new independent case; it does not reset the bounded six-case run budget.

## Retired grid style cache recovery

Run only the independent `motion-apply-style-rebind` case by passing its name as
the optional third driver argument. It uses the early commit-observer fault,
changes the real grid font-size and background-color options while row borrows
remain retired, then restores the document through native Undo. The probe reads
the actual cached wxFont point size, measured line height and background wxBrush,
not just the requested option values. The assertions require those caches to
update before rebind and remain correct after ready recovery, alongside recovered
native edit, clean Undo and ordinary process exit. Real monitor DPI transitions
and compile-Skia rendering are not dynamically covered by this case.

The preserved RED baseline is `build-dir/artifacts/motion-apply-style-red-01`.
`wx-boundary-4.json` requests font size 13 and black background while the retired
grid keeps font size 8, line height 20 and white background. After native Undo,
`wx-boundary-5.json` is ready but still holds those stale caches. Both
`styleWhileRetired` and `styleRebound` fail; the other behavior checks and process
exit pass. The GREEN result in `build-dir/artifacts/motion-apply-style-green-01`
records font size 13, line height 31 and black background both before and after
rebind, with every named behavior check passing. Each directory retains its
source patch, driver copy, binary digest and result receipts. This isolated
RED/GREEN pair does not rerun or reset the earlier bounded case budgets.
