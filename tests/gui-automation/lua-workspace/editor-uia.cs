#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:property InvariantGlobalization=false
#:project ../driver/Aegisub.GuiAutomation.Driver.csproj

using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Collections.Specialized;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Windows.Automation;
using System.Windows.Forms;
using Aegisub.GuiAutomation.Driver;

const string workerFlag = "--editor-worker";
try
{
    if (!args.Contains(workerFlag))
        return Supervise(args);
    return Run(args.Where(arg => arg != workerFlag).ToArray());
}
catch (Exception error)
{
    Console.Error.WriteLine($"editor-uia.error={error.GetType().Name}:{error.Message}");
    return 1;
}

static int Supervise(string[] args)
{
    var total = Stopwatch.StartNew();
    var artifactsIndex = Array.IndexOf(args, "--artifacts");
    if (artifactsIndex < 0 || artifactsIndex + 1 >= args.Length) throw new ArgumentException("--artifacts is required");
    var artifacts = Path.GetFullPath(args[artifactsIndex + 1]);
    Directory.CreateDirectory(artifacts);
    var receiptPath = Path.Combine(artifacts, "clipboard-receipt.json");
    File.Delete(receiptPath);
    File.Delete(Path.Combine(artifacts, "ready.json"));
    var initialSequence = GuardedKeyboard.ClipboardSequence();
    var snapshot = ClipboardSafety.OnSta(ClipboardSafety.Capture);
    if (GuardedKeyboard.ClipboardSequence() != initialSequence)
        throw new InvalidOperationException("Clipboard changed while its original contents were being captured");
    var executable = Environment.ProcessPath ?? throw new InvalidOperationException("Driver process path is unavailable");
    var start = new ProcessStartInfo(executable) { UseShellExecute = false };
    if (Path.GetFileNameWithoutExtension(executable).Equals("dotnet", StringComparison.OrdinalIgnoreCase))
        start.ArgumentList.Add(Assembly.GetEntryAssembly()?.Location ?? throw new InvalidOperationException("Driver assembly is unavailable"));
    start.ArgumentList.Add("--editor-worker");
    start.Environment["AEGISUB_UIA_CLIPBOARD_START_SEQUENCE"] = initialSequence.ToString(System.Globalization.CultureInfo.InvariantCulture);
    foreach (var arg in args) start.ArgumentList.Add(arg);
    using var worker = Process.Start(start) ?? throw new InvalidOperationException("Could not start UIA worker");
    var timedOut = false;
    var unavailableEditor = args.Zip(args.Skip(1)).Any(pair => pair.First == "--scenario" && pair.Second == "editor-unavailable");
    var totalBudgetSeconds = unavailableEditor ? 150 : 120;
    try
    {
        var workerBudget = TimeSpan.FromSeconds(unavailableEditor ? 140 : 110) - total.Elapsed;
        if (workerBudget <= TimeSpan.Zero || !worker.WaitForExit(workerBudget))
        {
            timedOut = true;
            worker.Kill(entireProcessTree: true);
            if (!worker.WaitForExit(TimeSpan.FromSeconds(5)))
                throw new TimeoutException("UIA worker could not be stopped before clipboard cleanup");
        }
    }
    finally
    {
        if (!worker.HasExited)
        {
            worker.Kill(entireProcessTree: true);
            if (!worker.WaitForExit(TimeSpan.FromSeconds(5)))
                throw new TimeoutException("UIA worker remains active; clipboard restoration was not attempted");
        }
        var readyPath = Path.Combine(artifacts, "ready.json");
        if (File.Exists(readyPath))
        {
            var readyPid = JsonDocument.Parse(File.ReadAllText(readyPath)).RootElement.GetProperty("process_id").GetInt32();
            try
            {
                using var host = Process.GetProcessById(readyPid);
                if (!host.HasExited) throw new InvalidOperationException("GUI host remains active; clipboard restoration was not attempted");
            }
            catch (ArgumentException) { }
        }
        var receipt = ClipboardReceipt.Read(receiptPath);
        var currentSequence = GuardedKeyboard.ClipboardSequence();
        string status;
        if (receipt is not null && currentSequence == receipt.Sequence)
        {
            try
            {
                ClipboardSafety.Restore(snapshot, receipt.Sequence);
                status = "restored";
            }
            catch (Exception error)
            {
                File.WriteAllText(Path.Combine(artifacts, "clipboard-supervisor.json"), JsonSerializer.Serialize(new { InitialSequence = initialSequence, FinalSequence = currentSequence, Receipt = receipt, Status = "cleanup-failed", Error = error.Message }, new JsonSerializerOptions { WriteIndented = true }));
                throw;
            }
        }
        else if (receipt is null && currentSequence == initialSequence)
            status = "unchanged";
        else
            status = "external-or-unreceipted-change-preserved";
        File.WriteAllText(Path.Combine(artifacts, "clipboard-supervisor.json"), JsonSerializer.Serialize(new { InitialSequence = initialSequence, FinalSequence = currentSequence, Receipt = receipt, Status = status }, new JsonSerializerOptions { WriteIndented = true }));
        if (status == "external-or-unreceipted-change-preserved")
            throw new InvalidOperationException("Clipboard changed without a matching GUI-test receipt; external content was preserved");
    }
    if (timedOut) throw new TimeoutException($"Lua Workspace GUI E2E exceeded its {totalBudgetSeconds}-second total limit");
    return worker.ExitCode;
}

static int Run(string[] args)
{
    string? exe = null;
    string? artifacts = null;
    var scenario = "editor";
    for (var i = 0; i < args.Length; ++i)
    {
        switch (args[i])
        {
            case "--exe": exe = Path.GetFullPath(args[++i]); break;
            case "--artifacts": artifacts = Path.GetFullPath(args[++i]); break;
            case "--scenario": scenario = args[++i]; break;
            default: throw new ArgumentException($"Unknown argument: {args[i]}");
        }
    }
    if (exe is null || !File.Exists(exe) || artifacts is null)
        throw new ArgumentException("--exe and --artifacts are required");
    Directory.CreateDirectory(artifacts);
    ClipboardReceipt.PathName = Path.Combine(artifacts, "clipboard-receipt.json");
    File.Delete(ClipboardReceipt.PathName);
    ClipboardReceipt.Initialize(uint.Parse(Environment.GetEnvironmentVariable("AEGISUB_UIA_CLIPBOARD_START_SEQUENCE") ?? throw new InvalidOperationException("Clipboard baseline is missing"), System.Globalization.CultureInfo.InvariantCulture));
    if (scenario is "language" or "language-tip") return RunLanguage(exe, artifacts, scenario == "language-tip");
    if (scenario == "language-settings-discovery") return RunLanguageSettingsDiscovery(exe, artifacts);
    if (scenario == "language-settings") return RunLanguageSettings(exe, artifacts);
    if (scenario is not "editor" and not "editor-unavailable") throw new ArgumentException($"Unknown scenario: {scenario}");
    var editorUnavailable = scenario == "editor-unavailable";
    var fixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "editor.ass");
    var mutationFixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "metadata-mutations.lua");
    var luaFixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "editor-file.lua");
    var driver = Path.Combine("tests", "gui-automation", "lua-workspace", "editor-uia.cs");
    var clipboardStaSource = Path.Combine("tests", "gui-automation", "driver", "ClipboardSta.cs");
    var startedUtc = DateTimeOffset.UtcNow;
    var exeHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(exe)));
    var fixtureHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(fixture)));
    var mutationFixtureHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(mutationFixture)));
    var luaFixtureHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(luaFixture)));
    var driverHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(driver)));
    var clipboardStaSourceHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(clipboardStaSource)));
    var executedDriverLibraryHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(typeof(UiaDriver).Assembly.Location)));
    var executedDriver = Assembly.GetEntryAssembly()?.Location ?? throw new InvalidOperationException("Executed driver assembly is unavailable");
    var executedDriverHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(executedDriver)));
    DateTimeOffset? finishedUtc = null;
    var exitStatus = "running";
    var original = File.ReadAllText(fixture);
    var input = Path.Combine(artifacts, "input.ass");
    File.Copy(fixture, input, overwrite: true);
    File.Copy(mutationFixture, Path.Combine(artifacts, "metadata-mutations.lua"), overwrite: true);
    var profile = Path.Combine(artifacts, "profile");
    Directory.CreateDirectory(profile);
    var runtimeExe = Path.Combine(Path.GetDirectoryName(exe)!, "runtimes", "LuaLS", "bin", "lua-language-server.exe");
    if (editorUnavailable)
    {
        Ensure(File.Exists(runtimeExe), "The complete default LuaLS release is required for unavailable-path no-fallback coverage");
        var user = Path.Combine(profile, "user");
        Directory.CreateDirectory(user);
        File.WriteAllText(Path.Combine(user, "config.json"), JsonSerializer.Serialize(new
        {
            Automation = new Dictionary<string, object> { ["Lua Workspace"] = new Dictionary<string, object>
            {
                ["Enable LuaLS"] = true, ["LuaLS Directory"] = "?user/missing-luals"
            } }
        }));
        Ensure(!Directory.Exists(Path.Combine(user, "missing-luals")), "Unavailable-path fixture unexpectedly exists");
    }
    DriverTiming.Start(artifacts);
    var results = new List<StepResult>();
    var allSteps = (editorUnavailable ? new[] { "host-ready", "open-workspace", "language-unavailable" } : new[] { "host-ready", "open-workspace" })
        .Concat(new[] { "edit-local-undo-redo", "format-apply-save", "main-undo-redo-isolation", "dirty-close-cancel-discard", "lua-file-save-success", "lua-file-save-failure", "external-text-conflict", "multi-code-switch", "host-close-dirty", "lua-file-external-change", "change-ass-dirty", "target-effect-style-comment", "target-deletion", "invalid-source", "explicit-reload", "file-conflict-branches", "successful-ass-generation" }).ToArray();
    Process? host = null;
    AutomationElement? workspace = null;
    AutomationElement? saveSubtitles = null;
    try
    {
        var start = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe)! };
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", artifacts, "--open", input })
            start.ArgumentList.Add(argument);
        host = Process.Start(start) ?? throw new InvalidOperationException("Could not start GUI host");
        Step("host-ready", () =>
        {
            var ready = AutomationProtocol.WaitForReadyArtifact(Path.Combine(artifacts, "ready.json"), host, TimeSpan.FromSeconds(15));
            Ensure(ready.ProcessId == host.Id, "ready.json belongs to another process");
            _ = UiaDriver.WaitForMainWindow(host, TimeSpan.FromSeconds(15));
        });
        var main = AutomationElement.FromHandle(host.MainWindowHandle);
    SaveNativeDialogEvidence(main, artifacts, "main-before-menu");
        Step("open-workspace", () =>
        {
            SelectCodeViaMacro(main, host, first: true);
            InvokeMenu(main, host, "Automation", "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(12));
            workspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(12));
            SaveNativeDialogEvidence(workspace, artifacts, "opened");
            var editor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("Lua source editor is absent");
            Ensure(ReadEditor(editor).Contains("local total", StringComparison.Ordinal), "Initial code line was not loaded");
        });
        var sourceEditor = FindNamed(workspace!, "Lua source")!;
        if (editorUnavailable)
        {
            Step("language-unavailable", () =>
            {
                SelectLanguageTab(workspace!);
                WaitForLanguageStatus(workspace!, value => value.Contains("unavailable", StringComparison.OrdinalIgnoreCase), TimeSpan.FromSeconds(6),
                    "Missing configured LuaLS directory did not report unavailable");
                Ensure(File.Exists(runtimeExe), "Default LuaLS release disappeared during no-fallback coverage");
                Ensure(!NativeProcess.DirectChildExecutablePaths(host!.Id).Any(path => PathsEqual(path, runtimeExe)),
                    "Missing configured LuaLS directory silently fell back to the default release");
                File.WriteAllText(Path.Combine(artifacts, "language-unavailable-status.txt"), LanguageStatus(workspace!), new UTF8Encoding(false));
            });
        }
        string? formattedSource = null;
        const string edited = "local values = {2, 3, 5}\nlocal total = 0\nfor _, value in ipairs(values) do\n  total = total + value * value\nend\nreturn tostring(total)";
        File.WriteAllText(Path.Combine(artifacts, "edited.lua"), edited, new UTF8Encoding(false));
        Step("edit-local-undo-redo", () =>
        {
            var prior = ReadEditor(sourceEditor);
            WriteEditor(sourceEditor, edited, host);
            WaitUntil(() => NormalizeSource(ReadEditor(sourceEditor)) == edited, TimeSpan.FromSeconds(5), "Editor did not accept the complete multiline source");
            GuardedKeyboard.FocusEditor(sourceEditor, host);
            GuardedKeyboard.SendChord(sourceEditor, host, 'Z');
            WaitUntil(() => ReadEditor(sourceEditor) == prior, TimeSpan.FromSeconds(5), "Workspace-local Undo did not restore prior source");
            GuardedKeyboard.FocusEditor(sourceEditor, host);
            GuardedKeyboard.SendChord(sourceEditor, host, 'Y');
            WaitUntil(() => NormalizeSource(ReadEditor(sourceEditor)) == edited, TimeSpan.FromSeconds(5), "Workspace-local Redo did not restore the complete edited source");
        });
        Step("format-apply-save", () =>
        {
            InvokeButton(workspace!, "Format");
            var formatted = ReadEditor(sourceEditor);
            File.WriteAllText(Path.Combine(artifacts, "formatted-observed.lua"), formatted, new UTF8Encoding(false));
            Ensure(formatted.Contains("value * value", StringComparison.Ordinal), "Format lost expression content");
            formattedSource = formatted;
            File.WriteAllText(Path.Combine(artifacts, "formatted.lua"), formatted, new UTF8Encoding(false));
            InvokeButton(workspace!, "Apply");
            saveSubtitles = UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles")
                ?? throw new InvalidOperationException("Main-window Save Subtitles toolbar button is unavailable");
            UiaDriver.Invoke(saveSubtitles);
            WaitUntil(() => File.ReadAllText(input) != original, TimeSpan.FromSeconds(8), "Saved ASS did not change");
            var changed = File.ReadAllText(input);
            File.WriteAllText(Path.Combine(artifacts, "output.ass"), changed);
            AssertPhysicalEventLines(changed, 3);
            var beforeLines = EventLines(original);
            var afterLines = EventLines(changed);
            Ensure(beforeLines.Count == 3 && afterLines.Count == 3, "ASS event count changed");
            Ensure(afterLines[0] with { Text = beforeLines[0].Text } == beforeLines[0], "Bound code line changed fields other than Text");
            Ensure(afterLines[0].Text.Contains("value", StringComparison.Ordinal), "Applied code expression is absent from its ASS event");
            Ensure(beforeLines[1] == afterLines[1] && beforeLines[2] == afterLines[2], "Unbound subtitles changed");
            SaveNativeDialogEvidence(workspace!, artifacts, "applied");
        });
        Step("main-undo-redo-isolation", () =>
        {
            InvokeMenu(main, host, "Edit", "Undo", TimeSpan.FromSeconds(6));
            InvokeKnownButton(saveSubtitles!, "Save the current subtitles");
            WaitUntil(() => EventLines(File.ReadAllText(input))[0].Text == EventLines(original)[0].Text, TimeSpan.FromSeconds(6), "Main Undo did not restore the original code line");
            Ensure(ReadEditor(sourceEditor) == formattedSource, "Main Undo changed the Workspace editor buffer");
            InvokeMenu(main, host, "Edit", "Redo", TimeSpan.FromSeconds(6));
            InvokeKnownButton(saveSubtitles!, "Save the current subtitles");
            WaitUntil(() => EventLines(File.ReadAllText(input))[0].Text.Contains("value", StringComparison.Ordinal), TimeSpan.FromSeconds(6), "Main Redo did not restore the applied source");
            var restored = EventLines(File.ReadAllText(input));
            var baseline = EventLines(original);
            Ensure(restored[1] == baseline[1] && restored[2] == baseline[2], "Main Undo/Redo changed unbound lines");
            Ensure(ReadEditor(sourceEditor) == formattedSource, "Main Redo changed the Workspace editor buffer");
            File.Copy(input, Path.Combine(artifacts, "applied.ass"), overwrite: true);
            File.Copy(input, Path.Combine(artifacts, "output.ass"), overwrite: true);
        });
        Step("dirty-close-cancel-discard", () =>
        {
            WriteEditor(sourceEditor, "return 'unsaved workspace source'", host);
            RequestClose(workspace!);
            var prompt = WaitWindow(host, "Unsaved Lua source", TimeSpan.FromSeconds(5));
            InvokeButton(prompt, "Cancel");
            Ensure(WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(5)) is not null, "Cancel closed the workspace");
            Ensure(ReadEditor(sourceEditor).Contains("unsaved workspace", StringComparison.Ordinal), "Cancel lost dirty source");
            RequestClose(workspace!);
            prompt = WaitWindow(host, "Unsaved Lua source", TimeSpan.FromSeconds(5));
            InvokeButton(prompt, "Discard");
            WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(5), "Discard did not close the workspace");
        });
        var luaFile = Path.Combine(artifacts, "editor-file.lua");
        File.Copy(luaFixture, luaFile, overwrite: true);
        const string savedFileSource = "local greeting = 'saved from workspace'\nreturn greeting";
        Step("lua-file-save-success", () =>
        {
            InvokeMenu(main, host, "Automation", "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(6));
            workspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(6));
            OpenLuaFile(workspace, host, luaFile, artifacts);
            sourceEditor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("Lua-file editor is absent");
            Ensure(ReadEditor(sourceEditor).Contains("message(name)", StringComparison.Ordinal), "Lua file did not open");
            WriteEditor(sourceEditor, savedFileSource, host);
            InvokeButton(workspace, "Save");
            WaitUntil(() => File.ReadAllText(luaFile) == savedFileSource, TimeSpan.FromSeconds(5), "Lua file Save did not persist the edited source");
            Ensure(HasUtf8Bom(luaFile), "Lua-file Save dropped its original UTF-8 BOM");
        });
        Step("lua-file-save-failure", () =>
        {
            var savedAttributes = File.GetAttributes(luaFile);
            try
            {
                File.SetAttributes(luaFile, savedAttributes | FileAttributes.ReadOnly);
                WriteEditor(sourceEditor, "return 'read-only file edit must survive'", host);
                InvokeButton(workspace!, "Save");
                Ensure(File.ReadAllText(luaFile) == savedFileSource, "Read-only Lua file was unexpectedly overwritten");
                Ensure(HasUtf8Bom(luaFile), "Failed Lua-file Save changed its BOM");
                Ensure(ReadEditor(sourceEditor) == "return 'read-only file edit must survive'", "Failed Lua file Save lost the editor buffer");
            }
            finally { File.SetAttributes(luaFile, savedAttributes); }
            RequestClose(workspace!);
            var prompt = WaitWindow(host, "Unsaved Lua source", TimeSpan.FromSeconds(5));
            InvokeButton(prompt, "Discard");
            WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(5), "Failed Lua file close did not discard dirty buffer");
        });
        Step("external-text-conflict", () =>
        {
            InvokeMenu(main, host, "Automation", "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(8));
            workspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(8));
            sourceEditor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("Reopened source editor is absent");
            WriteEditor(sourceEditor, "return 'workspace conflict source'", host);
            var subsEditBox = FindNamed(main, "SubsEditBox") ?? throw new InvalidOperationException("Main SubsEditBox is not exposed");
            var mainEdit = FindNamed(subsEditBox, "stcwindow") ?? throw new InvalidOperationException("Main subtitle STC is not exposed");
            WriteEditor(mainEdit, "return 'external subtitle change'", host);
            InvokeKnownButton(saveSubtitles!, "Save the current subtitles");
            WaitUntil(() => EventLines(File.ReadAllText(input))[0].Text.Contains("external subtitle change", StringComparison.Ordinal), TimeSpan.FromSeconds(5), "External subtitle edit was not committed to ASS");
            var applyTask = Task.Run(() => InvokeButton(workspace, "Apply"));
            var conflict = WaitWindow(host, "Lua source conflict", TimeSpan.FromSeconds(5));
            SaveNativeDialogEvidence(conflict, artifacts, "conflict");
            InvokeButton(conflict, "Cancel");
            if (!applyTask.Wait(TimeSpan.FromSeconds(5))) throw new TimeoutException("Apply did not resume after conflict dismissal");
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == "return 'workspace conflict source'", "Conflict lost the editor buffer");
        });
        Step("multi-code-switch", () =>
        {
            RequestClose(workspace!);
            InvokeButton(WaitWindow(host, "Unsaved Lua source", TimeSpan.FromSeconds(5)), "Discard");
            WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(5), "Conflict source could not be discarded before switching");
            SelectCodeViaMacro(main, host, first: true);
            InvokeMenu(main, host, "Automation", "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(6));
            workspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(6));
            sourceEditor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("Switch source editor is absent");
            WriteEditor(sourceEditor, "return 'switch cancel retained'", host);
            SelectCodeViaMacro(main, host, first: false);
            SwitchCodeWithDecision(main, host, "Cancel");
            Ensure(ReadEditor(sourceEditor) == "return 'switch cancel retained'", "Cancel lost the original code buffer");
            SwitchCodeWithDecision(main, host, "Discard");
            sourceEditor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("Second code editor is absent");
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == "return \"other code must remain\"", "Discard did not open the selected second code line");
            WriteEditor(sourceEditor, "return 'second line saved by switch'", host);
            SelectCodeViaMacro(main, host, first: true);
            SwitchCodeWithDecision(main, host, "Apply");
            sourceEditor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("First code editor is absent after Apply switch");
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == "return 'external subtitle change'", "Apply switch did not open first code line");
            InvokeKnownButton(saveSubtitles!, "Save the current subtitles");
            WaitUntil(() => EventLines(File.ReadAllText(input))[1].Text.Contains("second line saved by switch", StringComparison.Ordinal), TimeSpan.FromSeconds(5), "Second code Save was not persisted");
            var switched = EventLines(File.ReadAllText(input));
            Ensure(switched.Count == 3 && switched[1].Text.Contains("second line saved by switch", StringComparison.Ordinal), "Second code line was not saved during switch");
            Ensure(switched[2] == EventLines(original)[2], "Ordinary subtitle changed during code-line switching");
            File.Copy(input, Path.Combine(artifacts, "output.ass"), overwrite: true);
        });
        Step("host-close-dirty", () =>
        {
            WriteEditor(sourceEditor, "return 'ass dirty before main close'", host);
            InvokeButton(workspace!, "Apply");
            WriteEditor(sourceEditor, "return 'workspace dirty before main close'", host);
            RequestClose(main);
            InvokeButton(WaitWindow(host, "Unsaved Lua source", TimeSpan.FromSeconds(5)), "Discard");
            var unsavedAss = WaitWindow(host, "Unsaved changes", TimeSpan.FromSeconds(5));
            SaveNativeDialogEvidence(unsavedAss, artifacts, "main-close-unsaved-ass");
            InvokeTaskDialogButton(unsavedAss, "取消", "Cancel");
            workspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(5));
            sourceEditor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("Workspace vanished after cancelling main close");
            Ensure(ReadEditor(sourceEditor) == "return 'workspace dirty before main close'", "Main close cancellation lost dirty source");
            GuardedKeyboard.FocusEditor(sourceEditor, host);
            GuardedKeyboard.SendChord(sourceEditor, host, 'Z');
            Ensure(ReadEditor(sourceEditor) == "return 'ass dirty before main close'", "Main close cancellation lost Workspace Undo history");
            GuardedKeyboard.FocusEditor(sourceEditor, host);
            GuardedKeyboard.SendChord(sourceEditor, host, 'Y');
            Ensure(ReadEditor(sourceEditor) == "return 'workspace dirty before main close'", "Workspace Redo did not restore dirty source");
            RequestClose(workspace);
            InvokeButton(WaitWindow(host, "Unsaved Lua source", TimeSpan.FromSeconds(5)), "Discard");
            WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(5), "Workspace did not close after main-close coverage");
            InvokeKnownButton(saveSubtitles!, "Save the current subtitles");
            WaitUntil(() => EventLines(File.ReadAllText(input))[0].Text.Contains("ass dirty before main close", StringComparison.Ordinal), TimeSpan.FromSeconds(5), "Main-close ASS state was not persisted");
        });
        Step("lua-file-external-change", () =>
        {
            SelectCodeViaMacro(main, host, first: true);
            InvokeMenu(main, host, "Automation", "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(6));
            workspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(6));
            OpenLuaFile(workspace, host, luaFile, artifacts);
            sourceEditor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("External-file editor is absent");
            WriteEditor(sourceEditor, "return 'unsaved source survives external file change'", host);
            var externalBytes = new byte[] { 0xEF, 0xBB, 0xBF }.Concat(Encoding.UTF8.GetBytes("return 'external file content'\n")).ToArray();
            File.WriteAllBytes(luaFile, externalBytes);
            var saveTask = Task.Run(() => InvokeButton(workspace, "Save"));
            var conflict = WaitWindow(host, "Lua source conflict", TimeSpan.FromSeconds(5));
            SaveNativeDialogEvidence(conflict, artifacts, "file-conflict");
            InvokeButton(conflict, "Cancel");
            if (!saveTask.Wait(TimeSpan.FromSeconds(5))) throw new TimeoutException("External-file conflict did not resume");
            Ensure(ReadEditor(sourceEditor) == "return 'unsaved source survives external file change'", "External-file conflict lost the editor source");
            Ensure(File.ReadAllBytes(luaFile).SequenceEqual(externalBytes), "External-file conflict overwrote the changed file or its BOM");
            RequestClose(workspace);
            InvokeButton(WaitWindow(host, "Unsaved Lua source", TimeSpan.FromSeconds(5)), "Discard");
            WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(5), "External-file conflict workspace did not close");
        });
        Step("change-ass-dirty", () =>
        {
            SelectCodeViaMacro(main, host, first: true);
            InvokeMenu(main, host, "Automation", "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(6));
            workspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(6));
            sourceEditor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("ASS-change editor is absent");
            var cleanSource = NormalizeSource(ReadEditor(sourceEditor));
            WriteEditor(sourceEditor, "return 'picker cancellation must retain me'", host);
            OpenAssWithDirtyDecision(main, host, artifacts, null);
            Ensure(ReadEditor(sourceEditor) == "return 'picker cancellation must retain me'", "Cancelled ASS picker lost the dirty Workspace source");
            GuardedKeyboard.FocusEditor(sourceEditor, host);
            GuardedKeyboard.SendChord(sourceEditor, host, 'Z');
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == cleanSource, "Cancelled ASS picker lost Workspace Undo history");
            GuardedKeyboard.FocusEditor(sourceEditor, host);
            GuardedKeyboard.SendChord(sourceEditor, host, 'Y');
            Ensure(ReadEditor(sourceEditor) == "return 'picker cancellation must retain me'", "Cancelled ASS picker lost Workspace Redo history");
            var currentAss = File.ReadAllText(input);
            var first = currentAss.IndexOf("Comment: 0,", StringComparison.Ordinal);
            if (first < 0) throw new FormatException("Fixture ASS has no expected first Comment event");
            var malformed = currentAss[..first] + "Comment: invalid-layer," + currentAss[(first + "Comment: 0,".Length)..];
            var malformedPath = Path.Combine(artifacts, "invalid-layer.ass");
            File.WriteAllText(malformedPath, malformed, new UTF8Encoding(false));
            OpenAssWithDirtyDecision(main, host, artifacts, malformedPath, expectLoadError: true);
            Ensure(ReadEditor(sourceEditor) == "return 'picker cancellation must retain me'", "Failed ASS load lost the dirty Workspace source");
            Ensure((main.Current.Name ?? "").Contains("input.ass", StringComparison.OrdinalIgnoreCase), "Failed ASS load replaced the active subtitle document");
            GuardedKeyboard.FocusEditor(sourceEditor, host);
            GuardedKeyboard.SendChord(sourceEditor, host, 'Z');
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == cleanSource, "Failed ASS load lost Workspace Undo history");
            GuardedKeyboard.FocusEditor(sourceEditor, host);
            GuardedKeyboard.SendChord(sourceEditor, host, 'Y');
            Ensure(ReadEditor(sourceEditor) == "return 'picker cancellation must retain me'", "Failed ASS load lost Workspace Redo history");
            RequestClose(workspace);
            InvokeButton(WaitWindow(host, "Unsaved Lua source", TimeSpan.FromSeconds(5)), "Discard");
            WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(5), "ASS-change workspace did not close");
        });
        Step("target-effect-style-comment", () =>
        {
            var baseline = EventLines(File.ReadAllText(input));
            foreach (var mutation in new[] { (Name: "Workspace E2E Change Effect", Field: "effect"), (Name: "Workspace E2E Change Style", Field: "style") })
            {
                workspace = OpenFirstCodeWorkspace(main, host);
                sourceEditor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("Metadata editor is absent");
                WriteEditor(sourceEditor, "return 'metadata buffer retained'", host);
                InvokeMenu(main, host, "Automation", mutation.Name, TimeSpan.FromSeconds(6));
                WaitUntil(() => workspace.Current.Name.Contains("[conflict]", StringComparison.Ordinal), TimeSpan.FromSeconds(5), "Metadata mutation did not put Workspace in conflict state");
                var applyTask = Task.Run(() => InvokeButton(workspace, "Apply"));
                var conflict = WaitWindow(host, "Lua source conflict", TimeSpan.FromSeconds(5));
                SaveNativeDialogEvidence(conflict, artifacts, "metadata-" + mutation.Field + "-conflict");
                InvokeButton(conflict, "Cancel");
                if (!applyTask.Wait(TimeSpan.FromSeconds(5))) throw new TimeoutException("Metadata conflict did not resume");
                Ensure(ReadEditor(sourceEditor) == "return 'metadata buffer retained'", "Metadata conflict lost editor source");
                DiscardAndCloseWorkspace(workspace, host);
                InvokeKnownButton(saveSubtitles!, "Save the current subtitles");
                WaitUntil(() => mutation.Field == "effect"
                    ? EventLines(File.ReadAllText(input))[0].Effect == "code line"
                    : EventLines(File.ReadAllText(input))[0].Style == "Alt", TimeSpan.FromSeconds(5), "Metadata mutation was not persisted");
                var changed = EventLines(File.ReadAllText(input));
                Ensure(changed.Count == baseline.Count && changed[1] == baseline[1] && changed[2] == baseline[2], "Metadata macro changed unrelated events");
                Ensure(mutation.Field == "effect"
                    ? changed[0].Effect == "code line" && changed[0] with { Effect = baseline[0].Effect } == baseline[0]
                    : changed[0].Style == "Alt" && changed[0] with { Style = baseline[0].Style } == baseline[0], "Metadata macro changed unexpected fields");
                InvokeMenu(main, host, "Edit", "Undo", TimeSpan.FromSeconds(6));
                InvokeKnownButton(saveSubtitles!, "Save the current subtitles");
                WaitUntil(() => EventLines(File.ReadAllText(input)).SequenceEqual(baseline), TimeSpan.FromSeconds(5), "Metadata Undo was not persisted");
                Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), "Undo did not restore the metadata baseline");
            }
            workspace = OpenFirstCodeWorkspace(main, host);
            sourceEditor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("Comment-invalidated editor is absent");
            WriteEditor(sourceEditor, "return 'comment invalidation retained'", host);
            InvokeMenu(main, host, "Automation", "Workspace E2E Toggle Comment", TimeSpan.FromSeconds(6));
            WaitUntil(() => workspace.Current.Name.Contains("[target unavailable]", StringComparison.Ordinal), TimeSpan.FromSeconds(5), "Comment mutation did not invalidate the target");
            var apply = FindNamed(workspace, "Apply") ?? throw new InvalidOperationException("Apply button disappeared after Comment invalidation");
            Ensure(!apply.Current.IsEnabled, "Invalidated Comment target still offers Apply/Overwrite");
            Ensure(ReadEditor(sourceEditor) == "return 'comment invalidation retained'", "Comment invalidation lost editor source");
            DiscardAndCloseWorkspace(workspace, host);
            InvokeKnownButton(saveSubtitles!, "Save the current subtitles");
            WaitUntil(() => EventLines(File.ReadAllText(input))[0].Kind == "Dialogue", TimeSpan.FromSeconds(5), "Comment mutation was not persisted");
            var uncommented = EventLines(File.ReadAllText(input));
            Ensure(uncommented[0].Kind == "Dialogue" && uncommented[0] with { Kind = "Comment" } == baseline[0], "Toggle Comment changed more than the target's comment state");
            Ensure(uncommented[1] == baseline[1] && uncommented[2] == baseline[2], "Toggle Comment changed unrelated events");
            InvokeMenu(main, host, "Edit", "Undo", TimeSpan.FromSeconds(6));
            InvokeKnownButton(saveSubtitles!, "Save the current subtitles");
            WaitUntil(() => EventLines(File.ReadAllText(input)).SequenceEqual(baseline), TimeSpan.FromSeconds(5), "Comment Undo was not persisted");
            Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), "Undo did not restore Comment baseline");
        });
        Step("target-deletion", () =>
        {
            var baseline = EventLines(File.ReadAllText(input));
            workspace = OpenFirstCodeWorkspace(main, host);
            sourceEditor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("Deletion editor is absent");
            WriteEditor(sourceEditor, "return 'deleted target buffer retained'", host);
            InvokeMenu(main, host, "Automation", "Workspace E2E Delete Target", TimeSpan.FromSeconds(6));
            WaitUntil(() => workspace.Current.Name.Contains("[target unavailable]", StringComparison.Ordinal), TimeSpan.FromSeconds(5), "Deletion did not invalidate the target");
            var apply = FindNamed(workspace, "Apply") ?? throw new InvalidOperationException("Apply button disappeared after deletion");
            Ensure(!apply.Current.IsEnabled, "Deleted target still offers Apply/Overwrite");
            Ensure(ReadEditor(sourceEditor) == "return 'deleted target buffer retained'", "Deleted target lost editor source");
            InvokeKnownButton(saveSubtitles!, "Save the current subtitles");
            WaitUntil(() => EventLines(File.ReadAllText(input)).Count == 2, TimeSpan.FromSeconds(5), "Deletion was not persisted");
            var deleted = EventLines(File.ReadAllText(input));
            Ensure(deleted.Count == 2 && deleted[0] == baseline[1] && deleted[1] == baseline[2], "Delete Target changed surviving events");
            InvokeMenu(main, host, "Edit", "Undo", TimeSpan.FromSeconds(6));
            InvokeKnownButton(saveSubtitles!, "Save the current subtitles");
            WaitUntil(() => EventLines(File.ReadAllText(input)).SequenceEqual(baseline), TimeSpan.FromSeconds(5), "Deletion Undo was not persisted");
            Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), "Undo did not restore the deleted target and other events");
            Ensure(ReadEditor(sourceEditor) == "return 'deleted target buffer retained'", "Delete Undo lost editor source");
            DiscardAndCloseWorkspace(workspace, host);
        });
        Step("invalid-source", () =>
        {
            var unchangedAss = File.ReadAllBytes(input);
            var unchangedEvents = EventLines(File.ReadAllText(input));
            workspace = OpenFirstCodeWorkspace(main, host);
            sourceEditor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("Invalid-source editor is absent");
            var cleanSource = NormalizeSource(ReadEditor(sourceEditor));
            const string invalid = "local =";
            WriteEditor(sourceEditor, invalid, host);
            InvokeButton(workspace, "Format");
            SaveNativeDialogEvidence(workspace, artifacts, "invalid-format");
            WaitUntil(() => HasVisibleLuaDiagnostic(workspace), TimeSpan.FromSeconds(3), "Failed Format did not show a Lua diagnostic");
            Ensure(workspace.Current.Name.Contains(" *", StringComparison.Ordinal), "Failed Format did not retain dirty state");
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == invalid, "Failed Format changed invalid source");
            InvokeButton(workspace, "Apply");
            SaveNativeDialogEvidence(workspace, artifacts, "invalid-apply");
            WaitUntil(() => HasVisibleLuaDiagnostic(workspace), TimeSpan.FromSeconds(3), "Failed Apply did not show a Lua diagnostic");
            Ensure(workspace.Current.Name.Contains(" *", StringComparison.Ordinal), "Failed Apply did not retain dirty state");
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == invalid, "Failed Apply changed invalid source");
            Ensure(File.ReadAllBytes(input).SequenceEqual(unchangedAss), "Invalid Lua source altered the physical ASS file");
            Ensure(unchangedEvents.Count == 3 && unchangedEvents[0].Style == "Default", "Invalid-source probe has no expected Default-style code baseline");
            InvokeMenu(main, host, "Automation", "Workspace E2E Change Style", TimeSpan.FromSeconds(6));
            WaitUntil(() => workspace.Current.Name.Contains("[conflict]", StringComparison.Ordinal), TimeSpan.FromSeconds(5), "Controlled Style mutation did not reach Workspace conflict state");
            WaitUntil(() => saveSubtitles!.Current.IsEnabled,
                TimeSpan.FromSeconds(5), "Main Save did not become available after controlled Style mutation");
            SaveNativeDialogEvidence(main, artifacts, "invalid-style-before-save-main");
            SaveNativeDialogEvidence(workspace, artifacts, "invalid-style-before-save-workspace");
            InvokeKnownButton(saveSubtitles!, "Save the current subtitles");
            var styleProbe = unchangedEvents.ToArray();
            styleProbe[0] = styleProbe[0] with { Style = "Alt" };
            WaitUntil(() => EventLines(File.ReadAllText(input)).SequenceEqual(styleProbe), TimeSpan.FromSeconds(5), "Forced Save did not preserve all event fields except the controlled Style mutation");
            File.Copy(input, Path.Combine(artifacts, "invalid-source-style-probe.ass"), overwrite: true);
            InvokeMenu(main, host, "Edit", "Undo", TimeSpan.FromSeconds(6));
            WaitUntil(() => !workspace.Current.Name.Contains("[conflict]", StringComparison.Ordinal), TimeSpan.FromSeconds(5), "Style Undo did not restore the Workspace target state");
            WaitUntil(() => saveSubtitles!.Current.IsEnabled,
                TimeSpan.FromSeconds(5), "Main Save did not become available after Style Undo");
            SaveNativeDialogEvidence(main, artifacts, "invalid-style-undo-before-save-main");
            SaveNativeDialogEvidence(workspace, artifacts, "invalid-style-undo-before-save-workspace");
            InvokeKnownButton(saveSubtitles!, "Save the current subtitles");
            WaitUntil(() => EventLines(File.ReadAllText(input)).SequenceEqual(unchangedEvents), TimeSpan.FromSeconds(5), "Style-probe Undo did not restore the exact event baseline");
            File.Copy(input, Path.Combine(artifacts, "invalid-source-restored.ass"), overwrite: true);
            GuardedKeyboard.FocusEditor(sourceEditor, host);
            GuardedKeyboard.SendChord(sourceEditor, host, 'Z');
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == cleanSource, "Invalid-source Undo did not restore the valid coding baseline");
            RequestClose(workspace);
            WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(5), "Invalid-source Workspace did not close after Undo");
        });
        Step("explicit-reload", () =>
        {
            workspace = OpenFirstCodeWorkspace(main, host);
            sourceEditor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("Reload editor is absent");
            var cleanSource = NormalizeSource(ReadEditor(sourceEditor));
            const string dirty = "return 'reload decision buffer'";
            WriteEditor(sourceEditor, dirty, host);
            var reloadTask = Task.Run(() => InvokeButton(workspace, "Reload"));
            var prompt = WaitWindow(host, "Reload Lua source", TimeSpan.FromSeconds(5));
            SaveNativeDialogEvidence(prompt, artifacts, "reload-reject");
            InvokeTaskDialogButton(prompt, "否(N)", "No");
            if (!reloadTask.Wait(TimeSpan.FromSeconds(5))) throw new TimeoutException("Rejected Reload did not resume");
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == dirty, "Rejected Reload lost dirty source");
            GuardedKeyboard.FocusEditor(sourceEditor, host);
            GuardedKeyboard.SendChord(sourceEditor, host, 'Z');
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == cleanSource, "Rejected Reload lost Undo history");
            GuardedKeyboard.FocusEditor(sourceEditor, host);
            GuardedKeyboard.SendChord(sourceEditor, host, 'Y');
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == dirty, "Rejected Reload lost Redo history");
            reloadTask = Task.Run(() => InvokeButton(workspace, "Reload"));
            prompt = WaitWindow(host, "Reload Lua source", TimeSpan.FromSeconds(5));
            SaveNativeDialogEvidence(prompt, artifacts, "reload-confirm");
            InvokeTaskDialogButton(prompt, "是(Y)", "Yes");
            if (!reloadTask.Wait(TimeSpan.FromSeconds(5))) throw new TimeoutException("Confirmed Reload did not resume");
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == cleanSource, "Confirmed Reload did not reload current target");
            Ensure(!workspace.Current.Name.Contains(" *", StringComparison.Ordinal), "Confirmed Reload left Workspace dirty");
            RequestClose(workspace);
            WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(5), "Reload Workspace did not close cleanly");
        });
        Step("file-conflict-branches", () =>
        {
            workspace = OpenFirstCodeWorkspace(main, host);
            OpenLuaFile(workspace, host, luaFile, artifacts);
            sourceEditor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("File-conflict editor is absent");
            const string copiedSource = "return 'copy branch editor source'";
            WriteEditor(sourceEditor, copiedSource, host);
            var externalCopy = Utf8Bom("return 'copy branch external'");
            File.WriteAllBytes(luaFile, externalCopy);
            ChooseFileConflict(workspace, host, artifacts, "file-conflict-copy", "Copy editor source and keep editing", expectedCopied: copiedSource);
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == copiedSource, "Copy conflict branch lost the dirty editor source");
            Ensure(File.ReadAllBytes(luaFile).SequenceEqual(externalCopy), "Copy conflict branch changed the external file");
            ChooseFileConflict(workspace, host, artifacts, "file-conflict-keep", "Keep editing without saving");
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == copiedSource, "Keep-editing conflict branch lost source");
            Ensure(File.ReadAllBytes(luaFile).SequenceEqual(externalCopy), "Keep-editing conflict branch changed the external file");
            ChooseFileConflict(workspace, host, artifacts, "file-conflict-overwrite", "Overwrite this target with editor source");
            WaitUntil(() => File.ReadAllText(luaFile) == copiedSource, TimeSpan.FromSeconds(5), "Overwrite conflict branch did not save the editor source");
            Ensure(File.ReadAllBytes(luaFile).SequenceEqual(Utf8Bom(copiedSource)), "Overwrite conflict branch did not preserve exact UTF-8 BOM and source");
            Ensure(!workspace.Current.Name.Contains(" *", StringComparison.Ordinal), "Overwrite conflict branch left the editor dirty");
            const string reloadEditor = "return 'reload branch editor source'";
            const string reloadExternal = "return 'reload branch external source'";
            WriteEditor(sourceEditor, reloadEditor, host);
            var externalReload = Utf8Bom(reloadExternal);
            File.WriteAllBytes(luaFile, externalReload);
            ChooseFileConflict(workspace, host, artifacts, "file-conflict-reload", "Reload target and discard editor changes", confirmReload: true);
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == reloadExternal, "Reload conflict branch did not read the external source");
            Ensure(File.ReadAllBytes(luaFile).SequenceEqual(externalReload), "Reload conflict branch changed the external file");
            Ensure(!workspace.Current.Name.Contains(" *", StringComparison.Ordinal), "Reload conflict branch left the editor dirty");
            const string raceSource = "return 'race editor source must survive'";
            WriteEditor(sourceEditor, raceSource, host);
            File.WriteAllBytes(luaFile, Utf8Bom("return 'first external revision'"));
            var latestExternal = Utf8Bom("return 'second external revision'");
            ChooseFileConflict(workspace, host, artifacts, "file-conflict-race", "Overwrite this target with editor source", beforeConfirm: () => File.WriteAllBytes(luaFile, latestExternal));
            SaveNativeDialogEvidence(workspace, artifacts, "file-conflict-race-result");
            Ensure(File.ReadAllBytes(luaFile).SequenceEqual(latestExternal), "Second external revision was overwritten after conflict confirmation");
            Ensure(workspace.Current.Name.Contains(" *", StringComparison.Ordinal), "Second-change conflict no longer marks the source dirty");
            WaitUntil(() => HasVisibleDiagnostic(workspace, "changed again during confirmation"), TimeSpan.FromSeconds(3), "Second-change conflict diagnostic was not visible");
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == raceSource, "Second-change conflict lost the dirty editor source");
            DiscardAndCloseWorkspace(workspace, host);
        });
        Step("successful-ass-generation", () =>
        {
            var alternate = original.Replace("Title: Lua Workspace editor E2E", "Title: Alternate Lua Workspace document", StringComparison.Ordinal)
                .Replace("local total = 0; for i = 1,4 do total = total + i end; return tostring(total)", "return 'alternate generation source'", StringComparison.Ordinal);
            Ensure(alternate != original && alternate.Contains("alternate generation source", StringComparison.Ordinal), "Alternate ASS fixture was not constructed");
            var alternatePath = Path.Combine(artifacts, "alternate.ass");
            File.WriteAllText(alternatePath, alternate, new UTF8Encoding(false));
            var oldAssBytes = File.ReadAllBytes(input);
            var alternateBytes = File.ReadAllBytes(alternatePath);
            workspace = OpenFirstCodeWorkspace(main, host);
            sourceEditor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("Generation-bound editor is absent");
            var cleanSource = NormalizeSource(ReadEditor(sourceEditor));
            WriteEditor(sourceEditor, "return 'old generation unapplied source'", host);
            OpenAssWithDirtyDecision(main, host, artifacts, alternatePath);
            WaitUntil(() => (main.Current.Name ?? "").Contains("alternate.ass", StringComparison.OrdinalIgnoreCase), TimeSpan.FromSeconds(6), "Valid alternate ASS did not replace the active document");
            workspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(5));
            WaitUntil(() => workspace.Current.Name.Contains("[target unavailable]", StringComparison.Ordinal), TimeSpan.FromSeconds(5), "Old-generation binding did not invalidate");
            var apply = FindNamed(workspace, "Apply") ?? throw new InvalidOperationException("Apply button vanished after ASS generation change");
            Ensure(!apply.Current.IsEnabled, "Old-generation source can still Apply to the new document");
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == cleanSource, "Successful ASS replacement did not commit the user's Discard decision");
            Ensure(!workspace.Current.Name.Contains(" *", StringComparison.Ordinal), "Successful Discard left the old Workspace source dirty");
            Ensure(File.ReadAllBytes(input).SequenceEqual(oldAssBytes) && File.ReadAllBytes(alternatePath).SequenceEqual(alternateBytes), "Unapplied old source was written to a subtitle document");
            SaveNativeDialogEvidence(workspace, artifacts, "generation-invalidated");
        });
        foreach (var step in allSteps.Where(step => results.All(result => result.Name != step)))
            results.Add(new StepResult(step, "not-run", "Scenario is not yet implemented in this driver version"));
        exitStatus = results.All(result => result.Status == "passed") ? "passed" : "incomplete";
        finishedUtc = DateTimeOffset.UtcNow;
        WriteManifest();
        return exitStatus == "passed" ? 0 : 1;
    }
    catch (Exception error)
    {
        var failing = allSteps.FirstOrDefault(step => results.All(result => result.Name != step));
        if (failing is not null) results.Add(new StepResult(failing, "failed", error.Message));
        foreach (var step in allSteps.Where(step => results.All(result => result.Name != step)))
            results.Add(new StepResult(step, "not-run", "A preceding step failed"));
        exitStatus = "failed";
        finishedUtc = DateTimeOffset.UtcNow;
        WriteManifest();
        Console.Error.WriteLine(error);
        return 1;
    }
    finally
    {
        if (workspace is not null) TryEvidence(workspace, artifacts, "final");
        Exception? stabilizationError = null;
        if (host is not null && !host.HasExited)
        {
            try { GuardedKeyboard.StabilizeOwnedClipboard(host.Id); }
            catch (Exception error) { stabilizationError = error; }
            host.Kill(entireProcessTree: true);
            if (!host.WaitForExit(TimeSpan.FromSeconds(5)))
                throw new TimeoutException("GUI host did not stop within five seconds");
        }
        host?.Dispose();
        if (stabilizationError is not null)
            throw new InvalidOperationException("Verified GUI-host clipboard text could not be persisted before host shutdown", stabilizationError);
    }

    void Step(string name, Action action)
    {
        using var timing = DriverTiming.Measure("step:" + name);
        action();
        results.Add(new StepResult(name, "passed", ""));
        WriteManifest();
    }
    void WriteManifest() => File.WriteAllText(Path.Combine(artifacts, "manifest.json"), JsonSerializer.Serialize(new
    {
        Scenario = editorUnavailable ? "editor-unavailable" : "editor",
        StartedUtc = startedUtc,
        FinishedUtc = finishedUtc,
        BudgetSeconds = editorUnavailable ? 150 : 120,
        ExeSha256 = exeHash,
        Fixture = fixture.Replace('\\', '/'),
        FixtureSha256 = fixtureHash,
        MutationFixture = mutationFixture.Replace('\\', '/'),
        MutationFixtureSha256 = mutationFixtureHash,
        LuaFixture = luaFixture.Replace('\\', '/'),
        LuaFixtureSha256 = luaFixtureHash,
        DriverSha256 = driverHash,
        ClipboardStaSourceSha256 = clipboardStaSourceHash,
        ExecutedDriverLibrarySha256 = executedDriverLibraryHash,
        ExecutedDriverSha256 = executedDriverHash,
        ExitStatus = exitStatus,
        Steps = results
    }, new JsonSerializerOptions { WriteIndented = true }));
}

static int RunLanguageSettingsDiscovery(string exe, string artifacts)
{
    var fixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "editor.ass");
    var mutationFixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "metadata-mutations.lua");
    var input = Path.Combine(artifacts, "input.ass");
    var profile = Path.Combine(artifacts, "profile");
    File.Copy(fixture, input, overwrite: true);
    File.Copy(mutationFixture, Path.Combine(artifacts, "metadata-mutations.lua"), overwrite: true);
    Directory.CreateDirectory(profile);
    DriverTiming.Start(artifacts);
    var startedUtc = DateTimeOffset.UtcNow;
    var results = new List<StepResult>();
    var allSteps = new[] { "host-ready", "open-preferences", "select-automation", "cancel-preferences", "normal-close" };
    var status = "running";
    Process? host = null;
    AutomationElement? main = null;
    AutomationElement? preferences = null;
    Task? openTask = null;
    try
    {
        var start = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe)! };
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", artifacts, "--open", input })
            start.ArgumentList.Add(argument);
        host = Process.Start(start) ?? throw new InvalidOperationException("Could not start GUI host");
        Step("host-ready", () =>
        {
            var ready = AutomationProtocol.WaitForReadyArtifact(Path.Combine(artifacts, "ready.json"), host, TimeSpan.FromSeconds(15));
            Ensure(ready.ProcessId == host.Id, "ready.json belongs to another process");
            main = UiaDriver.WaitForMainWindow(host, TimeSpan.FromSeconds(15));
        });
        Step("open-preferences", () =>
        {
            openTask = Task.Run(() => InvokeButton(main!, "Configure Aegisub"));
            preferences = WaitWindow(host!, "Preferences", TimeSpan.FromSeconds(10));
            SaveUiEvidence(preferences, artifacts, "preferences-before-selection");
        });
        Step("select-automation", () =>
        {
            var pages = preferences!.FindAll(TreeScope.Descendants, new AndCondition(
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.TreeItem),
                new PropertyCondition(AutomationElement.NameProperty, "Automation", PropertyConditionFlags.IgnoreCase)))
                .Cast<AutomationElement>().Where(item => item.Current.ProcessId == host!.Id && !item.Current.IsOffscreen).ToArray();
            Ensure(pages.Length == 1, $"Expected one visible Automation preferences page entry, found {pages.Length}");
            Ensure(pages[0].TryGetCurrentPattern(SelectionItemPattern.Pattern, out var pattern), "Automation preferences page is not selectable");
            ((SelectionItemPattern)pattern).Select();
            WaitUntil(() => ((SelectionItemPattern)pattern).Current.IsSelected, TimeSpan.FromSeconds(3), "Automation preferences page did not become selected");
            SaveUiEvidence(preferences, artifacts, "preferences-automation");
        });
        Step("cancel-preferences", () =>
        {
            InvokeButton(preferences!, "Cancel");
            WaitUntil(() => FindWindow(host!, "Preferences") is null, TimeSpan.FromSeconds(5), "Preferences did not close after Cancel");
            if (openTask is not null && !openTask.Wait(TimeSpan.FromSeconds(5))) throw new TimeoutException("Preferences invocation did not complete after Cancel");
        });
        Step("normal-close", () =>
        {
            RequestClose(main!);
            Ensure(host!.WaitForExit(TimeSpan.FromSeconds(7)), "GUI host did not exit through its own close action");
            Ensure(host.ExitCode == 0, $"GUI host exited with code {host.ExitCode}");
        });
        status = "discovery-complete";
        WriteManifest();
        return 0;
    }
    catch (Exception error)
    {
        if (preferences is not null) TryEvidence(preferences, artifacts, "preferences-failure");
        var failing = allSteps.FirstOrDefault(step => results.All(result => result.Name != step));
        if (failing is not null) results.Add(new StepResult(failing, "failed", error.Message));
        foreach (var step in allSteps.Where(step => results.All(result => result.Name != step)))
            results.Add(new StepResult(step, "not-run", "A preceding step failed"));
        status = "failed";
        WriteManifest();
        Console.Error.WriteLine(error);
        return 1;
    }
    finally
    {
        if (host is not null && !host.HasExited)
        {
            host.Kill(entireProcessTree: true);
            if (!host.WaitForExit(TimeSpan.FromSeconds(5))) throw new TimeoutException("GUI host did not stop within five seconds");
        }
        host?.Dispose();
    }

    void Step(string name, Action action)
    {
        using var timing = DriverTiming.Measure("settings-discovery-step:" + name);
        action();
        results.Add(new StepResult(name, "passed", ""));
        WriteManifest();
    }
    void WriteManifest() => File.WriteAllText(Path.Combine(artifacts, "manifest.json"), JsonSerializer.Serialize(new
    {
        Scenario = "language-settings-discovery",
        Scope = "UIA discovery only; not E19a settings acceptance",
        StartedUtc = startedUtc,
        FinishedUtc = DateTimeOffset.UtcNow,
        BudgetSeconds = 120,
        ExeSha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(exe))),
        MutationFixtureSha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(mutationFixture))),
        DriverSha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(Path.Combine("tests", "gui-automation", "lua-workspace", "editor-uia.cs")))),
        ExecutedDriverDllSha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(Assembly.GetEntryAssembly()?.Location ?? throw new InvalidOperationException("Executed driver assembly is unavailable")))),
        ExitStatus = status,
        Steps = results
    }, new JsonSerializerOptions { WriteIndented = true }));
}

static int RunLanguageSettings(string exe, string artifacts)
{
    var startedUtc = DateTimeOffset.UtcNow;
    var fixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "editor.ass");
    var mutationFixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "metadata-mutations.lua");
    var languageFixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "language-gui-file.lua");
    var driver = Path.Combine("tests", "gui-automation", "lua-workspace", "editor-uia.cs");
    var defaultRuntime = Path.Combine(Path.GetDirectoryName(exe)!, "runtimes", "LuaLS");
    var defaultRuntimeExe = Path.Combine(defaultRuntime, "bin", "lua-language-server.exe");
    var customRuntime = Path.Combine(artifacts, "LuaLS 自定义 release");
    var customRuntimeExe = Path.Combine(customRuntime, "bin", "lua-language-server.exe");
    var incompleteRuntime = Path.Combine(artifacts, "LuaLS incomplete");
    var missingRuntime = Path.Combine(artifacts, "LuaLS missing");
    var input = Path.Combine(artifacts, "input.ass");
    var luaFile = Path.Combine(artifacts, "language-settings-file.lua");
    var profile = Path.Combine(artifacts, "profile");
    Directory.CreateDirectory(profile);
    File.Copy(fixture, input, overwrite: true);
    File.Copy(mutationFixture, Path.Combine(artifacts, "metadata-mutations.lua"), overwrite: true);
    File.Copy(languageFixture, luaFile, overwrite: true);
    Ensure(IsCompleteLuaLsRelease(defaultRuntime), "Default LuaLS directory is not a complete release beside the executable");
    CopyDirectory(defaultRuntime, customRuntime);
    Directory.CreateDirectory(incompleteRuntime);
    File.Copy(Path.Combine(defaultRuntime, "main.lua"), Path.Combine(incompleteRuntime, "main.lua"), overwrite: true);
    Ensure(IsCompleteLuaLsRelease(customRuntime), "Copied custom LuaLS directory is not a complete release");
    Ensure(!IsCompleteLuaLsRelease(incompleteRuntime) && !Directory.Exists(missingRuntime), "Fault-injection LuaLS directories are not invalid and missing");
    DriverTiming.Start(artifacts);
    var results = new List<StepResult>();
    var evidence = new List<object>();
    var statuses = new Dictionary<string, string>();
    var allSteps = new[] { "host-ready", "default-settings", "cancel-does-not-apply", "disable-apply", "custom-release-apply", "incomplete-no-fallback-edit", "missing-no-fallback", "restore-real-completion", "normal-close" };
    var status = "running";
    Process? host = null;
    AutomationElement? main = null;
    AutomationElement? configureButton = null;
    AutomationElement? workspace = null;
    AutomationElement? editor = null;
    try
    {
        var start = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe)! };
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", artifacts, "--open", input })
            start.ArgumentList.Add(argument);
        host = Process.Start(start) ?? throw new InvalidOperationException("Could not start GUI host");
        Step("host-ready", () =>
        {
            var ready = AutomationProtocol.WaitForReadyArtifact(Path.Combine(artifacts, "ready.json"), host, TimeSpan.FromSeconds(15));
            Ensure(ready.ProcessId == host.Id, "ready.json belongs to another process");
            main = UiaDriver.WaitForMainWindow(host, TimeSpan.FromSeconds(15));
            configureButton = UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5026", "Configure Aegisub")
                ?? throw new InvalidOperationException("Main-window Configure Aegisub toolbar button is unavailable");
            Ensure(File.ReadAllBytes(input).SequenceEqual(File.ReadAllBytes(fixture)), "Host startup changed the subtitle fixture");
        });
        Step("default-settings", () =>
        {
            SelectCodeViaMacro(main!, host, first: true);
            InvokeMenu(main!, host, "Automation", "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(6));
            workspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(10));
            OpenLuaFile(workspace, host, luaFile, artifacts);
            editor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("Lua source editor is absent");
            SelectLanguageTab(workspace);
            WaitForLanguageStatus(workspace, value => value.Contains("ready", StringComparison.OrdinalIgnoreCase), TimeSpan.FromSeconds(15), "Default LuaLS did not become ready");
            statuses["default"] = LanguageStatus(workspace);
            var (preferences, invocation) = OpenAutomationPreferences(main!, configureButton!, host);
            var controls = LanguageSettingsControls(preferences, host.Id);
            Ensure(controls.Enabled.Current.ToggleState == ToggleState.On, "Enable LuaLS is not on by default");
            Ensure(controls.Directory.Current.Value == "?data/runtimes/LuaLS", "Default LuaLS directory does not match the documented value");
            SaveSettingsEvidence(preferences, "settings-default");
            ClosePreferencesWithCancel(preferences, invocation, host);
            Ensure(NormalizeSource(ReadEditor(editor)) == NormalizeSource(File.ReadAllText(luaFile)), "Opening default settings changed Lua source");
        });
        Step("cancel-does-not-apply", () =>
        {
            var (preferences, invocation) = OpenAutomationPreferences(main!, configureButton!, host);
            var controls = LanguageSettingsControls(preferences, host.Id);
            SetPreferenceValue(controls.DirectoryElement, incompleteRuntime);
            SetPreferenceToggle(controls.EnabledElement, false);
            SaveSettingsEvidence(preferences, "settings-cancel-pending");
            ClosePreferencesWithCancel(preferences, invocation, host);
            Thread.Sleep(250);
            Ensure(LanguageStatus(workspace!).Contains("ready", StringComparison.OrdinalIgnoreCase), "Cancel changed the active LuaLS configuration");
            statuses["after-cancel"] = LanguageStatus(workspace!);
            (preferences, invocation) = OpenAutomationPreferences(main!, configureButton!, host);
            controls = LanguageSettingsControls(preferences, host.Id);
            Ensure(controls.Enabled.Current.ToggleState == ToggleState.On, "Cancel persisted the disabled state");
            Ensure(controls.Directory.Current.Value == "?data/runtimes/LuaLS", "Cancel persisted the pending LuaLS directory");
            ClosePreferencesWithCancel(preferences, invocation, host);
        });
        Step("disable-apply", () =>
        {
            var (preferences, invocation) = OpenAutomationPreferences(main!, configureButton!, host);
            var controls = LanguageSettingsControls(preferences, host.Id);
            SetPreferenceToggle(controls.EnabledElement, false);
            InvokeButton(preferences, "Apply");
            ClosePreferencesWithCancel(preferences, invocation, host);
            WaitForLanguageStatus(workspace!, value => value.Contains("disabled", StringComparison.OrdinalIgnoreCase), TimeSpan.FromSeconds(6), "Apply did not disable LuaLS");
            statuses["disabled"] = LanguageStatus(workspace!);
            Ensure(NormalizeSource(ReadEditor(editor!)) == NormalizeSource(File.ReadAllText(luaFile)), "Disabling LuaLS changed source");
        });
        Step("custom-release-apply", () =>
        {
            var (preferences, invocation) = OpenAutomationPreferences(main!, configureButton!, host);
            var controls = LanguageSettingsControls(preferences, host.Id);
            SetPreferenceToggle(controls.EnabledElement, true);
            SetPreferenceValue(controls.DirectoryElement, customRuntime);
            SaveSettingsEvidence(preferences, "settings-custom-release");
            InvokeButton(preferences, "Apply");
            ClosePreferencesWithCancel(preferences, invocation, host);
            WaitForLanguageStatus(workspace!, value => value.Contains("ready", StringComparison.OrdinalIgnoreCase), TimeSpan.FromSeconds(15), "Custom LuaLS release did not become ready");
            statuses["custom"] = LanguageStatus(workspace!);
            WaitUntil(() => NativeProcess.DirectChildExecutablePaths(host.Id).Any(path => PathsEqual(path, customRuntimeExe)), TimeSpan.FromSeconds(4), "The ready LuaLS child did not run from the custom Unicode/space directory");
            File.WriteAllLines(Path.Combine(artifacts, "custom-runtime-child-paths.txt"),
                NativeProcess.DirectChildExecutablePaths(host.Id).Select(path => Path.GetRelativePath(artifacts, path).Replace('\\', '/')));
        });
        Step("incomplete-no-fallback-edit", () =>
        {
            var originalSource = NormalizeSource(ReadEditor(editor!));
            ApplyLanguageSettings(main!, host, incompleteRuntime, true);
            WaitForLanguageStatus(workspace!, value => value.Contains("unavailable", StringComparison.OrdinalIgnoreCase), TimeSpan.FromSeconds(6), "Incomplete LuaLS release did not report unavailable");
            statuses["incomplete"] = LanguageStatus(workspace!);
            Ensure(!NativeProcess.DirectChildExecutablePaths(host.Id).Any(path => PathsEqual(path, defaultRuntimeExe)), "Incomplete LuaLS release silently fell back to the default server");
            const string edited = "local text = '汉🌟 with spaces'\nreturn text";
            WriteEditor(editor!, edited, host);
            GuardedKeyboard.FocusEditor(editor!, host);
            GuardedKeyboard.SendChord(editor!, host, 'S');
            WaitUntil(() => NormalizeSource(File.ReadAllText(luaFile)) == edited, TimeSpan.FromSeconds(5), "Saving source failed while LuaLS was unavailable");
            File.WriteAllText(Path.Combine(artifacts, "unavailable-saved.lua"), File.ReadAllText(luaFile), new UTF8Encoding(false));
            GuardedKeyboard.FocusEditor(editor!, host);
            GuardedKeyboard.SendChord(editor!, host, 'Z');
            WaitUntil(() => NormalizeSource(ReadEditor(editor!)) == originalSource, TimeSpan.FromSeconds(5), "Local Undo failed while LuaLS was unavailable");
            File.WriteAllText(Path.Combine(artifacts, "unavailable-undo.lua"), NormalizeSource(ReadEditor(editor!)), new UTF8Encoding(false));
        });
        Step("missing-no-fallback", () =>
        {
            ApplyLanguageSettings(main!, host, missingRuntime, true);
            WaitForLanguageStatus(workspace!, value => value.Contains("unavailable", StringComparison.OrdinalIgnoreCase), TimeSpan.FromSeconds(6), "Missing LuaLS release did not report unavailable");
            statuses["missing"] = LanguageStatus(workspace!);
            Ensure(!NativeProcess.DirectChildExecutablePaths(host.Id).Any(path => PathsEqual(path, defaultRuntimeExe)), "Missing LuaLS release silently fell back to the default server");
            SaveSettingsEvidence(workspace!, "settings-missing-unavailable");
        });
        Step("restore-real-completion", () =>
        {
            ApplyLanguageSettings(main!, host, "?data/runtimes/LuaLS", true);
            WaitForLanguageStatus(workspace!, value => value.Contains("ready", StringComparison.OrdinalIgnoreCase), TimeSpan.FromSeconds(15), "Restored default LuaLS did not become ready");
            statuses["restored"] = LanguageStatus(workspace!);
            const string completionSource = "local catalog = { value = '星🌟' }\nreturn catalog.val";
            WriteEditor(editor!, completionSource, host);
            GuardedKeyboard.FocusEditor(editor!, host);
            GuardedKeyboard.SendChord(editor!, host, (ushort)0x23);
            GuardedKeyboard.SendChord(editor!, host, (ushort)0x20);
            AutomationElement? popup = null;
            WaitUntil(() => (popup = FindVisibleLanguagePopup(workspace!, host, "AutoCompListBox")) is not null, TimeSpan.FromSeconds(8), "Restored LuaLS did not show a real completion popup");
            var popupPath = Path.Combine(artifacts, "settings-restored-completion.png");
            var capture = ScreenCapture.SaveWindowPng(popup!, popupPath);
            evidence.Add(new { Artifact = Path.GetFileName(popupPath), Sha256 = Sha256File(popupPath), Contract = "LuaLS value completion after restoring default configuration", capture.Width, capture.Height });
            GuardedKeyboard.SendKey(editor!, host, 0x0D);
            WaitUntil(() => NormalizeSource(ReadEditor(editor!)) == completionSource + "ue", TimeSpan.FromSeconds(5), "Restored LuaLS completion did not insert value");
            GuardedKeyboard.FocusEditor(editor!, host);
            GuardedKeyboard.SendChord(editor!, host, 'Z');
            WaitUntil(() => NormalizeSource(ReadEditor(editor!)) == completionSource, TimeSpan.FromSeconds(5), "One Undo did not revert the restored LuaLS completion");
            Ensure(File.ReadAllBytes(input).SequenceEqual(File.ReadAllBytes(fixture)), "Language settings or ordinary-file editing changed subtitle source");
        });
        Step("normal-close", () =>
        {
            RequestClose(workspace!);
            InvokeButton(WaitWindow(host, "Unsaved Lua source", TimeSpan.FromSeconds(5)), "Discard");
            WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(5), "Workspace did not close after Discard");
            GuardedKeyboard.StabilizeOwnedClipboard(host.Id);
            RequestClose(main!);
            Ensure(host.WaitForExit(TimeSpan.FromSeconds(7)), "GUI host did not exit through its own close action");
            Ensure(host.ExitCode == 0, $"GUI host exited with code {host.ExitCode}");
        });
        status = "passed";
        WriteManifest();
        return 0;
    }
    catch (Exception error)
    {
        if (workspace is not null) TryEvidence(workspace, artifacts, "language-settings-failure");
        var failing = allSteps.FirstOrDefault(step => results.All(result => result.Name != step));
        if (failing is not null) results.Add(new StepResult(failing, "failed", error.Message));
        foreach (var step in allSteps.Where(step => results.All(result => result.Name != step)))
            results.Add(new StepResult(step, "not-run", "A preceding step failed"));
        status = "failed";
        WriteManifest();
        Console.Error.WriteLine(error);
        return 1;
    }
    finally
    {
        if (host is not null && !host.HasExited)
        {
            try { GuardedKeyboard.StabilizeOwnedClipboard(host.Id); } catch { }
            host.Kill(entireProcessTree: true);
            if (!host.WaitForExit(TimeSpan.FromSeconds(5))) throw new TimeoutException("GUI host did not stop within five seconds");
        }
        host?.Dispose();
    }

    void Step(string name, Action action)
    {
        using var timing = DriverTiming.Measure("settings-step:" + name);
        action();
        results.Add(new StepResult(name, "passed", ""));
        WriteManifest();
    }

    void SaveSettingsEvidence(AutomationElement window, string name)
    {
        SaveNativeDialogEvidence(window, artifacts, name);
        var image = Path.Combine(artifacts, name + ".png");
        evidence.Add(new { Artifact = Path.GetFileName(image), Sha256 = Sha256File(image), Contract = name });
    }

    void ApplyLanguageSettings(AutomationElement root, Process process, string directory, bool enabled)
    {
        var (preferences, invocation) = OpenAutomationPreferences(root, configureButton!, process);
        var controls = LanguageSettingsControls(preferences, process.Id);
        if (controls.Enabled.Current.ToggleState == ToggleState.On)
            SetPreferenceValue(controls.DirectoryElement, directory);
        SetPreferenceToggle(controls.EnabledElement, enabled);
        if (enabled && controls.Directory.Current.Value != directory)
            SetPreferenceValue(controls.DirectoryElement, directory);
        InvokeButton(preferences, "Apply");
        ClosePreferencesWithCancel(preferences, invocation, process);
    }

    void WriteManifest() => File.WriteAllText(Path.Combine(artifacts, "manifest.json"), JsonSerializer.Serialize(new
    {
        Scenario = "language-settings",
        Scope = "E19a settings Apply/Cancel, enable/disable, custom complete release, invalid/missing refusal, unavailable editing, and restored completion",
        StartedUtc = startedUtc,
        FinishedUtc = DateTimeOffset.UtcNow,
        BudgetSeconds = 120,
        ExeSha256 = Sha256File(exe),
        DriverSha256 = Sha256File(driver),
        DefaultRuntimeExeSha256 = File.Exists(defaultRuntimeExe) ? Sha256File(defaultRuntimeExe) : null,
        CustomRuntimeExeSha256 = File.Exists(customRuntimeExe) ? Sha256File(customRuntimeExe) : null,
        CustomRuntime = Path.GetRelativePath(artifacts, customRuntime).Replace('\\', '/'),
        HostExitCode = host is { HasExited: true } ? (int?)host.ExitCode : null,
        Statuses = statuses,
        Evidence = evidence,
        ExitStatus = status,
        Steps = results
    }, new JsonSerializerOptions { WriteIndented = true }));
}

static (AutomationElement Preferences, Task Invocation) OpenAutomationPreferences(AutomationElement main, AutomationElement configureButton, Process host)
{
    Ensure(main.Current.ProcessId == configureButton.Current.ProcessId, "Configure button does not belong to the main window");
    var invocation = Task.Run(() => InvokeKnownButton(configureButton, "Configure Aegisub"));
    var preferences = WaitWindow(host, "Preferences", TimeSpan.FromSeconds(10));
    var trees = NativeMessage.DescendantsWithClass((nint)preferences.Current.NativeWindowHandle, "SysTreeView32")
        .Select(AutomationElement.FromHandle).Where(item => item.Current.ProcessId == host.Id && item.Current.ControlType == ControlType.Tree).ToArray();
    Ensure(trees.Length == 1, $"Expected one native Preferences page tree, found {trees.Length}");
    var pages = trees[0].FindAll(TreeScope.Descendants, new AndCondition(
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.TreeItem),
        new PropertyCondition(AutomationElement.NameProperty, "Automation", PropertyConditionFlags.IgnoreCase)))
        .Cast<AutomationElement>().Where(item => item.Current.ProcessId == host.Id && !item.Current.IsOffscreen).ToArray();
    Ensure(pages.Length == 1, $"Expected one visible Automation preferences page entry, found {pages.Length}");
    Ensure(pages[0].TryGetCurrentPattern(SelectionItemPattern.Pattern, out var selection), "Automation preferences page is not selectable");
    ((SelectionItemPattern)selection).Select();
    WaitUntil(() => ((SelectionItemPattern)selection).Current.IsSelected, TimeSpan.FromSeconds(3), "Automation preferences page did not become selected");
    return (preferences, invocation);
}

static LanguageSettingControls LanguageSettingsControls(AutomationElement preferences, int hostId)
{
    var checks = NativeMessage.DescendantsWithTitle((nint)preferences.Current.NativeWindowHandle, "Enable LuaLS")
        .Select(AutomationElement.FromHandle).Where(item => item.Current.ProcessId == hostId
            && item.Current.ControlType == ControlType.CheckBox && !item.Current.IsOffscreen).ToArray();
    var edits = NativeMessage.DescendantsWithClass((nint)preferences.Current.NativeWindowHandle, "Edit")
        .Select(AutomationElement.FromHandle).Where(item => item.Current.ProcessId == hostId
            && item.Current.ControlType == ControlType.Edit && item.Current.Name == "Lua Workspace" && !item.Current.IsOffscreen).ToArray();
    Ensure(checks.Length == 1, $"Expected one visible Enable LuaLS checkbox, found {checks.Length}");
    Ensure(edits.Length == 1, $"Expected one visible LuaLS directory editor, found {edits.Length}");
    Ensure(checks[0].TryGetCurrentPattern(TogglePattern.Pattern, out var toggle), "Enable LuaLS has no TogglePattern");
    Ensure(edits[0].TryGetCurrentPattern(ValuePattern.Pattern, out var value), "LuaLS directory has no ValuePattern");
    return new LanguageSettingControls(checks[0], (TogglePattern)toggle, edits[0], (ValuePattern)value);
}

static void SetPreferenceToggle(AutomationElement element, bool enabled)
{
    Ensure(element.TryGetCurrentPattern(TogglePattern.Pattern, out var pattern), "Preference checkbox has no TogglePattern");
    var toggle = (TogglePattern)pattern;
    var expected = enabled ? ToggleState.On : ToggleState.Off;
    if (toggle.Current.ToggleState != expected) toggle.Toggle();
    WaitUntil(() => toggle.Current.ToggleState == expected, TimeSpan.FromSeconds(2), "Preference checkbox did not reach the requested state");
}

static void SetPreferenceValue(AutomationElement element, string value)
{
    Ensure(element.Current.IsEnabled, "LuaLS directory preference is disabled");
    Ensure(element.TryGetCurrentPattern(ValuePattern.Pattern, out var pattern), "LuaLS directory has no ValuePattern");
    ((ValuePattern)pattern).SetValue(value);
    WaitUntil(() => ((ValuePattern)pattern).Current.Value == value, TimeSpan.FromSeconds(2), "LuaLS directory did not retain the requested text");
}

static void ClosePreferencesWithCancel(AutomationElement preferences, Task invocation, Process host)
{
    InvokeButton(preferences, "Cancel");
    WaitUntil(() => FindWindow(host, "Preferences") is null, TimeSpan.FromSeconds(5), "Preferences did not close after Cancel");
    if (!invocation.Wait(TimeSpan.FromSeconds(5))) throw new TimeoutException("Preferences invocation did not complete after Cancel");
}

static void WaitForLanguageStatus(AutomationElement workspace, Func<string, bool> expected, TimeSpan timeout, string message)
{
    WaitUntil(() => expected(LanguageStatus(workspace)), timeout, message + $"; observed '{LanguageStatus(workspace)}'");
}

static bool IsCompleteLuaLsRelease(string path) => File.Exists(Path.Combine(path, "bin", "lua-language-server.exe"))
    && File.Exists(Path.Combine(path, "main.lua")) && Directory.Exists(Path.Combine(path, "script")) && Directory.Exists(Path.Combine(path, "meta"));

static void CopyDirectory(string source, string destination)
{
    foreach (var directory in Directory.EnumerateDirectories(source, "*", SearchOption.AllDirectories))
        Directory.CreateDirectory(Path.Combine(destination, Path.GetRelativePath(source, directory)));
    Directory.CreateDirectory(destination);
    foreach (var file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
    {
        var target = Path.Combine(destination, Path.GetRelativePath(source, file));
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        File.Copy(file, target, overwrite: true);
    }
}

static bool PathsEqual(string left, string right) => string.Equals(Path.GetFullPath(left).TrimEnd(Path.DirectorySeparatorChar),
    Path.GetFullPath(right).TrimEnd(Path.DirectorySeparatorChar), StringComparison.OrdinalIgnoreCase);

static string Sha256File(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));

static int RunLanguage(string exe, string artifacts, bool tipsOnly)
{
    var startedUtc = DateTimeOffset.UtcNow;
    var fixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "editor.ass");
    var mutationFixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "metadata-mutations.lua");
    var languageFixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "language-gui-file.lua");
    var driver = Path.Combine("tests", "gui-automation", "lua-workspace", "editor-uia.cs");
    var runtime = Path.Combine(Path.GetDirectoryName(exe)!, "runtimes", "LuaLS");
    var runtimeExe = Path.Combine(runtime, "bin", "lua-language-server.exe");
    var input = Path.Combine(artifacts, "input.ass");
    var luaFile = Path.Combine(artifacts, "language-file.lua");
    var profile = Path.Combine(artifacts, "profile");
    Directory.CreateDirectory(profile);
    File.Copy(fixture, input, overwrite: true);
    File.Copy(mutationFixture, Path.Combine(artifacts, "metadata-mutations.lua"), overwrite: true);
    File.Copy(languageFixture, luaFile, overwrite: true);
    var results = new List<StepResult>();
    var visualEvidence = new List<object>();
    var allSteps = tipsOnly
        ? new[] { "host-ready", "open-ordinary-lua", "signature-shortcut", "current-diagnostics", "normal-close" }
        : new[] { "host-ready", "open-ordinary-lua", "completion-exact-replacement", "completion-cancel-and-one-undo", "completion-filter-after-dot", "signature-shortcut", "current-diagnostics", "normal-close" };
    var status = "running";
    Process? host = null;
    AutomationElement? main = null;
    AutomationElement? workspace = null;
    AutomationElement? editor = null;
    try
    {
        Ensure(File.Exists(runtimeExe) && File.Exists(Path.Combine(runtime, "main.lua"))
            && Directory.Exists(Path.Combine(runtime, "script")) && Directory.Exists(Path.Combine(runtime, "meta")),
            "Default LuaLS directory is not a complete release beside the executable");
        var start = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe)! };
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", artifacts, "--open", input })
            start.ArgumentList.Add(argument);
        host = Process.Start(start) ?? throw new InvalidOperationException("Could not start GUI host");
        Step("host-ready", () =>
        {
            var ready = AutomationProtocol.WaitForReadyArtifact(Path.Combine(artifacts, "ready.json"), host, TimeSpan.FromSeconds(15));
            Ensure(ready.ProcessId == host.Id, "ready.json belongs to another process");
            _ = UiaDriver.WaitForMainWindow(host, TimeSpan.FromSeconds(15));
        });
        main = AutomationElement.FromHandle(host.MainWindowHandle);
        Step("open-ordinary-lua", () =>
        {
            var openTimer = Stopwatch.StartNew();
            try
            {
                SelectCodeViaMacro(main, host, first: true);
                InvokeMenu(main, host, "Automation", "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(6));
                workspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(15));
            }
            finally
            {
                File.WriteAllText(Path.Combine(artifacts, "language-open-elapsed-ms.txt"), openTimer.ElapsedMilliseconds.ToString(System.Globalization.CultureInfo.InvariantCulture));
            }
            OpenLuaFile(workspace, host, luaFile, artifacts);
            editor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("Lua source editor is absent");
            SelectLanguageTab(workspace);
            WaitUntil(() => LanguageStatus(workspace).Contains("ready", StringComparison.OrdinalIgnoreCase), TimeSpan.FromSeconds(15), "LuaLS did not become ready for the ordinary Lua file");
            Ensure(NormalizeSource(ReadEditor(editor)) == NormalizeSource(File.ReadAllText(luaFile)), "Ordinary Lua file was not loaded");
            SaveUiEvidence(workspace, artifacts, "language-opened");
        });
        const string initialSource = "local catalog = { value = '星🌟', method = function(value) return value end }\nreturn '漢😀' .. catalog.val";
        Step("completion-exact-replacement", () =>
        {
            WriteEditor(editor!, initialSource, host);
            Ensure(NormalizeSource(ReadEditor(editor!)) == initialSource, "Completion source was not installed before the request");
            Ensure(FindVisibleLanguagePopup(workspace!, host, "AutoCompListBox") is null, "Completion popup was already visible before the request");
            GuardedKeyboard.FocusEditor(editor!, host);
            GuardedKeyboard.SendChord(editor!, host, (ushort)0x23);
            GuardedKeyboard.SendChord(editor!, host, (ushort)0x20);
            CapturePopup("AutoCompListBox", "completion-before", "value", "method absent after val prefix");
            GuardedKeyboard.SendKey(editor!, host, 0x0D);
            WaitUntil(() => NormalizeSource(ReadEditor(editor!)) == initialSource + "ue", TimeSpan.FromSeconds(5), "LuaLS completion did not replace only the unfinished member name");
            File.WriteAllText(Path.Combine(artifacts, "completion-observed.lua"), NormalizeSource(ReadEditor(editor!)), new UTF8Encoding(false));
            GuardedKeyboard.FocusEditor(editor!, host);
            GuardedKeyboard.SendChord(editor!, host, 'S');
            WaitUntil(() => NormalizeSource(File.ReadAllText(luaFile)) == initialSource + "ue", TimeSpan.FromSeconds(5), "Accepted completion was not saved with exact replacement");
        });
        Step("completion-cancel-and-one-undo", () =>
        {
            var completed = NormalizeSource(ReadEditor(editor!));
            Ensure(FindVisibleLanguagePopup(workspace!, host, "AutoCompListBox") is null, "Completion popup remained visible before the cancellation request");
            GuardedKeyboard.FocusEditor(editor!, host);
            GuardedKeyboard.SendChord(editor!, host, (ushort)0x23);
            GuardedKeyboard.SendChord(editor!, host, (ushort)0x20);
            CapturePopup("AutoCompListBox", "completion-cancel", "value", "Escape must leave the editor unchanged");
            GuardedKeyboard.SendKey(editor!, host, 0x1B);
            WaitUntil(() => FindVisibleLanguagePopup(workspace!, host, "AutoCompListBox") is null,
                TimeSpan.FromSeconds(3), "Escape did not dismiss the completion popup");
            Ensure(NormalizeSource(ReadEditor(editor!)) == completed, "Cancelling completion changed source");
            GuardedKeyboard.FocusEditor(editor!, host);
            GuardedKeyboard.SendChord(editor!, host, 'Z');
            WaitUntil(() => NormalizeSource(ReadEditor(editor!)) == initialSource, TimeSpan.FromSeconds(5), "One Undo did not restore the pre-completion source");
        });
        Step("completion-filter-after-dot", () =>
        {
            const string filterSource = "local catalog = { value = '星🌟', method = function(value) return value end }\nreturn '漢😀' .. catalog";
            WriteEditor(editor!, filterSource, host);
            Ensure(NormalizeSource(ReadEditor(editor!)) == filterSource, "Filter source was not installed before the request");
            Ensure(FindVisibleLanguagePopup(workspace!, host, "AutoCompListBox") is null, "Completion popup was already visible before filtering");
            GuardedKeyboard.FocusEditor(editor!, host);
            GuardedKeyboard.SendChord(editor!, host, (ushort)0x23);
            GuardedKeyboard.TypeText(editor!, host, ".");
            CapturePopup("AutoCompListBox", "completion-before-filter", "method, value", "unfiltered catalog members");
            GuardedKeyboard.TypeText(editor!, host, "v");
            _ = host.WaitForInputIdle(TimeSpan.FromMilliseconds(500));
            CapturePopup("AutoCompListBox", "completion-after-filter", "value", "method absent after typing v");
            GuardedKeyboard.SendKey(editor!, host, 0x0D);
            WaitUntil(() => NormalizeSource(ReadEditor(editor!)) == filterSource + ".value", TimeSpan.FromSeconds(5),
                "Filtered completion did not insert the exact value member");
            File.WriteAllText(Path.Combine(artifacts, "completion-filtered-observed.lua"), NormalizeSource(ReadEditor(editor!)), new UTF8Encoding(false));
            GuardedKeyboard.FocusEditor(editor!, host);
            GuardedKeyboard.SendChord(editor!, host, 'Z');
            WaitUntil(() => NormalizeSource(ReadEditor(editor!)) == filterSource + ".v", TimeSpan.FromSeconds(5),
                "One Undo did not restore the typed member prefix");
        }, "Guarded dot typing triggered the broad list; typing v filtered it, Enter inserted catalog.value, and one Undo restored catalog.v");
        Step("signature-shortcut", () =>
        {
            const string signatureSource = "local catalog = { method = function(value) return value end }\nreturn catalog.method(";
            WriteEditor(editor!, signatureSource, host);
            Ensure(NormalizeSource(ReadEditor(editor!)) == signatureSource, "Signature source was not installed before the request");
            Ensure(FindVisibleLanguagePopup(workspace!, host, "wxSTCCallTip") is null, "Calltip was already visible before the signature request");
            GuardedKeyboard.FocusEditor(editor!, host);
            GuardedKeyboard.SendChord(editor!, host, (ushort)0x23);
            GuardedKeyboard.SendChord(editor!, host, (ushort)0x20, shift: true);
            CapturePopup("wxSTCCallTip", "language-signature", "value", "method parameter name visible in calltip");
            GuardedKeyboard.SendKey(editor!, host, 0x1B);
        });
        Step("current-diagnostics", () =>
        {
            const string invalidSource = "local greeting = '星🌟'\nreturn missing_language_symbol";
            WriteEditor(editor!, invalidSource, host);
            WaitUntil(() => LanguageDiagnostics(workspace!).Contains("missing_language_symbol", StringComparison.Ordinal),
                TimeSpan.FromSeconds(8), "Current-version LuaLS diagnostics did not identify the missing global");
            SaveUiEvidence(workspace!, artifacts, "language-diagnostics");
            File.WriteAllText(Path.Combine(artifacts, "diagnostics-observed.txt"), LanguageDiagnostics(workspace!), new UTF8Encoding(false));
        });
        Step("normal-close", () =>
        {
            RequestClose(workspace!);
            InvokeButton(WaitWindow(host, "Unsaved Lua source", TimeSpan.FromSeconds(5)), "Discard");
            WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(5), "Workspace did not close after Discard");
            GuardedKeyboard.StabilizeOwnedClipboard(host.Id);
            RequestClose(main);
            Ensure(host.WaitForExit(TimeSpan.FromSeconds(7)), "GUI host did not exit through its own close action");
        });
        status = "awaiting-visual-review";
        WriteManifest();
        return 0;
    }
    catch (Exception error)
    {
        if (workspace is not null && host is not null && !host.HasExited)
        {
            foreach (var (popupName, artifact) in new[] { ("AutoCompListBox", "completion-failure"), ("wxSTCCallTip", "signature-failure") })
                try
                {
                    if (FindVisibleLanguagePopup(workspace, host, popupName) is { } popup)
                        ScreenCapture.SaveWindowPng(popup, Path.Combine(artifacts, artifact + ".png"));
                }
                catch { }
        }
        if (main is not null)
            try { ScreenCapture.SaveWindowPng(main, Path.Combine(artifacts, "language-failure-main.png")); } catch { }
        if (host is not null && !host.HasExited)
        {
            try { CaptureHostWindowInventory(host, artifacts); }
            catch (Exception captureError)
            {
                File.WriteAllText(Path.Combine(artifacts, "language-failure-inventory-error.txt"), captureError.ToString());
            }
        }
        var failing = allSteps.FirstOrDefault(step => results.All(result => result.Name != step));
        if (failing is not null) results.Add(new StepResult(failing, "failed", error.Message));
        foreach (var step in allSteps.Where(step => results.All(result => result.Name != step)))
            results.Add(new StepResult(step, "not-run", "A preceding step failed"));
        status = "failed";
        WriteManifest();
        Console.Error.WriteLine(error);
        return 1;
    }
    finally
    {
        if (workspace is not null)
            try { ScreenCapture.SaveWindowPng(workspace, Path.Combine(artifacts, "language-final.png")); } catch { }
        Exception? stabilizationError = null;
        if (host is not null && !host.HasExited)
        {
            try { GuardedKeyboard.StabilizeOwnedClipboard(host.Id); }
            catch (Exception error) { stabilizationError = error; }
            host.Kill(entireProcessTree: true);
            if (!host.WaitForExit(TimeSpan.FromSeconds(5)))
                throw new TimeoutException("GUI host did not stop within five seconds");
        }
        host?.Dispose();
        if (stabilizationError is not null)
            throw new InvalidOperationException("Verified GUI-host clipboard text could not be persisted before host shutdown", stabilizationError);
    }

    void Step(string name, Action action, string detail = "")
    {
        if (tipsOnly && name.StartsWith("completion-", StringComparison.Ordinal)) return;
        action();
        results.Add(new StepResult(name, "passed", detail));
        WriteManifest();
    }
    void WriteManifest() => File.WriteAllText(Path.Combine(artifacts, "manifest.json"), JsonSerializer.Serialize(new
    {
        Scenario = tipsOnly ? "language-tip" : "language",
        Scope = tipsOnly ? "diagnostic subset; not full language acceptance" : "full language scenario",
        StartedUtc = startedUtc,
        FinishedUtc = DateTimeOffset.UtcNow,
        BudgetSeconds = 120,
        ExeSha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(exe))),
        RuntimeExeSha256 = File.Exists(runtimeExe) ? Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(runtimeExe))) : null,
        HostExitCode = host is { HasExited: true } ? (int?)host.ExitCode : null,
        Fixture = languageFixture.Replace('\\', '/'),
        FixtureSha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(languageFixture))),
        DriverSha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(driver))),
        ExecutedDriverDllSha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(Assembly.GetEntryAssembly()?.Location ?? throw new InvalidOperationException("Executed driver assembly is unavailable")))),
        SharedDriverDllSha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(typeof(UiaDriver).Assembly.Location))),
        ExitStatus = status,
        Steps = results
    }, new JsonSerializerOptions { WriteIndented = true }));

    void CapturePopup(string popupName, string artifactName, string expectedPresent, string expectedAbsentOrContract)
    {
        AutomationElement? popup = null;
        try
        {
            WaitUntil(() => (popup = FindVisibleLanguagePopup(workspace!, host!, popupName)) is not null,
                TimeSpan.FromSeconds(popupName == "wxSTCCallTip" ? 6 : 8), $"Visible {popupName} popup was not found");
        }
        catch (TimeoutException) when (popupName == "wxSTCCallTip")
        {
            try { CaptureSignatureWindowInventory(workspace!, host!, artifacts); }
            catch (Exception error) { File.WriteAllText(Path.Combine(artifacts, "signature-window-inventory-error.txt"), error.ToString()); }
            throw;
        }
        var imagePath = Path.Combine(artifacts, artifactName + ".png");
        var capture = ScreenCapture.SaveWindowPng(popup!, imagePath);
        visualEvidence.Add(new { Artifact = Path.GetFileName(imagePath), Sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(imagePath))),
            PopupName = popupName, ExpectedPresent = expectedPresent, ExpectedAbsentOrContract = expectedAbsentOrContract,
            AccessibleName = popupName == "wxSTCCallTip" ? NativeMessage.AccessibleName((nint)popup!.Current.NativeWindowHandle) : null,
            Width = capture.Width, Height = capture.Height });
        File.WriteAllText(Path.Combine(artifacts, "visual-review.json"), JsonSerializer.Serialize(new
        {
            Status = "pending-human-review",
            Evidence = visualEvidence
        }, new JsonSerializerOptions { WriteIndented = true }));
    }
}

static void CaptureSignatureWindowInventory(AutomationElement workspace, Process host, string artifacts)
{
    var workspaceHandle = (nint)workspace.Current.NativeWindowHandle;
    var lines = new List<string>();
    var named = new List<AutomationElement>();
    var roots = AutomationElement.RootElement.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ProcessIdProperty, host.Id));
    foreach (AutomationElement root in roots)
        if (root.Current.Name == "wxSTCCallTip") named.Add(root);
    named.AddRange(workspace.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.NameProperty, "wxSTCCallTip")).Cast<AutomationElement>());
    var candidateHandles = new HashSet<nint>();
    foreach (var candidate in named)
    {
        try
        {
            var current = candidate.Current;
            var handle = (nint)current.NativeWindowHandle;
            lines.Add($"named\t{handle}\t{current.Name}\t{current.ControlType.ProgrammaticName}\t{current.IsEnabled}\t{current.IsOffscreen}\t{current.BoundingRectangle}");
            if (current.ProcessId == host.Id && handle != 0 && NativeMessage.IsVisible(handle)
                && NativeMessage.IsRelatedTo(handle, workspaceHandle))
                candidateHandles.Add(handle);
        }
        catch (ElementNotAvailableException) { }
    }
    foreach (var window in NativeMessage.SameProcessWindows(host.Id, workspaceHandle))
        lines.Add("native\t" + window);
    foreach (var hwnd in NativeMessage.OwnedCalltipPopups(host.Id, workspaceHandle))
        lines.Add($"owned-calltip-popup\t{hwnd}\taccessible-name={NativeMessage.AccessibleName(hwnd) ?? "<none>"}");
    File.WriteAllLines(Path.Combine(artifacts, "signature-window-inventory.txt"), lines, new UTF8Encoding(false));
    if (candidateHandles.Count == 1)
    {
        var candidate = AutomationElement.FromHandle(candidateHandles.Single());
        ScreenCapture.SaveWindowPng(candidate, Path.Combine(artifacts, "signature-native-visible-candidate.png"));
    }
}

static void Ensure(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

static void CaptureHostWindowInventory(Process host, string artifacts)
{
    var lines = new List<string>();
    var roots = AutomationElement.RootElement.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ProcessIdProperty, host.Id));
    lines.Add($"HostProcessId={host.Id};RootCount={roots.Count};MainWindowHandle={host.MainWindowHandle}");
    foreach (AutomationElement root in roots)
    {
        Describe(root, "root");
    }
    File.WriteAllLines(Path.Combine(artifacts, "language-failure-windows.txt"), lines, new UTF8Encoding(false));

    void Describe(AutomationElement element, string level)
    {
        try
        {
            var current = element.Current;
            lines.Add($"{level}\t{current.ControlType.ProgrammaticName}\t{current.Name}\t{current.ClassName}\t{current.NativeWindowHandle}\t{current.IsOffscreen}\t{current.IsEnabled}");
        }
        catch (ElementNotAvailableException) { lines.Add($"{level}\t<unavailable>"); }
    }
}

static void SelectLanguageTab(AutomationElement workspace)
{
    var tab = workspace.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.TabItem))
        .Cast<AutomationElement>().FirstOrDefault(item => item.Current.Name == "Language")
        ?? throw new InvalidOperationException("Workspace Language tab is not exposed by UIA");
    if (!tab.TryGetCurrentPattern(SelectionItemPattern.Pattern, out var pattern))
        throw new InvalidOperationException("Workspace Language tab is not UIA-selectable");
    ((SelectionItemPattern)pattern).Select();
    WaitUntil(() => ((SelectionItemPattern)pattern).Current.IsSelected, TimeSpan.FromSeconds(3), "Language tab did not become selected");
}

static string LanguageStatus(AutomationElement workspace)
{
    var nativeStatuses = NativeMessage.DescendantsWithTitlePrefix((nint)workspace.Current.NativeWindowHandle, "LuaLS")
        .Select(AutomationElement.FromHandle).Where(item =>
        {
            var current = item.Current;
            return current.ProcessId == workspace.Current.ProcessId && current.ControlType == ControlType.Text && !current.IsOffscreen;
        }).ToArray();
    Ensure(nativeStatuses.Length <= 1, $"Expected at most one visible native LuaLS status control, found {nativeStatuses.Length}");
    if (nativeStatuses.Length == 1)
        return NativeMessage.GetWindowText((nint)nativeStatuses[0].Current.NativeWindowHandle);
    var named = FindNamed(workspace, "Lua language status");
    if (named is not null)
    {
        var handle = new nint(named.Current.NativeWindowHandle);
        if (handle != 0) return NativeMessage.GetWindowText(handle);
    }
    var status = workspace.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Text))
        .Cast<AutomationElement>().FirstOrDefault(item => (item.Current.Name ?? "").Contains("LuaLS", StringComparison.OrdinalIgnoreCase));
    if (status is null) return named?.Current.Name ?? "";
    var observed = new nint(status.Current.NativeWindowHandle);
    return observed != 0 ? NativeMessage.GetWindowText(observed) : status.Current.Name;
}

static string LanguageDiagnostics(AutomationElement workspace)
{
    SelectLanguageTab(workspace);
    var hostId = workspace.Current.ProcessId;
    var statuses = workspace.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Text))
        .Cast<AutomationElement>().Where(item =>
        {
            var current = item.Current;
            return current.ProcessId == hostId && current.ClassName == "Static" && current.NativeWindowHandle != 0
                && current.IsEnabled && !current.IsOffscreen && (current.Name ?? "").StartsWith("LuaLS", StringComparison.OrdinalIgnoreCase);
        }).ToArray();
    Ensure(statuses.Length == 1, $"Expected one visible LuaLS status control, found {statuses.Length}");
    var panel = TreeWalker.ControlViewWalker.GetParent(statuses[0])
        ?? throw new InvalidOperationException("LuaLS status has no parent panel");
    Ensure(panel.Current.ControlType == ControlType.Pane && panel.Current.ProcessId == hostId,
        "LuaLS status does not belong to a same-process panel");
    var documents = panel.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Document))
        .Cast<AutomationElement>().Where(item =>
        {
            var current = item.Current;
            return current.ProcessId == hostId && current.ClassName == "Edit" && current.NativeWindowHandle != 0
                && current.IsEnabled && !current.IsOffscreen;
        }).ToArray();
    Ensure(documents.Length == 1, $"Expected one visible LuaLS diagnostics Edit under the status panel, found {documents.Length}");
    return NativeMessage.GetWindowText(new nint(documents[0].Current.NativeWindowHandle));
}

static AutomationElement? FindVisibleLanguagePopup(AutomationElement workspace, Process host, string name)
{
    var workspaceHandle = (nint)workspace.Current.NativeWindowHandle;
    if (name == "wxSTCCallTip")
    {
        var popups = NativeMessage.OwnedCalltipPopups(host.Id, workspaceHandle);
        var named = popups.Where(handle => NativeMessage.AccessibleName(handle) == name).ToArray();
        var selected = named.Length == 1 ? named[0] : named.Length == 0 && popups.Count == 1 ? popups[0] : 0;
        if (selected != 0)
        {
            var popup = AutomationElement.FromHandle(selected);
            var current = popup.Current;
            var rect = current.BoundingRectangle;
            if (current.ProcessId == host.Id && current.IsEnabled && !current.IsOffscreen
                && rect.Width > 0 && rect.Height > 0)
                return popup;
        }
    }
    var roots = AutomationElement.RootElement.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ProcessIdProperty, host.Id));
    var candidates = new List<AutomationElement>();
    foreach (AutomationElement root in roots)
        if (root.Current.Name == name) candidates.Add(root);
    candidates.AddRange(workspace.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.NameProperty, name)).Cast<AutomationElement>());
    var valid = new List<AutomationElement>();
    foreach (var candidate in candidates)
    {
        try
        {
            var current = candidate.Current;
            var handle = (nint)current.NativeWindowHandle;
            var rect = current.BoundingRectangle;
            if ((current.ControlType == ControlType.Pane || current.ControlType == ControlType.Window || current.ControlType == ControlType.ToolTip)
                && current.ProcessId == host.Id && handle != 0 && current.IsEnabled && !current.IsOffscreen
                && NativeMessage.IsVisible(handle) && NativeMessage.IsRelatedTo(handle, (nint)workspace.Current.NativeWindowHandle)
                && rect.Width > 0 && rect.Height > 0 && valid.All(item => item.Current.NativeWindowHandle != current.NativeWindowHandle))
                valid.Add(candidate);
        }
        catch (ElementNotAvailableException) { }
    }
    return valid.Count == 1 ? valid[0] : null;
}

static bool HasUtf8Bom(string path)
{
    var bytes = File.ReadAllBytes(path);
    return bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF;
}

static byte[] Utf8Bom(string source) => new byte[] { 0xEF, 0xBB, 0xBF }.Concat(Encoding.UTF8.GetBytes(source)).ToArray();

static bool HasVisibleLuaDiagnostic(AutomationElement workspace) => HasVisibleDiagnostic(workspace, "near");

static bool HasVisibleDiagnostic(AutomationElement workspace, string expectedText)
{
    return NativeMessage.DescendantsWithTitleSubstring((nint)workspace.Current.NativeWindowHandle, expectedText)
        .Select(AutomationElement.FromHandle).Any(item => item.Current.ProcessId == workspace.Current.ProcessId
            && item.Current.ControlType == ControlType.Text && item.Current.ClassName == "Static" && !item.Current.IsOffscreen);
}

static string NormalizeSource(string source) => source.Replace("\r\n", "\n", StringComparison.Ordinal).Replace('\r', '\n');

static void WaitUntil(Func<bool> check, TimeSpan timeout, string message)
{
    var timer = Stopwatch.StartNew();
    while (timer.Elapsed < timeout)
    {
        if (check()) return;
        Thread.Sleep(75);
    }
    throw new TimeoutException(message);
}

static AutomationElement? FindWindow(Process process, string title)
{
    using var timing = DriverTiming.Measure("FindWindow:" + title);
    IReadOnlyList<nint> handles;
    using (DriverTiming.Measure("FindWindow:native-enumeration:" + title))
        handles = NativeMessage.TopLevelWindows(process.Id, title);
    var matches = new List<AutomationElement>();
    using (DriverTiming.Measure("FindWindow:UIA-verification:" + title))
    {
        foreach (var handle in handles)
        {
            try
            {
                var window = AutomationElement.FromHandle(handle);
                var current = window.Current;
                if (current.ProcessId == process.Id && current.ControlType == ControlType.Window && !current.IsOffscreen
                    && (current.Name ?? "").StartsWith(title, StringComparison.OrdinalIgnoreCase))
                    matches.Add(window);
            }
            catch (ElementNotAvailableException) { }
        }
    }
    Ensure(matches.Count <= 1, $"More than one visible same-process top-level window begins with '{title}'");
    DriverTiming.Mark("FindWindow:native-top-level:" + title + ":" + matches.Count);
    return matches.Count == 1 ? matches[0] : null;
}

static AutomationElement WaitWindow(Process process, string title, TimeSpan timeout)
{
    AutomationElement? found = null;
    WaitUntil(() => (found = FindWindow(process, title)) is not null, timeout, $"Window '{title}' was not found");
    return found!;
}

static AutomationElement? FindNamed(AutomationElement root, string name)
{
    using var timing = DriverTiming.Measure("FindNamed:" + name);
    for (var attempt = 0; attempt < 2; ++attempt)
    {
        if (name == "Lua source")
        {
            var source = FindLuaSourceEditor(root);
            if (source is not null) return source;
            Thread.Sleep(50);
            continue;
        }
        if (name == "SubsEditBox")
        {
            var box = FindMainSubsEditBox(root);
            if (box is not null) return box;
            Thread.Sleep(50);
            continue;
        }
        if (name == "stcwindow" && root.Current.Name == "SubsEditBox")
        {
            AutomationElement[] candidates;
            using (DriverTiming.Measure("FindNamed:native-stcwindow:query"))
                candidates = NativeMessage.DescendantsWithTitle((nint)root.Current.NativeWindowHandle, "stcwindow")
                    .Select(AutomationElement.FromHandle).ToArray();
            AutomationElement[] editors;
            using (DriverTiming.Measure("FindNamed:native-stcwindow:validation"))
                editors = candidates.Where(item => IsStyledTextPane(item) && !item.Current.IsOffscreen
                    && item.Current.ProcessId == root.Current.ProcessId).ToArray();
            Ensure(editors.Length <= 1, $"SubsEditBox exposes {editors.Length} visible native STC editors");
            DriverTiming.Mark("FindNamed:native-stcwindow:" + editors.Length);
            if (editors.Length == 1) return editors[0];
            Thread.Sleep(50);
            continue;
        }
        var native = NativeMessage.DescendantsWithTitle((nint)root.Current.NativeWindowHandle, name)
            .Select(AutomationElement.FromHandle).Where(item => item.Current.ProcessId == root.Current.ProcessId
                && !item.Current.IsOffscreen && string.Equals(item.Current.Name, name, StringComparison.OrdinalIgnoreCase)).ToArray();
        Ensure(native.Length <= 1, $"More than one visible native element exactly matches '{name}'");
        if (native.Length == 1) return native[0];
        var element = root.FindFirst(TreeScope.Descendants,
            new PropertyCondition(AutomationElement.NameProperty, name, PropertyConditionFlags.IgnoreCase));
        if (element is not null) return element;
        Thread.Sleep(50);
    }
    return null;
}

static bool IsStyledTextPane(AutomationElement element)
{
    var current = element.Current;
    return current.ControlType == ControlType.Pane && current.ClassName == "wxWindow"
        && current.NativeWindowHandle != 0 && current.ProcessId != 0
        && (current.Name == "stcwindow" || element.TryGetCurrentPattern(ScrollPattern.Pattern, out _));
}

static AutomationElement? FindLuaSourceEditor(AutomationElement workspace)
{
    using var timing = DriverTiming.Measure("FindLuaSourceEditor:native-descendants");
    var processId = workspace.Current.ProcessId;
    var handles = new Dictionary<int, AutomationElement>();
    foreach (var handle in NativeMessage.DescendantsWithTitle((nint)workspace.Current.NativeWindowHandle, "stcwindow"))
    {
        try
        {
            var item = AutomationElement.FromHandle(handle);
            if (IsStyledTextPane(item) && item.Current.ProcessId == processId && !item.Current.IsOffscreen)
                handles.TryAdd(item.Current.NativeWindowHandle, item);
        }
        catch (ElementNotAvailableException) { }
    }
    Ensure(handles.Count <= 1, $"Workspace exposes {handles.Count} visible native Lua source STC controls");
    DriverTiming.Mark("FindLuaSourceEditor:native-descendants:" + handles.Count);
    return handles.Count == 1 ? handles.Values.Single() : null;
}

static AutomationElement? FindMainSubsEditBox(AutomationElement main)
{
    using var timing = DriverTiming.Measure("FindMainSubsEditBox:bounded-panes");
    static IEnumerable<AutomationElement> Panes(AutomationElement parent) => parent.FindAll(TreeScope.Children,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Pane)).Cast<AutomationElement>();
    var contents = Panes(main).Where(item => item.Current.Name == "panel").ToArray();
    DriverTiming.Mark("FindMainSubsEditBox:contents:" + contents.Length);
    var splitters = contents.SelectMany(Panes).Where(item => item.Current.Name == "splitter").ToArray();
    DriverTiming.Mark("FindMainSubsEditBox:splitters:" + splitters.Length);
    var editAreas = splitters.SelectMany(Panes).Where(item => item.Current.Name == "panel").ToArray();
    DriverTiming.Mark("FindMainSubsEditBox:edit-areas:" + editAreas.Length);
    var boxes = editAreas.SelectMany(Panes).Where(item => item.Current.Name == "SubsEditBox"
        && item.Current.ProcessId == main.Current.ProcessId && !item.Current.IsOffscreen).ToArray();
    Ensure(boxes.Length <= 1, $"Main edit area exposes {boxes.Length} visible SubsEditBox controls");
    DriverTiming.Mark("FindMainSubsEditBox:bounded-panes:" + boxes.Length);
    return boxes.Length == 1 ? boxes[0] : null;
}

static void InvokeButton(AutomationElement root, string name)
{
    using var timing = DriverTiming.Measure("InvokeButton:" + name);
    bool IsInvokableMatch(AutomationElement item) {
        try { return item.Current.IsEnabled && item.TryGetCurrentPattern(InvokePattern.Pattern, out _)
            && (item.Current.Name ?? "").Contains(name, StringComparison.OrdinalIgnoreCase); }
        catch (ElementNotAvailableException) { return false; }
    }
    AutomationElement? button;
    using (DriverTiming.Measure("InvokeButton:native-query-validation:" + name))
    {
        var native = NativeMessage.DescendantsWithTitle((nint)root.Current.NativeWindowHandle, name).Select(AutomationElement.FromHandle).Where(item =>
        {
            try { return item.Current.ControlType == ControlType.Button && IsInvokableMatch(item); }
            catch (ElementNotAvailableException) { return false; }
        }).ToArray();
        Ensure(native.Length <= 1, $"More than one enabled native button exactly matches '{name}'");
        if (native.Length == 1)
        {
            DriverTiming.Mark("InvokeButton:native-exact:" + name);
            UiaDriver.Invoke(native[0]);
            return;
        }
    }
    AutomationElement? toolbar;
    using (DriverTiming.Measure("InvokeButton:toolbar-discovery:" + name))
        toolbar = root.FindFirst(TreeScope.Children, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ToolBar));
    if (toolbar is not null)
    {
        AutomationElement[] toolbarButtons;
        using (DriverTiming.Measure("InvokeButton:toolbar-button-query:" + name))
            toolbarButtons = toolbar.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button))
                .Cast<AutomationElement>().ToArray();
        AutomationElement? toolbarButton;
        using (DriverTiming.Measure("InvokeButton:toolbar-validation:" + name))
            toolbarButton = toolbarButtons.FirstOrDefault(IsInvokableMatch);
        if (toolbarButton is not null)
        {
            DriverTiming.Mark("InvokeButton:toolbar:" + name);
            using (DriverTiming.Measure("InvokeButton:invoke:" + name)) UiaDriver.Invoke(toolbarButton);
            return;
        }
    }
    using (DriverTiming.Measure("InvokeButton:exact-query:" + name))
        button = root.FindFirst(TreeScope.Descendants, new AndCondition(
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button),
            new PropertyCondition(AutomationElement.NameProperty, name, PropertyConditionFlags.IgnoreCase)));
    bool valid;
    using (DriverTiming.Measure("InvokeButton:exact-validation:" + name))
        valid = button is not null && IsInvokableMatch(button);
    if (!valid)
    {
        using var fallbackTiming = DriverTiming.Measure("InvokeButton:fallback-query-validation:" + name);
        button = root.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button))
            .Cast<AutomationElement>().FirstOrDefault(IsInvokableMatch);
    }
    if (button is null) throw new InvalidOperationException($"Button '{name}' is unavailable");
    DriverTiming.Mark("InvokeButton:" + (string.Equals(button.Current.Name, name, StringComparison.OrdinalIgnoreCase) ? "exact:" : "fallback:") + name);
    using (DriverTiming.Measure("InvokeButton:invoke:" + name)) UiaDriver.Invoke(button);
}

static void InvokeKnownButton(AutomationElement button, string name)
{
    using var timing = DriverTiming.Measure("InvokeKnownButton:" + name);
    Ensure(button.Current.ControlType == ControlType.Button
        && (button.Current.Name ?? "").Contains(name, StringComparison.OrdinalIgnoreCase)
        && button.TryGetCurrentPattern(InvokePattern.Pattern, out _), $"Known button '{name}' is unavailable");
    WaitUntil(() => button.Current.IsEnabled, TimeSpan.FromSeconds(5), $"Known button '{name}' did not become enabled");
    UiaDriver.Invoke(button);
}

static void InvokeDialogStockButton(AutomationElement dialog, int stockId)
{
    var button = UiaDriver.FindDescendantByAutomationId(dialog, stockId.ToString(System.Globalization.CultureInfo.InvariantCulture), ControlType.Button)
        ?? throw new InvalidOperationException($"Expected dialog has no stock button ID {stockId}");
    Ensure(button.Current.IsEnabled && button.TryGetCurrentPattern(InvokePattern.Pattern, out _), $"Stock dialog button {stockId} cannot be invoked");
    UiaDriver.Invoke(button);
}

static void InvokeTaskDialogButton(AutomationElement dialog, params string[] observedNames)
{
    var button = dialog.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button))
        .Cast<AutomationElement>().FirstOrDefault(item => observedNames.Any(name => string.Equals(item.Current.Name, name, StringComparison.OrdinalIgnoreCase)))
        ?? throw new InvalidOperationException($"Expected TaskDialog has no observed semantic button: {string.Join('/', observedNames)}");
    Ensure(button.Current.IsEnabled && button.TryGetCurrentPattern(InvokePattern.Pattern, out _), "Observed TaskDialog button cannot be invoked");
    UiaDriver.Invoke(button);
}

static void SelectCodeViaMacro(AutomationElement main, Process host, bool first)
{
    var menu = first ? "Workspace E2E Select First Code" : "Workspace E2E Select Second Code";
    var effect = first ? "code once" : "code syl";
    InvokeMenu(main, host, "Automation", menu, TimeSpan.FromSeconds(6));
    WaitUntil(() =>
    {
        var editBox = FindNamed(main, "SubsEditBox");
        if (editBox is null) return false;
        return NativeMessage.DescendantsWithClass((nint)editBox.Current.NativeWindowHandle, "ComboBox")
            .Select(AutomationElement.FromHandle).Any(item => item.Current.ProcessId == main.Current.ProcessId
                && item.Current.ControlType == ControlType.ComboBox && !item.Current.IsOffscreen && item.Current.Name == effect);
    }, TimeSpan.FromSeconds(5), $"Selection macro did not activate {effect}");
}

static void SwitchCodeWithDecision(AutomationElement main, Process host, string decision)
{
    var switchTask = Task.Run(() => InvokeMenu(main, host, "Automation", "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(6)));
    var prompt = WaitWindow(host, "Unsaved Lua source", TimeSpan.FromSeconds(6));
    InvokeButton(prompt, decision);
    if (!switchTask.Wait(TimeSpan.FromSeconds(6))) throw new TimeoutException("Code-line switch did not resume after the dirty-source decision");
}

static AutomationElement OpenFirstCodeWorkspace(AutomationElement main, Process host)
{
    SelectCodeViaMacro(main, host, first: true);
    InvokeMenu(main, host, "Automation", "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(6));
    return WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(6));
}

static void DiscardAndCloseWorkspace(AutomationElement workspace, Process host)
{
    RequestClose(workspace);
    InvokeButton(WaitWindow(host, "Unsaved Lua source", TimeSpan.FromSeconds(5)), "Discard");
    WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(5), "Dirty Workspace did not close after Discard");
}

static void ChooseFileConflict(AutomationElement workspace, Process host, string artifacts, string artifactName, string choice, string? expectedCopied = null, Action? beforeConfirm = null, bool confirmReload = false)
{
    ClipboardReceipt.VerifyCurrent();
    var beforeSequence = GuardedKeyboard.ClipboardSequence();
    var saveTask = Task.Run(() => InvokeButton(workspace, "Save"));
    var dialog = WaitWindow(host, "Lua source conflict", TimeSpan.FromSeconds(6));
    SaveNativeDialogEvidence(dialog, artifacts, artifactName);
    var option = dialog.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ListItem))
        .Cast<AutomationElement>().FirstOrDefault(item => item.Current.Name == choice)
        ?? throw new InvalidOperationException($"File conflict has no exact option '{choice}'");
    if (!option.TryGetCurrentPattern(SelectionItemPattern.Pattern, out var selection))
        throw new InvalidOperationException("File conflict option is not UIA-selectable");
    ((SelectionItemPattern)selection).Select();
    beforeConfirm?.Invoke();
    var confirm = UiaDriver.FindDescendantByAutomationId(dialog, "5100", ControlType.Button)
        ?? throw new InvalidOperationException("File conflict has no observed OK button ID 5100");
    if (expectedCopied is not null)
        ClipboardReceipt.VerifyCurrent();
    if (confirmReload)
    {
        var confirmTask = Task.Run(() => UiaDriver.Invoke(confirm));
        var reload = WaitWindow(host, "Reload Lua source", TimeSpan.FromSeconds(5));
        SaveNativeDialogEvidence(reload, artifacts, artifactName + "-reload-confirm");
        InvokeTaskDialogButton(reload, "是(Y)", "Yes");
        if (!confirmTask.Wait(TimeSpan.FromSeconds(6))) throw new TimeoutException("Conflict confirmation did not resume after Reload decision");
    }
    else UiaDriver.Invoke(confirm);
    if (!saveTask.Wait(TimeSpan.FromSeconds(6))) throw new TimeoutException("File conflict choice did not complete");
    if (expectedCopied is not null)
    {
        WaitUntil(() => GuardedKeyboard.ClipboardSequence() != beforeSequence, TimeSpan.FromSeconds(3), "Copy-source conflict option did not update clipboard");
        var copiedSequence = GuardedKeyboard.ClipboardSequence();
        GuardedKeyboard.AssertClipboardState(copiedSequence, host.Id);
        var copied = ClipboardSafety.ReadText();
        GuardedKeyboard.AssertClipboardState(copiedSequence, host.Id);
        ClipboardReceipt.Record("conflict-copy-source", host.Id, copiedSequence);
        Ensure(NormalizeSource(copied) == NormalizeSource(expectedCopied), "Copy-source conflict option did not copy the complete editor source");
    }
}

static void OpenLuaFile(AutomationElement workspace, Process host, string path, string artifacts)
{
    var timings = new List<string>();
    var elapsed = Stopwatch.StartNew();
    var openTask = Task.Run(() => InvokeButton(workspace, "Open File"));
    timings.Add($"invoke-dispatched-ms={elapsed.ElapsedMilliseconds}");
    try
    {
        var dialog = WaitOwnedModalWindow(workspace, host, "Open Lua source", TimeSpan.FromSeconds(6));
        timings.Add($"modal-found-ms={elapsed.ElapsedMilliseconds}");
        ScreenCapture.SaveWindowPng(dialog, Path.Combine(artifacts, "open-file-dialog.png"));
        var filename = FindNativeDialogControl(dialog, 1148, ControlType.Edit);
        timings.Add($"filename-found-ms={elapsed.ElapsedMilliseconds}");
        if (filename is null || !filename.TryGetCurrentPattern(ValuePattern.Pattern, out var pattern))
            throw new InvalidOperationException("Open Lua source dialog has no UIA File name ValuePattern");
        ((ValuePattern)pattern).SetValue(path);
        timings.Add($"filename-set-ms={elapsed.ElapsedMilliseconds}");
        var open = FindNativeDialogControl(dialog, 1, ControlType.Button)
            ?? throw new InvalidOperationException("Open Lua source dialog has no native stock button ID 1");
        Ensure(open.Current.IsEnabled && open.TryGetCurrentPattern(InvokePattern.Pattern, out _), "Open Lua source stock button cannot be invoked");
        UiaDriver.Invoke(open);
        timings.Add($"stock-button-invoked-ms={elapsed.ElapsedMilliseconds}");
        if (!openTask.Wait(TimeSpan.FromSeconds(6))) throw new TimeoutException("Open Lua source dialog did not complete");
        timings.Add($"open-task-completed-ms={elapsed.ElapsedMilliseconds}");
    }
    finally
    {
        File.WriteAllLines(Path.Combine(artifacts, "open-file-timing.txt"), timings);
    }
}

static AutomationElement WaitOwnedModalWindow(AutomationElement owner, Process host, string title, TimeSpan timeout)
{
    var ownerHandle = (nint)owner.Current.NativeWindowHandle;
    if (ownerHandle == 0) throw new InvalidOperationException("Lua Workspace has no native window handle");
    AutomationElement? found = null;
    WaitUntil(() =>
    {
        var popup = NativeMessage.EnabledPopup(ownerHandle);
        if (popup == 0 || popup == ownerHandle || !NativeMessage.IsVisible(popup) || NativeMessage.ProcessId(popup) != host.Id)
            return false;
        var candidate = AutomationElement.FromHandle(popup);
        if (candidate.Current.ControlType != ControlType.Window || candidate.Current.ProcessId != host.Id || candidate.Current.IsOffscreen
            || !(candidate.Current.Name ?? "").StartsWith(title, StringComparison.OrdinalIgnoreCase))
            return false;
        found = candidate;
        return true;
    }, timeout, $"Owned modal window '{title}' was not found");
    return found!;
}

static AutomationElement? FindNativeDialogControl(AutomationElement dialog, int controlId, ControlType type)
{
    foreach (var handle in NativeMessage.DescendantsWithControlId((nint)dialog.Current.NativeWindowHandle, controlId))
    {
        var candidate = AutomationElement.FromHandle(handle);
        if (candidate.Current.ControlType == type && candidate.Current.AutomationId == controlId.ToString(System.Globalization.CultureInfo.InvariantCulture))
            return candidate;
    }
    return null;
}

static void OpenAssWithDirtyDecision(AutomationElement main, Process host, string artifacts, string? path, bool expectLoadError = false)
{
    var open = UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5001", "Open a subtitles file")
        ?? throw new InvalidOperationException("Main-window Open Subtitles toolbar button is unavailable");
    var openTask = Task.Run(() => UiaDriver.Invoke(open));
    InvokeButton(WaitWindow(host, "Unsaved Lua source", TimeSpan.FromSeconds(6)), "Discard");
    var picker = WaitWindow(host, "Open Subtitles", TimeSpan.FromSeconds(6));
    SaveNativeDialogEvidence(picker, artifacts, path is null ? "ass-picker-cancel" : expectLoadError ? "ass-picker-invalid" : "ass-picker-valid");
    if (path is null)
    {
        InvokeDialogStockButton(picker, 2);
    }
    else
    {
        var filename = UiaDriver.FindDescendantByAutomationId(picker, "1148", ControlType.Edit);
        if (filename is null || !filename.TryGetCurrentPattern(ValuePattern.Pattern, out var value))
            throw new InvalidOperationException("Open Subtitles picker has no File name ValuePattern");
        ((ValuePattern)value).SetValue(path);
        InvokeDialogStockButton(picker, 1);
        if (expectLoadError)
        {
            var error = WaitWindow(host, "Error", TimeSpan.FromSeconds(6));
            SaveNativeDialogEvidence(error, artifacts, "ass-load-error");
            InvokeTaskDialogButton(error, "OK");
        }
    }
    if (!openTask.Wait(TimeSpan.FromSeconds(8))) throw new TimeoutException("Open Subtitles did not finish after the picker decision");
}

static void InvokeMenu(AutomationElement main, Process process, string menuName, string itemName, TimeSpan timeout)
{
    using var timing = DriverTiming.Measure("InvokeMenu:" + menuName + "/" + itemName);
    var bars = main.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuBar))
        .Cast<AutomationElement>();
    AutomationElement? menu = null;
    foreach (var bar in bars)
    {
        menu = bar.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem))
            .Cast<AutomationElement>().FirstOrDefault(item => string.Equals((item.Current.Name ?? "").Replace("&", ""), menuName, StringComparison.OrdinalIgnoreCase));
        if (menu is not null) break;
    }
    if (menu is null) throw new InvalidOperationException($"Application menu '{menuName}' is unavailable");
    var opened = false;
    if (menu.TryGetCurrentPattern(ExpandCollapsePattern.Pattern, out var expand))
    {
        try { ((ExpandCollapsePattern)expand).Expand(); opened = true; }
        catch (InvalidOperationException) { }
    }
    if (!opened && menu.TryGetCurrentPattern(InvokePattern.Pattern, out var invoke))
    {
        ((InvokePattern)invoke).Invoke();
        opened = true;
    }
    if (!opened && menuName.Equals("Automation", StringComparison.OrdinalIgnoreCase))
    {
        NativeMessage.PostCommand(new nint(main.Current.NativeWindowHandle), 0x0112, 0xF100, 'u');
        opened = true;
    }
    if (!opened && menuName.Equals("File", StringComparison.OrdinalIgnoreCase))
    {
        NativeMessage.PostCommand(new nint(main.Current.NativeWindowHandle), 0x0112, 0xF100, 'f');
        opened = true;
    }
    if (!opened && menuName.Equals("Edit", StringComparison.OrdinalIgnoreCase))
    {
        NativeMessage.PostCommand(new nint(main.Current.NativeWindowHandle), 0x0112, 0xF100, 'e');
        opened = true;
    }
    if (!opened) throw new InvalidOperationException($"Menu '{menuName}' cannot be opened by UIA or its own window message");
    AutomationElement? command = null;
    WaitUntil(() =>
    {
        command = menu.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem))
            .Cast<AutomationElement>().FirstOrDefault(item => (item.Current.Name ?? "").Replace("&", "").Contains(itemName, StringComparison.OrdinalIgnoreCase));
        if (command is not null) return true;
        var windows = AutomationElement.RootElement.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ProcessIdProperty, process.Id));
        foreach (AutomationElement window in windows)
        {
            if (window.Current.ControlType != ControlType.Menu && window.Current.ClassName != "#32768") continue;
            command = window.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem))
                .Cast<AutomationElement>().FirstOrDefault(item => (item.Current.Name ?? "").Replace("&", "").Contains(itemName, StringComparison.OrdinalIgnoreCase));
            if (command is not null) return true;
        }
        return false;
    }, timeout, $"Menu command '{itemName}' is unavailable");
    UiaDriver.Invoke(command!);
}

static string ReadEditor(AutomationElement editor)
{
    using var timing = DriverTiming.Measure("ReadEditor");
    var hwnd = new nint(editor.Current.NativeWindowHandle);
    Ensure(hwnd != 0, "Editor has no UIA value or native window");
    if (!IsStyledTextPane(editor))
    {
        if (editor.TryGetCurrentPattern(ValuePattern.Pattern, out var value))
            return ((ValuePattern)value).Current.Value;
        return NativeMessage.GetWindowText(hwnd);
    }
    var root = editor;
    while (TreeWalker.ControlViewWalker.GetParent(root) is { } parent && parent.Current.ControlType != ControlType.Window)
        root = parent;
    var frame = TreeWalker.ControlViewWalker.GetParent(root) ?? root;
    var copy = frame.FindFirst(TreeScope.Descendants, new AndCondition(
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button),
        new PropertyCondition(AutomationElement.NameProperty, "Copy source")))
        ?? throw new InvalidOperationException("Lua source has no readable UIA or clipboard path");
    Ensure(copy.Current.IsEnabled && copy.TryGetCurrentPattern(InvokePattern.Pattern, out _), "Lua source Copy source button is not invokable");
    ClipboardReceipt.VerifyCurrent();
    var previousSequence = GuardedKeyboard.ClipboardSequence();
    Console.WriteLine("editor-uia.clipboard-stage=invoke");
    UiaDriver.Invoke(copy);
    WaitUntil(() => GuardedKeyboard.ClipboardSequence() != previousSequence, TimeSpan.FromSeconds(3), "Copy source did not change the clipboard");
    var copiedSequence = GuardedKeyboard.ClipboardSequence();
    GuardedKeyboard.AssertClipboardState(copiedSequence, editor.Current.ProcessId);
    var copied = ClipboardSafety.ReadText();
    GuardedKeyboard.AssertClipboardState(copiedSequence, editor.Current.ProcessId);
    ClipboardReceipt.Record("copy-source", editor.Current.ProcessId, copiedSequence);
    Console.WriteLine($"editor-uia.clipboard-stage=host-read;length={copied.Length};sha256={Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(copied)))}");
    return copied;
}

static void WriteEditor(AutomationElement editor, string text, Process host)
{
    using var timing = DriverTiming.Measure("WriteEditor");
    if (IsStyledTextPane(editor))
    {
        GuardedKeyboard.ReplaceWithClipboard(editor, host, text);
        return;
    }
    if (editor.TryGetCurrentPattern(ValuePattern.Pattern, out var value) && !((ValuePattern)value).Current.IsReadOnly)
    {
        ((ValuePattern)value).SetValue(text);
        return;
    }
    var hwnd = new nint(editor.Current.NativeWindowHandle);
    Ensure(hwnd != 0, "Editor cannot be changed via UIA or native HWND");
    Ensure(NativeMessage.TrySetWindowText(hwnd, text), "WM_SETTEXT is unsupported by this edit control");
}

static void RequestClose(AutomationElement window)
{
    var hwnd = new nint(window.Current.NativeWindowHandle);
    Ensure(hwnd != 0, "Workspace has no native HWND");
    NativeMessage.Post(hwnd, 0x0010);
}

static int ParseAssTime(string text)
{
    var parts = text.Split(':', '.');
    if (parts.Length != 4) throw new FormatException("ASS time field is malformed");
    return ((int.Parse(parts[0]) * 60 + int.Parse(parts[1])) * 60 + int.Parse(parts[2])) * 1000 + int.Parse(parts[3]) * 10;
}

static List<AssEvent> EventLines(string ass) => ass.Split('\n').Select(line => line.TrimEnd('\r'))
    .Where(line => line.StartsWith("Comment:", StringComparison.Ordinal) || line.StartsWith("Dialogue:", StringComparison.Ordinal))
    .Select(line =>
    {
        var separator = line.IndexOf(':');
        var fields = line[(separator + 1)..].TrimStart().Split(',', 10);
        if (fields.Length != 10) throw new FormatException("ASS event field count changed");
        return new AssEvent(line[..separator], int.Parse(fields[0]), ParseAssTime(fields[1]), ParseAssTime(fields[2]), fields[3].Trim(), fields[4].Trim(), int.Parse(fields[5]), int.Parse(fields[6]), int.Parse(fields[7]), fields[8].Trim(), fields[9]);
    }).ToList();

static void AssertPhysicalEventLines(string ass, int expectedEvents)
{
    var lines = ass.Split('\n').Select(line => line.TrimEnd('\r')).ToArray();
    var section = Array.FindIndex(lines, line => line == "[Events]");
    if (section < 0) throw new FormatException("Saved ASS has no Events section");
    var events = 0;
    foreach (var line in lines.Skip(section + 1))
    {
        if (line.Length == 0) continue;
        if (line.StartsWith('[')) break;
        if (line.StartsWith("Format:", StringComparison.Ordinal)) continue;
        if (!line.StartsWith("Comment:", StringComparison.Ordinal) && !line.StartsWith("Dialogue:", StringComparison.Ordinal))
            throw new FormatException("Saved ASS contains an orphan physical continuation line");
        ++events;
    }
    if (events != expectedEvents) throw new FormatException("Saved ASS physical event count changed");
}

static void SaveUiEvidence(AutomationElement window, string artifacts, string name)
{
    using var timing = DriverTiming.Measure("SaveUiEvidence:" + name);
    var nodes = new List<string>();
    foreach (AutomationElement element in window.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().Take(400))
    {
        try
        {
            var current = element.Current;
            nodes.Add($"{current.ControlType.ProgrammaticName}\t{current.Name}\t{current.AutomationId}\t{current.ClassName}\t{current.NativeWindowHandle}\t{current.IsEnabled}\t{string.Join(',', element.GetSupportedPatterns().Select(pattern => pattern.ProgrammaticName))}");
        }
        catch (ElementNotAvailableException) { nodes.Add("<element became unavailable during bounded evidence capture>"); }
    }
    File.WriteAllLines(Path.Combine(artifacts, name + "-uia.txt"), nodes);
    ScreenCapture.SaveWindowPng(window, Path.Combine(artifacts, name + ".png"));
}

static void SaveNativeDialogEvidence(AutomationElement window, string artifacts, string name)
{
    using var timing = DriverTiming.Measure("SaveNativeDialogEvidence:" + name);
    File.WriteAllLines(Path.Combine(artifacts, name + "-native.txt"), NativeMessage.DescribeDescendants((nint)window.Current.NativeWindowHandle));
    ScreenCapture.SaveWindowPng(window, Path.Combine(artifacts, name + ".png"));
}

static void TryEvidence(AutomationElement window, string artifacts, string name)
{
    try { SaveUiEvidence(window, artifacts, name); } catch { }
}

sealed record AssEvent(string Kind, int Layer, int StartMs, int EndMs, string Style, string Name, int MarginL, int MarginR, int MarginV, string Effect, string Text);
sealed record StepResult(string Name, string Status, string Detail);
sealed record ClipboardReceiptData(uint Sequence, int OwnerProcessId, string Stage);
sealed record LanguageSettingControls(AutomationElement EnabledElement, TogglePattern Enabled, AutomationElement DirectoryElement, ValuePattern Directory);

static class DriverTiming
{
    private static readonly object gate = new();
    private static readonly Stopwatch clock = new();
    private static string? path;

    public static void Start(string artifacts)
    {
        path = Path.Combine(artifacts, "editor-timing.tsv");
        File.WriteAllText(path, "utc\telapsed_ms\tevent\toperation\n");
        clock.Restart();
    }

    public static IDisposable Measure(string operation)
    {
        if (path is null) return new Scope(null);
        Record("begin", operation);
        return new Scope(operation);
    }

    public static void Mark(string operation)
    {
        if (path is not null) Record("mark", operation);
    }

    private static void Record(string kind, string operation)
    {
        lock (gate)
            File.AppendAllText(path!, $"{DateTimeOffset.UtcNow:O}\t{clock.ElapsedMilliseconds}\t{kind}\t{operation}\n");
    }

    private sealed class Scope(string? operation) : IDisposable
    {
        public void Dispose()
        {
            if (operation is not null) Record("end", operation);
        }
    }
}

static class ClipboardReceipt
{
    public static string? PathName { get; set; }
    private static uint expectedSequence;

    public static void Initialize(uint initialSequence) => expectedSequence = initialSequence;

    public static void VerifyCurrent()
    {
        if (GuardedKeyboard.ClipboardSequence() != expectedSequence)
            throw new InvalidOperationException("Clipboard changed outside this GUI test; preserving external data");
    }

    public static void Record(string stage, int ownerProcessId, uint verifiedSequence)
    {
        if (PathName is null) throw new InvalidOperationException("Clipboard receipt path was not initialized");
        GuardedKeyboard.AssertClipboardState(verifiedSequence, ownerProcessId);
        var receipt = new ClipboardReceiptData(verifiedSequence, ownerProcessId, stage);
        var pending = PathName + ".pending";
        File.WriteAllText(pending, JsonSerializer.Serialize(receipt));
        File.Move(pending, PathName, overwrite: true);
        GuardedKeyboard.AssertClipboardState(verifiedSequence, ownerProcessId);
        expectedSequence = receipt.Sequence;
    }

    public static ClipboardReceiptData? Read(string path)
    {
        if (!File.Exists(path)) return null;
        return JsonSerializer.Deserialize<ClipboardReceiptData>(File.ReadAllText(path));
    }
}

static class ClipboardSafety
{
    public static T OnSta<T>(Func<T> operation)
        => ClipboardSta.Invoke(operation);

    public static string ReadText()
    {
        var sequence = GuardedKeyboard.ClipboardSequence();
        var owner = GuardedKeyboard.ClipboardOwnerPid();
        return ClipboardText.ReadUnicodeText(sequence, owner);
    }
    public static uint SetText(string text) => OnSta(() =>
    {
        Clipboard.SetText(text);
        var sequence = GuardedKeyboard.ClipboardSequence();
        ClipboardReceipt.Record("test-text", Environment.ProcessId, sequence);
        return sequence;
    });
    public static bool Restore(DataObject? snapshot, uint expectedSequence) => OnSta(() =>
    {
        if (GuardedKeyboard.ClipboardSequence() != expectedSequence)
            throw new InvalidOperationException("Clipboard changed before restoration; external data was preserved");
        if (snapshot is not null) Clipboard.SetDataObject(snapshot, true);
        else Clipboard.Clear();
        return true;
    });

    public static DataObject? Capture()
    {
        var current = Clipboard.GetDataObject();
        if (current is null) return null;
        var snapshot = new DataObject();
        foreach (var format in current.GetFormats(autoConvert: false))
        {
            var value = current.GetData(format, autoConvert: false)
                ?? throw new InvalidOperationException("An existing clipboard format could not be materialized");
            object independent = value switch
            {
                string text => text,
                byte[] bytes => bytes.ToArray(),
                string[] paths => paths.ToArray(),
                StringCollection strings => CopyStrings(strings),
                Image image => new Bitmap(image),
                Stream stream => CopyStream(stream),
                _ => throw new NotSupportedException("An existing clipboard format cannot be safely materialized")
            };
            snapshot.SetData(format, autoConvert: false, independent);
        }
        return snapshot;
    }

    private static StringCollection CopyStrings(StringCollection source)
    {
        var result = new StringCollection();
        result.AddRange(source.Cast<string>().ToArray());
        return result;
    }

    private static MemoryStream CopyStream(Stream source)
    {
        var copy = new MemoryStream();
        var position = source.CanSeek ? source.Position : (long?)null;
        if (source.CanSeek) source.Position = 0;
        try { source.CopyTo(copy); }
        finally { if (position is not null) source.Position = position.Value; }
        copy.Position = 0;
        return copy;
    }
}

static class ClipboardReadbackDiagnostic
{
    [DllImport("user32.dll", SetLastError = true)] private static extern int OpenClipboard(nint owner);
    [DllImport("user32.dll", SetLastError = true)] private static extern int CloseClipboard();
    [DllImport("user32.dll", SetLastError = true)] private static extern nint GetClipboardData(uint format);
    [DllImport("user32.dll")] private static extern uint GetClipboardSequenceNumber();
    [DllImport("user32.dll")] private static extern nint GetClipboardOwner();
    [DllImport("user32.dll")] private static extern nint GetOpenClipboardWindow();
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(nint hwnd, out uint processId);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern nint GlobalLock(nint memory);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern int GlobalUnlock(nint memory);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern nuint GlobalSize(nint memory);

    public static void Write(string expected, string actual, uint sequence)
    {
        var directory = Path.GetDirectoryName(ClipboardReceipt.PathName ?? throw new InvalidOperationException("Clipboard receipt path is missing"))!;
        object observation;
        try { observation = ClipboardSafety.OnSta(() => Observe(sequence)); }
        catch (Exception error) { observation = new { Error = error.GetType().Name + ": " + error.Message }; }
        var limit = Math.Min(expected.Length, actual.Length);
        var firstDifference = 0;
        while (firstDifference < limit && expected[firstDifference] == actual[firstDifference]) ++firstDifference;
        if (firstDifference == limit && expected.Length == actual.Length) firstDifference = -1;
        File.WriteAllText(Path.Combine(directory, "paste-readback-mismatch.json"), JsonSerializer.Serialize(new
        {
            ExpectedUtf16Length = expected.Length,
            ReadUtf16Length = actual.Length,
            ExpectedSha256 = Hash(expected),
            ReadSha256 = Hash(actual),
            FirstDifferentUtf16Unit = firstDifference,
            Sequence = sequence,
            OwnerPid = ProcessId(GetClipboardOwner()),
            OpenPid = ProcessId(GetOpenClipboardWindow()),
            Observation = observation
        }, new JsonSerializerOptions { WriteIndented = true }));
    }

    private static object Observe(uint sequence)
    {
        if (!Owned(sequence)) return new { Status = "ownership-changed" };
        object typed;
        object raw;
        var data = Clipboard.GetDataObject();
        try
        {
            string? value = null;
            var success = data is not null && data.TryGetData(DataFormats.UnicodeText, autoConvert: false, out value);
            typed = new { Success = success, Utf16Length = success ? (int?)value?.Length : null,
                Sha256 = success && value is not null ? Hash(value) : null };
        }
        catch (Exception error) { typed = new { Error = error.GetType().Name }; }
        try
        {
            var value = data?.GetData(DataFormats.UnicodeText, autoConvert: false);
            raw = value switch
            {
                string text => new { Type = "String", Utf16Length = (int?)text.Length, Sha256 = Hash(text) },
                byte[] bytes => new { Type = "ByteArray", ByteLength = (int?)bytes.Length,
                    Sha256 = Convert.ToHexString(SHA256.HashData(bytes)) },
                _ => new { Type = value?.GetType().Name ?? "null" }
            };
        }
        catch (Exception error) { raw = new { Error = error.GetType().Name }; }
        var native = ReadNative(sequence);
        return Owned(sequence) ? new { Status = "observed", Typed = typed, Raw = raw, Native = native }
            : new { Status = "ownership-changed" };
    }

    private static object ReadNative(uint sequence)
    {
        if (!Owned(sequence)) return new { Status = "ownership-changed" };
        if (OpenClipboard(0) == 0) return new { Status = "open-failed", Error = Marshal.GetLastPInvokeError() };
        object result;
        try { result = ReadOpenedNative(); }
        finally { _ = CloseClipboard(); }
        return Owned(sequence) ? result : new { Status = "ownership-changed" };
    }

    private static object ReadOpenedNative()
    {
        var memory = GetClipboardData(13);
        if (memory == 0) return new { Status = "get-data-failed", Error = Marshal.GetLastPInvokeError() };
        var size = GlobalSize(memory);
        if (size == 0 || size > 8 * 1024 * 1024) return new { Status = "invalid-size", ByteLength = size <= int.MaxValue ? (int?)size : null };
        var locked = GlobalLock(memory);
        if (locked == 0) return new { Status = "lock-failed", Error = Marshal.GetLastPInvokeError() };
        try
        {
            var bytes = new byte[(int)size];
            Marshal.Copy(locked, bytes, 0, bytes.Length);
            int? firstNul = null;
            for (var i = 0; i + 1 < bytes.Length; i += 2)
                if (bytes[i] == 0 && bytes[i + 1] == 0) { firstNul = i / 2; break; }
            var prefixBytes = firstNul is { } units ? bytes.AsSpan(0, units * 2) : bytes.AsSpan(0, bytes.Length - bytes.Length % 2);
            return new { Status = "read", ByteLength = bytes.Length, FirstNulUtf16Unit = firstNul,
                Sha256 = Convert.ToHexString(SHA256.HashData(bytes)),
                PrefixSha256 = Hash(Encoding.Unicode.GetString(prefixBytes)) };
        }
        finally { _ = GlobalUnlock(memory); }
    }

    private static bool Owned(uint sequence) => GetClipboardSequenceNumber() == sequence && ProcessId(GetClipboardOwner()) == Environment.ProcessId;
    private static int ProcessId(nint handle)
    {
        if (handle == 0) return 0;
        GetWindowThreadProcessId(handle, out var processId);
        return checked((int)processId);
    }
    private static string Hash(string value) => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(value)));
}

static class NativeProcess
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct ProcessEntry
    {
        public uint Size;
        public uint Usage;
        public uint ProcessId;
        public nint DefaultHeap;
        public uint ModuleId;
        public uint Threads;
        public uint ParentProcessId;
        public int PriorityBase;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string ExeFile;
    }

    [DllImport("kernel32.dll", EntryPoint = "CreateToolhelp32Snapshot", ExactSpelling = true, SetLastError = true)]
    private static extern nint CreateSnapshot(uint flags, uint processId);

    [DllImport("kernel32.dll", EntryPoint = "Process32FirstW", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int ProcessFirst(nint snapshot, ref ProcessEntry entry);

    [DllImport("kernel32.dll", EntryPoint = "Process32NextW", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int ProcessNext(nint snapshot, ref ProcessEntry entry);

    [DllImport("kernel32.dll", EntryPoint = "OpenProcess", ExactSpelling = true, SetLastError = true)]
    private static extern nint OpenProcess(uint access, int inheritHandle, uint processId);

    [DllImport("kernel32.dll", EntryPoint = "QueryFullProcessImageNameW", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int QueryFullProcessImageName(nint process, uint flags, StringBuilder path, ref uint size);

    [DllImport("kernel32.dll", EntryPoint = "CloseHandle", ExactSpelling = true)]
    private static extern int CloseHandle(nint handle);

    public static IReadOnlyList<string> DirectChildExecutablePaths(int parentProcessId)
    {
        var snapshot = CreateSnapshot(0x00000002, 0);
        if (snapshot == new nint(-1)) throw new InvalidOperationException("Could not enumerate child processes");
        try
        {
            var children = new List<uint>();
            var entry = new ProcessEntry { Size = checked((uint)Marshal.SizeOf<ProcessEntry>()), ExeFile = "" };
            if (ProcessFirst(snapshot, ref entry) != 0)
            {
                do
                {
                    if (entry.ParentProcessId == checked((uint)parentProcessId)) children.Add(entry.ProcessId);
                    entry.Size = checked((uint)Marshal.SizeOf<ProcessEntry>());
                }
                while (ProcessNext(snapshot, ref entry) != 0);
            }
            return children.Select(TryExecutablePath).Where(path => path is not null).Cast<string>().ToArray();
        }
        finally { _ = CloseHandle(snapshot); }
    }

    private static string? TryExecutablePath(uint processId)
    {
        var process = OpenProcess(0x1000, 0, processId);
        if (process == 0) return null;
        try
        {
            var capacity = 32768u;
            var path = new StringBuilder(checked((int)capacity));
            return QueryFullProcessImageName(process, 0, path, ref capacity) != 0 ? path.ToString() : null;
        }
        finally { _ = CloseHandle(process); }
    }
}

static class GuardedKeyboard
{
    [StructLayout(LayoutKind.Sequential)] private struct Input { public uint Type; public InputUnion Union; }
    [StructLayout(LayoutKind.Explicit)] private struct InputUnion { [FieldOffset(0)] public MouseInput Mouse; [FieldOffset(0)] public KeyboardInput Keyboard; }
    [StructLayout(LayoutKind.Sequential)] private struct MouseInput { public int X, Y; public uint Data, Flags, Time; public nint ExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] private struct KeyboardInput { public ushort VirtualKey, ScanCode; public uint Flags, Time; public nint ExtraInfo; }

    [DllImport("user32.dll", SetLastError = true)] private static extern uint SendInput(uint count, Input[] inputs, int size);
    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
    [DllImport("user32.dll")] private static extern uint GetClipboardSequenceNumber();
    [DllImport("user32.dll")] private static extern nint GetClipboardOwner();
    [DllImport("user32.dll")] private static extern nint GetOpenClipboardWindow();
    [DllImport("user32.dll")] private static extern int IsClipboardFormatAvailable(uint format);
    [DllImport("user32.dll", SetLastError = true)] private static extern uint GetWindowThreadProcessId(nint hwnd, out uint processId);

    public static uint ClipboardSequence() => GetClipboardSequenceNumber();

    public static int ClipboardOwnerPid() => checked((int)ProcessIdForWindow(GetClipboardOwner()));

    private static uint ProcessIdForWindow(nint window)
    {
        if (window == 0) return 0;
        GetWindowThreadProcessId(window, out var processId);
        return processId;
    }

    public static void AssertClipboardOwner(int processId)
    {
        GetWindowThreadProcessId(GetClipboardOwner(), out var owner);
        if (owner != (uint)processId) throw new InvalidOperationException($"Clipboard owner PID {owner} differs from expected PID {processId}");
    }

    public static void AssertClipboardState(uint sequence, int processId)
    {
        if (ClipboardSequence() != sequence) throw new InvalidOperationException("Clipboard changed during receipt publication");
        AssertClipboardOwner(processId);
        if (ClipboardSequence() != sequence) throw new InvalidOperationException("Clipboard changed during owner verification");
    }

    public static void StabilizeOwnedClipboard(int hostProcessId)
    {
        if (ClipboardReceipt.PathName is null) return;
        var receipt = ClipboardReceipt.Read(ClipboardReceipt.PathName);
        if (receipt is null || receipt.OwnerProcessId != hostProcessId) return;
        ClipboardReceipt.VerifyCurrent();
        AssertClipboardState(receipt.Sequence, hostProcessId);
        var text = ClipboardSafety.ReadText();
        AssertClipboardState(receipt.Sequence, hostProcessId);
        var persistedSequence = ClipboardSafety.SetText(text);
        if (ClipboardSequence() != persistedSequence)
            throw new InvalidOperationException("Clipboard changed while persisting verified GUI-host text before shutdown");
    }

    private static void PrepareFocus(AutomationElement editor, Process host)
    {
        var frame = editor;
        while (frame.Current.ControlType != ControlType.Window)
            frame = TreeWalker.ControlViewWalker.GetParent(frame) ?? throw new InvalidOperationException("Editor has no top-level frame");
        UiaDriver.FocusAndVerify(frame, host, TimeSpan.FromSeconds(3));
        editor.SetFocus();
        var timer = Stopwatch.StartNew();
        while (timer.Elapsed < TimeSpan.FromSeconds(3))
        {
            var foreground = GetForegroundWindow();
            GetWindowThreadProcessId(foreground, out var owner);
            if (owner == (uint)host.Id && AutomationElement.FocusedElement.Current.NativeWindowHandle == editor.Current.NativeWindowHandle)
                return;
            Thread.Sleep(50);
        }
        throw new TimeoutException("Guarded input target did not gain exact foreground focus");
    }

    public static void FocusEditor(AutomationElement editor, Process host) => PrepareFocus(editor, host);

    private static void AssertOwnFocus(AutomationElement editor, Process host)
    {
        GetWindowThreadProcessId(GetForegroundWindow(), out var owner);
        if (owner != (uint)host.Id || AutomationElement.FocusedElement.Current.NativeWindowHandle != editor.Current.NativeWindowHandle)
            throw new InvalidOperationException("Guarded keyboard input stopped after focus left its GUI host editor");
    }

    public static void SendChord(AutomationElement editor, Process host, char letter)
        => SendChord(editor, host, (ushort)char.ToUpperInvariant(letter));

    public static void SendChord(AutomationElement editor, Process host, ushort virtualKey, bool shift = false)
    {
        AssertOwnFocus(editor, host);
        Input Key(ushort key, bool up) => new() { Type = 1, Union = new InputUnion { Keyboard = new KeyboardInput { VirtualKey = key, Flags = (key == 0x23 ? 1u : 0u) | (up ? 2u : 0u) } } };
        var inputs = shift
            ? new[] { Key(0x11, false), Key(0x10, false), Key(virtualKey, false), Key(virtualKey, true), Key(0x10, true), Key(0x11, true) }
            : new[] { Key(0x11, false), Key(virtualKey, false), Key(virtualKey, true), Key(0x11, true) };
        if (SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<Input>()) != inputs.Length)
        {
            var release = shift ? new[] { Key(virtualKey, true), Key(0x10, true), Key(0x11, true) }
                : new[] { Key(virtualKey, true), Key(0x11, true) };
            var released = SendInput((uint)release.Length, release, Marshal.SizeOf<Input>());
            throw new InvalidOperationException(released == release.Length
                ? "Guarded SendInput was partial; modifiers were released"
                : "Guarded SendInput was partial and modifier release was incomplete");
        }
    }

    public static void SendKey(AutomationElement editor, Process host, ushort virtualKey)
    {
        AssertOwnFocus(editor, host);
        var extended = virtualKey == 0x23 ? 1u : 0u;
        Input Key(bool up) => new() { Type = 1, Union = new InputUnion { Keyboard = new KeyboardInput { VirtualKey = virtualKey, Flags = extended | (up ? 2u : 0u) } } };
        var keys = new[] { Key(false), Key(true) };
        if (SendInput(2, keys, Marshal.SizeOf<Input>()) != 2)
        {
            _ = SendInput(1, new[] { Key(true) }, Marshal.SizeOf<Input>());
            throw new InvalidOperationException("Guarded key input was partial");
        }
    }

    public static void TypeText(AutomationElement editor, Process host, string text)
    {
        AssertOwnFocus(editor, host);
        foreach (var character in text)
        {
            Input Key(bool up) => new() { Type = 1, Union = new InputUnion { Keyboard = new KeyboardInput { VirtualKey = 0, ScanCode = character, Flags = 4u | (up ? 2u : 0u) } } };
            if (SendInput(2, new[] { Key(false), Key(true) }, Marshal.SizeOf<Input>()) != 2)
            {
                _ = SendInput(1, new[] { Key(true) }, Marshal.SizeOf<Input>());
                throw new InvalidOperationException("Guarded Unicode typing was partial");
            }
        }
    }

    public static void ReplaceWithClipboard(AutomationElement editor, Process host, string text)
    {
        PrepareFocus(editor, host);
        ClipboardReceipt.VerifyCurrent();
        Console.WriteLine("editor-uia.paste-stage=prepare");
        var clipboardSequence = ClipboardSafety.SetText(text);
        if (ClipboardSequence() != clipboardSequence) throw new InvalidOperationException("Clipboard changed after publishing this test's paste text");
        if (IsClipboardFormatAvailable(13) == 0) throw new InvalidOperationException("Published paste text has no CF_UNICODETEXT format");
        var published = ClipboardSafety.ReadText();
        if (ClipboardSequence() != clipboardSequence) throw new InvalidOperationException("Clipboard changed while verifying published paste text");
        if (published != text)
        {
            try { ClipboardReadbackDiagnostic.Write(text, published, clipboardSequence); }
            catch (Exception error) { Console.Error.WriteLine($"editor-uia.paste-readback-diagnostic-error={error.GetType().Name}:{error.Message}"); }
            throw new InvalidOperationException("Published paste text readback differed from expected source");
        }
        Console.WriteLine($"editor-uia.paste-stage=published;length={text.Length};sha256={Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(text)))};seq={clipboardSequence}");
        if (ClipboardSequence() != clipboardSequence) throw new InvalidOperationException("Clipboard changed before guarded paste input");
        Console.WriteLine("editor-uia.paste-stage=prepared");
        SendChord(editor, host, 'A');
        SendChord(editor, host, 'V');
        SendChord(editor, host, 'A');
        SendChord(editor, host, 'C');
        Console.WriteLine("editor-uia.paste-stage=queued");
        var timer = Stopwatch.StartNew();
        while (GetClipboardSequenceNumber() == clipboardSequence && timer.Elapsed < TimeSpan.FromSeconds(3))
            Thread.Sleep(25);
        if (GetClipboardSequenceNumber() == clipboardSequence)
        {
            Console.Error.WriteLine($"editor-uia.paste-stage=copy-timeout;seq={ClipboardSequence()};clipboard-owner-pid={ProcessIdForWindow(GetClipboardOwner())};open-clipboard-pid={ProcessIdForWindow(GetOpenClipboardWindow())};known-length={text.Length};known-sha256={Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(text)))}");
            throw new TimeoutException("Editor copy did not prove paste consumption");
        }
        var copiedSequence = WaitForStableClipboardOwner(host.Id, clipboardSequence, TimeSpan.FromMilliseconds(500));
        AssertClipboardState(copiedSequence, host.Id);
        var copied = ClipboardSafety.ReadText();
        AssertClipboardState(copiedSequence, host.Id);
        ClipboardReceipt.Record("host-copy-after-paste", host.Id, copiedSequence);
        var normalized = copied.Replace("\r\n", "\n", StringComparison.Ordinal);
        Console.WriteLine($"editor-uia.paste-stage=host-copy;length={copied.Length};sha256={Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(copied)))}");
        if (normalized != text.Replace("\r\n", "\n", StringComparison.Ordinal)) throw new InvalidOperationException("Editor copy did not match the pasted source after EOL normalization");
    }

    private static uint WaitForStableClipboardOwner(int ownerProcessId, uint previousSequence, TimeSpan timeout)
    {
        var timer = Stopwatch.StartNew();
        var stable = Stopwatch.StartNew();
        var observed = ClipboardSequence();
        while (timer.Elapsed < timeout)
        {
            var current = ClipboardSequence();
            var owner = ProcessIdForWindow(GetClipboardOwner());
            if (current == previousSequence || owner != ownerProcessId)
            {
                stable.Restart();
                observed = current;
            }
            else if (current != observed)
            {
                Console.WriteLine($"editor-uia.paste-stage=copy-publication-transition;from-seq={observed};to-seq={current};owner-pid={owner}");
                observed = current;
                stable.Restart();
            }
            else if (stable.Elapsed >= TimeSpan.FromMilliseconds(75))
                return observed;
            Thread.Sleep(15);
        }
        throw new TimeoutException("Editor copy did not reach a stable same-owner clipboard publication within 500 ms");
    }
}

static class NativeMessage
{
    private delegate int EnumChildCallback(nint hwnd, nint data);
    [StructLayout(LayoutKind.Sequential)] private struct WindowRect { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll", EntryPoint = "GetWindow", ExactSpelling = true)]
    private static extern nint GetWindow(nint hwnd, uint command);

    [DllImport("user32.dll", EntryPoint = "GetParent", ExactSpelling = true)]
    private static extern nint GetParent(nint hwnd);

    [DllImport("user32.dll", EntryPoint = "EnumWindows", ExactSpelling = true)]
    private static extern int EnumWindows(EnumChildCallback callback, nint data);

    [DllImport("user32.dll", EntryPoint = "IsWindowEnabled", ExactSpelling = true)]
    private static extern int IsWindowEnabled(nint hwnd);

    [DllImport("user32.dll", EntryPoint = "GetWindowRect", ExactSpelling = true)]
    private static extern int GetWindowRect(nint hwnd, out WindowRect rect);

    [DllImport("user32.dll", EntryPoint = "GetClassNameW", ExactSpelling = true, CharSet = CharSet.Unicode)]
    private static extern int GetClassName(nint hwnd, StringBuilder buffer, int capacity);

    [DllImport("user32.dll", EntryPoint = "GetWindowTextW", ExactSpelling = true, CharSet = CharSet.Unicode)]
    private static extern int GetWindowTitle(nint hwnd, StringBuilder buffer, int capacity);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW", ExactSpelling = true)]
    private static extern nint GetWindowLongPtr(nint hwnd, int index);

    [DllImport("oleacc.dll", ExactSpelling = true)]
    private static extern int AccessibleObjectFromWindow(nint hwnd, uint objectId, ref Guid interfaceId,
        [MarshalAs(UnmanagedType.Interface)] out object accessible);

    [DllImport("user32.dll", EntryPoint = "IsWindowVisible", ExactSpelling = true)]
    private static extern int IsWindowVisible(nint hwnd);

    [DllImport("user32.dll", EntryPoint = "GetWindowThreadProcessId", ExactSpelling = true)]
    private static extern uint GetWindowThreadProcessId(nint hwnd, out uint processId);

    [DllImport("user32.dll", EntryPoint = "EnumChildWindows", ExactSpelling = true)]
    private static extern int EnumChildWindows(nint parent, EnumChildCallback callback, nint data);

    [DllImport("user32.dll", EntryPoint = "GetDlgCtrlID", ExactSpelling = true)]
    private static extern int GetDlgCtrlID(nint hwnd);

    public static nint EnabledPopup(nint owner) => GetWindow(owner, 6);

    public static bool IsVisible(nint hwnd) => IsWindowVisible(hwnd) != 0;

    public static IReadOnlyList<nint> TopLevelWindows(int processId, string titlePrefix)
    {
        var handles = new List<nint>();
        var visited = 0;
        EnumChildCallback callback = (hwnd, _) =>
        {
            if (++visited > 2048) return 0;
            if (ProcessId(hwnd) != processId || !IsVisible(hwnd)) return 1;
            var title = new StringBuilder(256);
            _ = GetWindowTitle(hwnd, title, title.Capacity);
            if (title.ToString().StartsWith(titlePrefix, StringComparison.OrdinalIgnoreCase)) handles.Add(hwnd);
            return 1;
        };
        var completed = EnumWindows(callback, 0);
        GC.KeepAlive(callback);
        if (completed == 0 || visited > 2048)
            throw new InvalidOperationException("Native top-level window enumeration did not finish within its 2048-window bound");
        return handles;
    }

    public static string? AccessibleName(nint hwnd)
    {
        var interfaceId = new Guid("618736e0-3c3d-11cf-810c-00aa00389b71");
        try
        {
            if (AccessibleObjectFromWindow(hwnd, 0xFFFFFFFC, ref interfaceId, out var value) < 0) return null;
            try { return (value as Accessibility.IAccessible)?.get_accName(0); }
            finally { if (Marshal.IsComObject(value)) Marshal.ReleaseComObject(value); }
        }
        catch (COMException) { return null; }
    }

    public static IReadOnlyList<nint> OwnedCalltipPopups(int processId, nint workspace)
    {
        var handles = new List<nint>();
        var visited = 0;
        EnumChildCallback callback = (hwnd, _) =>
        {
            if (++visited > 2048) return 0;
            if (ProcessId(hwnd) != processId || GetWindow(hwnd, 4) != workspace
                || !IsVisible(hwnd) || IsWindowEnabled(hwnd) == 0 || GetWindowRect(hwnd, out var rect) == 0
                || rect.Right <= rect.Left || rect.Bottom <= rect.Top
                || ((ulong)GetWindowLongPtr(hwnd, -16) & 0x80000000UL) == 0)
                return 1;
            var className = new StringBuilder(256);
            var title = new StringBuilder(256);
            _ = GetClassName(hwnd, className, className.Capacity);
            _ = GetWindowTitle(hwnd, title, title.Capacity);
            if (className.ToString() == "wxWindowNR" && title.ToString() == "panel") handles.Add(hwnd);
            return 1;
        };
        EnumWindows(callback, 0);
        GC.KeepAlive(callback);
        return handles;
    }

    public static bool IsRelatedTo(nint hwnd, nint owner)
    {
        var current = hwnd;
        for (var depth = 0; current != 0 && depth < 16; ++depth)
        {
            if (current == owner) return true;
            var parent = GetParent(current);
            current = parent != 0 ? parent : GetWindow(current, 4);
        }
        return false;
    }

    public static IReadOnlyList<string> SameProcessWindows(int processId, nint workspace)
    {
        var handles = new HashSet<nint> { workspace };
        var visitedRoots = 0;
        EnumChildCallback roots = (hwnd, _) =>
        {
            if (++visitedRoots > 2048) return 0;
            if (ProcessId(hwnd) == processId) handles.Add(hwnd);
            return 1;
        };
        EnumWindows(roots, 0);
        var visitedChildren = 0;
        EnumChildCallback children = (hwnd, _) =>
        {
            if (++visitedChildren > 512) return 0;
            if (ProcessId(hwnd) == processId) handles.Add(hwnd);
            return 1;
        };
        EnumChildWindows(workspace, children, 0);
        GC.KeepAlive(roots);
        GC.KeepAlive(children);
        var lines = new List<string>();
        foreach (var hwnd in handles)
        {
            var className = new StringBuilder(256);
            var title = new StringBuilder(256);
            _ = GetClassName(hwnd, className, className.Capacity);
            _ = GetWindowTitle(hwnd, title, title.Capacity);
            _ = GetWindowRect(hwnd, out var rect);
            string uia;
            try
            {
                var current = AutomationElement.FromHandle(hwnd).Current;
                uia = $"{current.Name}\t{current.ControlType.ProgrammaticName}\t{current.IsOffscreen}";
            }
            catch (Exception error) { uia = "uia-error-" + error.GetType().Name; }
            lines.Add($"{hwnd}\tparent={GetParent(hwnd)}\towner={GetWindow(hwnd, 4)}\tclass={className}\tvisible={IsVisible(hwnd)}\tenabled={IsWindowEnabled(hwnd) != 0}\trect={rect.Left},{rect.Top},{rect.Right},{rect.Bottom}\ttitle={title}\tuia={uia}");
        }
        return lines;
    }

    public static int ProcessId(nint hwnd)
    {
        GetWindowThreadProcessId(hwnd, out var processId);
        return checked((int)processId);
    }

    public static IReadOnlyList<nint> DescendantsWithControlId(nint parent, int controlId)
    {
        var handles = new List<nint>();
        var visited = 0;
        EnumChildCallback callback = (hwnd, _) =>
        {
            if (++visited > 512) return 0;
            if (GetDlgCtrlID(hwnd) == controlId) handles.Add(hwnd);
            return 1;
        };
        EnumChildWindows(parent, callback, 0);
        GC.KeepAlive(callback);
        if (visited > 512) throw new InvalidOperationException("Native dialog has too many child windows");
        return handles;
    }

    public static IReadOnlyList<nint> DescendantsWithTitle(nint parent, string title)
    {
        var handles = new List<nint>();
        var visited = 0;
        EnumChildCallback callback = (hwnd, _) =>
        {
            if (++visited > 512) return 0;
            var observed = new StringBuilder(256);
            _ = GetWindowTitle(hwnd, observed, observed.Capacity);
            if (observed.ToString() == title) handles.Add(hwnd);
            return 1;
        };
        var completed = EnumChildWindows(parent, callback, 0);
        GC.KeepAlive(callback);
        if (completed == 0 || visited > 512)
            throw new InvalidOperationException("Native titled child-window enumeration did not finish within its 512-window bound");
        return handles;
    }

    public static IReadOnlyList<nint> DescendantsWithTitlePrefix(nint parent, string titlePrefix)
    {
        var handles = new List<nint>();
        var visited = 0;
        EnumChildCallback callback = (hwnd, _) =>
        {
            if (++visited > 512) return 0;
            var observed = new StringBuilder(1024);
            _ = GetWindowTitle(hwnd, observed, observed.Capacity);
            if (observed.ToString().StartsWith(titlePrefix, StringComparison.OrdinalIgnoreCase)) handles.Add(hwnd);
            return 1;
        };
        var completed = EnumChildWindows(parent, callback, 0);
        GC.KeepAlive(callback);
        if (completed == 0 || visited > 512)
            throw new InvalidOperationException("Native prefixed child-window enumeration did not finish within its 512-window bound");
        return handles;
    }

    public static IReadOnlyList<nint> DescendantsWithTitleSubstring(nint parent, string text)
    {
        var handles = new List<nint>();
        var visited = 0;
        EnumChildCallback callback = (hwnd, _) =>
        {
            if (++visited > 512) return 0;
            var observed = new StringBuilder(4096);
            _ = GetWindowTitle(hwnd, observed, observed.Capacity);
            if (observed.ToString().Contains(text, StringComparison.OrdinalIgnoreCase)) handles.Add(hwnd);
            return 1;
        };
        var completed = EnumChildWindows(parent, callback, 0);
        GC.KeepAlive(callback);
        if (completed == 0 || visited > 512)
            throw new InvalidOperationException("Native substring child-window enumeration did not finish within its 512-window bound");
        return handles;
    }

    public static IReadOnlyList<nint> DescendantsWithClass(nint parent, string expectedClass)
    {
        var handles = new List<nint>();
        var visited = 0;
        EnumChildCallback callback = (hwnd, _) =>
        {
            if (++visited > 512) return 0;
            var observed = new StringBuilder(256);
            _ = GetClassName(hwnd, observed, observed.Capacity);
            if (observed.ToString() == expectedClass) handles.Add(hwnd);
            return 1;
        };
        var completed = EnumChildWindows(parent, callback, 0);
        GC.KeepAlive(callback);
        if (completed == 0 || visited > 512)
            throw new InvalidOperationException("Native class child-window enumeration did not finish within its 512-window bound");
        return handles;
    }

    public static IReadOnlyList<nint> Descendants(nint parent)
    {
        var handles = new List<nint>();
        var visited = 0;
        EnumChildCallback callback = (hwnd, _) =>
        {
            if (++visited > 512) return 0;
            handles.Add(hwnd);
            return 1;
        };
        var completed = EnumChildWindows(parent, callback, 0);
        GC.KeepAlive(callback);
        if (completed == 0 || visited > 512)
            throw new InvalidOperationException("Native child-window enumeration did not finish within its 512-window bound");
        return handles;
    }

    public static IReadOnlyList<string> DescribeDescendants(nint parent)
    {
        return Descendants(parent).Select(hwnd =>
        {
            var title = new StringBuilder(1024);
            var className = new StringBuilder(256);
            _ = GetWindowTitle(hwnd, title, title.Capacity);
            _ = GetClassName(hwnd, className, className.Capacity);
            return $"handle={hwnd}\tparent={GetParent(hwnd)}\tclass={className}\tcontrol-id={GetDlgCtrlID(hwnd)}\tvisible={IsVisible(hwnd)}\ttitle={title}";
        }).ToArray();
    }

    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", ExactSpelling = true, SetLastError = true)]
    private static extern nint SendMessageTimeoutRaw(nint hwnd, uint message, nint wParam, nint lParam, uint flags, uint timeout, out nint result);

    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern nint SendMessageTimeoutText(nint hwnd, uint message, nint wParam, [MarshalAs(UnmanagedType.LPWStr)] string text, uint flags, uint timeout, out nint result);

    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern nint SendMessageTimeoutBuffer(nint hwnd, uint message, nint wParam, [Out] char[] buffer, uint flags, uint timeout, out nint result);

    [DllImport("user32.dll", EntryPoint = "PostMessageW", ExactSpelling = true, SetLastError = true)]
    private static extern int PostMessageW(nint hwnd, uint message, nint wParam, nint lParam);

    public static nint Send(nint hwnd, int message, nint wParam, nint lParam)
    {
        if (SendMessageTimeoutRaw(hwnd, (uint)message, wParam, lParam, 0x0002, 2000, out var result) == 0)
            throw new TimeoutException($"Native editor message {message} timed out or failed");
        return result;
    }

    public static string GetWindowText(nint hwnd)
    {
        var length = Send(hwnd, 0x000E, 0, 0).ToInt32();
        if (length < 0 || length > 1_000_000) throw new InvalidOperationException("Native editor text length is invalid");
        var buffer = new char[length + 1];
        if (SendMessageTimeoutBuffer(hwnd, 0x000D, length + 1, buffer, 0x0002, 2000, out var copied) == 0)
            throw new TimeoutException("WM_GETTEXT timed out or failed");
        return new string(buffer, 0, copied.ToInt32());
    }

    public static bool TrySetWindowText(nint hwnd, string text)
    {
        return SendMessageTimeoutText(hwnd, 0x000C, 0, text, 0x0002, 2000, out var result) != 0 && result != 0;
    }

    public static void Post(nint hwnd, int message)
    {
        if (PostMessageW(hwnd, (uint)message, 0, 0) == 0)
            throw new InvalidOperationException($"Could not post window message {message}");
    }

    public static void PostCommand(nint hwnd, int message, int wParam, int lParam)
    {
        if (PostMessageW(hwnd, (uint)message, wParam, lParam) == 0)
            throw new InvalidOperationException($"Could not post window command {message}");
    }
}
