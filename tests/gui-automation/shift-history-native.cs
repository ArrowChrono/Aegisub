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
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/shift-history-native.cs -- EXE ARTIFACTS");
    return 2;
}
try { return PrivateDesktop.Run(args, Path.GetFullPath(args[1]), isolation => new Scenario(args, isolation).Run()); }
catch (Exception error) { Console.Error.WriteLine(error); return 1; }

sealed class Scenario(string[] args, PrivateDesktop isolation)
{
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    readonly string artifacts = Path.GetFullPath(args[1]);
    readonly List<object> checks = [];
    Process? app;
    nint main, dialog, timeEdit, frameEdit, historyList;
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
            Require(baseline.Count(line => line.StartsWith("Dialogue:")) == 4 && baseline.Count(line => line.StartsWith("Comment:")) == 1, "All four dialogue rows and the comment loaded through native file-open");
            phase = "legacy-history"; Open();
            Require(Send(historyList, 0x18B) == 3, "Three legacy entries loaded atomically");
            Require(Native.ListText(historyList, 0).Contains("0:00:01.23 backward, s, all") && Native.ListText(historyList, 1).Contains("12 frames"), "Legacy comma/padding and frame signs publish canonical history labels");
            Capture("legacy-history"); EnterShift();
            Require(Text(timeEdit) == "0:00:01.23", "Native time input accepts canonical numeric shift");
            Require(Send(Control("Backward"), 0xF0) == 1 && Send(Control("Start times only"), 0xF0) == 1, "Actual native direction and fields selected");
            CloseDialog("Cancel");
            Require(File.ReadAllText(HistoryPath()) == initialHistory, "Cancel does not rewrite or add history");
            Require(SaveAs("after-cancel").SequenceEqual(baseline), "Cancel preserves complete ASS");
            phase = "apply-history"; Open(); EnterShift(); Capture("accepted-shift"); CloseDialog("OK");
            var actual = SaveAs("after-ok"); var expected = Expected();
            Require(actual.SequenceEqual(expected), "Typed shift changes only Start by -1230ms across dialogue/comment rows, preserving every other ASS field");
            var saved = JsonNode.Parse(File.ReadAllText(HistoryPath()))!.AsArray();
            Require(saved.Count == 4 && saved[0]!["amount"]!.GetValue<string>() == "0:00:01.23", "Accepted history stores actual canonical numeric shift");
            Require(saved[2]!["amount"]!.GetValue<string>() == "12", "Legacy frame amount canonicalizes without changing numeric value");
            Require(saved[0]!["selection"]![0]!["start"]!.GetValue<int>() == 1 && saved[0]!["selection"]![0]!["end"]!.GetValue<int>() == 1, "Saved selection blocks are one-based inclusive");
            File.Copy(HistoryPath(), Path.Combine(artifacts, "after-ok-history.json"));
            NormalExit();
            var preferences = Preferences();
            Require(preferences["Time"]!.GetValue<int>() == 1230 && preferences["Direction"]!.GetValue<bool>() && preferences["Type"]!.GetValue<int>() == 1, "Normal exit persists accepted numeric preference, direction and fields");
            phase = "restart"; Start(); Open(); Require(Send(historyList, 0x18B) == 4, "Same-profile restart rereads canonical history"); Require(Text(timeEdit) == "0:00:01.23" && Send(Control("Backward"), 0xF0) == 1 && Send(Control("Start times only"), 0xF0) == 1, "Restart reads actual accepted amount/direction/fields into native controls"); EnterShift(); Capture("restart-history"); CloseDialog("Cancel"); NormalExit();
            phase = "malformed";
            var broken = "[{\"filename\":\"valid.ass\",\"amount\":\"0:00:01.00\",\"is by time\":true,\"is backward\":false,\"fields\":0,\"mode\":0,\"selection\":[]},{\"amount\":null}]";
            File.WriteAllText(HistoryPath(), broken); Start(); Open();
            Require(Send(historyList, 0x18B) == 0, "Malformed second entry publishes no partial history list"); Capture("malformed-history");
            EnterShift(); CloseDialog("OK");
            Require(SaveAs("after-malformed-apply").SequenceEqual(Expected()), "Malformed auxiliary history does not prevent the exact subtitle shift");
            Require(File.ReadAllText(HistoryPath()) == broken, "Applying after malformed load does not overwrite the source history");
            Open();
            ObservedWindowsCommand.PostButtonClick(Control("Clear"), app!.Id);
            Wait(() => !File.Exists(HistoryPath()) && Send(historyList, 0x18B) == 0, "Explicit Clear removes malformed history and resets the list");
            CloseDialog("Cancel"); phase = "recovered"; Open();
            Require(Send(historyList, 0x18B) == 0, "Cleared history reopens without a warning or stale entries");
            EnterShift(); CloseDialog("OK");
            Require(SaveAs("after-recovered-apply").SequenceEqual(Expected(-246)), "Recovery applies exactly a second 1230ms shift with all unrelated fields preserved");
            var recovered = JsonNode.Parse(File.ReadAllText(HistoryPath()))!.AsArray();
            Require(recovered.Count == 1 && recovered[0]!["amount"]!.GetValue<string>() == "0:00:01.23", "Explicit Clear restores history writing with one canonical accepted record");
            NormalExit();
            LockedHistory();
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
    string HistoryPath() => Path.Combine(profile, "user", "shift_history.json");
    string initialHistory = "";
    void LockedHistory()
    {
        phase = "locked-history";
        var before = File.ReadAllBytes(HistoryPath());
        File.WriteAllBytes(Path.Combine(artifacts, "before-locked-history.json"), before);
        Start();
        bool warned = false, exactText = false, taskDialog = false;
        bool unchanged;
        string[] temporaryFiles;
        using (var historyLock = new FileStream(HistoryPath(), FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            Open();
            Require(Send(historyList, 0x18B) == 1, "Existing valid history remains readable while replacement is locked");
            EnterShift(); Capture("locked-history");
            var old = dialog;
            Action("button", "OK/history-replacement-locked");
            ObservedWindowsCommand.PostButtonClick(Control("OK"), app!.Id);
            Wait(() => {
                var warning = Windows().SingleOrDefault(h => Text(h) == "Shift history");
                if (warning != 0)
                {
                    Require(!warned, "Final history commit failure shows only one save-error notification");
                    Dump("save-error-controls");
                    ScreenCapture.SaveWindowPng(warning, Path.Combine(artifacts, "save-error.png"));
                    var accessible = AutomationElement.FromHandle(warning).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>()
                        .Select(e => new { name = e.Current.Name, control_type = e.Current.ControlType.ProgrammaticName, automation_id = e.Current.AutomationId }).ToArray();
                    File.WriteAllText(Path.Combine(artifacts, "save-error-accessibility.json"), JsonSerializer.Serialize(accessible, Json));
                    exactText = accessible.Any(e => e.control_type == "ControlType.Text" && e.name == "Subtitles were shifted, but shift history could not be saved.");
                    taskDialog = PrivateDesktop.ClassName(warning) == "#32770" && Children(warning).Any(h => PrivateDesktop.ClassName(h) == "DirectUIHWND");
                    var ok = Children(warning).Single(h => Text(h).Replace("&", "") == "OK");
                    isolation.VerifyWindow(ok, app!);
                    Send(warning, 0x466, 1);
                    warned = true;
                    return false;
                }
                return !Windows().Contains(old);
            }, "Locked history replacement finishes with a dismissible save-error notification");
            dialog = 0;
            var after = File.ReadAllBytes(HistoryPath());
            File.WriteAllBytes(Path.Combine(artifacts, "after-locked-history.json"), after);
            unchanged = after.SequenceEqual(before);
            temporaryFiles = Directory.GetFiles(Path.GetDirectoryName(HistoryPath())!, "shift_history_tmp*.json").Select(Path.GetFileName).ToArray()!;
            var actual = SaveAs("after-locked-apply");
            Require(actual.SequenceEqual(LiteralLockedExpected()), "Failed history replacement preserves the exact full ASS shift without rollback, including all unrelated fields and metadata");
        }
        NormalExit();
        File.WriteAllText(Path.Combine(artifacts, "locked-history-observations.json"), JsonSerializer.Serialize(new { warned, exactText, taskDialog, unchanged, temporaryFiles }, Json));
        Require(unchanged, "Failed history replacement leaves the previous history byte-for-byte unchanged");
        Require(warned && exactText && taskDialog, "Actual wx TaskDialog shows the exact history save-error text");
        Require(temporaryFiles.Length == 0, "Failed history replacement cleans every shift history temporary file");
    }
    string[] LiteralLockedExpected()
    {
        var literal = $$"""
            [Script Info]
            {{baseline[1]}}
            ; http://www.aegisub.org/
            Title: Native Timing Preferences E2E
            Original Script: metadata sentinel café 世界
            ScriptType: v4.00+
            PlayResX: 640
            PlayResY: 480
            WrapStyle: 2
            ScaledBorderAndShadow: yes

            [Aegisub Project Garbage]

            [V4+ Styles]
            Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
            Style: Default,Arial,24,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1
            Style: Alternate,Arial,24,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1
            Style: Third,Arial,24,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1

            [Events]
            Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
            Dialogue: 1,0:00:08.77,0:00:12.34,Default,one,11,12,13,fx-one,{\i1}café, 世界
            Comment: 9,0:00:08.87,0:00:10.50,Alternate,comment,91,92,93,sentinel,{\b1}untouched 😀
            Dialogue: 2,0:00:18.77,0:00:22.56,Alternate,two,21,22,23,fx-two,{\bord3}second
            Dialogue: 3,0:00:28.77,0:00:32.78,Third,three,31,32,33,fx-three,third 😀
            Dialogue: 4,0:00:38.77,0:00:42.90,Default,four,41,42,43,fx-four,{\b1}fourth{\b0}
            """;
        var expected = literal.Replace("\r", "").Split('\n');
        File.WriteAllLines(Path.Combine(artifacts, "expected-locked-apply.ass"), expected, new UTF8Encoding(true));
        return expected;
    }
    void Open()
    {
        Command("Timing", "Shift Times...");
        bool warned = false;
        Wait(() => {
            var warning = Windows().SingleOrDefault(h => Text(h) == "Shift history");
            if (warning != 0) {
                Require(phase == "malformed" && !warned, "Load failure produces exactly one explicit warning");
                ScreenCapture.SaveWindowPng(warning, Path.Combine(artifacts, "load-warning.png"));
                var ok = Children(warning).Single(h => Text(h).Replace("&", "") == "OK");
                isolation.VerifyWindow(ok, app!); Send(warning, 0x466, 1); warned = true;
                return false;
            }
            dialog = Windows().SingleOrDefault(h => Text(h) == "Shift Times"); return dialog != 0 && Native.IsWindowEnabled(dialog); }, "Actual Shift Times opened");
        Require(warned == (phase == "malformed"), "Warning matches actual history load failure");
        var children = Children(dialog);
        var edits = children.Where(h => PrivateDesktop.ClassName(h) == "Edit").OrderBy(h => Native.Bounds(h).Top).ToArray();
        Require(edits.Length == 2, "Two native amount controls identified"); timeEdit = edits[0]; frameEdit = edits[1];
        historyList = children.Single(h => PrivateDesktop.ClassName(h) == "ListBox"); Dump("controls-" + phase);
    }
    void EnterShift()
    {
        Native.SetText(timeEdit, "0:00:01.23");
        ObservedWindowsCommand.PostButtonClick(Control("Backward"), app!.Id);
        ObservedWindowsCommand.PostButtonClick(Control("Start times only"), app.Id);
        Wait(() => Send(Control("Backward"), 0xF0) == 1 && Send(Control("Start times only"), 0xF0) == 1, "Native direction and start-only controls selected");
        Action("native-time-and-controls", "0:00:01.23/backward/start-only");
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
    string[] Expected(int shift = -123)
    {
        var expected = baseline.Select(line => {
            if (!line.StartsWith("Dialogue:") && !line.StartsWith("Comment:")) return line;
            var fields = line.Split(',', 10);
            fields[1] = Shift(fields[1], shift);
            return string.Join(',', fields);
        }).ToArray();
        File.WriteAllLines(Path.Combine(artifacts, "expected-" + (-shift) + ".ass"), expected, new UTF8Encoding(true)); return expected;
    }
    static string Shift(string time, int centiseconds)
    {
        var parts = time.Split(':', '.').Select(int.Parse).ToArray();
        var total = ((parts[0] * 60 + parts[1]) * 60 + parts[2]) * 100 + parts[3] + centiseconds;
        return $"{total / 360000}:{total / 6000 % 60:00}:{total / 100 % 60:00}.{total % 100:00}";
    }
    void NormalExit()
    {
        isolation.VerifyWindow(main, app!); Action("normal-exit", "main/WM_CLOSE"); Native.Post(main, 0x10, 0, 0);
        Require(app!.WaitForExit(10_000), "WM_CLOSE exits normally within ten seconds without forced termination");
        var exitCode = isolation.ExitCode(app);
        Require(exitCode == 0, "Normal host native exit code is zero (actual " + exitCode + ")"); app.Dispose(); app = null; main = 0;
    }
    JsonNode Preferences() => JsonNode.Parse(File.ReadAllText(Path.Combine(profile, "user", "config.json")))!["Tool"]!["Shift Times"]!;
    void Prepare()
    {
        var host = Path.Combine(artifacts, "host"); profile = Path.Combine(host, "profile");
        Directory.CreateDirectory(Path.Combine(profile, "user"));
        var config = JsonNode.Parse("""
            {"Subtitle":{"Use STC":false,"Time Edit":{"Insert Mode":false}},"App":{"First Start":false,"Language":"en_US","Auto":{"Backup":false,"Check For Updates":false,"Load Linked Files":1,"Save":false,"Save on Every Change":false}},"Tool":{"Shift Times":{"Time":0,"Frames":0,"ByTime":true,"Direction":false,"Affect":0,"Type":0}}}
            """)!;
        File.WriteAllText(Path.Combine(profile, "user", "config.json"), config.ToJsonString(Json));
        initialHistory = """
            [{"filename":"legacy.ass","amount":"00:00:01,23","is by time":true,"is backward":true,"fields":1,"mode":0,"selection":[{"start":1,"end":1}]},{"filename":"frames.ass","amount":"+0012","is by time":false,"is backward":false,"fields":0,"mode":1,"selection":[{"start":2,"end":3}]},{"filename":"seconds.ass","amount":"2","is by time":true,"is backward":false,"fields":2,"mode":2,"selection":[{"start":4,"end":4}]}]
            """;
        File.WriteAllText(HistoryPath(), initialHistory);
        fixture = Path.Combine(host, "fixture.ass");
        var header = """
            [Script Info]
            Title: Native Timing Preferences E2E
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
        var events = new[] {
            "Dialogue: 1,0:00:10.00,0:00:12.34,Default,one,0011,0012,0013,fx-one,{\\i1}café, 世界",
            "Comment: 9,0:00:10.10,0:00:10.50,Alternate,comment,0091,0092,0093,sentinel,{\\b1}untouched 😀",
            "Dialogue: 2,0:00:20.00,0:00:22.56,Alternate,two,0021,0022,0023,fx-two,{\\bord3}second",
            "Dialogue: 3,0:00:30.00,0:00:32.78,Third,three,0031,0032,0033,fx-three,third 😀",
            "Dialogue: 4,0:00:40.00,0:00:42.90,Default,four,0041,0042,0043,fx-four,{\\b1}fourth{\\b0}"
        };
        File.WriteAllText(fixture, header + "\n" + string.Join('\n', styles) + "\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n" + string.Join('\n', events) + "\n", new UTF8Encoding(true));
    }
    void Start()
    {
        var host = Path.Combine(artifacts, "launch-" + ++launch); Directory.CreateDirectory(host);
        var start = new ProcessStartInfo(Path.GetFullPath(args[0])) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", host, "--open", fixture }) start.ArgumentList.Add(argument);
        app = isolation.Start(start, "shift-history-" + launch);
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
    [DllImport("user32.dll", EntryPoint = "PostMessageW")] static extern bool PostMessage(nint window, uint message, nint wp, nint lp);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")] static extern nint SendMessage(nint window, uint message, nint wp, nint lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint SendText(nint window, uint message, nint wp, string lp, uint flags, uint timeout, out nint result);
    public static Rectangle Bounds(nint window) { if (!GetWindowRect(window, out var rect)) throw new InvalidOperationException("Native bounds unavailable"); return new(rect.Left, rect.Top, rect.Right, rect.Bottom); }
    public static nint[] Children(nint parent) { List<nint> found = []; Callback callback = (h, _) => { found.Add(h); return true; }; EnumChildWindows(parent, callback, 0); GC.KeepAlive(callback); return found.ToArray(); }
    public static long Send(nint window, uint message, long wp, long lp) { if (SendMessage(window, message, (nint)wp, (nint)lp, 0x22, 1000, out var result) == 0) throw new TimeoutException("Native control message"); return result.ToInt64(); }
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint ReadListText(nint window, uint message, nint wp, StringBuilder text, uint flags, uint timeout, out nint result);
    public static string ListText(nint window, int index) {
        var length = Send(window, 0x18A, index, 0);
        if (length < 0 || length > 4096) throw new InvalidOperationException("Invalid observed list item length");
        var buffer = new StringBuilder((int)length + 1);
        if (ReadListText(window, 0x189, index, buffer, 0x22, 1000, out _) == 0) throw new TimeoutException("Native list item text");
        return buffer.ToString();
    }
    public static void SetText(nint window, string value) { if (SendText(window, 0xC, 0, value, 0x22, 1000, out _) == 0) throw new TimeoutException("Native text entry"); }
    public static void Post(nint window, uint message, long wp, long lp) { if (!PostMessage(window, message, (nint)wp, (nint)lp)) throw new InvalidOperationException("Native message post failed"); }
}
