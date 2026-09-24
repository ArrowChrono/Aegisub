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
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

if (args.Length is < 3 or > 5 || (args.Length == 5 && args[4] != "--inspect"))
{
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/motion-track-roi-input.cs -- EXE VIDEO ARTIFACTS [PROVIDER] [--inspect]");
    return 2;
}

var executable = Path.GetFullPath(args[0]);
var video = Path.GetFullPath(args[1]);
var artifacts = Path.GetFullPath(args[2]);
var provider = args.Length >= 4 ? args[3] : "libass";
var inspect = args.Length == 5;
if (Directory.Exists(artifacts) && Directory.EnumerateFileSystemEntries(artifacts).Any())
    throw new ArgumentException("Artifacts directory must be empty");
Directory.CreateDirectory(artifacts);
Process? app = null;
using var supervisor = new Timer(_ =>
{
    Console.Error.WriteLine("motion_track_roi.error=total_timeout_180s");
    try { if (app is { HasExited: false }) app.Kill(entireProcessTree: true); }
    finally { Environment.Exit(124); }
}, null, TimeSpan.FromSeconds(inspect ? 30 : 180), Timeout.InfiniteTimeSpan);

var outcomes = new List<object>();
try
{
    var fixture = Path.Combine(artifacts, "scenario.ass");
    File.WriteAllText(fixture, """
        [Script Info]
        Title: Motion track ROI input contracts
        ScriptType: v4.00+
        PlayResX: 640
        PlayResY: 480
        LayoutResX: 640
        LayoutResY: 480
        ScaledBorderAndShadow: yes
        YCbCr Matrix: TV.601
        [V4+ Styles]
        Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
        Style: Default,Arial,30,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1
        [Events]
        Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
        Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\pos(320,360)}ROI must not edit this line
        Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,{\pos(320,400)}Unselected sentinel
        """, new UTF8Encoding(false));
    var original = File.ReadAllBytes(fixture);
    var profile = Path.Combine(artifacts, "profile");
    Directory.CreateDirectory(Path.Combine(profile, "user"));
    File.WriteAllText(Path.Combine(profile, "user", "config.json"), JsonSerializer.Serialize(new
    {
        App = new Dictionary<string, object> { ["Auto"] = new Dictionary<string, object> { ["Check For Updates"] = false, ["Load Linked Files"] = 0 } },
        Subtitle = new Dictionary<string, object> { ["Provider"] = provider },
        Video = new Dictionary<string, object> { ["Open Audio"] = false, ["Scale with DPI"] = false, ["Default Zoom"] = 7,
            ["Renderer"] = new { Backend = "opengl" }, ["Skia Tools"] = new { Enabled = false } },
    }));
    var host = Path.Combine(artifacts, "host");
    Directory.CreateDirectory(host);
    var start = new ProcessStartInfo(executable) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Minimized };
    foreach (var arg in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", host, "--open", fixture, "--open", video })
        start.ArgumentList.Add(arg);
    start.Environment["AEGISUB_ENABLE_SKIA_VIDEO_TOOLS"] = "0";
    start.Environment.Remove("AEGISUB_PERF_TRACE");
    app = Process.Start(start) ?? throw new InvalidOperationException("Could not start Aegisub");
    AutomationProtocol.WaitForReadyArtifact(Path.Combine(host, "ready.json"), app, TimeSpan.FromSeconds(30));
    var main = UiaDriver.WaitForMainWindow(app, TimeSpan.FromSeconds(30));
    var mainHwnd = new nint(main.Current.NativeWindowHandle);
    Native.ShowWindow(mainHwnd, 4);
    if (!Native.SetWindowPos(mainHwnd, 0, 20, 20, 1400, 950, 0x0050))
        throw new InvalidOperationException("Could not position main window");
    Thread.Sleep(1200);
    var canvas = Native.Children(mainHwnd).FirstOrDefault(h => Native.ClassName(h) == "wxGLCanvas" && Native.IsWindowVisible(h));
    if (canvas == 0 || !Native.GetClientRect(canvas, out var rect) || rect.Right != 640 || rect.Bottom != 480)
        throw new InvalidOperationException("This exact-coordinate fixture requires a 640x480 video at 100% zoom");
    var failures = new List<string>();
    foreach (var kind in new[] { "band", "move", "resize-crossing", "resize-no-motion", "resize-return" })
    {
        Roi? reference = null;
        foreach (var terminalMotion in kind == "resize-no-motion" ? new[] { false } : new[] { true, false })
        {
            var label = kind + (terminalMotion ? "-reference" : "-release-gap");
            var dialog = OpenDialog(main, app);
            var initial = ReadRoi(dialog, artifacts);
            if (inspect) { CloseDialog(dialog); UiaDriver.RequestCleanClose(app, TimeSpan.FromSeconds(10)); return 2; }
            if (initial != new Roi(40, 30, 24, 16))
                throw new InvalidOperationException($"{label}: dialog did not reset its ROI: {initial}");
            if (kind != "band")
                Drag(canvas, new(100, 80), [new(140, 110), new(165, 125)], new(220, 160), true);
            var expected = kind switch
            {
                "band" => new Roi(100, 80, 120, 80),
                "move" => new Roi(190, 160, 120, 80),
                "resize-crossing" => new Roi(80, 50, 20, 30),
                _ => new Roi(100, 80, 120, 80),
            };
            if (kind == "band") Drag(canvas, new(100, 80), [new(140, 110), new(165, 125)], new(220, 160), terminalMotion);
            else if (kind == "move") Drag(canvas, new(160, 120), [new(200, 160), new(215, 175)], new(250, 200), terminalMotion);
            else if (kind == "resize-crossing") Drag(canvas, new(220, 160), [new(230, 170), new(180, 120)], new(80, 50), terminalMotion);
            else if (kind == "resize-no-motion") Drag(canvas, new(218, 158), [], new(218, 158), false);
            else Drag(canvas, new(220, 160), [new(250, 190), new(260, 200)], new(220, 160), terminalMotion);
            var actual = ReadRoi(dialog, artifacts);
            ScreenCapture.SaveWindowPng(dialog, Path.Combine(artifacts, label + "-dialog.png"));
            ScreenCapture.SaveWindowPng(main, Path.Combine(artifacts, label + "-main.png"));
            if (terminalMotion) reference = actual;
            var passed = actual == expected && (terminalMotion || kind == "resize-no-motion" || actual == reference);
            outcomes.Add(new { scenario = label, initial, expected, actual, reference, capture_released = Native.Capture(canvas) == 0, passed });
            if (!passed) failures.Add($"{label}: expected {expected}, actual {actual}");
            CloseDialog(dialog);
        }
    }

    var cancelDialog = OpenDialog(main, app);
    Drag(canvas, new(100, 80), [new(140, 110)], new(220, 160), true);
    Native.Mouse(canvas, 0x0200, new(160, 120), 0);
    Native.Mouse(canvas, 0x0201, new(160, 120), 1);
    RequireCapture(canvas, true);
    Native.Mouse(canvas, 0x0200, new(175, 135), 1);
    var beforeCancel = ReadRoi(cancelDialog, artifacts);
    Native.Send(canvas, 0x001F, 0, 0);
    RequireCapture(canvas, false);
    Native.Mouse(canvas, 0x0202, new(400, 300), 0);
    Native.Mouse(canvas, 0x0200, new(430, 330), 0);
    var afterCancel = ReadRoi(cancelDialog, artifacts);
    var cancelPassed = beforeCancel == new Roi(115, 95, 120, 80) && afterCancel == beforeCancel;
    outcomes.Add(new { scenario = "capture-cancel-no-fabricated-endpoint", beforeCancel, afterCancel, passed = cancelPassed });
    if (!cancelPassed) failures.Add("Capture cancellation changed ROI");
    ScreenCapture.SaveWindowPng(cancelDialog, Path.Combine(artifacts, "cancel-dialog.png"));
    CloseDialog(cancelDialog);

    app.Refresh();
    if (app.MainWindowTitle.TrimStart().StartsWith("*", StringComparison.Ordinal) || !original.SequenceEqual(File.ReadAllBytes(fixture)))
        failures.Add("ROI input modified subtitles");
    UiaDriver.RequestCleanClose(app, TimeSpan.FromSeconds(15));
    var providerLog = Directory.GetFiles(Path.Combine(profile, "user", "log"), "*.ndjson")
        .SelectMany(File.ReadLines).Select(line => JsonDocument.Parse(line)).ToArray();
    try
    {
        var selected = providerLog.Select(d => d.RootElement).Where(e =>
            e.TryGetProperty("section", out var section) && section.GetString() == "subtitle/provider/select")
            .Select(e => e.GetProperty("message").GetString()).Where(m => m?.StartsWith("Selected subtitles provider: ", StringComparison.Ordinal) == true).Distinct().ToArray();
        if (selected.Length != 1 || selected[0] != "Selected subtitles provider: " + provider)
            failures.Add("Requested subtitle provider was not selected exactly");
    }
    finally { foreach (var document in providerLog) document.Dispose(); }
    File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { provider, outcomes, failures, passed = failures.Count == 0 }, new JsonSerializerOptions { WriteIndented = true }));
    foreach (var failure in failures) Console.Error.WriteLine(failure);
    return failures.Count == 0 ? 0 : 1;
}
catch (Exception error)
{
    File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { outcomes, error = error.Message, passed = false }, new JsonSerializerOptions { WriteIndented = true }));
    Console.Error.WriteLine(error);
    return 1;
}
finally
{
    if (app is { HasExited: false })
    {
        app.Kill(entireProcessTree: true);
        app.WaitForExit(5000);
    }
    app?.Dispose();
}

static AutomationElement OpenDialog(AutomationElement main, Process app)
{
    var command = main.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>()
        .FirstOrDefault(e => e.Current.Name.Contains("Open the motion tracking dialog", StringComparison.OrdinalIgnoreCase)
            && e.TryGetCurrentPattern(InvokePattern.Pattern, out _))
        ?? throw new InvalidOperationException("Motion Track toolbar command is unavailable");
    UiaDriver.Invoke(command);
    var deadline = Stopwatch.GetTimestamp() + 10 * Stopwatch.Frequency;
    while (Stopwatch.GetTimestamp() < deadline)
    {
        var handle = Native.TopWindows(app.Id).FirstOrDefault(h => Native.Title(h) == "Motion Track");
        if (handle != 0)
        {
            Native.SetWindowPos(handle, 0, 950, 50, 0, 0, 0x0015);
            return AutomationElement.FromHandle(handle);
        }
        Thread.Sleep(50);
    }
    throw new TimeoutException("Motion Track dialog did not open; windows=" + string.Join("; ", Native.TopWindows(app.Id).Select(Native.Title)));
}

static void CloseDialog(AutomationElement dialog)
{
    var button = UiaDriver.FindEnabledInvokableButton(dialog, "Close") ?? throw new InvalidOperationException("Close button unavailable");
    UiaDriver.Invoke(button);
    Thread.Sleep(100);
}

static Roi ReadRoi(AutomationElement dialog, string artifacts)
{
    var elements = dialog.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
    File.WriteAllText(Path.Combine(artifacts, "roi-uia-latest.json"), JsonSerializer.Serialize(elements.Select(e => new
    {
        name = e.Current.Name,
        control_type = e.Current.ControlType.ProgrammaticName,
        class_name = e.Current.ClassName,
        rect = new { left = e.Current.BoundingRectangle.Left, top = e.Current.BoundingRectangle.Top, width = e.Current.BoundingRectangle.Width, height = e.Current.BoundingRectangle.Height },
        patterns = e.GetSupportedPatterns().Select(p => p.ProgrammaticName).ToArray(),
        value = e.TryGetCurrentPattern(RangeValuePattern.Pattern, out var range) ? ((RangeValuePattern)range).Current.Value.ToString(System.Globalization.CultureInfo.InvariantCulture) : UiaDriver.ReadElementState(e).Value,
    }), new JsonSerializerOptions { WriteIndented = true }));
    ScreenCapture.SaveWindowPng(dialog, Path.Combine(artifacts, "roi-dialog-latest.png"));
    int Read(string name)
    {
        var spinner = elements.Single(e => e.Current.Name == name && e.Current.ControlType == ControlType.Spinner);
        var value = ((RangeValuePattern)spinner.GetCurrentPattern(RangeValuePattern.Pattern)).Current.Value;
        if (value != Math.Truncate(value)) throw new InvalidOperationException($"{name}: non-integral ROI value");
        return checked((int)value);
    }
    return new(Read("ROI X"), Read("ROI Y"), Read("ROI W"), Read("ROI H"));
}

static void Drag(nint canvas, Point start, Point[] motions, Point end, bool terminalMotion)
{
    Native.Mouse(canvas, 0x0200, start, 0);
    Native.Mouse(canvas, 0x0201, start, 1);
    RequireCapture(canvas, true);
    foreach (var point in motions) Native.Mouse(canvas, 0x0200, point, 1);
    if (terminalMotion) Native.Mouse(canvas, 0x0200, end, 1);
    RequireCapture(canvas, true);
    Native.Mouse(canvas, 0x0202, end, 0);
    RequireCapture(canvas, false);
}

static void RequireCapture(nint canvas, bool captured)
{
    if (captured ? Native.Capture(canvas) != canvas : Native.Capture(canvas) != 0)
        throw new InvalidOperationException(captured ? "ROI input did not retain capture" : "ROI input did not release capture");
}

readonly record struct Point(int X, int Y);
sealed record Roi(int X, int Y, int W, int H);
static class Native
{
    public static void Mouse(nint window, uint message, Point p, int keys) => Send(window, message, keys, (nint)((p.Y << 16) | (p.X & 0xffff)));
    public static void Send(nint window, uint message, nint wParam, nint lParam)
    {
        if (SendMessageTimeout(window, message, wParam, lParam, 3, 3000, out _) == 0)
            throw new TimeoutException($"Native input timed out: {message:X}");
    }
    public static string ClassName(nint window)
    {
        var text = new StringBuilder(256);
        GetClassName(window, text, text.Capacity);
        return text.ToString();
    }
    public static List<nint> Children(nint parent)
    {
        var children = new List<nint>();
        EnumChildWindows(parent, (window, _) => { children.Add(window); return true; }, 0);
        return children;
    }
    public static List<nint> TopWindows(int pid)
    {
        var windows = new List<nint>();
        EnumWindows((window, _) => { GetWindowThreadProcessId(window, out var owner); if (owner == pid) windows.Add(window); return true; }, 0);
        return windows;
    }
    public static string Title(nint window)
    {
        var text = new StringBuilder(512);
        GetWindowText(window, text, text.Capacity);
        return text.ToString();
    }
    public static nint Capture(nint canvas)
    {
        var state = new GuiThreadInfo { Size = (uint)Marshal.SizeOf<GuiThreadInfo>() };
        if (!GetGUIThreadInfo(GetWindowThreadProcessId(canvas, out _), ref state))
            throw new InvalidOperationException("Could not read GUI capture");
        return state.Capture;
    }
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] struct GuiThreadInfo { public uint Size, Flags; public nint Active, Focus, Capture, MenuOwner, MoveSize, Caret; public Rect CaretRect; }
    delegate bool EnumProc(nint window, nint parameter);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(nint parent, EnumProc callback, nint parameter);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, nint parameter);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(nint window, StringBuilder text, int count);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(nint window, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] public static extern bool GetClientRect(nint window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool ShowWindow(nint window, int command);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(nint window, nint insertAfter, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(nint window, out uint process);
    [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(uint thread, ref GuiThreadInfo state);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")] static extern nint SendMessageTimeout(nint window, uint message, nint wParam, nint lParam, uint flags, uint timeout, out nint result);
}
