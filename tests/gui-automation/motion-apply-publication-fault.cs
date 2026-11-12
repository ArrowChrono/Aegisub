#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.Diagnostics;
using System.IO;
using System.Text;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

if (args.Length is < 2 or > 3) throw new ArgumentException("Expected executable, empty artifacts directory and optional case name");
var executable = Path.GetFullPath(args[0]);
var artifacts = Path.GetFullPath(args[1]);
if (Directory.Exists(artifacts) && Directory.EnumerateFileSystemEntries(artifacts).Any())
    throw new ArgumentException("Artifacts directory must be empty");
Directory.CreateDirectory(artifacts);
var results = new List<object>();
var failed = false;
var cases = new[] { "motion-apply-early", "motion-apply-selection", "motion-apply-early-karaoke", "motion-apply-selection-karaoke", "motion-apply-early-grid", "motion-apply-selection-grid", "motion-apply-file-rebind", "motion-apply-style-rebind" };
if (args.Length == 3 && !cases.Contains(args[2], StringComparer.Ordinal)) throw new ArgumentException("Unknown case name");
foreach (var kind in cases.Where(kind => args.Length != 3 || kind == args[2]))
{
    var fileRebind = kind == "motion-apply-file-rebind";
    var styleRebind = kind == "motion-apply-style-rebind";
    var faultKind = fileRebind || styleRebind ? "motion-apply-early" : kind;
    var directory = Path.Combine(artifacts, kind);
    var profile = Path.Combine(directory, "profile");
    var host = Path.Combine(directory, "host");
    Directory.CreateDirectory(Path.Combine(profile, "user"));
    Directory.CreateDirectory(host);
    File.WriteAllText(Path.Combine(profile, "user", "config.json"), """
        {"App":{"Language":"en_US","Auto":{"Check For Updates":false,"Load Linked Files":1}},"Subtitle":{"Use STC":false},"Video":{"Open Audio":false,"Renderer":{"Backend":"opengl"}}}
        """);
    var fixture = Path.Combine(directory, "scenario.ass");
    File.WriteAllText(fixture, """
        [Script Info]
        ScriptType: v4.00+
        PlayResX: 640
        PlayResY: 480
        [Aegisub Project Garbage]
        Video File: ?dummy:10.000000:20:640:480:0:0:0:
        Audio File: dummy-audio:silence?sr=44100&bd=16&ch=1&ln=88200
        [V4+ Styles]
        Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
        Style: Default,Arial,30,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1
        Style: Emphasis,Arial,30,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1
        [Events]
        Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
        Dialogue: 0,0:00:00.00,0:00:01.95,Default,,0,0,0,,{\pos(320,360)}Borrowed target
        Dialogue: 0,0:00:00.00,0:00:01.95,Default,,0,0,0,,Sentinel
        """);
    if (fileRebind)
        File.WriteAllText(Path.Combine(host, "rebind.ass"), File.ReadAllText(fixture)
            .Replace("Style: Default,", "Style: RebindOnly,", StringComparison.Ordinal)
            .Replace("Style: Emphasis,", "Style: RebindAlternate,", StringComparison.Ordinal)
            .Replace(",Default,,0,0,0,,", ",RebindOnly,LoadedActor,0,0,0,LoadedEffect,", StringComparison.Ordinal));
    var start = new ProcessStartInfo(executable) { UseShellExecute = false };
    foreach (var arg in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", host, "--open", fixture })
        start.ArgumentList.Add(arg);
    start.Environment["AEGISUB_GUI_TEST_WX_ASSERTIONS"] = "1";
    using var app = Process.Start(start) ?? throw new InvalidOperationException("Could not start native host");
    using var supervisor = new Timer(_ => { try { File.WriteAllText(Path.Combine(directory, "supervisor-timeout.json"), JsonSerializer.Serialize(new { kind, timeoutSeconds = 60 })); if (!app.HasExited) app.Kill(true); } finally { Environment.Exit(124); } }, null, TimeSpan.FromSeconds(60), Timeout.InfiniteTimeSpan);
    try
    {
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(host, "ready.json"), app, TimeSpan.FromSeconds(25));
        var main = UiaDriver.WaitForMainWindow(app, TimeSpan.FromSeconds(15));
        var hwnd = new nint(main.Current.NativeWindowHandle);
        var canvas = Native.Children(hwnd).FirstOrDefault(child => Native.ClassName(child) == "wxGLCanvas");
        if (canvas == 0) throw new InvalidOperationException("Native canvas missing");
        var sequence = 0;
        JsonElement Request(string requestKind)
        {
            var token = Guid.NewGuid().ToString("N");
            var current = ++sequence;
            File.WriteAllText(Path.Combine(host, "wx-boundary-request.json"), JsonSerializer.Serialize(new {
                sequence = current, token, kind = requestKind, canvas_identity = (long)canvas, x = 0, y = 0
            }));
            using var publication = new FileSystemWatcher(host) { EnableRaisingEvents = true };
            if (!Native.PostMessage(hwnd, 0x0111, 0x7FF6, 0)) throw new InvalidOperationException("Could not post native probe");
            var deadline = Stopwatch.StartNew();
            var receipt = Path.Combine(host, $"wx-boundary-{current}.json");
            FileStream? stream = null;
            while (!app.HasExited && deadline.Elapsed < TimeSpan.FromSeconds(15))
            {
                if (File.Exists(receipt))
                {
                    try { stream = new FileStream(receipt, FileMode.Open, FileAccess.Read, FileShare.Read); }
                    catch (IOException error) when (error.HResult is unchecked((int)0x80070020) or unchecked((int)0x80070021))
                    {
                        File.AppendAllText(Path.Combine(directory, "publication-pending.ndjson"), JsonSerializer.Serialize(new { current, error.HResult }) + Environment.NewLine);
                    }
                    if (stream != null) break;
                }
                publication.WaitForChanged(WatcherChangeTypes.Created | WatcherChangeTypes.Renamed | WatcherChangeTypes.Changed, Math.Max(1, (int)Math.Min(50, 15000 - deadline.ElapsedMilliseconds)));
            }
            using var receiptStream = stream ?? throw new TimeoutException("Current observer publication was not readable before its deadline");
            using var json = JsonDocument.Parse(receiptStream);
            var reply = json.RootElement;
            if (reply.GetProperty("status").GetString() != "dispatched" || reply.GetProperty("token").GetString() != token)
                throw new InvalidOperationException(reply.ToString());
            return reply.Clone();
        }
        var root = Request(faultKind);
        var probe = root.GetProperty("motion_apply");
        var setup = probe.GetProperty("pending_text_before").GetBoolean()
            && probe.GetProperty("styling_borrow_before").GetBoolean()
            && probe.GetProperty("visual_borrow_before").GetBoolean()
            && probe.GetProperty("karaoke_present").GetBoolean()
            && (kind.EndsWith("-grid", StringComparison.Ordinal)
                ? probe.GetProperty("grid_capture_before").GetBoolean()
                : probe.GetProperty("audio_marker_before").GetBoolean() && probe.GetProperty("audio_dragged_before").GetBoolean())
            && probe.GetProperty("drag_features_before").GetInt32() > 0
            && probe.GetProperty("drag_selected_features_before").GetInt32() > 0
            && probe.GetProperty("drag_cached_selection_before").GetInt32() > 0
            && probe.GetProperty("drag_visibility_before").GetBoolean()
            && probe.GetProperty("karaoke_timing_enabled").GetBoolean() == kind.EndsWith("-karaoke", StringComparison.Ordinal);
        if (!setup) throw new InvalidOperationException("Native borrower setup was incomplete: " + probe);
        var passed = probe.GetProperty("publication_failed").GetBoolean()
            && probe.GetProperty("fault_count").GetInt32() == 1
            && probe.GetProperty("undo_available").GetBoolean()
            && probe.GetProperty("row_count").GetInt32() == (faultKind.StartsWith("motion-apply-early", StringComparison.Ordinal) ? 21 : 2)
            && probe.GetProperty("all_borrows_live").GetBoolean()
            && !probe.GetProperty("audio_marker_present").GetBoolean()
            && !probe.GetProperty("audio_dragged_present").GetBoolean()
            && !probe.GetProperty("audio_scroll_timer").GetBoolean();
        var audioWindow = (nint)probe.GetProperty("capture_owner_hwnd").GetInt64();
        var audioPoint = (nint)probe.GetProperty("capture_owner_point").GetInt64();
        if (!Native.PostMessage(audioWindow, 0x0202, 0, audioPoint)) throw new InvalidOperationException("Could not post ordinary audio mouse-up");
        var captureReleased = Request("motion-apply-inspect");
        passed &= !captureReleased.GetProperty("motion_apply").GetProperty("audio_capture").GetBoolean()
            && !captureReleased.GetProperty("motion_apply").GetProperty("grid_capture").GetBoolean();
        if (!probe.GetProperty("audio_timing_ready").GetBoolean())
        {
            var audio = (nint)probe.GetProperty("audio_hwnd").GetInt64();
            var point = (nint)probe.GetProperty("audio_marker_point").GetInt64();
            if (!Native.PostMessage(audio, 0x0201, 1, point) || !Native.PostMessage(audio, 0x0202, 0, point))
                throw new InvalidOperationException("Could not post retired timing input");
            passed &= probe.GetProperty("audio_marker_count").GetInt32() == 0;
        }
        if (kind.StartsWith("motion-apply-selection", StringComparison.Ordinal))
            passed &= probe.GetProperty("initial_exact_all_ready").GetBoolean();
        var editor = (nint)probe.GetProperty("edit_hwnd").GetInt64();
        var styling = (nint)probe.GetProperty("styling_hwnd").GetInt64();
        Native.ReplaceText(editor, "Retired native text must not commit");
        Native.SetText(styling, "Emphasis");
        if (!Native.PostMessage(styling, 0x0100, 13, 0)) throw new InvalidOperationException("Could not post Styling Enter");
        var retired = Request("motion-apply-inspect");
        var stableFields = new[] { "document_revision", "document_generation", "undo_empty", "redo_empty", "undo_description", "redo_description", "active_id", "selected_ids" };
        var unchanged = stableFields.All(field => root.GetProperty("after").GetProperty(field).GetRawText() == retired.GetProperty("after").GetProperty(field).GetRawText())
            && root.GetProperty("after").GetProperty("rows").GetRawText() == retired.GetProperty("after").GetProperty("rows").GetRawText()
            && root.GetProperty("after").GetProperty("document_revision").GetInt64() == retired.GetProperty("after").GetProperty("document_revision").GetInt64();
        var textDelivered = retired.GetProperty("motion_apply").GetProperty("edit_text").GetString() == "Retired native text must not commit"
            && retired.GetProperty("motion_apply").GetProperty("styling_text").GetString() == "Emphasis";
        var styleRequested = styleRebind ? Request("motion-apply-retired-style") : retired;
        var requestedStyle = styleRequested.GetProperty("motion_apply");
        var styleWhileRetired = !styleRebind || !requestedStyle.GetProperty("grid_ready").GetBoolean()
            && requestedStyle.GetProperty("grid_font_size").GetInt32() == requestedStyle.GetProperty("requested_grid_font_size").GetInt32()
            && requestedStyle.GetProperty("grid_background").GetInt64() == requestedStyle.GetProperty("requested_grid_background").GetInt64()
            && requestedStyle.GetProperty("grid_line_height").GetInt32() > retired.GetProperty("motion_apply").GetProperty("grid_line_height").GetInt32();
        var undo = Native.FindUndo(hwnd);
        if (!fileRebind && !Native.PostMessage(hwnd, 0x0111, undo, 0)) throw new InvalidOperationException("Could not post Undo");
        var recovered = Request(fileRebind ? "motion-apply-load-rebind" : "motion-apply-inspect");
        var recoveredStyle = recovered.GetProperty("motion_apply");
        var styleRebound = !styleRebind || recoveredStyle.GetProperty("grid_ready").GetBoolean()
            && recoveredStyle.GetProperty("grid_font_size").GetInt32() == requestedStyle.GetProperty("requested_grid_font_size").GetInt32()
            && recoveredStyle.GetProperty("grid_background").GetInt64() == requestedStyle.GetProperty("requested_grid_background").GetInt64()
            && recoveredStyle.GetProperty("grid_line_height").GetInt32() == requestedStyle.GetProperty("grid_line_height").GetInt32();
        var metadataRebound = !fileRebind || new[] {
            (Field: "style_combo", Candidates: new[] { "RebindAlternate", "RebindOnly" }, Value: "RebindOnly"),
            (Field: "actor_combo", Candidates: new[] { "LoadedActor" }, Value: "LoadedActor"),
            (Field: "effect_combo", Candidates: new[] { "LoadedEffect" }, Value: "LoadedEffect")
        }.All(expected => {
            var combo = recovered.GetProperty("motion_apply").GetProperty(expected.Field);
            return combo.GetProperty("candidates").EnumerateArray().Select(value => value.GetString()).Order(StringComparer.Ordinal)
                .SequenceEqual(expected.Candidates.Order(StringComparer.Ordinal))
                && combo.GetProperty("value").GetString() == expected.Value;
        });
        var rebound = recovered.GetProperty("motion_apply").GetProperty("all_borrows_live").GetBoolean()
            && new[] { "grid_ready", "edit_ready", "styling_ready", "visual_ready", "audio_timing_ready" }.All(field => recovered.GetProperty("motion_apply").GetProperty(field).GetBoolean())
            && recovered.GetProperty("motion_apply").GetProperty("row_count").GetInt32() == (faultKind.StartsWith("motion-apply-early", StringComparison.Ordinal) ? 2 : 21);
        Native.ReplaceText(editor, "Recovered native input commits");
        var edited = Request("motion-apply-inspect");
        var committed = edited.GetProperty("after").GetProperty("rows").EnumerateArray().Any(row => row.GetProperty("text").GetString() == "Recovered native input commits")
            && edited.GetProperty("after").GetProperty("document_revision").GetInt64() > recovered.GetProperty("after").GetProperty("document_revision").GetInt64();
        if (!Native.PostMessage(hwnd, 0x0111, undo, 0)) throw new InvalidOperationException("Could not undo native edit");
        var restored = Request("motion-apply-inspect");
        if (kind.StartsWith("motion-apply-selection", StringComparison.Ordinal))
        {
            if (!Native.PostMessage(hwnd, 0x0111, undo, 0)) throw new InvalidOperationException("Could not undo initial Exact Apply");
            restored = Request("motion-apply-inspect");
        }
        var clean = !restored.GetProperty("after").GetProperty("modified").GetBoolean();
        passed &= unchanged && textDelivered && rebound && metadataRebound && styleWhileRetired && styleRebound && committed && clean
            && !retired.GetProperty("motion_apply").GetProperty("audio_capture").GetBoolean()
            && !retired.GetProperty("motion_apply").GetProperty("grid_capture").GetBoolean();
        if (!clean) throw new InvalidOperationException("Undo did not restore the clean fixture");
        UiaDriver.RequestCleanClose(app, TimeSpan.FromSeconds(10));
        passed &= app.ExitCode == 0;
        failed |= !passed;
        results.Add(new { name = kind, passed, unchanged, textDelivered, rebound, metadataRebound, styleWhileRetired, styleRebound, committed, clean, exitCode = app.ExitCode, evidence = probe.Clone() });
    }
    catch (Exception error)
    {
        failed = true;
        results.Add(new { name = kind, passed = false, error = error.ToString(), processExited = app.HasExited });
        File.WriteAllText(Path.Combine(directory, "failure.json"), JsonSerializer.Serialize(new { kind, error = error.ToString() }, new JsonSerializerOptions { WriteIndented = true }));
    }
    finally
    {
        if (!app.HasExited) app.Kill(true);
        if (!app.WaitForExit(5000)) throw new TimeoutException("Native fault host did not terminate");
    }
}
File.WriteAllText(Path.Combine(artifacts, "results.json"), JsonSerializer.Serialize(results, new JsonSerializerOptions { WriteIndented = true }));
return failed ? 1 : 0;

static class Native
{
    public delegate bool EnumCallback(nint window, nint parameter);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(nint parent, EnumCallback callback, nint parameter);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(nint window, StringBuilder text, int capacity);
    public static List<nint> Children(nint parent) { var result = new List<nint>(); EnumChildWindows(parent, (window, _) => { result.Add(window); return true; }, 0); return result; }
    public static string ClassName(nint window) { var text = new StringBuilder(256); GetClassName(window, text, text.Capacity); return text.ToString(); }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageTimeoutW", SetLastError = true)]
    private static extern nint SendText(nint window, uint message, nint wparam, string text, uint flags, uint timeout, out nuint result);
    public static void SetText(nint window, string text)
    {
        if (window == 0 || SendText(window, 0x000C, 0, text, 2, 3000, out _) == 0)
            throw new InvalidOperationException("Native text input timed out or failed");
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "SendMessageTimeoutW", SetLastError = true)]
    private static extern nint SendNumber(nint window, uint message, nint wparam, nint lparam, uint flags, uint timeout, out nuint result);
    public static void ReplaceText(nint window, string text)
    {
        if (window == 0 || SendNumber(window, 0x00B1, 0, -1, 2, 3000, out _) == 0
            || SendText(window, 0x00C2, 1, text, 2, 3000, out _) == 0)
            throw new InvalidOperationException("Native replacement input timed out or failed");
    }
    [DllImport("user32.dll")] private static extern nint GetMenu(nint window);
    [DllImport("user32.dll")] private static extern nint GetSubMenu(nint menu, int position);
    [DllImport("user32.dll")] private static extern int GetMenuItemCount(nint menu);
    [DllImport("user32.dll")] private static extern uint GetMenuItemID(nint menu, int position);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetMenuString(nint menu, uint item, StringBuilder text, int capacity, uint flags);
    public static nint FindUndo(nint window)
    {
        uint Find(nint menu)
        {
            for (var i = 0; i < GetMenuItemCount(menu); ++i)
            {
                var text = new StringBuilder(256);
                GetMenuString(menu, (uint)i, text, text.Capacity, 0x400);
                if (text.ToString().Replace("&", "").StartsWith("Undo", StringComparison.Ordinal) || text.ToString().Replace("&", "").StartsWith("Nothing to undo", StringComparison.Ordinal)) return GetMenuItemID(menu, i);
                var child = GetSubMenu(menu, i);
                if (child != 0 && Find(child) is var id && id != uint.MaxValue) return id;
            }
            return uint.MaxValue;
        }
        var result = Find(GetMenu(window));
        if (result == uint.MaxValue) throw new InvalidOperationException("Observed Undo menu command missing");
        return (nint)result;
    }

    [DllImport("user32.dll", EntryPoint = "PostMessageW", ExactSpelling = true, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PostMessage(nint hwnd, uint message, nint wparam, nint lparam);
}
