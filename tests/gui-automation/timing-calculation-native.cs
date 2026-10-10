#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

if (args.Length != 2)
{
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/timing-calculation-native.cs -- EXE ARTIFACTS");
    return 2;
}
try { return PrivateDesktop.Run(args, Path.GetFullPath(args[1]), isolation => new Scenario(args, isolation).Run()); }
catch (Exception error) { Console.Error.WriteLine(error); return 1; }

sealed class Scenario(string[] args, PrivateDesktop isolation)
{
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    static readonly int[] Initial = [110, 120, 30, 40, 50, 60, 70, 80];
    static readonly int[] Applied = [100, 200, 200, 600, 50, 60, 70, 80];
    static readonly string[] Labels = ["Add lead in:", "Add lead out:", "Max gap:", "Max overlap:", "Starts before thres.:", "Starts after thres.:", "Ends before thres.:", "Ends after thres.:"];
    static readonly string[] SeedEvents =
    [
        "Dialogue: 4,0:00:11.40,0:00:12.00,Alternate,d-gap,41,42,43,fx-d,{\\bord3}gap follower",
        "Dialogue: 1,0:00:01.00,0:00:02.00,Default,a-clamp,11,12,13,fx-a,{\\i1}café, 世界",
        "Comment: 9,0:00:01.90,0:00:02.90,Alternate,comment,91,92,93,sentinel,{\\b1}untouched 😀",
        "Dialogue: 8,0:00:30.00,0:00:32.00,Alternate,h-tie,81,82,83,fx-h,same start H",
        "Dialogue: 5,0:00:20.00,0:00:21.00,Third,e-overlap,51,52,53,fx-e,overlap predecessor",
        "Dialogue: 0,0:00:00.05,0:00:00.50,Default,zero,1,2,3,fx-zero,zero clamp",
        "Dialogue: 2,0:00:02.15,0:00:03.00,Alternate,b-clamp,21,22,23,fx-b,{\\b1}safe follower{\\b0}",
        "Dialogue: 9,0:00:40.00,0:00:41.00,Third,i-far,101,102,103,fx-i,far isolated 😀",
        "Dialogue: 3,0:00:10.00,0:00:11.00,Default,c-gap,31,32,33,fx-c,positive gap",
        "Dialogue: 7,0:00:30.00,0:00:32.00,Default,g-tie,71,72,73,fx-g,same start G",
        "Dialogue: 6,0:00:20.80,0:00:22.00,Alternate,f-overlap,61,62,63,fx-f,negative overlap",
    ];
    static readonly string[] ExpectedEvents =
    [
        "Dialogue: 4,0:00:11.23,0:00:12.20,Alternate,d-gap,41,42,43,fx-d,{\\bord3}gap follower",
        "Dialogue: 1,0:00:00.75,0:00:02.05,Default,a-clamp,11,12,13,fx-a,{\\i1}café, 世界",
        "Comment: 9,0:00:01.90,0:00:02.90,Alternate,comment,91,92,93,sentinel,{\\b1}untouched 😀",
        "Dialogue: 8,0:00:29.90,0:00:32.20,Alternate,h-tie,81,82,83,fx-h,same start H",
        "Dialogue: 5,0:00:19.90,0:00:21.08,Third,e-overlap,51,52,53,fx-e,overlap predecessor",
        "Dialogue: 0,0:00:00.00,0:00:00.75,Default,zero,1,2,3,fx-zero,zero clamp",
        "Dialogue: 2,0:00:02.05,0:00:03.20,Alternate,b-clamp,21,22,23,fx-b,{\\b1}safe follower{\\b0}",
        "Dialogue: 9,0:00:39.90,0:00:41.20,Third,i-far,101,102,103,fx-i,far isolated 😀",
        "Dialogue: 3,0:00:09.90,0:00:11.23,Default,c-gap,31,32,33,fx-c,positive gap",
        "Dialogue: 7,0:00:29.90,0:00:32.20,Default,g-tie,71,72,73,fx-g,same start G",
        "Dialogue: 6,0:00:21.08,0:00:22.20,Alternate,f-overlap,61,62,63,fx-f,negative overlap",
    ];
    readonly string artifacts = Path.GetFullPath(args[1]);
    readonly List<object> checks = [];
    Process? app;
    nint main, dialog, leadIn, leadOut, adjacent, keyframe, selection, slider;
    nint[] edits = [];
    string fixture = "", profile = "", phase = "";
    string[] baseline = [];
    int launch;

    public int Run()
    {
        Directory.CreateDirectory(artifacts); isolation.RecordDriver();
        using var log = new StreamWriter(Path.Combine(artifacts, "driver.log")) { AutoFlush = true };
        Console.SetOut(log); Console.SetError(log);
        using var watchdog = new Timer(_ => {
            File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { passed = false, error = "180-second watchdog expired; forced cleanup is not a normal exit", checks }, Json));
            try { if (app is not null) isolation.Stop(app); } finally { Environment.Exit(124); }
        }, null, 180_000, Timeout.Infinite);
        try
        {
            Prepare(); Start();
            baseline = SaveAs("baseline");
            Require(baseline.Where(IsEvent).SequenceEqual(SeedEvents), "Native baseline preserves all eleven literal events in their original unsorted file order");
            phase = "initial"; Open(); VerifyValues(Initial, 37, false); Capture("initial-dialog");
            SetValues([210, 320, 430, 540, 50, 60, 70, 80], 24, true);
            Check(leadIn, false); Check(selection, true);
            Capture("cancel-edits"); CloseDialog("Cancel");
            Require(!Text(main).StartsWith('*'), "Cancel does not mark the subtitle document dirty");
            Require(SaveAs("after-cancel").SequenceEqual(baseline), "Cancel preserves every saved header, style, project property and event field");
            phase = "apply"; Open(); VerifyValues(Initial, 37, false);
            Action("button", "None"); ObservedWindowsCommand.PostButtonClick(Control("None"), app!.Id);
            Wait(() => !Native.IsWindowEnabled(Control("OK")), "Native None disables OK when no style is eligible");
            Action("button", "All"); ObservedWindowsCommand.PostButtonClick(Control("All"), app!.Id);
            Wait(() => Native.IsWindowEnabled(Control("OK")), "Native All enables OK with all three styles eligible");
            RejectInvalidInput();
            phase = "apply-after-invalid-cancel"; Open(); VerifyValues(Initial, 37, false);
            SetValues(Applied, 25, true); Capture("apply-values");
            CloseDialog("OK");
            Require(Text(main).StartsWith('*'), "OK publishes the timing change to the actual document");
            var actual = SaveAs("after-ok"); var expected = Expected();
            Require(actual.SequenceEqual(expected), "Complete saved ASS equals the literal oracle for clamped leads, positive gap, negative overlap, tied starts, untouched comment and all non-time fields");
            isolation.VerifyWindow(main, app!); ScreenCapture.SaveWindowPng(main, Path.Combine(artifacts, "saved-result.png"));
            NormalExit();
            File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { passed = true, checks }, Json));
            return 0;
        }
        catch (Exception error)
        {
            if (app is not null && !app.HasExited)
                try { Dump("failure"); } catch (Exception inventoryError) { Console.Error.WriteLine(inventoryError); }
            File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { passed = false, checks, error = error.ToString() }, Json));
            Console.Error.WriteLine(error); return 1;
        }
        finally { if (app is not null) { isolation.Stop(app); app.Dispose(); } }
    }

    void Require(bool condition, string description)
    {
        checks.Add(new { phase, description, passed = condition }); Console.WriteLine(description + ": " + condition);
        if (!condition) throw new InvalidOperationException(description);
    }
    void Wait(Func<bool> predicate, string description)
    {
        var watch = Stopwatch.StartNew();
        while (watch.ElapsedMilliseconds < 10_000)
        {
            isolation.VerifyCurrent();
            if (app is { HasExited: true }) throw new InvalidOperationException("Host exited: " + description);
            if (predicate()) return;
            Thread.Sleep(50);
        }
        throw new TimeoutException(description);
    }
    string Text(nint window) => isolation.ReadText(window, app!);
    nint[] Children(nint window) => Native.Children(window).Where(Native.IsWindowVisible).ToArray();
    nint[] Windows() => isolation.Windows(app!).Where(Native.IsWindowVisible).ToArray();
    nint Control(string label) => Children(dialog).Single(h => Text(h).Replace("&", "") == label);
    long Send(nint window, uint message, long wp = 0, long lp = 0)
    {
        isolation.VerifyWindow(window, app!); return Native.Send(window, message, wp, lp);
    }
    void Action(string action, string target) => File.AppendAllText(Path.Combine(artifacts, "actions.ndjson"), JsonSerializer.Serialize(new { phase, action, target }) + Environment.NewLine);
    void Command(string menu, string label)
    {
        isolation.VerifyWindow(main, app!); Action("menu", menu + "/" + label);
        ObservedWindowsCommand.PostMenuCommand(main, app!.Id, ObservedWindowsCommand.FindMenuCommand(main, app.Id, menu, label));
    }
    void Open()
    {
        Command("Timing", "Timing Post-Processor...");
        Wait(() => { dialog = Windows().SingleOrDefault(h => Text(h) == "Timing Post-Processor"); return dialog != 0; }, "Actual Timing Post-Processor dialog opened");
        var children = Children(dialog);
        var candidates = children.Where(h => PrivateDesktop.ClassName(h) == "Edit").ToArray();
        Require(candidates.Length == 8, "Exactly eight actual native Edit controls observed");
        edits = Labels.Select(label => {
            var anchor = Control(label); var bounds = Native.Bounds(anchor);
            var matches = candidates.Where(h => {
                var b = Native.Bounds(h);
                return b.Left >= bounds.Right && Math.Abs(b.CenterY - bounds.CenterY) <= 12;
            }).OrderBy(h => Native.Bounds(h).Left).ToArray();
            if (matches.Length == 0) throw new InvalidOperationException("No adjacent Edit for " + label);
            return matches[0];
        }).ToArray();
        Require(edits.Distinct().Count() == 8, "Each numeric field identified uniquely by its actual adjacent label, Edit class and row bounds");
        leadIn = Control("Add lead in:"); leadOut = Control("Add lead out:"); selection = Control("Affect selection only");
        nint GroupEnable(string group)
        {
            var bounds = Native.Bounds(Control(group));
            return children.Single(h => Text(h).Replace("&", "") == "Enable" && PrivateDesktop.ClassName(h) == "Button" && bounds.Contains(Native.Bounds(h)));
        }
        adjacent = GroupEnable("Make adjacent subtitles continuous"); keyframe = GroupEnable("Keyframe snapping");
        slider = children.Single(h => PrivateDesktop.ClassName(h) == "msctls_trackbar32");
        Require(adjacent != keyframe, "The two Enable checkboxes identified by native class and enclosing labeled group bounds");
        Dump(phase + "-inventory");
    }
    void Check(nint control, bool value)
    {
        if (Send(control, 0xF0) != (value ? 1 : 0))
        {
            Require(Native.IsWindowEnabled(control), "Checkbox is enabled before native click");
            Action("native-checkbox-click", Text(control)); Native.Post(control, 0xF5, 0, 0);
            Wait(() => Send(control, 0xF0) == (value ? 1 : 0), "Native checkbox click updated " + Text(control));
        }
    }
    void SetValues(int[] values, int bias, bool enableAdjacent)
    {
        Check(leadIn, true); Check(leadOut, true); Check(adjacent, enableAdjacent); Check(selection, false);
        for (var i = 0; i < 4; ++i)
        {
            Require(Native.IsWindowEnabled(edits[i]), "Numeric control is enabled for actual input: " + Labels[i]);
            isolation.VerifyWindow(edits[i], app!); Action("native-edit-text", Labels[i] + values[i]);
            Native.SetText(edits[i], values[i].ToString(CultureInfo.InvariantCulture));
        }
        Action("native-slider-position", bias.ToString(CultureInfo.InvariantCulture)); Send(slider, 0x405, 1, bias);
        VerifyValues(values, bias, enableAdjacent);
    }
    void VerifyValues(int[] values, int bias, bool enableAdjacent)
    {
        Require(edits.Select(h => int.Parse(Text(h), CultureInfo.InvariantCulture)).SequenceEqual(values), "All eight labeled native numeric values read back: " + string.Join(',', values));
        Require(Send(slider, 0x400) == bias, "Native bias slider position is " + bias);
        Require(Send(leadIn, 0xF0) == 1 && Send(leadOut, 0xF0) == 1, "Both lead checkboxes are checked");
        Require(Send(adjacent, 0xF0) == (enableAdjacent ? 1 : 0), "Adjacent checkbox matches the stored preference");
        Require(Send(selection, 0xF0) == 0, "Affect selection only is unchecked");
        Require(!Native.IsWindowEnabled(keyframe) && Send(keyframe, 0xF0) == 0 && edits.Skip(4).All(h => !Native.IsWindowEnabled(h)), "Without video/keyframes, keyframe checkbox and threshold controls remain disabled and checkbox unchecked");
    }
    void RejectInvalidInput()
    {
        phase = "invalid-input";
        SetValues(Applied, 25, true);
        Require(Native.IsWindowEnabled(edits[0]), "Lead-in Edit is enabled before invalid native input");
        isolation.VerifyWindow(edits[0], app!); Action("native-edit-text", "Add lead in:not-an-integer");
        Native.SetText(edits[0], "not-an-integer");
        Require(Text(edits[0]) == "not-an-integer", "Actual lead-in Edit contains invalid text without forced focus or normalization");
        Capture("invalid-input");
        var processor = dialog;
        Action("button", "OK/invalid-input"); ObservedWindowsCommand.PostButtonClick(Control("OK"), app!.Id);
        nint warning = 0;
        Wait(() => {
            warning = Windows().SingleOrDefault(h => PrivateDesktop.ClassName(h) == "#32770"
                && Native.GetWindow(h, 4) == processor && Text(h) == "Invalid timing");
            return warning != 0;
        }, "Processor-owned Invalid timing dialog opened after invalid transfer");
        isolation.VerifyWindow(warning, app!); isolation.VerifyWindow(processor, app!);
        var elements = AutomationElement.FromHandle(warning).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
        var accessible = elements.Select(e => new { name = e.Current.Name, control_type = e.Current.ControlType.ProgrammaticName,
            automation_id = e.Current.AutomationId, handle = e.Current.NativeWindowHandle }).ToArray();
        File.WriteAllText(Path.Combine(artifacts, "invalid-timing-accessibility.json"), JsonSerializer.Serialize(accessible, Json));
        File.WriteAllText(Path.Combine(artifacts, "invalid-timing-owner.json"), JsonSerializer.Serialize(new {
            warning = warning.ToString("X"), observed_owner = Native.GetWindow(warning, 4).ToString("X"),
            processor = processor.ToString("X"), processor_owner = Native.GetWindow(processor, 4).ToString("X"),
            main = main.ToString("X"), process_id = app!.Id, same_owned_process_and_private_desktop_verified = true
        }, Json));
        Dump("invalid-timing-inventory");
        ScreenCapture.SaveWindowPng(warning, Path.Combine(artifacts, "invalid-timing-error.png"));
        Require(Native.GetWindow(warning, 4) == processor && Native.GetWindow(processor, 4) == main,
            "Observed GW_OWNER chain is actual Invalid timing popup -> processor dialog -> owned main window");
        Require(Children(warning).Any(h => PrivateDesktop.ClassName(h) == "DirectUIHWND"), "Actual error popup is the native wx TaskDialog with DirectUI content");
        Require(accessible.Any(e => e.control_type == "ControlType.Text"
            && e.name == "Timing values must be nonnegative integers. No subtitles were changed."), "Actual TaskDialog UIA Text exposes the exact invalid-input explanation");
        var accessibleOk = elements.Single(e => e.Current.ControlType == ControlType.Button
            && e.Current.Name.Replace("&", "") == "OK" && e.Current.NativeWindowHandle != 0);
        var ok = (nint)accessibleOk.Current.NativeWindowHandle;
        Require(Native.IsChild(warning, ok), "Unique actual UIA OK button has an observed native HWND inside the TaskDialog");
        isolation.VerifyWindow(ok, app!);
        Native.GetWindowThreadProcessId(ok, out var okProcessId);
        Require(okProcessId == app!.Id, "Observed UIA OK HWND belongs to the owned host process");
        Require(elements.Count(e => e.Current.ControlType == ControlType.Button && e.Current.NativeWindowHandle != 0) == 1,
            "Observed only-OK TaskDialog exposes exactly one native command button");
        Require(accessibleOk.Current.AutomationId == "CommandButton_2", "Actual only-OK wx TaskDialog exposes CommandButton_2 matching its documented IDCANCEL protocol");
        var logicalId = int.Parse(accessibleOk.Current.AutomationId["CommandButton_".Length..], CultureInfo.InvariantCulture);
        Require(logicalId == 2, "Observed logical command ID matches wxMSW only-wxOK IDCANCEL mapping");
        var supportsInvoke = accessibleOk.TryGetCurrentPattern(InvokePattern.Pattern, out _);
        File.WriteAllText(Path.Combine(artifacts, "invalid-timing-dismissal.json"), JsonSerializer.Serialize(new {
            popup = warning.ToString("X"), ok = ok.ToString("X"), ok_process_id = okProcessId,
            is_popup_child = Native.IsChild(warning, ok), native_control_id = Native.GetDlgCtrlID(ok),
            uia_name = accessibleOk.Current.Name, uia_automation_id = accessibleOk.Current.AutomationId,
            invoke_pattern_supported = supportsInvoke, logical_button_id = logicalId,
            dismissal_message = "TDM_CLICK_BUTTON",
            source_protocol = "wx3.3.1 src/msw/msgdlg.cpp:725-739 only-wxOK adds IDCANCEL as OK; :540-547 maps IDCANCEL to wxID_OK",
            popup_owner = Native.GetWindow(warning, 4).ToString("X"), processor_owner = Native.GetWindow(processor, 4).ToString("X")
        }, Json));
        Action("task-dialog-button", "Invalid timing/observed CommandButton_2 via wxMSW only-OK protocol"); Send(warning, 0x466, logicalId);
        Wait(() => !Windows().Contains(warning) && Windows().Contains(processor) && Native.IsWindowEnabled(processor),
            "Observed only-OK TDM_CLICK_BUTTON command dismisses only the error popup and keeps the processor open");
        Require(dialog == processor && Text(edits[0]) == "not-an-integer", "Rejected transfer preserves the original processor dialog and exact invalid lead-in text");
        Require(!Text(main).StartsWith('*'), "Rejected transfer does not mark the actual subtitle document dirty");
        Dump("after-invalid-dismiss-inventory"); Capture("after-invalid-dismiss");
        CloseDialog("Cancel");
        Require(!Text(main).StartsWith('*'), "Cancel after rejected transfer keeps the subtitle document clean");
        Require(SaveAs("after-invalid-cancel").SequenceEqual(baseline), "Rejected transfer then Cancel preserves the entire baseline ASS without partial publication");
    }
    void CloseDialog(string button)
    {
        var old = dialog; Action("button", button); ObservedWindowsCommand.PostButtonClick(Control(button), app!.Id);
        Wait(() => !Windows().Contains(old), "Native " + button + " closed the timing dialog"); dialog = 0;
    }
    void Dump(string label)
    {
        File.WriteAllText(Path.Combine(artifacts, label + ".json"), JsonSerializer.Serialize(Windows().Select(h => new {
            handle = h.ToString("X"), text = Text(h), class_name = PrivateDesktop.ClassName(h), bounds = Native.Bounds(h),
            children = Children(h).Select(c => new { handle = c.ToString("X"), text = Text(c), class_name = PrivateDesktop.ClassName(c), bounds = Native.Bounds(c), enabled = Native.IsWindowEnabled(c), id = Native.GetDlgCtrlID(c) })
        }), Json));
    }
    void Capture(string label)
    {
        isolation.VerifyWindow(dialog, app!); ScreenCapture.SaveWindowPng(dialog, Path.Combine(artifacts, label + ".png"));
    }
    string[] SaveAs(string label)
    {
        Command("File", "Save Subtitles as..."); nint save = 0;
        Wait(() => { save = Windows().SingleOrDefault(h => PrivateDesktop.ClassName(h) == "#32770" && Text(h).Contains("Save")); return save != 0; }, "Native Save As dialog opened");
        var elements = AutomationElement.FromHandle(save).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
        var edit = elements.Single(e => e.Current.ControlType == ControlType.Edit && e.Current.AutomationId == "1001");
        var path = Path.Combine(artifacts, label + ".ass"); var handle = (nint)edit.Current.NativeWindowHandle;
        isolation.VerifyWindow(handle, app!); Native.SetText(handle, path);
        var button = elements.Single(e => e.Current.ControlType == ControlType.Button && e.Current.AutomationId == "1" && e.Current.NativeWindowHandle != 0);
        isolation.VerifyWindow((nint)button.Current.NativeWindowHandle, app!);
        ObservedWindowsCommand.PostButtonClick((nint)button.Current.NativeWindowHandle, app!.Id);
        Wait(() => File.Exists(path) && !Windows().Contains(save), "Native Save As completed: " + label);
        Require(!Text(main).StartsWith('*'), "Native Save As cleared dirty state"); return File.ReadAllLines(path);
    }
    static bool IsEvent(string line) => line.StartsWith("Dialogue:") || line.StartsWith("Comment:");
    string[] Expected()
    {
        var index = 0;
        var expected = baseline.Select(line => IsEvent(line) ? ExpectedEvents[index++] : line).ToArray();
        Require(index == ExpectedEvents.Length, "Complete literal event oracle covers every saved event");
        File.WriteAllLines(Path.Combine(artifacts, "expected.ass"), expected, new UTF8Encoding(true)); return expected;
    }
    void NormalExit()
    {
        isolation.VerifyWindow(main, app!); Action("normal-exit", "main/WM_CLOSE"); Native.Post(main, 0x10, 0, 0);
        Require(app!.WaitForExit(10_000), "WM_CLOSE exits normally within ten seconds without forced termination");
        var exitCode = isolation.ExitCode(app);
        Require(exitCode == 0, "Normal host native exit code is zero (actual " + exitCode + ")"); app.Dispose(); app = null; main = 0;
    }
    void Prepare()
    {
        var host = Path.Combine(artifacts, "host"); profile = Path.Combine(host, "profile");
        Directory.CreateDirectory(Path.Combine(profile, "user"));
        var config = JsonNode.Parse("""
            {"Subtitle":{"Use STC":false},"App":{"First Start":false,"Language":"en_US","Auto":{"Backup":false,"Check For Updates":false,"Load Linked Files":1,"Save":false,"Save on Every Change":false}},"Tool":{"Timing Post Processor":{"Lead":{"IN":110,"OUT":120},"Threshold":{"Adjacent Gap":30,"Adjacent Overlap":40,"Key Start Before":50,"Key Start After":60,"Key End Before":70,"Key End After":80},"Adjacent Bias":0.37,"Enable":{"Lead":{"IN":true,"OUT":true},"Adjacent":false,"Keyframe":true},"Only Selection":false}}}
            """)!;
        File.WriteAllText(Path.Combine(profile, "user", "config.json"), config.ToJsonString(Json));
        File.WriteAllText(Path.Combine(artifacts, "initial-preferences.json"), config["Tool"]!["Timing Post Processor"]!.ToJsonString(Json));
        fixture = Path.Combine(host, "fixture.ass");
        var header = """
            [Script Info]
            Title: Native Timing Calculation E2E
            Original Script: metadata sentinel café 世界
            ScriptType: v4.00+
            PlayResX: 640
            PlayResY: 480
            WrapStyle: 2
            ScaledBorderAndShadow: yes
            [V4+ Styles]
            Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
            """;
        var styles = new[] { "Default", "Alternate", "Third" }.Select(style => "Style: " + style + ",Arial,24,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1");
        File.WriteAllText(fixture, header + "\n" + string.Join('\n', styles) + "\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n" + string.Join('\n', SeedEvents) + "\n", new UTF8Encoding(true));
    }
    void Start()
    {
        var host = Path.Combine(artifacts, "launch-" + ++launch); Directory.CreateDirectory(host);
        var start = new ProcessStartInfo(Path.GetFullPath(args[0])) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", host, "--open", fixture }) start.ArgumentList.Add(argument);
        app = isolation.Start(start, "timing-calculation-" + launch);
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(host, "ready.json"), app, TimeSpan.FromSeconds(20));
        main = isolation.MainWindow(app, 10);
    }
}

static class Native
{
    delegate bool Callback(nint window, nint data);
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int Left, Top, Right, Bottom; }
    public sealed record Rectangle(int Left, int Top, int Right, int Bottom)
    {
        public int CenterY => (Top + Bottom) / 2;
        public bool Contains(Rectangle other) => other.Left >= Left && other.Top >= Top && other.Right <= Right && other.Bottom <= Bottom;
    }
    [DllImport("user32.dll")] static extern bool GetWindowRect(nint window, out Rect rect);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(nint parent, Callback callback, nint data);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(nint window);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(nint window);
    [DllImport("user32.dll")] public static extern nint GetWindow(nint window, uint command);
    [DllImport("user32.dll")] public static extern bool IsChild(nint parent, nint child);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(nint window, out uint processId);
    [DllImport("user32.dll", EntryPoint = "PostMessageW")] static extern bool PostMessage(nint window, uint message, nint wp, nint lp);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")] static extern nint SendMessage(nint window, uint message, nint wp, nint lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint SendText(nint window, uint message, nint wp, string lp, uint flags, uint timeout, out nint result);
    public static Rectangle Bounds(nint window) { if (!GetWindowRect(window, out var rect)) throw new InvalidOperationException("Native bounds unavailable"); return new(rect.Left, rect.Top, rect.Right, rect.Bottom); }
    public static nint[] Children(nint parent) { List<nint> found = []; Callback callback = (h, _) => { found.Add(h); return true; }; EnumChildWindows(parent, callback, 0); GC.KeepAlive(callback); return found.ToArray(); }
    public static long Send(nint window, uint message, long wp, long lp) { if (SendMessage(window, message, (nint)wp, (nint)lp, 0x22, 1000, out var result) == 0) throw new TimeoutException("Native control message"); return result.ToInt64(); }
    public static void SetText(nint window, string value) { if (SendText(window, 0xC, 0, value, 0x22, 1000, out _) == 0) throw new TimeoutException("Native text entry"); }
    public static void Post(nint window, uint message, long wp, long lp) { if (!PostMessage(window, message, (nint)wp, (nint)lp)) throw new InvalidOperationException("Native message post failed"); }
}
