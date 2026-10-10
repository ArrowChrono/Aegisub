#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.Diagnostics;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

if (args.Length != 2)
{
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/override-position-native.cs -- EXE ARTIFACTS");
    return 2;
}
try { return PrivateDesktop.Run(args, Path.GetFullPath(args[1]), isolation => new Scenario(args, isolation).Run()); }
catch (Exception error) { Console.Error.WriteLine(error); return 1; }


sealed class Scenario(string[] args, PrivateDesktop isolation)
{
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    static readonly string[] Seed =
    [
        @"Dialogue: 1,0:00:10.00,0:00:12.34,Default,pick-active,11,12,13,fx-one,{\i1}A😀BCdé tail",
        @"Dialogue: 2,0:00:20.00,0:00:22.56,Alternate,pick-second,21,22,23,fx-two,{\bord3\c&H010203&}é界XY😀 tail",
        @"Comment: 9,0:00:20.10,0:00:20.50,Third,untouched-comment,91,92,93,sentinel,{\b1\1a&H77&}untouched café 😀",
        @"Dialogue: 3,0:00:30.00,0:00:32.78,Third,pick-third,31,32,33,fx-three,{\shad2\alpha&H40&}中🙂uv café"
    ];
    static readonly string[] Bold =
    [
        @"Dialogue: 1,0:00:10.00,0:00:12.34,Default,pick-active,11,12,13,fx-one,{\i1}A😀{\b1}BC{\b0}dé tail",
        @"Dialogue: 2,0:00:20.00,0:00:22.56,Alternate,pick-second,21,22,23,fx-two,{\bord3\c&H010203&}é界{\b1}XY{\b0}😀 tail",
        @"Comment: 9,0:00:20.10,0:00:20.50,Third,untouched-comment,91,92,93,sentinel,{\b1\1a&H77&}untouched café 😀",
        @"Dialogue: 3,0:00:30.00,0:00:32.78,Third,pick-third,31,32,33,fx-three,{\shad2\alpha&H40&}中🙂{\b0}uv{\b1} café"
    ];
    static readonly string[] Color =
    [
        @"Dialogue: 1,0:00:10.00,0:00:12.34,Default,pick-active,11,12,13,fx-one,{\i1}A😀{\c&H563412&}BCdé tail",
        @"Dialogue: 2,0:00:20.00,0:00:22.56,Alternate,pick-second,21,22,23,fx-two,{\bord3\c&H010203&}é界{\c&H563412&}XY😀 tail",
        @"Comment: 9,0:00:20.10,0:00:20.50,Third,untouched-comment,91,92,93,sentinel,{\b1\1a&H77&}untouched café 😀",
        @"Dialogue: 3,0:00:30.00,0:00:32.78,Third,pick-third,31,32,33,fx-three,{\shad2\alpha&H40&}中🙂{\c&H563412&}uv café"
    ];
    static readonly string[] Font =
    [
        @"Dialogue: 1,0:00:10.00,0:00:12.34,Default,pick-active,11,12,13,fx-one,{\i1}A😀{\fs41}BCdé tail",
        @"Dialogue: 2,0:00:20.00,0:00:22.56,Alternate,pick-second,21,22,23,fx-two,{\bord3\c&H010203&}é界{\fs41\i1}XY😀 tail",
        @"Comment: 9,0:00:20.10,0:00:20.50,Third,untouched-comment,91,92,93,sentinel,{\b1\1a&H77&}untouched café 😀",
        @"Dialogue: 3,0:00:30.00,0:00:32.78,Third,pick-third,31,32,33,fx-three,{\shad2\alpha&H40&}中🙂{\fs41\b0\i1}uv café"
    ];
    readonly string artifacts = Path.GetFullPath(args[1]);
    readonly List<object> checks = [];
    Process? app;
    nint main, dialog, combo, size;
    string profile = "", fixture = "", phase = "";
    string[] baseline = [];
    string output => artifacts;

    public int Run()
    {
        Directory.CreateDirectory(artifacts); isolation.RecordDriver();
        using var log = new StreamWriter(Path.Combine(artifacts, "driver.log")) { AutoFlush = true };
        Console.SetOut(log); Console.SetError(log);
        using var watchdog = new Timer(_ => {
            Result(false, "180-second watchdog expired; forced cleanup is failure");
            try { if (app is not null) isolation.Stop(app); } finally { Environment.Exit(124); }
        }, null, 180_000, Timeout.Infinite);
        try
        {
            var previousDpi = Native.SetThreadDpiAwarenessContext(-4);
            if (previousDpi == 0) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Scenario PerMonitorV2 DPI context unavailable");
            Require(Native.AreDpiAwarenessContextsEqual(Native.GetThreadDpiAwarenessContext(), -4), "Scenario thread uses PerMonitorV2 DPI context");
            Prepare(); Start();
            phase = "selection"; SelectLines("pick-");
            baseline = SaveAs("baseline");
            Require(baseline.Where(IsEvent).SequenceEqual(Seed), "Baseline equals all four independent literal events including untouched comment");
            SetRange(8, 10); Capture("nonzero-range-after-emoji", main);
            phase = "bold"; Click(Control(main, "Toggle Bold"));
            Wait(() => ActiveText() == EventText(Bold[0]), "Bold native command reaches active editor");
            AssertRange(13, 15); Capture("bold-result", main); AssertAss("bold", Bold);
            History("Undo toggle bold"); Wait(() => ActiveText() == EventText(Seed[0]), "One Undo removes all Bold spans"); AssertAss("bold-undo", Seed);
            History("Redo toggle bold"); Wait(() => ActiveText() == EventText(Bold[0]), "One Redo restores all Bold spans"); AssertAss("bold-redo", Bold);
            History("Undo toggle bold"); Wait(() => ActiveText() == EventText(Seed[0]), "Return to clean baseline for independent color operation");
            AssertAss("before-color", Seed);

            phase = "color-preview-cancel"; SetRange(8, 10); OpenColor();
            var html = ColorInput("HTML:");
            Require(Text(html) == "#332211", "Picker is seeded by active primary RGB, not a different selected row");
            var ass = ColorInput("ASS:");
            Require(Text(ass) == "&H112233&", "Picker ASS field exposes BGR spelling");
            var alpha = ColorInput("Alpha:");
            Require(Text(alpha) == "&H40&", "Picker shows effective alpha 40");
            SetHtml(html, "#123456", EventText(Color[0]));
            AssertRange(20, 20); Require(Text(alpha) == "&H40&", "RGB edit leaves chooser alpha unchanged");
            Capture("color-cancel-preview-input", dialog); Capture("color-cancel-preview-main", main);
            CloseDialog("Cancel"); Wait(() => ActiveText() == EventText(Seed[0]), "Cancel restores exact pre-preview active text");
            AssertRange(8, 10); AssertAss("color-cancel", Seed);

            phase = "color-accept"; SetRange(8, 8); OpenColor();
            html = ColorInput("HTML:"); alpha = ColorInput("Alpha:");
            SetHtml(html, "#654321", @"{\i1}A😀{\c&H214365&}BCdé tail");
            AssertRange(20, 20); Capture("color-first-preview-input", dialog);
            SetHtml(html, "#123456", EventText(Color[0]));
            AssertRange(20, 20); Require(Text(alpha) == "&H40&", "Repeated RGB previews retain alpha 40");
            Capture("color-final-preview-input", dialog); CloseDialog("OK");
            AssertRange(20, 20); Capture("color-result", main); AssertAss("color", Color);
            History("Undo set color"); Wait(() => ActiveText() == EventText(Seed[0]), "One Undo removes both amended RGB previews"); AssertAss("color-undo", Seed);
            History("Redo set color"); Wait(() => ActiveText() == EventText(Color[0]), "One Redo restores only final color, not first preview"); AssertAss("color-redo", Color);

            phase = "before-font"; History("Undo set color");
            Wait(() => ActiveText() == EventText(Seed[0]), "Return to pristine text before the independent Font Face operation");
            AssertAss("before-font", Seed);
            phase = "font-cancel-open"; SetRange(8, 10); OpenFont();
            ExpectFont("Arial"); Require(Text(size) == "24", "Font dialog reads the active line size at the original selection");
            phase = "font-cancel-preview37"; SetSize(37, @"{\i1}A😀{\fs37}BCdé tail");
            phase = "font-cancel-preview39"; SetSize(39, @"{\i1}A😀{\fs39}BCdé tail");
            phase = "font-cancel"; CloseDialog("Cancel");
            Wait(() => ActiveText() == EventText(Seed[0]), "Cancel restores exact active text after both font previews");
            AssertRange(8, 10); Capture("font-cancel-result", main); AssertAss("font-cancel", Seed);

            phase = "font-accept-open"; SetRange(8, 10); OpenFont();
            ExpectFont("Arial"); Require(Text(size) == "24", "Reopened Font dialog reads the unchanged baseline size");
            phase = "font-accept-preview35"; SetSize(35, @"{\i1}A😀{\fs35}BCdé tail");
            phase = "font-accept-preview41"; SetSize(41, @"{\i1}A😀{\fs41}BCdé tail");
            phase = "font-accept"; CloseDialog("OK");
            Wait(() => ActiveText() == EventText(Font[0]), "Accept preserves only the final literal font preview");
            AssertRange(15, 17); Capture("font-result", main); AssertAss("font", Font);
            phase = "font-undo"; History("Undo set font");
            Wait(() => ActiveText() == EventText(Seed[0]), "One Undo removes the accepted font operation, not just its last preview");
            AssertRange(8, 10); AssertAss("font-undo", Seed);
            phase = "font-redo"; History("Redo set font");
            Wait(() => ActiveText() == EventText(Font[0]), "One Redo restores only final size 41 at the original cross-line anchors");
            AssertRange(15, 17); AssertAss("font-redo", Font);
            Capture("saved-final", main); NormalExit(); Result(true); return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            if (app is not null && !app.HasExited)
                try { Dump("failure"); Capture("failure", dialog != 0 ? dialog : main); }
                catch (Exception captureError) { Console.Error.WriteLine(captureError); }
            Result(false, error.ToString()); return 1;
        }
        finally { if (app is not null) { isolation.Stop(app); app.Dispose(); } }
    }
    static bool IsEvent(string line) => line.StartsWith("Dialogue:") || line.StartsWith("Comment:");
    static string EventText(string line) => line.Split(',', 10)[9];
    nint ActiveEdit() => Children(main).Single(h => PrivateDesktop.ClassName(h) == "Edit" && Text(h).Contains("BC", StringComparison.Ordinal));
    string ActiveText() => Text(ActiveEdit());
    void SetRange(int start, int end)
    {
        var edit = ActiveEdit(); Action("native-editor-range", $"UTF16 [{start},{end}]");
        Send(edit, 0xB1, start, end);
        Send(edit, 0x101, 0x10, unchecked((int)0xC0000001));
        AssertRange(start, end);
    }
    (int Start, int End) ReadRange()
    {
        var range = Send(ActiveEdit(), 0xB0);
        return ((int)(range & 0xFFFF), (int)((range >> 16) & 0xFFFF));
    }
    void AssertRange(int start, int end)
    {
        var actual = ReadRange();
        Require(actual == (start, end), $"Actual native UTF16 range [{actual.Start},{actual.End}] equals [{start},{end}]");
        File.WriteAllText(Path.Combine(artifacts, phase + "-editor.json"), JsonSerializer.Serialize(new { text = ActiveText(), actual.Start, actual.End }, Json));
    }
    void OpenColor()
    {
        Click(Control(main, "Primary Color")); dialog = Window("Select Color");
        isolation.VerifyWindow(dialog, app!);
        Require(Native.GetWindow(dialog, 4) == main, "Actual Select Color dialog is owned by the observed host main window");
        Dump(phase + "-color-inventory"); Capture(phase + "-color-open", dialog);
    }
    nint ColorInput(string label)
    {
        var group = Native.Bounds(Control(dialog, "RGB color"));
        var labels = Children(dialog).Where(h => PrivateDesktop.ClassName(h) == "Static" && Text(h) == label && group.Contains(Native.Bounds(h))).ToArray();
        Require(labels.Length == 1, "Unique observed native RGB label: " + label);
        var bounds = Native.Bounds(labels[0]);
        var row = Children(dialog).Where(h => PrivateDesktop.ClassName(h) == "Edit")
            .Where(h => { var b = Native.Bounds(h); return group.Contains(b) && b.Left >= bounds.Right && Math.Abs(b.CenterY - bounds.CenterY) <= 8; })
            .OrderBy(h => Native.Bounds(h).Left).ToArray();
        Require(row.Length == (label == "Alpha:" ? 2 : 1), "Observed " + label + " row has exactly its expected native Edit controls");
        var result = row[0]; isolation.VerifyWindow(result, app!);
        Require(Native.GetParent(result) == dialog && Native.IsWindowEnabled(result), "Observed labeled input is enabled and directly parented by actual chooser");
        return result;
    }
    void SetHtml(nint input, string value, string expectedActive)
    {
        isolation.VerifyWindow(input, app!); Action("native-color-html-input", value);
        Native.ReplaceText(input, value);
        try
        {
            Wait(() => Text(input) == value && ActiveText() == expectedActive && ReadRange() == (20, 20),
                "HTML input, actual live preview and native caret [20,20] match independent literal");
        }
        finally
        {
            var actual = ReadRange();
            File.WriteAllText(Path.Combine(artifacts, phase + "-" + value[1..] + "-preview-completion.json"),
                JsonSerializer.Serialize(new { input = Text(input), text = ActiveText(), actual.Start, actual.End }, Json));
        }
    }
    void OpenFont()
    {
        var button = Children(main).Single(h => PrivateDesktop.ClassName(h) == "Button" && Text(h) == "Font Face");
        Action("observed-font-command", "Font Face/id=" + Native.GetDlgCtrlID(button));
        Click(button); dialog = Window("Select Font"); isolation.VerifyWindow(dialog, app!);
        Require(Native.GetWindow(dialog, 4) == main, "Actual Select Font dialog is owned by the observed host main window");
        var editable = AutomationElement.FromHandle(dialog).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>()
            .Where(e => e.Current.ControlType == ControlType.ComboBox && e.TryGetCurrentPattern(ValuePattern.Pattern, out var pattern) && !((ValuePattern)pattern).Current.IsReadOnly).ToArray();
        Require(editable.Length == 1, "Exactly one editable font ComboBox with writable UIA ValuePattern");
        combo = (nint)editable[0].Current.NativeWindowHandle; isolation.VerifyWindow(combo, app!);
        Require(editable[0].TryGetCurrentPattern(ExpandCollapsePattern.Pattern, out _), "Actual font ComboBox supports ExpandCollapsePattern");
        var row = Native.Bounds(combo); var group = Native.Bounds(Control(dialog, "Font"));
        Require(group.Contains(row), "Actual font ComboBox is inside the labeled Font group");
        var candidates = Children(dialog).Where(h => PrivateDesktop.ClassName(h) == "Edit" && Native.GetParent(h) != combo)
            .Where(h => { var bounds = Native.Bounds(h); return group.Contains(bounds) && bounds.Left >= row.Right && Math.Abs(bounds.CenterY - row.CenterY) <= 12; }).ToArray();
        Require(candidates.Length == 1, "Font size is uniquely the native Edit inside the Font group to the right on the font row");
        size = candidates[0]; isolation.VerifyWindow(size, app!);
        Dump(phase + "-font-inventory"); Capture(phase + "-font-open", dialog);
    }
    void ExpectFont(string expected)
    {
        Require(Text(Native.ComboEdit(combo)) == expected, "Actual font EDIT retains the original family: " + expected);
        var pattern = (ValuePattern)AutomationElement.FromHandle(combo).GetCurrentPattern(ValuePattern.Pattern);
        Require(pattern.Current.Value == expected, "Read-only UIA font value agrees with the native EDIT: " + expected);
    }
    void SetSize(int value, string expectedActive)
    {
        isolation.VerifyWindow(size, app!); Require(Native.IsWindowEnabled(size), "Observed font size Edit is enabled");
        var input = value.ToString(System.Globalization.CultureInfo.InvariantCulture);
        Action("native-font-size-input", input); Native.ReplaceText(size, input);
        try
        {
            Wait(() => Text(size) == input && ActiveText() == expectedActive && ReadRange() == (15, 17),
                "Size input, literal live preview and original-anchor native selection [15,17] all match");
            Require(ActiveText() == expectedActive, "Live preview equals the independent literal text, with no duplicate or displaced tag");
            AssertRange(15, 17); ExpectFont("Arial");
            Capture(phase + "-input", dialog); Capture(phase + "-main", main);
        }
        finally
        {
            var actual = ReadRange();
            File.WriteAllText(Path.Combine(artifacts, phase + "-preview-completion.json"),
                JsonSerializer.Serialize(new { input = Text(size), text = ActiveText(), expected = expectedActive, actual.Start, actual.End }, Json));
        }
    }
    void AssertAss(string label, string[] events)
    {
        int index = 0;
        var expected = baseline.Select(line => IsEvent(line) ? events[index++] : line).ToArray();
        Require(index == events.Length, "Oracle covers all four event records");
        File.WriteAllLines(Path.Combine(artifacts, label + "-expected.ass"), expected, new UTF8Encoding(true));
        Require(SaveAs(label).SequenceEqual(expected), "Complete ASS including nontext metadata equals independent literal events and baseline headers: " + label);
    }
    void Result(bool passed, string? error = null)
    {
        object[] snapshot; lock (checks) snapshot = checks.ToArray();
        File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { passed, error, checks = snapshot }, Json));
    }
    void Require(bool condition, string description)
    {
        lock (checks) checks.Add(new { phase, description, passed = condition });
        Console.WriteLine(phase + ": " + description + ": " + condition);
        Result(false, "Scenario is still running");
        if (!condition) throw new InvalidOperationException(description);
    }
    void Wait(Func<bool> predicate, string description, Stopwatch? deadline = null)
    {
        var watch = deadline ?? Stopwatch.StartNew();
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
    string TopCaption(nint window)
    {
        if (!Native.IsWindow(window)) return "";
        try { isolation.VerifyWindow(window, app!); return Native.Caption(window); }
        catch (InvalidOperationException) when (!Native.IsWindow(window)) { return ""; }
    }
    nint[] Children(nint window) => Native.Children(window).Where(Native.IsWindowVisible).ToArray();
    nint[] Windows() => isolation.Windows(app!).Where(Native.IsWindowVisible).ToArray();
    nint Control(nint parent, string name) => Children(parent).Single(h => Text(h).Replace("&", "", StringComparison.Ordinal) == name);
    long Send(nint window, uint message, long wp = 0, long lp = 0)
    {
        isolation.VerifyWindow(window, app!); return Native.Send(window, message, wp, lp);
    }
    void Action(string action, string target) => File.AppendAllText(Path.Combine(artifacts, "actions.ndjson"),
        JsonSerializer.Serialize(new { phase, action, target }) + Environment.NewLine);
    void Command(string menu, string label)
    {
        isolation.VerifyWindow(main, app!); Action("menu", menu + "/" + label);
        ObservedWindowsCommand.PostMenuCommand(main, app!.Id, ObservedWindowsCommand.FindMenuCommand(main, app.Id, menu, label));
    }

    static bool IsPopup(nint window)
    {
        if (!Native.IsWindow(window)) return false;
        try { return PrivateDesktop.ClassName(window) == "#32768"; }
        catch (Win32Exception error) when (error.NativeErrorCode == 1400) { return false; }
    }
    void History(string label)
    {
        isolation.VerifyWindow(main, app!);
        var edit = Native.Items(Native.GetMenu(main)).Single(item => item.Label == "Edit").Submenu;
        Require(edit != 0, "Observed Edit menu has an actual submenu");
        Action("native-menu-mnemonic", "Edit");
        Native.Post(main, 0x112, 0xF100, 'e');
        nint popup = 0;
        Wait(() => {
            popup = Windows().Where(IsPopup)
                .SingleOrDefault(h => (nint)Send(h, 0x1E1) == edit);
            return popup != 0;
        }, "Actual Edit popup opened and refreshed dynamic history labels");
        try
        {
            var items = Native.Items(edit);
            var artifact = phase + "-" + label.Replace(' ', '-');
            File.WriteAllText(Path.Combine(artifacts, artifact + "-menu.json"), JsonSerializer.Serialize(items.Select(item => new { item.Position, item.Label, item.Id, item.Enabled, item.State }), Json));
            Capture(artifact + "-menu", popup);
            var item = items.Single(item => item.Label == label);
            var command = ObservedWindowsCommand.FindMenuCommand(main, app!.Id, "Edit", label);
            Require(item.Enabled && item.Id == command, "Refreshed actual history item is enabled with exact observed label and ID: " + label);
            Action("native-popup-keyboard-selection", label);
            Native.Post(popup, 0x100, 0x24, 1);
            Native.Post(popup, 0x101, 0x24, unchecked((int)0xC0000001));
            Thread.Sleep(50);
            for (int step = 0; step < items.Length && !Native.Highlighted(edit, item.Position); ++step)
            {
                Native.Post(popup, 0x100, 0x28, 1);
                Native.Post(popup, 0x101, 0x28, unchecked((int)0xC0000001));
                Thread.Sleep(50);
            }
            Require(Native.Highlighted(edit, item.Position), "Actual popup highlights the exact observed history item");
            Native.Post(popup, 0x100, 0x0D, 1);
            Native.Post(popup, 0x101, 0x0D, unchecked((int)0xC0000001));
            Wait(() => !Windows().Any(IsPopup), "Actual history popup command completed");
        }
        finally
        {
            Native.Post(main, 0x1F);
            Wait(() => !Windows().Any(IsPopup), "Actual Edit popup dismissed");
        }
    }

    nint Window(string title)
    {
        nint found = 0; Wait(() => { found = Windows().SingleOrDefault(h => TopCaption(h) == title); return found != 0; }, title + " opened");
        return found;
    }
    void Click(nint control)
    {
        isolation.VerifyWindow(control, app!);
        Require(Native.IsWindowEnabled(control), "Observed native button is enabled: " + Text(control));
        Action("native-button-click", Text(control)); ObservedWindowsCommand.PostButtonClick(control, app!.Id);
    }
    void CloseDialog(string button)
    {
        var old = dialog; Click(Control(dialog, button)); Wait(() => !Windows().Contains(old), button + " closed the actual dialog");
        dialog = 0;
    }
    void SelectLines(string actor)
    {
        Command("Subtitle", "Select Lines..."); dialog = Window("Select");
        var edit = Children(dialog).Single(h => PrivateDesktop.ClassName(h) == "Edit");
        isolation.VerifyWindow(edit, app!); Action("native-selection-match", actor); Native.ReplaceText(edit, actor);
        foreach (var name in new[] { "Matches", "Contains", "Actor", "Set selection" })
        {
            var radio = Control(dialog, name); if (Send(radio, 0xF0) != 1) { Action("native-selection-radio", name); Native.Post(radio, 0xF5); }
            Wait(() => Send(radio, 0xF0) == 1, "Selection radio checked: " + name);
        }
        foreach (var option in new[] { (Name: "Dialogues", Value: true), (Name: "Comments", Value: false) })
        {
            var control = Control(dialog, option.Name);
            if (Send(control, 0xF0) != (option.Value ? 1 : 0)) { Action("native-selection-checkbox", option.Name); Native.Post(control, 0xF5); }
            Wait(() => Send(control, 0xF0) == (option.Value ? 1 : 0), "Selection checkbox matches: " + option.Name);
        }
        Dump(phase + "-selection-inventory"); Capture(phase + "-selection", dialog); CloseDialog("OK");
    }
    void Dump(string label)
    {
        File.WriteAllText(Path.Combine(artifacts, label + ".json"), JsonSerializer.Serialize(Windows().Select(h => new {
            handle = h.ToString("X"), text = TopCaption(h), class_name = PrivateDesktop.ClassName(h), bounds = Native.Bounds(h),
            children = Children(h).Select(c => new { handle = c.ToString("X"), text = Text(c), class_name = PrivateDesktop.ClassName(c), bounds = Native.Bounds(c), enabled = Native.IsWindowEnabled(c), id = Native.GetDlgCtrlID(c) })
        }), Json));
    }
    void Capture(string label, nint window)
    {
        isolation.VerifyWindow(window, app!); ScreenCapture.SaveWindowPng(window, Path.Combine(artifacts, label + ".png"));
    }
    string[] SaveAs(string label)
    {
        Command("File", "Save Subtitles as..."); nint save = 0;
        var preparation = Stopwatch.StartNew();
        Wait(() => { save = Windows().SingleOrDefault(h => PrivateDesktop.ClassName(h) == "#32770" && TopCaption(h).Contains("Save", StringComparison.Ordinal)); return save != 0; }, "Native Save As dialog opened", preparation);
        Capture(label + "-save-picker", save);
        nint handle = 0, button = 0;
        Wait(() => {
            var current = Windows().SingleOrDefault(h => PrivateDesktop.ClassName(h) == "#32770" && TopCaption(h).Contains("Save", StringComparison.Ordinal));
            if (current == 0) return false;
            if (current != save) throw new InvalidOperationException("Actual Save As dialog changed during control preparation");
            isolation.VerifyWindow(save, app!);
            try
            {
                var elements = AutomationElement.FromHandle(save).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>()
                    .Select(e => new { Type = e.Current.ControlType, Id = e.Current.AutomationId, Handle = e.Current.NativeWindowHandle }).ToArray();
                var edits = elements.Where(e => e.Type == ControlType.Edit && e.Id == "1001").ToArray();
                var buttons = elements.Where(e => e.Type == ControlType.Button && e.Id == "1").ToArray();
                if (edits.Length > 1 || buttons.Length > 1) throw new InvalidOperationException("Ambiguous native Save As filename or Save control");
                if (edits.Length == 0 || buttons.Length == 0 || edits[0].Handle == 0 || buttons[0].Handle == 0) return false;
                handle = (nint)edits[0].Handle; button = (nint)buttons[0].Handle;
                isolation.VerifyWindow(handle, app!); isolation.VerifyWindow(button, app!);
                return true;
            }
            catch (ElementNotAvailableException) { return false; }
        }, "Same owned Save As dialog exposes one filename EDIT and one Save button", preparation);
        var path = Path.Combine(artifacts, label + ".ass");
        isolation.VerifyWindow(handle, app!); Action("native-save-as-filename", label + ".ass"); Native.SetText(handle, path);
        Capture(label + "-save-picker-input", save);
        Click(button);
        Wait(() => File.Exists(path) && !Windows().Contains(save), "Native Save As completed: " + label);
        Require(!Text(main).StartsWith('*'), "Native Save As clears dirty state"); return File.ReadAllLines(path);
    }
    void NormalExit()
    {
        isolation.VerifyWindow(main, app!); Action("normal-exit", "main/WM_CLOSE"); Native.Post(main, 0x10);
        Require(app!.WaitForExit(10_000), "Normal WM_CLOSE exits within ten seconds without forced termination");
        Require(isolation.ExitCode(app) == 0, "Normal host native exit code is zero");
        app.Dispose(); app = null; main = 0;
    }

    void Prepare()
    {
        profile = Path.Combine(artifacts, "profile"); Directory.CreateDirectory(Path.Combine(profile, "user"));
        File.WriteAllText(Path.Combine(profile, "user", "config.json"), """
            {"Subtitle":{"Use STC":false},"App":{"First Start":false,"Language":"en_US","Auto":{"Backup":false,"Check For Updates":false,"Load Linked Files":1,"Save":false,"Save on Every Change":false}}}
            """);
        fixture = Path.Combine(artifacts, "fixture.ass");
        var header = """
            [Script Info]
            Title: Native Override Position E2E
            Original Script: metadata sentinel café 世界
            ScriptType: v4.00+
            PlayResX: 640
            PlayResY: 480
            WrapStyle: 2
            ScaledBorderAndShadow: yes
            [V4+ Styles]
            Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
            Style: Default,Arial,24,&H40112233,&H000000FF,&H00010203,&H00040506,0,0,0,0,101,99,0.5,3,1,2,1,2,11,12,13,1
            Style: Alternate,Arial,27,&H40112233,&H000000FF,&H00010203,&H00040506,0,0,0,0,103,98,0.8,4,1,3,2,7,21,22,23,1
            Style: Third,Arial,31,&H40112233,&H000000FF,&H00010203,&H00040506,-1,0,0,0,105,97,1.2,5,1,4,3,9,31,32,33,1
            [Events]
            Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
            """;
        File.WriteAllText(fixture, header + "\n" + string.Join('\n', Seed) + "\n", new UTF8Encoding(true));
    }
    void Start()
    {
        var host = Path.Combine(artifacts, "launch"); Directory.CreateDirectory(host);
        var start = new ProcessStartInfo(Path.GetFullPath(args[0])) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", host, "--open", fixture }) start.ArgumentList.Add(argument);
        app = isolation.Start(start, "override-position");
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(host, "ready.json"), app, TimeSpan.FromSeconds(20));
        main = isolation.MainWindow(app, 10);
    }
}

static class Native
{
    public sealed record MenuItem(int Position, string Label, uint Id, bool Enabled, uint State, nint Submenu);
    [DllImport("user32.dll")] public static extern nint GetMenu(nint window);
    [DllImport("user32.dll")] static extern nint GetSubMenu(nint menu, int position);
    [DllImport("user32.dll")] static extern int GetMenuItemCount(nint menu);
    [DllImport("user32.dll")] static extern uint GetMenuItemID(nint menu, int position);
    [DllImport("user32.dll")] static extern uint GetMenuState(nint menu, uint position, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetMenuStringW", CharSet = CharSet.Unicode)] static extern int GetMenuString(nint menu, uint position, StringBuilder text, int capacity, uint flags);
    public static MenuItem[] Items(nint menu)
    {
        var count = GetMenuItemCount(menu);
        if (count < 0) throw new InvalidOperationException("Observed native menu unavailable");
        return Enumerable.Range(0, count).Select(position => {
            var text = new StringBuilder(256);
            var length = GetMenuString(menu, (uint)position, text, text.Capacity, 0x400);
            if (length >= text.Capacity - 1) throw new InvalidOperationException("Native menu label truncated");
            var state = GetMenuState(menu, (uint)position, 0x400);
            if (state == uint.MaxValue) throw new InvalidOperationException("Observed native menu disappeared");
            return new MenuItem(position, text.ToString().Split('\t')[0].Replace("&", "", StringComparison.Ordinal), GetMenuItemID(menu, position), (state & 3) == 0, state, GetSubMenu(menu, position));
        }).ToArray();
    }
    public static bool Highlighted(nint menu, int position) => (GetMenuState(menu, (uint)position, 0x400) & 0x80) != 0;

    delegate bool Callback(nint window, nint data);
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] struct ComboBoxInfo { public int Size; public Rect Item, Button; public uint State; public nint Combo, Edit, List; }
    [DllImport("user32.dll")] static extern bool GetComboBoxInfo(nint window, ref ComboBoxInfo info);
    public sealed record Rectangle(int Left, int Top, int Right, int Bottom)
    {
        public int CenterY => (Top + Bottom) / 2;
        public bool Contains(Rectangle other) => other.Left >= Left && other.Top >= Top && other.Right <= Right && other.Bottom <= Bottom;
    }
    [DllImport("user32.dll")] static extern bool GetWindowRect(nint window, out Rect rect);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(nint parent, Callback callback, nint data);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] public static extern bool IsWindow(nint window);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(nint window);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(nint window);
    [DllImport("user32.dll")] public static extern nint GetParent(nint window);
    [DllImport("user32.dll")] public static extern nint GetWindow(nint window, uint command);
    [DllImport("user32.dll", SetLastError = true)] public static extern nint SetThreadDpiAwarenessContext(nint context);
    [DllImport("user32.dll")] public static extern nint GetThreadDpiAwarenessContext();
    [DllImport("user32.dll")] public static extern bool AreDpiAwarenessContextsEqual(nint first, nint second);
    [DllImport("user32.dll", EntryPoint = "GetWindowTextLengthW", SetLastError = true)] static extern int GetCaptionLength(nint window);
    [DllImport("user32.dll", EntryPoint = "GetWindowTextW", CharSet = CharSet.Unicode, SetLastError = true)] static extern int GetCaption(nint window, StringBuilder text, int capacity);
    [DllImport("user32.dll", EntryPoint = "PostMessageW")] static extern bool PostMessage(nint window, uint message, nint wp, nint lp);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")] static extern nint SendMessage(nint window, uint message, nint wp, nint lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint SendText(nint window, uint message, nint wp, string lp, uint flags, uint timeout, out nint result);
    public static Rectangle Bounds(nint window) { if (!GetWindowRect(window, out var rect)) throw new InvalidOperationException("Native bounds unavailable"); return new(rect.Left, rect.Top, rect.Right, rect.Bottom); }
    public static string Caption(nint window)
    {
        Marshal.SetLastPInvokeError(0); var length = GetCaptionLength(window); var error = Marshal.GetLastPInvokeError();
        if (length == 0 && error != 0) { if (!IsWindow(window)) return ""; throw new Win32Exception(error, "Top-level cached caption length unavailable"); }
        var text = new StringBuilder(length + 1);
        Marshal.SetLastPInvokeError(0); var copied = GetCaption(window, text, text.Capacity); error = Marshal.GetLastPInvokeError();
        if (copied == 0 && error != 0) { if (!IsWindow(window)) return ""; throw new Win32Exception(error, "Top-level cached caption unavailable"); }
        return text.ToString();
    }
    public static nint[] Children(nint parent) { List<nint> found = []; Callback callback = (h, _) => { found.Add(h); return true; }; EnumChildWindows(parent, callback, 0); GC.KeepAlive(callback); return found.ToArray(); }
    public static long Send(nint window, uint message, long wp = 0, long lp = 0) { if (SendMessage(window, message, (nint)wp, (nint)lp, 0x22, 1000, out var result) == 0) throw new TimeoutException("Native control message"); return result.ToInt64(); }
    public static void SetText(nint window, string value) { if (SendText(window, 0xC, 0, value, 0x22, 1000, out _) == 0) throw new TimeoutException("Native text entry"); }
    public static void ReplaceText(nint window, string value)
    {
        Send(window, 0xB1, 0, -1);
        if (SendText(window, 0xC2, 1, value, 0x22, 1000, out _) == 0) throw new TimeoutException("Native edit replacement");
    }
    public static nint ComboEdit(nint combo)
    {
        var info = new ComboBoxInfo { Size = Marshal.SizeOf<ComboBoxInfo>() };
        if (!GetComboBoxInfo(combo, ref info) || info.Edit == 0) throw new InvalidOperationException("Actual font ComboBox EDIT HWND unavailable");
        return info.Edit;
    }
    public static void Post(nint window, uint message, long wp = 0, long lp = 0) { if (!PostMessage(window, message, (nint)wp, (nint)lp)) throw new InvalidOperationException("Native message post failed"); }
}
