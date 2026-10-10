#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

if (args.Length != 2)
{
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/fold-column-native.cs -- EXE ARTIFACTS");
    return 2;
}
try { return PrivateDesktop.Run(args, Path.GetFullPath(args[1]), isolation => new Scenario(args, isolation).Run()); }
catch (Exception error) { Console.Error.WriteLine(error); return 1; }

sealed class Scenario(string[] args, PrivateDesktop isolation)
{
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true, IncludeFields = true };
    readonly string artifacts = Path.GetFullPath(args[1]);
    readonly List<object> checks = [];
    Process? app;
    nint main, grid;
    string profile = "", grouped = "", plain = "", phase = "";
    int launch, observation;
    int[] hiddenEdges = [], visibleEdges = [];
    string[] baselineFields = [];

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
            Prepare(); Start(grouped);
            phase = "default-mixed-groups"; Header(false, true);
            var initial = SaveAs("baseline-grouped");
            baselineFields = NonFoldFields(initial); VerifyGroups(initial, [(1, 3, true), (5, 7, false)]);
            visibleEdges = Capture("default-mixed-groups");

            phase = "hide-mixed-groups"; Header(true, true);
            Require(!Text(main).StartsWith('*'), "Changing the folding-column preference does not dirty the subtitle document");
            var hidden = SaveAs("hidden-mixed-groups");
            Require(hidden.SequenceEqual(initial), "Hiding the column preserves the entire serialized ASS, including collapsed flags and unrelated ExtraData");
            hiddenEdges = Capture("hidden-mixed-groups"); CompareColumnShift(visibleEdges, hiddenEdges);

            phase = "hidden-collapse-all"; FoldCommand("Collapse All Fold Groups");
            VerifyGroups(SaveAs("hidden-collapse-all"), [(1, 3, true), (5, 7, true)]);
            Require(Capture("hidden-collapse-all").SequenceEqual(hiddenEdges), "Collapse All remains usable with the column hidden and does not change header geometry");
            phase = "hidden-expand-all"; FoldCommand("Expand All Fold Groups");
            var expanded = SaveAs("hidden-expand-all"); VerifyGroups(expanded, [(1, 3, false), (5, 7, false)]);
            Require(Capture("hidden-expand-all").SequenceEqual(hiddenEdges), "Expand All remains usable with the column hidden");

            phase = "show-all-expanded"; Header(true, false);
            Require(SaveAs("shown-expanded").SequenceEqual(expanded), "Showing the column does not mutate any subtitle field or fold state");
            Require(Capture("shown-expanded").SequenceEqual(visibleEdges), "The folding column stays visible when every existing group is expanded");

            phase = "switch-to-plain"; Open(plain); VerifyGroups(SaveAs("plain-baseline"), []);
            Header(false, true);
            Require(Capture("plain-enabled").SequenceEqual(hiddenEdges), "Switching to an ungrouped document hides the enabled folding column");
            phase = "create-first-group"; Command("Subtitle", "Select All"); FoldCommand("Create Fold Group");
            VerifyGroups(SaveAs("first-created-group"), [(0, 8, true)]);
            Require(Capture("first-created-group").SequenceEqual(visibleEdges), "Creating the first group shows the enabled column immediately");
            phase = "delete-first-group"; FoldCommand("Clear All Fold Groups"); VerifyGroups(SaveAs("first-group-cleared"), []);
            Require(Capture("first-group-cleared").SequenceEqual(hiddenEdges), "Deleting the only newly created group removes the column");

            phase = "switch-to-grouped"; Open(grouped); VerifyGroups(SaveAs("switched-grouped"), [(1, 3, true), (5, 7, false)]);
            Require(Capture("switched-grouped").SequenceEqual(visibleEdges), "Switching back to a grouped document restores the column");
            phase = "persist-hidden"; Header(true, true); SaveAs("before-hidden-restart"); NormalExit(); VerifyPreference(false, "flushed-hidden");
            phase = "restart-hidden"; Start(grouped); Header(false, false);
            VerifyGroups(SaveAs("restart-hidden"), [(1, 3, true), (5, 7, false)]);
            Require(Capture("restart-hidden").SequenceEqual(hiddenEdges), "A fresh host with the same profile keeps the column hidden even with groups");
            Open(plain); Header(false, false);
            Require(Capture("restart-hidden-plain").SequenceEqual(hiddenEdges), "The persisted disabled preference also hides an ungrouped document");
            phase = "enable-without-groups"; Header(true, false);
            Require(Capture("enabled-without-groups").SequenceEqual(hiddenEdges), "Enabling the preference without groups does not reserve an empty folding column");
            SaveAs("before-enabled-restart"); NormalExit(); VerifyPreference(true, "flushed-enabled");
            phase = "restart-enabled"; Start(grouped); Header(false, true);
            VerifyGroups(SaveAs("restart-enabled"), [(1, 3, true), (5, 7, false)]);
            Require(Capture("restart-enabled").SequenceEqual(visibleEdges), "A second normal restart restores the enabled column for existing groups");

            phase = "expand-before-history"; FoldCommand("Expand All Fold Groups");
            VerifyGroups(SaveAs("expanded-before-history"), [(1, 3, false), (5, 7, false)]);
            Require(Capture("expanded-before-history").SequenceEqual(visibleEdges), "The enabled column remains visible before the independent history phase");
            phase = "clear-all"; FoldCommand("Clear All Fold Groups"); VerifyGroups(SaveAs("cleared"), []);
            Require(Capture("cleared").SequenceEqual(hiddenEdges), "Clearing the last groups removes the column immediately while the preference stays enabled");
            Header(false, true);
            phase = "undo-clear"; History("Undo"); VerifyGroups(SaveAs("undo-clear"), [(1, 3, false), (5, 7, false)]);
            Require(Capture("undo-clear").SequenceEqual(visibleEdges), "Undo restoring groups restores the folding column without changing the preference");
            phase = "redo-clear"; History("Redo"); VerifyGroups(SaveAs("redo-clear"), []);
            Require(Capture("redo-clear").SequenceEqual(hiddenEdges), "Redo removing the last groups removes the column");
            History("Undo"); SaveAs("restored-after-history");
            NormalExit(); VerifyPreference(true, "final-preference");
            File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { passed = true, checks }, Json));
            return 0;
        }
        catch (Exception error)
        {
            if (app is not null && !app.HasExited)
                try { Dump("failure"); Capture("failure"); } catch (Exception evidenceError) { Console.Error.WriteLine(evidenceError); }
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
    static string ClassName(nint window)
    {
        try { return PrivateDesktop.ClassName(window); }
        catch (Win32Exception error) when (error.NativeErrorCode == 1400 && !Native.IsWindow(window)) { return ""; }
    }
    nint[] Children(nint window) => Native.Children(window).Where(Native.IsWindowVisible).ToArray();
    nint[] Windows() => isolation.Windows(app!).Where(Native.IsWindowVisible).ToArray();
    void Action(string action, string target) => File.AppendAllText(Path.Combine(artifacts, "actions.ndjson"), JsonSerializer.Serialize(new { phase, action, target }) + Environment.NewLine);
    void Command(string menu, string label)
    {
        isolation.VerifyWindow(main, app!); Action("observed-menu-command", menu + "/" + label);
        ObservedWindowsCommand.PostMenuCommand(main, app!.Id, ObservedWindowsCommand.FindMenuCommand(main, app.Id, menu, label));
        Thread.Sleep(150);
    }
    void History(string action)
    {
        var edit = Native.Items(Native.GetMenu(main)).Single(item => item.Label == "Edit").Submenu;
        isolation.VerifyWindow(main, app!); Action("native-menu-mnemonic", "Edit"); Native.Post(main, 0x112, 0xF100, 'e');
        nint popup = 0;
        Wait(() => {
            popup = Windows().Where(h => ClassName(h) == "#32768").SingleOrDefault(h => (nint)Native.Send(h, 0x1E1) == edit);
            return popup != 0;
        }, "Actual Edit menu opened and dynamic history labels refreshed");
        var item = Native.Items(edit).Single(item => item.Label == action || item.Label.StartsWith(action + " ", StringComparison.Ordinal));
        Require(item.Enabled, "Actual history menu command is enabled: " + item.Label);
        ClickPopup(popup, edit, item.Position);
        Wait(() => !Windows().Any(h => ClassName(h) == "#32768"), "Actual history menu command completed"); Thread.Sleep(150);
    }
    (nint Window, nint Menu) Popup(bool header)
    {
        isolation.VerifyWindow(grid, app!);
        var point = Native.ClientPoint(grid, 8, header ? 4 : 45);
        Action("native-context-menu", header ? "grid/header" : "grid/rows");
        Native.Post(grid, 0x7B, grid.ToInt64(), Native.Position(point.X, point.Y));
        nint popup = 0;
        Wait(() => { popup = Windows().SingleOrDefault(h => ClassName(h) == "#32768"); return popup != 0; }, "Actual native grid popup opened");
        isolation.VerifyWindow(popup, app!);
        var menu = (nint)Native.Send(popup, 0x1E1);
        Require(menu != 0, "The actual popup exposes its native HMENU");
        var label = $"{++observation:00}-{phase}-{(header ? "header-menu" : "row-menu")}";
        File.WriteAllText(Path.Combine(artifacts, label + ".json"), JsonSerializer.Serialize(Native.Inventory(menu), Json));
        ScreenCapture.SaveWindowPng(popup, Path.Combine(artifacts, label + ".png"));
        return (popup, menu);
    }
    void ClosePopup()
    {
        Native.Post(grid, 0x1F);
        Wait(() => !Windows().Any(h => ClassName(h) == "#32768"), "Actual native popup dismissed");
    }
    void Header(bool toggle, bool expectedBefore)
    {
        var popup = Popup(true); var items = Native.Items(popup.Menu);
        Require(items.Length > 1 && items[0].Label == "Folding column", "Folding column is the first item of the real column-header popup");
        Require(items[0].Checked == expectedBefore && items[0].Enabled, "Folding column checkmark reflects the independent enabled preference");
        if (!toggle) { ClosePopup(); return; }
        ClickPopup(popup.Window, popup.Menu, items[0].Position);
        Wait(() => !Windows().Any(h => ClassName(h) == "#32768"), "Native column-header menu click completed");
        var after = Popup(true);
        Require(Native.Items(after.Menu)[0].Checked != expectedBefore, "Clicking the actual first menu item toggles its checkmark"); ClosePopup();
    }
    void ClickPopup(nint popup, nint menu, int position)
    {
        isolation.VerifyWindow(popup, app!);
        Action("native-popup-keyboard-selection", Native.Items(menu)[position].Label);
        Native.Post(popup, 0x100, 0x24, 1); Native.Post(popup, 0x101, 0x24, unchecked((int)0xC0000001)); Thread.Sleep(50);
        var count = Native.Items(menu).Length;
        for (var step = 0; step < count && !Native.Highlighted(menu, position); ++step)
        {
            Native.Post(popup, 0x100, 0x28, 1); Native.Post(popup, 0x101, 0x28, unchecked((int)0xC0000001)); Thread.Sleep(50);
        }
        Require(Native.Highlighted(menu, position), "Native popup keyboard navigation highlights the exact observed item");
        Native.Post(popup, 0x100, 0x0D, 1); Native.Post(popup, 0x101, 0x0D, unchecked((int)0xC0000001));
    }
    void FoldCommand(string label)
    {
        var popup = Popup(false); var group = Native.Items(popup.Menu).Single(item => item.Label == "Fold Groups");
        Require(group.Submenu != 0, "The original row-context Fold Groups submenu is still present");
        var command = Native.Items(group.Submenu).Single(item => item.Label == label);
        Require(command.Enabled, "Original fold menu command is enabled: " + label);
        ClickPopup(popup.Window, popup.Menu, group.Position);
        nint submenu = 0;
        Wait(() => {
            submenu = Windows().Where(h => ClassName(h) == "#32768").SingleOrDefault(h => (nint)Native.Send(h, 0x1E1) == group.Submenu);
            return submenu != 0;
        }, "Actual Fold Groups submenu opened");
        var screenshot = $"{++observation:00}-{phase}-fold-submenu";
        ScreenCapture.SaveWindowPng(submenu, Path.Combine(artifacts, screenshot + ".png"));
        ClickPopup(submenu, group.Submenu, command.Position);
        Wait(() => !Windows().Any(h => ClassName(h) == "#32768"), "Original fold menu command selected: " + label);
        Thread.Sleep(150);
    }
    int[] Capture(string label)
    {
        isolation.VerifyWindow(grid, app!); Thread.Sleep(100);
        var path = Path.Combine(artifacts, label + "-grid.png"); var evidence = ScreenCapture.SaveWindowPng(grid, path);
        using var bitmap = new Bitmap(path);
        var origin = Native.ClientPoint(grid, 0, 0); var bounds = Native.Bounds(grid);
        int dx = origin.X - bounds.Left, dy = origin.Y - bounds.Top;
        var line = Color.FromArgb(13, 24, 37).ToArgb();
        var edges = Enumerable.Range(dx + 1, Math.Min(700, bitmap.Width - dx - 2))
            .Where(x => Enumerable.Range(dy + 3, 5).All(y => bitmap.GetPixel(x, y).ToArgb() == line))
            .Where(x => bitmap.GetPixel(x - 1, dy + 4).ToArgb() != line).Take(6).Select(x => x - dx).ToArray();
        Require(edges.Length >= 5, "Actual painted header exposes at least five distinct vertical column edges: " + string.Join(',', edges));
        File.WriteAllText(Path.Combine(artifacts, label + "-geometry.json"), JsonSerializer.Serialize(new { evidence, client_origin = new { x = dx, y = dy }, dpi = Native.GetDpiForWindow(grid), column_edges = edges }, Json));
        return edges;
    }
    void CompareColumnShift(int[] shown, int[] hidden)
    {
        int width = shown[0]; var expected = (int)Math.Round(20.0 * Native.GetDpiForWindow(grid) / 96.0);
        Require(width == expected, $"The painted folding gutter is exactly 20 DIP (observed {width} pixels at current DPI)");
        Require(Enumerable.Range(0, 5).All(i => shown[i + 1] == hidden[i] + width), "Hiding the gutter shifts five successive real column boundaries left by its exact width");
    }
    void Dump(string label)
    {
        File.WriteAllText(Path.Combine(artifacts, label + "-inventory.json"), JsonSerializer.Serialize(Windows().Select(h => new {
            handle = h.ToString("X"), text = Text(h), class_name = ClassName(h), bounds = Native.Bounds(h),
            children = Children(h).Select(c => new { handle = c.ToString("X"), text = Text(c), class_name = ClassName(c), bounds = Native.Bounds(c), id = Native.GetDlgCtrlID(c) })
        }), Json));
    }
    nint FileDialog(string title)
    {
        nint dialog = 0;
        Wait(() => { dialog = Windows().SingleOrDefault(h => ClassName(h) == "#32770" && Text(h).Contains(title, StringComparison.OrdinalIgnoreCase)); return dialog != 0; }, "Native " + title + " file dialog opened");
        return dialog;
    }
    void EnterFile(nint dialog, string path, bool open)
    {
        var elements = AutomationElement.FromHandle(dialog).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
        nint handle;
        if (open)
        {
            var children = Children(dialog);
            handle = children.Single(h => ClassName(h) == "Edit" && Native.GetDlgCtrlID(h) == 1148);
            var combo = Native.GetParent(handle); var filename = Native.GetParent(combo);
            Require(ClassName(combo) == "ComboBox" && ClassName(filename) == "ComboBoxEx32" && Native.GetDlgCtrlID(filename) == 1148, "Actual Open filename Edit belongs to its native filename ComboBoxEx32");
            var label = children.Single(h => ClassName(h) == "Static" && Native.GetDlgCtrlID(h) == 1090);
            Require(Native.Bounds(label).Right <= Native.Bounds(handle).Left && Math.Abs(Native.Bounds(label).Top - Native.Bounds(handle).Top) <= 12, "Actual Open filename Edit is adjacent to the observed filename label");
        }
        else
        {
            var edit = elements.Single(e => e.Current.ControlType == ControlType.Edit && e.Current.AutomationId == "1001");
            handle = (nint)edit.Current.NativeWindowHandle;
        }
        isolation.VerifyWindow(handle, app!); Native.SetText(handle, path);
        var button = elements.Single(e => e.Current.ControlType == ControlType.Button && e.Current.AutomationId == "1" && e.Current.NativeWindowHandle != 0);
        isolation.VerifyWindow((nint)button.Current.NativeWindowHandle, app!); ObservedWindowsCommand.PostButtonClick((nint)button.Current.NativeWindowHandle, app!.Id);
        Wait(() => !Windows().Contains(dialog), "Native file dialog accepted the requested file");
    }
    string[] SaveAs(string label)
    {
        Command("File", "Save Subtitles as..."); var dialog = FileDialog("Save");
        var path = Path.Combine(artifacts, label + ".ass"); EnterFile(dialog, path, false);
        Wait(() => File.Exists(path) && !Text(main).StartsWith('*'), "Native Save As wrote a clean document: " + label);
        return File.ReadAllLines(path);
    }
    void Open(string path)
    {
        Command("File", "Open Subtitles..."); EnterFile(FileDialog("Open"), path, true);
        Wait(() => Text(main).Contains(Path.GetFileName(path)) && !Text(main).StartsWith('*'), "Native File Open switched the actual document");
    }
    static string[] NonFoldFields(string[] lines) => lines.Where(line => line.StartsWith("Dialogue:") || line.StartsWith("Comment:")).Select(line => Regex.Replace(line, @"(?<=,)(\{(?:=\d+)+\})(?=.)", "")).ToArray();
    void VerifyGroups(string[] lines, (int First, int Last, bool Collapsed)[] expected)
    {
        var rows = lines.Where(line => line.StartsWith("Dialogue:") || line.StartsWith("Comment:")).ToArray();
        var extras = lines.Where(line => line.StartsWith("Data:")).Select(line => line[5..].Trim().Split(',', 3)).ToDictionary(parts => int.Parse(parts[0]), parts => (Key: parts[1], Value: parts[2]));
        var linked = rows.Select(row => {
            var text = row.Split(',', 10)[9]; var match = Regex.Match(text, @"^\{((?:=\d+)+)\}");
            return match.Success ? Regex.Matches(match.Groups[1].Value, @"\d+").Select(m => extras[int.Parse(m.Value)]).ToArray() : [];
        }).ToArray();
        var markers = linked.Select(data => data.Where(entry => entry.Key == "_aegi_folddata").Select(entry => entry.Value).ToArray()).ToArray();
        Require(rows.Length == 9 && NonFoldFields(lines).SequenceEqual(baselineFields), "All nine complete event fields, text, comments, Unicode and override tags are preserved");
        Require(linked[1].Count(entry => entry.Key == "fold-column-sentinel" && entry.Value == "eretained") == 1, "Unrelated ExtraData remains linked to the original row");
        Require(markers.Sum(values => values.Length) == expected.Length * 2, "Exactly two linked fold boundaries exist per expected group");
        var ids = new List<string>();
        foreach (var (first, last, collapsed) in expected)
        {
            var start = markers[first].Single(); var end = markers[last].Single();
            var parts = start.Split(';'); var finish = end.Split(';');
            Require(parts.Length == 3 && finish.Length == 3 && parts[0] == "e0" && finish[0] == "e1" && parts[1] == (collapsed ? "1" : "0") && finish[1] == parts[1] && finish[2] == parts[2], $"Saved group [{first + 1},{last + 1}] has matching boundaries and collapsed={collapsed}");
            ids.Add(parts[2]);
        }
        Require(ids.Distinct().Count() == ids.Count, "Every saved group has a distinct identity");
    }
    void NormalExit()
    {
        isolation.VerifyWindow(main, app!); Action("normal-exit", "main/WM_CLOSE"); Native.Post(main, 0x10);
        Require(app!.WaitForExit(10_000), "WM_CLOSE exits normally within ten seconds without forced termination");
        Require(isolation.ExitCode(app) == 0, "Normal host native exit code is zero"); app.Dispose(); app = null; main = grid = 0;
    }
    void VerifyPreference(bool expected, string label)
    {
        var stored = JsonNode.Parse(File.ReadAllText(Path.Combine(profile, "user", "config.json")))!["Subtitle"]!["Grid"]!["Show Folding Column"]!;
        File.WriteAllText(Path.Combine(artifacts, label + ".json"), stored.ToJsonString(Json));
        Require(stored.GetValue<bool>() == expected, "Normal shutdown flushes the exact folding-column Boolean preference: " + expected);
    }
    void Prepare()
    {
        var host = Path.Combine(artifacts, "fixtures"); Directory.CreateDirectory(host);
        profile = Path.Combine(artifacts, "profile"); Directory.CreateDirectory(Path.Combine(profile, "user"));
        File.WriteAllText(Path.Combine(profile, "user", "config.json"), """
            {"Subtitle":{"Use STC":false},"Colour":{"Subtitle Grid":{"Lines":"rgb(13,24,37)"}},"App":{"First Start":false,"Language":"en_US","Auto":{"Backup":false,"Check For Updates":false,"Load Linked Files":1,"Save":false,"Save on Every Change":false}}}
            """);
        var header = """
            [Script Info]
            Title: Native Folding Column E2E
            Original Script: metadata sentinel café 世界
            ScriptType: v4.00+
            PlayResX: 640
            PlayResY: 480
            WrapStyle: 2
            [V4+ Styles]
            Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
            Style: Default,Arial,24,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1
            Style: Other,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1
            [Events]
            Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
            """;
        string[] texts = ["before café", "{\\i1}first group, 中文", "comment member 😀", "first end", "between sentinel", "second group", "second member", "second end", "after 世界"];
        string Build(bool groups)
        {
            var events = texts.Select((text, row) => {
                var ids = row == 1 ? (groups ? "{=0=4}" : "{=4}") : groups ? row switch { 3 => "{=1}", 5 => "{=2}", 7 => "{=3}", _ => "" } : "";
                return $"{(row == 2 ? "Comment" : "Dialogue")}: {row % 3},0:00:{row + 1:00}.00,0:00:{row + 2:00}.23,{(row % 2 == 0 ? "Default" : "Other")},actor-{row},0011,0012,0013,fx-{row},{ids}{text}";
            });
            var data = groups ? "Data: 0,_aegi_folddata,e0;1;101\nData: 1,_aegi_folddata,e1;1;101\nData: 2,_aegi_folddata,e0;0;202\nData: 3,_aegi_folddata,e1;0;202\n" : "";
            return header + "\n" + string.Join('\n', events) + "\n[Aegisub Extradata]\n" + data + "Data: 4,fold-column-sentinel,eretained\n";
        }
        grouped = Path.Combine(host, "grouped.ass"); plain = Path.Combine(host, "plain.ass");
        File.WriteAllText(grouped, Build(true), new UTF8Encoding(true)); File.WriteAllText(plain, Build(false), new UTF8Encoding(true));
    }
    void Start(string fixture)
    {
        var host = Path.Combine(artifacts, "launch-" + ++launch); Directory.CreateDirectory(host);
        var start = new ProcessStartInfo(Path.GetFullPath(args[0])) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", host, "--open", fixture }) start.ArgumentList.Add(argument);
        app = isolation.Start(start, "fold-column-" + launch);
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(host, "ready.json"), app, TimeSpan.FromSeconds(20));
        main = isolation.MainWindow(app, 10); isolation.VerifyWindow(main, app);
        Require(Native.SetWindowPos(main, 0, 0, 0, 1100, 760, 0x16), "Main window receives a repeatable non-activating native size");
        Wait(() => {
            var scrollbars = Children(main).Where(h => ClassName(h) == "ScrollBar" && Native.GetDlgCtrlID(h) == 1730).ToArray();
            if (scrollbars.Length != 1) return false;
            grid = Native.GetParent(scrollbars[0]); return grid != 0 && ClassName(grid).StartsWith("wxWindow", StringComparison.Ordinal);
        }, "Subtitle grid discovered from its actual unique native grid scrollbar");
        Dump("launch-" + launch);
    }
}

static class Native
{
    delegate bool Callback(nint window, nint data);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X, Y; }
    public sealed record Item(int Position, string Label, uint Id, bool Checked, bool Enabled, nint Submenu);
    [DllImport("user32.dll")] public static extern nint GetMenu(nint window);
    [DllImport("user32.dll")] static extern nint GetSubMenu(nint menu, int position);
    [DllImport("user32.dll")] static extern int GetMenuItemCount(nint menu);
    [DllImport("user32.dll")] static extern uint GetMenuItemID(nint menu, int position);
    [DllImport("user32.dll")] static extern uint GetMenuState(nint menu, uint position, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetMenuStringW", CharSet = CharSet.Unicode)] static extern int GetMenuString(nint menu, uint position, StringBuilder text, int capacity, uint flags);
    [DllImport("user32.dll")] static extern bool GetMenuItemRect(nint window, nint menu, uint position, out Rect rect);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(nint parent, Callback callback, nint data);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] public static extern bool IsWindow(nint window);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(nint window);
    [DllImport("user32.dll")] public static extern nint GetParent(nint window);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(nint window);
    [DllImport("user32.dll")] static extern bool GetWindowRect(nint window, out Rect rect);
    [DllImport("user32.dll")] static extern bool ClientToScreen(nint window, ref Point point);
    [DllImport("user32.dll")] static extern bool ScreenToClient(nint window, ref Point point);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(nint window, nint after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll", EntryPoint = "PostMessageW")] static extern bool PostMessage(nint window, uint message, nint wp, nint lp);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")] static extern nint SendMessage(nint window, uint message, nint wp, nint lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint SendText(nint window, uint message, nint wp, string lp, uint flags, uint timeout, out nint result);
    public static Item[] Items(nint menu) => Enumerable.Range(0, GetMenuItemCount(menu)).Select(position => {
        var text = new StringBuilder(256); var length = GetMenuString(menu, (uint)position, text, text.Capacity, 0x400);
        if (length >= text.Capacity - 1) throw new InvalidOperationException("Native menu label was truncated");
        var state = GetMenuState(menu, (uint)position, 0x400);
        if (state == uint.MaxValue) throw new InvalidOperationException("Observed native menu disappeared");
        return new Item(position, text.ToString().Split('\t')[0].Replace("&", "", StringComparison.Ordinal), GetMenuItemID(menu, position), (state & 8) != 0, (state & 3) == 0, GetSubMenu(menu, position));
    }).ToArray();
    public static object Inventory(nint menu) => Items(menu).Select(item => new { item.Position, item.Label, item.Id, item.Checked, item.Enabled, children = item.Submenu == 0 ? null : Inventory(item.Submenu) }).ToArray();
    public static bool Highlighted(nint menu, int position) => (GetMenuState(menu, (uint)position, 0x400) & 0x80) != 0;
    public static Rect Bounds(nint window) { if (!GetWindowRect(window, out var rect)) throw new InvalidOperationException("Native bounds unavailable"); return rect; }
    public static Rect MenuItemBounds(nint menu, int position) { if (!GetMenuItemRect(0, menu, (uint)position, out var rect)) throw new InvalidOperationException("Native menu item bounds unavailable"); return rect; }
    public static Point ClientPoint(nint window, int x, int y) { var point = new Point { X = x, Y = y }; if (!ClientToScreen(window, ref point)) throw new InvalidOperationException("ClientToScreen failed"); return point; }
    public static Point ScreenPoint(nint window, int x, int y) { var point = new Point { X = x, Y = y }; if (!ScreenToClient(window, ref point)) throw new InvalidOperationException("ScreenToClient failed"); return point; }
    public static long Position(int x, int y) => unchecked((int)((ushort)x | (uint)(ushort)y << 16));
    public static nint[] Children(nint parent) { List<nint> found = []; Callback callback = (h, _) => { found.Add(h); return true; }; EnumChildWindows(parent, callback, 0); GC.KeepAlive(callback); return found.ToArray(); }
    public static long Send(nint window, uint message, long wp = 0, long lp = 0) { if (SendMessage(window, message, (nint)wp, (nint)lp, 0x22, 1000, out var result) == 0) throw new TimeoutException("Native control message"); return result.ToInt64(); }
    public static void SetText(nint window, string value) { if (SendText(window, 0xC, 0, value, 0x22, 1000, out _) == 0) throw new TimeoutException("Native text entry"); }
    public static void Post(nint window, uint message, long wp = 0, long lp = 0) { if (!PostMessage(window, message, (nint)wp, (nint)lp)) throw new InvalidOperationException("Native message post failed"); }
}
