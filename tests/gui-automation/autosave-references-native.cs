#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

if (args.Length != 2 && !(args.Length == 3 && args[2] == "--cross-directory-save-as"))
{
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/autosave-references-native.cs -- EXE ARTIFACTS [--cross-directory-save-as]");
    return 2;
}
try { return PrivateDesktop.Run(args, Path.GetFullPath(args[1]), desktop => new Scenario(args, desktop).Run()); }
catch (Exception error) { Console.Error.WriteLine(error); return 1; }

sealed class Scenario(string[] args, PrivateDesktop isolation)
{
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    static readonly string[] CaseNames = ["AutosaveRelocatesSourceReferences", "AutosavePreservesLiveDocumentAndHistory", "RecoveredAutosaveResolvesSourceReferences", "DummyReferencesRemainOpaque"];
    const string Macro = "Autosave Mark Dialogue";
    const string Marker = " [autosave edit]";
    const string DummyAudio = "dummy-audio:silence?sr=44100&bd=16&ch=1&ln=441000";
    const string DummyVideo = "?dummy:24.000000:240:640:480:0:0:0:";
    readonly string artifacts = Path.GetFullPath(args[1]);
    readonly List<CheckResult> checks = [];
    readonly Dictionary<string, string> linked = [];
    Process? app;
    nint main;
    string phase = "setup", fixture = "", emptyFixture = "", dummyFixture = "", profile = "", scriptDir = "", baseDir = "", externalDir = "", backupDir = "", sourceText = "", sourceReferences = "";
    string[] baselineEvents = [], editedEvents = [];

    public int Run()
    {
        Directory.CreateDirectory(artifacts); isolation.RecordDriver();
        using var log = new StreamWriter(Path.Combine(artifacts, "driver.log")) { AutoFlush = true };
        Console.SetOut(log); Console.SetError(log);
        using var watchdog = new Timer(_ => {
            WriteResult(false, "180-second watchdog expired");
            try { if (app is not null) isolation.Stop(app); } finally { Environment.Exit(124); }
        }, null, 180_000, Timeout.Infinite);
        try
        {
            Prepare(); Start();
            phase = CaseNames[0];
            EnableAutosave();
            OpenFile(fixture, false); ReadLinkedDialog("source-linked", linked);
            Wait(() => Native.IsWindowEnabled(main) && Text(main).Contains("source.ass", StringComparison.Ordinal), "Source document opened");
            AssertMacros("source-macros", Macro, "Autosave Base", "Autosave Absolute");
            Command("Automation", Macro);
            Wait(() => Text(main).StartsWith('*'), "Macro changed the live document");
            var backup = WaitForBackup("source.ass", editedEvents);
            AssertReferenceTargets(backup);
            Require(Events(File.ReadAllLines(backup)).SequenceEqual(editedEvents), "Autosave contains the one edited dialogue and every other event unchanged");

            phase = CaseNames[1];
            Require(Text(main).StartsWith('*') && Text(main).Contains("source.ass", StringComparison.Ordinal) && !Text(main).Contains("AUTOSAVE", StringComparison.Ordinal), "Autosave preserves the live filename and dirty state");
            Require(File.ReadAllText(fixture) == sourceText, "Autosave leaves the source ASS byte-decoded contents unchanged");
            var secondEditStarted = DateTime.UtcNow;
            Command("Automation", Macro);
            var secondBackup = WaitForBackup("source.ass", Edited(editedEvents), secondEditStarted);
            AssertReferenceTargets(secondBackup);
            Require(Events(File.ReadAllLines(secondBackup)).SequenceEqual(Edited(editedEvents)), "A second live edit autosaves with original references without compounding relocation");
            var firstUndoStarted = DateTime.UtcNow;
            HistoryCommand("Undo");
            var firstUndo = WaitForBackup("source.ass", editedEvents, firstUndoStarted);
            AssertReferenceTargets(firstUndo);
            Require(Text(main).StartsWith('*'), "One Undo after two edits preserves the first edit and dirty state");
            var undoStarted = DateTime.UtcNow;
            HistoryCommand("Undo");
            Wait(() => !Text(main).StartsWith('*'), "Second Undo returns to the original saved revision");
            Require(!Text(main).StartsWith('*'), "Autosave did not add an undo step or mark the edit saved");
            var undone = WaitForBackup("source.ass", baselineEvents, undoStarted);
            AssertReferenceTargets(undone);
            Require(Events(File.ReadAllLines(undone)).SequenceEqual(baselineEvents), "Second Undo restores all original event fields and order");
            var redoStarted = DateTime.UtcNow;
            HistoryCommand("Redo");
            Wait(() => Text(main).StartsWith('*'), "One Redo restores the dirty edit");
            backup = WaitForBackup("source.ass", editedEvents, redoStarted);
            AssertReferenceTargets(backup);
            Require(Events(File.ReadAllLines(backup)).SequenceEqual(editedEvents), "One Redo restores exactly the original macro edit");
            Require(File.ReadAllText(fixture) == sourceText, "Undo and Redo autosaves leave the original source untouched");
            DismissDirtyByOpeningEmpty();

            phase = CaseNames[2];
            OpenFile(backup, false); ReadLinkedDialog("recovered-linked", linked);
            Wait(() => Native.IsWindowEnabled(main) && Text(main).Contains(Path.GetFileName(backup), StringComparison.Ordinal), "Recovered autosave opened");
            AssertMacros("recovered-macros", Macro, "Autosave Base", "Autosave Absolute");
            var recoveryEditStarted = DateTime.UtcNow;
            Command("Automation", Macro);
            Wait(() => Text(main).StartsWith('*'), "Recovered local macro executes from its original script");
            if (args.Length == 3)
            {
                var roundtrip = Path.Combine(scriptDir, "recovered-save.ass"); SaveAs(roundtrip);
                Require(Events(File.ReadAllLines(roundtrip)).SequenceEqual(Edited(editedEvents)), "Recovered macro executed and Save As persists its second edit without changing other events");
                AssertScriptTargets(roundtrip, false);
                Require(string.Equals(Resolve(scriptDir, Properties(roundtrip)["Secondary Subtitles File"]), linked["Secondary Subtitles File"], StringComparison.OrdinalIgnoreCase), "Recovered secondary subtitle source persists through Save As");
            }
            else
            {
                var recoveredBackup = WaitForBackup(Path.GetFileName(backup), Edited(editedEvents), recoveryEditStarted);
                AssertReferenceTargets(recoveredBackup);
                Require(Events(File.ReadAllLines(recoveredBackup)).SequenceEqual(Edited(editedEvents)), "Recovered macro executes and the real timer persists its second edit without changing other events");
            }
            OpenFile(emptyFixture, true);

            phase = CaseNames[3];
            OpenFile(dummyFixture, false);
            ReadLinkedDialog("dummy-linked", new() { ["Audio File"] = DummyAudio, ["Video File"] = DummyVideo });
            Wait(() => Native.IsWindowEnabled(main) && Text(main).Contains("dummy.ass", StringComparison.Ordinal), "Dummy document opened");
            Command("Automation", Macro);
            Wait(() => Text(main).StartsWith('*'), "Dummy fixture edited");
            var dummyBackup = WaitForBackup("dummy.ass", editedEvents);
            var dummyInfo = Properties(dummyBackup);
            Require(dummyInfo["Audio File"] == DummyAudio, "Autosave preserves the complete dummy audio URI exactly");
            Require(dummyInfo["Video File"] == DummyVideo, "Autosave preserves the complete dummy video URI exactly");
            AssertScriptTargets(dummyBackup, true);
            Require(Events(File.ReadAllLines(dummyBackup)).SequenceEqual(editedEvents), "Dummy autosave preserves the edited subtitle events");
            DismissDirtyByOpeningEmpty();
            var closeNeedsDiscard = Text(main).StartsWith('*');
            Native.Post(main, 0x10, 0, 0);
            if (closeNeedsDiscard) DeclineUnsavedChanges();
            Require(app!.WaitForExit(10_000), "Host closes normally within ten seconds");
            Require(isolation.ExitCode(app) == 0, "Normal host exit code is zero");
            WriteResult(true, null);
            using var report = JsonDocument.Parse(File.ReadAllText(Path.Combine(artifacts, "result.json")));
            var cases = report.RootElement.GetProperty("cases").EnumerateArray().ToArray();
            if (!report.RootElement.GetProperty("passed").GetBoolean() || !cases.Select(c => c.GetProperty("name").GetString()).SequenceEqual(CaseNames)
                || cases.Any(c => c.GetProperty("status").GetString() != "passed" || c.GetProperty("checks").GetInt32() == 0)
                || report.RootElement.GetProperty("checks").EnumerateArray().Any(c => !c.GetProperty("Passed").GetBoolean()))
                throw new InvalidOperationException("Result report has missing, empty or unsuccessful cases");
            return 0;
        }
        catch (Exception error)
        {
            if (app is not null && !app.HasExited)
                try { Dump("failure"); } catch (Exception captureError) { Console.Error.WriteLine(captureError); }
            WriteResult(false, error.ToString()); Console.Error.WriteLine(error); return 1;
        }
        finally { if (app is not null) { isolation.Stop(app); app.Dispose(); } }
    }

    void EnableAutosave()
    {
        Command("View", "Options..."); nint dialog = 0;
        Wait(() => { dialog = Windows().SingleOrDefault(h => Text(h) == "Preferences"); return dialog != 0; }, "Preferences opened");
        var page = AutomationElement.FromHandle(dialog).FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.TreeItem))
            .Cast<AutomationElement>().Single(e => e.Current.Name == "Backup");
        if (page.Current.ProcessId != app!.Id) throw new InvalidOperationException("Backup page belongs to another process");
        ((SelectionItemPattern)page.GetCurrentPattern(SelectionItemPattern.Pattern)).Select();
        AutomationElement[] Elements() => AutomationElement.FromHandle(dialog).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().Where(e => !e.Current.IsOffscreen).ToArray();
        Wait(() => Elements().Count(e => e.Current.ControlType == ControlType.CheckBox && e.Current.Name == "Enable") == 2, "Automatic Save and Backup controls appeared");
        void Record(string label)
        {
            File.WriteAllText(Path.Combine(artifacts, label + ".json"), JsonSerializer.Serialize(Elements().Select(e => new {
                name = e.Current.Name, type = e.Current.ControlType.ProgrammaticName, id = e.Current.AutomationId,
                enabled = e.Current.IsEnabled, top = e.Current.BoundingRectangle.Top,
                value = e.TryGetCurrentPattern(ValuePattern.Pattern, out var value) ? ((ValuePattern)value).Current.Value : null,
                toggle = e.TryGetCurrentPattern(TogglePattern.Pattern, out var toggle) ? ((TogglePattern)toggle).Current.ToggleState.ToString() : null
            }), Json));
            Capture(dialog, label);
        }
        Record("autosave-options-before");
        var enable = Elements().Where(e => e.Current.ControlType == ControlType.CheckBox && e.Current.Name == "Enable").OrderBy(e => e.Current.BoundingRectangle.Top).First();
        var toggle = (TogglePattern)enable.GetCurrentPattern(TogglePattern.Pattern);
        Require(toggle.Current.ToggleState == ToggleState.Off, "GUI test startup initially disables automatic save");
        var handle = (nint)enable.Current.NativeWindowHandle; isolation.VerifyWindow(handle, app);
        Native.Post(handle, 0xF5, 0, 0);
        Wait(() => toggle.Current.ToggleState == ToggleState.On, "Native Automatic Save Enable checkbox is checked");
        var values = Children(dialog).Where(h => PrivateDesktop.ClassName(h) == "Edit" && Native.IsWindowEnabled(h)).Select(Text).ToArray();
        File.WriteAllText(Path.Combine(artifacts, "autosave-option-values.json"), JsonSerializer.Serialize(new { values }, Json));
        Require(values.Contains("2"), "Actual enabled autosave interval is two seconds");
        Require(values.Any(value => value is not null && string.Equals(value, backupDir, StringComparison.OrdinalIgnoreCase)), "Actual enabled autosave directory is the isolated recovery directory");
        Record("autosave-options-enabled");
        var apply = Children(dialog).Single(h => PrivateDesktop.ClassName(h) == "Button" && Text(h).Replace("&", "", StringComparison.Ordinal) == "Apply");
        Wait(() => Native.IsWindowEnabled(apply), "Autosave enablement creates a pending preference change");
        Click(dialog, "Apply");
        Wait(() => !Native.IsWindowEnabled(apply), "Apply commits autosave enablement through the real option subscription");
        Require(toggle.Current.ToggleState == ToggleState.On, "Automatic Save remains enabled after Apply");
        Record("autosave-options-applied");
        Click(dialog, "Cancel");
        Wait(() => !Windows().Contains(dialog) && Native.IsWindowEnabled(main), "Applied Preferences closed");
    }
    void HistoryCommand(string operation)
    {
        isolation.VerifyWindow(main, app!);
        var before = Native.ReadMenu(main, "Edit", false);
        var after = Native.ReadMenu(main, "Edit", true);
        File.WriteAllText(Path.Combine(artifacts, "history-menu-" + checks.Count + ".json"), JsonSerializer.Serialize(new { operation, before, after }, Json));
        var item = after.Single(item => item.Label == operation || item.Label.StartsWith(operation + " ", StringComparison.Ordinal));
        Require(item.Enabled, "Observed refreshed " + operation + " menu command is enabled");
        Action("history-menu", item.Label);
        ObservedWindowsCommand.PostMenuCommand(main, app!.Id, item.Id);
    }
    void DismissDirtyByOpeningEmpty()
    {
        Require(Text(main).StartsWith('*'), "Live edit still requires an explicit discard before opening another document");
        OpenFile(emptyFixture, true);
    }
    void DeclineUnsavedChanges()
    {
        nint warning = 0;
        Wait(() => { warning = Windows().SingleOrDefault(h => Text(h) == "Unsaved changes"); return warning != 0; }, "Observed dirty document prompts before being discarded");
        DeclineInteraction(warning);
        Wait(() => !Windows().Contains(warning), "Unsaved changes prompt closed after explicit discard");
    }
    void DeclineInteraction(nint dialog)
    {
        isolation.VerifyWindow(dialog, app!);
        var button = AutomationElement.FromHandle(dialog).FindFirst(TreeScope.Descendants, new PropertyCondition(AutomationElement.AutomationIdProperty, "CommandButton_7"))
            ?? throw new InvalidOperationException("Observed native interaction has no No button");
        if (button.Current.ProcessId != app!.Id || !button.Current.IsEnabled) throw new InvalidOperationException("No button does not belong to the enabled host interaction");
        Action("interaction-no", Text(dialog));
        var handle = (nint)button.Current.NativeWindowHandle;
        isolation.VerifyWindow(handle, app);
        Native.Post(handle, 0xF5, 0, 0);
    }
    void ReadLinkedDialog(string label, Dictionary<string, string> expected)
    {
        nint dialog = 0;
        Wait(() => { dialog = Windows().SingleOrDefault(h => Text(h) == "(Un)Load files?"); return dialog != 0; }, "Native linked-file confirmation opened");
        var content = AutomationElement.FromHandle(dialog).FindFirst(TreeScope.Descendants, new PropertyCondition(AutomationElement.AutomationIdProperty, "ContentText"))
            ?? throw new InvalidOperationException("Linked-file dialog has no ContentText");
        if (content.Current.ProcessId != app!.Id) throw new InvalidOperationException("Dialog content belongs to another process");
        var instruction = AutomationElement.FromHandle(dialog).FindFirst(TreeScope.Descendants, new PropertyCondition(AutomationElement.AutomationIdProperty, "MainInstruction"))
            ?? throw new InvalidOperationException("Linked-file dialog has no MainInstruction");
        if (instruction.Current.ProcessId != app.Id) throw new InvalidOperationException("Dialog instruction belongs to another process");
        var heading = instruction.Current.Name;
        var actual = content.Current.Name.Replace("\r\n", "\n", StringComparison.Ordinal);
        var labels = new Dictionary<string, string> { ["Audio File"] = "audio", ["Video File"] = "video", ["Timecodes File"] = "timecodes", ["Keyframes File"] = "keyframes" };
        var expectedEntries = labels.Where(pair => expected.ContainsKey(pair.Key)).ToArray();
        var lines = actual.Split('\n');
        File.WriteAllText(Path.Combine(artifacts, label + ".json"), JsonSerializer.Serialize(new { expected, heading, actual }, Json));
        Capture(dialog, label);
        Require(lines.Length == expectedEntries.Length && heading == "Do you want to load/unload the associated files?", "Native linked-file confirmation has exactly the expected entries");
        for (var index = 0; index < expectedEntries.Length; index++)
        {
            var entry = expectedEntries[index];
            var prefix = "Load " + entry.Value + " file: ";
            var line = lines[index];
            Require(line.StartsWith(prefix, StringComparison.Ordinal), "Native linked-file confirmation preserves expected type and order at position " + (index + 1));
            var path = line[prefix.Length..];
            var target = expected[entry.Key];
            var dummy = target.StartsWith("?dummy", StringComparison.Ordinal) || target.StartsWith("dummy-audio:", StringComparison.Ordinal);
            Require(dummy ? path == target : string.Equals(Path.GetFullPath(path), target, StringComparison.OrdinalIgnoreCase), "Native " + entry.Key + " resolves to the original source");
        }
        DeclineInteraction(dialog); Wait(() => !Windows().Contains(dialog), "Linked-file confirmation dismissed without decoding media");
    }
    string WaitForBackup(string name, string[] expectedEvents, DateTime? writtenAfter = null)
    {
        string? result = null;
        Wait(() => {
            foreach (var path in Directory.EnumerateFiles(backupDir, name + ".*.AUTOSAVE.ass").Where(p => writtenAfter is null || File.GetLastWriteTimeUtc(p) >= writtenAfter.Value).OrderByDescending(File.GetLastWriteTimeUtc))
                try { if (Events(File.ReadAllLines(path)).SequenceEqual(expectedEvents)) { result = path; return true; } }
                catch (IOException error)
                {
                    File.AppendAllText(Path.Combine(artifacts, "autosave-read-pending.ndjson"), JsonSerializer.Serialize(new {
                        phase, path = Path.GetRelativePath(artifacts, path), error = error.Message, error.HResult, observedAtUtc = DateTime.UtcNow
                    }) + Environment.NewLine);
                }
            return false;
        }, "Timer autosave with expected event revision: " + name);
        var copy = Path.Combine(artifacts, phase + "-" + checks.Count + ".ass");
        File.Copy(result!, copy, true);
        Action("observed-autosave", Path.GetRelativePath(artifacts, result!));
        return result!;
    }
    static Dictionary<string, string> Properties(string path) => File.ReadAllLines(path).Where(line => line.Contains(": ", StringComparison.Ordinal))
        .GroupBy(line => line[..line.IndexOf(": ", StringComparison.Ordinal)])
        .ToDictionary(group => group.Key, group => group.First()[(group.Key.Length + 2)..]);
    static string Resolve(string directory, string reference) => Path.GetFullPath(Path.Combine(directory, reference));
    void AssertReferenceTargets(string path)
    {
        var properties = Properties(path);
        foreach (var pair in linked)
            Require(properties.TryGetValue(pair.Key, out var value) && string.Equals(Resolve(Path.GetDirectoryName(path)!, value), pair.Value, StringComparison.OrdinalIgnoreCase), "Autosave " + pair.Key + " resolves to the original source");
        AssertScriptTargets(path, true);
    }
    void AssertScriptTargets(string path, bool preservePrefixes)
    {
        var references = Properties(path)["Automation Scripts"].Split('|');
        var expected = new[] { Path.Combine(scriptDir, "local.lua"), Path.Combine(baseDir, "base.lua"), Path.Combine(externalDir, "absolute.lua") };
        Require(references.Length == expected.Length, "Autosave retains all three scripts in the original order");
        for (int index = 0; index < references.Length; index++)
        {
            var reference = references[index];
            var directory = reference[0] switch { '~' => Path.GetDirectoryName(path)!, '$' => baseDir, '/' => "", _ => throw new InvalidOperationException("Unknown reference prefix") };
            Require(string.Equals(Resolve(directory, reference[1..]), expected[index], StringComparison.OrdinalIgnoreCase), "Script reference resolves to original source at position " + (index + 1));
        }
        if (preservePrefixes)
        {
            var original = sourceReferences.Split('|');
            Require(references[1] == original[1], "Autosave preserves the base script reference string exactly");
        }
    }
    static string[] Edited(string[] events) => events.Select((line, index) => index == 0 ? line + Marker : line).ToArray();
    void Prepare()
    {
        profile = Path.Combine(artifacts, "profile"); scriptDir = Path.Combine(artifacts, "documents");
        baseDir = Path.Combine(artifacts, "automation-base", "nested"); externalDir = Path.Combine(artifacts, "external");
        backupDir = Path.Combine(artifacts, "recovery", "autosaves");
        if (Directory.Exists(backupDir) && Directory.EnumerateFileSystemEntries(backupDir).Any()) throw new InvalidOperationException("Use a fresh artifact directory; existing autosaves cannot be evidence for this run");
        foreach (var dir in new[] { Path.Combine(profile, "user"), scriptDir, baseDir, externalDir, backupDir }) Directory.CreateDirectory(dir);
        var config = JsonNode.Parse("""
            {"Subtitle":{"Use STC":false},"App":{"First Start":false,"Language":"en_US","Auto":{"Backup":false,"Check For Updates":false,"Load Linked Files":2,"Save":true,"Save Every Seconds":2,"Save on Every Change":false}},"Automation":{"Lua Workspace":{"Enable LuaLS":false}},"Path":{"Auto":{"Save":""},"Automation":{"Autoload":"","Base":""}}}
            """)!;
        config["Path"]!["Auto"]!["Save"] = backupDir;
        config["Path"]!["Automation"]!["Base"] = baseDir;
        File.WriteAllText(Path.Combine(profile, "user", "config.json"), config.ToJsonString(Json));
        AutomationTestProfile.WriteMenusWithoutClipboardValidation(profile, artifacts);
        File.WriteAllText(Path.Combine(scriptDir, "local.lua"), """
            script_name = "Autosave Local"
            aegisub.register_macro("Autosave Mark Dialogue", "Change the first dialogue", function(subs)
                for i = 1, #subs do
                    local line = subs[i]
                    if line.class == "dialogue" and not line.comment then
                        line.text = line.text .. " [autosave edit]"
                        subs[i] = line
                        aegisub.set_undo_point("Autosave fixture edit")
                        return
                    end
                end
                error("Fixture has no dialogue")
            end)
            """, new UTF8Encoding(false));
        File.WriteAllText(Path.Combine(baseDir, "base.lua"), "script_name='Autosave Base'\naegisub.register_macro('Autosave Base', '', function() end)\n");
        File.WriteAllText(Path.Combine(externalDir, "absolute.lua"), "script_name='Autosave Absolute'\naegisub.register_macro('Autosave Absolute', '', function() end)\n");
        sourceReferences = "~local.lua|$base.lua|/" + Path.Combine(externalDir, "absolute.lua").Replace('\\', '/');
        var document = """
            [Script Info]
            Title: Autosave References E2E
            ScriptType: v4.00+
            PlayResX: 640
            PlayResY: 480
            [Aegisub Project Garbage]
            PROJECT_PROPERTIES
            [V4+ Styles]
            Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
            Style: Default,Arial,24,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1
            [Events]
            Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
            Dialogue: 0,0:00:01.00,0:00:03.00,Default,first,0,0,0,,{\i1}café, 世界
            Comment: 1,0:00:02.00,0:00:04.00,Default,comment,0,0,0,sentinel,Keep this comment
            Dialogue: 2,0:00:05.00,0:00:07.25,Default,last,0,0,0,,Second dialogue
            """;
        fixture = Path.Combine(scriptDir, "source.ass"); emptyFixture = Path.Combine(scriptDir, "empty.ass"); dummyFixture = Path.Combine(scriptDir, "dummy.ass");
        var secondary = Path.Combine(scriptDir, "secondary.ass");
        File.WriteAllText(secondary, document.Replace("PROJECT_PROPERTIES", "", StringComparison.Ordinal), new UTF8Encoding(true));
        linked["Audio File"] = Path.Combine(externalDir, "audio.wav");
        linked["Video File"] = Path.Combine(externalDir, "video.avi");
        linked["Timecodes File"] = Path.Combine(scriptDir, "timecodes.txt");
        linked["Keyframes File"] = Path.Combine(scriptDir, "keyframes.txt");
        linked["Secondary Subtitles File"] = secondary;
        File.WriteAllText(linked["Timecodes File"], "# timecode format v2\n0\n41.708\n83.417\n125.125\n");
        File.WriteAllText(linked["Keyframes File"], "# keyframe format v1\nfps 23.976\n0\n2\n");
        File.WriteAllText(linked["Audio File"], "Path-resolution fixture; media decoding is intentionally not requested.\n");
        File.WriteAllText(linked["Video File"], "Path-resolution fixture; media decoding is intentionally not requested.\n");
        var properties = "Automation Scripts: " + sourceReferences + "\n" + string.Join('\n', linked.Select(pair => pair.Key + ": " + Path.GetRelativePath(scriptDir, pair.Value).Replace('\\', '/')));
        sourceText = document.Replace("PROJECT_PROPERTIES", properties, StringComparison.Ordinal);
        File.WriteAllText(fixture, sourceText, new UTF8Encoding(true));
        File.WriteAllText(emptyFixture, document.Replace("PROJECT_PROPERTIES", "", StringComparison.Ordinal), new UTF8Encoding(true));
        File.WriteAllText(dummyFixture, document.Replace("PROJECT_PROPERTIES", "Automation Scripts: " + sourceReferences + "\nAudio File: " + DummyAudio + "\nVideo File: " + DummyVideo, StringComparison.Ordinal), new UTF8Encoding(true));
        baselineEvents = Events(document.Split('\n').Select(line => line.TrimEnd('\r')));
        editedEvents = Edited(baselineEvents);
    }
    void Start()
    {
        var host = Path.Combine(artifacts, "host"); Directory.CreateDirectory(host);
        var start = new ProcessStartInfo(Path.GetFullPath(args[0])) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", host, "--open", emptyFixture }) start.ArgumentList.Add(argument);
        app = isolation.Start(start, "autosave-references");
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(host, "ready.json"), app, TimeSpan.FromSeconds(20));
        main = isolation.MainWindow(app, 10);
    }
    void WriteResult(bool passed, string? error)
    {
        File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new {
            passed, error,
            cases = CaseNames.Select(name => new { name, status = checks.Any(c => c.Name == name && !c.Passed) ? "failed" : passed && checks.Any(c => c.Name == name) ? "passed" : "incomplete", checks = checks.Count(c => c.Name == name) }), checks
        }, Json));
    }
    void Require(bool condition, string description)
    {
        checks.Add(new(phase, description, condition)); Console.WriteLine(phase + ": " + description + ": " + condition);
        if (!condition) throw new InvalidOperationException(description);
    }
    void Wait(Func<bool> predicate, string description)
    {
        var timer = Stopwatch.StartNew();
        while (timer.ElapsedMilliseconds < 10_000)
        {
            isolation.VerifyCurrent();
            if (app is { HasExited: true }) throw new InvalidOperationException("Host exited: " + description);
            if (predicate()) return;
            Thread.Sleep(40);
        }
        throw new TimeoutException(description);
    }
    string Text(nint window) => isolation.ReadText(window, app!);
    nint[] Windows() => isolation.Windows(app!).Where(Native.IsWindowVisible).ToArray();
    nint[] Children(nint window) => Native.Children(window).Where(Native.IsWindowVisible).ToArray();
    void Action(string action, string target) => File.AppendAllText(Path.Combine(artifacts, "actions.ndjson"), JsonSerializer.Serialize(new { phase, action, target }) + Environment.NewLine);
    void Command(string menu, string label)
    {
        isolation.VerifyWindow(main, app!); Action("menu", menu + "/" + label);
        ObservedWindowsCommand.PostMenuCommand(main, app!.Id, ObservedWindowsCommand.FindMenuCommand(main, app.Id, menu, label));
    }
    void Click(nint parent, string label)
    {
        var button = Children(parent).Single(h => PrivateDesktop.ClassName(h) == "Button" && Text(h).Replace("&", "", StringComparison.Ordinal) == label);
        isolation.VerifyWindow(button, app!);
        if (!Native.IsWindowEnabled(button)) throw new InvalidOperationException("Observed button is disabled: " + label);
        Action("button", label); Native.Post(button, 0xF5, 0, 0);
    }
    void AssertMacros(string label, params string[] expected)
    {
        isolation.VerifyWindow(main, app!); var entries = Native.MenuLabels(main, "Automation");
        File.WriteAllText(Path.Combine(artifacts, label + ".json"), JsonSerializer.Serialize(new { expected, entries }, Json));
        Require(entries.Where(item => item.StartsWith("Autosave ", StringComparison.Ordinal)).Order(StringComparer.Ordinal).SequenceEqual(expected.Order(StringComparer.Ordinal)), "Actual Automation menu exposes exactly the retained fixture macros");
        if (expected.Length == 0) Require(entries.Contains("No Automation macros loaded"), "Empty local manager publishes the empty macro menu");
    }
    nint FileDialog(string title)
    {
        nint result = 0;
        Wait(() => { result = Windows().SingleOrDefault(h => PrivateDesktop.ClassName(h) == "#32770" && Text(h) == title); return result != 0; }, "Native file dialog opened: " + title);
        return result;
    }
    void FillFileDialog(nint picker, string path)
    {
        var elements = AutomationElement.FromHandle(picker).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
        var edit = elements.Single(e => e.Current.ControlType == ControlType.Edit && e.Current.AutomationId is "1001" or "1148");
        var handle = (nint)edit.Current.NativeWindowHandle; isolation.VerifyWindow(handle, app!); Native.SetText(handle, path);
        var button = elements.Single(e => e.Current.ControlType == ControlType.Button && e.Current.AutomationId == "1" && e.Current.NativeWindowHandle != 0);
        handle = (nint)button.Current.NativeWindowHandle; isolation.VerifyWindow(handle, app!);
        Action("file-dialog-path", Path.GetRelativePath(artifacts, path)); ObservedWindowsCommand.PostButtonClick(handle, app!.Id);
    }
    void OpenFile(string path, bool complete)
    {
        var needsDiscard = Text(main).StartsWith('*');
        Command("File", "Open Subtitles...");
        if (needsDiscard) DeclineUnsavedChanges();
        var picker = FileDialog("Open subtitles file"); FillFileDialog(picker, path);
        Wait(() => !Windows().Contains(picker), "Native open file picker closed");
        if (complete) Wait(() => Native.IsWindowEnabled(main) && Text(main).Contains(Path.GetFileName(path), StringComparison.Ordinal), "Native file open completed");
    }
    void SaveAs(string path)
    {
        Command("File", "Save Subtitles as..."); nint picker = 0;
        Wait(() => { picker = Windows().SingleOrDefault(h => PrivateDesktop.ClassName(h) == "#32770" && Text(h).Contains("Save", StringComparison.Ordinal)); return picker != 0; }, "Native Save As opened");
        FillFileDialog(picker, path);
        Wait(() => File.Exists(path) && !Windows().Contains(picker), "Native Save As wrote output");
        Require(!Text(main).StartsWith('*'), "Native Save As clears document dirty state");
    }
    void Capture(nint window, string label)
    {
        isolation.VerifyWindow(window, app!); ScreenCapture.SaveWindowPng(window, Path.Combine(artifacts, label + ".png"));
    }
    void Dump(string label)
    {
        File.WriteAllText(Path.Combine(artifacts, label + ".json"), JsonSerializer.Serialize(Windows().Select(h => new {
            handle = h.ToString("X"), text = Text(h), class_name = PrivateDesktop.ClassName(h),
            children = Children(h).Select(c => new { handle = c.ToString("X"), text = Text(c), class_name = PrivateDesktop.ClassName(c), enabled = Native.IsWindowEnabled(c) }),
            accessible = AutomationElement.FromHandle(h).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().Select(e => new { name = e.Current.Name, type = e.Current.ControlType.ProgrammaticName, id = e.Current.AutomationId })
        }), Json));
        foreach (var window in Windows().Where(h => PrivateDesktop.ClassName(h) == "#32770" || PrivateDesktop.ClassName(h).StartsWith("wxWindow", StringComparison.Ordinal))) Capture(window, label + "-" + window.ToString("X"));
    }
    static string[] Events(IEnumerable<string> lines) => lines.Where(line => line.StartsWith("Dialogue:", StringComparison.Ordinal) || line.StartsWith("Comment:", StringComparison.Ordinal)).ToArray();
}

sealed record CheckResult(string Name, string Description, bool Passed);

static class Native
{
    delegate bool Callback(nint window, nint data);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(nint parent, Callback callback, nint data);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(nint window);
    [DllImport("user32.dll", EntryPoint = "PostMessageW")] static extern bool PostMessage(nint window, uint message, nint wp, nint lp);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint SendText(nint window, uint message, nint wp, string lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")] static extern nint SendNumber(nint window, uint message, nint wp, nint lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll")] static extern uint GetMenuItemID(nint menu, int position);
    [DllImport("user32.dll")] static extern uint GetMenuState(nint menu, uint position, uint flags);
    [DllImport("user32.dll")] static extern nint GetMenu(nint window);
    [DllImport("user32.dll")] static extern nint GetSubMenu(nint menu, int position);
    [DllImport("user32.dll")] static extern int GetMenuItemCount(nint menu);
    [DllImport("user32.dll", EntryPoint = "GetMenuStringW", CharSet = CharSet.Unicode)] static extern int GetMenuString(nint menu, uint position, StringBuilder text, int capacity, uint flags);
    public static nint[] Children(nint parent) { List<nint> found = []; Callback callback = (h, _) => { found.Add(h); return true; }; EnumChildWindows(parent, callback, 0); GC.KeepAlive(callback); return found.ToArray(); }
    public static void SetText(nint window, string value) { if (SendText(window, 0xC, 0, value, 0x22, 1000, out _) == 0) throw new TimeoutException("Native text entry"); }
    public static void Post(nint window, uint message, long wp, long lp) { if (!PostMessage(window, message, (nint)wp, (nint)lp)) throw new InvalidOperationException("Native message post failed"); }
    static string Label(nint menu, int position)
    {
        var value = new StringBuilder(512); var length = GetMenuString(menu, (uint)position, value, value.Capacity, 0x400);
        if (length >= value.Capacity - 1) throw new InvalidOperationException("Native menu text is truncated");
        return value.ToString().Split('\t')[0].Replace("&", "", StringComparison.Ordinal);
    }
    public sealed record MenuItem(string Label, int Id, bool Enabled);
    static void Send(nint window, uint message, nint wp)
    {
        if (SendNumber(window, message, wp, 0, 0x22, 1000, out _) == 0) throw new TimeoutException("Native menu initialization");
    }
    public static MenuItem[] ReadMenu(nint window, string name, bool refresh)
    {
        var menu = GetMenu(window);
        var position = Enumerable.Range(0, GetMenuItemCount(menu)).Single(i => Label(menu, i) == name);
        var sub = GetSubMenu(menu, position);
        if (sub == 0) throw new InvalidOperationException("Observed menu has no submenu");
        if (refresh) Send(window, 0x117, sub);
        try
        {
            return Enumerable.Range(0, GetMenuItemCount(sub)).Where(i => GetMenuItemID(sub, i) is > 0 and < 65536).Select(i => {
                var state = GetMenuState(sub, (uint)i, 0x400);
                if (state == uint.MaxValue) throw new InvalidOperationException("Native menu state is unavailable");
                return new MenuItem(Label(sub, i), checked((int)GetMenuItemID(sub, i)), (state & 3) == 0);
            }).ToArray();
        }
        finally { if (refresh) Send(window, 0x125, sub); }
    }
    public static string[] MenuLabels(nint window, string name)
    {
        var menu = GetMenu(window);
        var position = Enumerable.Range(0, GetMenuItemCount(menu)).Single(i => Label(menu, i) == name);
        var sub = GetSubMenu(menu, position);
        return Enumerable.Range(0, GetMenuItemCount(sub)).Select(i => Label(sub, i)).Where(label => label.Length != 0).ToArray();
    }
}
