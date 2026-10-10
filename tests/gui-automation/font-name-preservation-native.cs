#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.Diagnostics;
using System.ComponentModel;
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
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/font-name-preservation-native.cs -- EXE ARTIFACTS");
    return 2;
}
try { return PrivateDesktop.Run(args, Path.GetFullPath(args[1]), isolation => new Scenario(args, isolation).Run()); }
catch (Exception error) { Console.Error.WriteLine(error); return 1; }

sealed class Scenario(string[] args, PrivateDesktop isolation)
{
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    const string English = "Microsoft YaHei", Localized = "微软雅黑", Unknown = "Uninstalled E2E Face 世界";
    static readonly (string Name, string Font)[] Styles = [("Default", English), ("Localized", Localized),
        ("VerticalEnglish", "@" + English), ("VerticalLocalized", "@" + Localized), ("Unknown", Unknown), ("OtherFamily", "Arial")];
    readonly string artifacts = Path.GetFullPath(args[1]);
    readonly List<object> checks = [];
    Process? app;
    nint main, manager, dialog, combo, size;
    string profile = "", fixture = "", output = "", phase = "";
    string[] expected = [];
    int launch;
    sealed record DropdownLayout(long Top, long Caret, Native.Rectangle Item, Native.Rectangle Client, int X, int Y, long Hit, bool Dropped, uint Dpi);

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
            if (previousDpi == 0) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Scenario thread PerMonitorV2 DPI context unavailable");
            var currentDpi = Native.GetThreadDpiAwarenessContext();
            File.WriteAllText(Path.Combine(artifacts, "dpi-context.json"), JsonSerializer.Serialize(new { previous_context = previousDpi.ToString("X"), current_context = currentDpi.ToString("X"), per_monitor_v2 = Native.AreDpiAwarenessContextsEqual(currentDpi, -4) }, Json));
            Require(Native.AreDpiAwarenessContextsEqual(currentDpi, -4), "Scenario thread uses the host's PerMonitorV2 DPI context");
            foreach (var preferLocalized in new[] { true, false }) RunProfile(preferLocalized);
            Result(true); return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            if (app is not null && !app.HasExited)
            {
                try { Dump("failure"); Capture("failure", dialog != 0 ? dialog : manager != 0 ? manager : main); }
                catch (Exception captureError) { Console.Error.WriteLine(captureError); }
                try
                {
                    if (dialog != 0) CloseDialog("Cancel");
                    if (manager != 0) CloseManager();
                    WriteExpected("failure-current");
                    SaveAs("failure-current");
                }
                catch (Exception saveError) { Console.Error.WriteLine(saveError); }
            }
            Result(false, error.ToString()); return 1;
        }
        finally { if (app is not null) { isolation.Stop(app); app.Dispose(); } }
    }

    void RunProfile(bool preferLocalized)
    {
        output = Path.Combine(artifacts, preferLocalized ? "localized" : "english");
        Directory.CreateDirectory(output); Prepare(preferLocalized); Start();
        phase = "baseline"; Capture("baseline", main); expected = SaveAs("baseline"); WriteExpected("baseline");
        Require(expected.Count(line => line.StartsWith("Dialogue:")) == 4 && expected.Count(line => line.StartsWith("Comment:")) == 1,
            "Native open loaded four dialogues and one comment");
        foreach (var style in Styles)
            Require(StyleFields(style.Name)[1] == style.Font, "ASS writer preserves original Fontname for " + style.Name);

        foreach (var style in Styles)
        {
            phase = "unchanged-style-" + style.Name; OpenStyle(style.Name); ExpectFont(style.Font);
            if (style.Name == "Default") VerifyCandidates(preferLocalized);
            Capture(phase, dialog); CloseDialog("OK"); CloseManager(); Compare(phase);
        }
        phase = "style-size-only"; OpenStyle("Default"); ExpectFont(English); SetSize(29); ExpectFont(English);
        Capture(phase, dialog); CloseDialog("OK"); CloseManager(); ChangeStyle("Default", 2, "29"); Compare(phase);

        phase = "copy-unknown-style"; OpenStyle("Unknown", copy: true); ExpectFont(Unknown);
        Capture(phase, dialog); CloseDialog("OK"); CloseManager(); AppendStyleCopy("Unknown"); Compare(phase);
        phase = "copy-known-English-style"; OpenStyle("Default", copy: true); ExpectFont(English);
        Require(Text(size) == "29", "Known English-alias copy displays its source's updated font size 29");
        Capture(phase, dialog); CloseDialog("OK"); CloseManager(); AppendStyleCopy("Default"); Compare(phase);

        var candidate = preferLocalized ? Localized : English;
        phase = "style-explicit-candidate"; OpenStyle("Default"); SelectCandidate(candidate); Capture(phase, dialog);
        CloseDialog("OK"); CloseManager(); ChangeStyle("Default", 1, candidate); Compare(phase);
        phase = "style-typed-font"; OpenStyle("Default"); TypeFont("Typed Style Face café 世界"); SetSize(30);
        ExpectFont("Typed Style Face café 世界"); Capture(phase, dialog); CloseDialog("OK"); CloseManager();
        ChangeStyle("Default", 1, "Typed Style Face café 世界"); ChangeStyle("Default", 2, "30"); Compare(phase);

        phase = "select-three-mixed-font-lines"; SelectLines("group-"); Compare(phase);
        phase = "font-dialog-unchanged"; OpenFont(); ExpectFont(English); VerifyCandidates(preferLocalized);
        Capture(phase, dialog); CloseDialog("OK"); Compare(phase);
        phase = "font-dialog-localized-unchanged"; SelectLines("group-Localized");
        ChangeActiveLine(1); OpenFont(); ExpectFont(Localized); Capture(phase, dialog); CloseDialog("OK"); Compare(phase);
        phase = "reselect-three-mixed-font-lines"; SelectLines("group-English"); SelectLines("group-");
        ChangeActiveLine(0); Compare(phase);
        phase = "font-dialog-preview-cancel"; var pristine = ActiveText(); OpenFont(); ExpectFont(English); SetSize(37);
        Wait(() => ActiveText().Contains("\\fs37", StringComparison.Ordinal), "Size live preview reaches the actual active edit box");
        Require(ActiveText().Contains("\\fn" + English + "\\", StringComparison.Ordinal), "Live size preview preserves the active explicit English alias");
        Capture(phase, dialog); CloseDialog("Cancel");
        Wait(() => ActiveText() == pristine, "Cancel restores the exact active text before live preview"); Compare(phase);

        phase = "font-dialog-size-only-mixed-lines"; OpenFont(); ExpectFont(English); SetSize(41); ExpectFont(English);
        Capture(phase, dialog); CloseDialog("OK");
        ChangeEvents(fields => fields[4].StartsWith("group-", StringComparison.Ordinal), text => text.Replace("\\fs24", "\\fs41", StringComparison.Ordinal));
        Compare(phase);
        Require(SavedEventText("group-English").Contains("\\fn" + English + "\\", StringComparison.Ordinal)
            && SavedEventText("group-Localized").Contains("\\fn" + Localized + "\\", StringComparison.Ordinal)
            && SavedEventText("group-Other").Contains("\\fnArial\\", StringComparison.Ordinal),
            "Size-only operation preserves each selected line's different alias and different family");

        phase = "font-dialog-mixed-lines-explicit-candidate"; OpenFont(); ExpectFont(English); SelectCandidate(candidate);
        Capture(phase, dialog); CloseDialog("OK");
        ChangeEvents(fields => fields[4].StartsWith("group-", StringComparison.Ordinal), text => text
            .Replace("\\fn" + English + "\\", "\\fn" + candidate + "\\", StringComparison.Ordinal)
            .Replace("\\fn" + Localized + "\\", "\\fn" + candidate + "\\", StringComparison.Ordinal)
            .Replace("\\fnArial\\", "\\fn" + candidate + "\\", StringComparison.Ordinal));
        Compare(phase);
        Require(new[] { "group-English", "group-Localized", "group-Other" }.All(actor => SavedEventText(actor).Contains("\\fn" + candidate + "\\", StringComparison.Ordinal)),
            "Explicit real candidate selection writes the selected spelling on all three lines even when active text already equals it");

        phase = "select-one-explicit-font-line"; SelectLines("group-English"); Compare(phase);
        phase = "font-dialog-explicit-candidate"; OpenFont(); ExpectFont(candidate); SelectCandidate(candidate);
        Wait(() => ActiveText().Contains("\\fn" + candidate + "\\", StringComparison.Ordinal), "Explicit candidate reaches the active text live preview");
        Capture(phase, dialog); CloseDialog("OK");
        Compare(phase);
        phase = "font-dialog-typed-font"; OpenFont(); ExpectFont(candidate); TypeFont("Typed Line Face 世界"); SetSize(43);
        ExpectFont("Typed Line Face 世界"); Capture(phase, dialog); CloseDialog("OK");
        ChangeEvents(fields => fields[4] == "group-English", text => text.Replace("\\fn" + candidate + "\\", "\\fnTyped Line Face 世界\\", StringComparison.Ordinal).Replace("\\fs41", "\\fs43", StringComparison.Ordinal));
        Compare(phase); NormalExit();
        var stored = JsonNode.Parse(File.ReadAllText(Path.Combine(profile, "user", "config.json")))!;
        Require(stored["Subtitle"]!["Font"]!["Prefer Localized Family Names"]!.GetValue<bool>() == preferLocalized,
            "The isolated Prefer Localized preference survives normal exit unchanged");
    }

    void Result(bool passed, string? error = null)
    {
        object[] snapshot; lock (checks) snapshot = checks.ToArray();
        File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new { passed, error, checks = snapshot }, Json));
    }
    void Require(bool condition, string description)
    {
        lock (checks) checks.Add(new { profile = Path.GetFileName(output), phase, description, passed = condition });
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
        JsonSerializer.Serialize(new { profile = Path.GetFileName(output), phase, action, target }) + Environment.NewLine);
    void Command(string menu, string label)
    {
        isolation.VerifyWindow(main, app!); Action("menu", menu + "/" + label);
        ObservedWindowsCommand.PostMenuCommand(main, app!.Id, ObservedWindowsCommand.FindMenuCommand(main, app.Id, menu, label));
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
        dialog = combo = size = 0;
    }
    void CloseManager()
    {
        var old = manager; Click(Control(manager, "Close")); Wait(() => !Windows().Contains(old), "Styles Manager closed"); manager = 0;
    }
    void OpenStyle(string name, bool copy = false)
    {
        Command("Subtitle", "Styles Manager..."); manager = Window("Styles Manager");
        var group = Native.Bounds(Control(manager, "Current script"));
        var list = Children(manager).Single(h => PrivateDesktop.ClassName(h) == "ListBox" && group.Contains(Native.Bounds(h)));
        var items = Enumerable.Range(0, checked((int)Send(list, 0x18B))).Select(index => Native.ListText(list, index)).ToArray();
        File.WriteAllText(Path.Combine(output, phase + "-current-styles.json"), JsonSerializer.Serialize(items, Json));
        var matches = Enumerable.Range(0, items.Length).Where(index => items[index] == name).ToArray();
        Require(matches.Length == 1, "Exactly one actual current-script ListBox item has the requested style name: " + name);
        var item = matches[0]; Send(list, 0x197, Math.Max(0, item - 2)); var rect = Native.ItemBounds(list, item);
        var point = Native.Position(Math.Max(1, rect.Left + 8), rect.CenterY);
        Action("native-style-item-mouse-click", name); Send(list, 0x201, 1, point); Send(list, 0x202, 0, point);
        Wait(() => Send(list, 0x190) == 1 && Send(list, 0x187, item) == 1, "Native style ListBox selects exactly the requested item: " + name);
        var buttons = Children(manager).Where(h => PrivateDesktop.ClassName(h) == "Button" && Text(h).Replace("&", "", StringComparison.Ordinal) == (copy ? "Copy" : "Edit") && group.Contains(Native.Bounds(h))).ToArray();
        Require(buttons.Length == 1, "Current-script " + (copy ? "Copy" : "Edit") + " identified uniquely by group bounds");
        Click(buttons[0]); dialog = Window("Style Editor"); DiscoverFont(); Dump(phase + "-inventory");
    }
    void OpenFont()
    {
        var button = Children(main).Single(h => PrivateDesktop.ClassName(h) == "Button" && Text(h) == "Font Face");
        Action("observed-font-command", "Font Face/id=" + Native.GetDlgCtrlID(button));
        Click(button); dialog = Window("Select Font"); DiscoverFont(); Dump(phase + "-inventory");
    }
    void DiscoverFont()
    {
        var editable = AutomationElement.FromHandle(dialog).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>()
            .Where(e => e.Current.ControlType == ControlType.ComboBox && e.TryGetCurrentPattern(ValuePattern.Pattern, out var pattern) && !((ValuePattern)pattern).Current.IsReadOnly).ToArray();
        Require(editable.Length == 1, "Exactly one editable font ComboBox with writable UIA ValuePattern");
        combo = (nint)editable[0].Current.NativeWindowHandle; isolation.VerifyWindow(combo, app!);
        Require(editable[0].TryGetCurrentPattern(ExpandCollapsePattern.Pattern, out _), "Actual font ComboBox supports ExpandCollapsePattern");
        var row = Native.Bounds(combo);
        var group = Native.Bounds(Control(dialog, "Font"));
        Require(group.Contains(row), "Actual font ComboBox is inside the labeled Font group");
        var candidates = Children(dialog).Where(h => PrivateDesktop.ClassName(h) == "Edit" && Native.GetParent(h) != combo)
            .Where(h => { var bounds = Native.Bounds(h); return group.Contains(bounds) && bounds.Left >= row.Right && Math.Abs(bounds.CenterY - row.CenterY) <= 12; }).ToArray();
        Require(candidates.Length == 1, "Font size identified uniquely as the Edit inside the Font group to the right on the font row");
        size = candidates[0];
    }
    void ExpectFont(string value)
    {
        Wait(() => Text(Native.ComboInfo(combo).Edit) == value, "Font field reads exact text: " + value);
        var pattern = (ValuePattern)AutomationElement.FromHandle(combo).GetCurrentPattern(ValuePattern.Pattern);
        Require(pattern.Current.Value == value, "Native edit and UIA ValuePattern both preserve exact font name: " + value);
    }
    void VerifyCandidates(bool preferLocalized)
    {
        var original = Text(Native.ComboInfo(combo).Edit);
        Action("native-combo-expand", original); Send(combo, 0x14F, 1);
        Wait(() => Send(combo, 0x157) != 0, "Actual font candidate dropdown expanded");
        var choices = Enumerable.Range(0, checked((int)Send(combo, 0x146))).Select(index => Native.ComboText(combo, index)).ToArray();
        File.WriteAllText(Path.Combine(output, phase + "-candidates.json"), JsonSerializer.Serialize(choices, Json));
        var preferred = preferLocalized ? Localized : English; var other = preferLocalized ? English : Localized;
        Require(choices.Contains(preferred, StringComparer.Ordinal), "Installed Microsoft YaHei alias appears in preferred candidate spelling: " + preferred);
        Require(!choices.Contains(other, StringComparer.Ordinal), "Candidate list does not include the nonpreferred duplicate alias: " + other);
        ExpectFont(original); Action("native-combo-collapse", original); Send(combo, 0x14F, 0);
        Wait(() => Send(combo, 0x157) == 0, "Candidate dropdown collapsed without selection"); ExpectFont(original);
    }
    void SelectCandidate(string value)
    {
        var deadline = Stopwatch.StartNew();
        Action("native-combo-expand-for-selection", value); Send(combo, 0x14F, 1);
        Wait(() => Send(combo, 0x157) != 0, "Candidate dropdown expanded for explicit selection", deadline);
        var choices = Enumerable.Range(0, checked((int)Send(combo, 0x146))).Select(index => Native.ComboText(combo, index)).ToArray();
        var index = Array.IndexOf(choices, value); Require(index >= 0, "Explicit selection target is a real installed candidate: " + value);
        var list = Native.ComboInfo(combo).List; isolation.VerifyWindow(list, app!);
        DropdownLayout? latest = null, previous = null; var stable = Stopwatch.StartNew(); var stage = "opening-layout";
        object Evidence(DropdownLayout layout) => new {
            stage, wanted_label = value, wanted_index = index, top_index = layout.Top, caret_index = layout.Caret,
            item_rect = layout.Item, client_rect = layout.Client, click_point = new { x = layout.X, y = layout.Y },
            raw_hit = layout.Hit, hit_index = layout.Hit & 0xFFFF, outside = (layout.Hit >> 16 & 0xFFFF) != 0,
            item_fully_visible = layout.Client.Contains(layout.Item), dropped = layout.Dropped, window_dpi = layout.Dpi,
            preparation_elapsed_ms = deadline.ElapsedMilliseconds
        };
        DropdownLayout Observe()
        {
            var rect = Native.ItemBounds(list, index); var client = Native.ClientBounds(list); var top = Send(list, 0x18E); var caret = Send(list, 0x188);
            var x = Math.Max(1, rect.Left + 8); var y = rect.CenterY; var hit = Send(list, 0x1A9, 0, Native.Position(x, y));
            latest = new(top, caret, rect, client, x, y, hit, Send(combo, 0x157) != 0, Native.GetDpiForWindow(list));
            File.WriteAllText(Path.Combine(output, phase + "-selection-layout-latest.json"), JsonSerializer.Serialize(Evidence(latest), Json));
            return latest;
        }
        bool Settled(DropdownLayout current, bool requireHit)
        {
            var ready = current.Dropped && (!requireHit || ((current.Hit >> 16 & 0xFFFF) == 0 && (current.Hit & 0xFFFF) == index && current.Client.Contains(current.Item)));
            if (!ready || current != previous) { previous = current; stable.Restart(); return false; }
            return stable.ElapsedMilliseconds >= 200;
        }
        try
        {
            Wait(() => Settled(Observe(), false), "Dropdown opening-side layout and caret are stable for 200ms before the scenario scrolls", deadline);
            stage = "scrolled-layout"; Action("native-dropdown-scroll-once", value + "/index=" + index);
            Send(list, 0x197, Math.Max(0, index - 2)); previous = null; stable.Restart();
            Wait(() => Settled(Observe(), true), "Candidate layout is stable for 200ms and its real click point hits the requested item", deadline);
        }
        catch (TimeoutException error)
        {
            File.WriteAllText(Path.Combine(output, phase + "-selection-layout-timeout.json"), JsonSerializer.Serialize(new { error = error.Message, last_observation = latest is null ? null : Evidence(latest) }, Json));
            throw;
        }
        stage = "before-click"; var final = Observe(); var finalPoint = Native.Position(final.X, final.Y); var evidence = Evidence(final);
        File.WriteAllText(Path.Combine(output, phase + "-selection-before.json"), JsonSerializer.Serialize(evidence, Json));
        Require(final.Dropped && (final.Hit >> 16 & 0xFFFF) == 0 && (final.Hit & 0xFFFF) == index && final.Client.Contains(final.Item) && final == previous,
            "Immediately before the only mouse action, the real dropdown click point still hits the requested stable item");
        Action("native-dropdown-item-mouse-click", value); Send(list, 0x201, 1, finalPoint); Send(list, 0x202, 0, finalPoint);
        File.WriteAllText(Path.Combine(output, phase + "-selection-after.json"), JsonSerializer.Serialize(new {
            before = evidence, selected_index = Send(combo, 0x147), selected_label = Text(Native.ComboInfo(combo).Edit), dropped = Send(combo, 0x157) != 0
        }, Json));
        Wait(() => Send(combo, 0x157) == 0, "Real candidate selection closes the dropdown"); ExpectFont(value);
    }
    void TypeFont(string value)
    {
        Send(combo, 0x14F, 0); var edit = Native.ComboInfo(combo).Edit; isolation.VerifyWindow(edit, app!);
        Action("native-font-edit-keystrokes", value); Native.ReplaceText(edit, "");
        foreach (var ch in value) { Send(edit, 0xB1, -1, -1); Send(edit, 0x102, ch); Thread.Sleep(25); }
        ExpectFont(value); Send(combo, 0x14F, 0);
    }
    void SetSize(int value)
    {
        isolation.VerifyWindow(size, app!); Require(Native.IsWindowEnabled(size), "Font size Edit is enabled for input");
        Action("native-size-edit", value.ToString(CultureInfo.InvariantCulture)); Native.ReplaceText(size, value.ToString(CultureInfo.InvariantCulture));
        Wait(() => Text(size) == value.ToString(CultureInfo.InvariantCulture), "Font size edit reads back the target");
    }
    nint ActiveEdit()
    {
        var found = Children(main).Where(h => PrivateDesktop.ClassName(h) == "Edit" && Text(h).Contains("group English café", StringComparison.Ordinal)).ToArray();
        if (found.Length != 1) throw new InvalidOperationException("Actual active subtitle Edit must be identified uniquely by fixture sentinel");
        return found[0];
    }
    string ActiveText() => Text(ActiveEdit());
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
        Dump(phase + "-" + actor + "-inventory"); Capture(phase + "-" + actor, dialog); CloseDialog("OK");
    }
    void Dump(string label)
    {
        File.WriteAllText(Path.Combine(output, label + ".json"), JsonSerializer.Serialize(Windows().Select(h => new {
            handle = h.ToString("X"), text = TopCaption(h), class_name = PrivateDesktop.ClassName(h), bounds = Native.Bounds(h),
            children = Children(h).Select(c => new { handle = c.ToString("X"), text = Text(c), class_name = PrivateDesktop.ClassName(c), bounds = Native.Bounds(c), enabled = Native.IsWindowEnabled(c), id = Native.GetDlgCtrlID(c) })
        }), Json));
    }
    void Capture(string label, nint window)
    {
        isolation.VerifyWindow(window, app!); ScreenCapture.SaveWindowPng(window, Path.Combine(output, label + ".png"));
    }
    string[] SaveAs(string label)
    {
        Command("File", "Save Subtitles as..."); nint save = 0;
        Wait(() => { save = Windows().SingleOrDefault(h => PrivateDesktop.ClassName(h) == "#32770" && TopCaption(h).Contains("Save", StringComparison.Ordinal)); return save != 0; }, "Native Save As dialog opened");
        var elements = AutomationElement.FromHandle(save).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
        var edit = elements.Single(e => e.Current.ControlType == ControlType.Edit && e.Current.AutomationId == "1001");
        var path = Path.Combine(output, label + ".ass"); var handle = (nint)edit.Current.NativeWindowHandle;
        isolation.VerifyWindow(handle, app!); Action("native-save-as-filename", label + ".ass"); Native.SetText(handle, path);
        var button = elements.Single(e => e.Current.ControlType == ControlType.Button && e.Current.AutomationId == "1" && e.Current.NativeWindowHandle != 0);
        Click((nint)button.Current.NativeWindowHandle);
        Wait(() => File.Exists(path) && !Windows().Contains(save), "Native Save As completed: " + label);
        Require(!Text(main).StartsWith('*'), "Native Save As clears dirty state"); return File.ReadAllLines(path);
    }
    void WriteExpected(string label) => File.WriteAllLines(Path.Combine(output, label + "-expected.ass"), expected, new UTF8Encoding(true));
    void Compare(string label)
    {
        WriteExpected(label); var actual = SaveAs(label);
        Require(actual.SequenceEqual(expected, StringComparer.Ordinal), "Complete saved ASS exactly matches the independent oracle: " + label);
    }
    string[] StyleFields(string name) => expected.Single(line => line.StartsWith("Style: " + name + ",", StringComparison.Ordinal))[7..].Split(',');
    void AppendStyleCopy(string name)
    {
        var copy = StyleFields(name); copy[0] = name + " - Copy";
        var lastStyle = Array.FindLastIndex(expected, line => line.StartsWith("Style:"));
        expected = expected.Take(lastStyle + 1).Append("Style: " + string.Join(',', copy)).Concat(expected.Skip(lastStyle + 1)).ToArray();
    }
    void ChangeStyle(string name, int field, string value)
    {
        expected = expected.Select(line => {
            if (!line.StartsWith("Style: " + name + ",", StringComparison.Ordinal)) return line;
            var fields = line[7..].Split(','); fields[field] = value; return "Style: " + string.Join(',', fields);
        }).ToArray();
    }
    void ChangeEvents(Func<string[], bool> select, Func<string, string> change)
    {
        expected = expected.Select(line => {
            if (!line.StartsWith("Dialogue:", StringComparison.Ordinal)) return line;
            var fields = line.Split(',', 10); if (select(fields)) fields[9] = change(fields[9]); return string.Join(',', fields);
        }).ToArray();
    }
    void ChangeActiveLine(int row)
    {
        var withoutActive = expected.Where(line => !line.StartsWith("Active Line:", StringComparison.Ordinal)).ToList();
        if (row != 0)
        {
            var project = withoutActive.IndexOf("[Aegisub Project Garbage]");
            if (project < 0) throw new InvalidOperationException("Expected ASS has no project metadata section");
            var end = withoutActive.FindIndex(project + 1, line => line.StartsWith("[", StringComparison.Ordinal));
            var videoPosition = withoutActive.FindIndex(project + 1, end - project - 1, line => line.StartsWith("Video Position:", StringComparison.Ordinal));
            var insertion = videoPosition >= 0 ? videoPosition : end;
            while (insertion > project + 1 && withoutActive[insertion - 1].Length == 0) --insertion;
            withoutActive.Insert(insertion, "Active Line: " + row);
        }
        expected = withoutActive.ToArray();
    }
    string SavedEventText(string actor) => File.ReadLines(Path.Combine(output, phase + ".ass"))
        .Where(line => line.StartsWith("Dialogue:", StringComparison.Ordinal)).Select(line => line.Split(',', 10)).Single(fields => fields[4] == actor)[9];
    void NormalExit()
    {
        isolation.VerifyWindow(main, app!); Action("normal-exit", "main/WM_CLOSE"); Native.Post(main, 0x10);
        Require(app!.WaitForExit(10_000), "Normal WM_CLOSE exits within ten seconds without forced termination");
        Require(isolation.ExitCode(app) == 0, "Normal host native exit code is zero");
        app.Dispose(); app = null; main = 0;
    }
    void Prepare(bool preferLocalized)
    {
        profile = Path.Combine(output, "profile"); Directory.CreateDirectory(Path.Combine(profile, "user"));
        var config = JsonNode.Parse("""
            {"Subtitle":{"Use STC":false,"Font":{"Prefer Localized Family Names":true,"Use Contains Matching":true,"Auto Expand List On Input":true,"Compact Vertical Font List":false}},"App":{"First Start":false,"Language":"en_US","Auto":{"Backup":false,"Check For Updates":false,"Load Linked Files":1,"Save":false,"Save on Every Change":false}}}
            """)!;
        config["Subtitle"]!["Font"]!["Prefer Localized Family Names"] = preferLocalized;
        File.WriteAllText(Path.Combine(profile, "user", "config.json"), config.ToJsonString(Json));
        File.WriteAllText(Path.Combine(output, "initial-preferences.json"), config.ToJsonString(Json));
        fixture = Path.Combine(output, "fixture.ass");
        var header = """
            [Script Info]
            Title: Native Font Name Preservation E2E
            Original Script: metadata sentinel café 世界
            ScriptType: v4.00+
            PlayResX: 640
            PlayResY: 480
            WrapStyle: 2
            ScaledBorderAndShadow: yes
            [Aegisub Project Garbage]
            Last Style Storage: Default
            Active Line: 0
            [V4+ Styles]
            Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
            """;
        var styles = Styles.Select(style => "Style: " + style.Name + "," + style.Font + ",24,&H00EFEFEF,&H000000FF,&H00010203,&H00040506,0,0,0,0,101,99,0.5,3,1,2,1,2,11,12,13,1");
        var events = new[] {
            "Dialogue: 1,0:00:10.00,0:00:12.34,Default,group-English,0011,0012,0013,fx-one,{\\fn" + English + "\\fs24\\bord3}group English café, 世界{\\i1}tail{\\i0}",
            "Dialogue: 2,0:00:20.00,0:00:22.56,Localized,group-Localized,0021,0022,0023,fx-two,{\\fn" + Localized + "\\fs24\\shad2}group localized 😀{\\rLocalized}reset sentinel",
            "Comment: 9,0:00:20.10,0:00:20.50,Unknown,comment-sentinel,0091,0092,0093,sentinel,{\\fn@Uninstalled Comment Face\\fs31\\b1}untouched comment 😀",
            "Dialogue: 3,0:00:30.00,0:00:32.78,OtherFamily,group-Other,0031,0032,0033,fx-three,{\\fnArial\\fs24\\1c&HABCDEF&}group other family{\\pos(120,240)}",
            "Dialogue: 4,0:00:40.00,0:00:42.90,VerticalEnglish,untouched-sentinel,0041,0042,0043,fx-four,{\\fn@" + English + "\\fs31\\u1}untouched vertical{\\u0}"
        };
        File.WriteAllText(fixture, header + "\n" + string.Join('\n', styles) + "\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n" + string.Join('\n', events) + "\n", new UTF8Encoding(true));
    }
    void Start()
    {
        var host = Path.Combine(output, "launch-" + ++launch); Directory.CreateDirectory(host);
        var start = new ProcessStartInfo(Path.GetFullPath(args[0])) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", host, "--open", fixture }) start.ArgumentList.Add(argument);
        app = isolation.Start(start, "font-name-preservation-" + launch);
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(host, "ready.json"), app, TimeSpan.FromSeconds(20)); main = isolation.MainWindow(app, 10);
    }
}

static class Native
{
    delegate bool Callback(nint window, nint data);
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] struct ComboBoxInfo { public int Size; public Rect Item, Button; public uint State; public nint Combo, Edit, List; }
    public sealed record Rectangle(int Left, int Top, int Right, int Bottom)
    {
        public int CenterY => (Top + Bottom) / 2;
        public bool Contains(Rectangle other) => other.Left >= Left && other.Top >= Top && other.Right <= Right && other.Bottom <= Bottom;
    }
    [DllImport("user32.dll")] static extern bool GetWindowRect(nint window, out Rect rect);
    [DllImport("user32.dll")] static extern bool GetClientRect(nint window, out Rect rect);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(nint parent, Callback callback, nint data);
    [DllImport("user32.dll")] static extern bool GetComboBoxInfo(nint window, ref ComboBoxInfo info);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] public static extern bool IsWindow(nint window);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(nint window);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(nint window);
    [DllImport("user32.dll")] public static extern nint GetParent(nint window);
    [DllImport("user32.dll", SetLastError = true)] public static extern nint SetThreadDpiAwarenessContext(nint context);
    [DllImport("user32.dll")] public static extern nint GetThreadDpiAwarenessContext();
    [DllImport("user32.dll")] public static extern bool AreDpiAwarenessContextsEqual(nint first, nint second);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(nint window);
    [DllImport("user32.dll", EntryPoint = "GetWindowTextLengthW", SetLastError = true)] static extern int GetCaptionLength(nint window);
    [DllImport("user32.dll", EntryPoint = "GetWindowTextW", CharSet = CharSet.Unicode, SetLastError = true)] static extern int GetCaption(nint window, StringBuilder text, int capacity);
    [DllImport("user32.dll", EntryPoint = "PostMessageW")] static extern bool PostMessage(nint window, uint message, nint wp, nint lp);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")] static extern nint SendMessage(nint window, uint message, nint wp, nint lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint SendText(nint window, uint message, nint wp, string lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint ReadText(nint window, uint message, nint wp, StringBuilder lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")] static extern nint ReadRect(nint window, uint message, nint wp, ref Rect lp, uint flags, uint timeout, out nint result);
    public static Rectangle Bounds(nint window) { if (!GetWindowRect(window, out var rect)) throw new InvalidOperationException("Native bounds unavailable"); return new(rect.Left, rect.Top, rect.Right, rect.Bottom); }
    public static Rectangle ClientBounds(nint window) { if (!GetClientRect(window, out var rect)) throw new InvalidOperationException("Native client bounds unavailable"); return new(rect.Left, rect.Top, rect.Right, rect.Bottom); }
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
        var parent = GetParent(window); var id = GetDlgCtrlID(window);
        if (parent != 0 && id != 0) Send(parent, 0x111, (0x300L << 16) | (id & 0xFFFF), window.ToInt64());
    }
    public static void Post(nint window, uint message, long wp = 0, long lp = 0) { if (!PostMessage(window, message, (nint)wp, (nint)lp)) throw new InvalidOperationException("Native message post failed"); }
    public static (nint Edit, nint List) ComboInfo(nint window)
    {
        var info = new ComboBoxInfo { Size = Marshal.SizeOf<ComboBoxInfo>() };
        if (!GetComboBoxInfo(window, ref info) || info.Edit == 0 || info.List == 0) throw new InvalidOperationException("Actual ComboBox edit/list HWND unavailable");
        return (info.Edit, info.List);
    }
    public static string ComboText(nint combo, int index) => ItemText(combo, index, 0x149, 0x148);
    public static string ListText(nint list, int index) => ItemText(list, index, 0x18A, 0x189);
    static string ItemText(nint window, int index, uint lengthMessage, uint textMessage)
    {
        var length = checked((int)Send(window, lengthMessage, index));
        if (length < 0) throw new InvalidOperationException("Native item text length unavailable");
        var text = new StringBuilder(length + 1);
        if (ReadText(window, textMessage, index, text, 0x22, 1000, out var result) == 0 || result.ToInt64() < 0) throw new InvalidOperationException("Native item text unavailable");
        return text.ToString();
    }
    public static Rectangle ItemBounds(nint list, int index)
    {
        var rect = new Rect();
        if (ReadRect(list, 0x198, index, ref rect, 0x22, 1000, out var result) == 0 || result.ToInt64() < 0 || rect.Bottom <= rect.Top) throw new InvalidOperationException("Candidate item bounds unavailable");
        return new(rect.Left, rect.Top, rect.Right, rect.Bottom);
    }
    public static long Position(int x, int y) => (y << 16) | (x & 0xFFFF);
}
