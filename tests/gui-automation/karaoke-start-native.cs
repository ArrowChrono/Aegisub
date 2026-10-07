#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

if (args.Length != 3 || args[2] is not ("rejected" or "restart"))
{
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/karaoke-start-native.cs -- EXE ARTIFACTS rejected|restart");
    return 2;
}
try { return PrivateDesktop.Run(args, Path.GetFullPath(args[1]), isolation => new Scenario(args, isolation).Run()); }
catch (Exception error) { Console.Error.WriteLine(error); return 1; }

sealed class Scenario(string[] args, PrivateDesktop isolation)
{
    readonly string exe = Path.GetFullPath(args[0]), artifacts = Path.GetFullPath(args[1]);
    readonly List<object> checks = [];
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    Process? app;
    nint main, dialog;
    int undoCommand, redoCommand;
    string[] baseline = [];

    public int Run()
    {
        Directory.CreateDirectory(artifacts); isolation.RecordDriver();
        using var log = new StreamWriter(Path.Combine(artifacts, "driver.log")) { AutoFlush = true };
        Console.SetOut(log); Console.SetError(log);
        using var watchdog = new Timer(_ => { try { if (app is not null) isolation.Stop(app); } finally { Environment.Exit(124); } }, null, 180_000, Timeout.Infinite);
        File.Copy("tests/gui-automation/karaoke-start-native.cs", Path.Combine(artifacts, "producer.cs"));
        File.WriteAllText(Path.Combine(artifacts, "inputs.json"), JsonSerializer.Serialize(new {
            executable_sha256 = Hash(exe), scenario = args[2], sources = new[] { "src/dialog_kara_timing_copy.cpp", "src/command/tool.cpp" }.Select(path => new { path, sha256 = Hash(path) }),
            private_desktop = true, global_input = false, clipboard = false, total_timeout_seconds = 180
        }, Json));
        try
        {
            Start();
            if (args[2] == "rejected") RejectedStart(); else SuccessfulRestart();
            File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { passed = true, checks }, Json));
            return 0;
        }
        catch (Exception error)
        {
            if (app is not null && !app.HasExited) Dump("failure");
            File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { passed = false, checks, error = error.ToString() }, Json));
            Console.Error.WriteLine(error); return 1;
        }
        finally { if (app is not null) { isolation.Stop(app); app.Dispose(); } }
    }

    static string Hash(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
    void Require(bool value, string description)
    {
        checks.Add(new { description, passed = value }); Console.WriteLine(description + ": " + value);
        if (!value) throw new InvalidOperationException(description);
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
    nint Control(nint parent, string label) => Children(parent).Single(h => Text(h).Replace("&", "") == label);
    long Send(nint window, uint message, long wp = 0, long lp = 0)
    {
        isolation.VerifyWindow(window, app!); return Native.Send(window, message, wp, lp);
    }
    void Action(string action, string target)
        => File.AppendAllText(Path.Combine(artifacts, "actions.ndjson"), JsonSerializer.Serialize(new { action, target }) + Environment.NewLine);
    void Click(nint parent, string label)
    {
        var control = Control(parent, label); isolation.VerifyWindow(control, app!);
        Action("button", label); ObservedWindowsCommand.PostButtonClick(control, app!.Id); Thread.Sleep(180);
    }
    void Click(string label) => Click(dialog, label);
    void Command(string menu, string label)
    {
        isolation.VerifyWindow(main, app!);
        var id = ObservedWindowsCommand.FindMenuCommand(main, app!.Id, menu, label);
        Action("menu", menu + "/" + label); ObservedWindowsCommand.PostMenuCommand(main, app.Id, id); Thread.Sleep(180);
    }
    void Dump(string label)
    {
        File.WriteAllText(Path.Combine(artifacts, label + "-windows.json"), JsonSerializer.Serialize(Windows().Select(h => new {
            handle = h.ToString("X"), text = Text(h), class_name = PrivateDesktop.ClassName(h),
            children = Children(h).Select(c => new { handle = c.ToString("X"), text = Text(c), class_name = PrivateDesktop.ClassName(c), id = Native.GetDlgCtrlID(c) })
        }), Json));
    }
    void Capture(nint window, string label)
    {
        isolation.VerifyWindow(window, app!); Dump(label);
        ScreenCapture.SaveWindowPng(window, Path.Combine(artifacts, label + ".png"));
    }
    void OpenTimer()
    {
        Command("Timing", "Kanji Timer...");
        Wait(() => { dialog = Windows().SingleOrDefault(h => Text(h) == "Kanji timing"); return dialog != 0; }, "Native Kanji timing dialog opened through default Timing menu");
        var interpolate = Control(dialog, "Attempt to interpolate kanji.");
        if (Send(interpolate, 0xF0) != 0) { Native.Post(interpolate, 0xF5, 0, 0); Wait(() => Send(interpolate, 0xF0) == 0, "Interpolation unchecked by native checkbox click"); }
        Require(Send(interpolate, 0xF0) == 0, "Manual Link matching uses the actual unchecked interpolation control");
    }
    void Style(int ordinal, string value)
    {
        var combos = Children(dialog).Where(h => PrivateDesktop.ClassName(h) == "ComboBox").OrderBy(Native.Top).ToArray();
        Require(combos.Length == 2, "Both native read-only style controls observed");
        var combo = combos[ordinal]; isolation.VerifyWindow(combo, app!);
        var index = Native.FindString(combo, value);
        Require(index >= 0, "Style exists in native dropdown: " + value);
        Action("combo-keyboard-selection", (ordinal == 0 ? "Source:" : "Destination:") + value);
        Key(combo, 0x24);
        for (var i = 0; i < index; ++i) Key(combo, 0x28);
        Wait(() => Text(combo) == value, "Native style selection: " + value);
    }
    void Key(nint control, int key)
    {
        Send(control, 0x100, key, 1); Send(control, 0x101, key, unchecked((int)0xC0000001));
    }
    void Warning(string message, string label)
    {
        nint warning = 0;
        Wait(() => { warning = Windows().SingleOrDefault(h => h != main && h != dialog && Text(h) == "Error"); return warning != 0; }, "Native Start warning appeared");
        var elements = AutomationElement.FromHandle(warning).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>()
            .Select(e => new { name = e.Current.Name, control_type = e.Current.ControlType.ProgrammaticName, automation_id = e.Current.AutomationId }).ToArray();
        File.WriteAllText(Path.Combine(artifacts, label + "-uia.json"), JsonSerializer.Serialize(elements, Json));
        Capture(warning, label);
        Require(elements.Any(e => e.name == message), "Start warning text: " + message);
        var ok = Control(warning, "OK"); isolation.VerifyWindow(ok, app!); Action("button", "Warning/OK"); Native.Post(ok, 0xF5, 0, 0);
        Wait(() => !Windows().Contains(warning), "User accepted the native warning");
    }
    void StageTwoRows()
    {
        Style(0, "Source"); Style(1, "Destination"); Click("Start!");
        for (var row = 1; row <= 2; ++row)
        {
            for (var syllable = 0; syllable < 3; ++syllable) Click("Link");
            Capture(dialog, "linked-row-" + row); Click("Accept Line");
            Require(!Windows().Any(h => Text(h) == "Error"), "All three source syllables linked before Accept Line " + row);
            Require(!Text(main).StartsWith('*'), "Accept Line " + row + " remains staged until Close");
        }
        Capture(dialog, "two-rows-staged");
    }
    void CloseTimer()
    {
        var old = dialog; Click("Close");
        Wait(() => !Windows().Contains(old), "Native Close completed"); dialog = 0;
    }
    string[] SaveAs(string label)
    {
        Command("File", "Save Subtitles as..."); nint save = 0;
        Wait(() => { save = Windows().SingleOrDefault(h => PrivateDesktop.ClassName(h) == "#32770" && Text(h).Contains("Save")); return save != 0; }, "Native Save As dialog");
        var elements = AutomationElement.FromHandle(save).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
        var name = elements.Single(e => e.Current.ControlType == ControlType.Edit && e.Current.AutomationId == "1001");
        var path = Path.Combine(artifacts, label + ".ass");
        var handle = (nint)name.Current.NativeWindowHandle; isolation.VerifyWindow(handle, app!); Native.SetText(handle, path);
        var button = elements.Single(e => e.Current.ControlType == ControlType.Button && e.Current.AutomationId == "1" && e.Current.NativeWindowHandle != 0);
        isolation.VerifyWindow((nint)button.Current.NativeWindowHandle, app!);
        ObservedWindowsCommand.PostButtonClick((nint)button.Current.NativeWindowHandle, app!.Id);
        Wait(() => File.Exists(path) && !Windows().Contains(save), "Native Save As completed: " + label);
        Require(!Text(main).StartsWith('*'), "Save As cleared dirty state: " + label);
        return File.ReadAllLines(path);
    }
    string[] Expected(params (string Actor, string Text)[] changes)
    {
        var expected = baseline.ToArray();
        foreach (var (actor, text) in changes)
        {
            var row = Enumerable.Range(0, expected.Length).Single(i => expected[i].StartsWith("Dialogue:") && expected[i].Split(',', 10)[4] == actor);
            var fields = expected[row].Split(',', 10); fields[9] = text; expected[row] = string.Join(',', fields);
        }
        File.WriteAllLines(Path.Combine(artifacts, "expected.ass"), expected, new UTF8Encoding(true));
        return expected;
    }
    void History(string[] expected)
    {
        Action("menu", "Edit/Undo"); ObservedWindowsCommand.PostMenuCommand(main, app!.Id, undoCommand); Thread.Sleep(180);
        Require(SaveAs("undo").SequenceEqual(baseline), "One Undo restores the entire baseline ASS including every metadata and event field");
        Action("menu", "Edit/Redo"); ObservedWindowsCommand.PostMenuCommand(main, app.Id, redoCommand); Thread.Sleep(180);
        Require(SaveAs("redo").SequenceEqual(expected), "One Redo restores precisely the complete committed ASS");
    }
    void RejectedStart()
    {
        OpenTimer(); Click("Start!"); Warning("Select source and destination styles first.", "missing-style-warning");
        Require(!Text(main).StartsWith('*'), "Missing initial style selection does not dirty the document");
        CloseTimer(); Require(SaveAs("after-missing-style").SequenceEqual(baseline), "Missing initial styles followed by Close preserve the entire ASS");
        OpenTimer(); StageTwoRows();
        Style(1, "Source"); Click("Start!"); Warning("The source and destination styles must be different.", "same-style-warning");
        Require(!Text(main).StartsWith('*'), "Rejected same-style Start does not prematurely commit staged rows");
        Capture(dialog, "after-rejected-start"); CloseTimer();
        var dirty = Text(main).StartsWith('*');
        var actual = SaveAs("after-close");
        var expected = Expected(("dest-one", "{\\k12}漢{\\k23}字{\\k34}語"), ("dest-two", "{\\k21}春{\\k32}夏{\\k43}秋"));
        Require(actual.SequenceEqual(expected), "Rejected same-style Start preserves both staged rows for Close and every other ASS field remains unchanged");
        Require(dirty, "Close publishes preserved staged rows as a document change");
        History(expected);
    }
    void SuccessfulRestart()
    {
        OpenTimer(); StageTwoRows();
        Style(0, "Alternate Source"); Style(1, "Alternate Destination"); Click("Start!");
        for (var syllable = 0; syllable < 3; ++syllable) Click("Link");
        Capture(dialog, "restart-linked"); Click("Accept Line");
        Require(!Windows().Any(h => Text(h) == "Error"), "Successful new Start permits a complete alternate pair");
        Require(!Text(main).StartsWith('*'), "New Start and alternate Accept remain staged"); CloseTimer();
        var actual = SaveAs("after-close");
        var expected = Expected(("alternate-dest", "{\\k15}山{\\k25}川{\\k35}海"));
        Require(actual.SequenceEqual(expected), "Successful new Start resets both earlier staged rows and commits only the alternate pair, preserving the complete remaining ASS");
        History(expected);
    }
    void Start()
    {
        var root = Path.Combine(artifacts, "host"); var profile = Path.Combine(root, "profile");
        Directory.CreateDirectory(Path.Combine(profile, "user"));
        File.WriteAllText(Path.Combine(profile, "user", "config.json"), """
            {"Subtitle":{"Use STC":false},"Tool":{"Kanji Timer":{"Interpolation":false}},"App":{"First Start":false,"Language":"en_US","Auto":{"Backup":false,"Check For Updates":false,"Load Linked Files":1,"Save":false,"Save on Every Change":false}}}
            """);
        var fixture = Path.Combine(root, "fixture.ass");
        var header = """
            [Script Info]
            Title: Native Karaoke Start Boundary E2E
            Original Script: ASS metadata sentinel
            ScriptType: v4.00+
            PlayResX: 640
            PlayResY: 480
            WrapStyle: 2
            ScaledBorderAndShadow: yes
            [V4+ Styles]
            Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
            """;
        var styles = new[] { "Default", "Source", "Destination", "Alternate Source", "Alternate Destination", "Unused" }
            .Select(style => "Style: " + style + ",Arial,24,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1");
        var events = new[] {
            "Comment: 9,0:00:00.00,0:00:00.50,Default,comment,0091,0092,0093,sentinel,{\\i1}unmodified, café 😀",
            "Dialogue: 1,0:00:01.00,0:00:01.69,Source,source-one,0011,0012,0013,source-fx,{\\k12}a{\\k23}b{\\k34}c",
            "Dialogue: 2,0:00:01.11,0:00:02.22,Destination,dest-one,0021,0022,0023,dest-fx,{\\i1}漢字語",
            "Dialogue: 3,0:00:03.00,0:00:03.75,Alternate Source,alternate-source,0031,0032,0033,alt-fx,{\\k15}x{\\k25}y{\\k35}z",
            "Dialogue: 4,0:00:03.21,0:00:04.32,Alternate Destination,alternate-dest,0041,0042,0043,alt-dest-fx,{\\b1}山川海",
            "Dialogue: 5,0:00:05.00,0:00:05.96,Source,source-two,0051,0052,0053,source-fx,{\\k21}d{\\k32}e{\\k43}f",
            "Dialogue: 6,0:00:05.12,0:00:06.34,Destination,dest-two,0061,0062,0063,dest-fx,{\\bord3}春夏秋",
            "Dialogue: 7,0:00:07.00,0:00:07.66,Source,source-three,0071,0072,0073,source-fx,{\\k11}g{\\k22}h{\\k33}i",
            "Dialogue: 8,0:00:07.23,0:00:08.45,Destination,dest-three,0081,0082,0083,dest-fx,秋冬月",
            "Dialogue: 10,0:00:09.00,0:00:10.00,Default,sentinel,0101,0102,0103,sentinel,untouched {\\b1}世界{\\b0}, 😀"
        };
        File.WriteAllText(fixture, header + "\n" + string.Join('\n', styles) + "\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n" + string.Join('\n', events) + "\n", new UTF8Encoding(true));
        var start = new ProcessStartInfo(exe) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var arg in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", root, "--open", fixture }) start.ArgumentList.Add(arg);
        app = isolation.Start(start, args[2]);
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(root, "ready.json"), app, TimeSpan.FromSeconds(20));
        main = isolation.MainWindow(app, 10);
        undoCommand = ObservedWindowsCommand.FindMenuCommand(main, app.Id, "Edit", "Nothing to undo");
        redoCommand = ObservedWindowsCommand.FindMenuCommand(main, app.Id, "Edit", "Nothing to redo");
        baseline = SaveAs("baseline");
        Require(baseline.Count(line => line.StartsWith("Dialogue:") || line.StartsWith("Comment:")) == 10, "All ten fixture events loaded through the native file-open path");
    }
}

static class Native
{
    delegate bool Callback(nint window, nint data);
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] static extern bool GetWindowRect(nint window, out Rect rect);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(nint parent, Callback callback, nint data);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(nint window);
    [DllImport("user32.dll", EntryPoint = "PostMessageW")] static extern bool PostMessage(nint window, uint message, nint wp, nint lp);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")] static extern nint SendMessage(nint window, uint message, nint wp, nint lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint SendText(nint window, uint message, nint wp, string lp, uint flags, uint timeout, out nint result);
    public static int Top(nint window) { if (!GetWindowRect(window, out var rect)) throw new InvalidOperationException("Native control bounds unavailable"); return rect.Top; }
    public static nint[] Children(nint parent) { List<nint> found = []; Callback callback = (h, _) => { found.Add(h); return true; }; EnumChildWindows(parent, callback, 0); GC.KeepAlive(callback); return found.ToArray(); }
    public static long Send(nint window, uint message, long wp, long lp) { if (SendMessage(window, message, (nint)wp, (nint)lp, 0x22, 1000, out var result) == 0) throw new TimeoutException("Native control message"); return result.ToInt64(); }
    public static void SetText(nint window, string value) { if (SendText(window, 0xC, 0, value, 0x22, 1000, out _) == 0) throw new TimeoutException("Native text entry"); }
    public static int FindString(nint window, string value) { if (SendText(window, 0x158, -1, value, 0x22, 1000, out var result) == 0) throw new TimeoutException("Native style lookup"); return result.ToInt32(); }
    public static void Post(nint window, uint message, long wp, long lp) { if (!PostMessage(window, message, (nint)wp, (nint)lp)) throw new InvalidOperationException("Native message post failed"); }
}
