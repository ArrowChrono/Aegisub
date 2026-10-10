#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property UseWindowsForms=true
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
using System.Text.Json.Nodes;
using Aegisub.GuiAutomation.Driver;
using System.Windows.Automation;

if (args.Length != 3)
{
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/search-replace-native.cs -- EXE ARTIFACTS filters|atomic|zero|grouped");
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
    nint main, dialog, editor;
    string fixture = "", phase = "";
    int launch;

    public int Run()
    {
        Directory.CreateDirectory(artifacts); isolation.RecordDriver();
        using var log = new StreamWriter(Path.Combine(artifacts, "driver.log")) { AutoFlush = true };
        Console.SetOut(log); Console.SetError(log);
        using var watchdog = new System.Threading.Timer(_ => { try { if (app is not null) isolation.Stop(app); } finally { Environment.Exit(124); } }, null, 180_000, Timeout.Infinite);
        File.Copy("tests/gui-automation/search-replace-native.cs", Path.Combine(artifacts, "producer.cs"));
        var sources = new[] { "src/search_replace_engine.cpp", "src/search_replace_engine.h", "src/subtitle_matcher.cpp", "src/subtitle_matcher.h", "src/subtitle_match_report.cpp", "src/subtitle_match_report.h", "src/dialog_search_results.cpp", "src/dialog_search_results.h" };
        File.WriteAllText(Path.Combine(artifacts, "inputs.json"), JsonSerializer.Serialize(new {
            executable_sha256 = Hash(exe), scenario = args[2], sources = sources.Select(path => new { path, sha256 = Hash(path) }),
            private_desktop = true, global_input = false, clipboard = false, total_timeout_seconds = 180
        }, Json));
        try
        {
            if (args[2] == "filters") { Filters(true); Filters(false); NonTextFilter(); }
            else if (args[2] == "atomic") Atomic();
            else if (args[2] == "zero") ZeroWidth();
            else if (args[2] == "grouped") Grouped();
            else throw new ArgumentException("Unknown scenario");
            File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { passed = true, checks }, Json));
            return 0;
        }
        catch (Exception error)
        {
            if (app is not null && !app.HasExited) Dump("failure");
            File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { passed = false, checks, error = error.ToString() }, Json));
            Console.Error.WriteLine(error); return 1;
        }
        finally { Stop(); }
    }

    static string Hash(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
    void Require(bool value, string description)
    {
        checks.Add(new { phase, description, passed = value }); Console.WriteLine(description + ": " + value);
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
    void Click(string label) { ObservedWindowsCommand.PostButtonClick(Control(dialog, label), app!.Id); Thread.Sleep(180); }
    void Check(string label, bool value)
    {
        var control = Control(dialog, label);
        if (Send(control, 0xF0) != (value ? 1 : 0)) { Send(control, 0xF1, value ? 1 : 0); ObservedWindowsCommand.PostButtonClick(control, app!.Id); }
        Require(Send(control, 0xF0) == (value ? 1 : 0), label + " configured");
    }
    void Command(string label)
    {
        var id = ObservedWindowsCommand.FindMenuCommand(main, app!.Id, "Search Test", label);
        ObservedWindowsCommand.PostMenuCommand(main, app.Id, id); Thread.Sleep(180);
    }
    void Query(string find, string replacement, bool regex = false)
    {
        var combos = Children(dialog).Where(h => PrivateDesktop.ClassName(h) == "ComboBox").ToArray();
        Require(combos.Length == 2, "Native Find and Replace controls observed");
        Native.SetText(combos[0], find); Native.SetText(combos[1], replacement);
        Check("Use regular expressions", regex); Check("Match case", true);
    }
    void Style(string style)
    {
        var list = Children(dialog).Single(h => PrivateDesktop.ClassName(h) == "ListBox");
        var index = Send(list, 0x18B);
        Require(index == 2, "Both fixture styles appear in native checklist");
        var wanted = style == "Default" ? 0 : 1;
        Send(list, 0x186, wanted);
        Native.Post(list, 0x100, 0x20, 1); Native.Post(list, 0x101, 0x20, unchecked((int)0xC0000001));
        Thread.Sleep(180);
    }
    (long Start, long End) Selection()
    {
        var range = Send(editor, 0xB0); var text = Text(editor);
        return (Encoding.UTF8.GetByteCount(text[..(int)(range & 0xFFFF)]), Encoding.UTF8.GetByteCount(text[..(int)((range >> 16) & 0xFFFF)]));
    }
    void Selected(long start, long end)
    {
        var actual = Selection();
        Require(actual == (start, end), $"Editor UTF-8 selection [{start},{end}] (actual {actual})");
    }
    void Dump(string label)
    {
        var windows = Windows();
        File.WriteAllText(Path.Combine(artifacts, phase + "-" + label + ".json"), JsonSerializer.Serialize(windows.Select(h => new {
            handle = h.ToString("X"), text = Text(h), class_name = PrivateDesktop.ClassName(h),
            children = Children(h).Select(c => new { handle = c.ToString("X"), text = Text(c), class_name = PrivateDesktop.ClassName(c), id = Native.GetDlgCtrlID(c) })
        }), Json));
    }
    string[] Save(string label)
    {
        Command("Save"); Wait(() => !Text(main).StartsWith('*'), "Save cleared dirty state");
        File.Copy(fixture, Path.Combine(artifacts, phase + "-" + label + ".ass"), true);
        return File.ReadAllLines(fixture).Where(s => s.StartsWith("Dialogue:") || s.StartsWith("Comment:")).ToArray();
    }
    void Stop()
    {
        if (app is not null) { isolation.Stop(app); app.Dispose(); app = null; }
        main = dialog = editor = 0;
    }
    void Start(string name, bool comment = false, string? lateText = null, string? firstText = null, string? secondText = null)
    {
        Stop(); phase = name;
        var root = Path.Combine(artifacts, "host-" + ++launch); var profile = Path.Combine(root, "profile");
        Directory.CreateDirectory(Path.Combine(profile, "user"));
        File.WriteAllText(Path.Combine(profile, "user", "config.json"), """
            {"Subtitle":{"Use STC":false},"App":{"First Start":false,"Language":"en_US","Auto":{"Backup":false,"Check For Updates":false,"Load Linked Files":1,"Save":false,"Save on Every Change":false}}}
            """);
        var menu = JsonNode.Parse(File.ReadAllText("src/libresrc/default_menu.json"))!;
        menu["main"]!.AsArray().Add(new JsonObject { ["submenu"] = "search-test", ["text"] = "Search Test" });
        var commands = new JsonArray();
        foreach (var (command, label) in new[] { ("edit/find_replace", "Replace"), ("subtitle/save", "Save"), ("subtitle/save/as", "Save As"), ("edit/undo", "Undo"), ("edit/redo", "Redo"), ("edit/line/duplicate", "Duplicate") })
            commands.Add(new JsonObject { ["command"] = command, ["text"] = label });
        menu["search-test"] = commands;
        File.WriteAllText(Path.Combine(profile, "user", "menu.json"), menu.ToJsonString());
        fixture = Path.Combine(root, "fixture.ass");
        var header = """
            [Script Info]
            Title: Native Search Replace E2E
            ScriptType: v4.00+
            PlayResX: 640
            PlayResY: 480
            [V4+ Styles]
            Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
            Style: Default,Arial,24,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1
            Style: Other,Arial,20,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1
            [Events]
            Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
            """;
        var events = new[] {
            $"{(comment ? "Comment" : "Dialogue")}: 1,0:00:01.00,0:00:02.00,Default,first,0011,0012,0013,fx,{{\\i1}}needle café 😀 needle",
            "Dialogue: 2,0:00:02.00,0:00:03.00,Other,second,0021,0022,0023,fx,needle 中文 😀 needle",
            "Comment: 3,0:00:03.00,0:00:04.00,Other,comment,0031,0032,0033,fx,needle sentinel",
            "Dialogue: 4,0:00:04.00,0:00:05.00,Default,sentinel,0041,0042,0043,fx,unchanged sentinel"
        };
        if (firstText is not null) events[0] = events[0][..(events[0].LastIndexOf(',') + 1)] + firstText;
        if (secondText is not null) events[1] = events[1][..(events[1].LastIndexOf(',') + 1)] + secondText;
        if (lateText is not null) events[^1] = events[^1][..(events[^1].LastIndexOf(',') + 1)] + lateText;
        File.WriteAllText(fixture, header + "\n" + string.Join('\n', events) + "\n", new UTF8Encoding(true));
        var start = new ProcessStartInfo(exe) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var arg in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", root, "--open", fixture }) start.ArgumentList.Add(arg);
        app = isolation.Start(start, name);
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(root, "ready.json"), app, TimeSpan.FromSeconds(20));
        main = isolation.MainWindow(app, 10);
        editor = Children(main).Single(h => PrivateDesktop.ClassName(h) == "Edit" && Text(h) == (firstText ?? "{\\i1}needle café 😀 needle"));
        Save("before"); Command("Replace");
        Wait(() => { dialog = Windows().SingleOrDefault(h => Text(h) == "Replace"); return dialog != 0; }, "Native Replace dialog");
        Dump("opened");
    }
    void Filters(bool comment)
    {
        Start(comment ? "skip-comment" : "checked-style", comment);
        Query("needle", "changed"); Click("Find next"); Selected(5, 11);
        if (comment) Check("Skip Comments", true); else Style("Other");
        Click("Replace next"); Selected(0, 6);
        Require(Text(editor).StartsWith("needle 中文"), "Replace Next traversed to the eligible line");
        Require(!Text(main).StartsWith('*'), "Changing filters did not mutate the old selected match");
        Click("Replace next");
        var after = Save("after");
        Require(after[0].EndsWith("{\\i1}needle café 😀 needle") && after[1].EndsWith("changed 中文 😀 needle") && after[2].EndsWith("needle sentinel"), "Only the eligible next line was replaced");
        Command("Undo"); var undo = Save("undo");
        Require(undo[1].EndsWith("needle 中文 😀 needle") && undo[0] == after[0], "One Undo restores the sole eligible replacement");
    }
    string[] SaveAs(string label)
    {
        Command("Save As"); nint save = 0;
        Wait(() => { save = Windows().SingleOrDefault(h => PrivateDesktop.ClassName(h) == "#32770" && Text(h).Contains("Save")); return save != 0; }, "Native Save As dialog");
        var elements = AutomationElement.FromHandle(save).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
        var name = elements.Single(e => e.Current.ControlType == ControlType.Edit && e.Current.AutomationId == "1001");
        var path = Path.Combine(artifacts, phase + "-" + label + ".ass");
        var handle = (nint)name.Current.NativeWindowHandle; isolation.VerifyWindow(handle, app!); Native.SetText(handle, path);
        var button = elements.Single(e => e.Current.ControlType == ControlType.Button && e.Current.AutomationId == "1" && e.Current.NativeWindowHandle != 0);
        ObservedWindowsCommand.PostButtonClick((nint)button.Current.NativeWindowHandle, app!.Id);
        Wait(() => File.Exists(path) && !Windows().Contains(save), "Native Save As completed"); fixture = path;
        return File.ReadAllLines(path).Where(s => s.StartsWith("Dialogue:") || s.StartsWith("Comment:")).ToArray();
    }
    void Next(long start, long end) { Click("Find next"); Selected(start, end); }
    sealed record HighlightRow(int Top, int Bottom, int[] Starts, int[] Ends);
    sealed record ResultView(nint Window, HighlightRow[] Original, HighlightRow[] Replacement, string[] Lines);
    static HighlightRow[] HighlightRows(System.Drawing.Bitmap bitmap, int colour, (int Top, int Bottom)[] rowBounds)
    {
        var rows = new List<HighlightRow>();
        foreach (var bounds in rowBounds)
        {
            int top = -1, bottom = -1, bestCount = 0;
            int[] bestStarts = [], bestEnds = [];
            for (var y = Math.Max(0, bounds.Top); y < Math.Min(bitmap.Height, bounds.Bottom); ++y)
            {
                var starts = new List<int>(); var ends = new List<int>();
                var inside = false; var count = 0;
                for (var x = 0; x <= bitmap.Width; ++x)
                {
                    var marked = x < bitmap.Width && bitmap.GetPixel(x, y).ToArgb() == colour;
                    if (marked) { ++count; if (!inside) starts.Add(x); }
                    else if (inside) ends.Add(x);
                    inside = marked;
                }
                if (count == 0) continue;
                if (top < 0) top = y;
                bottom = y;
                if (count > bestCount) { bestCount = count; bestStarts = starts.ToArray(); bestEnds = ends.ToArray(); }
            }
            if (top >= 0) rows.Add(new(top, bottom, bestStarts, bestEnds));
        }
        return rows.ToArray();
    }
    ResultView ObserveResults(string label, bool replacement = true)
    {
        var title = replacement ? "Replace results" : "Search results";
        nint results = 0;
        Wait(() => { results = Windows().SingleOrDefault(h => Text(h) == title); return results != 0; }, title);
        isolation.VerifyWindow(results, app!);
        Native.Resize(results, 1320, 520);
        Thread.Sleep(180);
        var path = Path.Combine(artifacts, phase + "-" + label);
        var capture = ScreenCapture.SaveWindowPng(results, path + ".png");
        using var bitmap = new System.Drawing.Bitmap(path + ".png");
        var root = AutomationElement.FromHandle(results);
        var windowBounds = root.Current.BoundingRectangle;
        var accessible = root.FindAll(TreeScope.Descendants, Condition.TrueCondition)
            .Cast<AutomationElement>().Select(e => new { name = e.Current.Name, control_type = e.Current.ControlType.ProgrammaticName, bounds = e.Current.BoundingRectangle }).ToArray();
        var rowBounds = accessible.Where(e => e.control_type == ControlType.ListItem.ProgrammaticName).Select(e => (
            Top: (int)Math.Floor((e.bounds.Top - windowBounds.Top) * bitmap.Height / windowBounds.Height),
            Bottom: (int)Math.Ceiling((e.bounds.Bottom - windowBounds.Top) * bitmap.Height / windowBounds.Height))).ToArray();
        var original = HighlightRows(bitmap, System.Drawing.Color.FromArgb(255, 184, 108).ToArgb(), rowBounds);
        var replaced = HighlightRows(bitmap, System.Drawing.Color.FromArgb(126, 220, 150).ToArgb(), rowBounds);
        File.WriteAllText(path + ".json", JsonSerializer.Serialize(new {
            capture, original_highlight_rows = original, replacement_highlight_rows = replaced,
            native_text = Children(results).Select(Text).ToArray(), window_bounds = windowBounds.ToString(),
            accessible = accessible.Select(e => new { e.name, e.control_type, bounds = e.bounds.ToString() }),
            text_verification = "Saved ASS and editor are machine-checked; custom-rendered column strings require PNG review."
        }, Json));
        Dump(label + "-native");
        return new(results, original, replaced, accessible.Where(e => e.control_type == ControlType.ListItem.ProgrammaticName).Select(e => e.name).ToArray());
    }
    void ResultStatus(nint results, string expected)
    {
        var labels = Children(results).Select(Text).ToArray();
        Require(labels.Contains(expected), "Result status: " + expected + " (observed " + string.Join(" / ", labels.Where(t => t.Contains("match") || t.Contains("edited"))) + ")");
    }
    void HighlightCounts(ResultView view, int[] original, int[] replacement)
    {
        Require(view.Original.Select(r => r.Starts.Length).SequenceEqual(original), "Rendered orange highlight segments per result row: " + string.Join(",", original));
        Require(view.Replacement.Select(r => r.Starts.Length).SequenceEqual(replacement), "Rendered green highlight segments per result row: " + string.Join(",", replacement));
    }
    void ResultLines(ResultView view, params string[] expected)
    {
        Require(view.Lines.SequenceEqual(expected), "Accessible result subtitle line numbers: " + string.Join(",", expected));
    }
    void ActivateResult(ResultView view, int index)
    {
        var row = view.Original[index];
        var frame = Native.Bounds(view.Window);
        var x = frame.Left + row.Starts[0] + 1;
        var y = frame.Top + (row.Top + row.Bottom) / 2;
        var target = Children(view.Window).Where(h => Native.Bounds(h).Contains(x, y))
            .OrderBy(h => Native.Bounds(h).Area).First();
        isolation.VerifyWindow(target, app!);
        var point = Native.ClientPoint(target, x, y);
        Native.Post(target, 0x201, 1, point); Native.Post(target, 0x202, 0, point);
        Native.Post(target, 0x203, 1, point); Native.Post(target, 0x202, 0, point);
        Thread.Sleep(180);
    }
    void CloseResults(nint results)
    {
        ObservedWindowsCommand.PostButtonClick(Control(results, "Close"), app!.Id);
        Wait(() => !Windows().Contains(results), "Results closed");
    }
    void Grouped()
    {
        const string first = "{\\i1}前😀 x ééé x", second = "次 café ééé x";
        Start("grouped-unicode", firstText: first, secondText: second);
        var before = SaveAs("original");
        Query("x|ééé", "界", true); Check("Skip Comments", true); Click("Replace all");
        var view = ObserveResults("replaced");
        HighlightCounts(view, [3, 2], [3, 2]); ResultLines(view, "1", "2"); ResultStatus(view.Window, "5 matches were replaced.");
        var after = Save("replaced");
        Require(after[0].EndsWith("{\\i1}前😀 界 界 界") && after[1].EndsWith("次 café 界 界") && after.Skip(2).SequenceEqual(before.Skip(2)), "Mixed-length Unicode regex replacement saves complete final lines and untouched sentinels");
        ActivateResult(view, 1); Require(Text(editor) == "次 café 界 界", "Second grouped result navigates to its own subtitle"); Selected(10, 13);
        ActivateResult(view, 0); Require(Text(editor) == "{\\i1}前😀 界 界 界", "First grouped result navigates to complete replaced text"); Selected(13, 16);
        Command("Undo"); Require(Save("undo").SequenceEqual(before), "One Undo restores every original subtitle");
        view = ObserveResults("undo"); HighlightCounts(view, [3, 2], [3, 2]); ResultLines(view, "1", "2"); ResultStatus(view.Window, "5 matches found.");
        ActivateResult(view, 1); Require(Text(editor) == second, "Undo restores the second result's original text"); Selected(10, 16);
        ActivateResult(view, 0); Require(Text(editor) == first, "Undo restores the first result's original text"); Selected(13, 14);
        Send(editor, 0xB1, 0, -1); Native.ReplaceSelection(editor, "edited historical row");
        Wait(() => Text(main).StartsWith('*'), "Native editor change commits");
        var edited = Save("edited"); Require(edited[0].EndsWith("edited historical row") && edited[1] == before[1], "Manual edit changes only the active subtitle");
        var stale = ObserveResults("edited"); HighlightCounts(stale, [2], [2]); ResultLines(stale, "1", "2"); ResultStatus(stale.Window, "5 matches found.  (one row re-edited)");
        ActivateResult(view, 1); Require(Text(editor) == second, "Unedited grouped result remains navigable"); Selected(10, 16);
        ActivateResult(view, 0); Require(Text(editor) == second, "Re-edited grouped result cannot navigate using stale coordinates"); Selected(10, 16);
        CloseResults(view.Window);
        Query("x|ééé", "", true); Click("Find all");
        var found = ObserveResults("find-remains-per-match", false); HighlightCounts(found, [1, 1], []); ResultLines(found, "2", "2"); ResultStatus(found.Window, "2 matches found.");

        Start("grouped-deletion", firstText: first, secondText: second);
        Query("x|ééé", "", true); Check("Skip Comments", true); Click("Replace all");
        view = ObserveResults("replaced"); HighlightCounts(view, [3, 2], [1, 1]); ResultLines(view, "1", "2"); ResultStatus(view.Window, "5 matches were replaced.");
        Require(view.Replacement[0].Ends[0] - view.Replacement[0].Starts[0] > view.Replacement[1].Ends[0] - view.Replacement[1].Starts[0], "Three adjacent deletion markers form a wider merged green region than two");
        after = Save("replaced"); Require(after[0].EndsWith("{\\i1}前😀   ") && after[1].EndsWith("次 café  "), "Deletion reports show complete lines after every deletion");
        ActivateResult(view, 0); Selected(13, 13);

        Start("grouped-separated-deletion", firstText: "{\\i1}前😀 x / ééé / x !", secondText: "次 café ééé / x !");
        Query("x|ééé", "", true); Check("Skip Comments", true); Click("Replace all");
        view = ObserveResults("replaced"); HighlightCounts(view, [3, 2], [3, 2]); ResultLines(view, "1", "2"); ResultStatus(view.Window, "5 matches were replaced.");
        after = Save("replaced"); Require(after[0].EndsWith("{\\i1}前😀  /  /  !") && after[1].EndsWith("次 café  /  !"), "Separated deletion markers retain every visible separator and suffix");
        ActivateResult(view, 0); Selected(13, 13);

        Start("grouped-zero-width", firstText: "前😀 x x", secondText: "次 é x");
        Query("$", "", true); Check("Skip Comments", true); Style("Other"); Click("Find all");
        found = ObserveResults("find-line-end", false); HighlightCounts(found, [1], []); ResultLines(found, "2"); ResultStatus(found.Window, "One match found.");
        ActivateResult(found, 0); Require(Text(editor) == "次 é x", "End-of-line Find All navigates to its filtered subtitle"); Selected(8, 8); CloseResults(found.Window);
        Query("(?=x)|$", "", true); Click("Find all");
        found = ObserveResults("find-interior-and-line-end", false); HighlightCounts(found, [1, 1], []); ResultLines(found, "2", "2"); ResultStatus(found.Window, "2 matches found.");
        ActivateResult(found, 0); Selected(7, 7); ActivateResult(found, 1); Selected(8, 8); CloseResults(found.Window);
        Query("(?=x)|$", "界", true); Click("Replace all");
        view = ObserveResults("replaced"); HighlightCounts(view, [2], [2]); ResultLines(view, "2"); ResultStatus(view.Window, "2 matches were replaced.");
        after = Save("replaced"); Require(after[0].EndsWith("前😀 x x") && after[1].EndsWith("次 é 界x界") && after[2].EndsWith("needle sentinel") && after[3].EndsWith("unchanged sentinel"), "Zero-width insertions honor style and comment filters and preserve every insertion");
        ActivateResult(view, 0); Require(Text(editor) == "次 é 界x界", "Zero-width grouped result navigates to its filtered subtitle"); Selected(7, 10);
        Command("Undo"); view = ObserveResults("undo"); ActivateResult(view, 0); Require(Text(editor) == "次 é x", "Undo restores zero-width original line"); Selected(7, 7);
        File.WriteAllText(Path.Combine(artifacts, "visual-review.json"), JsonSerializer.Serialize(new {
            required_review = "Review every grouped PNG: line numbers, full Context/Replaced line, and ordered Match/Replacement fragments. Pixel assertions verify visible row/segment counts, not glyph semantics.",
            unicode_columns = new[] {
                new { line = 1, match = "x | ééé | x", replacement = "界 | 界 | 界", context = first, replaced_line = "{\\i1}前😀 界 界 界" },
                new { line = 2, match = "ééé | x", replacement = "界 | 界", context = second, replaced_line = "次 café 界 界" }
            }
        }, Json));
    }
    void ZeroWidth()
    {
        Start("zero-basic", firstText: "aa"); Query("(?=a)", "", true);
        Next(0, 0); Next(1, 1);
        Start("zero-unicode-alternatives", firstText: "😀aéa"); Query("(?=😀|a)|😀|a|$", "", true); Check("Selected rows", true);
        Next(0, 0); Next(0, 4); Next(4, 4); Next(4, 5); Next(7, 7); Next(7, 8); Next(8, 8); Next(14, 14);
        Require(Text(editor) == "needle 中文 😀 needle", "End-of-line zero width advances to the next row");
        Start("zero-normalized", firstText: "e\u0301é"); Query("(?=é)|é|$", "", true); Check("Match case", false);
        Next(0, 0); Next(0, 3); Next(3, 3); Next(3, 5); Next(5, 5);
        Start("zero-tags", firstText: "😀{\\i1}a{\\i0}a"); Query("(?=a)|a", "", true); Check("Skip Override Tags", true);
        Next(9, 9); Next(9, 10); Next(15, 15); Next(15, 16);
        Start("zero-reset", firstText: "aa"); Query("(?=a)", "", true); Next(0, 0);
        Query("a", "", true); Next(0, 1);
        Send(editor, 0xB1, 0, 0); Native.Post(editor, 0x101, 0x25, unchecked((int)0xC0000001)); Thread.Sleep(100);
        Query("(?=a)", "", true); Next(0, 0); Next(1, 1);
        Send(editor, 0xB1, 0, 0); Native.Post(editor, 0x101, 0x25, unchecked((int)0xC0000001)); Thread.Sleep(100);
        Next(0, 0);
        Require(!Text(main).StartsWith('*'), "Find Next never dirties the document");
        Command("Duplicate"); Command("Undo"); Next(0, 0);
        Require(!Text(main).StartsWith('*'), "Undo invalidates zero-width continuation without introducing another edit");
        Start("first-hit-short-circuit", firstText: "x " + new string('a', 100) + "!"); Query("(?=x)|x|(?:a+)+$", "", true);
        Next(0, 0); Next(0, 1);
        Require(!Windows().Any(h => Text(h) == "Error"), "A valid first match succeeds without evaluating a later catastrophic suffix");
    }
    void Atomic()
    {
        Start("late-regex-error", lateText: new string('a', 100) + "!");
        Command("Duplicate"); Command("Undo");
        var before = SaveAs("before-error");
        Query("needle|^(a+)+$", "changed", true); Click("Replace all");
        nint error = 0;
        Wait(() => { error = Windows().FirstOrDefault(h => h != main && h != dialog && Text(h).Contains("Error")); return error != 0; }, "Real Boost runtime error dialog");
        Dump("regex-error");
        var messages = AutomationElement.FromHandle(error).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().Select(e => e.Current.Name).ToArray();
        File.WriteAllText(Path.Combine(artifacts, "boost-error.json"), JsonSerializer.Serialize(messages, Json));
        Require(messages.Any(s => s.Contains("complexity", StringComparison.OrdinalIgnoreCase) || s.Contains("stack space", StringComparison.OrdinalIgnoreCase)), "Boost reports an actual runtime regex complexity failure");
        Send(Control(error, "OK"), 0xF5); Wait(() => !Windows().Contains(error), "Regex error dismissed");
        Require(!Text(main).StartsWith('*'), "Failed Replace All did not dirty the document");
        var after = SaveAs("after-error");
        Require(after.SequenceEqual(before), "Late regex failure left every live field unchanged");
        Command("Redo"); var redo = Save("existing-redo");
        Require(redo.Length == before.Length + 1 && redo[0] == before[0] && redo[1] == before[0], "Failed Replace All preserves pre-existing Redo");
        Command("Undo"); Require(Save("existing-redo-undone").SequenceEqual(before), "Pre-existing history remains reversible");
        Query("needle", "changed"); Click("Replace all");
        Wait(() => Text(main).StartsWith('*'), "Successful Replace All commits");
        var success = Save("success");
        Require(success.SequenceEqual(before.Select(s => s.Replace("needle", "changed"))), "Successful Replace All publishes all replacements together");
        Command("Undo"); Require(Save("one-undo").SequenceEqual(before), "One Undo restores all replaced rows");
        Command("Redo"); Require(Save("one-redo").SequenceEqual(success), "One Redo restores the complete replacement transaction");
    }
    void NonTextFilter()
    {
        Start("actor-filter"); Query("first", "renamed"); Check("Actor", true); Style("Other");
        Click("Replace next");
        Require(!Text(main).StartsWith('*'), "An ineligible actor field cannot be replaced without a text selection");
        var after = Save("after"); Require(after[0].Contains(",first,"), "Ineligible actor preserved");
        Query("second", "renamed"); Click("Replace next"); Click("Replace next");
        var eligible = Save("eligible-actor"); Require(eligible[1].Contains(",renamed,") && eligible[1].EndsWith("needle 中文 😀 needle"), "Actor mode replaces the eligible actor without changing subtitle text");
    }
}

static class Native
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect
    {
        public int Left, Top, Right, Bottom;
        public readonly long Area => (long)(Right - Left) * (Bottom - Top);
        public readonly bool Contains(int x, int y) => x >= Left && x < Right && y >= Top && y < Bottom;
    }
    [StructLayout(LayoutKind.Sequential)] struct Point { public int X, Y; }
    delegate bool Callback(nint window, nint data);
    [DllImport("user32.dll")] static extern bool GetWindowRect(nint window, out Rect rect);
    [DllImport("user32.dll")] static extern bool ScreenToClient(nint window, ref Point point);
    [DllImport("user32.dll")] static extern bool SetWindowPos(nint window, nint after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(nint parent, Callback callback, nint data);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(nint window);
    [DllImport("user32.dll", EntryPoint = "PostMessageW")] static extern bool PostMessage(nint window, uint message, nint wp, nint lp);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")] static extern nint SendMessage(nint window, uint message, nint wp, nint lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint SendText(nint window, uint message, nint wp, string lp, uint flags, uint timeout, out nint result);
    public static nint[] Children(nint parent) { List<nint> found = []; Callback callback = (h, _) => { found.Add(h); return true; }; EnumChildWindows(parent, callback, 0); GC.KeepAlive(callback); return found.ToArray(); }
    public static Rect Bounds(nint window) { if (!GetWindowRect(window, out var rect)) throw new InvalidOperationException("Native window bounds unavailable"); return rect; }
    public static long ClientPoint(nint window, int x, int y) { var point = new Point { X = x, Y = y }; if (!ScreenToClient(window, ref point)) throw new InvalidOperationException("Native coordinate conversion failed"); return (ushort)point.X | ((long)(ushort)point.Y << 16); }
    public static void Resize(nint window, int width, int height) { if (!SetWindowPos(window, 0, 0, 0, width, height, 0x16)) throw new InvalidOperationException("Native result resize failed"); }
    public static long Send(nint window, uint message, long wp, long lp) { if (SendMessage(window, message, (nint)wp, (nint)lp, 0x22, 1000, out var result) == 0) throw new TimeoutException("Native control message"); return result.ToInt64(); }
    public static void SetText(nint window, string value) { if (SendText(window, 0xC, 0, value, 0x22, 1000, out _) == 0) throw new TimeoutException("Native text entry"); }
    public static void ReplaceSelection(nint window, string value) { if (SendText(window, 0xC2, 1, value, 0x22, 1000, out _) == 0) throw new TimeoutException("Native replacement text entry"); }
    public static void Post(nint window, uint message, long wp, long lp) { if (!PostMessage(window, message, (nint)wp, (nint)lp)) throw new InvalidOperationException("Native message post failed"); }
}
