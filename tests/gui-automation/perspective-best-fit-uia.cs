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
using System.Text.RegularExpressions;
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

if (args.Length is < 3 or > 4 || (args.Length == 4 && args[3] != "--inspect"))
{
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/perspective-best-fit-uia.cs -- EXE VIDEO ARTIFACTS [--inspect]");
    return 2;
}
try
{
    return PrivateDesktop.Run(args, Path.GetFullPath(args[2]), isolation => new Scenario(args, isolation).Run());
}
catch (Exception error) { Console.Error.WriteLine(error); return 1; }

sealed class Scenario
{
    readonly string exe, video, artifacts, profile, fixture;
    readonly PrivateDesktop isolation;
    readonly List<object> checks = [];
    Process? app;
    nint main, canvas, subToolbar;
    int launch;
    readonly bool inspect;
    string source = Source;
    const string Best = "Choose the best geometric fit without reducing tags";
    const string Fit = "Scale text to fit the target";
    const string Fax = "Allow position and scale changes, but restrict Perspective";
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    static readonly Regex Geometry = new(@"\\(?:an\d|pos\([^)]*\)|org\([^)]*\)|(?:frx|fry|frz|fax|fay|fscx|fscy)[-+\d.]+)");
    const string Source = """
        [Script Info]
        Title: Perspective Best Fit E2E
        ScriptType: v4.00+
        PlayResX: 640
        PlayResY: 480
        LayoutResX: 640
        LayoutResY: 480
        ScaledBorderAndShadow: yes
        YCbCr Matrix: TV.601
        [V4+ Styles]
        Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
        Style: Default,Arial,30,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,7,10,10,10,1
        Style: Sentinel,Arial,23,&H0066DDFF,&H000000FF,&H00000000,&H00000000,0,-1,0,0,110,95,1.2,0,1,1,0,2,15,20,25,1
        [Events]
        Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
        Dialogue: 3,0:00:00.00,0:00:10.00,Default,primary,12,23,34,,{\an7\pos(180,320)\org(210,330)\frz12\fax0.15\fscx125\fscy85\bord2.5\shad1.25\1c&H74B2E8&\alpha&H10&}Best fit\NGeometry
        Dialogue: 1,0:00:20.00,0:00:25.00,Sentinel,unselected,31,42,53,,{\pos(420,400)\frz-7\bord1.5\1c&H80E8B2&}Unselected sentinel
        """;

    public Scenario(string[] args, PrivateDesktop isolation)
    {
        exe = Path.GetFullPath(args[0]); video = Path.GetFullPath(args[1]); artifacts = Path.GetFullPath(args[2]);
        profile = Path.Combine(artifacts, "profile"); fixture = Path.Combine(artifacts, "scenario.ass"); this.isolation = isolation; inspect = args.Length == 4;
    }

    public int Run()
    {
        Native.SetThreadDpiAwarenessContext(-4);
        Directory.CreateDirectory(artifacts); isolation.RecordDriver();
        using var output = new StreamWriter(Path.Combine(artifacts, "driver.log")) { AutoFlush = true };
        Console.SetOut(output); Console.SetError(output);
        using var watchdog = new Timer(_ => { try { if (app is not null) isolation.Stop(app); } finally { Environment.Exit(124); } }, null, 180_000, Timeout.Infinite);
        try
        {
            Require(File.Exists(exe) && File.Exists(video), "Executable and video fixture exist");
            File.WriteAllText(Path.Combine(artifacts, "input.ass"), source, new UTF8Encoding(false));
            Profile(); Start();
            if (inspect) { Dump(main, "tool-probe-uia"); ScreenCapture.SaveWindowPng(main, Path.Combine(artifacts, "tool-probe.png")); Close(); File.WriteAllText(Path.Combine(artifacts, "probe.json"), JsonSerializer.Serialize(new { passed = true, scope = "tool-selection-only", checks }, Json)); return 0; }
            Require(!ToolbarState(Best).Checked, "Best Fit defaults off when absent from configuration");
            var reduced = Apply("reduced"); Undo();
            ClickToolbar(Best);
            Require(ToolbarState(Best).Checked && !ToolbarState(Fax).Enabled, "Toolbar enables Best Fit and disables Fax + Frz Only");
            PreferencesRoundTrip();
            var best = Apply("best-fit");
            Require(best != reduced, "Best Fit changes generated ASS compared with constrained reduction");
            foreach (var tag in new[] { "pos", "fscx", "fscy", "fax", "fay", "frx", "fry", "frz" })
                Require(Regex.Matches(best, @"\\" + tag + @"(?:\(|[-+\d])").Count == 1, "Unreduced output includes exactly one " + tag + " tag, including zero values");
            Require(Geometry.Matches(best).Any(m => Regex.IsMatch(m.Value, @"\.\d{4}(?!\d)")), "Best Fit uses the requested four-digit geometry precision without shortening");
            Require(Tag(best, "frx") != 0 || Tag(best, "fry") != 0, "Best Fit can use perspective axes despite saved Fax + Frz Only");
            Undo(); ClickToolbar(Fit);
            Require(!ToolbarState(Fit).Checked, "Fit Text toggles off");
            var preserve = Apply("preserve-scale");
            Require(Tag(preserve, "fscx") == 125 && Tag(preserve, "fscy") == 85, "Fit Text off preserves both source scales");
            Undo(); ClickToolbar(Fit); Save("reset"); Close();
            var config = ReadConfig();
            Require(config["Tool"]!["Visual"]!["Perspective"]!["Best Fit"]!.GetValue<bool>(), "Best Fit persists true on clean shutdown");
            var perspective = config["Tool"]!["Visual"]!["Perspective"]!;
            Require(perspective["Fax Frz Only"]!.GetValue<bool>() && perspective["Shape Tolerance"]!.GetValue<double>() == 100, "Ignored saved preferences are not overwritten");
            perspective["Fax Frz Only"] = false; perspective["Shape Tolerance"] = 0.001; WriteConfig(config);
            Start(); Require(ToolbarState(Best).Checked, "Best Fit restores checked after restart");
            var independent = Apply("ignored-options");
            Require(best == independent, "Best Fit output ignores Fax + Frz Only and Shape Tolerance");
            Close();
            config = ReadConfig(); config["Tool"]!["Visual"]!["Perspective"]!["Decimal Places"] = 2; WriteConfig(config);
            Start(); var rounded = Apply("two-decimals");
            var decimals = Geometry.Matches(rounded).SelectMany(m => Regex.Matches(m.Value, @"[-+]?\d+(?:\.(\d+))?").Cast<Match>()).Select(m => m.Groups[1].Value.Length).ToArray();
            Require(decimals.Length >= 6 && decimals.Max() <= 2, "All emitted geometry uses at most two decimal places");
            Require(rounded != best, "Decimal Places changes generated output");
            Close();
            config = ReadConfig(); config["Tool"]!["Visual"]!["Perspective"]!["Decimal Places"] = 0; WriteConfig(config);
            source = Source.Replace(@"\fax0.15", @"\fax0");
            Start(); var integers = Apply("zero-decimals");
            Require(!Geometry.Matches(integers).Any(m => m.Value.Contains('.')), "Zero decimal places remains applicable and writes integers");
            Close();
            source = Source.Replace("Style: Default,Arial,30", "Style: Default,@Microsoft YaHei,30")
                .Replace("0,0,0,0,100,100,0,0,1,2,0,7", "0,0,0,0,100,100,0,12.34,1,2,0,7")
                .Replace(@"\frz12\fax0.15", "").Replace(@"Best fit\NGeometry", "竖排字幕透视拟合验证");
            File.WriteAllText(Path.Combine(artifacts, "vertical-input.ass"), source, new UTF8Encoding(false));
            Start(); var vertical = Apply("vertical-zero-decimals");
            Require(!Regex.IsMatch(vertical, @"\\fr(?:z)?[-+\d.]"), "Vertical font keeps fractional style rotation inherited at zero decimals");
            Close();
            File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { passed = true, checks, target = new[] { new[] { 140, 150 }, new[] { 510, 180 }, new[] { 465, 325 }, new[] { 180, 285 } }, timeout_seconds = 180 }, Json));
            return 0;
        }
        catch (Exception error)
        {
            if (app is { HasExited: false })
                File.WriteAllText(Path.Combine(artifacts, "top-windows.json"), JsonSerializer.Serialize(isolation.Windows(app).Select(h => new { handle = $"{h:X}", title = isolation.ReadText(h, app), class_name = PrivateDesktop.ClassName(h), visible = Native.IsWindowVisible(h), enabled = Native.IsWindowEnabled(h), children = Native.Children(h).Select(c => isolation.ReadText(c, app)).ToArray() }), Json));
            if (main != 0 && app is { HasExited: false })
                try { ScreenCapture.SaveWindowPng(main, Path.Combine(artifacts, "failure.png")); Dump(main, "failure-uia"); } catch { }
            File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { passed = false, checks, error = error.Message }, Json));
            Console.Error.WriteLine(error); return 1;
        }
        finally { if (app is not null) { isolation.Stop(app); app.Dispose(); } }
    }

    void Profile()
    {
        Directory.CreateDirectory(Path.Combine(profile, "user"));
        WriteConfig(JsonNode.Parse("""
            { "App": { "Auto": { "Check For Updates": false, "Load Linked Files": 0 } },
              "Subtitle": { "Provider": "libass" },
              "Video": { "Open Audio": false, "Scale with DPI": false, "Default Zoom": 7, "Renderer": { "Backend": "opengl" }, "Skia Tools": { "Enabled": false } },
              "Tool": { "Visual": { "Autohide": false, "Perspective": { "Fit Text": true, "Fax Frz Only": true, "Shape Tolerance": 100, "Decimal Places": 4 } } } }
            """)!);
        var menu = JsonNode.Parse(File.ReadAllText(Path.Combine("src", "libresrc", "default_menu.json")))!;
        menu["main"]!.AsArray().Add(new JsonObject { ["submenu"] = "best-fit-test", ["text"] = "Best Fit Test" });
        var commands = new JsonArray();
        foreach (var (command, label) in new[] { ("video/tool/measure", "Measure"), ("video/tool/measure/perspective", "Perspective"), ("subtitle/save", "Save"), ("edit/undo", "Undo"), ("app/options", "Preferences"), ("video/frame/save", "Save Frame") })
            commands.Add(new JsonObject { ["command"] = command, ["text"] = label });
        menu["best-fit-test"] = commands;
        File.WriteAllText(Path.Combine(profile, "user", "menu.json"), menu.ToJsonString());
    }
    JsonNode ReadConfig() => JsonNode.Parse(File.ReadAllText(Path.Combine(profile, "user", "config.json")))!;
    void WriteConfig(JsonNode config) => File.WriteAllText(Path.Combine(profile, "user", "config.json"), config.ToJsonString(Json));
    void Start()
    {
        File.WriteAllText(fixture, source, new UTF8Encoding(false));
        var host = Path.Combine(artifacts, "host-" + ++launch); Directory.CreateDirectory(host);
        var start = new ProcessStartInfo(exe) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        start.Environment["AEGISUB_ENABLE_SKIA_VIDEO_TOOLS"] = "0";
        start.Environment["AEGISUB_AUDIO_TILE_DIAGNOSTICS"] = "1";
        foreach (var arg in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", host, "--open", fixture, "--open", video }) start.ArgumentList.Add(arg);
        app = isolation.Start(start, "host-" + launch);
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(host, "ready.json"), app, TimeSpan.FromSeconds(20));
        main = isolation.MainWindow(app, 20); isolation.VerifyWindow(main, app);
        Native.ShowWindow(main, 4); Native.SetWindowPos(main, 0, 20, 20, 1400, 950, 0x0050); Thread.Sleep(500);
        Wait(() => { canvas = Native.Children(main).FirstOrDefault(h => PrivateDesktop.ClassName(h) == "wxGLCanvas" && Native.IsWindowVisible(h)); return canvas != 0; }, "video canvas");
        Native.GetClientRect(canvas, out var observed); Console.WriteLine($"canvas={canvas:X} dimensions={observed.Right}x{observed.Bottom} main_enabled={Native.IsWindowEnabled(main)}");
        Wait(() => Native.GetClientRect(canvas, out var size) && size.Right == 640 && size.Bottom == 480 && Native.IsWindowEnabled(main), "640x480 video canvas at 100% zoom");
        Thread.Sleep(1000);
        SelectTool("Measure with temporary guides");
        Wait(() => { subToolbar = Elements(main).Where(e => e.Current.ControlType == ControlType.ToolBar).Select(e => new nint(e.Current.NativeWindowHandle)).FirstOrDefault(h => Native.Buttons(h, app!.Id).Count(b => (b.Style & 1) == 0) == 3); return subToolbar != 0; }, "Measure subtoolbar");
        SelectTool("Draw a target quadrilateral");
        Wait(() => Native.Buttons(subToolbar, app!.Id).Count(b => (b.Style & 1) == 0) == 8, "Perspective toolbar");
        File.WriteAllText(Path.Combine(artifacts, "native-toolbar-" + launch + ".json"), JsonSerializer.Serialize(Native.Buttons(subToolbar, app!.Id), Json));
    }
    void Close()
    {
        Native.PostMessage(main, 0x0010, 0, 0);
        if (!app!.WaitForExit(10_000)) throw new TimeoutException("Host failed to close within ten seconds");
        Require(isolation.ExitCode(app) == 0, "Clean host exit"); app.Dispose(); app = null; main = canvas = 0;
    }
    void Wait(Func<bool> predicate, string name)
    {
        var deadline = Stopwatch.GetTimestamp() + 15 * Stopwatch.Frequency;
        while (Stopwatch.GetTimestamp() < deadline)
        {
            if (app is { HasExited: true }) throw new InvalidOperationException("Host exited while waiting for " + name);
            isolation.VerifyCurrent(); if (predicate()) return; Thread.Sleep(50);
        }
        if (subToolbar != 0) File.WriteAllText(Path.Combine(artifacts, "failed-toolbar.json"), JsonSerializer.Serialize(Native.Buttons(subToolbar, app!.Id), Json));
        throw new TimeoutException("Timed out waiting for " + name);
    }
    void Require(bool value, string description)
    {
        checks.Add(new { description, passed = value }); Console.WriteLine(description + ": " + value);
        if (!value) throw new InvalidOperationException(description);
    }
    void Command(string label)
    {
        isolation.VerifyWindow(main, app!);
        var id = ObservedWindowsCommand.FindMenuCommand(main, app!.Id, "Best Fit Test", label);
        ObservedWindowsCommand.PostMenuCommand(main, app.Id, id); Thread.Sleep(200);
    }
    AutomationElement[] Elements(nint hwnd) => AutomationElement.FromHandle(hwnd).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
    (AutomationElement Element, nint Toolbar, int Id) Toolbar(string tooltip)
    {
        var target = Elements(main).SingleOrDefault(e => e.Current.ControlType == ControlType.Button && e.Current.Name.Contains(tooltip, StringComparison.OrdinalIgnoreCase));
        if (target is not null)
        {
            var id = int.Parse(Regex.Match(target.Current.AutomationId, @"(\d+)$").Value, CultureInfo.InvariantCulture);
            for (var parent = TreeWalker.RawViewWalker.GetParent(target); parent is not null; parent = TreeWalker.RawViewWalker.GetParent(parent))
                if (parent.Current.ControlType == ControlType.ToolBar && parent.Current.NativeWindowHandle != 0)
                    return (target, new nint(parent.Current.NativeWindowHandle), id);
        }
        var buttons = Native.Buttons(subToolbar, app!.Id).Where(b => (b.Style & 1) == 0).ToArray();
        var matching = buttons.Select((button, index) => (button, index)).Where(item => item.button.Text.Contains(tooltip, StringComparison.Ordinal)).ToArray();
        var index = matching.Length == 1 ? matching[0].index : tooltip switch
        {
            "Draw a target quadrilateral" when buttons.Length == 3 => 1,
            "Apply Perspective tags" when buttons.Length == 8 => 6,
            _ => throw new InvalidOperationException("Observed native tooltip was missing or ambiguous: " + tooltip)
        };        var children = Elements(subToolbar).Where(e => e.Current.ControlType == ControlType.Button).ToArray();
        if (children.Length != buttons.Length || index >= buttons.Length) throw new InvalidOperationException("Native and UIA subtoolbar button counts differ");
        return (children[index], subToolbar, buttons[index].Id);
    }    (bool Checked, bool Enabled) ToolbarState(string tooltip)
    {
        var target = Toolbar(tooltip); var state = Native.Send(target.Toolbar, 0x0412, target.Id, 0).ToInt64();
        if (state < 0) throw new InvalidOperationException("Toolbar command disappeared");
        return ((state & 1) != 0, (state & 4) != 0);
    }
    void SelectTool(string tooltip) => ClickToolbar(tooltip);
    void ClickToolbar(string tooltip)
    {
        var target = Toolbar(tooltip);
        if ((Native.Send(target.Toolbar, 0x0412, target.Id, 0).ToInt64() & 4) == 0) throw new InvalidOperationException("Toolbar is disabled: " + tooltip);
        var rect = target.Element.Current.BoundingRectangle;
        var point = new Point { X = (int)(rect.Left + rect.Width / 2), Y = (int)(rect.Top + rect.Height / 2) };
        Native.ScreenToClient(target.Toolbar, ref point);
        Console.WriteLine($"click_toolbar={target.Element.Current.Name} id={target.Id} rect={rect} point={point.X},{point.Y} before={Native.Send(target.Toolbar, 0x0412, target.Id, 0)}");
        Click(target.Toolbar, point.X, point.Y); Thread.Sleep(150);
        Console.WriteLine($"toolbar_after={Native.Send(target.Toolbar, 0x0412, target.Id, 0)}");
    }
    void Click(nint hwnd, int x, int y)
    {
        isolation.VerifyWindow(hwnd, app!); Native.Mouse(hwnd, 0x0200, x, y, 0); Native.Mouse(hwnd, 0x0201, x, y, 1); Native.Mouse(hwnd, 0x0202, x, y, 0);
    }
    void Drag(int x1, int y1, int x2, int y2)
    {
        isolation.VerifyWindow(canvas, app!);
        Native.Mouse(canvas, 0x0200, x1, y1, 0); Native.Mouse(canvas, 0x0201, x1, y1, 1);
        Native.Mouse(canvas, 0x0200, (x1 + x2) / 2, (y1 + y2) / 2, 1); Native.Mouse(canvas, 0x0200, x2, y2, 1); Native.Mouse(canvas, 0x0202, x2, y2, 0);
        var observed = Native.Buttons(subToolbar, app!.Id);
        var apply = observed.Where(b => (b.Style & 1) == 0).ElementAt(6);
        File.AppendAllText(Path.Combine(artifacts, "gestures.ndjson"), JsonSerializer.Serialize(new { launch, x1, y1, x2, y2, target_valid = apply.Text == "Apply Perspective tags", buttons = observed }) + Environment.NewLine);
        Require(apply.Text == "Apply Perspective tags" && (apply.State & 4) != 0, $"Gesture ({x1},{y1}) to ({x2},{y2}) leaves a valid applicable target");
    }
    string Apply(string label)
    {
        if (ToolbarState("Discard the target quadrilateral").Enabled) ClickToolbar("Discard the target quadrilateral");
        Drag(150, 140, 500, 320); Drag(150, 140, 140, 150); Drag(500, 140, 510, 180); Drag(500, 320, 465, 325); Drag(150, 320, 180, 285);
        Wait(() => ToolbarState("Apply Perspective tags").Enabled, label + " Apply enabled");
        ScreenCapture.SaveWindowPng(main, Path.Combine(artifacts, label + "-target.png"));
        ClickToolbar("Apply Perspective tags");
        Wait(() => isolation.ReadText(main, app!).TrimStart().StartsWith("*", StringComparison.Ordinal), label + " modified subtitles");
        var text = Save(label);
        if (label is "best-fit" or "vertical-zero-decimals") ExportFrame(label);
        ScreenCapture.SaveWindowPng(main, Path.Combine(artifacts, label + ".png"));
        var original = source.Split('\n').Where(s => s.StartsWith("Dialogue:")).Select(s => s.Trim()).ToArray();
        var actual = File.ReadLines(fixture).Where(s => s.StartsWith("Dialogue:")).ToArray();
        Require(actual.Length == 2 && actual[1] == original[1], label + " leaves unselected line unchanged");
        Require(Geometry.Replace(actual[0], "") == Geometry.Replace(original[0], ""), label + " preserves text, unrelated tags, and event metadata");
        Require(File.ReadLines(fixture).Where(s => s.StartsWith("Style:")).SequenceEqual(source.Split('\n').Where(s => s.StartsWith("Style:")).Select(s => s.Trim())), label + " preserves both styles");
        return text;
    }
    string Save(string label)
    {
        Command("Save"); Wait(() => !isolation.ReadText(main, app!).TrimStart().StartsWith("*", StringComparison.Ordinal), "saved subtitles");
        File.Copy(fixture, Path.Combine(artifacts, label + ".ass"), true);
        return File.ReadLines(fixture).First(s => s.StartsWith("Dialogue:"));
    }
    void ExportFrame(string label)
    {
        Command("Save Frame"); nint dialog = 0;
        Wait(() => { dialog = isolation.Windows(app!).FirstOrDefault(h => isolation.ReadText(h, app!) == "Save PNG snapshot"); return dialog != 0 && Native.IsWindowVisible(dialog); }, "Save PNG snapshot dialog");
        var edit = Native.Children(dialog).Single(h => PrivateDesktop.ClassName(h) == "Edit" && Native.IsWindowVisible(h) && isolation.ReadText(h, app!).EndsWith(".png", StringComparison.OrdinalIgnoreCase));
        var destination = Path.Combine(artifacts, label + "-frame.png");
        isolation.VerifyWindow(edit, app!); Native.Text(edit, destination);
        Require(isolation.ReadText(edit, app!) == destination, label + " snapshot filename entered");
        var button = Native.Children(dialog).Single(h => PrivateDesktop.ClassName(h) == "Button" && Native.IsWindowVisible(h)
            && Regex.Replace(isolation.ReadText(h, app!).Replace("&", ""), @"\([A-Za-z]\)$", "") is "Save" or "保存");
        ObservedWindowsCommand.PostButtonClick(button, app!.Id);
        Wait(() => !isolation.Windows(app!).Contains(dialog) && PngComplete(destination), "exported subtitle frame");
        ScreenCapture.ValidateSavedPng(destination, 640, 480);
    }
    static bool PngComplete(string path)
    {
        try
        {
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            if (stream.Length < 24) return false;
            stream.Seek(-12, SeekOrigin.End); var tail = new byte[12]; stream.ReadExactly(tail);
            return tail.SequenceEqual(new byte[] { 0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130 });
        }
        catch (IOException) { return false; }
    }
    void Undo()
    {
        Command("Undo"); var text = Save("undo-" + checks.Count);
        Require(text == source.Split('\n').First(s => s.StartsWith("Dialogue:")).Trim(), "Undo restores original active dialogue");
    }
    static double Tag(string text, string name)
    {
        var match = Regex.Match(text, @"\\" + name + @"([-+]?\d+(?:\.\d+)?)");
        return match.Success ? double.Parse(match.Groups[1].Value, CultureInfo.InvariantCulture) : 0;
    }
    void Dump(nint hwnd, string name) => File.WriteAllText(Path.Combine(artifacts, name + ".json"), JsonSerializer.Serialize(Elements(hwnd).Select(e => new { name = e.Current.Name, type = e.Current.ControlType.ProgrammaticName, id = e.Current.AutomationId, hwnd = e.Current.NativeWindowHandle, rect = e.Current.BoundingRectangle.ToString(), value = UiaDriver.ReadElementState(e).Value, patterns = e.GetSupportedPatterns().Select(p => p.ProgrammaticName).ToArray() }), Json));
    int[] CheckedRows(nint grid, string label)
    {
        var path = Path.Combine(artifacts, label + ".png"); ScreenCapture.SaveWindowPng(grid, path);
        using var bitmap = new System.Drawing.Bitmap(path); var rows = new List<int>();
        for (var y = 0; y < Math.Min(400, bitmap.Height); ++y)
        {
            var blue = 0;
            for (var x = bitmap.Width / 2 + 4; x < bitmap.Width / 2 + 28; ++x)
            {
                var pixel = bitmap.GetPixel(x, y);
                if (pixel.R < 80 && pixel.G < 160 && pixel.B > 150) ++blue;
            }
            if (blue >= 8) rows.Add(y);
        }
        var groups = new List<List<int>>();
        foreach (var y in rows)
        {
            if (groups.Count == 0 || y - groups[^1][^1] > 3) groups.Add([]);
            groups[^1].Add(y);
        }
        return groups.Where(g => g.Count >= 8).Select(g => (g[0] + g[^1]) / 2).ToArray();
    }
    void PreferencesRoundTrip()
    {
        Command("Preferences"); nint dialog = 0;
        Wait(() => { dialog = isolation.Windows(app!).FirstOrDefault(h => PrivateDesktop.Text(h) == "Preferences"); return dialog != 0; }, "Preferences dialog");
        var tree = Elements(dialog).Single(e => e.Current.ControlType == ControlType.TreeItem && e.Current.Name == "Visual Tools");
        ((SelectionItemPattern)tree.GetCurrentPattern(SelectionItemPattern.Pattern)).Select(); Thread.Sleep(200);
        Dump(dialog, "preferences-uia"); ScreenCapture.SaveWindowPng(dialog, Path.Combine(artifacts, "preferences-on.png"));
        var grid = new nint(Elements(dialog).Single(e => e.Current.Name == "wxPropertyGrid" && e.Current.NativeWindowHandle != 0).Current.NativeWindowHandle);
        var before = CheckedRows(grid, "preferences-grid-on");
        Require(before.Length == 3, "Preferences displays Fit Text, Best Fit and stored Fax + Frz Only checked");
        Native.GetClientRect(grid, out var gridRect); Click(grid, gridRect.Right / 2 + 15, before[1]);
        var editor = Native.Children(grid).Single(h => Native.IsWindowVisible(h) && isolation.ReadText(h, app!) == "control");
        isolation.VerifyWindow(editor, app!); Native.Send(editor, 0x0100, 0x20, 0x00390001); Native.Send(editor, 0x0101, 0x20, unchecked((nint)0xC0390001));
        Thread.Sleep(100);
        ScreenCapture.SaveWindowPng(dialog, Path.Combine(artifacts, "preferences-off.png"));
        Require(CheckedRows(grid, "preferences-grid-off").Length == 2, "Preferences Best Fit checkbox changes off while other saved checks remain");        var ok = UiaDriver.FindEnabledInvokableButton(AutomationElement.FromHandle(dialog), "OK") ?? throw new InvalidOperationException("Preferences OK absent"); var okHandle = new nint(ok.Current.NativeWindowHandle); isolation.VerifyWindow(okHandle, app!); Native.PostMessage(okHandle, 0x00F5, 0, 0);
        Wait(() => Native.IsWindowEnabled(main), "Preferences closed");
        Require(!ToolbarState(Best).Checked && ToolbarState(Fax).Enabled, "Preferences changes synchronize back to toolbar");
        ClickToolbar(Best);
    }
}

sealed record NativeButton(int Index, int Id, int State, int Style, string Text);
[StructLayout(LayoutKind.Sequential)] struct Point { public int X, Y; }
[StructLayout(LayoutKind.Sequential)] struct Rect { public int Left, Top, Right, Bottom; }
static class Native
{
    [DllImport("kernel32.dll")] static extern nint OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern nint VirtualAllocEx(nint process, nint address, nuint size, uint type, uint protection);
    [DllImport("kernel32.dll")] static extern bool VirtualFreeEx(nint process, nint address, nuint size, uint type);
    [DllImport("kernel32.dll")] static extern bool WriteProcessMemory(nint process, nint address, byte[] buffer, nuint size, out nuint written);
    [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(nint process, nint address, byte[] buffer, nuint size, out nuint read);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(nint handle);
    public static List<NativeButton> Buttons(nint toolbar, int pid)
    {
        if (toolbar == 0) return [];
        var count = checked((int)Send(toolbar, 0x0418, 0, 0));
        if (count < 0 || count > 100) throw new InvalidOperationException("Invalid native toolbar size");
        var process = OpenProcess(0x0038, false, pid);
        if (process == 0) throw new InvalidOperationException("Cannot allocate control-message buffer in owned host");
        var buffer = VirtualAllocEx(process, 0, 1024, 0x3000, 4);
        try
        {
            if (buffer == 0) throw new InvalidOperationException("Control-message allocation failed");
            var result = new List<NativeButton>(); var bytes = new byte[1024];
            for (var index = 0; index < count; ++index)
            {
                if (Send(toolbar, 0x0417, index, buffer) == 0 || !ReadProcessMemory(process, buffer, bytes, 32, out var read) || read != 32)
                    throw new InvalidOperationException("Cannot read observed native toolbar button");
                var id = BitConverter.ToInt32(bytes, 4); var state = bytes[8]; var style = bytes[9];
                var length = (style & 1) == 0 ? checked((int)Send(toolbar, 0x044B, id, buffer)) : -1;
                var text = "";
                if (length >= 0)
                {
                    if (length >= 510 || !ReadProcessMemory(process, buffer, bytes, (nuint)((length + 1) * 2), out read)) throw new InvalidOperationException("Cannot read native toolbar text");
                    text = Encoding.Unicode.GetString(bytes, 0, length * 2);
                }
                                if (text.Length == 0 && (style & 1) == 0)
                {
                    var tooltip = Send(toolbar, 0x0423, 0, 0);
                    for (var toolIndex = 0; toolIndex < checked((int)Send(tooltip, 0x040D, 0, 0)); ++toolIndex)
                    {
                        var info = new byte[72]; BitConverter.GetBytes(72).CopyTo(info, 0);
                        if (!WriteProcessMemory(process, buffer, info, 72, out _)) throw new InvalidOperationException("Cannot write tooltip enumeration buffer");
                        if (Send(tooltip, 0x043A, toolIndex, buffer) == 0 || !ReadProcessMemory(process, buffer, info, 72, out _)) throw new InvalidOperationException("Cannot enumerate native tooltip");
                        if (unchecked((int)BitConverter.ToInt64(info, 16)) != id) continue;
                        BitConverter.GetBytes((buffer + 128).ToInt64()).CopyTo(info, 48);
                        if (!WriteProcessMemory(process, buffer, info, 72, out _)) throw new InvalidOperationException("Cannot write tooltip query buffer");
                        Send(tooltip, 0x0438, 384, buffer);
                        if (ReadProcessMemory(process, buffer + 128, bytes, 768, out _)) text = Encoding.Unicode.GetString(bytes, 0, 768).Split('\0')[0];
                        break;
                    }
                }                result.Add(new NativeButton(index, id, state, style, text));
            }
            return result;
        }
        finally { if (buffer != 0) VirtualFreeEx(process, buffer, 0, 0x8000); CloseHandle(process); }
    }
    [DllImport("user32.dll")] public static extern nint SetThreadDpiAwarenessContext(nint context);
    [DllImport("user32.dll")] public static extern bool ShowWindow(nint hwnd, int command);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(nint hwnd, nint after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] public static extern bool GetClientRect(nint hwnd, out Rect rect);
    [DllImport("user32.dll")] public static extern bool ScreenToClient(nint hwnd, ref Point point);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(nint hwnd);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(nint hwnd);
    [DllImport("user32.dll")] public static extern bool PostMessage(nint hwnd, uint message, nint wParam, nint lParam);
    [DllImport("user32.dll")] static extern nint SendMessageTimeout(nint hwnd, uint message, nint wParam, nint lParam, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(nint parent, Callback callback, nint parameter);
    delegate bool Callback(nint hwnd, nint parameter);
    public static nint[] Children(nint parent) { var result = new List<nint>(); EnumChildWindows(parent, (h, _) => { result.Add(h); return true; }, 0); return result.ToArray(); }
    public static nint Send(nint hwnd, uint message, nint wParam, nint lParam)
    {
        if (SendMessageTimeout(hwnd, message, wParam, lParam, 3, 5000, out var result) == 0) throw new TimeoutException("Native window message did not complete");
        return result;
    }
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint SendText(nint hwnd, uint message, nint wParam, string text, uint flags, uint timeout, out nint result);
    public static void Text(nint hwnd, string text) { if (SendText(hwnd, 0x000C, 0, text, 3, 5000, out var result) == 0 || result == 0) throw new InvalidOperationException("Observed edit rejected text"); }
    public static void Mouse(nint hwnd, uint message, int x, int y, int state) => Send(hwnd, message, state, unchecked((nint)((y << 16) | (x & 0xffff))));
}
