#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

if (args.Length != 2)
{
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/local-script-references-native.cs -- EXE ARTIFACTS");
    return 2;
}
try { return PrivateDesktop.Run(args, Path.GetFullPath(args[1]), desktop => new Scenario(args, desktop).Run()); }
catch (Exception error) { Console.Error.WriteLine(error); return 1; }

sealed class Scenario(string[] args, PrivateDesktop isolation)
{
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    static readonly string[] CaseNames = ["ReferenceDiagnosticsInInputOrder", "TrimmedPrefixesPreserveLoadedOrder", "ManagerAddPublishesMacro", "ReloadUsesChangedLua", "ManagerRemovePublishesReferences", "SaveAndReopenReferences", "ReopenEmptyClearsMacros"];
    readonly string artifacts = Path.GetFullPath(args[1]);
    readonly List<CheckResult> checks = [];
    Process? app;
    nint main, manager;
    string phase = "setup", fixture = "", emptyFixture = "", profile = "", baseDir = "", scriptDir = "", externalDir = "";
    string[] baselineEvents = [];

    public int Run()
    {
        Directory.CreateDirectory(artifacts); isolation.RecordDriver();
        using var log = new StreamWriter(Path.Combine(artifacts, "driver.log")) { AutoFlush = true };
        Console.SetOut(log); Console.SetError(log);
        using var watchdog = new Timer(_ => {
            WriteResult(false, "180-second watchdog expired");
            try { if (app is not null) isolation.Stop(app); } finally { Environment.Exit(124); }
        }, null, 180_000, Timeout.Infinite);
        try
        {
            Prepare(); Start();
            phase = CaseNames[0];
            OpenFile(fixture, false);
            ReadDiagnostics();
            Wait(() => Text(main).Contains("references.ass", StringComparison.Ordinal) && Native.IsWindowEnabled(main), "Reference document finished opening");
            phase = CaseNames[1]; OpenManager();
            AssertRows("initial-manager", "Local Alpha", "Base Beta", "Absolute Gamma", "broken.lua", "unknown", "Tail Delta");
            var initial = Rows();
            Require(initial.All(row => row[0] == "L"), "All retained entries are local scripts");
            Require(initial[3][3] == FailureDescription(), "Failed Lua remains in its original list position with its exact diagnostic");
            Require(initial[4][3] == "File was not recognized as a script", "Unrecognized file remains in its original list position with the original description");
            CloseManager(); AssertMacros("initial-menu", "Ref Alpha", "Ref Beta", "Ref Gamma", "Ref Delta");

            phase = CaseNames[2]; OpenManager(); AddScript(Path.Combine(externalDir, "added.lua"));
            AssertRows("after-add", "Local Alpha", "Base Beta", "Absolute Gamma", "broken.lua", "unknown", "Tail Delta", "Added Epsilon");
            CloseManager(); AssertMacros("after-add-menu", "Ref Alpha", "Ref Beta", "Ref Gamma", "Ref Delta", "Ref Epsilon");

            phase = CaseNames[3]; OpenManager();
            WriteLua(Path.Combine(scriptDir, "local.lua"), "Local Alpha Revised", "Ref Alpha Revised", "reloaded description");
            SelectRow("Local Alpha"); Click(manager, "Reload");
            Wait(() => Rows().Any(row => row[1] == "Local Alpha Revised"), "Reload consumed the changed Lua file");
            AssertRows("after-reload", "Local Alpha Revised", "Base Beta", "Absolute Gamma", "broken.lua", "unknown", "Tail Delta", "Added Epsilon");
            Require(Rows()[0][3] == "reloaded description", "Reload replaced the displayed script description");
            CloseManager(); AssertMacros("after-reload-menu", "Ref Alpha Revised", "Ref Beta", "Ref Gamma", "Ref Delta", "Ref Epsilon");

            phase = CaseNames[4]; OpenManager();
            RemoveScript("broken.lua"); RemoveScript("unknown"); RemoveScript("Tail Delta");
            AssertRows("after-remove", "Local Alpha Revised", "Base Beta", "Absolute Gamma", "Added Epsilon");
            CloseManager(); AssertMacros("after-remove-menu", "Ref Alpha Revised", "Ref Beta", "Ref Gamma", "Ref Epsilon");

            phase = CaseNames[5];
            var saved = Path.Combine(scriptDir, "saved.ass"); SaveAs(saved);
            var lines = File.ReadAllLines(saved);
            var reference = lines.Single(line => line.StartsWith("Automation Scripts: ", StringComparison.Ordinal));
            Require(reference == "Automation Scripts: ~local.lua|$base.lua|~../external/absolute.lua|~../external/added.lua", "Saved ASS preserves loaded order and the shortest base-relative or script-relative references");
            Require(Events(lines).SequenceEqual(baselineEvents), "Manager changes preserve every event field and event order");
            File.WriteAllText(Path.Combine(artifacts, "saved-reference.json"), JsonSerializer.Serialize(new { reference, events = Events(lines) }, Json));
            OpenFile(emptyFixture, true); OpenManager(); AssertRows("between-reopen-empty"); CloseManager();
            OpenFile(saved, true); OpenManager();
            AssertRows("reopened-manager", "Local Alpha Revised", "Base Beta", "Absolute Gamma", "Added Epsilon");
            CloseManager(); AssertMacros("reopened-menu", "Ref Alpha Revised", "Ref Beta", "Ref Gamma", "Ref Epsilon");
            var roundtrip = Path.Combine(scriptDir, "roundtrip.ass"); SaveAs(roundtrip);
            var reopened = File.ReadAllLines(roundtrip);
            Require(reopened.Single(line => line.StartsWith("Automation Scripts: ", StringComparison.Ordinal)) == reference, "Reopen and a second Save As preserve the exact reference string");
            Require(Events(reopened).SequenceEqual(baselineEvents), "Reopen and a second Save As preserve all subtitle events");

            phase = CaseNames[6]; OpenFile(emptyFixture, true); OpenManager(); AssertRows("final-empty-manager"); CloseManager();
            AssertMacros("final-empty-menu");
            Native.Post(main, 0x10, 0, 0);
            Require(app!.WaitForExit(10_000), "Host closes normally within ten seconds");
            Require(isolation.ExitCode(app) == 0, "Normal host exit code is zero");
            WriteResult(true, null);
            using var report = JsonDocument.Parse(File.ReadAllText(Path.Combine(artifacts, "result.json")));
            var cases = report.RootElement.GetProperty("cases").EnumerateArray().ToArray();
            if (!report.RootElement.GetProperty("passed").GetBoolean() || cases.Length != CaseNames.Length || cases.Any(c => c.GetProperty("status").GetString() != "passed" || c.GetProperty("checks").GetInt32() == 0))
                throw new InvalidOperationException("Result report has missing, empty or unsuccessful cases");
            return 0;
        }
        catch (Exception error)
        {
            if (app is not null && !app.HasExited)
                try { Dump("failure"); } catch (Exception captureError) { Console.Error.WriteLine(captureError); }
            WriteResult(false, error.ToString()); Console.Error.WriteLine(error); return 1;
        }
        finally { if (app is not null) { isolation.Stop(app); app.Dispose(); } }
    }

    void WriteResult(bool passed, string? error)
    {
        File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new {
            passed, error,
            cases = CaseNames.Select(name => new { name, status = checks.Any(c => c.Name == name && !c.Passed) ? "failed" : passed && checks.Any(c => c.Name == name) ? "passed" : "incomplete", checks = checks.Count(c => c.Name == name) }), checks
        }, Json));
    }
    void Require(bool condition, string description)
    {
        checks.Add(new(phase, description, condition)); Console.WriteLine(phase + ": " + description + ": " + condition);
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
    void Command(string menu, string label)
    {
        isolation.VerifyWindow(main, app!); Action("menu", menu + "/" + label);
        ObservedWindowsCommand.PostMenuCommand(main, app!.Id, ObservedWindowsCommand.FindMenuCommand(main, app.Id, menu, label));
    }
    void Click(nint parent, string label)
    {
        var button = Children(parent).Single(h => PrivateDesktop.ClassName(h) == "Button" && Text(h).Replace("&", "", StringComparison.Ordinal) == label);
        isolation.VerifyWindow(button, app!);
        if (!Native.IsWindowEnabled(button)) throw new InvalidOperationException("Observed button is disabled: " + label);
        Action("button", label); Native.Post(button, 0xF5, 0, 0);
    }
    void OpenManager()
    {
        Command("Automation", "Automation...");
        Wait(() => { manager = Windows().SingleOrDefault(h => Text(h) == "Automation Manager"); return manager != 0; }, "Automation Manager opened");
    }
    void CloseManager()
    {
        var old = manager; Click(manager, "Close"); Wait(() => !Windows().Contains(old), "Automation Manager closed"); manager = 0;
    }
    AutomationElement List()
    {
        var handle = Children(manager).Single(h => PrivateDesktop.ClassName(h) == "SysListView32");
        isolation.VerifyWindow(handle, app!); return AutomationElement.FromHandle(handle);
    }
    string[][] Rows()
    {
        var grid = (GridPattern)List().GetCurrentPattern(GridPattern.Pattern);
        return Enumerable.Range(0, grid.Current.RowCount).Select(row => Enumerable.Range(0, grid.Current.ColumnCount).Select(column => grid.GetItem(row, column).Current.Name).ToArray()).ToArray();
    }
    void AssertRows(string label, params string[] expected)
    {
        var actual = Rows();
        File.WriteAllText(Path.Combine(artifacts, label + ".json"), JsonSerializer.Serialize(new { expected, rows = actual }, Json));
        Capture(manager, label);
        Require(actual.All(row => row.Length == 4) && actual.Select(row => row[1]).SequenceEqual(expected), "Exact script names and list order: " + string.Join(", ", expected));
    }
    void SelectRow(string name)
    {
        var list = List(); var grid = (GridPattern)list.GetCurrentPattern(GridPattern.Pattern);
        var index = Enumerable.Range(0, grid.Current.RowCount).Single(i => grid.GetItem(i, 1).Current.Name == name);
        var rows = list.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.DataItem)).Cast<AutomationElement>().ToArray();
        if (rows.Length != grid.Current.RowCount) throw new InvalidOperationException("List row selection does not match observed grid");
        if (rows[index].Current.ProcessId != app!.Id) throw new InvalidOperationException("List row no longer belongs to host");
        ((SelectionItemPattern)rows[index].GetCurrentPattern(SelectionItemPattern.Pattern)).Select();
        Wait(() => ((SelectionItemPattern)rows[index].GetCurrentPattern(SelectionItemPattern.Pattern)).Current.IsSelected, "Requested manager row selected");
        Action("select-script", name);
    }
    void AddScript(string path)
    {
        Click(manager, "Add"); var picker = FileDialog("Add Automation script"); FillFileDialog(picker, path);
        Wait(() => !Windows().Contains(picker) && Rows().Any(row => row[1] == "Added Epsilon"), "Add loaded the selected Lua file");
    }
    void RemoveScript(string name)
    {
        var count = Rows().Length; SelectRow(name); Click(manager, "Remove");
        Wait(() => Rows().Length == count - 1, "Remove unloaded selected script");
        Require(Rows().All(row => row[1] != name), "Removed script is absent: " + name);
    }
    void AssertMacros(string label, params string[] expected)
    {
        isolation.VerifyWindow(main, app!); var entries = Native.MenuLabels(main, "Automation");
        File.WriteAllText(Path.Combine(artifacts, label + ".json"), JsonSerializer.Serialize(new { expected, entries }, Json));
        Require(entries.Where(item => item.StartsWith("Ref ", StringComparison.Ordinal)).Order(StringComparer.Ordinal).SequenceEqual(expected.Order(StringComparer.Ordinal)), "Actual Automation menu exposes exactly the retained fixture macros");
        if (expected.Length == 0) Require(entries.Contains("No Automation macros loaded"), "Empty local manager publishes the empty macro menu");
    }
    nint FileDialog(string title)
    {
        nint result = 0;
        Wait(() => { result = Windows().SingleOrDefault(h => PrivateDesktop.ClassName(h) == "#32770" && Text(h) == title); return result != 0; }, "Native file dialog opened: " + title);
        return result;
    }
    void FillFileDialog(nint picker, string path)
    {
        var elements = AutomationElement.FromHandle(picker).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
        var edit = elements.Single(e => e.Current.ControlType == ControlType.Edit && e.Current.AutomationId is "1001" or "1148");
        var handle = (nint)edit.Current.NativeWindowHandle; isolation.VerifyWindow(handle, app!); Native.SetText(handle, path);
        var button = elements.Single(e => e.Current.ControlType == ControlType.Button && e.Current.AutomationId == "1" && e.Current.NativeWindowHandle != 0);
        handle = (nint)button.Current.NativeWindowHandle; isolation.VerifyWindow(handle, app!);
        Action("file-dialog-path", Path.GetRelativePath(artifacts, path)); ObservedWindowsCommand.PostButtonClick(handle, app!.Id);
    }
    void OpenFile(string path, bool complete)
    {
        Command("File", "Open Subtitles..."); var picker = FileDialog("Open subtitles file"); FillFileDialog(picker, path);
        Wait(() => !Windows().Contains(picker), "Native open file picker closed");
        if (complete) Wait(() => Native.IsWindowEnabled(main) && Text(main).Contains(Path.GetFileName(path), StringComparison.Ordinal), "Native file open completed");
    }
    void SaveAs(string path)
    {
        Command("File", "Save Subtitles as..."); nint picker = 0;
        Wait(() => { picker = Windows().SingleOrDefault(h => PrivateDesktop.ClassName(h) == "#32770" && Text(h).Contains("Save", StringComparison.Ordinal)); return picker != 0; }, "Native Save As opened");
        FillFileDialog(picker, path);
        Wait(() => File.Exists(path) && !Windows().Contains(picker), "Native Save As wrote output");
        Require(!Text(main).StartsWith('*'), "Native Save As clears document dirty state");
    }
    string FailureDescription() => "Error initialising Lua script \"broken.lua\":\n\n    File \"" + Path.Combine(scriptDir, "broken.lua")
        + "\", line 1\n<main>\n    File \"<C function>\", line -1\n?\nfixture-load-failure";
    void ReadDiagnostics()
    {
        var expected = new[] {
            "Automation Script referenced with unknown location specifier character.\nLocation specifier found: ?\nFilename specified: mystery.lua",
            "Automation Script referenced could not be found.\nFilename specified: ~missing.lua\nSearched relative to: " + scriptDir + "\nResolved filename: " + Path.Combine(scriptDir, "missing.lua"),
            "Failed to load Automation script '" + Path.Combine(scriptDir, "broken.lua") + "':\n" + FailureDescription(),
            "The file was not recognised as an Automation script: " + Path.Combine(scriptDir, "unknown.txt")
        };
        var observed = new List<string>();
        for (var index = 0; index < expected.Length; index++)
        {
            nint warning = 0;
            Wait(() => { warning = Windows().SingleOrDefault(h => Text(h) == "Automation"); return warning != 0; }, "Expected ordered Automation diagnostic");
            isolation.VerifyWindow(warning, app!);
            var content = AutomationElement.FromHandle(warning).FindFirst(TreeScope.Descendants, new PropertyCondition(AutomationElement.AutomationIdProperty, "ContentText"))
                ?? throw new InvalidOperationException("Observed Automation dialog has no ContentText");
            if (content.Current.ProcessId != app!.Id) throw new InvalidOperationException("Diagnostic content no longer belongs to host");
            var message = content.Current.Name.Replace("\r\n", "\n", StringComparison.Ordinal); observed.Add(message);
            File.WriteAllText(Path.Combine(artifacts, "diagnostics.json"), JsonSerializer.Serialize(new { expected, observed }, Json));
            Capture(warning, "diagnostic-" + (index + 1));
            Require(message == expected[index], "Original diagnostic text and order at position " + (index + 1));
            Click(warning, "OK");
            Wait(() => !Windows().Contains(warning), "Acknowledged Automation diagnostic closed");
        }
    }
    void Capture(nint window, string label)
    {
        isolation.VerifyWindow(window, app!); ScreenCapture.SaveWindowPng(window, Path.Combine(artifacts, label + ".png"));
    }
    void Dump(string label)
    {
        File.WriteAllText(Path.Combine(artifacts, label + ".json"), JsonSerializer.Serialize(Windows().Select(h => new {
            handle = h.ToString("X"), text = Text(h), class_name = PrivateDesktop.ClassName(h),
            children = Children(h).Select(c => new { handle = c.ToString("X"), text = Text(c), class_name = PrivateDesktop.ClassName(c), enabled = Native.IsWindowEnabled(c) }),
            accessible = AutomationElement.FromHandle(h).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().Select(e => new { name = e.Current.Name, type = e.Current.ControlType.ProgrammaticName, id = e.Current.AutomationId })
        }), Json));
        foreach (var window in Windows().Where(h => PrivateDesktop.ClassName(h) == "#32770" || PrivateDesktop.ClassName(h).StartsWith("wxWindow", StringComparison.Ordinal))) Capture(window, label + "-" + window.ToString("X"));
    }
    static string[] Events(IEnumerable<string> lines) => lines.Where(line => line.StartsWith("Dialogue:", StringComparison.Ordinal) || line.StartsWith("Comment:", StringComparison.Ordinal)).ToArray();
    static void WriteLua(string path, string name, string macro, string description)
    {
        File.WriteAllText(path, $"script_name = {JsonSerializer.Serialize(name)}\nscript_description = {JsonSerializer.Serialize(description)}\naegisub.register_macro({JsonSerializer.Serialize(macro)}, 'reference fixture', function() end)\n", new UTF8Encoding(false));
    }
    void Prepare()
    {
        profile = Path.Combine(artifacts, "profile"); scriptDir = Path.Combine(artifacts, "documents");
        baseDir = Path.Combine(artifacts, "automation-base", "nested"); externalDir = Path.Combine(artifacts, "external");
        foreach (var dir in new[] { Path.Combine(profile, "user"), scriptDir, baseDir, externalDir }) Directory.CreateDirectory(dir);
        var config = JsonNode.Parse("""
            {"Subtitle":{"Use STC":false},"App":{"First Start":false,"Language":"en_US","Auto":{"Backup":false,"Check For Updates":false,"Load Linked Files":1,"Save":false,"Save on Every Change":false}},"Automation":{"Lua Workspace":{"Enable LuaLS":false}},"Path":{"Automation":{"Autoload":"","Base":""}}}
            """)!;
        config["Path"]!["Automation"]!["Base"] = baseDir;
        File.WriteAllText(Path.Combine(profile, "user", "config.json"), config.ToJsonString(Json));
        WriteLua(Path.Combine(scriptDir, "local.lua"), "Local Alpha", "Ref Alpha", "original description");
        WriteLua(Path.Combine(baseDir, "base.lua"), "Base Beta", "Ref Beta", "base directory fixture");
        WriteLua(Path.Combine(externalDir, "absolute.lua"), "Absolute Gamma", "Ref Gamma", "absolute input fixture");
        WriteLua(Path.Combine(scriptDir, "tail.lua"), "Tail Delta", "Ref Delta", "loaded after errors");
        WriteLua(Path.Combine(externalDir, "added.lua"), "Added Epsilon", "Ref Epsilon", "added through manager");
        File.WriteAllText(Path.Combine(scriptDir, "broken.lua"), "error('fixture-load-failure', 0)\n");
        File.WriteAllText(Path.Combine(scriptDir, "unknown.txt"), "This is deliberately not a recognized script format.\n");
        var refs = "  |  ~local.lua  ||\t$base.lua\t| /" + Path.Combine(externalDir, "absolute.lua").Replace('\\', '/') + " |?mystery.lua| ~missing.lua |~broken.lua|~unknown.txt| ~tail.lua | ";
        var document = """
            [Script Info]
            Title: Local Script References E2E
            ScriptType: v4.00+
            PlayResX: 640
            PlayResY: 480
            [Aegisub Project Garbage]
            Automation Scripts: REFERENCES
            [V4+ Styles]
            Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
            Style: Default,Arial,24,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1
            [Events]
            Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
            Dialogue: 0,0:00:01.00,0:00:03.00,Default,first,0,0,0,,{\i1}café, 世界
            Comment: 1,0:00:02.00,0:00:04.00,Default,comment,0,0,0,sentinel,Keep this comment
            Dialogue: 2,0:00:05.00,0:00:07.25,Default,last,0,0,0,,Second dialogue
            """;
        fixture = Path.Combine(scriptDir, "references.ass"); emptyFixture = Path.Combine(scriptDir, "empty.ass");
        File.WriteAllText(fixture, document.Replace("REFERENCES", refs, StringComparison.Ordinal), new UTF8Encoding(true));
        File.WriteAllText(emptyFixture, document.Replace("Automation Scripts: REFERENCES", "", StringComparison.Ordinal), new UTF8Encoding(true));
        baselineEvents = Events(document.Split('\n').Select(line => line.TrimEnd('\r')));
    }
    void Start()
    {
        var host = Path.Combine(artifacts, "host"); Directory.CreateDirectory(host);
        var start = new ProcessStartInfo(Path.GetFullPath(args[0])) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", host, "--open", emptyFixture }) start.ArgumentList.Add(argument);
        app = isolation.Start(start, "local-script-references");
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(host, "ready.json"), app, TimeSpan.FromSeconds(20));
        main = isolation.MainWindow(app, 10);
    }
}

sealed record CheckResult(string Name, string Description, bool Passed);

static class Native
{
    delegate bool Callback(nint window, nint data);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(nint parent, Callback callback, nint data);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(nint window);
    [DllImport("user32.dll", EntryPoint = "PostMessageW")] static extern bool PostMessage(nint window, uint message, nint wp, nint lp);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint SendText(nint window, uint message, nint wp, string lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll")] static extern nint GetMenu(nint window);
    [DllImport("user32.dll")] static extern nint GetSubMenu(nint menu, int position);
    [DllImport("user32.dll")] static extern int GetMenuItemCount(nint menu);
    [DllImport("user32.dll", EntryPoint = "GetMenuStringW", CharSet = CharSet.Unicode)] static extern int GetMenuString(nint menu, uint position, StringBuilder text, int capacity, uint flags);
    public static nint[] Children(nint parent) { List<nint> found = []; Callback callback = (h, _) => { found.Add(h); return true; }; EnumChildWindows(parent, callback, 0); GC.KeepAlive(callback); return found.ToArray(); }
    public static void SetText(nint window, string value) { if (SendText(window, 0xC, 0, value, 0x22, 1000, out _) == 0) throw new TimeoutException("Native text entry"); }
    public static void Post(nint window, uint message, long wp, long lp) { if (!PostMessage(window, message, (nint)wp, (nint)lp)) throw new InvalidOperationException("Native message post failed"); }
    static string Label(nint menu, int position)
    {
        var value = new StringBuilder(512); var length = GetMenuString(menu, (uint)position, value, value.Capacity, 0x400);
        if (length >= value.Capacity - 1) throw new InvalidOperationException("Native menu text is truncated");
        return value.ToString().Split('\t')[0].Replace("&", "", StringComparison.Ordinal);
    }
    public static string[] MenuLabels(nint window, string name)
    {
        var menu = GetMenu(window);
        var position = Enumerable.Range(0, GetMenuItemCount(menu)).Single(i => Label(menu, i) == name);
        var sub = GetSubMenu(menu, position);
        return Enumerable.Range(0, GetMenuItemCount(sub)).Select(i => Label(sub, i)).Where(label => label.Length != 0).ToArray();
    }
}
