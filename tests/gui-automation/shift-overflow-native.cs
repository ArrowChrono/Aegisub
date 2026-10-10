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
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/shift-overflow-native.cs -- EXE ARTIFACTS");
    return 2;
}
try { return PrivateDesktop.Run(args, Path.GetFullPath(args[1]), isolation => new Scenario(args, isolation).Run()); }
catch (Exception error) { Console.Error.WriteLine(error); return 1; }

sealed class Scenario(string[] args, PrivateDesktop isolation)
{
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    const string InitialHistory = """
        [{"filename":"legacy.ass","amount":"0:00:00.25","is by time":true,"is backward":false,"fields":2,"mode":1,"selection":[{"start":7,"end":8}]}]
        """;
    const string TimeHistory = """
        [{"filename":"after-late-cancel.ass","amount":"0:00:00.10","is by time":true,"is backward":true,"fields":1,"mode":0,"selection":[{"start":2,"end":3},{"start":5,"end":5}]},
         {"filename":"legacy.ass","amount":"0:00:00.25","is by time":true,"is backward":false,"fields":2,"mode":1,"selection":[{"start":7,"end":8}]}]
        """;
    const string FrameHistory = """
        [{"filename":"after-time-retry.ass","amount":"1","is by time":false,"is backward":false,"fields":0,"mode":1,"selection":[{"start":2,"end":3},{"start":5,"end":5}]},
         {"filename":"after-late-cancel.ass","amount":"0:00:00.10","is by time":true,"is backward":true,"fields":1,"mode":0,"selection":[{"start":2,"end":3},{"start":5,"end":5}]},
         {"filename":"legacy.ass","amount":"0:00:00.25","is by time":true,"is backward":false,"fields":2,"mode":1,"selection":[{"start":7,"end":8}]}]
        """;
    const string FinalHistory = """
        [{"filename":"after-frame-retry.ass","amount":"0","is by time":false,"is backward":false,"fields":2,"mode":2,"selection":[{"start":2,"end":3},{"start":5,"end":5}]},
         {"filename":"after-time-retry.ass","amount":"1","is by time":false,"is backward":false,"fields":0,"mode":1,"selection":[{"start":2,"end":3},{"start":5,"end":5}]},
         {"filename":"after-late-cancel.ass","amount":"0:00:00.10","is by time":true,"is backward":true,"fields":1,"mode":0,"selection":[{"start":2,"end":3},{"start":5,"end":5}]},
         {"filename":"legacy.ass","amount":"0:00:00.25","is by time":true,"is backward":false,"fields":2,"mode":1,"selection":[{"start":7,"end":8}]}]
        """;
    static readonly string[] SeedEvents =
    [
        "Dialogue: 0,0:00:00.05,0:00:00.50,Default,early,1,2,3,fx-zero,zero clamp",
        "Comment: 9,0:00:10.01,0:00:10.51,Alternate,pick-comment,91,92,93,sentinel,{\\b1}comment café 世界",
        "Dialogue: 2,0:00:20.00,0:00:22.57,Alternate,pick-one,21,22,23,fx-one,{\\i1}first, selected",
        "Dialogue: 3,0:00:30.01,0:00:32.78,Third,gap,31,32,33,fx-gap,unselected gap 😀",
        "Dialogue: 4,0:00:40.01,0:00:42.90,Default,pick-two,41,42,43,fx-two,{\\bord3}second selected",
        "Dialogue: 5,596:31:20.01,596:31:22.64,Third,late,51,52,53,fx-late,last overflow sentinel",
    ];
    static readonly string[] TimeEvents =
    [
        "Dialogue: 0,0:00:00.00,0:00:00.50,Default,early,1,2,3,fx-zero,zero clamp",
        "Comment: 9,0:00:09.91,0:00:10.51,Alternate,pick-comment,91,92,93,sentinel,{\\b1}comment café 世界",
        "Dialogue: 2,0:00:19.90,0:00:22.57,Alternate,pick-one,21,22,23,fx-one,{\\i1}first, selected",
        "Dialogue: 3,0:00:29.91,0:00:32.78,Third,gap,31,32,33,fx-gap,unselected gap 😀",
        "Dialogue: 4,0:00:39.91,0:00:42.90,Default,pick-two,41,42,43,fx-two,{\\bord3}second selected",
        "Dialogue: 5,596:31:19.91,596:31:22.64,Third,late,51,52,53,fx-late,last overflow sentinel",
    ];
    static readonly string[] FrameEvents =
    [
        "Dialogue: 0,0:00:00.00,0:00:00.50,Default,early,1,2,3,fx-zero,zero clamp",
        "Comment: 9,0:00:09.94,0:00:10.54,Alternate,pick-comment,91,92,93,sentinel,{\\b1}comment café 世界",
        "Dialogue: 2,0:00:19.94,0:00:22.62,Alternate,pick-one,21,22,23,fx-one,{\\i1}first, selected",
        "Dialogue: 3,0:00:29.91,0:00:32.78,Third,gap,31,32,33,fx-gap,unselected gap 😀",
        "Dialogue: 4,0:00:39.94,0:00:42.94,Default,pick-two,41,42,43,fx-two,{\\bord3}second selected",
        "Dialogue: 5,596:31:19.91,596:31:22.64,Third,late,51,52,53,fx-late,last overflow sentinel",
    ];
    static readonly string[] FinalEvents =
    [
        "Dialogue: 0,0:00:00.00,0:00:00.50,Default,early,1,2,3,fx-zero,zero clamp",
        "Comment: 9,0:00:09.94,0:00:10.54,Alternate,pick-comment,91,92,93,sentinel,{\\b1}comment café 世界",
        "Dialogue: 2,0:00:19.94,0:00:22.62,Alternate,pick-one,21,22,23,fx-one,{\\i1}first, selected",
        "Dialogue: 3,0:00:29.91,0:00:32.78,Third,gap,31,32,33,fx-gap,unselected gap 😀",
        "Dialogue: 4,0:00:39.94,0:00:42.94,Default,pick-two,41,42,43,fx-two,{\\bord3}second selected",
        "Dialogue: 5,596:31:19.91,596:31:22.62,Third,late,51,52,53,fx-late,last overflow sentinel",
    ];
    readonly string artifacts = Path.GetFullPath(args[1]);
    readonly List<object> checks = [];
    Process? app;
    nint main, dialog, timeEdit, frameEdit, historyList;
    string fixture = "", profile = "", phase = "";
    string[] baseline = [];

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
            phase = "setup";
            Command("Video", "Open Timecodes File...");
            FileDialog("Open Timecodes File", Path.Combine(artifacts, "host", "25fps.txt"));
            SelectLines();
            baseline = SaveAs("baseline");
            Require(baseline.Where(IsEvent).SequenceEqual(SeedEvents), "Baseline contains all six independently specified event lines in file order, including comment and late boundary row");
            VerifyRawEndpoints(false);
            phase = "zero-time"; Open();
            SetShift(true, "0:00:00.00", false, "All rows", "Start and End times");
            Capture("zero-time"); CloseDialog("OK");
            Require(!Text(main).StartsWith('*'), "Zero time closes without a subtitle commit");
            Require(File.ReadAllBytes(HistoryPath()).SequenceEqual(Encoding.UTF8.GetBytes(InitialHistory)), "Zero time leaves the entire history file byte-for-byte unchanged");
            AssertAss("after-zero-time", SeedEvents);

            phase = "late-overflow-cancel"; Open();
            SetShift(true, "0:00:02.00", false, "All rows", "Start and End times");
            RejectOverflow("0:00:02.00", Encoding.UTF8.GetBytes(InitialHistory));
            CloseDialog("Cancel");
            Require(!Text(main).StartsWith('*'), "Cancel after the last-row overflow leaves the original document clean");
            AssertAss("after-late-cancel", SeedEvents);
            VerifyRawEndpoints(false);

            phase = "large-time-retry"; Open();
            SetShift(true, "596:31:23.00", false, "All rows", "Start and End times");
            RejectOverflow("596:31:23.00", Encoding.UTF8.GetBytes(InitialHistory));
            SetShift(true, "0:00:00.10", true, "All rows", "Start times only");
            Capture("large-time-corrected"); CloseDialog("OK");
            Require(Text(main).StartsWith('*'), "Correcting the same time dialog publishes the valid shift");
            VerifyHistory(TimeHistory, "after-time-retry");
            AssertAss("after-time-retry", TimeEvents);
            VerifyRawEndpoints(true);

            phase = "large-frame-retry"; Open();
            SetShift(false, "60000000", false, "Selected rows", "Start and End times");
            var historyBeforeFrame = File.ReadAllBytes(HistoryPath());
            RejectOverflow("60000000", historyBeforeFrame);
            SetShift(false, "1", false, "Selected rows", "Start and End times");
            Capture("large-frame-corrected"); CloseDialog("OK");
            Require(Text(main).StartsWith('*'), "Correcting the same frame dialog publishes the valid shift");
            VerifyHistory(FrameHistory, "after-frame-retry");
            AssertAss("after-frame-retry", FrameEvents);

            phase = "zero-frame-onward"; Open();
            SetShift(false, "0", false, "Selection onward", "End times only");
            Capture("zero-frame-onward"); CloseDialog("OK");
            Require(Text(main).StartsWith('*'), "Zero frames still maps frame boundaries and commits the result");
            VerifyHistory(FinalHistory, "after-zero-frame");
            AssertAss("after-zero-frame", FinalEvents);
            ScreenCapture.SaveWindowPng(main, Path.Combine(artifacts, "saved-result.png"));
            NormalExit();
            VerifyHistory(FinalHistory, "after-normal-exit");
            File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { passed = true, checks }, Json));
            return 0;
        }
        catch (Exception error)
        {
            if (app is not null && !app.HasExited)
                try { Dump("failure"); ScreenCapture.SaveWindowPng(dialog != 0 ? dialog : main, Path.Combine(artifacts, "failure.png")); }
                catch (Exception inventoryError) { Console.Error.WriteLine(inventoryError); }
            File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { passed = false, checks, error = error.ToString() }, Json));
            Console.Error.WriteLine(error); return 1;
        }
        finally { if (app is not null) { isolation.Stop(app); app.Dispose(); } }
    }

    static bool IsEvent(string line) => line.StartsWith("Dialogue:") || line.StartsWith("Comment:");
    string HistoryPath() => Path.Combine(profile, "user", "shift_history.json");
    void AssertAss(string name, string[] events)
    {
        int index = 0;
        var expected = baseline.Select(line => IsEvent(line) ? events[index++] : line).ToArray();
        Require(index == events.Length, "Full-file oracle contains exactly the six literal event rows");
        File.WriteAllLines(Path.Combine(artifacts, name + "-expected.ass"), expected, new UTF8Encoding(true));
        Require(SaveAs(name).SequenceEqual(expected), "Complete saved ASS matches the independent literal events and every unchanged header, style and project property: " + name);
    }
    void VerifyHistory(string expected, string name)
    {
        File.WriteAllText(Path.Combine(artifacts, name + "-expected-history.json"), expected);
        var actual = File.ReadAllText(HistoryPath());
        File.WriteAllText(Path.Combine(artifacts, name + "-history.json"), actual);
        Require(JsonNode.DeepEquals(JsonNode.Parse(actual), JsonNode.Parse(expected)), "Entire history matches the independent literal array, including original discontiguous one-based inclusive blocks: " + name);
    }
    void Check(string label, bool value = true)
    {
        var control = Control(label);
        Require(Native.IsWindowEnabled(control), "Observed native choice is enabled: " + label);
        if (Send(control, 0xF0) != (value ? 1 : 0))
        {
            Action("native-choice", label + "=" + value); Native.Post(control, 0xF5, 0, 0);
            Wait(() => Send(control, 0xF0) == (value ? 1 : 0), "Native choice updated: " + label);
        }
    }
    void Open()
    {
        Command("Timing", "Shift Times...");
        Wait(() => { dialog = Windows().SingleOrDefault(h => Text(h) == "Shift Times"); return dialog != 0 && Native.IsWindowEnabled(dialog); }, "Actual Shift Times opened");
        var children = Children(dialog);
        var edits = children.Where(h => PrivateDesktop.ClassName(h) == "Edit").ToArray();
        Require(edits.Length == 2, "Exactly two native Shift Times amount Edit controls observed");
        nint AmountBeside(string label)
        {
            var anchor = Native.Bounds(Control(label));
            return edits.Single(h => { var b = Native.Bounds(h); return b.Left >= anchor.Right && Math.Abs(b.CenterY - anchor.CenterY) <= 12; });
        }
        timeEdit = AmountBeside("Time:"); frameEdit = AmountBeside("Frames:");
        Require(timeEdit != frameEdit, "Amount controls independently identified by adjacent Time/Frames labels and native bounds");
        historyList = children.Single(h => PrivateDesktop.ClassName(h) == "ListBox");
        Require(Native.IsWindowEnabled(Control("Frames:")), "Actual loaded 25fps timecodes enable frame shifting");
        Dump(phase + "-controls");
    }
    void SetShift(bool byTime, string value, bool backward, string scope, string fields)
    {
        Check(byTime ? "Time:" : "Frames:"); Check(backward ? "Backward" : "Forward"); Check(scope); Check(fields);
        var edit = byTime ? timeEdit : frameEdit;
        Require(Native.IsWindowEnabled(edit), "Chosen amount control is enabled");
        isolation.VerifyWindow(edit, app!); Action("native-amount", value); Native.SetText(edit, value);
        Require(Text(edit) == value, "Actual native amount reads back exactly: " + value);
    }
    void SelectLines()
    {
        Command("Subtitle", "Select Lines...");
        Wait(() => { dialog = Windows().SingleOrDefault(h => Text(h) == "Select"); return dialog != 0; }, "Native Select Lines opened");
        foreach (var label in new[] { "Matches", "Contains", "Actor", "Set selection", "Dialogues", "Comments" }) Check(label);
        var edit = Children(dialog).Single(h => PrivateDesktop.ClassName(h) == "Edit");
        isolation.VerifyWindow(edit, app!); Native.SetText(edit, "pick-");
        Require(Text(edit) == "pick-", "Native actor selection targets comment and dialogue rows 2-3 and 5");
        Capture("selection-blocks"); CloseDialog("OK");
    }
    void VerifyRawEndpoints(bool shifted)
    {
        var exact = Children(main).Single(h => PrivateDesktop.ClassName(h) == "Button" && Text(h).Replace("&", "") == "Exact");
        if (Send(exact, 0xF0) != 1) Native.Post(exact, 0xF5, 0, 0);
        Wait(() => Send(exact, 0xF0) == 1, "Main editor Exact mode enabled through its actual native radio button");
        var edits = Children(main).Where(h => PrivateDesktop.ClassName(h) == "Edit").Select(Text).ToArray();
        var candidates = new[] {
            (Text: "{\\b1}comment café 世界", Start: shifted ? "0:00:09.910" : "0:00:10.006", End: "0:00:10.509"),
            (Text: "{\\i1}first, selected", Start: shifted ? "0:00:19.900" : "0:00:20.003", End: "0:00:22.568"),
            (Text: "{\\bord3}second selected", Start: shifted ? "0:00:39.910" : "0:00:40.009", End: "0:00:42.903")
        };
        var active = candidates.Single(candidate => edits.Contains(candidate.Text));
        Require(edits.Contains(active.Start) && edits.Contains(active.End), "Actual Exact editor proves active line centisecond input conversion and untouched raw-millisecond End: " + active.Text);
        File.WriteAllText(Path.Combine(artifacts, phase + "-exact-editor.json"), JsonSerializer.Serialize(new { shifted, active, edit_values = edits }, new JsonSerializerOptions { WriteIndented = true, IncludeFields = true }));
        ScreenCapture.SaveWindowPng(main, Path.Combine(artifacts, phase + "-exact-editor.png"));
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
    nint Control(string label) => Children(dialog).Single(h => Text(h).Replace("&", "").Trim() == label);
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

    void RejectOverflow(string amount, byte[] historyBefore)
    {
        var shiftDialog = dialog;
        var input = Native.IsWindowEnabled(timeEdit) ? timeEdit : frameEdit;
        var controlsBefore = Children(dialog).Where(h => PrivateDesktop.ClassName(h) == "Button")
            .Select(h => new { handle = h, text = Text(h), state = Send(h, 0xF0) }).ToArray();
        var historyCount = Send(historyList, 0x18B);
        Capture(phase + "-input");
        Action("button", "OK/overflow"); ObservedWindowsCommand.PostButtonClick(Control("OK"), app!.Id);
        nint warning = 0;
        Wait(() => {
            warning = Windows().SingleOrDefault(h => h != shiftDialog && PrivateDesktop.ClassName(h) == "#32770"
                && Native.GetWindow(h, 4) == shiftDialog && Text(h) == "Shift Times");
            return warning != 0;
        }, "Owned Shift Times overflow TaskDialog appeared");
        isolation.VerifyWindow(warning, app!); isolation.VerifyWindow(shiftDialog, app!);
        var elements = AutomationElement.FromHandle(warning).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
        var accessible = elements.Select(e => new { name = e.Current.Name, control_type = e.Current.ControlType.ProgrammaticName,
            automation_id = e.Current.AutomationId, handle = e.Current.NativeWindowHandle }).ToArray();
        File.WriteAllText(Path.Combine(artifacts, phase + "-error-accessibility.json"), JsonSerializer.Serialize(accessible, Json));
        Dump(phase + "-error-controls"); ScreenCapture.SaveWindowPng(warning, Path.Combine(artifacts, phase + "-error.png"));
        Require(Native.GetWindow(warning, 4) == shiftDialog && Native.GetWindow(shiftDialog, 4) == main,
            "Actual error owner chain is TaskDialog -> unchanged Shift Times -> owned main window");
        Require(Children(warning).Any(h => PrivateDesktop.ClassName(h) == "DirectUIHWND"), "Actual wx TaskDialog contains native DirectUI content");
        Require(accessible.Any(e => e.control_type == "ControlType.Text"
            && e.name == "The shifted time is outside the supported range. No subtitles were changed."), "Actual UIA Text contains the exact supported-range rejection explanation");
        var accessibleOk = elements.Single(e => e.Current.ControlType == ControlType.Button
            && e.Current.Name.Replace("&", "") == "OK" && e.Current.NativeWindowHandle != 0);
        var ok = (nint)accessibleOk.Current.NativeWindowHandle;
        isolation.VerifyWindow(ok, app!); Native.GetWindowThreadProcessId(ok, out var okProcessId);
        Require(Native.IsChild(warning, ok) && okProcessId == app!.Id, "Unique UIA OK button is an actual native child in the owned process");
        Require(elements.Count(e => e.Current.ControlType == ControlType.Button && e.Current.NativeWindowHandle != 0) == 1
            && accessibleOk.Current.AutomationId == "CommandButton_2", "Only-OK wx TaskDialog exposes the observed CommandButton_2 IDCANCEL mapping");
        var logicalId = int.Parse(accessibleOk.Current.AutomationId["CommandButton_".Length..], CultureInfo.InvariantCulture);
        File.WriteAllText(Path.Combine(artifacts, phase + "-error-dismissal.json"), JsonSerializer.Serialize(new {
            popup = warning.ToString("X"), popup_owner = Native.GetWindow(warning, 4).ToString("X"),
            shift_dialog = shiftDialog.ToString("X"), shift_owner = Native.GetWindow(shiftDialog, 4).ToString("X"), main = main.ToString("X"),
            process_id = app!.Id, ok = ok.ToString("X"), ok_process_id = okProcessId, actual_child = Native.IsChild(warning, ok),
            uia_automation_id = accessibleOk.Current.AutomationId, logical_button_id = logicalId,
            native_control_id = Native.GetDlgCtrlID(ok), dismissal_message = "TDM_CLICK_BUTTON",
            invoke_pattern_supported = accessibleOk.TryGetCurrentPattern(InvokePattern.Pattern, out _),
            source_protocol = "wx3.3.1 src/msw/msgdlg.cpp:725-739 only-wxOK adds IDCANCEL as OK; :540-547 maps IDCANCEL to wxID_OK"
        }, Json));
        Action("task-dialog-button", "Shift Times/observed CommandButton_2"); Send(warning, 0x466, logicalId);
        Wait(() => !Windows().Contains(warning) && Windows().Contains(shiftDialog) && Native.IsWindowEnabled(shiftDialog), "Error dismissal restores the same Shift Times dialog without closing it");
        Require(dialog == shiftDialog && Text(input) == amount, "Rejected operation preserves original dialog HWND and exact amount text");
        Require(controlsBefore.All(c => Text(c.handle) == c.text && Send(c.handle, 0xF0) == c.state), "Every original direction, scope and endpoint choice is retained after rejection");
        Require(!Text(main).StartsWith('*'), "Overflow does not mark the actual subtitle document dirty");
        Require(File.ReadAllBytes(HistoryPath()).SequenceEqual(historyBefore) && Send(historyList, 0x18B) == historyCount, "Overflow leaves history bytes and the native history list unchanged");
        File.WriteAllBytes(Path.Combine(artifacts, phase + "-history-after-rejection.json"), File.ReadAllBytes(HistoryPath()));
        Capture(phase + "-retained");
    }
    void FileDialog(string title, string path)
    {
        nint picker = 0;
        Wait(() => { picker = Windows().SingleOrDefault(h => PrivateDesktop.ClassName(h) == "#32770" && Text(h) == title); return picker != 0; }, "Native file dialog opened: " + title);
        Dump("timecodes-picker-controls"); ScreenCapture.SaveWindowPng(picker, Path.Combine(artifacts, "timecodes-picker.png"));
        var children = Children(picker);
        var label = children.Single(h => PrivateDesktop.ClassName(h) == "Static" && Native.GetDlgCtrlID(h) == 1090);
        var combo = children.Single(h => PrivateDesktop.ClassName(h) == "ComboBoxEx32" && Native.GetDlgCtrlID(h) == 1148);
        var edit = Children(combo).Single(h => PrivateDesktop.ClassName(h) == "Edit" && Native.GetDlgCtrlID(h) == 1148);
        var labelBounds = Native.Bounds(label); var editBounds = Native.Bounds(edit);
        Require(Text(label).Length > 0 && editBounds.Left >= labelBounds.Right && Math.Abs(editBounds.CenterY - labelBounds.CenterY) <= 12
            && Native.IsChild(picker, combo) && Native.IsChild(combo, edit), "Observed Open picker filename Edit belongs to its ComboBoxEx and sits beside the actual filename label");
        isolation.VerifyWindow(edit, app!); Native.SetText(edit, path);
        Require(Text(edit) == path, "Actual timecodes filename Edit reads back the generated fixture path");
        var button = children.Single(h => PrivateDesktop.ClassName(h) == "Button" && Native.GetDlgCtrlID(h) == 1);
        isolation.VerifyWindow(button, app!);
        Require(Native.IsChild(picker, button) && Native.IsWindowEnabled(button), "Observed Open command is an enabled native button inside the owned picker");
        ObservedWindowsCommand.PostButtonClick(button, app!.Id);
        Wait(() => !Windows().Contains(picker), "Native file dialog accepted: " + title);
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
            {"Subtitle":{"Use STC":false,"Time Edit":{"Insert Mode":false}},"App":{"First Start":false,"Language":"en_US","Auto":{"Backup":false,"Check For Updates":false,"Load Linked Files":1,"Save":false,"Save on Every Change":false}},"Tool":{"Shift Times":{"Time":0,"Frames":0,"ByTime":true,"Direction":false,"Affect":0,"Type":0}}}
            """)!;
        File.WriteAllText(Path.Combine(profile, "user", "config.json"), config.ToJsonString(Json));
        File.WriteAllText(HistoryPath(), InitialHistory);
        File.WriteAllText(Path.Combine(host, "25fps.txt"), "# timecode format v1\nAssume 25\n");
        fixture = Path.Combine(host, "fixture.ass");
        var header = """
            [Script Info]
            Title: Native Shift Overflow E2E
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
        var events = """
            Dialogue: 0,0:00:00.050,0:00:00.500,Default,early,1,2,3,fx-zero,zero clamp
            Comment: 9,0:00:10.006,0:00:10.509,Alternate,pick-comment,91,92,93,sentinel,{\b1}comment café 世界
            Dialogue: 2,0:00:20.003,0:00:22.568,Alternate,pick-one,21,22,23,fx-one,{\i1}first, selected
            Dialogue: 3,0:00:30.007,0:00:32.784,Third,gap,31,32,33,fx-gap,unselected gap 😀
            Dialogue: 4,0:00:40.009,0:00:42.903,Default,pick-two,41,42,43,fx-two,{\bord3}second selected
            Dialogue: 5,596:31:20.009,596:31:22.637,Third,late,51,52,53,fx-late,last overflow sentinel
            """;
        File.WriteAllText(fixture, header + "\n" + string.Join('\n', styles) + "\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n" + events + "\n", new UTF8Encoding(true));
    }
    void Start()
    {
        var host = Path.Combine(artifacts, "launch"); Directory.CreateDirectory(host);
        var start = new ProcessStartInfo(Path.GetFullPath(args[0])) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", host, "--open", fixture }) start.ArgumentList.Add(argument);
        app = isolation.Start(start, "shift-overflow");
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(host, "ready.json"), app, TimeSpan.FromSeconds(20));
        main = isolation.MainWindow(app, 10);
    }
}

static class Native
{
    [DllImport("user32.dll")] public static extern nint GetWindow(nint window, uint command);
    [DllImport("user32.dll")] public static extern bool IsChild(nint parent, nint child);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(nint window, out uint process);
    delegate bool Callback(nint window, nint data);
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int Left, Top, Right, Bottom; }
    public sealed record Rectangle(int Left, int Top, int Right, int Bottom)
    {
        public int CenterY => (Top + Bottom) / 2;
    }
    [DllImport("user32.dll")] static extern bool GetWindowRect(nint window, out Rect rect);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(nint parent, Callback callback, nint data);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(nint window);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(nint window);
    [DllImport("user32.dll", EntryPoint = "PostMessageW")] static extern bool PostMessage(nint window, uint message, nint wp, nint lp);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")] static extern nint SendMessage(nint window, uint message, nint wp, nint lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint SendText(nint window, uint message, nint wp, string lp, uint flags, uint timeout, out nint result);
    public static Rectangle Bounds(nint window) { if (!GetWindowRect(window, out var rect)) throw new InvalidOperationException("Native bounds unavailable"); return new(rect.Left, rect.Top, rect.Right, rect.Bottom); }
    public static nint[] Children(nint parent) { List<nint> found = []; Callback callback = (h, _) => { found.Add(h); return true; }; EnumChildWindows(parent, callback, 0); GC.KeepAlive(callback); return found.ToArray(); }
    public static long Send(nint window, uint message, long wp, long lp) { if (SendMessage(window, message, (nint)wp, (nint)lp, 0x22, 1000, out var result) == 0) throw new TimeoutException("Native control message"); return result.ToInt64(); }
    public static void SetText(nint window, string value) { if (SendText(window, 0xC, 0, value, 0x22, 1000, out _) == 0) throw new TimeoutException("Native text entry"); }
    public static void Post(nint window, uint message, long wp, long lp) { if (!PostMessage(window, message, (nint)wp, (nint)lp)) throw new InvalidOperationException("Native message post failed"); }
}
