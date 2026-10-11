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
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

if (args.Length != 2)
{
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/automation-neutral-native.cs -- EXE ARTIFACTS");
    return 2;
}
try { return PrivateDesktop.Run(args, Path.GetFullPath(args[1]), desktop => new Scenario(args, desktop).Run()); }
catch (Exception error) { Console.Error.WriteLine(error); return 1; }

sealed class Scenario(string[] args, PrivateDesktop isolation)
{
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    static readonly string[] CaseNames = ["ValidationAndToggle", "DialogAcceptAndSelection", "DialogCancelRollsBack", "ProgressCancelRollsBack", "RuntimeFailureRollsBack", "UndoAndSave", "ExportConfigurationAndExecution"];
    readonly string artifacts = Path.GetFullPath(args[1]);
    readonly List<CheckResult> checks = [];
    readonly HashSet<string> completed = [];
    readonly object resultLock = new();
    Process? app;
    nint main;
    string phase = "setup", documents = "", profile = "";
    string[] baseline = [], accepted = [], exported = [];

    public int Run()
    {
        Directory.CreateDirectory(artifacts);
        isolation.RecordDriver();
        using var log = new StreamWriter(Path.Combine(artifacts, "driver.log")) { AutoFlush = true };
        Console.SetOut(log); Console.SetError(log);
        using var watchdog = new Timer(_ => {
            try { WriteResult(false, "180-second watchdog expired"); }
            finally { try { if (app is not null) isolation.Stop(app); } finally { Environment.Exit(124); } }
        }, null, 180_000, Timeout.Infinite);
        try
        {
            Prepare(); Start();
            Case(CaseNames[0], () => {
                AssertEvents(SaveAs("baseline.ass"), baseline, "Baseline save preserves all four complete events");
                AssertValidation(false, "baseline-validation");
                Require(!UndoItem().Enabled, "Freshly loaded fixture has no undo history");
            });
            Case(CaseNames[1], () => {
                Command("Automation", "Native Neutral Accept");
                var dialog = DialogWithText("Native mixed-control contract");
                SetEdit(dialog, "prefix-seed", "Accepted café, 世界");
                SetEdit(dialog, "detail-seed", "line one\r\nline two");
                SetEdit(dialog, "137", "250");
                SetEdit(dialog, "1.25", "1.75");
                SetEdit(dialog, "#3F", "#7F");
                SetChecked(dialog, "Use bold layer", true);
                Capture(dialog, "accepted-controls");
                Click(dialog, "Apply changes"); FinishDialog(dialog);
                Require(Text(main).StartsWith('*'), "Accepted Lua edits mark the subtitle document dirty");
                AssertEvents(SaveAs("accepted.ass"), accepted, "Accepted typed values produce the exact expected event fields");
                Require(File.ReadAllLines(Path.Combine(documents, "accepted.ass")).Contains("Active Line: 2"), "Saved UI state identifies the third event as the active row");
                AssertValidation(true, "accepted-validation");
                VerifyAcceptedSelection("accepted-selection");
                Require(UndoItem() is { Enabled: true, Label: "Undo Native neutral accept" }, "Accepted macro publishes its named undo point");
            });
            Case(CaseNames[2], () => {
                var undo = UndoItem();
                Command("Automation", "Native Neutral Dialog Cancel");
                var dialog = DialogWithText("Cancel staged subtitle and undo changes");
                SetEdit(dialog, "cancel-seed", "discarded dialog input");
                Capture(dialog, "cancel-controls");
                Click(dialog, "Cancel"); FinishDialog(dialog);
                AssertRollback("dialog-cancel", undo);
            });
            Case(CaseNames[3], () => {
                var undo = UndoItem();
                Command("Automation", "Native Neutral Progress Cancel");
                var progress = DialogWithText("Staged changes are waiting for explicit cancellation");
                Require(!Native.IsWindowEnabled(main), "Real running progress owns the modal invocation");
                Capture(progress, "progress-before-cancel");
                Click(progress, "Cancel"); FinishDialog(progress);
                AssertRollback("progress-cancel", undo);
            });
            Case(CaseNames[4], () => {
                var undo = UndoItem();
                Command("Automation", "Native Neutral Runtime Failure");
                nint progress = 0;
                Wait(() => {
                    progress = Windows().SingleOrDefault(h => Text(h) == "Native Neutral Runtime Failure" && HasButton(h, "Close"));
                    return progress != 0;
                }, "Failed Lua invocation leaves its completed error log open");
                var messages = Children(progress).Select(Text).Where(value => value.Length > 0).ToArray();
                Write("runtime-failure-log.json", new { title = Text(progress), messages });
                Require(messages.Any(value => value.Contains("native-neutral-runtime-failure", StringComparison.Ordinal)), "Real runtime failure diagnostic contains the fixture error");
                Capture(progress, "runtime-failure-log");
                Click(progress, "Close"); FinishDialog(progress, closingProgressLog: true);
                AssertRollback("runtime-failure", undo);
            });
            Case(CaseNames[5], () => {
                var undo = UndoItem();
                Require(undo is { Enabled: true, Label: "Undo Native neutral accept" }, "Cancelled and failed invocations left the accepted macro at the top of history");
                Command("Edit", undo.Label);
                Wait(() => Native.IsWindowEnabled(main) && !UndoItem().Enabled, "Exactly one Undo consumes the accepted macro history entry");
                AssertEvents(SaveAs("undone.ass"), baseline, "Undo restores every original subtitle event field and event order");
                AssertValidation(false, "undone-validation");
                Command("Automation", "Native Neutral Verify Baseline");
                Acknowledge("Native baseline Lua inspection completed", "baseline-lua-verification");
            });
            Case(CaseNames[6], Export);
            Native.Post(main, 0x10, 0, 0);
            Require(app!.WaitForExit(10_000), "Host closes normally within ten seconds");
            Require(isolation.ExitCode(app) == 0, "Normal host exit code is zero");
            WriteResult(true, null);
            using var result = JsonDocument.Parse(File.ReadAllText(Path.Combine(artifacts, "result.json")));
            var cases = result.RootElement.GetProperty("cases").EnumerateArray().ToArray();
            if (!result.RootElement.GetProperty("passed").GetBoolean()
                || !cases.Select(item => item.GetProperty("name").GetString()).SequenceEqual(CaseNames)
                || cases.Any(item => item.GetProperty("status").GetString() != "passed" || item.GetProperty("checks").GetInt32() == 0))
                throw new InvalidOperationException("Result report has missing, empty or unsuccessful cases");
            return 0;
        }
        catch (Exception error)
        {
            if (app is { HasExited: false })
                try { Dump(); } catch (Exception captureError) { Console.Error.WriteLine(captureError); }
            WriteResult(false, error.ToString()); Console.Error.WriteLine(error); return 1;
        }
        finally { if (app is not null) { isolation.Stop(app); app.Dispose(); } }
    }

    void Case(string name, Action action)
    {
        phase = name; action();
        lock (resultLock) completed.Add(name);
    }
    void Write(string name, object value) => File.WriteAllText(Path.Combine(artifacts, name), JsonSerializer.Serialize(value, Json));
    void WriteResult(bool passed, string? error)
    {
        lock (resultLock)
            Write("result.json", new {
                passed, error,
                cases = CaseNames.Select(name => new { name, status = checks.Any(item => item.Name == name && !item.Passed) ? "failed" : completed.Contains(name) ? "passed" : "incomplete", checks = checks.Count(item => item.Name == name) }),
                checks,
                limitations = new[] {
                    "Workspace active-session and replacement guards require the separate neutral runtime E2E; this driver does not equate modal disabling with those guards.",
                    "Export settings are checked through same-document dialog reconstruction and re-export; ASS reopen persistence is not claimed.",
                    "Dropdown and color controls retain their nontrivial defaults; native color-picker interaction is not exercised."
                }
            });
    }
    void Require(bool condition, string description)
    {
        lock (resultLock) checks.Add(new(phase, description, condition));
        Console.WriteLine(phase + ": " + description + ": " + condition);
        if (!condition) throw new InvalidOperationException(description);
    }
    void Wait(Func<bool> predicate, string description)
    {
        var timer = Stopwatch.StartNew();
        while (timer.ElapsedMilliseconds < 10_000)
        {
            isolation.VerifyCurrent();
            if (app is { HasExited: true }) throw new InvalidOperationException("Host exited: " + description);
            if (predicate()) return;
            Thread.Sleep(40);
        }
        throw new TimeoutException(description);
    }
    string Text(nint window) => isolation.ReadText(window, app!);
    nint[] Windows() => isolation.Windows(app!).Where(Native.IsWindowVisible).ToArray();
    nint[] Children(nint window) => Native.Children(window).Where(Native.IsWindowVisible).ToArray();
    void Action(string action, string target) => File.AppendAllText(Path.Combine(artifacts, "actions.ndjson"), JsonSerializer.Serialize(new { phase, action, target }) + Environment.NewLine);
    Native.MenuItem[] Menu(string menu)
    {
        isolation.VerifyWindow(main, app!);
        return Native.ReadMenu(main, menu);
    }
    void Command(string menu, string label)
    {
        var item = Menu(menu).Single(item => item.Label == label);
        if (!item.Enabled) throw new InvalidOperationException("Observed menu command is disabled: " + label);
        Action("menu", menu + "/" + label);
        ObservedWindowsCommand.PostMenuCommand(main, app!.Id, item.Id);
    }
    Native.MenuItem UndoItem() => Menu("Edit").Single(item => item.Label == "Nothing to undo" || item.Label.StartsWith("Undo ", StringComparison.Ordinal));
    void AssertValidation(bool enabled, string label)
    {
        var items = Menu("Automation");
        var validate = items.Single(item => item.Label == "Native Neutral Validate");
        var toggle = items.Single(item => item.Label == "Native Neutral Toggle");
        Require(validate.Enabled == enabled, "Validation publishes the expected enabled state: " + enabled);
        Require(toggle.Enabled && toggle.Checked == enabled, "Lua is-active publishes the expected toggle state: " + enabled);
        var expectedHelp = enabled ? "Native accepted validation help" : "Native baseline validation help";
        isolation.VerifyWindow(main, app!);
        Native.Highlight(main, "Automation", validate.Id);
        var status = Children(main).Single(window => PrivateDesktop.ClassName(window) == "msctls_statusbar32");
        isolation.VerifyWindow(status, app!);
        var names = AutomationElement.FromHandle(status).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().Select(element => element.Current.Name).ToArray();
        Write(label + ".json", new { items, expected_help = expectedHelp, status_text = names });
        Require(names.Contains(expectedHelp), "Validated dynamic help reaches the native status bar");
        Native.CloseMenu(main, "Automation");
    }
    bool HasButton(nint parent, string label) => Children(parent).Any(h => PrivateDesktop.ClassName(h) == "Button" && Text(h).Replace("&", "", StringComparison.Ordinal) == label);
    void Click(nint parent, string label)
    {
        var button = Children(parent).Single(h => PrivateDesktop.ClassName(h) == "Button" && Text(h).Replace("&", "", StringComparison.Ordinal) == label);
        isolation.VerifyWindow(button, app!);
        if (!Native.IsWindowEnabled(button)) throw new InvalidOperationException("Observed button is disabled: " + label);
        Action("button", label); Native.Post(button, 0xF5, 0, 0);
    }
    void SetEdit(nint parent, string oldValue, string newValue)
    {
        var edit = Children(parent).Single(h => PrivateDesktop.ClassName(h) == "Edit" && Text(h) == oldValue);
        isolation.VerifyWindow(edit, app!); Native.SetText(edit, newValue);
        Require(Text(edit) == newValue, "Observed native edit accepts replacement of " + oldValue);
        Action("edit", oldValue + " -> " + newValue);
    }
    void SetChecked(nint parent, string label, bool value)
    {
        var checkbox = Children(parent).Single(h => PrivateDesktop.ClassName(h) == "Button" && Text(h).Replace("&", "", StringComparison.Ordinal) == label);
        isolation.VerifyWindow(checkbox, app!);
        if ((Native.Send(checkbox, 0xF0, 0, 0) != 0) != value) Click(parent, label);
        Wait(() => (Native.Send(checkbox, 0xF0, 0, 0) != 0) == value, "Checkbox reaches requested state: " + label);
        Require((Native.Send(checkbox, 0xF0, 0, 0) != 0) == value, "Observed checkbox state: " + label + " = " + value);
    }
    nint DialogWithText(string marker)
    {
        nint dialog = 0;
        Wait(() => {
            dialog = Windows().SingleOrDefault(h => h != main && PrivateDesktop.ClassName(h) == "#32770" && Children(h).Any(child => Text(child) == marker));
            return dialog != 0;
        }, "Actual Lua dialog exposes marker: " + marker);
        return dialog;
    }
    void FinishDialog(nint dialog, bool closingProgressLog = false)
    {
        Wait(() => {
            FailOnUnexpectedProgressLog(closingProgressLog ? dialog : 0);
            return !Windows().Contains(dialog);
        }, "Observed dialog closes");
        Wait(() => {
            FailOnUnexpectedProgressLog(0);
            return Native.IsWindowEnabled(main);
        }, "Macro finishes and re-enables the host");
    }
    void FailOnUnexpectedProgressLog(nint ignored)
    {
        foreach (var window in Windows().Where(h => h != ignored && h != main))
        {
            if (!TryReadCompletedProgressLog(window, out var title, out var messages)) continue;
            Write("unexpected-progress-log.json", new { phase, title, messages });
            Capture(window, "unexpected-progress-log");
            Require(false, "Unexpected completed Lua progress log in " + title + ": " + string.Join(Environment.NewLine, messages));
        }
    }
    bool TryReadCompletedProgressLog(nint window, out string title, out string[] messages)
    {
        title = ""; messages = [];
        var observed = window;
        try
        {
            if (PrivateDesktop.ClassName(window) != "#32770") return false;
            var close = false; var gauge = false; var log = new List<string>();
            foreach (var child in Children(window))
            {
                observed = child;
                var kind = PrivateDesktop.ClassName(child);
                if (kind == "Button") close |= Text(child).Replace("&", "", StringComparison.Ordinal) == "Close";
                else if (kind == "msctls_progress32") gauge = true;
                else if (kind == "Edit")
                {
                    var value = Text(child);
                    if (value.Length > 0) log.Add(value);
                }
            }
            if (!close || !gauge || log.Count == 0) return false;
            observed = window;
            title = Text(window); messages = log.ToArray();
            return true;
        }
        catch (Win32Exception error) when (error.NativeErrorCode == 1400 && !Native.IsWindow(observed) && app is { HasExited: false })
        {
            Action("observation-vanished", observed.ToString("X") + "/1400");
            return false;
        }
    }
    void Acknowledge(string marker, string label)
    {
        var dialog = DialogWithText(marker);
        Capture(dialog, label); Require(Children(dialog).Any(child => Text(child) == marker), "Lua inspection reached its success acknowledgement: " + marker);
        Click(dialog, "Acknowledge"); FinishDialog(dialog);
    }
    void VerifyAcceptedSelection(string label)
    {
        Command("Automation", "Native Neutral Validate");
        Acknowledge("Native validation and selection verified: rows 1,3; active 3", label);
    }
    void AssertRollback(string label, Native.MenuItem undo)
    {
        Require(!Text(main).StartsWith('*'), "Discarded staging leaves the saved document clean");
        var after = UndoItem();
        Require(after.Label == undo.Label && after.Enabled == undo.Enabled, "Discarded staging leaves the exact prior undo entry");
        AssertEvents(SaveAs(label + ".ass"), accepted, "Discarded staging preserves every accepted event field and event order");
        VerifyAcceptedSelection(label + "-selection");
    }
    void AssertEvents(string path, string[] expected, string description)
    {
        var actual = Events(File.ReadAllLines(path));
        Write(Path.GetFileNameWithoutExtension(path) + "-events.json", new { expected, actual });
        Require(actual.SequenceEqual(expected), description);
    }
    nint FileDialog(string title)
    {
        nint result = 0;
        Wait(() => { result = Windows().SingleOrDefault(h => PrivateDesktop.ClassName(h) == "#32770" && Text(h) == title); return result != 0; }, "Native file dialog opened: " + title);
        return result;
    }
    void FillFileDialog(nint picker, string path)
    {
        isolation.VerifyWindow(picker, app!);
        var elements = AutomationElement.FromHandle(picker).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
        var edit = elements.Single(e => e.Current.ControlType == ControlType.Edit && e.Current.AutomationId is "1001" or "1148");
        var handle = (nint)edit.Current.NativeWindowHandle; isolation.VerifyWindow(handle, app!); Native.SetText(handle, path);
        var button = elements.Single(e => e.Current.ControlType == ControlType.Button && e.Current.AutomationId == "1" && e.Current.NativeWindowHandle != 0);
        handle = (nint)button.Current.NativeWindowHandle; isolation.VerifyWindow(handle, app!);
        Action("file-dialog-path", Path.GetRelativePath(artifacts, path)); ObservedWindowsCommand.PostButtonClick(handle, app!.Id);
    }
    string SaveAs(string name)
    {
        var path = Path.Combine(documents, name);
        Command("File", "Save Subtitles as...");
        nint picker = 0;
        Wait(() => { picker = Windows().SingleOrDefault(h => PrivateDesktop.ClassName(h) == "#32770" && Text(h).Contains("Save", StringComparison.Ordinal)); return picker != 0; }, "Native Save As opened");
        FillFileDialog(picker, path);
        Wait(() => File.Exists(path) && !Windows().Contains(picker) && Native.IsWindowEnabled(main), "Native Save As writes the requested output");
        Require(!Text(main).StartsWith('*'), "Native Save As clears document dirty state");
        return path;
    }
    nint OpenExport()
    {
        Command("File", "Export Subtitles...");
        nint dialog = 0;
        Wait(() => { dialog = Windows().SingleOrDefault(h => Text(h) == "Export" && HasButton(h, "Export...")); return dialog != 0; }, "Native Export dialog opens");
        Require(Children(dialog).Any(h => Text(h) == "Native export settings"), "The fixture export filter is selected and its real configuration is visible");
        return dialog;
    }
    void ExportTo(nint dialog, string filename)
    {
        var path = Path.Combine(documents, filename);
        Click(dialog, "Export...");
        var picker = FileDialog("Export subtitles file"); FillFileDialog(picker, path);
        Wait(() => File.Exists(path) && !Windows().Contains(dialog) && Native.IsWindowEnabled(main), "Lua export completes and writes output");
        AssertEvents(path, exported, "Export transforms exactly the three dialogue events and preserves the comment");
    }
    void Export()
    {
        var undo = UndoItem();
        var dialog = OpenExport();
        SetEdit(dialog, "export-seed", "EXPORT:|,# café ");
        SetEdit(dialog, "11", "3");
        SetChecked(dialog, "Apply native export", true);
        Capture(dialog, "export-accepted-controls");
        ExportTo(dialog, "export-first.ass");
        AssertEvents(SaveAs("after-export.ass"), baseline, "Export operates on a copy and preserves the live document");
        var after = UndoItem();
        Require(after.Label == undo.Label && after.Enabled == undo.Enabled, "Export does not create subtitle undo history");
        dialog = OpenExport();
        var edits = Children(dialog).Where(h => PrivateDesktop.ClassName(h) == "Edit").Select(Text).ToArray();
        Write("export-restored-controls.json", new { edit_values = edits });
        Require(edits.Contains("EXPORT:|,# café ") && edits.Contains("3") && !edits.Contains("export-seed"), "Reconstructed export dialog restores serialized accepted string and integer values");
        var checkbox = Children(dialog).Single(h => PrivateDesktop.ClassName(h) == "Button" && Text(h) == "Apply native export");
        isolation.VerifyWindow(checkbox, app!);
        Require(Native.Send(checkbox, 0xF0, 0, 0) == 1, "Reconstructed export dialog restores the accepted boolean");
        Capture(dialog, "export-restored-controls");
        ExportTo(dialog, "export-second.ass");
        AssertEvents(SaveAs("final.ass"), baseline, "Repeated export preserves the complete live subtitle document");
    }
    void Capture(nint window, string label)
    {
        isolation.VerifyWindow(window, app!); ScreenCapture.SaveWindowPng(window, Path.Combine(artifacts, label + ".png"));
    }
    void Dump()
    {
        Write("failure.json", Windows().Select(h => new {
            handle = h.ToString("X"), text = Text(h), class_name = PrivateDesktop.ClassName(h),
            children = Children(h).Select(c => new { handle = c.ToString("X"), text = Text(c), class_name = PrivateDesktop.ClassName(c), enabled = Native.IsWindowEnabled(c) })
        }).ToArray());
        foreach (var window in Windows().Where(h => PrivateDesktop.ClassName(h) == "#32770" || PrivateDesktop.ClassName(h).StartsWith("wxWindow", StringComparison.Ordinal))) Capture(window, "failure-" + window.ToString("X"));
    }
    static string[] Events(IEnumerable<string> lines) => lines.Where(line => line.StartsWith("Dialogue:", StringComparison.Ordinal) || line.StartsWith("Comment:", StringComparison.Ordinal)).ToArray();
    void Prepare()
    {
        var source = Path.GetFullPath("tests/gui-automation/automation-neutral-native");
        documents = Path.Combine(artifacts, "documents"); profile = Path.Combine(artifacts, "profile");
        Directory.CreateDirectory(documents); Directory.CreateDirectory(Path.Combine(profile, "user"));
        foreach (var filename in new[] { "fixture.ass", "runtime.lua", "expected-accepted.events", "expected-export.events" }) File.Copy(Path.Combine(source, filename), Path.Combine(documents, filename));
        baseline = Events(File.ReadAllLines(Path.Combine(documents, "fixture.ass")));
        accepted = Events(File.ReadAllLines(Path.Combine(documents, "expected-accepted.events")));
        exported = Events(File.ReadAllLines(Path.Combine(documents, "expected-export.events")));
        if (baseline.Length != 4 || accepted.Length != 4 || exported.Length != 4) throw new InvalidDataException("Expected event fixtures must each contain four complete events");
        var config = JsonNode.Parse("""
            {"Subtitle":{"Use STC":false},"App":{"First Start":false,"Language":"en_US","Save UI State":true,"Auto":{"Backup":false,"Check For Updates":false,"Load Linked Files":1,"Save":false,"Save on Every Change":false}},"Automation":{"Lua Workspace":{"Enable LuaLS":false}},"Path":{"Automation":{"Autoload":"","Base":""}}}
            """)!;
        config["Path"]!["Automation"]!["Base"] = documents;
        File.WriteAllText(Path.Combine(profile, "user", "config.json"), config.ToJsonString(Json));
        AutomationTestProfile.WriteMenusWithoutClipboardValidation(profile, artifacts);
    }
    void Start()
    {
        var host = Path.Combine(artifacts, "host"); Directory.CreateDirectory(host);
        var start = new ProcessStartInfo(Path.GetFullPath(args[0])) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", host, "--open", Path.Combine(documents, "fixture.ass") }) start.ArgumentList.Add(argument);
        app = isolation.Start(start, "automation-neutral-native");
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(host, "ready.json"), app, TimeSpan.FromSeconds(20));
        main = isolation.MainWindow(app, 10);
        Wait(() => Native.IsWindowEnabled(main), "Host finishes opening its real Lua fixture");
    }
}

sealed record CheckResult(string Name, string Description, bool Passed);

static class Native
{
    public sealed record MenuItem(string Label, int Id, bool Enabled, bool Checked);
    [return: MarshalAs(UnmanagedType.Bool)] delegate bool Callback(nint window, nint data);
    [DllImport("user32.dll", ExactSpelling = true)]
    [return: MarshalAs(UnmanagedType.Bool)] static extern bool EnumChildWindows(nint parent, Callback callback, nint data);
    [DllImport("user32.dll", ExactSpelling = true)]
    [return: MarshalAs(UnmanagedType.Bool)] public static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll", ExactSpelling = true)]
    [return: MarshalAs(UnmanagedType.Bool)] public static extern bool IsWindow(nint window);
    [DllImport("user32.dll", ExactSpelling = true)]
    [return: MarshalAs(UnmanagedType.Bool)] public static extern bool IsWindowEnabled(nint window);
    [DllImport("user32.dll", EntryPoint = "PostMessageW", ExactSpelling = true, SetLastError = true)] static extern int PostMessage(nint window, uint message, nuint wp, nint lp);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)] static extern nint SendText(nint window, uint message, nuint wp, string lp, uint flags, uint timeout, out nuint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", ExactSpelling = true, SetLastError = true)] static extern nint SendNumber(nint window, uint message, nuint wp, nint lp, uint flags, uint timeout, out nuint result);
    [DllImport("user32.dll", ExactSpelling = true)] static extern nint GetMenu(nint window);
    [DllImport("user32.dll", ExactSpelling = true)] static extern nint GetSubMenu(nint menu, int position);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int GetMenuItemCount(nint menu);
    [DllImport("user32.dll", ExactSpelling = true)] static extern uint GetMenuItemID(nint menu, int position);
    [DllImport("user32.dll", ExactSpelling = true)] static extern uint GetMenuState(nint menu, uint position, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetMenuStringW", ExactSpelling = true, CharSet = CharSet.Unicode)] static extern int GetMenuString(nint menu, uint position, [Out] char[] text, int capacity, uint flags);
    public static nint[] Children(nint parent)
    {
        List<nint> found = []; Callback callback = (h, _) => { found.Add(h); return true; };
        EnumChildWindows(parent, callback, 0); GC.KeepAlive(callback); return found.ToArray();
    }
    public static void SetText(nint window, string value)
    {
        if (SendText(window, 0xC, 0, value, 0x22, 1000, out _) == 0) throw new TimeoutException("Native text entry");
    }
    public static void Post(nint window, uint message, nint wp, nint lp)
    {
        if (PostMessage(window, message, unchecked((nuint)wp), lp) == 0) throw new InvalidOperationException("Native message post failed: " + Marshal.GetLastPInvokeError());
    }
    public static nint Send(nint window, uint message, nint wp, nint lp)
    {
        if (SendNumber(window, message, unchecked((nuint)wp), lp, 0x22, 1000, out var result) == 0) throw new TimeoutException("Native message timed out: " + message.ToString("X") + "; error " + Marshal.GetLastPInvokeError());
        return unchecked((nint)result);
    }
    static string Label(nint menu, int position)
    {
        var value = new char[512]; var length = GetMenuString(menu, (uint)position, value, value.Length, 0x400);
        if (length >= value.Length - 1) throw new InvalidOperationException("Native menu text is truncated");
        return new string(value, 0, length).Split('\t')[0].Replace("&", "", StringComparison.Ordinal);
    }
    static nint Submenu(nint window, string name)
    {
        var menu = GetMenu(window);
        var position = Enumerable.Range(0, GetMenuItemCount(menu)).Single(index => Label(menu, index) == name);
        var submenu = GetSubMenu(menu, position);
        if (submenu == 0) throw new InvalidOperationException("Observed menu has no submenu: " + name);
        return submenu;
    }
    public static MenuItem[] ReadMenu(nint window, string name)
    {
        var submenu = Submenu(window, name);
        Send(window, 0x117, submenu, 0);
        try {
            return Enumerable.Range(0, GetMenuItemCount(submenu)).Where(index => GetMenuItemID(submenu, index) is > 0 and < 65536).Select(index => {
                var state = GetMenuState(submenu, (uint)index, 0x400);
                if (state == uint.MaxValue) throw new InvalidOperationException("Native menu state is unavailable");
                return new MenuItem(Label(submenu, index), checked((int)GetMenuItemID(submenu, index)), (state & 3) == 0, (state & 8) != 0);
            }).ToArray();
        }
        finally { Send(window, 0x125, submenu, 0); }
    }
    public static void Highlight(nint window, string menu, int command)
    {
        var submenu = Submenu(window, menu);
        Send(window, 0x117, submenu, 0); Send(window, 0x11F, command | (0x80 << 16), submenu);
    }
    public static void CloseMenu(nint window, string menu) => Send(window, 0x125, Submenu(window, menu), 0);
}
