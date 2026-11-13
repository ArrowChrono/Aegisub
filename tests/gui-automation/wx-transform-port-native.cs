#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Aegisub.GuiAutomation.Driver;

if (args.Length != 4) { Console.Error.WriteLine("Usage: wx-transform-port-native EXE ARTIFACTS ARTIFACT_ROOT LABEL"); return 2; }
try
{
    return PrivateDesktop.Run(args, Path.GetFullPath(args[1]), desktop => new TransformPortScenario(args, desktop).Run(),
        Path.GetFullPath(args[2]), 170_000, guardDescendants: true);
}
catch (Exception failure) { Console.Error.WriteLine(failure); return 1; }

sealed class TransformPortScenario(string[] args, PrivateDesktop desktop)
{
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    static readonly TransformCase[] Gestures =
    [
        new("RotateZHold", "Z"), new("RotateXYHold", "XY"), new("ScaleHold", "Scale"),
        new("RotateZOrigin", "Z", Origin: true), new("RotateXYOriginShift", "XY", Origin: true, Shift: true),
        new("RotateXYShiftControl", "XY", Shift: true, Control: true), new("RotateZControl", "Z", Control: true),
        new("ScaleShift", "Scale", Shift: true), new("ScaleControl", "Scale", Control: true),
        new("RotateXYReturnNormalization", "XY", Return: true),
    ];
    static readonly string[] Poses = ["Neutral", "NormalXYZShear", "EdgeOn", "NearCamera"];
    static readonly string[] ZAppearancePoses = ["Neutral", "NormalXYZShear"];
    readonly string artifacts = Path.GetFullPath(args[1]);
    readonly string executable = Path.GetFullPath(args[0]);
    readonly Dictionary<string, CaseResult> results = new[] { "HostAndFoldSetup" }.Concat(Gestures.Select(g => g.Name)).Concat(Poses.Select(p => "XYDraw" + p)).Concat(ZAppearancePoses.Select(p => "ZPressAppearance" + p)).Append("RuntimeAndTraceEvidence")
        .ToDictionary(name => name, _ => new CaseResult("not-run", 0, null));
    readonly List<Cohort> cohorts = [];
    readonly long deadline = Stopwatch.GetTimestamp() + 150 * Stopwatch.Frequency;
    Process? app;
    nint main, canvas;
    string profile = "", host = "", fixture = "", phase = "setup";
    int stateSequence, captureSequence, checks;
    JsonElement lastState;

    public int Run()
    {
        Directory.CreateDirectory(artifacts);
        using var dpi = new DriverThreadDpiScope(artifacts);
        dpi.RecordActivation();
        desktop.RecordDriver();
        using var log = new StreamWriter(Path.Combine(artifacts, "driver.log")) { AutoFlush = true };
        var previousOut = Console.Out; var previousError = Console.Error;
        Console.SetOut(log); Console.SetError(log);
        using var watchdog = new Timer(_ =>
        {
            try { WriteReports("150-second business deadline expired"); }
            finally { try { if (app is not null) desktop.Stop(app); } finally { Environment.Exit(124); } }
        }, null, 150_000, Timeout.Infinite);
        try
        {
            Case("HostAndFoldSetup", () =>
            {
                Prepare(); Start(); Command("Reload Scripts");
                Wait(() => HasCommand("Automation", "Visual Port Select Sources"), "Synchronous autoload registration");
                Macro("Visual Port Select Sources");
                Wait(() => State().GetProperty("selected_ids").GetArrayLength() == 3, "Three source-row selection");
                Command("Fold Sources"); Wait(() => Folded(State()), "Actual collapsed source projection"); Save("initial-normalized");
            });
            foreach (var gesture in Gestures) Case(gesture.Name, () => Gesture(gesture));
            foreach (var pose in ZAppearancePoses) Case("ZPressAppearance" + pose, () => ZPressAppearance(pose));
            foreach (var pose in Poses) Case("XYDraw" + pose, () => DrawCohort(pose));
            Case("RuntimeAndTraceEvidence", () =>
            {
                Save("final"); desktop.VerifyWindow(main, app!); Send(main, 0x0010, 0, 0);
                if (!app!.WaitForExit(Math.Min(5000, RemainingMilliseconds()))) throw new TimeoutException("Normal wx close exceeded remaining deadline");
                Require(desktop.ExitCode(app) == 0, "Owned wx host exits normally with code zero"); AnalyzeTrace();
            });
            WriteReports(null);
            using var report = JsonDocument.Parse(File.ReadAllText(Path.Combine(artifacts, "result.json")));
            Require(report.RootElement.GetProperty("cases").EnumerateObject().Count() == results.Count
                && report.RootElement.GetProperty("cases").EnumerateObject().All(item => item.Value.GetProperty("Status").GetString() == "passed"
                    && item.Value.GetProperty("Checks").GetInt32() > 0), "Completed result contains every exact case and nonempty checks");
            var junit = XDocument.Load(Path.Combine(artifacts, "junit.xml"));
            Require(junit.Descendants("testcase").Count() == results.Count && !junit.Descendants("failure").Any()
                && !junit.Descendants("skipped").Any(), "JUnit accounts for all successful cases");
            return 0;
        }
        catch (Exception failure)
        {
            File.WriteAllText(Path.Combine(artifacts, "failure.json"), JsonSerializer.Serialize(new
            {
                phase, error = failure.ToString(), state = lastState.ValueKind == JsonValueKind.Object ? lastState : (object?)null,
                host_pid = app?.Id, input = "owned-native-window-messages", global_input = false
            }, Json));
            WriteReports(failure.ToString()); Console.Error.WriteLine(failure); return 1;
        }
        finally
        {
            try { if (app is not null) { desktop.Stop(app); app.Dispose(); } }
            finally { Console.SetOut(previousOut); Console.SetError(previousError); }
        }
    }

    void Prepare()
    {
        Check(); profile = Path.Combine(artifacts, "profile"); host = Path.Combine(artifacts, "host");
        Directory.CreateDirectory(Path.Combine(profile, "user", "automation", "autoload")); Directory.CreateDirectory(host);
        fixture = Path.Combine(artifacts, "fixture.ass");
        File.Copy("tests/gui-automation/wx-transform-port/fixture.ass", fixture);
        File.Copy("tests/gui-automation/wx-transform-port/poses.lua", Path.Combine(profile, "user", "automation", "autoload", "poses.lua"));
        var config = JsonNode.Parse("""
          {"App":{"First Start":false,"Language":"en_US","Save UI State":true,"Auto":{"Backup":false,"Check For Updates":false,"Load Linked Files":0,"Save":false,"Save on Every Change":false}},"Subtitle":{"Provider":"libass","Use STC":true},"Video":{"Provider":"YUV4MPEG","Open Audio":false,"Default Zoom":7,"Scale with DPI":false,"Renderer":{"Backend":"opengl"},"Skia Tools":{"Enabled":false}},"Tool":{"Visual":{"Autohide":false}},"Automation":{"Lua Workspace":{"Enable LuaLS":false}},"Path":{"Automation":{"Autoload":"?user/automation/autoload/"}}}
          """)!;
        File.WriteAllText(Path.Combine(profile, "user", "config.json"), config.ToJsonString(Json));
        AutomationTestProfile.WriteMenusWithoutClipboardValidation(profile, artifacts);
        var menuPath = Path.Combine(profile, "user", "menu.json"); var menus = JsonNode.Parse(File.ReadAllText(menuPath))!.AsObject();
        menus["main"]!.AsArray().Add(new JsonObject { ["submenu"] = "visual-port", ["text"] = "Visual Port Test" });
        JsonArray commands = [];
        foreach (var (name, label) in new[]
        {
            ("am/reload/autoload", "Reload Scripts"), ("subtitle/save", "Save"), ("edit/undo", "Undo"), ("edit/redo", "Redo"),
            ("grid/fold/create", "Fold Sources"), ("video/tool/cross", "Cross"), ("video/tool/rotate/z", "Z"), ("video/tool/rotate/xy", "XY"),
            ("video/tool/scale", "Scale"), ("video/tool/geometry_linking", "Linking")
        }) commands.Add(new JsonObject { ["command"] = name, ["text"] = label });
        menus["visual-port"] = commands; File.WriteAllText(menuPath, menus.ToJsonString(Json));
        using var video = File.Create(Path.Combine(artifacts, "fixture.y4m"));
        video.Write(Encoding.ASCII.GetBytes("YUV4MPEG2 W640 H480 F25:1 Ip A1:1 C420jpeg\n"));
        var luma = new byte[640 * 480];
        for (var y = 0; y < 480; ++y) for (var x = 0; x < 640; ++x) luma[y * 640 + x] = (byte)(40 + ((x / 32 + y / 32) % 2) * 20);
        var chroma = Enumerable.Repeat((byte)128, 320 * 240).ToArray();
        for (var i = 0; i < 40; ++i) { video.Write("FRAME\n"u8); video.Write(luma); video.Write(chroma); video.Write(chroma); }
        video.Flush();
        File.WriteAllText(Path.Combine(artifacts, "contract.json"), JsonSerializer.Serialize(new
        {
            label = args[3], functional_cases = Gestures, poses = Poses, warmup_paints = 16, measured_paints = 96,
            business_seconds = 150, native_seconds = 170, fixed_frame = 0, subtitle_provider = "libass", video_renderer = "opengl",
            physical_input = false, global_input = false, alt_native = "not-covered; shared-policy and unchanged wx argument routing only",
            timing = "Application wall scopes including driver blocking; captures excluded; no CPU-cycle, GPU elapsed or scan-out claim"
        }, Json));
    }

    void Start()
    {
        var start = new ProcessStartInfo(executable) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        start.Environment["AEGISUB_ENABLE_SKIA_VIDEO_TOOLS"] = "0"; start.Environment["AEGISUB_ENABLE_DXGI_PRESENTATION"] = "0";
        start.Environment["AEGISUB_PERF_TRACE"] = "video,wx-transform-shared-port";
        foreach (var value in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", host, "--open", fixture, "--open", Path.Combine(artifacts, "fixture.y4m") }) start.ArgumentList.Add(value);
        app = desktop.Start(start, "actual-wx-host", deadline); Wait(() => File.Exists(Path.Combine(host, "ready.json")), "Real wx GUI host readiness");
        using var ready = JsonDocument.Parse(File.ReadAllText(Path.Combine(host, "ready.json")));
        Require(ready.RootElement.GetProperty("process_id").GetInt32() == app.Id, "Host ready artifact belongs to exact owned PID");
        main = desktop.MainWindow(app, Math.Max(1, Math.Min(10, RemainingMilliseconds() / 1000)), deadline); desktop.VerifyWindow(main, app);
        if (WxNative.SetWindowPos(main, 0, 40, 40, 1280, 960, 0x50) == 0) throw new Win32Exception(Marshal.GetLastPInvokeError(), "SetWindowPos");
        Wait(() =>
        {
            canvas = WxNative.Children(main).Where(WxNative.IsVisible).Where(h => PrivateDesktop.ClassName(h) == "wxGLCanvas")
                .OrderByDescending(h => { WxNative.GetClientRect(h, out var r); return (long)r.Right * r.Bottom; }).FirstOrDefault();
            return canvas != 0 && State().GetProperty("viewport").GetProperty("width").GetDouble() > 0;
        }, "Actual wxGLCanvas and positive viewport");
        desktop.VerifyWindow(canvas, app);
        if (WxNative.GetClientRect(canvas, out var client) == 0) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Selected canvas GetClientRect");
        File.WriteAllText(Path.Combine(artifacts, "canvas-target.json"), JsonSerializer.Serialize(new
        {
            hwnd = $"0x{canvas:X}", main_hwnd = $"0x{main:X}", pid = app.Id, native_class = PrivateDesktop.ClassName(canvas),
            client_rect = new { left = client.Left, top = client.Top, right = client.Right, bottom = client.Bottom },
            selection_rule = "Largest native client area among visible wxGLCanvas descendants of the owned main HWND", time_ns = MonotonicNanoseconds()
        }, Json));
        Require(!WxNative.MenuChecked(main, "Visual Port Test", "Linking"), "Geometry Linking starts off in actual wx host"); Capture("setup");
        using var setupCapture = JsonDocument.Parse(File.ReadAllText(Path.Combine(artifacts, "setup-capture.json")));
        var captured = setupCapture.RootElement;
        if (WxNative.GetClientRect(canvas, out var physicalClient) == 0) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Physical canvas GetClientRect");
        var physicalMatch = physicalClient.Right - physicalClient.Left == captured.GetProperty("canvas_client_width").GetInt32()
            && physicalClient.Bottom - physicalClient.Top == captured.GetProperty("canvas_client_height").GetInt32();
        File.WriteAllText(Path.Combine(artifacts, "dpi-canvas-contract.json"), JsonSerializer.Serialize(new
        {
            hwnd = $"0x{canvas:X}", pid = app.Id, awareness_evidence = "dpi-awareness.ndjson",
            native_client = new { left = physicalClient.Left, top = physicalClient.Top, right = physicalClient.Right, bottom = physicalClient.Bottom },
            captured_client = new { width = captured.GetProperty("canvas_client_width").GetInt32(), height = captured.GetProperty("canvas_client_height").GetInt32() },
            captured_dpi_scale = captured.GetProperty("dpi_scale").GetDouble(), backing_scale = captured.GetProperty("backing_scale").GetInt32(),
            captured_image = new { width = captured.GetProperty("physical_width").GetInt32(), height = captured.GetProperty("physical_height").GetInt32() },
            physical_extent_matches = physicalMatch
        }, Json));
        Require(physicalMatch, "Driver-thread physical native client extent exactly matches the C++ viewport capture before input");
    }

    void ZPressAppearance(string pose)
    {
        Macro("Visual Port Pose " + pose); Macro("Visual Port Select Active Source"); Command("Z");
        Save("ZPressAppearance" + pose + "-pose");
        var origin = Pixel(State(), 220, 180);
        // Start a rotation hold outside the origin handle but inside the mouse-guide dead zone.
        // Native synthetic hover can generate Leave on a private, non-input desktop.
        var point = new Point(origin.X + 9, origin.Y);
        Mouse(0x200, point, 0);
        Wait(() => !State().GetProperty("presentation_snapshot_available").GetBoolean(), "Idle Z uses the live overlay");
        var label = "ZPressAppearance" + pose;
        Capture(label + "-idle");
        Mouse(0x201, point, 1);
        var pressed = State();
        Require(pressed.GetProperty("native_capture").GetBoolean() && pressed.GetProperty("visual_interacting").GetBoolean()
            && pressed.GetProperty("presentation_snapshot_available").GetBoolean(), "Z press uses a paired snapshot with native capture");
        Capture(label + "-pressed");
        Mouse(0x202, point, 0);
        Wait(() => !State().GetProperty("presentation_snapshot_available").GetBoolean(), "Released Z retires the paired snapshot");
        Require(!lastState.GetProperty("native_capture").GetBoolean() && !lastState.GetProperty("visual_interacting").GetBoolean(), "Z release retires native capture");
        Capture(label + "-released");
        // The viewport encoder is deterministic. Comparing its lossless PNG output also avoids
        // WPF image decoding changing the process-wide DPI context during a native input test.
        var idle = File.ReadAllBytes(Path.Combine(artifacts, label + "-idle.png"));
        foreach (var suffix in new[] { "-pressed", "-released" })
        {
            var actual = File.ReadAllBytes(Path.Combine(artifacts, label + suffix + ".png"));
            var identical = idle.AsSpan().SequenceEqual(actual);
            File.WriteAllText(Path.Combine(artifacts, label + suffix + "-comparison.json"), JsonSerializer.Serialize(new
            {
                pose, phase = suffix, identical_png_bytes = identical, expected_identical_png_bytes = true
            }, Json));
            Require(identical, "Unchanged Z pose must have identical lossless viewport captures in live and paired-snapshot presentation: " + suffix);
        }
    }

    void Gesture(TransformCase test)
    {
        Command(test.Tool); var before = State(); Require(Folded(before), "Selected hidden sources retain the real collapsed projection");
        var original = Save(test.Name + "-before"); before = State();
        var start = Pixel(before, test.Origin ? 220 : 320, 180);
        var end = test.Tool == "Z" && test.Control ? Pixel(before, 220, 280) : new Point(start.X + 25, start.Y - 11);
        var keys = (test.Shift ? 4 : 0) | (test.Control ? 8 : 0);
        Mouse(0x200, start, keys); Mouse(0x201, start, 1 | keys);
        var pressed = State();
        Require(pressed.GetProperty("native_capture").GetBoolean() && pressed.GetProperty("visual_interacting").GetBoolean(), "Native Down acquires real wx visual capture");
        Mouse(0x200, end, 1 | keys); var preview = State();
        Require(Rows(preview).Take(3).All(row => row.GetProperty("selected").GetBoolean()), "Captured transform preserves all hidden selected sources");
        Capture(test.Name + "-preview");
        if (test.Return) { end = start; Mouse(0x200, end, 1 | keys); }
        Mouse(0x202, end, keys); var after = State();
        Require(!after.GetProperty("native_capture").GetBoolean() && !after.GetProperty("visual_interacting").GetBoolean(), "Native Up retires wx capture and interaction");
        Require(Folded(after), "Transform retains actual collapsed display projection");
        var beforeRows = Rows(before); var afterRows = Rows(after);
        Require(before.GetProperty("active_id").GetInt32() == after.GetProperty("active_id").GetInt32()
            && beforeRows.Select(Id).SequenceEqual(afterRows.Select(Id))
            && before.GetProperty("selected_ids").EnumerateArray().Select(x => x.GetInt32()).Order().SequenceEqual(after.GetProperty("selected_ids").EnumerateArray().Select(x => x.GetInt32()).Order()),
            "Runtime source/active/selection IDs remain stable across the actual gesture");
        var owned = test.Origin ? @"\\org\([^)]*\)" : test.Tool == "XY" ? @"\\fr[xy]-?[0-9]+(?:\.[0-9]+)?" : test.Tool == "Z" ? @"\\frz-?[0-9]+(?:\.[0-9]+)?" : @"\\fsc[xy]-?[0-9]+(?:\.[0-9]+)?";
        for (var row = 0; row < 4; ++row)
        {
            var a = beforeRows[row].GetProperty("text").GetString()!; var b = afterRows[row].GetProperty("text").GetString()!;
            if (row == 3) { Require(a == b, "Unselected future sentinel is exact"); continue; }
            Require(Regex.Replace(a, owned, "") == Regex.Replace(b, owned, ""), "Selected source preserves every non-owned override and multilingual body byte");
            var prefix = beforeRows[row].GetProperty("event").GetString()![..^a.Length];
            Require(afterRows[row].GetProperty("event").GetString() == prefix + b, "Complete event metadata remains exact");
            VerifyGeometry(test, before, start, end, row, b);
        }
        var edited = Save(test.Name + "-after"); Require(edited != original, "Gesture changes selected source geometry, not only command state");
        Command("Undo"); Require(Save(test.Name + "-undo") == original, "One wx coalesced Undo restores exact controlled ASS sections");
        Command("Redo"); Require(Save(test.Name + "-redo") == edited, "One wx Redo restores exact controlled ASS sections");
        Command("Undo"); Require(Save(test.Name + "-restored") == original, "Next case begins at the same controlled document baseline"); Capture(test.Name + "-restored");
    }

    void VerifyGeometry(TransformCase test, JsonElement state, Point start, Point end, int row, string text)
    {
        var dx = (double)(end.X - start.X); var dy = (double)(end.Y - start.Y);
        if (test.Shift) { if (Math.Abs(dx) > Math.Abs(dy)) dy = 0; else dx = 0; }
        if (test.Origin)
        {
            var positions = new[] { new[] { 220d, 180d }, new[] { 420d, 280d }, new[] { 100d, 360d } }; var view = state.GetProperty("viewport");
            var wanted = new[] { positions[row][0] + dx * 640 / view.GetProperty("width").GetDouble(), positions[row][1] + dy * 480 / view.GetProperty("height").GetDouble() };
            var actual = TupleTag(text, "org");
            Require(actual.Length == 2 && actual.SequenceEqual(wanted.Select(x => Math.Round(x, 3, MidpointRounding.ToEven))), "Every source origin uses its own explicit origin or position fallback and ASS three-decimal serialization");
        }
        else if (test.Tool == "XY")
        {
            var x = 15 - dy * 2; var y = 25 + dx * 2;
            if (test.Control) { x = Math.Floor(x / 30 + .5) * 30; y = Math.Floor(y / 30 + .5) * 30; }
            Require(Scalar(text, "frx") == Four((x + 360) % 360) && Scalar(text, "fry") == Four((y + 360) % 360), "XY display delta, Shift/Ctrl and signed wrapping match wx contract");
        }
        else if (test.Tool == "Z")
        {
            var origin = ContinuousPixel(state, 220, 180);
            var angle = 30 + (Math.Atan2(origin.Y - start.Y, origin.X - start.X) - Math.Atan2(origin.Y - end.Y, origin.X - end.X)) * 180 / Math.PI;
            if (test.Control) angle = Math.Floor(angle / 30 + .5) * 30;
            Require(Scalar(text, "frz") == Four((angle + 360) % 360), "Z angle uses the continuous projected origin and exact four-significant-digit serialization");
        }
        else
        {
            var x = Math.Max(0, 135 + dx * 1.25); var y = Math.Max(0, 75 - dy * 1.25);
            if (test.Control) { x = Math.Floor(x / 25 + .5) * 25; y = Math.Floor(y / 25 + .5) * 25; }
            Require(Scalar(text, "fscx") == (int)x && Scalar(text, "fscy") == (int)y, "Scale uses active percentages, display delta, Shift/Ctrl and wx integer hold serialization");
        }
    }

    void DrawCohort(string pose)
    {
        Macro("Visual Port Pose " + pose); Command("XY"); Save("XYDraw" + pose + "-pose"); var before = State();
        var expected = pose switch { "Neutral" => new[] { 0d, 0, 0, 0, 0, 480 }, "NormalXYZShear" => new[] { 15d, 25, 30, .2, -.1, 480 }, "EdgeOn" => new[] { 90d, 0, 30, .2, -.1, 480 }, "NearCamera" => new[] { 75d, 80, 15, .2, -.1, 38400 }, _ => throw new InvalidOperationException("Unknown comparison pose") };
        var text = Rows(before)[0].GetProperty("text").GetString()!;
        Require(new[] { "frx", "fry", "frz", "fax", "fay" }.Select(tag => Scalar(text, tag)).SequenceEqual(expected.Take(5)), "Comparison cohort observes its exact XYZ/shear pose in the runtime active source");
        Require(File.ReadLines(fixture).Contains("LayoutResY: " + expected[5].ToString(CultureInfo.InvariantCulture)), "Comparison cohort saves its exact layout camera resolution");
        Require(Folded(before) && before.GetProperty("video_frame").GetInt32() == 0, "Comparison pose retains collapsed hidden source selection and fixed frame zero");
        var start = Pixel(before, 320, 180);
        Mouse(0x200, start, 0); Mouse(0x201, start, 1);
        Wait(() => State().GetProperty("presentation_snapshot_available").GetBoolean(), "XY paired snapshot exists before draw measurement");
        Capture("XYDraw" + pose + "-before");
        for (var i = 0; i < 16; ++i) Paint();
        var begin = MonotonicNanoseconds(); for (var i = 0; i < 96; ++i) Paint(); var finish = MonotonicNanoseconds();
        lock (results) cohorts.Add(new Cohort(pose, begin, finish, 16, 96));
        Capture("XYDraw" + pose + "-after"); Mouse(0x202, start, 0);
        Require(!State().GetProperty("visual_interacting").GetBoolean(), "Fixed-pose XY draw cohort releases its actual host");
        File.WriteAllText(Path.Combine(artifacts, "cohorts.json"), JsonSerializer.Serialize(cohorts, Json));
    }
    void Paint()
    {
        Check(); desktop.VerifyWindow(canvas, app!);
        if (WxNative.InvalidateRect(canvas, 0, 0) == 0) throw new Win32Exception(Marshal.GetLastPInvokeError(), "InvalidateRect"); Send(canvas, 0xF, 0, 0);
    }
    JsonDocument ReadPublishedJson(string path)
    {
        JsonDocument? document = null;
        Wait(() =>
        {
            if (!File.Exists(path)) return false;
            try { document = JsonDocument.Parse(File.ReadAllText(path)); return true; }
            // Published observer files can briefly remain locked on Windows.
            catch (IOException error) when ((error.HResult & 0xffff) is 32 or 33) { return false; }
        }, "Readonly observer publication completed");
        return document!;
    }
    JsonElement State()
    {
        var path = Path.Combine(host, "visual-state-" + (++stateSequence) + ".json"); Require(!File.Exists(path), "Readonly state request has a fresh sequence");
        RequestObserver(0x7FFD, stateSequence);
        using var doc = ReadPublishedJson(path); lastState = doc.RootElement.Clone();
        Require(lastState.GetProperty("sequence").GetInt32() == stateSequence, "Readonly state response matches this request"); return lastState;
    }
    void Capture(string label)
    {
        for (var attempt = 0; attempt < 10; ++attempt)
        {
            var path = Path.Combine(host, "viewport-" + (++captureSequence) + ".json"); Require(!File.Exists(path), "Viewport capture request has a fresh sequence");
            RequestObserver(0x7FFE, captureSequence);
            using var json = ReadPublishedJson(path); var receipt = json.RootElement;
            Require(receipt.GetProperty("sequence").GetInt32() == captureSequence, "Viewport capture receipt matches this request");
            if (!receipt.GetProperty("current").GetBoolean())
            {
                Require(receipt.TryGetProperty("error", out var error) && error.GetString() == "captured frame is not the current provider delivery",
                    "A pending-delivery wait does not hide a render, allocation or readback failure");
                Thread.Sleep(10); continue;
            }
            Require(receipt.GetProperty("captured").GetBoolean() && receipt.GetProperty("swapped").GetBoolean(), "Actual normal viewport capture completes and swaps");
            File.Copy(Path.Combine(host, receipt.GetProperty("image").GetString()!), Path.Combine(artifacts, label + ".png"));
            File.Copy(path, Path.Combine(artifacts, label + "-capture.json")); return;
        }
        throw new TimeoutException("Ten bounded captures did not observe the current async subtitle delivery");
    }
    string Save(string label)
    {
        Command("Save"); Wait(() => !State().GetProperty("modified").GetBoolean(), "Actual wx save checkpoint");
        var copy = Path.Combine(artifacts, label + ".ass"); File.Copy(fixture, copy);
        string section = ""; List<string> retained = [];
        foreach (var line in File.ReadLines(copy))
        {
            if (line.StartsWith('[')) section = line;
            if (section is "[Script Info]" or "[V4+ Styles]" or "[Events]" or "[Aegisub Extradata]") retained.Add(line);
        }
        return string.Join('\n', retained);
    }
    void Command(string label) => ObservedCommand("Visual Port Test", label);
    void Macro(string name) => ObservedCommand("Automation", name);
    bool HasCommand(string menu, string label) { Check(); return WxNative.MenuLabels(main, menu).Contains(label, StringComparer.OrdinalIgnoreCase); }
    void ObservedCommand(string menu, string label)
    {
        Check(); desktop.VerifyWindow(main, app!); var id = ObservedWindowsCommand.FindMenuCommand(main, app!.Id, menu, label);
        var toolCommand = menu == "Visual Port Test" && (label is "Z" or "XY" or "Scale");
        Dictionary<string, bool> ObserveRadio(string stage)
        {
            Check(); desktop.VerifyWindow(main, app);
            var states = new[] { "Cross", "Z", "XY", "Scale" }.ToDictionary(name => name, name => WxNative.MenuChecked(main, menu, name));
            File.AppendAllText(Path.Combine(artifacts, "tool-radio.ndjson"), JsonSerializer.Serialize(new { stage, menu, label, id, pid = app.Id, radio = states, time_ns = MonotonicNanoseconds() }) + Environment.NewLine);
            return states;
        }
        if (toolCommand) ObserveRadio("before");
        File.AppendAllText(Path.Combine(artifacts, "commands.ndjson"), JsonSerializer.Serialize(new { menu, label, id, pid = app.Id, time_ns = MonotonicNanoseconds() }) + Environment.NewLine); Send(main, 0x111, id, 0);
        if (toolCommand) Require(ObserveRadio("after")[label], "Actual native radio state confirms the requested visual tool command");
    }
    void Mouse(uint message, Point point, int keys)
    {
        Check(); desktop.VerifyWindow(canvas, app!); WxNative.VerifyPoint(canvas, point.X, point.Y);
        File.AppendAllText(Path.Combine(artifacts, "input.ndjson"), JsonSerializer.Serialize(new { phase, message, point.X, point.Y, keys, shift = (keys & 4) != 0, control = (keys & 8) != 0, time_ns = MonotonicNanoseconds() }) + Environment.NewLine);
        Send(canvas, message, keys, unchecked((nint)(((long)(ushort)(short)point.Y << 16) | (ushort)(short)point.X)));
    }
    void RequestObserver(int command, int sequence)
    {
        Check(); desktop.VerifyWindow(main, app!);
        File.AppendAllText(Path.Combine(artifacts, "observer-requests.ndjson"), JsonSerializer.Serialize(new { command_id = command, sequence, pid = app!.Id, hwnd = $"0x{main:X}", time_ns = MonotonicNanoseconds() }) + Environment.NewLine);
        if (!WxNative.PostMessageW(main, 0x111, command, 0)) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Owned readonly observer command " + command.ToString("X"));
    }
    nint Send(nint window, uint message, nint wp, nint lp) { Check(); desktop.VerifyWindow(window, app!); return WxNative.Send(window, message, wp, lp, (uint)Math.Min(1000, RemainingMilliseconds())); }
    void Wait(Func<bool> predicate, string label)
    {
        var localDeadline = Math.Min(deadline, Stopwatch.GetTimestamp() + 10 * Stopwatch.Frequency);
        while (Stopwatch.GetTimestamp() < localDeadline) { Check(); if (predicate()) return; Thread.Sleep(10); } throw new TimeoutException(label);
    }
    void Check()
    {
        if (Stopwatch.GetTimestamp() >= deadline) throw new TimeoutException("Non-resettable 150-second wx contract deadline");
        if (app is not null && app.HasExited) throw new InvalidOperationException("Owned wx host exited early: " + desktop.ExitCode(app));
    }
    int RemainingMilliseconds() => Math.Max(1, checked((int)Math.Min(int.MaxValue, (deadline - Stopwatch.GetTimestamp()) * 1000 / Stopwatch.Frequency)));
    void Require(bool valid, string description) { ++checks; if (!valid) throw new InvalidOperationException(description); }
    void Case(string name, Action action)
    {
        phase = name; var before = checks;
        try { action(); Require(checks > before, "Case contains real assertions"); lock (results) results[name] = new("passed", checks - before, null); }
        catch (Exception error) { lock (results) results[name] = new("failed", checks - before, error.Message); throw; }
        WriteReports(null);
    }
    static JsonElement[] Rows(JsonElement state) => state.GetProperty("rows").EnumerateArray().ToArray();
    static int Id(JsonElement row) => row.GetProperty("source_id").GetInt32();
    static bool Folded(JsonElement state)
    {
        var rows = Rows(state);
        return rows.Length == 4 && rows.Take(3).All(r => r.GetProperty("selected").GetBoolean())
            && rows[0].GetProperty("display_row").GetInt32() == 0 && rows[1].GetProperty("display_row").GetInt32() == -1
            && rows[2].GetProperty("display_row").GetInt32() == -1 && state.GetProperty("display_rows").GetInt32() == 2
            && state.GetProperty("fold_groups").GetArrayLength() == 1 && state.GetProperty("fold_groups")[0].GetProperty("collapsed").GetBoolean()
            && state.GetProperty("active_id").GetInt32() == Id(rows[0]) && rows.Select(Id).All(id => id > 0);
    }
    static (double X, double Y) ContinuousPixel(JsonElement state, double x, double y)
    {
        var view = state.GetProperty("viewport"); return (view.GetProperty("x").GetDouble() + x * view.GetProperty("width").GetDouble() / 640, view.GetProperty("y").GetDouble() + y * view.GetProperty("height").GetDouble() / 480);
    }
    static Point Pixel(JsonElement state, double x, double y) { var p = ContinuousPixel(state, x, y); return new((int)Math.Round(p.X), (int)Math.Round(p.Y)); }
    static double Four(double value) => double.Parse(value.ToString("G4", CultureInfo.InvariantCulture), CultureInfo.InvariantCulture);
    double Scalar(string text, string tag)
    {
        var matches = Regex.Matches(text, @"\\" + tag + @"(-?[0-9]+(?:\.[0-9]+)?)"); Require(matches.Count == 1, "Exactly one owned scalar tag is serialized"); return double.Parse(matches[0].Groups[1].Value, CultureInfo.InvariantCulture);
    }
    double[] TupleTag(string text, string tag)
    {
        var matches = Regex.Matches(text, @"\\" + tag + @"\(([^)]*)\)"); Require(matches.Count == 1, "Exactly one owned tuple tag is serialized"); return matches[0].Groups[1].Value.Split(',').Select(s => double.Parse(s, CultureInfo.InvariantCulture)).ToArray();
    }
    static long MonotonicNanoseconds() { var ticks = Stopwatch.GetTimestamp(); return ticks / Stopwatch.Frequency * 1_000_000_000L + ticks % Stopwatch.Frequency * 1_000_000_000L / Stopwatch.Frequency; }

    void AnalyzeTrace()
    {
        var traces = Directory.GetFiles(Path.Combine(profile, "user", "perf-sessions"), "trace.ndjson", SearchOption.AllDirectories);
        Require(traces.Length == 1, "Exactly one current host trace session exists");
        var entries = File.ReadLines(traces[0]).Select(line => { using var d = JsonDocument.Parse(line); return d.RootElement.Clone(); }).ToArray();
        var metadata = entries.Select(e => e.GetProperty("payload")).Where(p => p.TryGetProperty("renderer_primary", out _)).ToArray();
        Require(metadata.Any(p => p.GetProperty("renderer_primary").GetString() is "OpenGL" or "opengl"), "Actual renderer memory receipt identifies OpenGL");
        Require(metadata.Any(p => p.TryGetProperty("subtitles_provider", out var value) && value.GetString() == "libass"), "Actual subtitle provider memory receipt identifies libass");
        List<object> metrics = [];
        foreach (var cohort in cohorts)
        {
            var samples = entries.Where(e => e.GetProperty("name").GetString() == "video_ui_duration"
                && e.GetProperty("t_monotonic_ns").GetInt64() >= cohort.Begin && e.GetProperty("t_monotonic_ns").GetInt64() <= cohort.End).Select(e => e.GetProperty("payload")).ToArray();
            var draw = samples.Where(p => p.GetProperty("phase").GetString() == "video_display.tool_snapshot.draw").Select(p => p.GetProperty("duration_ms").GetDouble()).Order().ToArray();
            Require(draw.Length >= 96 && draw.All(value => double.IsFinite(value) && value > 0), "Measured XY cohort contains at least 96 positive actual snapshot draws, not requested-input counts");
            var render = samples.Where(p => p.GetProperty("phase").GetString() == "video_display.render").Select(p => p.GetProperty("duration_ms").GetDouble()).Order().ToArray();
            var swap = samples.Where(p => p.GetProperty("phase").GetString() == "video_display.swap").Select(p => p.GetProperty("duration_ms").GetDouble()).Order().ToArray();
            metrics.Add(new { cohort.Name, actual_draws = draw.Length, warmup_paints = cohort.Warmups, requested_paints = cohort.Paints,
                p50_ms = draw[(int)Math.Ceiling(draw.Length * .5) - 1], p95_ms = draw[(int)Math.Ceiling(draw.Length * .95) - 1], max_ms = draw[^1], total_ms = draw.Sum(),
                render_wall = WallSummary(render), swap_wall = WallSummary(swap) });
        }
        File.WriteAllText(Path.Combine(artifacts, "timing.json"), JsonSerializer.Serialize(new { label = args[3], metrics, metadata, quantile = "nearest rank: sorted[ceil(N*p)-1]; milliseconds; all actual samples retained", limits = "Application wall scopes, including GL driver blocking; render includes swap. No CPU-cycle, GPU-timer, scan-out or physical-input claim; external predeclared draw P95 gate only" }, Json));
    }
    static object WallSummary(double[] values) => new { count = values.Length,
        p50_ms = values.Length == 0 ? (double?)null : values[(int)Math.Ceiling(values.Length * .5) - 1],
        p95_ms = values.Length == 0 ? (double?)null : values[(int)Math.Ceiling(values.Length * .95) - 1],
        max_ms = values.Length == 0 ? (double?)null : values[^1], total_ms = values.Sum() };
    void WriteReports(string? error)
    {
        lock (results)
        {
            if (error is not null && results.TryGetValue(phase, out var active) && active.Status != "failed")
                results[phase] = new("failed", active.Checks, error);
            File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { label = args[3], passed = error is null && results.Values.All(r => r.Status == "passed"), error, cases = results, cohorts }, Json));
            var suite = new XElement("testsuite", new XAttribute("name", "WxSharedTransformPort"), new XAttribute("tests", results.Count),
                new XAttribute("failures", results.Values.Count(r => r.Status == "failed")), new XAttribute("skipped", results.Values.Count(r => r.Status == "not-run")));
            foreach (var (name, result) in results)
            {
                var item = new XElement("testcase", new XAttribute("name", name), new XAttribute("classname", "WxSharedTransformPort"));
                if (result.Status == "failed") item.Add(new XElement("failure", result.Error)); else if (result.Status == "not-run") item.Add(new XElement("skipped", new XAttribute("message", "not-run"))); suite.Add(item);
            }
            if (error is not null) suite.Add(new XElement("system-err", error)); new XDocument(new XElement("testsuites", suite)).Save(Path.Combine(artifacts, "junit.xml"));
        }
    }
    sealed record TransformCase(string Name, string Tool, bool Origin = false, bool Shift = false, bool Control = false, bool Return = false);
    sealed record CaseResult(string Status, int Checks, string? Error);
    sealed record Cohort(string Name, long Begin, long End, int Warmups, int Paints);
    readonly record struct Point(int X, int Y);
}

sealed class DriverThreadDpiScope : IDisposable
{
    static readonly nint PerMonitorV2 = -4;
    readonly nint previous;
    readonly string journal;
    readonly int ownerThread = Environment.CurrentManagedThreadId;
    bool disposed;

    public DriverThreadDpiScope(string artifacts)
    {
        journal = Path.Combine(artifacts, "dpi-awareness.ndjson");
        previous = SetThreadDpiAwarenessContext(PerMonitorV2);
        if (previous == 0) throw new Win32Exception(Marshal.GetLastPInvokeError(), "SetThreadDpiAwarenessContext PMv2");
    }

    public void RecordActivation()
    {
        Record("before", previous);
        var active = GetThreadDpiAwarenessContext();
        Record("active", active);
        if (AreDpiAwarenessContextsEqual(active, PerMonitorV2) == 0) throw new InvalidOperationException("Driver thread did not enter per-monitor-v2 DPI awareness");
    }

    void Record(string stage, nint context)
    {
        var awareness = GetAwarenessFromDpiAwarenessContext(context);
        if (awareness < 0) throw new InvalidOperationException("Invalid driver thread DPI awareness context");
        File.AppendAllText(journal, JsonSerializer.Serialize(new
        {
            stage, pid = Environment.ProcessId, managed_thread_id = Environment.CurrentManagedThreadId, context = $"0x{context:X}", awareness,
            per_monitor_v2 = AreDpiAwarenessContextsEqual(context, PerMonitorV2) != 0,
            matches_previous = AreDpiAwarenessContextsEqual(context, previous) != 0
        }) + Environment.NewLine);
    }

    public void Dispose()
    {
        if (disposed) return;
        if (Environment.CurrentManagedThreadId != ownerThread) throw new InvalidOperationException("DPI awareness lease must be restored on its owning driver thread");
        if (SetThreadDpiAwarenessContext(previous) == 0) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Restore driver thread DPI awareness");
        disposed = true;
        var restored = GetThreadDpiAwarenessContext();
        Record("restored", restored);
        if (AreDpiAwarenessContextsEqual(restored, previous) == 0) throw new InvalidOperationException("Driver thread DPI awareness restoration did not match its prior context");
    }

    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)] static extern nint SetThreadDpiAwarenessContext(nint context);
    [DllImport("user32.dll", ExactSpelling = true)] static extern nint GetThreadDpiAwarenessContext();
    [DllImport("user32.dll", ExactSpelling = true)] static extern int GetAwarenessFromDpiAwarenessContext(nint context);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int AreDpiAwarenessContextsEqual(nint first, nint second);
}

static class WxNative
{
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] struct Point { public int X, Y; }
    [return: MarshalAs(UnmanagedType.Bool)] delegate bool EnumWindow(nint window, nint parameter);
    [DllImport("user32.dll", ExactSpelling = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool EnumChildWindows(nint parent, EnumWindow callback, nint parameter);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int IsWindowVisible(nint window);
    [DllImport("user32.dll", ExactSpelling = true)] static extern nint GetParent(nint window);
    [DllImport("user32.dll", ExactSpelling = true)] public static extern int GetClientRect(nint window, out Rect rect);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int ClientToScreen(nint window, ref Point point);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int ScreenToClient(nint window, ref Point point);
    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)] public static extern int SetWindowPos(nint window, nint after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)] public static extern int InvalidateRect(nint window, nint rectangle, int erase);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", ExactSpelling = true, SetLastError = true)] static extern nint SendMessageTimeout(nint window, uint message, nint wp, nint lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] public static extern bool PostMessageW(nint window, uint message, nint wp, nint lp);
    [DllImport("user32.dll", ExactSpelling = true)] static extern nint GetMenu(nint window);
    [DllImport("user32.dll", ExactSpelling = true)] static extern nint GetSubMenu(nint menu, int position);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int GetMenuItemCount(nint menu);
    [DllImport("user32.dll", ExactSpelling = true)] static extern uint GetMenuState(nint menu, uint position, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetMenuStringW", ExactSpelling = true, CharSet = CharSet.Unicode)] static extern int GetMenuString(nint menu, uint position, [Out] char[] text, int length, uint flags);
    public static bool IsVisible(nint window) => IsWindowVisible(window) != 0;
    public static nint[] Children(nint parent) { List<nint> list = []; EnumWindow callback = (h, _) => { list.Add(h); return true; }; if (!EnumChildWindows(parent, callback, 0)) throw new InvalidOperationException("EnumChildWindows"); GC.KeepAlive(callback); return list.ToArray(); }
    public static nint Send(nint window, uint message, nint wp, nint lp, uint timeout) { if (SendMessageTimeout(window, message, wp, lp, 0x22, timeout, out var result) == 0) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Owned native message " + message.ToString("X")); return result; }
    public static void VerifyPoint(nint canvas, int x, int y)
    {
        if (GetClientRect(canvas, out var area) == 0 || x < 0 || y < 0 || x >= area.Right || y >= area.Bottom) throw new InvalidOperationException("Input is outside the actual canvas client");
        var screen = new Point { X = x, Y = y }; if (ClientToScreen(canvas, ref screen) == 0) throw new InvalidOperationException("ClientToScreen");
        for (var parent = GetParent(canvas); parent != 0; parent = GetParent(parent))
        {
            var point = screen;
            if (ScreenToClient(parent, ref point) == 0 || GetClientRect(parent, out var bounds) == 0 || point.X < 0 || point.Y < 0 || point.X >= bounds.Right || point.Y >= bounds.Bottom) throw new InvalidOperationException("Input is clipped by its actual native ancestor");
        }
    }
    static string Label(nint menu, int index) { var text = new char[512]; var length = GetMenuString(menu, (uint)index, text, text.Length, 0x400); if (length >= text.Length - 1) throw new InvalidOperationException("Native menu label truncated"); return new string(text, 0, length).Split('\t')[0].Replace("&", "", StringComparison.Ordinal); }
    static nint Submenu(nint window, string name) { var menu = GetMenu(window); for (var i = 0; i < GetMenuItemCount(menu); ++i) if (Label(menu, i).Equals(name, StringComparison.OrdinalIgnoreCase)) return GetSubMenu(menu, i); throw new InvalidOperationException("Native menu absent: " + name); }
    public static string[] MenuLabels(nint window, string name) { var menu = Submenu(window, name); return Enumerable.Range(0, GetMenuItemCount(menu)).Select(i => Label(menu, i)).ToArray(); }
    public static bool MenuChecked(nint window, string menuName, string label) { var menu = Submenu(window, menuName); for (var i = 0; i < GetMenuItemCount(menu); ++i) if (Label(menu, i).Equals(label, StringComparison.OrdinalIgnoreCase)) return (GetMenuState(menu, (uint)i, 0x400) & 8) != 0; throw new InvalidOperationException("Native menu item absent: " + label); }
}
