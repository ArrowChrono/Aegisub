#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Windows.Automation;
using Microsoft.Win32.SafeHandles;
using Aegisub.GuiAutomation.Driver;

var flags = args.Skip(2).ToArray();
if (args.Length is < 2 or > 5 || flags.Distinct(StringComparer.Ordinal).Count() != flags.Length
    || flags.Any(flag => flag is not ("--allow-session-font-changes" or "--probe-controls" or "--interaction-controls" or "--background-supported-inputs"))
    || (flags.Contains("--probe-controls", StringComparer.Ordinal) && flags.Length != 1)
    || (flags.Contains("--background-supported-inputs", StringComparer.Ordinal) && !flags.Contains("--interaction-controls", StringComparer.Ordinal)))
{
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/font-usage-inventory-native.cs -- EXE ARTIFACTS [--probe-controls | --interaction-controls [--background-supported-inputs] [--allow-session-font-changes] | --allow-session-font-changes]");
    return 2;
}
foreach (var path in args.Take(2))
    if (Path.IsPathRooted(path) || path.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar).Contains(".."))
        throw new ArgumentException("Use repository-relative executable and artifact paths without parent traversal");
try { return PrivateDesktop.Run(args, Path.GetFullPath(args[1]), isolation => new Scenario(args, isolation).Run()); }
catch (Exception error) { Console.Error.WriteLine(error.Message); return 1; }

sealed class Scenario(string[] args, PrivateDesktop isolation)
{
    const string FixtureFamily = "Aegisub GDI Variant Fixture";
    const string CopyMissingFamily = "ZZZ Aegisub Inventory Copy Absent Face 8F4D7A21";
    const string CopyMissingReasons = "Not found; Low usage";
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    readonly string artifacts = args[1];
    readonly string runtime = Path.Combine(args[1], "runtime");
    readonly List<object> checks = [];
    readonly List<string> registeredFonts = [];
    readonly List<object> fontEvents = [];
    readonly List<object> renderingReviews = [];
    readonly List<object> tooltipReviews = [];
    readonly object fontGate = new();
    readonly bool allowFonts = args.Skip(2).Contains("--allow-session-font-changes", StringComparer.Ordinal);
    readonly bool probeOnly = args.Skip(2).Contains("--probe-controls", StringComparer.Ordinal);
    readonly bool interactionMode = args.Skip(2).Contains("--interaction-controls", StringComparer.Ordinal);
    readonly bool backgroundSupportedInputs = args.Skip(2).Contains("--background-supported-inputs", StringComparer.Ordinal);
    Process? app, cli;
    nint main, inventory, records, recursive, threshold;
    string phase = "startup", family = "Arial", mainCaption = "";
    string source = "", originalAss = "";
    string? terminalFailure;
    int checkpoint;
    bool recursiveState;
    bool strictF5CaseExecuted;
    volatile bool timedOut;

    public int Run()
    {
        if (Directory.Exists(artifacts)) throw new InvalidOperationException("Use a fresh artifact directory");
        Directory.CreateDirectory(artifacts);
        isolation.RecordDriver();
        using var log = new StreamWriter(Path.Combine(artifacts, "driver.log")) { AutoFlush = true };
        Console.SetOut(log); Console.SetError(log);
        using var watchdog = new Timer(_ => {
            timedOut = true;
            terminalFailure = "120-second watchdog expired; forced cleanup is failure";
            Result(false, terminalFailure);
            try
            {
                if (cli is { HasExited: false }) cli.Kill(true);
                if (app is not null) isolation.Stop(app);
            }
            finally
            {
                try { UnregisterFonts(); }
                catch (Exception error) { Result(false, "Watchdog session-font cleanup failed: " + error.Message); }
                Environment.Exit(124);
            }
        }, null, 120_000, Timeout.Infinite);
        try
        {
            Prepare(); Start();
            if (probeOnly)
            {
                phase = "control_capability_probe"; Open(); Capture("probe-inventory");
                var input = Path.Combine(runtime, "copy-multi.ass");
                var sourceLabels = Native.Children(inventory).Where(handle => IsLiveOwned(handle) && Native.GetParent(handle) == inventory
                    && PrivateDesktop.ClassName(handle) == "Static" && Text(handle) == "No source selected").ToArray();
                if (sourceLabels.Length != 1) throw new InvalidOperationException("The baseline probe must identify exactly one actual direct-child source Static before submitting input");
                var sourceLabel = sourceLabels[0]; var sourceLabelId = Native.GetDlgCtrlID(sourceLabel);
                Write("probe-source-label-target.json", new { phase, hwnd = sourceLabel.ToString("X"), native_control_id = sourceLabelId, class_name = "Static",
                    baseline_text = "No source selected", direct_parent = inventory.ToString("X"), discovery_before_source_submission = true });
                Drop(input);
                Wait(() => {
                    isolation.VerifyWindow(sourceLabel, app!);
                    if (Native.GetParent(sourceLabel) != inventory || Native.GetDlgCtrlID(sourceLabel) != sourceLabelId || PrivateDesktop.ClassName(sourceLabel) != "Static")
                        throw new InvalidOperationException("The observed stable source Static changed identity while awaiting input acceptance");
                    return Text(sourceLabel) == Path.GetFileName(input);
                }, "The actual source label accepts the probe filename; this is not a scan-completion oracle");
                var sampling = Stopwatch.StartNew();
                foreach (var milliseconds in new[] { 500, 1500 })
                {
                    Wait(() => sampling.ElapsedMilliseconds >= milliseconds, "Reach the bounded read-only control-probe sample time");
                    var label = "probe-populated-" + milliseconds + "ms";
                    SnapshotControls(inventory, label); Capture(label);
                    Write(label + "-sample.json", new { phase, source = input, target_delay_milliseconds = milliseconds, elapsed_milliseconds = sampling.ElapsedMilliseconds,
                        source_label_guard = "input filename accepted only", scan_completion_or_count_or_copy_asserted = false, selection_or_pattern_action_invoked = false });
                }
                nint[] readonlyFields = []; var fieldsSeenWithinDeadline = false;
                Wait(() => {
                    if (sampling.ElapsedMilliseconds >= 10_000) return true;
                    readonlyFields = ProbeReadonlyFields();
                    fieldsSeenWithinDeadline = readonlyFields.Length >= 2 && sampling.ElapsedMilliseconds <= 10_000;
                    return fieldsSeenWithinDeadline || sampling.ElapsedMilliseconds >= 10_000;
                }, "Observe an actual readonly native Edit pair within the ten-second probe deadline");
                var finalLabel = fieldsSeenWithinDeadline ? "probe-readonly-fields-ready" : "probe-readonly-fields-timeout";
                Write("probe-readonly-fields-wait.json", new { phase, deadline_milliseconds_from_source_acceptance = 10_000, elapsed_milliseconds = sampling.ElapsedMilliseconds,
                    readonly_edit_count = readonlyFields.Length, readonly_edit_hwnds = readonlyFields.Select(handle => handle.ToString("X")).ToArray(),
                    pair_observed_within_deadline = fieldsSeenWithinDeadline, criterion = "at least two live owned native Edit children with ES_READONLY",
                    semantic_field_identity_or_count_or_copy_asserted = false });
                SnapshotControls(inventory, finalLabel); Capture(finalLabel);
                if (!fieldsSeenWithinDeadline) throw new TimeoutException("No actual readonly native Edit name/status pair became observable within ten seconds of source acceptance");
                Native.Post(main, 0x10);
                if (!app!.WaitForExit(10_000) || isolation.ExitCode(app) != 0) throw new InvalidOperationException("Control probe host did not exit normally within ten seconds");
                Write("probe-summary.json", new { completed = true, probe_only = true, behavioral_acceptance_run = false,
                    requested_control_contract = "independent readonly name/status text fields with native fragments and two full-list context actions", source_samples = new[] { "baseline", "input accepted plus 500ms", "input accepted plus 1500ms", "actual readonly native Edit pair observed within ten seconds" },
                    fonts_registered = false, clipboard_accessed = false, selection_or_pattern_actions_invoked = false,
                    result_interpretation = "Exit zero confirms evidence collection only; no inventory behavior has passed" });
                Result(false, "Control capability probe only; behavioral acceptance was not run");
                return 0;
            }
            if (!interactionMode) throw new InvalidOperationException("The original SCI scenario is retired after the explicit two-text-field contract change; use --interaction-controls or --probe-controls");
            return InteractionRoundTrip();
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(Scrub(error.ToString()));
            terminalFailure = error.ToString();
            Result(false, error.ToString());
            try { Capture("failure"); }
            catch (Exception captureError) { Write("failure-capture-error.json", new { error = captureError.ToString(), original_error_preserved = true }); Console.Error.WriteLine(Scrub(captureError.ToString())); }
            Result(false, error.ToString()); return 1;
        }
        finally
        {
            if (cli is { HasExited: false }) { cli.Kill(true); cli.WaitForExit(5_000); }
            if (app is not null) { isolation.Stop(app); app.Dispose(); }
            if (registeredFonts.Count > 0) UnregisterFonts();
        }
    }

    int InteractionRoundTrip()
    {
        phase = "interaction_V01_menu_singleton_nonmodal";
        Open(); var first = inventory; Open();
        Check(inventory == first && Windows().Count(h => Native.Caption(h) == "Font Usage List") == 1, "Repeated File entry activates one owned inventory window");
        Check(Native.IsWindowEnabled(main), "Main editor remains enabled while the nonmodal inventory is open");
        Check((Native.GetWindowLongPtr(inventory, -20).ToInt64() & 8) == 0, "Inventory does not have WS_EX_TOPMOST");
        Check(Native.GetWindow(inventory, 4) == main, "Inventory is owned by the Aegisub main window");
        Check(Text(threshold) == "5" && Send(recursive, 0xF0) == 0, "Fresh profile defaults are threshold five and nonrecursive; list copy format is an explicit menu action");
        var defaultBounds = Native.WindowRectangle(inventory);
        var dpi = Native.GetDpiForWindow(inventory);
        var expectedWidth = checked((int)((600L * dpi + 48) / 96));
        var expectedHeight = checked((int)((560L * dpi + 48) / 96));
        Write("default-window-size.json", new { phase, dpi, expected_width_dip = 600, expected_height_dip = 560,
            expected_width_px = expectedWidth, expected_height_px = expectedHeight,
            actual_width_px = defaultBounds.Right - defaultBounds.Left, actual_height_px = defaultBounds.Bottom - defaultBounds.Top,
            measured_surface = "outer_window_not_client_area" });
        Check(dpi != 0 && defaultBounds.Right - defaultBounds.Left == expectedWidth && defaultBounds.Bottom - defaultBounds.Top == expectedHeight,
            "The default outer inventory window is exactly 600 by 560 DIP at its actual GetDpiForWindow scale");
        ExpectNoInteractionCopy("initial-no-source");

        var b = Path.Combine(source, "b.ASS");
        if (allowFonts)
        {
            phase = "interaction_V15_real_session_font_registration";
            var missing = JsonRow(1, [b]);
            Check(missing.GetProperty("display_name").GetString() == FixtureFamily, "Unregistered dedicated family retains its independent missing-family display name");
            Check(missing.GetProperty("reasons").EnumerateArray().Select(reason => reason.GetProperty("code").GetString()).SequenceEqual(new[] { "missing_font", "low_usage" }), "Unregistered dedicated family has exactly the missing/low reason list; font-variant information is not a row warning");
            Drop(b); ExpectInteractionRecord(FixtureFamily, 1, CopyMissingReasons, "font-before-registration");
            ReviewInteraction("font-before-registration", "Original missing request name stays visible; red compact suffix is one use plus missing status, with detailed reasons reserved for hover");
            var matchedNames = missing.GetProperty("requests").EnumerateArray()
                .SelectMany(request => new[] { request.GetProperty("matched_name").GetString(), request.GetProperty("matched_full_name").GetString() })
                .Where(name => !string.IsNullOrEmpty(name)).Select(name => name!).Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal).ToArray();
            Check(matchedNames.SequenceEqual(new[] { "Arial" }), "The controlled short missing-family case has the same-platform CLI GDI name guard Arial; this guard is not rendered tooltip evidence");
            MissingTooltipInteraction(b, matchedNames[0]);
            ResizeSuffixInteraction(FixtureFamily, 1, CopyMissingReasons, "missing-one");
            phase = "interaction_V15_real_session_font_registration";
            RegisterFonts(); Native.Post(main, 0x1D);
        }

        phase = "interaction_V02_V03_three_five_and_thresholds";
        var row = JsonRow(3);
        Check(row.GetProperty("reasons").EnumerateArray().Select(reason => reason.GetProperty("code").GetString()).SequenceEqual(new[] { "low_usage" }), "Independent three-use fixture has exactly the low-usage reason at threshold five");
        var display = row.GetProperty("display_name").GetString()!;
        if (allowFonts)
        {
            Check(display == FixtureFamily + "<" + FixtureFamily + ">", "Registered CC0 fixture retains its fixed primary metadata display name");
            ExpectInteractionRecord(display, 1, "Low usage", "font-after-registration-count-unchanged");
            ReviewInteraction("font-after-registration", "The same one-use source now resolves to the installed dedicated family and low-frequency blue");
            MatchedWeightDetailsInteraction(b, display);
            ResizeSuffixInteraction(display, 1, "Low usage", "resolved-one");
        }
        else RecordBlocked("V04_hover_missing_details", "The short missing-family hover case requires the dedicated-family session-font opt-in and its absent-family precondition");
        phase = "interaction_V02_V03_three_five_and_thresholds";
        Drop(source); ExpectInteractionRecord(display, 3, "Low usage", "nonrecursive-three");
        ReviewInteraction("nonrecursive-three", "Exactly one three-use low-frequency blue record; compact source row and controls fit");
        SetThreshold("0"); ExpectInteractionRecord(display, 3, "", "three-threshold-zero");
        ReviewInteraction("three-threshold-zero", "Threshold zero disables low frequency; the three-use record has normal text color");
        SetThreshold("5"); ExpectInteractionRecord(display, 3, "Low usage", "three-threshold-five");
        SetThreshold("6"); ExpectInteractionRecord(display, 3, "Low usage", "three-threshold-six");
        Click(recursive); recursiveState = true;
        ExpectInteractionRecord(display, 5, "Low usage", "recursive-five-threshold-six");
        SetThreshold("5"); ExpectInteractionRecord(display, 5, "", "five-threshold-five-boundary");
        SetThreshold("0"); ExpectInteractionRecord(display, 5, "", "five-threshold-zero");
        SetThreshold("6"); ExpectInteractionRecord(display, 5, "Low usage", "five-threshold-six-boundary");

        phase = "interaction_V07_native_fragment_and_list_copy_scopes";
        if (display.Contains("GDI", StringComparison.Ordinal)) FragmentInteraction(display, 5, "Low usage");
        else RecordBlocked("V07_literal_GDI_fragment", "Exact GDI fragment requires the dedicated GDI fixture session-font variant; no whole-record copy substitutes for it");

        phase = "interaction_V07_multiple_records_exact_CRLF";
        Drop(Path.Combine(runtime, "copy-multi.ass")); recursiveState = false;
        ExpectInteractionCopy(display + "\r\n" + CopyMissingFamily, "multiple-names-exact-CRLF", true);
        ExpectInteractionCopy(FullInteractionRecord(display, 1, "Low usage") + "\r\n" + FullInteractionRecord(CopyMissingFamily, 1, CopyMissingReasons), "multiple-full-exact-CRLF-and-reasons");
        var selectionRows = RequireFieldRows([(display, "1 use · Low usage"), (CopyMissingFamily, "1 use · Not found")]);
        MultiFieldSelectionInteraction(selectionRows);
        ReviewInteraction("multiple-full-records", "Two independent readonly name/status pairs; status fields right-aligned, known low-frequency blue, missing red; no selected text obscures colors");
        LongListWheelInteraction();

        phase = "interaction_V08_watch_write_atomic_create_rename_delete";
        Drop(source); recursiveState = true; SetThreshold("5");
        ExpectInteractionRecord(display, 5, "", "watch-baseline-five");
        SameTextRefreshInteraction(display, 5, "");
        phase = "interaction_V08_watch_write_atomic_create_rename_delete";
        File.AppendAllText(b, "Dialogue: 0,0:00:07.00,0:00:08.00,Default,watch-add,0,0,0,,A\n");
        ExpectInteractionRecord(display, 6, "", "watch-write-six");
        var temp = b + ".tmp";
        File.WriteAllText(temp, File.ReadAllText(b) + "Dialogue: 0,0:00:09.00,0:00:10.00,Default,atomic-add,0,0,0,,A\n");
        File.Move(temp, b, true); ExpectInteractionRecord(display, 7, "", "watch-atomic-replace-seven");
        var added = Path.Combine(source, "created.ass");
        File.Copy(Path.Combine(source, "nested", "c.ass"), added); ExpectInteractionRecord(display, 9, "", "watch-create-nine");
        var excludedName = Path.Combine(source, "renamed.txt");
        File.Move(added, excludedName); ExpectInteractionRecord(display, 7, "", "watch-rename-out-seven");
        var renamed = Path.Combine(source, "renamed.ASS");
        File.Move(excludedName, renamed); ExpectInteractionRecord(display, 9, "", "watch-rename-in-nine");
        File.Delete(renamed); ExpectInteractionRecord(display, 7, "", "watch-delete-seven");

        phase = backgroundSupportedInputs ? "interaction_V09_deleted_root_watch_rebuild_and_later_write" : "interaction_V09_deleted_root_F5_rebuild_and_later_write";
        var backup = Path.Combine(runtime, "interaction-root-backup"); CopyDirectory(source, backup);
        RemoveRuntimeTree(source); ExpectNoInteractionCopy("deleted-root");
        if (backgroundSupportedInputs)
            RecordBlocked("V09_actual_F5_while_root_missing", "Explicit background-supported-inputs scope does not post the known unsupported F5 key route; deleted-root unavailability, recovery and later watch changes remain tested");
        else { F5(); ExpectNoInteractionCopy("F5-while-root-missing"); }
        ReviewInteraction("deleted-root-status", backgroundSupportedInputs
            ? "Root removal shows an unavailable/failure status; actual F5 is blocked and not executed in this scope; the status text is not automatically readable"
            : "Root removal and F5 while missing show an unavailable/failure status; the status text is not automatically readable");
        CopyDirectory(backup, source); ExpectInteractionRecord(display, 7, "", "rebuilt-root-seven");
        var savedB = File.ReadAllText(b);
        File.AppendAllText(b, "Dialogue: 0,0:00:11.00,0:00:12.00,Default,after-root-rebuild,0,0,0,,A\n");
        ExpectInteractionRecord(display, 8, "", "post-rebuild-watch-write-eight");
        File.WriteAllText(b, savedB); ExpectInteractionRecord(display, 7, "", "post-rebuild-restored-seven");

        RootChurnInteraction(display, backup, b);

        phase = "interaction_V02_source_replacement_and_non_ASS_retention";
        var a = Path.Combine(source, "a.ass"); Drop(a); recursiveState = false;
        ExpectInteractionRecord(display, 2, "Low usage", "single-source-replacement-two");
        Check(!Native.IsWindowEnabled(recursive), "A file-only replacement disables recursion without losing its remembered value");
        Drop(Path.Combine(runtime, "not-ass.txt")); ExpectInteractionRecord(display, 2, "Low usage", "invalid-only-drop-retains-two");
        ReviewInteraction("invalid-only-drop", "A non-ASS-only drop retains the previous two-use source and shows an invalid-source error");
        Drop(a, b, a, Path.Combine(runtime, "not-ass.txt"));
        ExpectInteractionRecord(display, 5, "", "mixed-drop-deduplicates-paths-five");
        Drop(source); recursiveState = true; ExpectInteractionRecord(display, 7, "", "directory-replacement-restores-seven");
        Check(Send(recursive, 0xF0) == 1, "Directory replacement restores the remembered recursive setting");

        phase = "interaction_V01_unsaved_main_editor_edit_real_undo";
        var undo = ObservedWindowsCommand.FindMenuCommand(main, app!.Id, "Edit", "Nothing to undo");
        var editors = Native.Children(main).Where(h => Native.IsWindowVisible(h) && PrivateDesktop.ClassName(h) == "Edit"
            && (Native.GetWindowLongPtr(h, -16).ToInt64() & 0x804) == 4 && Text(h) == "A").ToArray();
        Check(editors.Length == 1, "The saved fixture identifies one writable native multiline subtitle editor");
        Native.ReplaceText(editors[0], "{\\fnAegisub Inventory Live Unsaved Face}B");
        Wait(() => Text(editors[0]) == "{\\fnAegisub Inventory Live Unsaved Face}B" && Native.Caption(main) != mainCaption, "The main editor accepts an actual unsaved edit with the nonmodal inventory open");
        if (backgroundSupportedInputs)
        {
            RecordBlocked("V01_actual_F5_with_unsaved_main_editor", "Explicit background-supported-inputs scope does not post the known unsupported F5 key route; a real same-content source write exercises watched disk-source refresh instead");
            var before = RequireFieldRows([(display, "7 uses")]).Single();
            var oldName = ReadContentIdentity(before.Name) ?? throw new InvalidOperationException("Unsaved-editor watch baseline name field lost its live identity");
            var oldStatus = ReadContentIdentity(before.Status) ?? throw new InvalidOperationException("Unsaved-editor watch baseline status field lost its live identity");
            File.WriteAllText(b, File.ReadAllText(b));
            Wait(() => {
                var rows = ReadFieldRows();
                return rows is { Length: 1 } && rows[0].NameText == display && rows[0].StatusText == "7 uses"
                    && ReadContentIdentity(rows[0].Name) is { Enabled: true } name && name != oldName
                    && ReadContentIdentity(rows[0].Status) is { Enabled: true } status && status != oldStatus;
            }, "A real same-content watched source write publishes different complete enabled field identities while the main editor has an unsaved edit");
            ExpectInteractionRecord(display, 7, "", "unsaved-editor-watch-still-disk-seven");
        }
        else { F5(); ExpectInteractionRecord(display, 7, "", "unsaved-editor-F5-still-disk-seven"); }
        Check(File.ReadAllText(Path.Combine(runtime, "editor.ass")) == originalAss, "Inventory refresh does not save the real unsaved main-editor edit");
        ObservedWindowsCommand.PostMenuCommand(main, app.Id, undo);
        Wait(() => Text(editors[0]) == "A" && Native.Caption(main) == mainCaption, "The actual Edit/Undo command restores subtitle text and clean document state");

        phase = "interaction_V10_close_reopen_persists_settings_without_sources";
        SetThreshold("6");
        Native.Post(inventory, 0x10); Wait(() => !Native.IsWindow(inventory), "The inventory closes without terminating its owner");
        Open();
        Check(Text(threshold) == "6" && Send(recursive, 0xF0) == 1, "Threshold and recursive preference survive inventory reopen; the removed checkbox is not a preference");
        ExpectNoInteractionCopy("reopened-without-source");
        ReviewInteraction("reopened-without-source", "Reopened inventory has no source and no stale records; controls retain threshold six and remembered recursion");

        if (allowFonts)
        {
            phase = "interaction_V15_real_session_font_removal";
            SetThreshold("5"); Drop(b); recursiveState = false;
            ExpectInteractionRecord(display, 3, "Low usage", "font-before-removal-three");
            var beforeRemoval = RequireFieldRows([(display, "3 uses · Low usage")]).Single();
            var oldName = ReadContentIdentity(beforeRemoval.Name) ?? throw new InvalidOperationException("Font-removal baseline name field lost its live identity");
            var oldStatus = ReadContentIdentity(beforeRemoval.Status) ?? throw new InvalidOperationException("Font-removal baseline status field lost its live identity");
            UnregisterFonts(); Native.Post(main, 0x1D);
            FieldRow? afterRemoval = null;
            Wait(() => {
                var current = ReadFieldRows();
                if (current is not { Length: 1 }) return false;
                var candidate = current[0];
                if (candidate.NameText != FixtureFamily || candidate.StatusText != "3 uses · Not found"
                    || ReadContentIdentity(candidate.Name) is not { Enabled: true } name || name == oldName
                    || ReadContentIdentity(candidate.Status) is not { Enabled: true } status || status == oldStatus) return false;
                afterRemoval = candidate; return true;
            }, "Actual font removal publishes a new complete enabled missing-font pair before opening any copy popup");
            Write("font-removal-public-ready.json", new { phase, old_name = oldName, old_status = oldStatus,
                actual_name = ReadContentIdentity(afterRemoval!.Name), actual_status = ReadContentIdentity(afterRemoval.Status),
                observed_surface = "actual complete enabled native Edit values and public control identities", copy_popup_opened_during_transition = false });
            ExpectInteractionRecord(FixtureFamily, 3, CopyMissingReasons, "font-after-removal-count-unchanged-three");
            ReviewInteraction("font-after-removal", "The same three-use source becomes missing/red with a compact missing suffix after actual session font removal; complete clipboard reasons remain unchanged and no F5 was used");
        }
        else RecordBlocked("V15", "No --allow-session-font-changes opt-in: no real session font-set mutation was performed");

        phase = "interaction_V01_owner_close_and_read_only_main_subtitle";
        Check(File.ReadAllText(Path.Combine(runtime, "editor.ass")) == originalAss, "All drops, watches, font changes and refreshes leave the active editor's saved subtitle unchanged");
        Check(Native.Caption(main) == mainCaption, "Inventory input changes neither replace nor dirty the main subtitle");
        RecordBlocked("V01_foreground", "Foreground validation is outside the authorized background-only run; no desktop switch or global SendInput is used");
        RecordBlocked("V04_warning_yellow", "No controlled regular-only synthetic-variant interaction stage is included in this batch; yellow warning rendering remains unverified");
        RecordBlocked("V05_V06_V07_V09_V10_V11_V14", "Locale/identity variants, real keyboard Ctrl+A/C routing, debounce blind-window/slow-scan races, permissions/backend failures and live font/DPI/theme settings remain unproved; rendered colors/layout require separate PNG review");
        Native.Post(main, 0x10);
        if (!app!.WaitForExit(10_000) || isolation.ExitCode(app) != 0) throw new InvalidOperationException("Interaction host did not exit normally within ten seconds");
        Check(!Native.IsWindow(inventory), "Owned inventory is destroyed with its main window");
        Result(true); return 0;
    }

    static string FullInteractionRecord(string display, int count, string reasons) => display + "\t" + count + (count == 1 ? " use" : " uses") + (reasons.Length == 0 ? "" : "\t" + reasons);

    sealed record FieldRow(nint Parent, nint Name, nint Status, string NameText, string StatusText);
    sealed record ContentIdentity(string Hwnd, int ControlId, string ParentHwnd, int ParentControlId, string ClassName, long Style, string Value, bool Enabled);

    ContentIdentity? ReadContentIdentity(nint handle)
    {
        if (!IsLiveOwned(handle)) return null;
        try
        {
            isolation.VerifyWindow(handle, app!);
            var parent = Native.GetParent(handle);
            return new ContentIdentity(handle.ToString("X"), Native.GetDlgCtrlID(handle), parent.ToString("X"), Native.GetDlgCtrlID(parent),
                PrivateDesktop.ClassName(handle), Native.GetWindowLongPtr(handle, -16).ToInt64(), Text(handle), Native.IsWindowEnabled(handle));
        }
        catch (InvalidOperationException error) when (error.Message == "Target HWND is not owned by the live private-desktop host" && app is { HasExited: false } && !IsLiveOwned(handle)) { return null; }
        catch (Win32Exception error) when (error.NativeErrorCode == 1400 && !Native.IsWindow(handle)) { return null; }
    }

    FieldRow[]? ReadFieldRows()
    {
        var cells = new List<(nint Handle, nint Parent, long Style, string Text)>();
        foreach (var field in ProbeReadonlyFields())
        {
            if (!Native.IsWindowVisible(field)) continue;
            try
            {
                isolation.VerifyWindow(field, app!);
                var parent = Native.GetParent(field);
                if (Native.GetParent(parent) != records) throw new InvalidOperationException("Readonly record Edit is not in an actual direct-child row panel");
                cells.Add((field, parent, Native.GetWindowLongPtr(field, -16).ToInt64(), Text(field)));
            }
            catch (InvalidOperationException error) when (error.Message == "Target HWND is not owned by the live private-desktop host" && app is { HasExited: false } && !IsLiveOwned(field))
            {
                Write("field-render-lifetime-race-" + ++checkpoint + ".json", new { phase, hwnd = field.ToString("X"), live_owned_at_exception_filter = false, snapshot_status = "incomplete_visible_field_snapshot_not_empty", error = error.Message });
                return null;
            }
            catch (Win32Exception error) when (error.NativeErrorCode == 1400 && app is { HasExited: false } && !Native.IsWindow(field))
            {
                Write("field-render-lifetime-race-" + ++checkpoint + ".json", new { phase, hwnd = field.ToString("X"), native_error_code = error.NativeErrorCode,
                    host_alive_at_exception_filter = true, window_exists_at_exception_filter = false, snapshot_status = "incomplete_visible_field_snapshot_not_empty", error = error.Message });
                return null;
            }
        }
        var groups = cells.GroupBy(cell => cell.Parent).ToArray();
        if (groups.Any(group => group.Count() != 2))
        {
            Write("field-groups-incomplete.json", new { phase, status = "incomplete_visible_field_snapshot_not_empty", groups = groups.Select(group => new {
                parent = group.Key.ToString("X"), count = group.Count(), fields = group.Select(cell => new { hwnd = cell.Handle.ToString("X"), cell.Style, cell.Text }) }) });
            return null;
        }
        return groups.Select(group => {
            var name = group.Single(cell => (cell.Style & 3) == 0); var status = group.Single(cell => (cell.Style & 3) == 2);
            return new FieldRow(group.Key, name.Handle, status.Handle, name.Text, status.Text);
        }).OrderBy(row => Native.WindowRectangle(row.Name).Top).ToArray();
    }

    FieldRow[] RequireFieldRows((string Name, string Status)[] expected)
    {
        FieldRow[]? rows = null;
        Wait(() => { rows = ReadFieldRows(); return rows is not null && rows.Select(row => (row.NameText, row.StatusText)).SequenceEqual(expected); }, "Every visible field group is complete and all native readonly pairs equal the independent published oracle");
        foreach (var row in rows!)
        {
            foreach (var field in new[] { row.Name, row.Status })
            {
                var style = Native.GetWindowLongPtr(field, -16).ToInt64();
                Check((style & 0x804) == 0x804 && (style & 0x00800000) == 0, "Actual record Edit is multiline, read-only and borderless");
            }
            Check((Native.GetWindowLongPtr(row.Status, -16).ToInt64() & 3) == 2, "The actual status Edit uses native right alignment");
        }
        Write("field-pairs-" + ++checkpoint + ".json", new { phase, expected = expected.Select(value => new { name = value.Name, status = value.Status }), rows = rows.Select(row => new { parent = row.Parent.ToString("X"), name_hwnd = row.Name.ToString("X"), status_hwnd = row.Status.ToString("X"), row.NameText, row.StatusText }) });
        return rows;
    }

    static string SummaryText(int count, string reasons) => count + (count == 1 ? " use" : " uses") + (reasons.Contains("Not found", StringComparison.Ordinal) ? " · Not found" : reasons.Contains("Missing glyphs", StringComparison.Ordinal) ? " · Missing glyphs" : reasons.Contains("Warning", StringComparison.Ordinal) ? " · Warning" : reasons.Contains("Low usage", StringComparison.Ordinal) ? " · Low usage" : "");
    (int Start, int End) CellSelection(nint field) { var packed = unchecked((uint)Send(field, 0xB0)); return ((int)(packed & 0xFFFF), (int)(packed >> 16)); }
    bool HasCellSelection(nint field) { var selection = CellSelection(field); return selection.Start != selection.End; }
    void SetCellSelection(nint field, int start, int end)
    {
        Send(field, 0xB1, start, end);
        Check(CellSelection(field) == (start, end), "Public native EM_SETSEL/EM_GETSEL establish the exact field character range");
    }
    string CopyField(nint field, string label)
    {
        var before = Native.GetClipboardSequenceNumber(); Send(field, 0x301);
        Wait(() => Native.GetClipboardSequenceNumber() != before, "The actual native Edit WM_COPY publishes its selected fragment");
        var value = ClipboardText.ReadUnicodeText(Native.GetClipboardSequenceNumber(), app!.Id);
        File.WriteAllText(Path.Combine(artifacts, label + ".txt"), value, new UnicodeEncoding(false, false));
        return value;
    }

    void FragmentInteraction(string display, int count, string reasons)
    {
        var row = RequireFieldRows([(display, SummaryText(count, reasons))]).Single();
        var start = display.IndexOf("GDI", StringComparison.Ordinal);
        Check(start >= 0 && display != "GDI", "The independent fixture has a proper GDI substring, not a whole-record alias");
        SetCellSelection(row.Name, start, start + 3);
        Check(CopyField(row.Name, "fragment-GDI-before-list-actions") == "GDI", "Native field WM_COPY returns exactly the proper GDI substring");
        ExpectInteractionCopy(display, "fragment-preserved-through-names-list", true, row.Name);
        Check(CellSelection(row.Name) == (start, start + 3) && CopyField(row.Name, "fragment-GDI-after-names-list") == "GDI", "Names-list action does not change native fragment selection or its copy scope");
        ExpectInteractionCopy(FullInteractionRecord(display, count, reasons), "fragment-preserved-through-full-list", false, row.Status);
        Check(CellSelection(row.Name) == (start, start + 3) && CopyField(row.Name, "fragment-GDI-after-full-list") == "GDI", "Full-list action does not change native fragment selection or its copy scope");
        Check(InvokeActualMenu(row.Name, "Select All"), "Actual field Select All is enabled");
        Check(CellSelection(row.Name) == (0, display.Length), "The actual field Select All selects its full text only");
        var before = Native.GetClipboardSequenceNumber();
        Check(InvokeActualMenu(row.Name, "Copy"), "Actual field Copy is enabled for its selected text");
        Wait(() => Native.GetClipboardSequenceNumber() != before, "Actual field context Copy publishes selected field text");
        Check(ClipboardText.ReadUnicodeText(Native.GetClipboardSequenceNumber(), app!.Id) == display, "Field context Select All/Copy excludes every status, reason and other field");
        SetCellSelection(row.Name, 0, 0); before = Native.GetClipboardSequenceNumber(); Send(row.Name, 0x301);
        Check(Native.GetClipboardSequenceNumber() == before, "Native field WM_COPY with an empty selection does not change the clipboard");
        Check(Text(row.Name) == display && Text(row.Status) == SummaryText(count, reasons) && (Native.GetWindowLongPtr(row.Name, -16).ToInt64() & 0x800) != 0, "Fragment and list copying never alter field values or readonly state");
        ReviewInteraction("native-fragment-and-list-scopes", "Actual readonly name and status fields; no row-selection or double-click mode");
    }

    void MultiFieldSelectionInteraction(FieldRow[] rows)
    {
        phase = "interaction_V07_multiple_field_selection_exclusivity";
        var first = rows[0]; var second = rows[1];
        SetCellSelection(first.Name, 0, Math.Min(3, first.NameText.Length));
        Send(second.Name, 0x201, 1, Native.Position(8, 8)); Send(second.Name, 0x202, 0, Native.Position(8, 8));
        Check(!HasCellSelection(first.Name), "Selecting another row name collapses the prior name selection");
        Check(Native.ThreadFocus(second.Name) == second.Name, "The newly selected row name owns native thread focus");

        SetCellSelection(second.Name, 0, Math.Min(3, second.NameText.Length));
        Send(second.Status, 0x201, 1, Native.Position(8, 8)); Send(second.Status, 0x202, 0, Native.Position(8, 8));
        Check(!HasCellSelection(second.Name), "Selecting the same row status collapses its name selection");
        Check(Native.ThreadFocus(second.Status) == second.Status, "The same-row status field owns native thread focus");

        var statusStart = second.StatusText.IndexOf("use", StringComparison.Ordinal);
        Check(statusStart >= 0, "The independent status fixture contains a selectable use fragment");
        SetCellSelection(second.Status, statusStart, statusStart + 3);
        Send(second.Name, 0x201, 1, Native.Position(8, 8)); Send(second.Name, 0x202, 0, Native.Position(8, 8));
        SetCellSelection(second.Name, 0, Math.Min(3, second.NameText.Length));
        Check(Native.ThreadFocus(second.Name) == second.Name && HasCellSelection(second.Name), "A name fragment is selected before invoking the status context menu");
        Check(InvokeActualMenu(second.Status, "Select All"), "The actual nonfocused status field exposes Select All");
        Check(CellSelection(second.Status) == (0, second.StatusText.Length), "Right-click Select All selects the nonfocused status field text");
        Check(!HasCellSelection(first.Name) && !HasCellSelection(second.Name) && !HasCellSelection(first.Status),
            "Right-click Select All clears the previously selected name field");

        Send(second.Name, 0x201, 1, Native.Position(8, 8)); Send(second.Name, 0x202, 0, Native.Position(8, 8));
        Check(!HasCellSelection(second.Status), "Selecting the name after status Select All clears the menu-selected status range");

        Check(Native.ThreadFocus(second.Name) == second.Name, "The status Copy context action starts with focus in another field");
        SetCellSelection(second.Status, statusStart, statusStart + 3);
        Check(Native.ThreadFocus(second.Name) == second.Name, "Selecting the status fragment does not move focus from the name field");
        var before = Native.GetClipboardSequenceNumber();
        Check(InvokeActualMenu(second.Status, "Copy"), "The actual status context Copy action is enabled for its selection");
        Wait(() => Native.GetClipboardSequenceNumber() != before, "The status context Copy action publishes its selected fragment");
        Check(ClipboardText.ReadUnicodeText(Native.GetClipboardSequenceNumber(), app!.Id) == "use", "Right-click Copy publishes the selected field fragment only");
        Check(CellSelection(second.Status) == (statusStart, statusStart + 3), "Right-click Copy preserves the selected status fragment");
        Check(!HasCellSelection(first.Name) && !HasCellSelection(second.Name) && !HasCellSelection(first.Status),
            "Right-click Copy leaves the selected fragment exclusive to its field");
    }

    void SameTextRefreshInteraction(string display, int count, string reasons)
    {
        phase = "interaction_V08_same_text_watch_selection_focus_restoration";
        var row = RequireFieldRows([(display, SummaryText(count, reasons))]).Single();
        var start = display.IndexOf("GDI", StringComparison.Ordinal); if (start < 0) start = 1;
        var end = Math.Min(display.Length, start + 3);
        var status = row.StatusText;
        var statusStart = status.IndexOf("use", StringComparison.Ordinal);
        Check(statusStart >= 0, "The same-text refresh status fixture contains a selectable use fragment");
        SetCellSelection(row.Status, statusStart, statusStart + 3);
        Send(row.Name, 0x201, 1, Native.Position(8, 8)); Send(row.Name, 0x202, 0, Native.Position(8, 8));
        Check(!HasCellSelection(row.Status), "Selecting the name before refresh clears the prior status selection");
        Check(Native.ThreadFocus(row.Name) == row.Name, "A background native click gives this real field thread-local focus without foreground input");
        SetCellSelection(row.Name, start, end);
        var originalIdentity = ReadContentIdentity(row.Name) ?? throw new InvalidOperationException("Same-text watch baseline field lost its live identity");
        var b = Path.Combine(source, "b.ASS"); var text = File.ReadAllText(b);
        File.WriteAllText(b, text);
        FieldRow? refreshed = null;
        Wait(() => { refreshed = ReadFieldRows()?.SingleOrDefault(value => value.NameText == display && value.StatusText == SummaryText(count, reasons)); return refreshed is not null && ReadContentIdentity(refreshed.Name) is { Enabled: true } current && Native.IsWindowEnabled(refreshed.Status) && current != originalIdentity; }, "A real same-content watched write publishes different enabled public native fields with unchanged key/text");
        Check(CellSelection(refreshed!.Name) == (start, end), "The same-key same-text native field restores its exact character selection after watched refresh");
        Check(!HasCellSelection(refreshed.Status), "Same-text refresh restores only the selected field, leaving its paired status field unselected");
        Check(Native.ThreadFocus(refreshed.Name) == refreshed.Name, "The replacement field restores the actual thread-local focus after watched refresh");
        Check(CopyField(refreshed.Name, "same-text-watch-restored-fragment") == display[start..end], "Restored selection copies the exact same native fragment after publication");
        Write("same-text-watch-focus-selection.json", new { phase, before_instance = originalIdentity, after_instance = ReadContentIdentity(refreshed.Name), start, end, expected_text = display, expected_fragment = display[start..end], foreground_or_real_keyboard_tested = false });
        ExpectInteractionRecord(display, count, reasons, "same-text-watch-count-and-reasons-unchanged");
    }

    void LongListWheelInteraction()
    {
        phase = "interaction_V14_nested_native_Edit_mousewheel_scroll";
        var header = File.ReadAllText(Path.Combine(source, "b.ASS")).Split("Dialogue:")[0];
        var names = Enumerable.Range(0, 32).Select(index => "ZZZ Inventory Scroll Absent Fixture " + index.ToString("D2")).ToArray();
        var input = Path.Combine(runtime, "scroll-list.ass");
        File.WriteAllText(input, header + string.Concat(names.Select(name => "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\fn" + name + "}A\n")));
        Drop(input); ExpectInteractionCopy(string.Join("\r\n", names), "long-list-names-independent-order", true);
        FieldRow[]? rows = null;
        Wait(() => { rows = ReadFieldRows(); return rows is not null && rows.Select(row => row.NameText).SequenceEqual(names); }, "All independently named long-list fields and every visible field group are complete before wheel observation");
        var first = rows![0]; var before = Native.WindowRectangle(first.Name); var point = Native.ToScreen(first.Name, 8, 8);
        Capture("interaction-long-list-before-wheel");
        Send(first.Name, 0x20A, unchecked((int)(-360 << 16)), Native.Position(point.X, point.Y));
        Wait(() => Native.WindowRectangle(first.Name).Top < before.Top, "A standard native wheel event delivered to the nested Edit scrolls the containing list");
        var after = Native.WindowRectangle(first.Name);
        Check(Text(first.Name) == names[0], "Wheel forwarding does not modify the readonly name field");
        Capture("interaction-long-list-after-wheel");
        Write("nested-Edit-wheel.json", new { phase, target = first.Name.ToString("X"), wheel_delta = -360, screen_point = new { point.X, point.Y }, before_top = before.Top, after_top = after.Top, foreground_or_global_pointer_input = false });
        Send(records, 0x115, 6);
    }

    void MissingTooltipInteraction(string input, string matchedName)
    {
        phase = backgroundSupportedInputs ? "interaction_V04_local_tip_observation_watch_refresh" : "interaction_V04_local_tip_observation_refresh";
        var expected = FixtureFamily + "\n1 use\nReasons: Low usage; Not found\nASS requested names: " + FixtureFamily
            + "\nGDI returned/matched names: " + matchedName + "\nSources:\n" + Path.GetFullPath(input) + " · Line 11";
        Write("tooltip-independent-oracle.json", new { expected, original_ass_name = FixtureFamily, usage_count = 1, source = input, ass_line = 11,
            reason_codes = new[] { "missing_font", "low_usage" }, matched_name = matchedName, matched_name_source = "same-platform CLI cross-check only",
            current_backend = "local wxTipWindow", expected_text_is_rendered_evidence = false, legacy_static_TOOL_oracle = "retired_by_user_contract_change" });
        var row = RequireFieldRows([(FixtureFamily, "1 use · Not found")]).Single();
        RecordBlocked("V04_legacy_static_native_TOOL_oracle", "The user-approved local wxTipWindow replaces native static TOOL registration; earlier registered-text evidence is historical, not a passing current tooltip check");
        ObserveLocalTip(row.Name, expected);
        var oldName = ReadContentIdentity(row.Name) ?? throw new InvalidOperationException("Tooltip refresh baseline name field lost its live identity");
        var oldStatus = ReadContentIdentity(row.Status) ?? throw new InvalidOperationException("Tooltip refresh baseline status field lost its live identity");
        if (backgroundSupportedInputs)
        {
            RecordBlocked("V04_actual_F5_tooltip_refresh", "Explicit background-supported-inputs scope does not post the known unsupported F5 key route; a real same-content source write exercises watched tooltip refresh instead");
            File.WriteAllText(input, File.ReadAllText(input));
            Wait(() => {
                var rows = ReadFieldRows();
                return rows is { Length: 1 } && rows[0].NameText == FixtureFamily && rows[0].StatusText == "1 use · Not found"
                    && ReadContentIdentity(rows[0].Name) is { Enabled: true } name && name != oldName
                    && ReadContentIdentity(rows[0].Status) is { Enabled: true } status && status != oldStatus;
            }, "A real same-content watched write publishes a complete enabled replacement name/status pair before the same-output copy or tooltip oracle");
        }
        else F5();
        ExpectInteractionRecord(FixtureFamily, 1, CopyMissingReasons, backgroundSupportedInputs ? "local-tip-watch-refresh-copy-contract" : "local-tip-refresh-copy-contract");
        var refreshed = RequireFieldRows([(FixtureFamily, "1 use · Not found")]).Single();
        var newName = ReadContentIdentity(refreshed.Name); var newStatus = ReadContentIdentity(refreshed.Status);
        Write("local-tip-refresh-field-instances.json", new { phase, oldName, oldStatus, newName, newStatus, same_hwnd_alone_is_not_instance_identity = true });
        Check(newName is not null && newName != oldName && newStatus is not null && newStatus != oldStatus, "Refresh publishes different complete public name/status field identities, even if a numeric HWND is reused");
        RecordBlocked("V04_local_tip_refresh_closure", "The watched replacement fields and exact list payload are observed, but closing a previously visible local tooltip on refresh requires an actual shown tooltip; expected/source/help text is not that evidence");
    }

    void MatchedWeightDetailsInteraction(string input, string display)
    {
        phase = "interaction_V04_registered_matched_weight_accessibility_details";
        var resolved = JsonRow(1, [input]);
        var requests = resolved.GetProperty("requests").EnumerateArray().ToArray();
        Check(!resolved.GetProperty("missing_font").GetBoolean() && resolved.GetProperty("display_name").GetString() == FixtureFamily + "<" + FixtureFamily + ">",
            "The weight-detail fixture is the session-registered dedicated family, not an unresolved fallback name");
        Check(requests.Length == 1 && requests[0].GetProperty("ass_name").GetString() == FixtureFamily
            && requests[0].GetProperty("matched_name").GetString() == FixtureFamily && requests[0].GetProperty("matched_weight").GetInt32() == 400,
            "The single regular request resolves to the registered CC0 fixture with its fixed OS/2 matched weight 400");
        Check(resolved.GetProperty("reasons").EnumerateArray().Select(reason => reason.GetProperty("code").GetString()).SequenceEqual(new[] { "low_usage" }),
            "The resolved regular weight-detail fixture retains exactly the independent low-use reason");
        var matchedNames = requests.SelectMany(request => new[] { request.GetProperty("matched_name").GetString(), request.GetProperty("matched_full_name").GetString() })
            .Where(name => !string.IsNullOrEmpty(name)).Select(name => name!).Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal).ToArray();
        var matchedPaths = requests.SelectMany(request => request.GetProperty("paths").EnumerateArray()).Select(path => path.GetString()!)
            .Where(path => !string.IsNullOrEmpty(path)).Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal).ToArray();
        var heading = display + "\n1 use\nReasons: Low usage\nASS requested names: " + FixtureFamily + "\nGDI returned/matched names: " + string.Join("; ", matchedNames);
        var sources = "\nSources:\n" + Path.GetFullPath(input) + " · Line 11"
            + (matchedPaths.Length == 0 ? "" : "\nMatched files: " + string.Join("; ", matchedPaths));
        var previousDetails = heading + sources;
        var expected = heading + "\nMatched font weights: 400" + sources;
        Write("matched-weights-registered-fixture-oracle.json", new { phase, expected, fixed_matched_weight = 400,
            fixture = "tests/fixtures/gdi-fonts/regular.ttf", weight_source = "fixed regular fixture OS/2 usWeightClass, cross-checked by the resolved system-platform CLI request",
            matched_names = matchedNames, matched_paths = matchedPaths, metadata_source = "same-platform CLI cross-check, not actual GUI text", actual_visible_popup_asserted = false });
        var fields = RequireFieldRows([(display, "1 use · Low usage")]).Single();
        ExpectAccessibleDetails(fields, expected, previousDetails, "matched-weights-registered");
        ExpectInteractionCopy(display, "matched-weights-names-copy-unchanged", true, fields.Name);
        ExpectInteractionCopy(FullInteractionRecord(display, 1, "Low usage"), "matched-weights-full-copy-unchanged", false, fields.Status);
    }

    void ExpectAccessibleDetails(FieldRow row, string expected, string previousDetails, string label)
    {
        var observations = new[] { (Role: "name", Handle: row.Name), (Role: "status", Handle: row.Status) }.Select(field => {
            isolation.VerifyWindow(field.Handle, app!);
            var property = AutomationElement.FromHandle(field.Handle).GetCurrentPropertyValue(AutomationElement.HelpTextProperty, true);
            var supported = !ReferenceEquals(property, AutomationElement.NotSupported);
            var actual = property as string;
            return new { field.Role, hwnd = field.Handle.ToString("X"), property_supported = supported, actual,
                actual_type = property?.GetType().FullName, matches_previous_details = supported && actual == previousDetails, exact_match = supported && actual == expected };
        }).ToArray();
        var unavailable = observations.Where(observation => !observation.property_supported || string.IsNullOrEmpty(observation.actual)).Select(observation => observation.Role).ToArray();
        var readableMismatch = observations.Any(observation => observation.property_supported && !string.IsNullOrEmpty(observation.actual) && !observation.exact_match);
        var blocked = backgroundSupportedInputs && unavailable.Length > 0 && !readableMismatch;
        Write(label + "-accessible-details.json", new { phase, expected, previous_details_without_matched_weights = previousDetails, observations,
            unavailable_fields = unavailable, background_supported_inputs = backgroundSupportedInputs,
            status = blocked ? "blocked_unavailable_property" : "exact_equality_required",
            observed_surface = "actual field UIA HelpText property only", actual_visible_popup_asserted = false });
        if (blocked)
        {
            RecordBlocked("V04_matched_weights_accessibility_details", "Actual field HelpText is unsupported or empty for " + string.Join(", ", unavailable)
                + "; the complete actual property observations are preserved in " + label + "-accessible-details.json. No matched-weight detail or visible-popup acceptance is asserted, and any readable nonempty mismatch still fails.");
            return;
        }
        Check(observations.All(observation => observation.exact_match), "Both actual fields expose the exact neutral matched-weight details through HelpText; this does not assert a visible tooltip: " + label);
    }

    void ObserveLocalTip(nint field, string expected)
    {
        var cursorReadable = Native.GetCursorPos(out var cursor);
        var cursorError = cursorReadable ? 0 : Marshal.GetLastPInvokeError();
        var bounds = Native.WindowRectangle(field);
        var inside = cursorReadable && cursor.X >= bounds.Left && cursor.X < bounds.Right && cursor.Y >= bounds.Top && cursor.Y < bounds.Bottom;
        Write("local-tip-hover-capability.json", new { phase, cursor_readable = cursorReadable, native_error = cursorError,
            actual_cursor = cursorReadable ? new { cursor.X, cursor.Y } : null, field_bounds = new { bounds.Left, bounds.Top, bounds.Right, bounds.Bottom }, cursor_inside_field = inside,
            attempt = 1, input_desktop_switch = false, global_pointer_move = false, fallback_coordinates_are_hover_evidence = false });
        if (cursorReadable && inside)
        {
            var before = isolation.Windows(app!).ToHashSet();
            Send(field, 0x200, 0, Native.Position(cursor.X - bounds.Left, cursor.Y - bounds.Top));
            nint[] candidates = [];
            var shown = SpinWait.SpinUntil(() => {
                isolation.VerifyCurrent();
                if (app is { HasExited: true }) throw new InvalidOperationException("Host exited during the single local tooltip observation");
                candidates = isolation.Windows(app!).Where(handle => !before.Contains(handle) && IsLiveOwned(handle) && Native.IsWindowVisible(handle)).ToArray();
                return candidates.Length > 0;
            }, 2000);
            Write("local-tip-popup-observation.json", new { phase, shown, candidate_windows = candidates.Select(ReadContentIdentity).ToArray(),
                expected, text_equality_is_duration_acceptance = false, candidate_identity_requires_original_PNG_review = true });
            foreach (var candidate in candidates)
                if (IsLiveOwned(candidate)) ScreenCapture.SaveWindowPng(candidate, Path.Combine(artifacts, "local-tip-candidate-" + candidate.ToString("X") + ".png"));
        }
        RecordBlocked("V04_local_tip_visible_content", "A single background cursor/local-move capability observation is recorded where possible; neither expected details nor HelpText/native TOOL registration proves readable content in an actual local wxTipWindow bubble");
        RecordBlocked("V04_local_tip_12_second_persistence", "No verified continuous real hover and identifiable visible bubble were established for a twelve-second duration assertion; text inspection or a fixed delay is not persistence evidence");
        RecordBlocked("V04_local_tip_leave_closure", "No physical pointer movement or desktop switch is authorized; a previously visible local bubble followed by an actual leave was not established by this background observation");
    }

    TooltipObservation[] ObserveFieldTooltips(nint field)
    {
        var observations = new List<TooltipObservation>();
        foreach (var window in isolation.Windows(app!).Where(IsNativeTooltip))
        {
            isolation.VerifyWindow(window, app!);
            var visible = Native.IsWindowVisible(window);
            var query = Native.QueryRecordsTooltip(window, field, app!.Id);
            observations.Add(new TooltipObservation(window.ToString("X"), visible, visible ? Text(window) : null, visible ? AutomationElement.FromHandle(window).Current.Name : null, query.ToolCount, query.Registered, query.Text, query.Current));
        }
        return observations.ToArray();
    }

    bool IsNativeTooltip(nint window)
    {
        try { return PrivateDesktop.ClassName(window) == "tooltips_class32"; }
        catch (Win32Exception error) when (error.NativeErrorCode == 1400 && !Native.IsWindow(window))
        {
            Write("tooltip-lifetime-race-" + ++checkpoint + ".json", new { phase, hwnd = window.ToString("X"), native_error = error.NativeErrorCode, is_window_at_exception_filter = false }); return false;
        }
    }

    void ResizeSuffixInteraction(string display, int count, string reasons, string label)
    {
        phase = "interaction_V04_native_fields_resize_selection_preserved";
        var row = RequireFieldRows([(display, SummaryText(count, reasons))]).Single();
        var start = Math.Max(0, display.IndexOf("GDI", StringComparison.Ordinal)); var end = Math.Min(display.Length, start + 3);
        SetCellSelection(row.Name, start, end);
        var initial = Native.WindowRectangle(inventory); var minimum = Native.MinimumTrackSize(inventory);
        var width = initial.Right - initial.Left; var height = initial.Bottom - initial.Top;
        Check(minimum.X > 0 && minimum.Y > 0 && width >= minimum.X, "Resize uses actual bounds and positive native minimum tracking size");
        var sizes = new[] { (Name: "initial", Width: width), (Name: "wide", Width: Math.Max(width + 1, (int)Math.Round(width * 1.25))), (Name: "narrow", Width: Math.Max(minimum.X, (int)Math.Round(width * 0.65))), (Name: "restored", Width: width) };
        foreach (var size in sizes)
        {
            if (!Native.SetWindowPos(inventory, 0, 0, 0, size.Width, height, 0x0016)) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Background field window resize failed");
            Wait(() => { var bounds = Native.WindowRectangle(inventory); return bounds.Right - bounds.Left == size.Width; }, "Inventory reaches the actual requested resize width");
            var current = RequireFieldRows([(display, SummaryText(count, reasons))]).Single();
            Check(current.Name == row.Name && current.Status == row.Status && CellSelection(current.Name) == (start, end), "Resize preserves the existing field HWNDs and exact native name selection");
            Check(Send(current.Status, 0xBA) == 1, "The compact status native Edit occupies exactly one visual line without wrap");
            ExpectInteractionRecord(display, count, reasons, "resize-" + label + "-" + size.Name);
            Capture("interaction-resize-" + label + "-" + size.Name + "-selected");
            ClearInteractionSelection();
            var before = Native.GetClipboardSequenceNumber(); Send(current.Name, 0x301);
            Check(Native.GetClipboardSequenceNumber() == before, "Resize unselected artifact is backed by an unchanged empty-field clipboard sequence");
            var screenshot = "interaction-resize-" + label + "-" + size.Name;
            Capture(screenshot);
            var actual = Native.WindowRectangle(inventory);
            Write(screenshot + "-bounds.json", new { phase, requested_width = size.Width, actual = new { actual.Left, actual.Top, actual.Right, actual.Bottom }, minimum_track = new { minimum.X, minimum.Y }, name_visual_lines = Send(current.Name, 0xBA), status_visual_lines = Send(current.Status, 0xBA), name_value_has_artificial_linebreak = Text(current.Name).Contains('\n'), right_alignment_visual_review = "pending_manual" });
            renderingReviews.Add(new { phase, screenshot = screenshot + ".png", status = "pending_manual_review", expected_review = "Complete unmodified name wraps naturally in its field; single-line highest-priority suffix anchors to the actual right edge; selected/unselected images separate selection from colors" });
            SetCellSelection(current.Name, start, end);
        }
        Write("rendering-review.json", new { status = "pending_manual_review", automatic_color_or_pixel_alignment_assertions = false, reviews = renderingReviews });
    }

    void RootChurnInteraction(string display, string backup, string b)
    {
        phase = "interaction_V09_root_churn_recovers_without_modal";
        CheckRootChurnWindows();
        var before = RequireFieldRows([(display, "7 uses")]).Single();
        var oldName = ReadContentIdentity(before.Name) ?? throw new InvalidOperationException("Root-churn baseline name field lost its live identity");
        var oldStatus = ReadContentIdentity(before.Status) ?? throw new InvalidOperationException("Root-churn baseline status field lost its live identity");
        const int cycles = 8;
        var events = new List<object>();
        for (var cycle = 1; cycle <= cycles; ++cycle)
        {
            RemoveRuntimeTree(source);
            events.Add(new { cycle, action = "source_removed", utc = DateTime.UtcNow });
            Write("root-churn-source-events.json", new { phase, source, backup, fixed_cycles = cycles, completed_cycles = cycle - 1, events });
            CopyDirectory(backup, source);
            events.Add(new { cycle, action = "source_restored_from_backup", utc = DateTime.UtcNow });
            Write("root-churn-source-events.json", new { phase, source, backup, fixed_cycles = cycles, completed_cycles = cycle, events });
        }
        FieldRow? restored = null;
        Wait(() => {
            CheckRootChurnWindows();
            var rows = ReadFieldRows();
            if (rows is not { Length: 1 } || rows[0].NameText != display || rows[0].StatusText != "7 uses") return false;
            if (ReadContentIdentity(rows[0].Name) is not { Enabled: true } name || name == oldName
                || ReadContentIdentity(rows[0].Status) is not { Enabled: true } status || status == oldStatus) return false;
            restored = rows[0]; return true;
        }, "Eight fixed root delete/restore cycles publish a new complete enabled exact seven-use field pair without a modal dialog");
        var ready = restored ?? throw new InvalidOperationException("Root-churn refresh completed without an observed field pair");
        Write("root-churn-public-ready.json", new { phase, old_name = oldName, old_status = oldStatus,
            new_name = ReadContentIdentity(ready.Name), new_status = ReadContentIdentity(ready.Status), fixed_cycles = cycles,
            same_output_alone_is_refresh_evidence = false });
        CheckRootChurnWindows();
        Check(Send(inventory, 0) == 0, "The original inventory answers a bounded native WM_NULL after root churn");
        ExpectInteractionRecord(display, 7, "", "root-churn-recovers-seven");
        CheckRootChurnWindows();
        var savedB = File.ReadAllText(b);
        File.AppendAllText(b, "Dialogue: 0,0:00:13.00,0:00:14.00,Default,after-root-churn,0,0,0,,A\n");
        ExpectInteractionRecord(display, 8, "", "post-churn-watch-write-eight");
        CheckRootChurnWindows();
        File.WriteAllText(b, savedB);
        ExpectInteractionRecord(display, 7, "", "post-churn-restored-seven");
        CheckRootChurnWindows();
        Check(Native.IsWindowEnabled(main) && Native.IsWindowEnabled(inventory), "Root churn leaves the exact main and inventory windows enabled without a modal dialog");
    }

    void CheckRootChurnWindows()
    {
        isolation.VerifyWindow(main, app!); isolation.VerifyWindow(inventory, app!);
        var unexpected = Windows().Where(handle => handle != main && handle != inventory && PrivateDesktop.ClassName(handle) == "#32770")
            .Select(handle => new { hwnd = handle.ToString("X"), class_name = PrivateDesktop.ClassName(handle), text = Text(handle),
                owner = Native.GetWindow(handle, 4).ToString("X"), enabled = Native.IsWindowEnabled(handle) }).ToArray();
        if (unexpected.Length != 0)
        {
            Write("root-churn-unexpected-modal.json", new { phase, main = main.ToString("X"), inventory = inventory.ToString("X"), unexpected });
            throw new InvalidOperationException("Root churn opened an unexpected owned #32770 dialog; the main and inventory HWNDs are the only dialog exceptions");
        }
        if (!Native.IsWindowEnabled(main) || !Native.IsWindowEnabled(inventory))
        {
            Write("root-churn-disabled-owner.json", new { phase, main = main.ToString("X"), main_enabled = Native.IsWindowEnabled(main),
                inventory = inventory.ToString("X"), inventory_enabled = Native.IsWindowEnabled(inventory) });
            throw new InvalidOperationException("Root churn disabled the exact main or inventory window unexpectedly");
        }
    }

    void ExpectInteractionRecord(string display, int count, string reasons, string label)
    {
        ExpectInteractionCopy(FullInteractionRecord(display, count, reasons), label);
        RequireFieldRows([(display, SummaryText(count, reasons))]);
    }

    void ExpectInteractionCopy(string expected, string label, bool namesOnly = false, nint target = 0)
    {
        Write(label + "-oracle.json", new { phase, expected, status = "waiting_for_exact_full_list_menu_clipboard", count_and_reasons_source = "independent fixture constants", clipboard_artifact_encoding = "UTF-16LE without BOM" });
        string? observed = null; uint? pendingBefore = null; var attempts = 0; var polls = 0; var polling = Stopwatch.StartNew(); long nextAttempt = 0;
        Wait(() => {
            if (polling.ElapsedMilliseconds < nextAttempt) return false;
            nextAttempt = polling.ElapsedMilliseconds + 150;
            if (pendingBefore is null)
            {
                var before = Native.GetClipboardSequenceNumber();
                var enabled = InvokeActualMenu(target == 0 ? records : target, namesOnly ? "Copy font names list" : "Copy full list");
                Write(label + "-copy-attempt-" + ++attempts + ".json", new { phase, before_sequence = before, after_sequence_at_menu_return = Native.GetClipboardSequenceNumber(), enabled,
                    action = namesOnly ? "Copy font names list" : "Copy full list", pending_invocation = enabled });
                if (!enabled) return false;
                pendingBefore = before;
            }
            var after = Native.GetClipboardSequenceNumber();
            Write(label + "-pending-clipboard.json", new { phase, invocation = attempts, poll = ++polls, original_before_sequence = pendingBefore.Value, current_sequence = after,
                sequence_changed = after != pendingBefore.Value, menu_reopened_while_pending = false });
            if (after == pendingBefore.Value) return false;
            var snapshot = ClipboardText.ReadOwnedUnicodeTextAfter(pendingBefore.Value, app!.Id);
            observed = snapshot.Text;
            File.WriteAllText(Path.Combine(artifacts, label + "-clipboard-" + attempts + ".txt"), observed, new UnicodeEncoding(false, false));
            Write(label + "-response-" + attempts + ".json", new { phase, original_before_sequence = pendingBefore.Value, response_sequence = snapshot.SequenceAfterCopy,
                clipboard_snapshot = snapshot, actual = observed, expected, exact_match = observed == expected });
            pendingBefore = null;
            return observed == expected;
        }, "The real full-list context-menu action publishes the exact independent clipboard oracle");
        Write(label + "-oracle.json", new { phase, expected, actual = observed, attempts, polls, clipboard_artifact_encoding = "UTF-16LE without BOM" });
        Check(observed == expected, "Actual full-list context action equals the exact independent count/reasons/CRLF oracle: " + label);
    }

    void ExpectNoInteractionCopy(string label)
    {
        Stopwatch? stable = null; var confirmed = false; var attempts = 0;
        Wait(() => {
            var before = Native.GetClipboardSequenceNumber();
            var namesDisabled = !InvokeActualMenu(records, "Copy font names list"); var fullDisabled = !InvokeActualMenu(records, "Copy full list");
            var after = Native.GetClipboardSequenceNumber();
            Write(label + "-empty-list-menu-" + ++attempts + ".json", new { phase, before, after, namesDisabled, fullDisabled });
            if (!namesDisabled || !fullDisabled || after != before) { stable = null; return false; }
            stable ??= Stopwatch.StartNew(); confirmed = stable.ElapsedMilliseconds >= 250; return confirmed;
        }, "Both actual full-list context actions are disabled and leave the clipboard unchanged while no current list exists");
        Check(confirmed, "Empty/stale/unavailable list has both full-list actions disabled without clipboard writes: " + label);
    }

    void ClearInteractionSelection()
    {
        var rows = ReadFieldRows() ?? throw new InvalidOperationException("Cannot clear or review selection while an extra/incomplete visible field group exists");
        foreach (var row in rows) foreach (var field in new[] { row.Name, row.Status }) SetCellSelection(field, 0, 0);
    }
    void ReviewInteraction(string label, string expectedReview, bool clearSelection = true)
    {
        if (clearSelection) ClearInteractionSelection(); Capture("interaction-" + label);
        renderingReviews.Add(new { phase, screenshot = "interaction-" + label + ".png", status = "pending_manual_review", expected_review = expectedReview });
        Write("rendering-review.json", new { status = "pending_manual_review", automatic_color_or_pixel_alignment_assertions = false, reviews = renderingReviews });
    }

    bool InvokeActualMenu(nint target, string itemLabel)
    {
        isolation.VerifyWindow(target, app!); var screen = Native.ToScreen(target, 8, 8);
        Native.Post(target, 0x7B, target.ToInt64(), Native.Position(screen.X, screen.Y));
        nint popup = 0;
        Wait(() => { popup = Windows().SingleOrDefault(IsNativeMenuPopup); return popup != 0; }, "Actual native context menu opens");
        isolation.VerifyWindow(popup, app!); var menu = (nint)Send(popup, 0x1E1);
        if (menu == 0) throw new InvalidOperationException("Actual popup has no observable HMENU");
        var items = Native.Items(menu); var selected = items.Single(item => item.Label == itemLabel);
        var bounds = Native.MenuItemBounds(menu, selected.Position); var label = "interaction-popup-" + ++checkpoint;
        Write(label + ".json", new { phase, target = target.ToString("X"), popup = popup.ToString("X"), menu = menu.ToString("X"), items, selected_label = itemLabel, selected_position = selected.Position, selected.Enabled, bounds = new { bounds.Left, bounds.Top, bounds.Right, bounds.Bottom }, route = "actual popup bounded DOWN/highlight/ENTER, never observed command-ID bypass" });
        ScreenCapture.SaveWindowPng(popup, Path.Combine(artifacts, label + ".png"));
        if (!selected.Enabled)
        {
            Native.Post(popup, 0x100, 0x1B, 1); Native.Post(popup, 0x101, 0x1B, unchecked((int)0xC0000001));
            Wait(() => !Windows().Any(IsNativeMenuPopup), "Disabled actual menu action is dismissed without command execution"); return false;
        }
        var initial = items.Select(item => new { item.Position, item.Label, highlighted = Native.Highlighted(menu, item.Position) }).ToArray();
        var steps = new List<object>();
        for (var step = 0; step < items.Length && !Native.Highlighted(menu, selected.Position); ++step)
        {
            var previous = items.Where(item => Native.Highlighted(menu, item.Position)).Select(item => item.Position).ToArray();
            Native.Post(popup, 0x100, 0x28, 1); Native.Post(popup, 0x101, 0x28, unchecked((int)0xC0000001));
            Wait(() => { var current = items.Where(item => Native.Highlighted(menu, item.Position)).Select(item => item.Position).ToArray(); return current.Length != 0 && !current.SequenceEqual(previous); }, "Actual popup DOWN establishes or changes its highlighted item");
            steps.Add(new { down_step = step + 1, items = items.Select(item => new { item.Position, item.Label, highlighted = Native.Highlighted(menu, item.Position) }).ToArray() });
            Write(label + "-navigation.json", new { initial, maximum_down_steps = items.Length, steps });
        }
        if (!Native.Highlighted(menu, selected.Position)) throw new InvalidOperationException("Actual popup did not highlight the exact observed target item");
        ScreenCapture.SaveWindowPng(popup, Path.Combine(artifacts, label + "-highlighted.png"));
        Write(label + "-selection.json", new { itemLabel, verified_highlight_before_enter = true, initial, maximum_down_steps = items.Length, steps });
        Native.Post(popup, 0x100, 0x0D, 1); Native.Post(popup, 0x101, 0x0D, unchecked((int)0xC0000001));
        Wait(() => !Windows().Any(IsNativeMenuPopup), "Actual highlighted menu action completes"); return true;
    }

    bool IsNativeMenuPopup(nint window)
    {
        try { return PrivateDesktop.ClassName(window) == "#32768"; }
        catch (Win32Exception error) when (error.NativeErrorCode == 1400 && !Native.IsWindow(window))
        {
            Write("menu-lifetime-race-" + ++checkpoint + ".json", new { phase, hwnd = window.ToString("X"), native_error = error.NativeErrorCode, is_window_at_exception_filter = false, classification = "destroyed_between_enumeration_and_class_query" });
            return false;
        }
    }

    void Prepare()
    {
        var installed = System.Windows.Media.Fonts.SystemFontFamilies.Select(font => font.Source).ToArray();
        if (allowFonts)
        {
            Check(!installed.Contains(FixtureFamily, StringComparer.OrdinalIgnoreCase), "Dedicated test font family is absent before any registration");
            family = FixtureFamily;
        }
        else Check(installed.Contains(family, StringComparer.OrdinalIgnoreCase), "Default platform GUI oracle requires installed Arial");
        source = Path.Combine(runtime, "root"); CopyDirectory("tests/fixtures/font-usage-inventory/root", source);
        foreach (var ass in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
            File.WriteAllText(ass, File.ReadAllText(ass).Replace(FixtureFamily, family, StringComparison.Ordinal));
        Directory.CreateDirectory(Path.Combine(runtime, "empty"));
        File.WriteAllText(Path.Combine(runtime, "not-ass.txt"), "Ignored drag source sentinel\n");
        var header = File.ReadAllText(Path.Combine(source, "b.ASS")).Split("Dialogue:")[0];
        File.WriteAllText(Path.Combine(runtime, "comments.ass"), header + "Comment: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,A\n");
        File.WriteAllText(Path.Combine(runtime, "copy-multi.ass"), header
            + "Dialogue: 0,0:00:01.00,0:00:02.00,Default,copy-primary,0,0,0,,A\n"
            + "Dialogue: 0,0:00:03.00,0:00:04.00,Default,copy-missing,0,0,0,,{\\fn" + CopyMissingFamily + "}A\n");
        originalAss = File.ReadAllText(Path.Combine(source, "b.ASS")); File.WriteAllText(Path.Combine(runtime, "editor.ass"), originalAss);
        var user = Path.Combine(artifacts, "profile", "user"); Directory.CreateDirectory(user);
        File.WriteAllText(Path.Combine(user, "config.json"), """
            {"Tool":{"Font Usage Inventory":{"Recursive":false,"Low Frequency Threshold":5}},"Subtitle":{"Use STC":false},"App":{"First Start":false,"Language":"en_US","Auto":{"Backup":false,"Check For Updates":false,"Load Linked Files":1,"Save":false,"Save on Every Change":false}}}
            """);
        Write("fixture-preconditions.json", new { platform_family = family, session_font_changes_authorized = allowFonts,
            executable_sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(args[0]))),
            expected_nonrecursive = 3, expected_recursive = 5 });
    }

    void Start()
    {
        var launch = Path.Combine(artifacts, "launch"); Directory.CreateDirectory(launch);
        var start = new ProcessStartInfo(Path.GetFullPath(args[0])) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var argument in new[] { "gui-test", "host", "--profile-dir", Path.GetFullPath(Path.Combine(artifacts, "profile")), "--artifacts", Path.GetFullPath(launch), "--open", Path.GetFullPath(Path.Combine(runtime, "editor.ass")) }) start.ArgumentList.Add(argument);
        app = isolation.Start(start, "font-usage-inventory");
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(launch, "ready.json"), app, TimeSpan.FromSeconds(20));
        main = isolation.MainWindow(app, 10); mainCaption = Native.Caption(main);
    }

    void Open()
    {
        isolation.VerifyWindow(main, app!);
        SnapshotControls(main, "main-before-inventory-" + ++checkpoint);
        var id = ObservedWindowsCommand.FindMenuCommand(main, app!.Id, "File", "Font Usage List...");
        Write("open-command-" + checkpoint + ".json", new { phase, menu = "File", command = "Font Usage List...", observed_native_id = id });
        ObservedWindowsCommand.PostMenuCommand(main, app.Id, id);
        Wait(() => { inventory = Windows().SingleOrDefault(h => Native.Caption(h) == "Font Usage List"); return inventory != 0; }, "File / Font Usage List opens");
        SnapshotControls(inventory, "inventory-open-" + ++checkpoint);
        if (probeOnly) return;
        var children = Native.Children(inventory).Where(Native.IsWindowVisible).ToArray();
        records = children.Single(h => Native.GetParent(h) == inventory && PrivateDesktop.ClassName(h) == "wxWindow" && Native.Caption(h) == "panel");
        recursive = children.Single(h => Native.GetParent(h) == inventory && PrivateDesktop.ClassName(h) == "Button" && Text(h).Replace("&", "", StringComparison.Ordinal) == "Recursive subdirectories");
        var edits = children.Where(h => Native.GetParent(h) == inventory && PrivateDesktop.ClassName(h) == "Edit" && (Native.GetWindowLongPtr(h, -16).ToInt64() & 0x800) == 0).ToArray();
        Check(edits.Length == 1, "Threshold is the unique direct native editable input; record fields are readonly descendants"); threshold = edits[0];
        Write("controls-" + ++checkpoint + ".json", children.Select(h => new { hwnd = h.ToString("X"), class_name = PrivateDesktop.ClassName(h), text = Text(h), enabled = Native.IsWindowEnabled(h) }));
    }

    nint[] ProbeReadonlyFields()
    {
        var fields = new List<nint>();
        foreach (var handle in Native.Children(inventory))
        {
            if (!IsLiveOwned(handle)) continue;
            try
            {
                if (PrivateDesktop.ClassName(handle) == "Edit" && (Native.GetWindowLongPtr(handle, -16).ToInt64() & 0x800) != 0 && IsLiveOwned(handle))
                    fields.Add(handle);
            }
            catch (Win32Exception error) when (error.NativeErrorCode == 1400 && !Native.IsWindow(handle))
            {
                Write("probe-field-lifetime-race-" + ++checkpoint + ".json", new { phase, hwnd = handle.ToString("X"), native_error = error.NativeErrorCode,
                    is_window_at_exception_filter = false, classification = "destroyed_between_enumeration_and_class_query" });
            }
        }
        return fields.ToArray();
    }

    void SnapshotControls(nint window, string label)
    {
        isolation.VerifyWindow(window, app!);
        var native = Native.Children(window).Prepend(window).Select(handle => {
            var entry = new Dictionary<string, object?> { ["hwnd"] = handle.ToString("X") };
            try
            {
                isolation.VerifyWindow(handle, app!);
                var text = Text(handle);
                entry["parent_hwnd"] = Native.GetParent(handle).ToString("X");
                entry["class_name"] = PrivateDesktop.ClassName(handle);
                var style = Native.GetWindowLongPtr(handle, -16).ToInt64();
                entry["native_style"] = unchecked((uint)style);
                entry["native_ex_style"] = unchecked((uint)Native.GetWindowLongPtr(handle, -20).ToInt64());
                entry["readonly_edit"] = entry["class_name"] as string == "Edit" ? (style & 0x800) != 0 : (bool?)null;
                entry["visible"] = Native.IsWindowVisible(handle);
                entry["enabled"] = Native.IsWindowEnabled(handle);
                entry["native_control_id"] = Native.GetDlgCtrlID(handle);
                entry["bounds"] = Native.Bounds(handle);
                var client = Native.ClientRectangle(handle);
                entry["client_bounds"] = new { client.Left, client.Top, client.Right, client.Bottom };
                entry["wm_gettext_observation"] = Preview(text);
                entry["wm_gettext_is_inventory_result_oracle"] = false;
                entry["status"] = "observed";
            }
            catch (Exception error) { entry["status"] = "unavailable"; entry["error"] = error.Message; }
            return entry;
        }).ToArray();
        Write(label + "-native.json", new { phase, window = window.ToString("X"), count = native.Length, controls = native });

        var elements = AutomationElement.FromHandle(window).FindAll(TreeScope.Subtree, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
        var uia = new List<Dictionary<string, object?>>();
        var patterns = new Dictionary<string, int>(StringComparer.Ordinal);
        var remainingGridCellQueries = 64;
        foreach (var element in elements)
        {
            var entry = new Dictionary<string, object?> { ["index"] = uia.Count };
            try
            {
                var current = element.Current;
                entry["process_id"] = current.ProcessId;
                if (current.ProcessId != app!.Id) throw new InvalidOperationException("UIA element belongs to a different process");
                entry["hwnd"] = ((nint)current.NativeWindowHandle).ToString("X");
                entry["name"] = current.Name;
                entry["class_name"] = current.ClassName;
                entry["automation_id"] = current.AutomationId;
                entry["control_type"] = current.ControlType.ProgrammaticName;
                entry["offscreen"] = current.IsOffscreen;
                entry["enabled"] = current.IsEnabled;
                entry["bounds"] = new[] { current.BoundingRectangle.Left, current.BoundingRectangle.Top, current.BoundingRectangle.Width, current.BoundingRectangle.Height };
                var available = element.GetSupportedPatterns().Select(pattern => pattern.ProgrammaticName).Order(StringComparer.Ordinal).ToArray();
                entry["patterns"] = available;
                foreach (var pattern in available) patterns[pattern] = patterns.GetValueOrDefault(pattern) + 1;
                if (element.TryGetCurrentPattern(ValuePattern.Pattern, out var rawValue))
                {
                    var value = ((ValuePattern)rawValue).Current;
                    entry["value"] = Preview(value.Value); entry["value_read_only"] = value.IsReadOnly;
                }
                if (element.TryGetCurrentPattern(TextPattern.Pattern, out var rawText))
                {
                    var text = (TextPattern)rawText; var document = text.DocumentRange;
                    var contents = document.GetText(-1);
                    entry["text_supported_selection"] = text.SupportedTextSelection.ToString();
                    entry["document_range"] = Preview(contents);
                    entry["document_font_name"] = DescribeAttribute(document.GetAttributeValue(TextPattern.FontNameAttribute));
                    entry["document_font_size"] = DescribeAttribute(document.GetAttributeValue(TextPattern.FontSizeAttribute));
                    entry["selection_ranges"] = text.SupportedTextSelection == SupportedTextSelection.None ? [] : text.GetSelection().Select(range => Preview(range.GetText(-1))).ToArray();
                    if (contents.Length > 0)
                    {
                        var filename = label + "-uia-document-" + uia.Count + ".txt";
                        File.WriteAllText(Path.Combine(artifacts, filename), Scrub(contents)); entry["document_text_artifact"] = filename;
                    }
                }
                if (element.TryGetCurrentPattern(GridPattern.Pattern, out var rawGrid))
                {
                    var grid = (GridPattern)rawGrid; var shape = grid.Current;
                    entry["grid_row_count"] = shape.RowCount; entry["grid_column_count"] = shape.ColumnCount;
                    var cells = new List<object>();
                    for (var row = 0; row < shape.RowCount && remainingGridCellQueries > 0; ++row)
                        for (var column = 0; column < shape.ColumnCount && remainingGridCellQueries > 0; ++column)
                        {
                            --remainingGridCellQueries;
                            try { cells.Add(new { row, column, status = "observed", element = DescribeElement(grid.GetItem(row, column)) }); }
                            catch (Exception error) { cells.Add(new { row, column, status = "unavailable", error = error.Message }); }
                        }
                    entry["grid_cells"] = cells; entry["grid_cell_getter_limit_per_snapshot"] = 64;
                    entry["grid_cell_total"] = (long)shape.RowCount * shape.ColumnCount;
                    entry["grid_cells_truncated"] = (long)shape.RowCount * shape.ColumnCount > cells.Count;
                }
                if (element.TryGetCurrentPattern(TablePattern.Pattern, out var rawTable))
                {
                    var table = (TablePattern)rawTable;
                    entry["table_row_or_column_major"] = table.Current.RowOrColumnMajor.ToString();
                    entry["table_row_header_count"] = table.Current.GetRowHeaders().Length;
                    entry["table_column_header_count"] = table.Current.GetColumnHeaders().Length;
                }
                if (element.TryGetCurrentPattern(GridItemPattern.Pattern, out var rawGridItem))
                {
                    var item = ((GridItemPattern)rawGridItem).Current;
                    entry["grid_item"] = new { row = item.Row, column = item.Column, row_span = item.RowSpan, column_span = item.ColumnSpan, containing_grid = DescribeElement(item.ContainingGrid) };
                }
                if (element.TryGetCurrentPattern(SelectionPattern.Pattern, out var rawSelection))
                {
                    var selection = (SelectionPattern)rawSelection; var currentSelection = selection.Current; var selected = currentSelection.GetSelection();
                    entry["selection_can_select_multiple"] = currentSelection.CanSelectMultiple;
                    entry["selection_required"] = currentSelection.IsSelectionRequired;
                    entry["selected_item_count"] = selected.Length;
                    entry["selected_items"] = selected.Take(64).Select(DescribeElement).ToArray();
                    entry["selected_items_truncated"] = selected.Length > 64;
                }
                if (element.TryGetCurrentPattern(SelectionItemPattern.Pattern, out var rawSelectionItem))
                {
                    var item = ((SelectionItemPattern)rawSelectionItem).Current;
                    entry["selection_item_selected"] = item.IsSelected;
                    entry["selection_item_container"] = DescribeElement(item.SelectionContainer);
                }
                if (element.TryGetCurrentPattern(ScrollPattern.Pattern, out var rawScroll))
                {
                    var scroll = ((ScrollPattern)rawScroll).Current;
                    entry["scroll"] = new { scroll.HorizontallyScrollable, scroll.VerticallyScrollable, scroll.HorizontalScrollPercent, scroll.VerticalScrollPercent, scroll.HorizontalViewSize, scroll.VerticalViewSize };
                }
                entry["status"] = "observed";
            }
            catch (Exception error) { entry["status"] = "unavailable"; entry["error"] = error.Message; }
            uia.Add(entry);
        }
        Write(label + "-uia.json", new { phase, window = window.ToString("X"), count = uia.Count, pattern_counts = patterns,
            grid_cell_getter_limit = 64, grid_cell_getter_count = 64 - remainingGridCellQueries,
            typed_legacy_accessible_reader_available = false, legacy_accessible_limitation = "The managed UIA reference has no LegacyIAccessiblePattern type; no COM interop layer is added for this probe", controls = uia });
    }

    object? DescribeElement(AutomationElement? element)
    {
        if (element is null) return null;
        var current = element.Current;
        if (current.ProcessId != app!.Id) throw new InvalidOperationException("Referenced UIA element belongs to a different process");
        return new { hwnd = ((nint)current.NativeWindowHandle).ToString("X"), name = current.Name, class_name = current.ClassName,
            control_type = current.ControlType.ProgrammaticName, offscreen = current.IsOffscreen, enabled = current.IsEnabled,
            bounds = new[] { current.BoundingRectangle.Left, current.BoundingRectangle.Top, current.BoundingRectangle.Width, current.BoundingRectangle.Height },
            patterns = element.GetSupportedPatterns().Select(pattern => pattern.ProgrammaticName).Order(StringComparer.Ordinal).ToArray() };
    }

    static object Preview(string value) => new { character_count = value.Length, text = value.Length <= 512 ? value : value[..512], preview_truncated = value.Length > 512 };
    static object? DescribeAttribute(object value) => value == AutomationElement.NotSupported ? "not_supported" : value == TextPattern.MixedAttributeValue ? "mixed" : value;

    void Drop(params string[] paths)
    {
        isolation.VerifyWindow(inventory, app!); Native.Drop(inventory, paths.Select(Path.GetFullPath).ToArray());
        Write("drop-" + ++checkpoint + ".json", new { phase, sources = paths });
    }

    void SetThreshold(string value)
    {
        isolation.VerifyWindow(threshold, app!); Native.SetText(threshold, value);
        Native.Post(threshold, 0x100, 13); Native.Post(threshold, 0x102, 13); Native.Post(threshold, 0x101, 13);
        Wait(() => Text(threshold) == (int.TryParse(value, out var parsed) && parsed >= 0 ? value : "5"), "Threshold input commits or restores the previous valid value");
    }

    JsonElement JsonRow(int count, string[]? paths = null)
    {
        var start = new ProcessStartInfo(Path.GetFullPath(args[0])) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var argument in new[] { "fontcollector", "inventory" }.Concat(paths ?? [source])) start.ArgumentList.Add(argument);
        if (recursiveState && paths is null) start.ArgumentList.Add("--recursive");
        start.ArgumentList.Add("--low-frequency-threshold"); start.ArgumentList.Add(Text(threshold));
        using var process = Process.Start(start) ?? throw new InvalidOperationException("CLI comparison process did not start"); cli = process;
        var stdout = process.StandardOutput.ReadToEndAsync(); var stderr = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(60_000)) { process.Kill(true); throw new TimeoutException("CLI comparison exceeded 60 seconds"); }
        Task.WaitAll(stdout, stderr); cli = null;
        using var report = JsonDocument.Parse(stdout.Result);
        Write("cli-" + ++checkpoint + ".json", SanitizeJson(report.RootElement));
        File.WriteAllText(Path.Combine(artifacts, "cli-" + checkpoint + ".stderr.txt"), Scrub(stderr.Result));
        Check(process.ExitCode == 0, "Same-configuration platform CLI exits zero");
        Check(report.RootElement.GetProperty("format").GetString() == "font_usage_inventory" && report.RootElement.GetProperty("schema_version").GetInt32() == 1, "CLI comparison is inventory schema one");
        Check(report.RootElement.GetProperty("fonts").GetArrayLength() == 1, "CLI independently reports exactly one used family");
        var row = report.RootElement.GetProperty("fonts")[0];
        Check(row.GetProperty("usage_count").GetInt32() == count, "CLI count equals the independent fixture oracle " + count);
        return row.Clone();
    }

    void RegisterFonts()
    {
        lock (fontGate)
        {
            if (timedOut) throw new TimeoutException("Font registration refused after the scenario deadline");
            foreach (var name in new[] { "regular.ttf", "bold.ttf", "italic.ttf", "bold-italic.ttf" })
            {
                var path = Path.GetFullPath(Path.Combine("tests", "fixtures", "gdi-fonts", name));
                var added = Native.AddFontResourceEx(path, 0, 0);
                if (added == 0) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Session fixture registration failed: " + name);
                registeredFonts.Add(path);
                fontEvents.Add(new { phase, action = "registered", font = Path.Combine("tests", "fixtures", "gdi-fonts", name), added_faces = added, outstanding_registrations = registeredFonts.Count });
                Write("session-font-events.json", fontEvents);
                Check(added > 0, "Registered this run's session-only dedicated fixture: " + name);
            }
        }
    }

    void UnregisterFonts()
    {
        lock (fontGate)
        {
            List<Exception> failures = [];
            for (var index = registeredFonts.Count - 1; index >= 0; --index)
            {
                var path = registeredFonts[index];
                if (!Native.RemoveFontResourceEx(path, 0, 0))
                {
                    var error = new Win32Exception(Marshal.GetLastPInvokeError(), "Session fixture removal failed: " + Path.GetFileName(path));
                    failures.Add(error);
                    fontEvents.Add(new { phase, action = "removal_failed", font = Path.Combine("tests", "fixtures", "gdi-fonts", Path.GetFileName(path)), error = error.Message, outstanding_registrations = registeredFonts.Count });
                    Write("session-font-events.json", fontEvents);
                    continue;
                }
                registeredFonts.RemoveAt(index);
                fontEvents.Add(new { phase, action = "removed", font = Path.Combine("tests", "fixtures", "gdi-fonts", Path.GetFileName(path)), outstanding_registrations = registeredFonts.Count });
                Write("session-font-events.json", fontEvents);
                Check(true, "Removed only this run's registered session fixture: " + Path.GetFileName(path));
            }
            if (failures.Count != 0) throw new AggregateException("Session font cleanup failed for one or more owned registrations", failures);
        }
    }

    void Wait(Func<bool> predicate, string message)
    {
        if (!SpinWait.SpinUntil(() => {
            isolation.VerifyCurrent();
            if (app is { HasExited: true }) throw new InvalidOperationException("Host exited while waiting: " + message);
            return predicate();
        }, 10_000)) throw new TimeoutException(message);
    }
    void WaitStable(Func<bool> predicate, string message, int milliseconds)
    {
        Stopwatch? stable = null;
        Wait(() => {
            if (!predicate()) { stable = null; return false; }
            stable ??= Stopwatch.StartNew(); return stable.ElapsedMilliseconds >= milliseconds;
        }, message);
    }
    void F5()
    {
        strictF5CaseExecuted = true;
        isolation.VerifyWindow(records, app!);
        var before = Native.Children(records).Select(handle => (Handle: handle, Identity: ReadContentIdentity(handle)
            ?? throw new InvalidOperationException("F5 baseline content changed identity before key dispatch"))).ToArray();
        var label = "F5-" + ++checkpoint;
        Write(label + ".json", new { phase, action = "window-local F5 keydown/up", target_hwnd = records.ToString("X"), before = before.Select(entry => entry.Identity),
            popup_opened_before_transition = false, sequence = "post key, observe actual content/active transition, only then permit copy popup" });
        Native.Post(records, 0x100, 0x74); Native.Post(records, 0x101, 0x74);
        ContentIdentity? changedBefore = null, changedAfter = null;
        Wait(() => {
            foreach (var entry in before)
            {
                var current = ReadContentIdentity(entry.Handle);
                if (current != entry.Identity) { changedBefore = entry.Identity; changedAfter = current; return true; }
            }
            return before.Length == 0 && Native.Children(records).Any(IsLiveOwned);
        }, "Queued F5 must produce a real content instance/value/enabled transition before any new PopupMenu or same-value clipboard oracle");
        Write(label + "-observed-transition.json", new { phase, changedBefore, changedAfter, baseline_child_count = before.Length, actual_transition_observed = true,
            new_popup_was_not_used_to_drain_keys = true, same_output_alone_is_refresh_evidence = false });
    }
    void RemoveRuntimeTree(string path)
    {
        var absolute = Path.GetFullPath(path); var permitted = Path.GetFullPath(runtime) + Path.DirectorySeparatorChar;
        if (!absolute.StartsWith(permitted, StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException("Fixture deletion escaped the runtime root");
        foreach (var file in Directory.EnumerateFiles(path, "*", SearchOption.AllDirectories)) File.Delete(file);
        foreach (var directory in Directory.EnumerateDirectories(path, "*", SearchOption.AllDirectories).OrderByDescending(value => value.Length)) Directory.Delete(directory);
        Directory.Delete(path);
    }
    long Send(nint window, uint message, long wp = 0, long lp = 0) { isolation.VerifyWindow(window, app!); return Native.Send(window, message, wp, lp); }
    string Text(nint window) => isolation.ReadText(window, app!);
    nint[] Windows() => isolation.Windows(app!).Where(Native.IsWindowVisible).ToArray();
    void Click(nint button)
    {
        isolation.VerifyWindow(button, app!);
        var before = Send(button, 0xF0);
        Check(before is 0 or 1 && Native.IsWindowEnabled(button), "Observed checkbox is enabled and has a determinate native state");
        Native.Post(button, 0xF5);
        Wait(() => Send(button, 0xF0) == 1 - before, "Native BM_CLICK toggles the actual checkbox state");
    }
    void Capture(string name)
    {
        var target = IsLiveOwned(inventory) ? inventory : IsLiveOwned(main) ? main : 0;
        if (target != 0) ScreenCapture.SaveWindowPng(target, Path.Combine(artifacts, name + ".png"));
        if (interactionMode) Write(name + "-capture-state.json", new { captured_window = target.ToString("X"), records_hwnd = records.ToString("X"), records_text_status = "native_readonly_Edit_fields_observable", native_pane_caption_is_result_oracle = false });
        else if (IsLiveOwned(records)) File.WriteAllText(Path.Combine(artifacts, name + ".txt"), Scrub(Text(records)));
        else Write(name + "-capture-state.json", new { captured_window = target.ToString("X"), records_hwnd = records.ToString("X"), records_text_status = records == 0 ? "not_discovered" : "not_live_owned", original_error_preserved = true });
    }
    bool IsLiveOwned(nint window) => window != 0 && app is { HasExited: false } && Native.IsWindow(window)
        && Native.GetWindowThreadProcessId(window, out var owner) != 0 && owner == app.Id;
    string Scrub(string value) => value.Replace(Directory.GetCurrentDirectory(), ".", StringComparison.OrdinalIgnoreCase).Replace(Directory.GetCurrentDirectory().Replace('\\', '/'), ".", StringComparison.OrdinalIgnoreCase);
    object? SanitizeJson(JsonElement value) => value.ValueKind switch {
        JsonValueKind.Object => value.EnumerateObject().ToDictionary(property => property.Name, property => SanitizeJson(property.Value)),
        JsonValueKind.Array => value.EnumerateArray().Select(SanitizeJson).ToArray(),
        JsonValueKind.String => SanitizePath(value.GetString()!),
        JsonValueKind.Number => value.GetDouble(),
        JsonValueKind.True => true,
        JsonValueKind.False => false,
        JsonValueKind.Null => null,
        _ => throw new InvalidDataException("Invalid JSON kind")
    };
    string SanitizePath(string value) { value = Scrub(value); return Path.IsPathRooted(value) ? "external-fonts/" + Path.GetFileName(value) : value; }
    void Write(string name, object? value) => File.WriteAllText(Path.Combine(artifacts, name), JsonSerializer.Serialize(SanitizeJson(JsonSerializer.SerializeToElement(value, Json)), Json));
    void Check(bool condition, string message) { lock (checks) checks.Add(new { phase, assertion = message, passed = condition }); Result(false, "Scenario is still running"); if (!condition) throw new InvalidOperationException(message); }
    void RecordBlocked(string requirement, string reason) { lock (checks) checks.Add(new { requirement, status = "blocked", reason }); }
    void Result(bool passed, string? error = null)
    {
        error = terminalFailure ?? error; passed = passed && terminalFailure is null;
        object[] snapshot; lock (checks) snapshot = checks.ToArray();
        if (interactionMode)
        {
            Write("interaction-summary.json", new { passed = false, interaction_passed = passed, error, checks = snapshot, full_acceptance = false, mode = backgroundSupportedInputs ? "interaction-controls/background-supported-inputs" : "interaction-controls/strict-f5", background_supported_inputs = backgroundSupportedInputs, strict_f5_case_executed = strictF5CaseExecuted, scope = "background native two-field fragments, explicit full-list context actions, counts/thresholds/watch/root recovery/owner/fonts, local wxTipWindow capability observations, resize and same-text selection/focus restoration", automatic_result_oracle = "actual full-list menus and native Edit CF_UNICODETEXT, complete field values/styles/selections, focus thread state; local-tooltip visibility/duration remain separately blocked", clipboard_artifact_encoding = "UTF-16LE without BOM", rendering_review = "manual; legacy static TOOL text, HelpText and source strings are not local-bubble or duration acceptance", original_sci_suite = "retired_after_user_contract_change", session_font_changes_authorized = allowFonts });
            Write("result.json", new { passed = false, interaction_passed = passed, error, checks = snapshot, full_acceptance = false, mode = backgroundSupportedInputs ? "interaction-controls/background-supported-inputs" : "interaction-controls/strict-f5", background_supported_inputs = backgroundSupportedInputs, strict_f5_case_executed = strictF5CaseExecuted, original_sci_suite = "retired_after_user_contract_change" });
        }
        else Write("result.json", new { passed, error, checks = snapshot, full_acceptance = false, probe_only = probeOnly });
    }
    static void CopyDirectory(string from, string to) { Directory.CreateDirectory(to); foreach (var file in Directory.EnumerateFiles(from)) File.Copy(file, Path.Combine(to, Path.GetFileName(file))); foreach (var directory in Directory.EnumerateDirectories(from)) CopyDirectory(directory, Path.Combine(to, Path.GetFileName(directory))); }
}

sealed record TooltipObservation(string Hwnd, bool Visible, string? NativeText, string? UiaName, int ToolCount, bool RecordsToolRegistered, string? RegisteredText, bool CurrentToolIsRecords);

static class Native
{
    delegate bool WindowCallback(nint window, nint data);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(nint parent, WindowCallback callback, nint data);
    [DllImport("user32.dll")] public static extern bool IsWindow(nint window);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(nint window);
    [DllImport("user32.dll")] public static extern nint GetWindow(nint window, uint command);
    [DllImport("user32.dll")] public static extern nint GetParent(nint window);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(nint window, out uint processId);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(nint window);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(nint window);
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] public static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll", SetLastError = true)] static extern bool GetWindowRect(nint window, out Rect rectangle);
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool GetClientRect(nint window, out Rect rectangle);
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] public static extern bool SetWindowPos(nint window, nint after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", SetLastError = true)] static extern nint SendMinMax(nint window, uint message, nint wp, [In, Out] ref MinMaxInfo info, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool ClientToScreen(nint window, ref Point point);
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool ScreenToClient(nint window, ref Point point);
    [DllImport("user32.dll")] static extern int GetMenuItemCount(nint menu);
    [DllImport("user32.dll")] static extern uint GetMenuItemID(nint menu, int position);
    [DllImport("user32.dll")] static extern uint GetMenuState(nint menu, uint position, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetMenuStringW", CharSet = CharSet.Unicode)] static extern int GetMenuString(nint menu, uint position, [Out] char[] text, int capacity, uint flags);
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool GetMenuItemRect(nint window, nint menu, uint position, out Rect rectangle);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] public static extern nint GetWindowLongPtr(nint window, int index);
    [DllImport("user32.dll")] public static extern uint GetClipboardSequenceNumber();
    [DllImport("user32.dll", EntryPoint = "GetWindowTextW", CharSet = CharSet.Unicode)] static extern int GetWindowText(nint window, StringBuilder text, int capacity);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")] static extern nint SendMessage(nint window, uint message, nint wp, nint lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint SendText(nint window, uint message, nint wp, string lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "PostMessageW", SetLastError = true)] static extern bool PostMessage(nint window, uint message, nint wp, nint lp);
    [DllImport("kernel32.dll", SetLastError = true)] static extern nint GlobalAlloc(uint flags, nuint bytes);
    [DllImport("kernel32.dll", SetLastError = true)] static extern nint GlobalLock(nint memory);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GlobalUnlock(nint memory);
    [DllImport("kernel32.dll", SetLastError = true)] static extern nint GlobalFree(nint memory);
    [DllImport("kernel32.dll", SetLastError = true)] static extern SafeProcessHandle OpenProcess(uint access, [MarshalAs(UnmanagedType.Bool)] bool inherit, uint pid);
    [DllImport("kernel32.dll", SetLastError = true)] static extern nint VirtualAllocEx(SafeProcessHandle process, nint address, nuint size, uint type, uint protection);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool VirtualFreeEx(SafeProcessHandle process, nint address, nuint size, uint type);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool WriteProcessMemory(SafeProcessHandle process, nint address, [In] byte[] buffer, nuint size, out nuint written);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool ReadProcessMemory(SafeProcessHandle process, nint address, [Out] byte[] buffer, nuint size, out nuint read);
    [DllImport("gdi32.dll", EntryPoint = "AddFontResourceExW", CharSet = CharSet.Unicode, SetLastError = true)] public static extern int AddFontResourceEx(string file, uint flags, nint reserved);
    [DllImport("gdi32.dll", EntryPoint = "RemoveFontResourceExW", CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool RemoveFontResourceEx(string file, uint flags, nint reserved);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] struct GuiThreadInfo { public uint Size, Flags; public nint Active, Focus, Capture, MenuOwner, MoveSize, Caret; public Rect CaretRectangle; }
    [DllImport("user32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool GetGUIThreadInfo(uint thread, ref GuiThreadInfo info);
    public static nint ThreadFocus(nint window)
    {
        var thread = GetWindowThreadProcessId(window, out _);
        if (thread == 0) throw new InvalidOperationException("Focus observation target has no live native thread");
        var info = new GuiThreadInfo { Size = checked((uint)Marshal.SizeOf<GuiThreadInfo>()) };
        if (!GetGUIThreadInfo(thread, ref info)) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Cannot observe the actual target thread focus");
        return info.Focus;
    }
    [StructLayout(LayoutKind.Sequential)] struct MinMaxInfo { public Point Reserved, MaximumSize, MaximumPosition, MinimumTrackSize, MaximumTrackSize; }
    [StructLayout(LayoutKind.Sequential)] struct ToolInfo { public uint Size, Flags; public nint Window; public nuint Id; public Rect Rectangle; public nint Instance, Text, Data, Reserved; }
    public sealed record TooltipQuery(int ToolCount, bool Registered, string? Text, bool Current, double? MotionMilliseconds = null, double? TextQueryMilliseconds = null);

    public static Rect WindowRectangle(nint window) { if (!GetWindowRect(window, out var rectangle)) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Native window bounds unavailable"); return rectangle; }
    public static Rect ClientRectangle(nint window) { if (!GetClientRect(window, out var rectangle)) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Native client bounds unavailable"); return rectangle; }
    public static Point MinimumTrackSize(nint window)
    {
        var info = new MinMaxInfo();
        if (SendMinMax(window, 0x24, 0, ref info, 0x22, 1000, out _) == 0) throw new TimeoutException("Native WM_GETMINMAXINFO query timed out");
        return info.MinimumTrackSize;
    }

    public static TooltipQuery QueryRecordsTooltip(nint tooltip, nint records, int pid, Point? motion = null)
    {
        if (IntPtr.Size != 8 || Marshal.SizeOf<ToolInfo>() != 72 || Marshal.OffsetOf<ToolInfo>(nameof(ToolInfo.Text)).ToInt32() != 48)
            throw new PlatformNotSupportedException("The native tooltip query requires the repository's Windows x64 TOOLINFOW layout");
        if (GetWindowThreadProcessId(tooltip, out var tooltipPid) == 0 || tooltipPid != pid || GetWindowThreadProcessId(records, out var recordsPid) == 0 || recordsPid != pid)
            throw new InvalidOperationException("Tooltip query is restricted to the actual records and tooltip HWNDs owned by this Aegisub host");
        var count = checked((int)Send(tooltip, 0x040D));
        if (count is < 0 or > 512) throw new InvalidOperationException("Observed tooltip tool count exceeds the bounded query inventory");
        using var process = OpenProcess(0x0038, false, checked((uint)pid));
        if (process.IsInvalid) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Cannot open owned host for a standard tooltip query buffer");
        const int textCharacters = 4096, textOffset = 128, allocationBytes = textOffset + textCharacters * 2;
        var buffer = VirtualAllocEx(process, 0, allocationBytes, 0x3000, 4);
        if (buffer == 0) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Cannot allocate bounded tooltip query buffer in owned host");
        try
        {
            var info = new ToolInfo { Size = checked((uint)Marshal.SizeOf<ToolInfo>()) };
            WriteToolInfo(process, buffer, info);
            var current = false;
            if (Send(tooltip, 0x043B, 0, buffer.ToInt64()) != 0)
            {
                var currentInfo = MemoryMarshal.Read<ToolInfo>(ReadRemote(process, buffer, Marshal.SizeOf<ToolInfo>()));
                current = currentInfo.Window == records && currentInfo.Id == unchecked((nuint)records);
            }
            for (var index = 0; index < count; ++index)
            {
                WriteToolInfo(process, buffer, info);
                if (Send(tooltip, 0x043A, index, buffer.ToInt64()) == 0)
                {
                    if (Send(tooltip, 0x040D) <= index) break;
                    throw new InvalidOperationException("Native tooltip enumeration failed for a still-present index");
                }
                var item = MemoryMarshal.Read<ToolInfo>(ReadRemote(process, buffer, Marshal.SizeOf<ToolInfo>()));
                if (item.Window != records || item.Id != unchecked((nuint)records)) continue;
                item.Text = buffer + textOffset;
                WriteRemote(process, item.Text, new byte[textCharacters * 2]);
                WriteToolInfo(process, buffer, item);
                Stopwatch? motionClock = null; double? motionMilliseconds = null, textQueryMilliseconds = null;
                if (motion is { } point)
                {
                    motionClock = Stopwatch.StartNew();
                    Send(records, 0x200, 0, Position(point.X, point.Y));
                    motionMilliseconds = motionClock.Elapsed.TotalMilliseconds;
                }
                Send(tooltip, 0x0438, textCharacters, buffer.ToInt64());
                if (motionClock is not null) textQueryMilliseconds = motionClock.Elapsed.TotalMilliseconds - motionMilliseconds!.Value;
                var bytes = ReadRemote(process, item.Text, textCharacters * 2);
                var characters = MemoryMarshal.Cast<byte, char>(bytes.AsSpan());
                var terminator = characters.IndexOf('\0');
                if (terminator < 0 || terminator >= textCharacters - 1) throw new InvalidOperationException("Native tooltip text is not terminated within the bounded query buffer");
                return new TooltipQuery(count, true, new string(characters[..terminator]), current, motionMilliseconds, textQueryMilliseconds);
            }
            return new TooltipQuery(count, false, null, current);
        }
        finally
        {
            if (!VirtualFreeEx(process, buffer, 0, 0x8000)) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Failed to release the owned-host tooltip query buffer");
        }
    }

    static void WriteToolInfo(SafeProcessHandle process, nint address, ToolInfo info)
    {
        var bytes = new byte[Marshal.SizeOf<ToolInfo>()]; MemoryMarshal.Write(bytes.AsSpan(), in info); WriteRemote(process, address, bytes);
    }
    static void WriteRemote(SafeProcessHandle process, nint address, byte[] bytes)
    {
        if (!WriteProcessMemory(process, address, bytes, (nuint)bytes.Length, out var written) || written != (nuint)bytes.Length)
            throw new Win32Exception(Marshal.GetLastPInvokeError(), "Cannot write the complete standard tooltip query buffer");
    }
    static byte[] ReadRemote(SafeProcessHandle process, nint address, int size)
    {
        var bytes = new byte[size];
        if (!ReadProcessMemory(process, address, bytes, (nuint)size, out var read) || read != (nuint)size)
            throw new Win32Exception(Marshal.GetLastPInvokeError(), "Cannot read the complete standard tooltip query buffer");
        return bytes;
    }
    public sealed record MenuItem(int Position, string Label, uint Id, bool Enabled);
    public static MenuItem[] Items(nint menu)
    {
        var count = GetMenuItemCount(menu);
        if (count < 0) throw new InvalidOperationException("Observed native menu disappeared");
        return Enumerable.Range(0, count).Select(position => {
            var text = new char[256]; var length = GetMenuString(menu, (uint)position, text, text.Length, 0x400);
            if (length >= text.Length - 1) throw new InvalidOperationException("Native menu label was truncated");
            var state = GetMenuState(menu, (uint)position, 0x400);
            if (state == uint.MaxValue) throw new InvalidOperationException("Observed native menu disappeared");
            return new MenuItem(position, new string(text, 0, length).Split('\t')[0].Replace("&", "", StringComparison.Ordinal), GetMenuItemID(menu, position), (state & 3) == 0);
        }).ToArray();
    }
    public static Rect MenuItemBounds(nint menu, int position) { if (!GetMenuItemRect(0, menu, (uint)position, out var rectangle)) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Native menu item bounds unavailable"); return rectangle; }
    public static bool Highlighted(nint menu, int position) { var state = GetMenuState(menu, (uint)position, 0x400); if (state == uint.MaxValue) throw new InvalidOperationException("Observed native menu disappeared"); return (state & 0x80) != 0; }
    public static Point ToScreen(nint window, int x, int y) { var point = new Point { X = x, Y = y }; if (!ClientToScreen(window, ref point)) throw new Win32Exception(Marshal.GetLastPInvokeError(), "ClientToScreen failed"); return point; }
    public static Point ToClient(nint window, int x, int y) { var point = new Point { X = x, Y = y }; if (!ScreenToClient(window, ref point)) throw new Win32Exception(Marshal.GetLastPInvokeError(), "ScreenToClient failed"); return point; }
    public static long Position(int x, int y) => unchecked((int)((ushort)x | (uint)(ushort)y << 16));
    public static object Bounds(nint window) { if (!GetWindowRect(window, out var rectangle)) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Native window bounds unavailable"); return new { left = rectangle.Left, top = rectangle.Top, right = rectangle.Right, bottom = rectangle.Bottom }; }
    public static nint[] Children(nint parent) { List<nint> result = []; WindowCallback callback = (window, _) => { result.Add(window); return true; }; EnumChildWindows(parent, callback, 0); GC.KeepAlive(callback); return result.ToArray(); }
    public static string Caption(nint window) { var text = new StringBuilder(2048); GetWindowText(window, text, text.Capacity); return text.ToString(); }
    public static long Send(nint window, uint message, long wp = 0, long lp = 0) { if (SendMessage(window, message, (nint)wp, (nint)lp, 0x22, 1000, out var result) == 0) throw new TimeoutException("Native control message timed out"); return result.ToInt64(); }
    public static void SetText(nint window, string value) { if (SendText(window, 0xC, 0, value, 0x22, 1000, out _) == 0) throw new TimeoutException("Native text entry timed out"); }
    public static void ReplaceText(nint window, string value) { Send(window, 0xB1, 0, -1); if (SendText(window, 0xC2, 1, value, 0x22, 1000, out _) == 0) throw new TimeoutException("Native edit replacement timed out"); }
    public static void Post(nint window, uint message, long wp = 0, long lp = 0) { if (!PostMessage(window, message, (nint)wp, (nint)lp)) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Native message post failed"); }
    public static void Drop(nint window, string[] paths)
    {
        var payload = Encoding.Unicode.GetBytes(string.Join('\0', paths) + "\0\0");
        var memory = GlobalAlloc(0x42, (nuint)(20 + payload.Length));
        if (memory == 0) throw new Win32Exception(Marshal.GetLastPInvokeError(), "DROPFILES allocation failed");
        var transferred = false;
        try
        {
            var pointer = GlobalLock(memory); if (pointer == 0) throw new Win32Exception(Marshal.GetLastPInvokeError(), "DROPFILES lock failed");
            try { Marshal.WriteInt32(pointer, 0, 20); Marshal.WriteInt32(pointer, 16, 1); Marshal.Copy(payload, 0, pointer + 20, payload.Length); }
            finally { GlobalUnlock(memory); }
            if (!PostMessage(window, 0x233, memory, 0)) throw new Win32Exception(Marshal.GetLastPInvokeError(), "WM_DROPFILES post failed");
            transferred = true;
        }
        finally { if (!transferred) GlobalFree(memory); }
    }
}
