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
    try
    {
        var workerBudget = TimeSpan.FromSeconds(110) - total.Elapsed;
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
    if (timedOut) throw new TimeoutException("Lua Workspace GUI E2E exceeded its 120-second total limit");
    return worker.ExitCode;
}

static int Run(string[] args)
{
    string? exe = null;
    string? artifacts = null;
    for (var i = 0; i < args.Length; ++i)
    {
        switch (args[i])
        {
            case "--exe": exe = Path.GetFullPath(args[++i]); break;
            case "--artifacts": artifacts = Path.GetFullPath(args[++i]); break;
            default: throw new ArgumentException($"Unknown argument: {args[i]}");
        }
    }
    if (exe is null || !File.Exists(exe) || artifacts is null)
        throw new ArgumentException("--exe and --artifacts are required");
    Directory.CreateDirectory(artifacts);
    ClipboardReceipt.PathName = Path.Combine(artifacts, "clipboard-receipt.json");
    File.Delete(ClipboardReceipt.PathName);
    ClipboardReceipt.Initialize(uint.Parse(Environment.GetEnvironmentVariable("AEGISUB_UIA_CLIPBOARD_START_SEQUENCE") ?? throw new InvalidOperationException("Clipboard baseline is missing"), System.Globalization.CultureInfo.InvariantCulture));
    var fixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "editor.ass");
    var mutationFixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "metadata-mutations.lua");
    var luaFixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "editor-file.lua");
    var driver = Path.Combine("tests", "gui-automation", "lua-workspace", "editor-uia.cs");
    var startedUtc = DateTimeOffset.UtcNow;
    var exeHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(exe)));
    var fixtureHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(fixture)));
    var mutationFixtureHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(mutationFixture)));
    var luaFixtureHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(luaFixture)));
    var driverHash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(driver)));
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
    var results = new List<StepResult>();
    var allSteps = new[] { "host-ready", "open-workspace", "edit-local-undo-redo", "format-apply-save", "main-undo-redo-isolation", "dirty-close-cancel-discard", "lua-file-save-success", "lua-file-save-failure", "external-text-conflict", "multi-code-switch", "host-close-dirty", "lua-file-external-change", "change-ass-dirty", "target-effect-style-comment", "target-deletion", "invalid-source", "explicit-reload", "file-conflict-branches", "successful-ass-generation" };
    Process? host = null;
    AutomationElement? workspace = null;
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
        SaveUiEvidence(main, artifacts, "main-before-menu");
        Step("open-workspace", () =>
        {
            SelectCodeViaMacro(main, host, first: true);
            InvokeMenu(main, host, "Automation", "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(12));
            workspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(12));
            SaveUiEvidence(workspace, artifacts, "opened");
            var editor = FindNamed(workspace, "Lua source") ?? throw new InvalidOperationException("Lua source editor is absent");
            Ensure(ReadEditor(editor).Contains("local total", StringComparison.Ordinal), "Initial code line was not loaded");
        });
        var sourceEditor = FindNamed(workspace!, "Lua source")!;
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
            var saveSubtitles = UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles")
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
            SaveUiEvidence(workspace!, artifacts, "applied");
        });
        Step("main-undo-redo-isolation", () =>
        {
            InvokeMenu(main, host, "Edit", "Undo", TimeSpan.FromSeconds(6));
            InvokeButton(main, "Save the current subtitles");
            WaitUntil(() => EventLines(File.ReadAllText(input))[0].Text == EventLines(original)[0].Text, TimeSpan.FromSeconds(6), "Main Undo did not restore the original code line");
            Ensure(ReadEditor(sourceEditor) == formattedSource, "Main Undo changed the Workspace editor buffer");
            InvokeMenu(main, host, "Edit", "Redo", TimeSpan.FromSeconds(6));
            InvokeButton(main, "Save the current subtitles");
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
            InvokeButton(main, "Save the current subtitles");
            WaitUntil(() => EventLines(File.ReadAllText(input))[0].Text.Contains("external subtitle change", StringComparison.Ordinal), TimeSpan.FromSeconds(5), "External subtitle edit was not committed to ASS");
            var applyTask = Task.Run(() => InvokeButton(workspace, "Apply"));
            var conflict = WaitWindow(host, "Lua source conflict", TimeSpan.FromSeconds(5));
            SaveUiEvidence(conflict, artifacts, "conflict");
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
            InvokeButton(main, "Save the current subtitles");
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
            SaveUiEvidence(unsavedAss, artifacts, "main-close-unsaved-ass");
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
            InvokeButton(main, "Save the current subtitles");
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
            SaveUiEvidence(conflict, artifacts, "file-conflict");
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
                SaveUiEvidence(conflict, artifacts, "metadata-" + mutation.Field + "-conflict");
                InvokeButton(conflict, "Cancel");
                if (!applyTask.Wait(TimeSpan.FromSeconds(5))) throw new TimeoutException("Metadata conflict did not resume");
                Ensure(ReadEditor(sourceEditor) == "return 'metadata buffer retained'", "Metadata conflict lost editor source");
                DiscardAndCloseWorkspace(workspace, host);
                InvokeButton(main, "Save the current subtitles");
                WaitUntil(() => mutation.Field == "effect"
                    ? EventLines(File.ReadAllText(input))[0].Effect == "code line"
                    : EventLines(File.ReadAllText(input))[0].Style == "Alt", TimeSpan.FromSeconds(5), "Metadata mutation was not persisted");
                var changed = EventLines(File.ReadAllText(input));
                Ensure(changed.Count == baseline.Count && changed[1] == baseline[1] && changed[2] == baseline[2], "Metadata macro changed unrelated events");
                Ensure(mutation.Field == "effect"
                    ? changed[0].Effect == "code line" && changed[0] with { Effect = baseline[0].Effect } == baseline[0]
                    : changed[0].Style == "Alt" && changed[0] with { Style = baseline[0].Style } == baseline[0], "Metadata macro changed unexpected fields");
                InvokeMenu(main, host, "Edit", "Undo", TimeSpan.FromSeconds(6));
                InvokeButton(main, "Save the current subtitles");
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
            InvokeButton(main, "Save the current subtitles");
            WaitUntil(() => EventLines(File.ReadAllText(input))[0].Kind == "Dialogue", TimeSpan.FromSeconds(5), "Comment mutation was not persisted");
            var uncommented = EventLines(File.ReadAllText(input));
            Ensure(uncommented[0].Kind == "Dialogue" && uncommented[0] with { Kind = "Comment" } == baseline[0], "Toggle Comment changed more than the target's comment state");
            Ensure(uncommented[1] == baseline[1] && uncommented[2] == baseline[2], "Toggle Comment changed unrelated events");
            InvokeMenu(main, host, "Edit", "Undo", TimeSpan.FromSeconds(6));
            InvokeButton(main, "Save the current subtitles");
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
            InvokeButton(main, "Save the current subtitles");
            WaitUntil(() => EventLines(File.ReadAllText(input)).Count == 2, TimeSpan.FromSeconds(5), "Deletion was not persisted");
            var deleted = EventLines(File.ReadAllText(input));
            Ensure(deleted.Count == 2 && deleted[0] == baseline[1] && deleted[1] == baseline[2], "Delete Target changed surviving events");
            InvokeMenu(main, host, "Edit", "Undo", TimeSpan.FromSeconds(6));
            InvokeButton(main, "Save the current subtitles");
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
            SaveUiEvidence(workspace, artifacts, "invalid-format");
            WaitUntil(() => HasVisibleLuaDiagnostic(workspace), TimeSpan.FromSeconds(3), "Failed Format did not show a Lua diagnostic");
            Ensure(workspace.Current.Name.Contains(" *", StringComparison.Ordinal), "Failed Format did not retain dirty state");
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == invalid, "Failed Format changed invalid source");
            InvokeButton(workspace, "Apply");
            SaveUiEvidence(workspace, artifacts, "invalid-apply");
            WaitUntil(() => HasVisibleLuaDiagnostic(workspace), TimeSpan.FromSeconds(3), "Failed Apply did not show a Lua diagnostic");
            Ensure(workspace.Current.Name.Contains(" *", StringComparison.Ordinal), "Failed Apply did not retain dirty state");
            Ensure(NormalizeSource(ReadEditor(sourceEditor)) == invalid, "Failed Apply changed invalid source");
            Ensure(File.ReadAllBytes(input).SequenceEqual(unchangedAss), "Invalid Lua source altered the physical ASS file");
            Ensure(unchangedEvents.Count == 3 && unchangedEvents[0].Style == "Default", "Invalid-source probe has no expected Default-style code baseline");
            InvokeMenu(main, host, "Automation", "Workspace E2E Change Style", TimeSpan.FromSeconds(6));
            WaitUntil(() => workspace.Current.Name.Contains("[conflict]", StringComparison.Ordinal), TimeSpan.FromSeconds(5), "Controlled Style mutation did not reach Workspace conflict state");
            WaitUntil(() => UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles") is not null,
                TimeSpan.FromSeconds(5), "Main Save did not become available after controlled Style mutation");
            SaveUiEvidence(main, artifacts, "invalid-style-before-save-main");
            SaveUiEvidence(workspace, artifacts, "invalid-style-before-save-workspace");
            InvokeButton(main, "Save the current subtitles");
            var styleProbe = unchangedEvents.ToArray();
            styleProbe[0] = styleProbe[0] with { Style = "Alt" };
            WaitUntil(() => EventLines(File.ReadAllText(input)).SequenceEqual(styleProbe), TimeSpan.FromSeconds(5), "Forced Save did not preserve all event fields except the controlled Style mutation");
            File.Copy(input, Path.Combine(artifacts, "invalid-source-style-probe.ass"), overwrite: true);
            InvokeMenu(main, host, "Edit", "Undo", TimeSpan.FromSeconds(6));
            WaitUntil(() => !workspace.Current.Name.Contains("[conflict]", StringComparison.Ordinal), TimeSpan.FromSeconds(5), "Style Undo did not restore the Workspace target state");
            WaitUntil(() => UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles") is not null,
                TimeSpan.FromSeconds(5), "Main Save did not become available after Style Undo");
            SaveUiEvidence(main, artifacts, "invalid-style-undo-before-save-main");
            SaveUiEvidence(workspace, artifacts, "invalid-style-undo-before-save-workspace");
            InvokeButton(main, "Save the current subtitles");
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
            SaveUiEvidence(prompt, artifacts, "reload-reject");
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
            SaveUiEvidence(prompt, artifacts, "reload-confirm");
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
            SaveUiEvidence(workspace, artifacts, "file-conflict-race-result");
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
            SaveUiEvidence(workspace, artifacts, "generation-invalidated");
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
        action();
        results.Add(new StepResult(name, "passed", ""));
        WriteManifest();
    }
    void WriteManifest() => File.WriteAllText(Path.Combine(artifacts, "manifest.json"), JsonSerializer.Serialize(new
    {
        StartedUtc = startedUtc,
        FinishedUtc = finishedUtc,
        BudgetSeconds = 120,
        ExeSha256 = exeHash,
        Fixture = fixture.Replace('\\', '/'),
        FixtureSha256 = fixtureHash,
        MutationFixture = mutationFixture.Replace('\\', '/'),
        MutationFixtureSha256 = mutationFixtureHash,
        LuaFixture = luaFixture.Replace('\\', '/'),
        LuaFixtureSha256 = luaFixtureHash,
        DriverSha256 = driverHash,
        ExecutedDriverSha256 = executedDriverHash,
        ExitStatus = exitStatus,
        Steps = results
    }, new JsonSerializerOptions { WriteIndented = true }));
}

static void Ensure(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
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
    return workspace.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Text))
        .Cast<AutomationElement>().Any(item => item.Current.ClassName == "Static"
            && !item.Current.IsOffscreen
            && (item.Current.Name ?? "").Contains(expectedText, StringComparison.OrdinalIgnoreCase));
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
    var windows = AutomationElement.RootElement.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ProcessIdProperty, process.Id));
    foreach (AutomationElement root in windows)
    {
        var candidates = new[] { root }.Concat(root.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Window)).Cast<AutomationElement>());
        foreach (var window in candidates)
        {
            if (window.Current.ProcessId == process.Id
                && !window.Current.IsOffscreen
                && (window.Current.Name ?? "").StartsWith(title, StringComparison.OrdinalIgnoreCase))
                return window;
        }
    }
    return null;
}

static AutomationElement WaitWindow(Process process, string title, TimeSpan timeout)
{
    AutomationElement? found = null;
    WaitUntil(() => (found = FindWindow(process, title)) is not null, timeout, $"Window '{title}' was not found");
    return found!;
}

static AutomationElement? FindNamed(AutomationElement root, string name)
{
    for (var attempt = 0; attempt < 2; ++attempt)
    {
        var elements = root.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
        AutomationElement? stc = null;
        foreach (var element in elements)
        {
            try
            {
                var current = element.Current;
                if (string.Equals(current.Name, name, StringComparison.OrdinalIgnoreCase)) return element;
                if (name == "Lua source" && current.ControlType == ControlType.Pane && current.Name == "stcwindow") stc = element;
            }
            catch (ElementNotAvailableException) { }
        }
        if (stc is not null) return stc;
        Thread.Sleep(50);
    }
    return null;
}

static void InvokeButton(AutomationElement root, string name)
{
    var button = UiaDriver.FindEnabledInvokableButton(root, name) ?? throw new InvalidOperationException($"Button '{name}' is unavailable");
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
        return editBox.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ComboBox))
            .Cast<AutomationElement>().Any(item => item.Current.Name == effect);
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
    SaveUiEvidence(dialog, artifacts, artifactName);
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
        SaveUiEvidence(reload, artifacts, artifactName + "-reload-confirm");
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
    var openTask = Task.Run(() => InvokeButton(workspace, "Open File"));
    var dialog = WaitWindow(host, "Open Lua source", TimeSpan.FromSeconds(6));
    SaveUiEvidence(dialog, artifacts, "open-file-dialog");
    var filename = UiaDriver.FindDescendantByAutomationId(dialog, "1148", ControlType.Edit);
    if (filename is null || !filename.TryGetCurrentPattern(ValuePattern.Pattern, out var pattern))
        throw new InvalidOperationException("Open Lua source dialog has no UIA File name ValuePattern");
    ((ValuePattern)pattern).SetValue(path);
    InvokeDialogStockButton(dialog, 1);
    if (!openTask.Wait(TimeSpan.FromSeconds(6))) throw new TimeoutException("Open Lua source dialog did not complete");
}

static void OpenAssWithDirtyDecision(AutomationElement main, Process host, string artifacts, string? path, bool expectLoadError = false)
{
    var open = UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5001", "Open a subtitles file")
        ?? throw new InvalidOperationException("Main-window Open Subtitles toolbar button is unavailable");
    var openTask = Task.Run(() => UiaDriver.Invoke(open));
    InvokeButton(WaitWindow(host, "Unsaved Lua source", TimeSpan.FromSeconds(6)), "Discard");
    var picker = WaitWindow(host, "Open Subtitles", TimeSpan.FromSeconds(6));
    SaveUiEvidence(picker, artifacts, path is null ? "ass-picker-cancel" : expectLoadError ? "ass-picker-invalid" : "ass-picker-valid");
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
            SaveUiEvidence(error, artifacts, "ass-load-error");
            InvokeTaskDialogButton(error, "OK");
        }
    }
    if (!openTask.Wait(TimeSpan.FromSeconds(8))) throw new TimeoutException("Open Subtitles did not finish after the picker decision");
}

static void InvokeMenu(AutomationElement main, Process process, string menuName, string itemName, TimeSpan timeout)
{
    var direct = main.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem))
        .Cast<AutomationElement>().FirstOrDefault(item => (item.Current.Name ?? "").Replace("&", "").Contains(itemName, StringComparison.OrdinalIgnoreCase));
    if (direct is not null && direct.TryGetCurrentPattern(InvokePattern.Pattern, out var directPattern))
    {
        ((InvokePattern)directPattern).Invoke();
        return;
    }
    var menu = main.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem))
        .Cast<AutomationElement>().FirstOrDefault(item => (item.Current.Name ?? "").Contains(menuName, StringComparison.OrdinalIgnoreCase))
        ?? throw new InvalidOperationException($"Menu '{menuName}' is unavailable");
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
        var windows = AutomationElement.RootElement.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ProcessIdProperty, process.Id));
        foreach (AutomationElement window in windows)
        {
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
    if (editor.TryGetCurrentPattern(ValuePattern.Pattern, out var value))
        return ((ValuePattern)value).Current.Value;
    var hwnd = new nint(editor.Current.NativeWindowHandle);
    Ensure(hwnd != 0, "Editor has no UIA value or native window");
    if (!string.Equals(editor.Current.Name, "stcwindow", StringComparison.OrdinalIgnoreCase))
        return NativeMessage.GetWindowText(hwnd);
    var root = editor;
    while (TreeWalker.ControlViewWalker.GetParent(root) is { } parent && parent.Current.ControlType != ControlType.Window)
        root = parent;
    var frame = TreeWalker.ControlViewWalker.GetParent(root) ?? root;
    var copy = UiaDriver.FindEnabledInvokableButton(frame, "Copy source")
        ?? throw new InvalidOperationException("Lua source has no readable UIA or clipboard path");
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
    if (editor.TryGetCurrentPattern(ValuePattern.Pattern, out var value) && !((ValuePattern)value).Current.IsReadOnly)
    {
        ((ValuePattern)value).SetValue(text);
        return;
    }
    var hwnd = new nint(editor.Current.NativeWindowHandle);
    Ensure(hwnd != 0, "Editor cannot be changed via UIA or native HWND");
    if (!string.Equals(editor.Current.Name, "stcwindow", StringComparison.OrdinalIgnoreCase))
    {
        Ensure(NativeMessage.TrySetWindowText(hwnd, text), "WM_SETTEXT is unsupported by this edit control");
        return;
    }
    GuardedKeyboard.ReplaceWithClipboard(editor, host, text);
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
    var nodes = window.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>()
        .Take(400).Select(element => $"{element.Current.ControlType.ProgrammaticName}\t{element.Current.Name}\t{element.Current.AutomationId}\t{element.Current.ClassName}\t{element.Current.NativeWindowHandle}\t{element.Current.IsEnabled}\t{string.Join(',', element.GetSupportedPatterns().Select(pattern => pattern.ProgrammaticName))}");
    File.WriteAllLines(Path.Combine(artifacts, name + "-uia.txt"), nodes);
    ScreenCapture.SaveWindowPng(window, Path.Combine(artifacts, name + ".png"));
}

static void TryEvidence(AutomationElement window, string artifacts, string name)
{
    try { SaveUiEvidence(window, artifacts, name); } catch { }
}

sealed record AssEvent(string Kind, int Layer, int StartMs, int EndMs, string Style, string Name, int MarginL, int MarginR, int MarginV, string Effect, string Text);
sealed record StepResult(string Name, string Status, string Detail);
sealed record ClipboardReceiptData(uint Sequence, int OwnerProcessId, string Stage);

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
    {
        T? result = default;
        Exception? failure = null;
        var thread = new Thread(() =>
        {
            try { result = operation(); }
            catch (Exception error) { failure = error; }
        });
        thread.SetApartmentState(ApartmentState.STA);
        thread.IsBackground = true;
        thread.Start();
        if (!thread.Join(TimeSpan.FromSeconds(5))) throw new TimeoutException("STA clipboard operation exceeded five seconds");
        if (failure is not null) throw new InvalidOperationException("STA clipboard operation failed", failure);
        return result!;
    }

    public static string ReadText() => OnSta(() => Clipboard.GetText());
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
    {
        AssertOwnFocus(editor, host);
        var key = (ushort)char.ToUpperInvariant(letter);
        Input Key(ushort virtualKey, bool up) => new() { Type = 1, Union = new InputUnion { Keyboard = new KeyboardInput { VirtualKey = virtualKey, Flags = up ? 2u : 0u } } };
        var inputs = new[] { Key(0x11, false), Key(key, false), Key(key, true), Key(0x11, true) };
        if (SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<Input>()) != inputs.Length)
        {
            var released = SendInput(2, new[] { Key(key, true), Key(0x11, true) }, Marshal.SizeOf<Input>());
            throw new InvalidOperationException(released == 2
                ? "Guarded SendInput was partial; modifiers were released"
                : "Guarded SendInput was partial and modifier release was incomplete");
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
        if (published != text) throw new InvalidOperationException("Published paste text did not survive the STA thread exit intact");
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
        var copiedSequence = ClipboardSequence();
        AssertClipboardState(copiedSequence, host.Id);
        var copied = ClipboardSafety.ReadText();
        AssertClipboardState(copiedSequence, host.Id);
        ClipboardReceipt.Record("host-copy-after-paste", host.Id, copiedSequence);
        var normalized = copied.Replace("\r\n", "\n", StringComparison.Ordinal);
        Console.WriteLine($"editor-uia.paste-stage=host-copy;length={copied.Length};sha256={Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(copied)))}");
        if (normalized != text.Replace("\r\n", "\n", StringComparison.Ordinal)) throw new InvalidOperationException("Editor copy did not match the pasted source after EOL normalization");
    }
}

static class NativeMessage
{
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
