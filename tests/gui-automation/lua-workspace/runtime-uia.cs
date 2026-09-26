#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:property InvariantGlobalization=false
#:project ../driver/Aegisub.GuiAutomation.Driver.csproj

using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

const string workerFlag = "--runtime-worker";
try
{
    return args.Contains(workerFlag) ? Run(args.Where(arg => arg != workerFlag).ToArray()) : Supervise(args);
}
catch (Exception error)
{
    Console.Error.WriteLine($"runtime-uia.error={error.GetType().Name}:{error.Message}");
    return 1;
}

static int Supervise(string[] args)
{
    var started = Stopwatch.StartNew();
    var artifactsIndex = Array.IndexOf(args, "--artifacts");
    if (artifactsIndex < 0 || artifactsIndex + 1 >= args.Length) throw new ArgumentException("--artifacts is required");
    var artifacts = Path.GetFullPath(args[artifactsIndex + 1]);
    if (Directory.Exists(artifacts) || File.Exists(artifacts))
        throw new InvalidOperationException("Use a fresh runtime UIA artifact directory; existing evidence is never overwritten");
    Directory.CreateDirectory(artifacts);
    var exeIndex = Array.IndexOf(args, "--exe");
    if (exeIndex < 0 || exeIndex + 1 >= args.Length) throw new ArgumentException("--exe is required");
    var expectedExe = Path.GetFullPath(args[exeIndex + 1]);
    var readyPath = Path.Combine(artifacts, "ready.json");
    var identityPath = Path.Combine(artifacts, "host-identity.json");
    var executable = Environment.ProcessPath ?? throw new InvalidOperationException("Driver process path is unavailable");
    var start = new ProcessStartInfo(executable) { UseShellExecute = false };
    if (Path.GetFileNameWithoutExtension(executable).Equals("dotnet", StringComparison.OrdinalIgnoreCase))
        start.ArgumentList.Add(Assembly.GetEntryAssembly()?.Location ?? throw new InvalidOperationException("Driver assembly is unavailable"));
    start.ArgumentList.Add(workerFlag);
    foreach (var arg in args) start.ArgumentList.Add(arg);
    using var worker = Process.Start(start) ?? throw new InvalidOperationException("Could not start runtime UIA worker");
    var workerBudget = TimeSpan.FromSeconds(170) - started.Elapsed;
    var timedOut = workerBudget <= TimeSpan.Zero || !worker.WaitForExit(workerBudget);
    if (timedOut)
    {
        worker.Kill(entireProcessTree: true);
        if (!worker.WaitForExit(TimeSpan.FromSeconds(5))) throw new TimeoutException("Runtime UIA worker did not stop after timeout");
    }
    var hostLeftRunning = false;
    if (File.Exists(readyPath))
    {
        var hostId = JsonDocument.Parse(File.ReadAllText(readyPath)).RootElement.GetProperty("process_id").GetInt32();
        var identity = File.Exists(identityPath)
            ? JsonSerializer.Deserialize<HostIdentity>(File.ReadAllText(identityPath))
            : null;
        if (identity is null || identity.ProcessId != hostId)
            throw new InvalidOperationException("GUI host PID has no matching worker identity; no process was terminated");
        try
        {
            using var host = Process.GetProcessById(hostId);
            if (!host.HasExited)
            {
                hostLeftRunning = true;
                if (host.StartTime.ToUniversalTime().ToString("O") != identity.StartTimeUtc
                    || !Path.GetFullPath(host.MainModule?.FileName ?? "").Equals(expectedExe, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("Ready PID no longer belongs to this GUI host; no unrelated process was terminated");
                host.Kill(entireProcessTree: true);
                if (!host.WaitForExit(TimeSpan.FromSeconds(5))) throw new TimeoutException("GUI host could not be stopped after worker exit");
            }
        }
        catch (ArgumentException) { }
    }
    File.WriteAllText(Path.Combine(artifacts, "supervisor.json"), JsonSerializer.Serialize(new
    {
        BudgetSeconds = 180,
        TimedOut = timedOut,
        HostLeftRunning = hostLeftRunning,
        WorkerExitCode = worker.ExitCode,
        Status = timedOut ? "timeout" : hostLeftRunning ? "host-cleanup-required" : worker.ExitCode == 0 ? "passed" : "failed"
    }, new JsonSerializerOptions { WriteIndented = true }));
    if (timedOut) throw new TimeoutException("Lua Workspace runtime GUI E2E exceeded its 180-second total limit");
    return hostLeftRunning ? 1 : worker.ExitCode;
}

static int Run(string[] args)
{
    string? exe = null;
    string? artifacts = null;
    for (var index = 0; index < args.Length; ++index)
    {
        switch (args[index])
        {
            case "--exe": exe = Path.GetFullPath(args[++index]); break;
            case "--artifacts": artifacts = Path.GetFullPath(args[++index]); break;
            default: throw new ArgumentException($"Unknown argument: {args[index]}");
        }
    }
    if (exe is null || !File.Exists(exe) || artifacts is null)
        throw new ArgumentException("--exe and --artifacts are required");
    Directory.CreateDirectory(artifacts);
    var startedUtc = DateTimeOffset.UtcNow;
    DateTimeOffset? finishedUtc = null;
    var exitStatus = "running";
    var fixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "runtime.ass");
    var expectedPath = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "runtime.expected.json");
    var actions = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "runtime-actions.lua");
    var templater = Path.Combine("automation", "autoload", "kara-templater.lua");
    var includeRoot = Path.Combine("automation", "include");
    var driver = Path.Combine("tests", "gui-automation", "lua-workspace", "runtime-uia.cs");
    var hashes = new Dictionary<string, string>
    {
        ["exe"] = Hash(exe), ["fixture"] = Hash(fixture), ["expected"] = Hash(expectedPath),
        ["actions"] = Hash(actions), ["templater"] = Hash(templater), ["driver_source"] = Hash(driver),
        ["executed_driver"] = Hash(Assembly.GetEntryAssembly()?.Location ?? throw new InvalidOperationException("Executed assembly is unavailable"))
    };
    var includeFiles = Directory.GetFiles(includeRoot, "*", SearchOption.AllDirectories);
    foreach (var file in includeFiles)
        hashes[Path.Combine(includeRoot, Path.GetRelativePath(includeRoot, file)).Replace('\\', '/')] = Hash(file);
    var expected = JsonSerializer.Deserialize<RuntimeExpected>(File.ReadAllText(expectedPath), new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower })
        ?? throw new FormatException("Runtime expected JSON is empty");
    var wantedGenerated = expected.Generated ?? throw new FormatException("Runtime expected JSON has no generated events");
    Ensure(wantedGenerated.Count == expected.GeneratedCount && expected.GeneratedCount > 0, "Runtime expected JSON has inconsistent generated count");
    var input = Path.Combine(artifacts, "input.ass");
    File.Copy(fixture, input, overwrite: true);
    File.Copy(actions, Path.Combine(artifacts, "runtime-actions.lua"), overwrite: true);
    File.Copy(templater, Path.Combine(artifacts, "kara-templater.lua"), overwrite: true);
    foreach (var file in includeFiles)
    {
        var destination = Path.Combine(artifacts, "include", Path.GetRelativePath(includeRoot, file));
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        File.Copy(file, destination, overwrite: true);
    }
    var originalEvents = EventLines(File.ReadAllText(input));
    Ensure(originalEvents.Count == 5, "Runtime fixture does not have five original events");
    var profile = Path.Combine(artifacts, "profile");
    Directory.CreateDirectory(profile);
    var steps = new[] { "host-ready", "workspace-absent", "open-workspace", "template-completed", "persisted-generated", "validate-preserves", "no-output", "cancel", "error", "nil-error", "caught-cancel", "caught-cancel-then-error", "dialog-cancel", "progress-cancel", "rollback-state", "hide-no-observe", "reopen-new-observe", "template-code-parse", "template-code-runtime", "template-expression-parse", "template-expression-runtime", "undo-restores", "hide-active-run", "normal-shutdown" };
    var results = new List<StepResult>();
    Process? host = null;
    AutomationElement? workspace = null;
    AutomationElement? contextControl = null;
    AutomationElement? generatedControl = null;
    try
    {
        var start = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe)! };
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", artifacts, "--open", input })
            start.ArgumentList.Add(argument);
        host = Process.Start(start) ?? throw new InvalidOperationException("Could not start GUI host");
        File.WriteAllText(Path.Combine(artifacts, "host-identity.json"), JsonSerializer.Serialize(new HostIdentity(host.Id, host.StartTime.ToUniversalTime().ToString("O"))));
        Step("host-ready", () =>
        {
            var ready = AutomationProtocol.WaitForReadyArtifact(Path.Combine(artifacts, "ready.json"), host, TimeSpan.FromSeconds(15));
            Ensure(ready.ProcessId == host.Id, "ready.json belongs to another process");
            _ = UiaDriver.WaitForMainWindow(host, TimeSpan.FromSeconds(15));
        });
        var main = AutomationElement.FromHandle(host.MainWindowHandle);
        SaveUiEvidence(main, artifacts, "main-before-open");
        Step("workspace-absent", () =>
        {
            Ensure(FindWindow(host, "Lua Workspace") is null, "Workspace was created before an explicit open action");
            var task = Task.Run(() => InvokeMenu(main, host, "Workspace Runtime Hidden Acknowledgement", TimeSpan.FromSeconds(8)));
            var dialog = WaitWindowContainingText(host, "The hidden Workspace invocation has started", TimeSpan.FromSeconds(8));
            SaveUiEvidence(dialog, artifacts, "absent-workspace-acknowledgement");
            Ensure(FindWindow(host, "Lua Workspace") is null, "Macro created Workspace while its acknowledgement dialog was open");
            InvokeExactButton(dialog, "OK");
            if (!task.Wait(TimeSpan.FromSeconds(10))) throw new TimeoutException("Pre-open acknowledgement macro did not finish after exact OK");
            WaitUntil(() => main.Current.IsEnabled, TimeSpan.FromSeconds(5), "Main window did not re-enable after pre-open macro");
            Ensure(FindWindow(host, "Lua Workspace") is null, "Macro created Workspace before the user opened it");
        });
        Step("open-workspace", () =>
        {
            InvokeMenu(main, host, "Workspace Runtime Select Code", TimeSpan.FromSeconds(8));
            WaitUntil(() => HasSelectedEffect(main, "code once"), TimeSpan.FromSeconds(5), "Selection macro did not activate the code line");
            InvokeMenu(main, host, "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(8));
            workspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(8));
            SaveUiEvidence(workspace, artifacts, "workspace-opened");
            SelectTab(workspace, "Context");
            contextControl = FindPanelElement(workspace, "Context");
            SelectTab(workspace, "Generated");
            generatedControl = FindPanelElement(workspace, "Generated");
            SelectTab(workspace, "Context");
            var context = ReadPanelValue(contextControl);
            Ensure(context.Contains("No observed macro run", StringComparison.Ordinal), "Workspace began with a fabricated runtime observation");
        });
        string completedContext = "";
        string completedGenerated = "";
        Step("template-completed", () =>
        {
            InvokeMacroWithOptionalProgress(main, host, contextControl!, "Apply karaoke template", artifacts, "template", "completed");
            completedContext = WaitContextOutcome(contextControl!, "Apply karaoke template", "completed", TimeSpan.FromSeconds(10));
            completedGenerated = ReadPanelValue(generatedControl!);
            File.WriteAllText(Path.Combine(artifacts, "template-context.txt"), completedContext, new UTF8Encoding(false));
            File.WriteAllText(Path.Combine(artifacts, "template-generated.txt"), completedGenerated, new UTF8Encoding(false));
            SaveRuntimeEvidence(workspace!, contextControl!, generatedControl!, artifacts, "template-completed");
            var last = wantedGenerated[^1];
            Ensure(completedContext.Contains("Template kind: generated-line", StringComparison.Ordinal), "Final template kind is not the observed generated-line context");
            Ensure(completedContext.Contains("Phase: syl-text", StringComparison.Ordinal) && completedContext.Contains("Scope: syl", StringComparison.Ordinal), "Final template phase or scope is incorrect");
            Ensure(completedContext.Contains("Loop index: 2", StringComparison.Ordinal) && completedContext.Contains("Loop count: 2", StringComparison.Ordinal), "Final loop context is incorrect");
            Ensure(completedContext.Contains("Source line: 11", StringComparison.Ordinal)
                && completedContext.Contains("Source text: !retime(\"syl\",0,0)!S!j!:!decorate(syl.text_stripped)!", StringComparison.Ordinal), "Final source template context is incorrect");
            Ensure(completedContext.Contains("Target line: 13", StringComparison.Ordinal)
                && completedContext.Contains($"Target text: {last.Text}", StringComparison.Ordinal), "Final target line context is incorrect");
            Ensure(completedContext.Contains("Syllable index: 2", StringComparison.Ordinal)
                && completedContext.Contains("Syllable text: delta", StringComparison.Ordinal), "Final syllable context is incorrect");
            Ensure(completedGenerated.Contains($"Reported generated count: {expected.GeneratedCount}", StringComparison.Ordinal), "Generated count does not match independent fixture expectation");
            Ensure(completedGenerated.Contains($"Last generated index: {expected.GeneratedCount}", StringComparison.Ordinal), "Last generated index does not match independent fixture expectation");
            Ensure(completedGenerated.Contains($"Last text: {last.Text}", StringComparison.Ordinal), "Last generated text does not match independent fixture expectation");
            Ensure(completedGenerated.Contains("Last style: Default", StringComparison.Ordinal), "Last generated style is incorrect");
            Ensure(completedGenerated.Contains($"Last start (ms): {last.StartMs}", StringComparison.Ordinal)
                && completedGenerated.Contains($"Last end (ms): {last.EndMs}", StringComparison.Ordinal), "Last generated timing is incorrect");
        });
        Step("persisted-generated", () =>
        {
            WaitUntil(() => UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles") is not null,
                TimeSpan.FromSeconds(8), "Main Save did not become available after template macro");
            UiaDriver.Invoke(UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles")!);
            WaitUntil(() => EventLines(File.ReadAllText(input)).Count == originalEvents.Count + expected.GeneratedCount,
                TimeSpan.FromSeconds(8), "Saved ASS did not contain the expected generated event count");
            var saved = File.ReadAllText(input);
            File.WriteAllText(Path.Combine(artifacts, "output.ass"), saved);
            AssertPhysicalEventLines(saved, originalEvents.Count + expected.GeneratedCount);
            var events = EventLines(saved);
            for (var index = 0; index < 3; ++index)
                Ensure(events[index] == originalEvents[index], $"Coding/template event {index} changed");
            for (var index = 3; index < 5; ++index)
                Ensure(events[index] with { Kind = originalEvents[index].Kind, Effect = originalEvents[index].Effect } == originalEvents[index]
                    && events[index].Kind == "Comment" && events[index].Effect == "karaoke", $"Original karaoke line {index} was not retained as a comment");
            for (var index = 0; index < expected.GeneratedCount; ++index)
            {
                var actual = events[originalEvents.Count + index];
                var wanted = wantedGenerated[index];
                Ensure(actual.Kind == "Dialogue" && actual.Effect == "fx" && actual.Style == "Default" && actual.Name == ""
                    && actual.MarginL == 0 && actual.MarginR == 0 && actual.MarginV == 0
                    && actual.Text == wanted.Text && actual.Layer == wanted.Layer
                    && actual.StartMs == wanted.StartMs && actual.EndMs == wanted.EndMs,
                    $"Generated event {index + 1} differs from its independent business expectation");
            }
        });
        Step("validate-preserves", () =>
        {
            var command = OpenAutomationMenuCommand(main, host, "Workspace Runtime No Output", TimeSpan.FromSeconds(8), forceOpen: true);
            Ensure(FindVisibleMenuCommand(host, "Apply karaoke template") is not null, "Automation menu did not expose the real karaoke-template validation entry");
            var observedContext = ReadPanelValue(contextControl!);
            var observedGenerated = ReadPanelValue(generatedControl!);
            Ensure(observedContext == completedContext && observedGenerated == completedGenerated,
                "Automation menu validation erased or changed the completed runtime observation");
            SaveUiEvidence(workspace!, artifacts, "after-menu-validation");
            UiaDriver.Invoke(command);
            WaitContextOutcome(contextControl!, "Workspace Runtime No Output", "completed", TimeSpan.FromSeconds(8));
        });
        Step("no-output", () =>
        {
            var generated = ReadPanelValue(generatedControl!);
            Ensure(generated.Contains("No generated line reported for this run.", StringComparison.Ordinal), "New No Output run retained stale generated content");
            Ensure(!generated.Contains("Last generated index:", StringComparison.Ordinal), "No Output retained the preceding run's generated line");
            SaveRuntimeEvidence(workspace!, contextControl!, generatedControl!, artifacts, "no-output");
        });
        var savedAssBytes = File.ReadAllBytes(input);

        void RunOutcome(string macro, string expectedOutcome, string name, bool errorDialog = false)
        {
            if (errorDialog) InvokeMacroWithErrorProgress(main, host, macro, artifacts, name);
            else InvokeMenu(main, host, macro, TimeSpan.FromSeconds(8));
            EnsureOutcomeAndRollback(macro, expectedOutcome, name, verifyRollback: true);
        }

        void EnsureOutcomeAndRollback(string macro, string outcome, string name, bool verifyRollback)
        {
            WaitContextOutcome(contextControl!, macro, outcome, TimeSpan.FromSeconds(8));
            SaveRuntimeEvidence(workspace!, contextControl!, generatedControl!, artifacts, name);
            Ensure(File.ReadAllBytes(input).SequenceEqual(savedAssBytes), $"{macro} changed the persisted ASS file");
            if (!verifyRollback) return;
            InvokeMenu(main, host, "Workspace Runtime Verify Rollback", TimeSpan.FromSeconds(8));
            WaitContextOutcome(contextControl!, "Workspace Runtime Verify Rollback", "completed", TimeSpan.FromSeconds(8));
            Ensure(File.ReadAllBytes(input).SequenceEqual(savedAssBytes), $"Rollback verification changed the persisted ASS file after {macro}");
        }

        Step("cancel", () => RunOutcome("Workspace Runtime Cancel", "cancelled", "cancel"));
        Step("error", () => RunOutcome("Workspace Runtime Error", "failed", "error", errorDialog: true));
        Step("nil-error", () => RunOutcome("Workspace Runtime Nil Error", "failed", "nil-error", errorDialog: true));
        Step("caught-cancel", () => RunOutcome("Workspace Runtime Caught Cancel", "completed", "caught-cancel"));
        Step("caught-cancel-then-error", () => RunOutcome("Workspace Runtime Caught Cancel Then Error", "failed", "caught-cancel-then-error", errorDialog: true));
        Step("dialog-cancel", () =>
        {
            var task = Task.Run(() => InvokeMenu(main, host, "Workspace Runtime Dialog Cancel", TimeSpan.FromSeconds(8)));
            var dialog = WaitWindowContainingText(host, "Cancel only this dialog; the invocation must complete", TimeSpan.FromSeconds(8));
            SaveUiEvidence(dialog, artifacts, "dialog-cancel-prompt");
            InvokeExactButton(dialog, "Cancel");
            if (!task.Wait(TimeSpan.FromSeconds(10))) throw new TimeoutException("Dialog Cancel macro did not finish");
            EnsureOutcomeAndRollback("Workspace Runtime Dialog Cancel", "completed", "dialog-cancel", verifyRollback: false);
        });
        Step("progress-cancel", () =>
        {
            var task = Task.Run(() => InvokeMenu(main, host, "Workspace Runtime Progress Cancel", TimeSpan.FromSeconds(8)));
            var progress = WaitProgressPane(host, "Workspace Runtime Progress Cancel", TimeSpan.FromSeconds(8));
            SaveUiEvidence(progress, artifacts, "progress-cancel-dialog");
            InvokeExactButton(progress, "Cancel");
            if (!task.Wait(TimeSpan.FromSeconds(12))) throw new TimeoutException("Progress Cancel macro did not finish after one Cancel action");
            EnsureOutcomeAndRollback("Workspace Runtime Progress Cancel", "cancelled", "progress-cancel", verifyRollback: true);
        });
        Step("rollback-state", () =>
        {
            InvokeMenu(main, host, "Workspace Runtime Verify Rollback", TimeSpan.FromSeconds(8));
            WaitContextOutcome(contextControl!, "Workspace Runtime Verify Rollback", "completed", TimeSpan.FromSeconds(8));
            Ensure(File.ReadAllBytes(input).SequenceEqual(savedAssBytes), "Outcome matrix changed the persisted subtitle file");
            SaveRuntimeEvidence(workspace!, contextControl!, generatedControl!, artifacts, "rollback-final");
        });
        Step("hide-no-observe", () =>
        {
            var before = ReadPanelValue(contextControl!);
            CloseWorkspace(workspace!);
            WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(6), "Workspace did not hide");
            var task = Task.Run(() => InvokeMenu(main, host, "Workspace Runtime Hidden Acknowledgement", TimeSpan.FromSeconds(8)));
            var dialog = WaitWindowContainingText(host, "The hidden Workspace invocation has started", TimeSpan.FromSeconds(8));
            SaveUiEvidence(dialog, artifacts, "hidden-invocation-acknowledgement");
            Ensure(FindWindow(host, "Lua Workspace") is null, "Hidden Workspace reopened during a macro run");
            Ensure(ReadPanelValue(contextControl!) == before, "Hidden Workspace observed a new macro run");
            InvokeExactButton(dialog, "OK");
            if (!task.Wait(TimeSpan.FromSeconds(10))) throw new TimeoutException("Hidden Acknowledgement macro did not finish after exact OK");
            WaitUntil(() => main.Current.IsEnabled, TimeSpan.FromSeconds(5), "Main window did not re-enable after hidden macro");
            Ensure(ReadPanelValue(contextControl!) == before, "Hidden Workspace context changed after the acknowledged macro completed");
            File.WriteAllText(Path.Combine(artifacts, "hidden-context.txt"), before, new UTF8Encoding(false));
        });
        Step("reopen-new-observe", () =>
        {
            InvokeMenu(main, host, "Workspace Runtime Select Code", TimeSpan.FromSeconds(8));
            WaitUntil(() => HasSelectedEffect(main, "code once"), TimeSpan.FromSeconds(5), "Reopen selection macro did not activate the code line");
            InvokeMenu(main, host, "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(8));
            workspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(8));
            SaveUiEvidence(workspace, artifacts, "workspace-reopened");
            SelectTab(workspace, "Context");
            contextControl = FindPanelElement(workspace, "Context");
            SelectTab(workspace, "Generated");
            generatedControl = FindPanelElement(workspace, "Generated");
            InvokeMenu(main, host, "Workspace Runtime No Output", TimeSpan.FromSeconds(8));
            WaitContextOutcome(contextControl!, "Workspace Runtime No Output", "completed", TimeSpan.FromSeconds(8));
            SaveRuntimeEvidence(workspace, contextControl!, generatedControl!, artifacts, "reopened-new-observation");
        });
        void RunTemplateFailure(TemplateFailureCase scenario)
        {
            var baselineEvents = EventLines(File.ReadAllText(input));
            var baselineBytes = File.ReadAllBytes(input);
            InvokeMacroWithOptionalProgress(main, host, contextControl!, "Workspace Runtime Prepare " + scenario.Name,
                artifacts, scenario.ArtifactName + "-prepare", "completed");
            WaitContextOutcome(contextControl!, "Workspace Runtime Prepare " + scenario.Name, "completed", TimeSpan.FromSeconds(8));
            Ensure(File.ReadAllBytes(input).SequenceEqual(baselineBytes), "Prepare macro unexpectedly wrote the physical ASS file");
            SaveRuntimeEvidence(workspace!, contextControl!, generatedControl!, artifacts, scenario.ArtifactName + "-prepared");
            var preparedEvents = baselineEvents.ToArray();
            var changedIndex = scenario.SourceLine == 9 ? 0 : scenario.SourceLine == 10 ? 1
                : throw new InvalidOperationException("Failure fixture source line has no expected original event");
            preparedEvents[changedIndex] = preparedEvents[changedIndex] with { Text = scenario.SourceText };
            WaitUntil(() => UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles") is not null,
                TimeSpan.FromSeconds(8), $"Main Save did not become available after {scenario.Name} preparation");
            UiaDriver.Invoke(UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles")!);
            WaitUntil(() => EventLines(File.ReadAllText(input)).SequenceEqual(preparedEvents), TimeSpan.FromSeconds(8),
                $"Saved {scenario.Name} preparation changed more than its expected source Text");
            File.Copy(input, Path.Combine(artifacts, scenario.ArtifactName + "-prepared.ass"), overwrite: true);
            var preparedBytes = File.ReadAllBytes(input);

            InvokeMacroWithOptionalProgress(main, host, contextControl!, "Apply karaoke template", artifacts,
                scenario.ArtifactName + "-apply", "failed");
            var failedContext = WaitContextOutcome(contextControl!, "Apply karaoke template", "failed", TimeSpan.FromSeconds(8));
            SaveRuntimeEvidence(workspace!, contextControl!, generatedControl!, artifacts, scenario.ArtifactName + "-failed");
            Ensure(HasExactLine(failedContext, "Template kind: " + scenario.ExpectedKind),
                $"{scenario.Name} did not retain the exact template failure kind");
            Ensure(HasExactLine(failedContext, "Source line: " + scenario.SourceLine)
                && HasExactLine(failedContext, "Source style: Default")
                && HasExactLine(failedContext, "Source text: " + scenario.SourceText),
                $"{scenario.Name} did not retain the prepared source-line identity and text");
            var diagnosticLabel = scenario.RuntimeMarker is null ? "Parse error: " : "Runtime error: ";
            var diagnosticLine = failedContext.Split('\n').Select(line => line.TrimEnd('\r'))
                .FirstOrDefault(line => line.StartsWith(diagnosticLabel, StringComparison.Ordinal));
            Ensure(diagnosticLine is not null && diagnosticLine.Length > diagnosticLabel.Length,
                $"{scenario.Name} did not expose a nonempty error diagnostic");
            if (scenario.RuntimeMarker is not null)
                Ensure(diagnosticLine!.EndsWith(": " + scenario.RuntimeMarker, StringComparison.Ordinal),
                    $"{scenario.Name} runtime diagnostic did not end with its expected payload marker");
            Ensure(File.ReadAllBytes(input).SequenceEqual(preparedBytes), $"{scenario.Name} failure changed the prepared physical ASS file");

            InvokeMacroWithOptionalProgress(main, host, contextControl!, "Workspace Runtime Verify Prepared Rollback", artifacts,
                scenario.ArtifactName + "-verify-rollback", "completed");
            WaitContextOutcome(contextControl!, "Workspace Runtime Verify Prepared Rollback", "completed", TimeSpan.FromSeconds(8));
            Ensure(File.ReadAllBytes(input).SequenceEqual(preparedBytes), $"{scenario.Name} rollback verification changed the prepared physical ASS file");

            InvokeMenu(main, host, "Undo", TimeSpan.FromSeconds(8), menuName: "Edit");
            WaitUntil(() => UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles") is not null,
                TimeSpan.FromSeconds(8), $"Main Save did not become available after undoing {scenario.Name} preparation");
            UiaDriver.Invoke(UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles")!);
            WaitUntil(() => EventLines(File.ReadAllText(input)).SequenceEqual(baselineEvents), TimeSpan.FromSeconds(8),
                $"One Undo did not restore the exact baseline after {scenario.Name}");
            File.Copy(input, Path.Combine(artifacts, scenario.ArtifactName + "-restored.ass"), overwrite: true);
        }

        Step("template-code-parse", () => RunTemplateFailure(new TemplateFailureCase("Code Parse", "template-code-parse", "code-parse", 9,
            "decorate = function(value) return value end; local broken =", null)));
        Step("template-code-runtime", () => RunTemplateFailure(new TemplateFailureCase("Code Runtime", "template-code-runtime", "code-error", 9,
            "_G.error(\"workspace-code-runtime\")", "workspace-code-runtime")));
        Step("template-expression-parse", () => RunTemplateFailure(new TemplateFailureCase("Expression Parse", "template-expression-parse", "expression-parse", 10,
            "L!j!:!(1 +)!;", null)));
        Step("template-expression-runtime", () => RunTemplateFailure(new TemplateFailureCase("Expression Runtime", "template-expression-runtime", "expression-error", 10,
            "L!j!:!_G.error(\"workspace-expression-runtime\")!;", "workspace-expression-runtime")));
        Step("undo-restores", () =>
        {
            InvokeMenu(main, host, "Undo", TimeSpan.FromSeconds(8), menuName: "Edit");
            WaitUntil(() => UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles") is not null,
                TimeSpan.FromSeconds(8), "Main Save did not become available after one Undo");
            UiaDriver.Invoke(UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles")!);
            WaitUntil(() => EventLines(File.ReadAllText(input)).SequenceEqual(originalEvents), TimeSpan.FromSeconds(8), "One main Undo did not restore all five original events");
            var restored = File.ReadAllText(input);
            AssertPhysicalEventLines(restored, originalEvents.Count);
            File.WriteAllText(Path.Combine(artifacts, "restored.ass"), restored);
            SaveRuntimeEvidence(workspace!, contextControl!, generatedControl!, artifacts, "after-main-undo");
        });
        Step("hide-active-run", () =>
        {
            var originalBytes = File.ReadAllBytes(input);
            Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(originalEvents), "Active-run close began without the restored five-event baseline");
            var workspaceTitle = workspace!.Current.Name ?? "";
            Ensure(!workspaceTitle.Contains(" *", StringComparison.Ordinal)
                && !workspaceTitle.Contains("[conflict]", StringComparison.Ordinal)
                && !workspaceTitle.Contains("[target unavailable]", StringComparison.Ordinal),
                "Active-run close requires a clean, valid Workspace target");
            var cleanLabel = workspace.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Text))
                .Cast<AutomationElement>().Any(item => item.Current.ClassName == "Static" && !item.Current.IsOffscreen
                    && item.Current.Name == "Source matches the saved baseline.");
            Ensure(cleanLabel, "Workspace did not expose the observed clean-source diagnostic before queued close");
            SaveUiEvidence(workspace, artifacts, "active-run-clean-baseline");
            var task = Task.Run(() => InvokeMenu(main, host, "Workspace Runtime Progress Cancel", TimeSpan.FromSeconds(8)));
            var progress = WaitProgressPane(host, "Workspace Runtime Progress Cancel", TimeSpan.FromSeconds(8));
            WaitContextOutcome(contextControl!, "Workspace Runtime Progress Cancel", "running", TimeSpan.FromSeconds(5));
            var mainEnabled = main.Current.IsEnabled;
            var workspaceEnabled = workspace!.Current.IsEnabled;
            File.WriteAllText(Path.Combine(artifacts, "active-run-modal-state.json"), JsonSerializer.Serialize(new
            {
                MainEnabled = mainEnabled,
                WorkspaceEnabled = workspaceEnabled,
                ProgressName = progress.Current.Name,
                ProgressClass = progress.Current.ClassName,
                ProgressType = progress.Current.ControlType.ProgrammaticName,
                ProgressHandle = progress.Current.NativeWindowHandle
            }, new JsonSerializerOptions { WriteIndented = true }));
            SaveProcessUiEvidence(host, artifacts, "active-run-before-close");
            SaveUiEvidence(progress, artifacts, "active-run-progress");
            SaveUiEvidence(workspace, artifacts, "active-run-workspace");
            Ensure(!mainEnabled && !workspaceEnabled, "Modal progress did not disable the main and Workspace windows as observed");
            var workspaceHandle = new nint(workspace.Current.NativeWindowHandle);
            Ensure(workspaceHandle != 0 && workspace.Current.ControlType == ControlType.Window && workspace.Current.ProcessId == host.Id,
                "Active Workspace has no exact same-host top-level window handle");
            GetWindowThreadProcessId(workspaceHandle, out var owner);
            Ensure(owner == (uint)host.Id, "Active Workspace HWND no longer belongs to the launched GUI host");
            if (PostMessageW(workspaceHandle, 0x0010, 0, 0) == 0)
                throw new InvalidOperationException("Queued WM_CLOSE could not be posted to the confirmed Workspace HWND");
            WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(5), "Queued OS close did not hide Workspace during the active macro");
            Ensure(FindProgressPane(host, "Workspace Runtime Progress Cancel") is not null,
                "Progress invocation disappeared before its explicit cancellation after Workspace hide");
            SaveProcessUiEvidence(host, artifacts, "active-run-after-close");
            InvokeExactButton(progress, "Cancel");
            if (!task.Wait(TimeSpan.FromSeconds(12))) throw new TimeoutException("Active-run Progress Cancel did not finish after one exact Cancel");
            WaitContextOutcome(contextControl!, "Workspace Runtime Progress Cancel", "cancelled", TimeSpan.FromSeconds(8));
            File.WriteAllText(Path.Combine(artifacts, "active-run-hidden-final-context.txt"), ReadPanelValue(contextControl!), new UTF8Encoding(false));
            File.WriteAllText(Path.Combine(artifacts, "active-run-hidden-final-generated.txt"), ReadPanelValue(generatedControl!), new UTF8Encoding(false));
            Ensure(FindWindow(host, "Lua Workspace") is null, "Workspace reopened after active invocation cancellation");
            WaitUntil(() => main.Current.IsEnabled, TimeSpan.FromSeconds(5), "Main window did not re-enable after active invocation cancellation");
            Ensure(File.ReadAllBytes(input).SequenceEqual(originalBytes)
                && EventLines(File.ReadAllText(input)).SequenceEqual(originalEvents), "Active cancellation changed the restored physical ASS baseline");
        });
        Step("normal-shutdown", () =>
        {
            if (!main.TryGetCurrentPattern(WindowPattern.Pattern, out var pattern))
                throw new InvalidOperationException("Main window has no real UIA WindowPattern.Close action");
            ((WindowPattern)pattern).Close();
            if (!host.WaitForExit(TimeSpan.FromSeconds(10))) throw new TimeoutException("GUI host did not exit normally within ten seconds");
            File.WriteAllText(Path.Combine(artifacts, "normal-shutdown.json"), JsonSerializer.Serialize(new
            {
                HostExitCode = host.ExitCode,
                Method = "WindowPattern.Close"
            }, new JsonSerializerOptions { WriteIndented = true }));
            Ensure(host.ExitCode == 0, "GUI host exited nonzero after main WindowPattern.Close");
        });
        foreach (var step in steps.Where(step => results.All(result => result.Name != step)))
            results.Add(new StepResult(step, "not-run", "Scenario was not executed"));
        exitStatus = results.Count == steps.Length && results.All(result => result.Status == "passed") ? "passed" : "incomplete";
        finishedUtc = DateTimeOffset.UtcNow;
        WriteManifest();
        return exitStatus == "passed" ? 0 : 1;
    }
    catch (Exception error)
    {
        if (host is not null && !host.HasExited)
            TryProcessUiEvidence(host, artifacts, "failure-pid-tree");
        var failing = steps.FirstOrDefault(step => results.All(result => result.Name != step));
        if (failing is not null) results.Add(new StepResult(failing, "failed", error.Message));
        foreach (var step in steps.Where(step => results.All(result => result.Name != step)))
            results.Add(new StepResult(step, "not-run", "A preceding step failed"));
        exitStatus = "failed";
        finishedUtc = DateTimeOffset.UtcNow;
        WriteManifest();
        Console.Error.WriteLine(error);
        return 1;
    }
    finally
    {
        if (workspace is not null) TryEvidence(workspace, artifacts, "final-workspace");
        if (host is not null && !host.HasExited)
        {
            host.Kill(entireProcessTree: true);
            if (!host.WaitForExit(TimeSpan.FromSeconds(5))) throw new TimeoutException("GUI host did not stop within five seconds");
        }
        host?.Dispose();
    }

    void Step(string name, Action action)
    {
        LogStage(artifacts, $"start-action:{name}");
        action();
        LogStage(artifacts, $"passed-action:{name}");
        results.Add(new StepResult(name, "passed", ""));
        WriteManifest();
    }

    void WriteManifest() => File.WriteAllText(Path.Combine(artifacts, "manifest.json"), JsonSerializer.Serialize(new
    {
        StartedUtc = startedUtc,
        FinishedUtc = finishedUtc,
        BudgetSeconds = 180,
        ExeSha256 = hashes["exe"],
        Fixtures = new[] { fixture.Replace('\\', '/'), expectedPath.Replace('\\', '/'), actions.Replace('\\', '/'), templater.Replace('\\', '/') },
        Sha256 = hashes,
        ExitStatus = exitStatus,
        Steps = results
    }, new JsonSerializerOptions { WriteIndented = true }));

}

static string Hash(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));

static void Ensure(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

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
        var descendants = root.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Window));
        foreach (var candidate in new[] { root }.Concat(descendants.Cast<AutomationElement>()))
        {
            try
            {
                if (candidate.Current.ProcessId == process.Id && !candidate.Current.IsOffscreen
                    && (candidate.Current.Name ?? "").StartsWith(title, StringComparison.OrdinalIgnoreCase)) return candidate;
            }
            catch (ElementNotAvailableException) { }
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

static AutomationElement? FindProgressPane(Process process, string exactTitle)
{
    var roots = AutomationElement.RootElement.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ProcessIdProperty, process.Id));
    var candidates = new List<AutomationElement>();
    foreach (AutomationElement root in roots)
    {
        foreach (var item in new[] { root }.Concat(root.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Pane)).Cast<AutomationElement>()))
        {
            try
            {
                var current = item.Current;
                if (current.ProcessId == process.Id && current.ControlType == ControlType.Pane && current.ClassName == "#32770"
                    && current.Name == exactTitle && current.NativeWindowHandle != 0 && !current.IsOffscreen)
                    candidates.Add(item);
            }
            catch (ElementNotAvailableException) { }
        }
    }
    Ensure(candidates.Count <= 1, $"Progress title '{exactTitle}' matched {candidates.Count} same-PID visible #32770 panes");
    return candidates.Count == 1 ? candidates[0] : null;
}

static AutomationElement WaitProgressPane(Process process, string exactTitle, TimeSpan timeout)
{
    AutomationElement? found = null;
    WaitUntil(() => (found = FindProgressPane(process, exactTitle)) is not null, timeout, $"Exact progress pane '{exactTitle}' was not found");
    return found!;
}

static AutomationElement WaitWindowContainingText(Process process, string exactText, TimeSpan timeout)
{
    AutomationElement? found = null;
    WaitUntil(() =>
    {
        var roots = AutomationElement.RootElement.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ProcessIdProperty, process.Id));
        foreach (AutomationElement root in roots)
        {
            var labels = root.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Text));
            foreach (AutomationElement label in labels)
            {
                try
                {
                    if (label.Current.Name != exactText) continue;
                    AutomationElement? owner = label;
                    while ((owner = TreeWalker.ControlViewWalker.GetParent(owner)) is not null)
                    {
                        var current = owner.Current;
                        if (current.ProcessId == process.Id && (current.ControlType == ControlType.Pane || current.ControlType == ControlType.Window)
                            && current.ClassName == "#32770" && current.NativeWindowHandle != 0 && !current.IsOffscreen)
                        {
                            found = owner;
                            return true;
                        }
                    }
                }
                catch (ElementNotAvailableException) { }
            }
        }
        return false;
    }, timeout, $"Dialog containing '{exactText}' was not found");
    return found!;
}

static AutomationElement? FindNamed(AutomationElement root, string name)
{
    foreach (AutomationElement item in root.FindAll(TreeScope.Descendants, Condition.TrueCondition))
    {
        try { if (item.Current.Name == name) return item; }
        catch (ElementNotAvailableException) { }
    }
    return null;
}

static bool HasSelectedEffect(AutomationElement main, string effect)
{
    var editBox = FindNamed(main, "SubsEditBox");
    if (editBox is null) return false;
    return editBox.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ComboBox))
        .Cast<AutomationElement>().Any(item => item.Current.Name == effect);
}

static AutomationElement FindRuntimeNotebook(AutomationElement workspace)
{
    var notebooks = workspace.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Tab))
        .Cast<AutomationElement>().Where(item => item.Current.ClassName == "_wx_SysTabCtl32" && item.Current.NativeWindowHandle != 0).ToArray();
    Ensure(notebooks.Length == 1, $"Expected one observed runtime notebook, found {notebooks.Length}");
    return notebooks[0];
}

static AutomationElement FindRuntimeTab(AutomationElement notebook, string page)
{
    var tabs = notebook.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.TabItem))
        .Cast<AutomationElement>().Where(item => item.Current.Name == page).ToArray();
    Ensure(tabs.Length == 1, $"Runtime notebook has {tabs.Length} exact '{page}' tabs");
    return tabs[0];
}

static void SelectTab(AutomationElement workspace, string page)
{
    var tab = FindRuntimeTab(FindRuntimeNotebook(workspace), page);
    if (!tab.TryGetCurrentPattern(SelectionItemPattern.Pattern, out var pattern))
        throw new InvalidOperationException($"Runtime notebook tab '{page}' has no SelectionItemPattern");
    ((SelectionItemPattern)pattern).Select();
    WaitUntil(() => ((SelectionItemPattern)pattern).Current.IsSelected, TimeSpan.FromSeconds(3), $"Runtime notebook did not select '{page}'");
}

static AutomationElement FindPanelElement(AutomationElement workspace, string selectedPage)
{
    var notebook = FindRuntimeNotebook(workspace);
    var tab = FindRuntimeTab(notebook, selectedPage);
    if (!tab.TryGetCurrentPattern(SelectionItemPattern.Pattern, out var selected)
        || !((SelectionItemPattern)selected).Current.IsSelected)
        throw new InvalidOperationException($"Runtime '{selectedPage}' page was not selected before locating its text control");
    var controls = notebook.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>()
        .Where(item => !item.Current.IsOffscreen && item.Current.NativeWindowHandle != 0 && item.Current.ClassName == "Edit"
            && (item.Current.ControlType == ControlType.Document || item.Current.ControlType == ControlType.Edit)
            && (item.TryGetCurrentPattern(TextPattern.Pattern, out _) || item.TryGetCurrentPattern(ValuePattern.Pattern, out _)))
        .ToArray();
    Ensure(controls.Length == 1, $"Selected runtime '{selectedPage}' page has {controls.Length} visible readable Edit controls, not exactly one");
    return controls[0];
}

static string ReadPanelValue(AutomationElement control)
{
    var current = control.Current;
    Ensure(current.NativeWindowHandle != 0 && current.ClassName == "Edit"
        && (current.ControlType == ControlType.Document || current.ControlType == ControlType.Edit),
        "Cached runtime text control is no longer the observed native Edit");
    var handle = new nint(current.NativeWindowHandle);
    GetWindowThreadProcessId(handle, out var owner);
    Ensure(owner != 0 && owner == (uint)current.ProcessId, "Cached runtime Edit HWND no longer belongs to its GUI host PID");
    var classBuffer = new char[64];
    var classLength = GetClassNameW(handle, classBuffer, classBuffer.Length);
    Ensure(classLength > 0 && classLength < classBuffer.Length - 1 && new string(classBuffer, 0, classLength) == "Edit",
        "Cached runtime HWND no longer has the native Edit class");
    const int capacity = 65_536;
    var buffer = new char[capacity];
    if (SendMessageTimeoutW(handle, 0x000D, capacity, buffer, 0x0002, 2000, out var copied) == 0)
        throw new TimeoutException("Bounded WM_GETTEXT for the observed runtime Edit timed out or failed");
    var length = copied.ToInt64();
    Ensure(length >= 0 && length < capacity - 1, "Runtime Edit text exceeded the fixed WM_GETTEXT buffer; refusing truncated content");
    return new string(buffer, 0, (int)length);
}

static string ExpectedFeatureLine(string macro)
{
    var script = macro == "Apply karaoke template" ? "kara-templater" : "runtime-actions";
    return $"Feature: automation/lua/{script}/{macro}";
}

static bool HasExactLine(string text, string expected) => text.Split('\n').Any(line => line.TrimEnd('\r') == expected);

static bool HasContextOutcome(AutomationElement contextControl, string macro, string outcome)
{
    var value = ReadPanelValue(contextControl);
    return HasExactLine(value, ExpectedFeatureLine(macro)) && HasExactLine(value, $"Status: {outcome}");
}

static string WaitContextOutcome(AutomationElement contextControl, string macro, string outcome, TimeSpan timeout)
{
    string observed = "";
    WaitUntil(() =>
    {
        observed = ReadPanelValue(contextControl);
        return HasExactLine(observed, ExpectedFeatureLine(macro)) && HasExactLine(observed, $"Status: {outcome}");
    }, timeout, $"Runtime view did not report {macro} as {outcome}");
    return observed;
}

static void SaveRuntimeEvidence(AutomationElement workspace, AutomationElement contextControl, AutomationElement generatedControl, string artifacts, string name)
{
    var context = ReadPanelValue(contextControl);
    SelectTab(workspace, "Context");
    File.WriteAllText(Path.Combine(artifacts, name + "-context.txt"), context, new UTF8Encoding(false));
    SaveUiEvidence(workspace, artifacts, name + "-context");
    var generated = ReadPanelValue(generatedControl);
    SelectTab(workspace, "Generated");
    File.WriteAllText(Path.Combine(artifacts, name + "-generated.txt"), generated, new UTF8Encoding(false));
    SaveUiEvidence(workspace, artifacts, name + "-generated");
}

static void SaveUiEvidence(AutomationElement window, string artifacts, string name)
{
    var nodes = new List<string>();
    foreach (AutomationElement item in window.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().Take(500))
    {
        try
        {
            var current = item.Current;
            nodes.Add($"{current.ControlType.ProgrammaticName}\t{current.Name}\t{current.AutomationId}\t{current.ClassName}\t{current.NativeWindowHandle}\t{current.IsEnabled}\t{string.Join(',', item.GetSupportedPatterns().Select(pattern => pattern.ProgrammaticName))}");
        }
        catch (ElementNotAvailableException) { }
    }
    File.WriteAllLines(Path.Combine(artifacts, name + "-uia.txt"), nodes);
    _ = ScreenCapture.SaveWindowPng(window, Path.Combine(artifacts, name + ".png"));
}

static void TryEvidence(AutomationElement window, string artifacts, string name)
{
    try { SaveUiEvidence(window, artifacts, name); } catch { }
}

static void LogStage(string artifacts, string stage)
{
    File.AppendAllText(Path.Combine(artifacts, "action-stages.log"), $"{DateTimeOffset.UtcNow:O}\t{stage}{Environment.NewLine}");
}

static void SaveProcessUiEvidence(Process host, string artifacts, string name)
{
    var roots = AutomationElement.RootElement.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ProcessIdProperty, host.Id));
    var lines = new List<string>();
    var remaining = 2000;
    var rootIndex = 0;
    foreach (AutomationElement root in roots)
    {
        if (remaining <= 0) break;
        lines.Add($"ROOT {rootIndex}");
        foreach (var item in new[] { root }.Concat(root.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>()))
        {
            if (remaining <= 0) break;
            --remaining;
            try
            {
                var current = item.Current;
                lines.Add($"{current.ControlType.ProgrammaticName}\t{current.Name}\t{current.AutomationId}\t{current.ClassName}\t{current.NativeWindowHandle}\t{current.IsEnabled}\t{current.IsOffscreen}\t{string.Join(',', item.GetSupportedPatterns().Select(pattern => pattern.ProgrammaticName))}");
            }
            catch (ElementNotAvailableException) { lines.Add("<stale UIA element>"); }
        }
        try
        {
            if (root.Current.NativeWindowHandle != 0 && !root.Current.IsOffscreen)
            {
                var capture = ScreenCapture.SaveWindowPng(root, Path.Combine(artifacts, $"{name}-root-{rootIndex}.png"));
                lines.Add($"ROOT {rootIndex} capture={capture.Width}x{capture.Height} nonblack={capture.NonBlackPixelCount}");
            }
        }
        catch (Exception error) { lines.Add($"ROOT {rootIndex} capture-error={error.GetType().Name}:{error.Message}"); }
        ++rootIndex;
    }
    lines.Add($"remaining-node-budget={remaining}");
    File.WriteAllLines(Path.Combine(artifacts, name + "-uia.txt"), lines);
}

static void TryProcessUiEvidence(Process host, string artifacts, string name)
{
    try { SaveProcessUiEvidence(host, artifacts, name); }
    catch (Exception error) { File.WriteAllText(Path.Combine(artifacts, name + "-error.txt"), $"{error.GetType().Name}:{error.Message}"); }
}

static AutomationElement FindMenu(AutomationElement main)
{
    return main.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem))
        .Cast<AutomationElement>().FirstOrDefault(item => (item.Current.Name ?? "").Replace("&", "").Contains("Automation", StringComparison.OrdinalIgnoreCase))
        ?? throw new InvalidOperationException("Automation menu is unavailable");
}

static AutomationElement? FindVisibleMenuCommand(Process host, string commandName)
{
    var roots = AutomationElement.RootElement.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ProcessIdProperty, host.Id));
    foreach (AutomationElement root in roots)
    {
        var found = root.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem))
            .Cast<AutomationElement>().FirstOrDefault(item => !item.Current.IsOffscreen
                && (item.Current.Name ?? "").Replace("&", "").Contains(commandName, StringComparison.OrdinalIgnoreCase));
        if (found is not null) return found;
    }
    return null;
}

static AutomationElement OpenAutomationMenuCommand(AutomationElement main, Process host, string commandName, TimeSpan timeout, string menuName = "Automation", bool forceOpen = false)
{
    var direct = main.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem))
        .Cast<AutomationElement>().FirstOrDefault(item => (item.Current.Name ?? "").Replace("&", "").Contains(commandName, StringComparison.OrdinalIgnoreCase));
    if (!forceOpen && direct is not null) return direct;
    var menu = menuName == "Automation" ? FindMenu(main) : main.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem))
        .Cast<AutomationElement>().FirstOrDefault(item => (item.Current.Name ?? "").Replace("&", "").Contains(menuName, StringComparison.OrdinalIgnoreCase))
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
    if (!opened)
    {
        var key = menuName == "Automation" ? 'u' : menuName == "Edit" ? 'e' : throw new InvalidOperationException("No observed menu mnemonic");
        PostSystemMenu(new nint(main.Current.NativeWindowHandle), key);
    }
    AutomationElement? command = null;
    WaitUntil(() => (command = FindVisibleMenuCommand(host, commandName)) is not null, timeout, $"Menu command '{commandName}' was not exposed");
    return command!;
}

static void InvokeMenu(AutomationElement main, Process host, string command, TimeSpan timeout, string menuName = "Automation")
{
    UiaDriver.Invoke(OpenAutomationMenuCommand(main, host, command, timeout, menuName));
}

static void InvokeExactButton(AutomationElement window, string name)
{
    var button = window.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button))
        .Cast<AutomationElement>().FirstOrDefault(item => item.Current.Name == name && item.Current.IsEnabled && item.TryGetCurrentPattern(InvokePattern.Pattern, out _))
        ?? throw new InvalidOperationException($"Expected dialog has no enabled exact '{name}' button");
    UiaDriver.Invoke(button);
}

static bool HasExactButton(AutomationElement window, string name)
{
    try
    {
        return window.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button))
            .Cast<AutomationElement>().Any(item => item.Current.Name == name && item.Current.IsEnabled && item.TryGetCurrentPattern(InvokePattern.Pattern, out _));
    }
    catch (ElementNotAvailableException) { return false; }
}

static void InvokeMacroWithOptionalProgress(AutomationElement main, Process host, AutomationElement contextControl, string macro, string artifacts, string artifactName, string expectedOutcome)
{
    var task = Task.Run(() => InvokeMenu(main, host, macro, TimeSpan.FromSeconds(8)));
    var closedProgress = false;
    WaitUntil(() =>
    {
        var progress = FindProgressPane(host, macro);
        if (!closedProgress && progress is not null && HasExactButton(progress, "Close"))
        {
            SaveUiEvidence(progress, artifacts, artifactName + "-progress-close");
            InvokeExactButton(progress, "Close");
            closedProgress = true;
        }
        return HasContextOutcome(contextControl, macro, expectedOutcome) && task.IsCompleted;
    }, TimeSpan.FromSeconds(25), $"{macro} did not reach {expectedOutcome} runtime observation and finish its progress UI");
    if (!task.Wait(TimeSpan.FromSeconds(10))) throw new TimeoutException($"{macro} did not finish after progress completion");
}

static void InvokeMacroWithErrorProgress(AutomationElement main, Process host, string macro, string artifacts, string name)
{
    LogStage(artifacts, $"error-progress:{name}:invoke-start");
    var task = Task.Run(() => InvokeMenu(main, host, macro, TimeSpan.FromSeconds(8)));
    LogStage(artifacts, $"error-progress:{name}:invoke-queued");
    TryProcessUiEvidence(host, artifacts, name + "-before-progress-wait");
    LogStage(artifacts, $"error-progress:{name}:wait-window");
    var progress = WaitProgressPane(host, macro, TimeSpan.FromSeconds(8));
    LogStage(artifacts, $"error-progress:{name}:progress-found");
    WaitUntil(() => HasExactButton(progress, "Close"), TimeSpan.FromSeconds(8), $"{macro} error progress did not expose its completed Close button");
    SaveUiEvidence(progress, artifacts, name + "-progress-close");
    InvokeExactButton(progress, "Close");
    if (!task.Wait(TimeSpan.FromSeconds(10))) throw new TimeoutException($"{macro} did not finish after its exact progress Close action");
}

static void CloseWorkspace(AutomationElement workspace)
{
    if (workspace.TryGetCurrentPattern(WindowPattern.Pattern, out var pattern))
    {
        ((WindowPattern)pattern).Close();
        return;
    }
    var titleBar = workspace.FindFirst(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.TitleBar))
        ?? throw new InvalidOperationException("Workspace has no UIA WindowPattern or title bar");
    var close = titleBar.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button))
        .Cast<AutomationElement>().FirstOrDefault(item => (item.Current.Name is "Close" or "关闭") && item.TryGetCurrentPattern(InvokePattern.Pattern, out _))
        ?? throw new InvalidOperationException("Workspace has no observed title-bar Close button");
    UiaDriver.Invoke(close);
}

[DllImport("user32.dll", EntryPoint = "PostMessageW", ExactSpelling = true, SetLastError = true)]
static extern int PostMessageW(nint hwnd, uint message, nint wParam, nint lParam);

[DllImport("user32.dll", SetLastError = true)]
static extern uint GetWindowThreadProcessId(nint hwnd, out uint processId);

[DllImport("user32.dll", EntryPoint = "GetClassNameW", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
static extern int GetClassNameW(nint hwnd, [Out] char[] className, int maxCount);

[DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
static extern nint SendMessageTimeoutW(nint hwnd, uint message, nint wParam, [Out] char[] buffer, uint flags, uint timeout, out nint result);

static void PostSystemMenu(nint hwnd, char mnemonic)
{
    if (PostMessageW(hwnd, 0x0112, 0xF100, mnemonic) == 0)
        throw new InvalidOperationException("Observed menu mnemonic could not be posted to the GUI host");
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
        return new AssEvent(line[..separator], int.Parse(fields[0]), ParseAssTime(fields[1]), ParseAssTime(fields[2]), fields[3].Trim(), fields[4].Trim(),
            int.Parse(fields[5]), int.Parse(fields[6]), int.Parse(fields[7]), fields[8].Trim(), fields[9]);
    }).ToList();

static void AssertPhysicalEventLines(string ass, int expectedCount)
{
    var lines = ass.Split('\n').Select(line => line.TrimEnd('\r')).ToArray();
    var section = Array.FindIndex(lines, line => line == "[Events]");
    if (section < 0) throw new FormatException("Saved ASS has no Events section");
    var count = 0;
    foreach (var line in lines.Skip(section + 1))
    {
        if (line.Length == 0) continue;
        if (line.StartsWith('[')) break;
        if (line.StartsWith("Format:", StringComparison.Ordinal)) continue;
        if (!line.StartsWith("Comment:", StringComparison.Ordinal) && !line.StartsWith("Dialogue:", StringComparison.Ordinal))
            throw new FormatException("Saved ASS contains an orphan physical continuation line");
        ++count;
    }
    Ensure(count == expectedCount, "Saved ASS physical event count differs from the independent expectation");
}

sealed record AssEvent(string Kind, int Layer, int StartMs, int EndMs, string Style, string Name, int MarginL, int MarginR, int MarginV, string Effect, string Text);
sealed record StepResult(string Name, string Status, string Detail);
sealed record RuntimeExpected(int GeneratedCount, List<ExpectedGenerated> Generated);
sealed record ExpectedGenerated(string Text, int Layer, int StartMs, int EndMs);
sealed record HostIdentity(int ProcessId, string StartTimeUtc);
sealed record TemplateFailureCase(string Name, string ArtifactName, string ExpectedKind, int SourceLine, string SourceText, string? RuntimeMarker);
