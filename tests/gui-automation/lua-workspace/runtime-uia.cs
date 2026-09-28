#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:property InvariantGlobalization=false
#:project ../driver/Aegisub.GuiAutomation.Driver.csproj

using System.Diagnostics;
using System.Globalization;
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
    var generatedHistory = args.Zip(args.Skip(1)).Any(pair => pair.First == "--scenario" && pair.Second == "generated-history");
    var budgetSeconds = generatedHistory ? 660 : 1440;
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
    var workerBudget = TimeSpan.FromSeconds(budgetSeconds - 10) - started.Elapsed;
    var timedOut = workerBudget <= TimeSpan.Zero || !worker.WaitForExit(workerBudget);
    if (timedOut)
    {
        worker.Kill(entireProcessTree: true);
        if (!worker.WaitForExit(TimeSpan.FromSeconds(5))) throw new TimeoutException("Runtime UIA worker did not stop after timeout");
    }
    var hostLeftRunning = false;
    var readyPaths = generatedHistory
        ? new[] { "branches", "capacity", "mutated-input", "cancel" }.Select(name => Path.Combine(artifacts, name, "ready.json"))
        : new[] { readyPath };
    foreach (var candidateReadyPath in readyPaths.Where(File.Exists))
    {
        var hostId = JsonDocument.Parse(File.ReadAllText(candidateReadyPath)).RootElement.GetProperty("process_id").GetInt32();
        var candidateIdentityPath = generatedHistory ? Path.Combine(Path.GetDirectoryName(candidateReadyPath)!, "host-identity.json") : identityPath;
        var identity = File.Exists(candidateIdentityPath)
            ? JsonSerializer.Deserialize<HostIdentity>(File.ReadAllText(candidateIdentityPath))
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
        BudgetSeconds = budgetSeconds,
        TimedOut = timedOut,
        HostLeftRunning = hostLeftRunning,
        WorkerExitCode = worker.ExitCode,
        Status = timedOut ? "timeout" : hostLeftRunning ? "host-cleanup-required" : worker.ExitCode == 0 ? "passed" : "failed"
    }, new JsonSerializerOptions { WriteIndented = true }));
    if (timedOut) throw new TimeoutException($"Lua Workspace runtime GUI E2E exceeded its {budgetSeconds}-second total limit");
    return hostLeftRunning ? 1 : worker.ExitCode;
}

static int Run(string[] args)
{
    string? exe = null;
    string? artifacts = null;
    string? scenario = null;
    for (var index = 0; index < args.Length; ++index)
    {
        switch (args[index])
        {
            case "--exe": exe = Path.GetFullPath(args[++index]); break;
            case "--artifacts": artifacts = Path.GetFullPath(args[++index]); break;
            case "--scenario": scenario = args[++index]; break;
            default: throw new ArgumentException($"Unknown argument: {args[index]}");
        }
    }
    if (exe is null || !File.Exists(exe) || artifacts is null)
        throw new ArgumentException("--exe and --artifacts are required");
    if (scenario == "generated-history") return RunGeneratedHistory(exe, artifacts);
    if (scenario is not null) throw new ArgumentException($"Unknown runtime scenario: {scenario}");
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
            var task = StartMenuInvocation(main, host, "Workspace Runtime Hidden Acknowledgement");
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
            Ensure(HasExactLine(completedContext, "Event type: generated-line") && HasExactLine(completedContext, "Template type: syl"),
                "Final event type and template type are not independently identified");
            Ensure(ContextNumber(completedContext, "Template ID") > 0
                && HasExactLine(completedContext, "Template name: [not captured]")
                && HasExactLine(completedContext, "Owner script: kara-templater.lua"),
                "Template debug ID, optional authored name, or owner script is incorrect");
            Ensure(HasExactLine(completedContext, "Phase: syl-text") && HasExactLine(completedContext, "Scope: syl"), "Final template phase or scope is incorrect");
            Ensure(HasExactLine(completedContext, "Template fragment kind: text-template")
                && HasExactLine(completedContext, "Source fragments: 1 total, 1 shown")
                && HasExactLine(completedContext, "Source fragment 1:")
                && HasExactLine(completedContext, "  kind: syl")
                && HasExactLine(completedContext, "  line: 11")
                && HasExactLine(completedContext, "  text: !retime(\"syl\",0,0)!S!j!:!decorate(syl.text_stripped)!"),
                "Final source fragment does not identify the executed template text");
            Ensure(completedContext.Contains("Loop index: 2", StringComparison.Ordinal) && completedContext.Contains("Loop count: 2", StringComparison.Ordinal), "Final loop context is incorrect");
            Ensure(completedContext.Contains("Source line: 11", StringComparison.Ordinal)
                && completedContext.Contains("Source text: !retime(\"syl\",0,0)!S!j!:!decorate(syl.text_stripped)!", StringComparison.Ordinal), "Final source template context is incorrect");
            Ensure(completedContext.Contains("Target line: 13", StringComparison.Ordinal)
                && completedContext.Contains($"Target text: {last.Text}", StringComparison.Ordinal), "Final target line context is incorrect");
            Ensure(completedContext.Contains("Syllable index: 2", StringComparison.Ordinal)
                && completedContext.Contains("Syllable text: delta", StringComparison.Ordinal), "Final syllable context is incorrect");
            Ensure(HasExactLine(completedContext, "Template event snapshot: latest reported event, not live values from a paused Lua frame.")
                && HasExactLine(completedContext, "Debug pause: not attached to this Automation-menu invocation"),
                "Template snapshot was confused with a current debugger pause");
            Ensure(HasExactLine(completedContext, "Template source revision: not captured (saved Automation source)")
                && completedContext.Split('\n').Select(line => line.TrimEnd('\r'))
                    .Any(line => line.StartsWith("Workspace editor revision at observation ", StringComparison.Ordinal)
                        && line.EndsWith(" (current)", StringComparison.Ordinal)),
                "Completed menu invocation confused the Workspace editor revision with the saved template source");
            Ensure(HasExactLine(completedContext, "PlayRes X: 1280") && HasExactLine(completedContext, "PlayRes Y: 720"),
                "Script PlayRes geometry does not match the ASS fixture");
            Ensure(HasExactLine(completedContext, "Orgline index: 13")
                && HasExactLine(completedContext, "Orgline layer: 0")
                && HasExactLine(completedContext, "Orgline text: {\\k75}gamma{\\k75}delta")
                && HasExactLine(completedContext, "Orgline start (ms): 3000")
                && HasExactLine(completedContext, "Orgline end (ms): 4500")
                && HasExactLine(completedContext, "Current line index: 13")
                && HasExactLine(completedContext, "Current line layer: 2")
                && HasExactLine(completedContext, $"Current line text: {last.Text}")
                && HasExactLine(completedContext, "Current line start (ms): 3750")
                && HasExactLine(completedContext, "Current line end (ms): 4500")
                && HasExactLine(completedContext, "Line text change: changed")
                && HasExactLine(completedContext, "Line timing change: changed")
                && HasExactLine(completedContext, "Line layer change: changed")
                && HasExactLine(completedContext, "Line style change: unchanged")
                && HasExactLine(completedContext, "Line effect change: changed"),
                "Original and current line snapshots or their changes do not match the generated result");
            Ensure(HasExactLine(completedContext,
                "Syllable timing is relative to the input line; karaskel geometry uses PlayRes/script coordinates, not screen pixels."),
                "Context did not identify the unit and coordinate domain of syllable geometry");
            foreach (var prefix in new[] { "Syllable", "Base syllable" })
            {
                Ensure(HasExactLine(completedContext, $"{prefix} index: 2")
                    && HasExactLine(completedContext, $"{prefix} text: delta")
                    && HasExactLine(completedContext, $"{prefix} start relative to line (ms): 750")
                    && HasExactLine(completedContext, $"{prefix} end relative to line (ms): 1500")
                    && HasExactLine(completedContext, $"{prefix} duration (ms): 750")
                    && HasExactLine(completedContext, $"{prefix} inline_fx: "),
                    $"{prefix} source, relative timing, or empty inline_fx was not retained");
                var left = ContextNumber(completedContext, $"{prefix} left");
                var center = ContextNumber(completedContext, $"{prefix} center");
                var right = ContextNumber(completedContext, $"{prefix} right");
                var width = ContextNumber(completedContext, $"{prefix} width");
                var height = ContextNumber(completedContext, $"{prefix} height");
                Ensure(left < center && center < right && width > 0 && height > 0
                    && Math.Abs(right - left - width) <= 1.0,
                    $"{prefix} karaskel geometry is inconsistent");
            }
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
            var task = StartMenuInvocation(main, host, "Workspace Runtime Dialog Cancel");
            var dialog = WaitWindowContainingText(host, "Cancel only this dialog; the invocation must complete", TimeSpan.FromSeconds(8));
            SaveUiEvidence(dialog, artifacts, "dialog-cancel-prompt");
            InvokeExactButton(dialog, "Cancel");
            if (!task.Wait(TimeSpan.FromSeconds(10))) throw new TimeoutException("Dialog Cancel macro did not finish");
            EnsureOutcomeAndRollback("Workspace Runtime Dialog Cancel", "completed", "dialog-cancel", verifyRollback: false);
        });
        Step("progress-cancel", () =>
        {
            var task = StartMenuInvocation(main, host, "Workspace Runtime Progress Cancel");
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
            var task = StartMenuInvocation(main, host, "Workspace Runtime Hidden Acknowledgement");
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
            Ensure(HasExactLine(failedContext, "Event type: " + scenario.ExpectedKind),
                $"{scenario.Name} did not retain the exact template failure kind");
            Ensure(HasExactLine(failedContext, "Source line: " + scenario.SourceLine)
                && HasExactLine(failedContext, "Source style: Default")
                && HasExactLine(failedContext, "Source text: " + scenario.SourceText),
                $"{scenario.Name} did not retain the prepared source-line identity and text");
            if (scenario.SourceLine == 9)
                Ensure(HasExactLine(failedContext, "Scope: once")
                    && HasExactLine(failedContext, "Orgline: [not applicable]")
                    && HasExactLine(failedContext, "Current line: [not applicable]")
                    && HasExactLine(failedContext, "Syllable: [not applicable]")
                    && HasExactLine(failedContext, "Base syllable: [not applicable]"),
                    $"{scenario.Name} misrepresented non-applicable syllable fields as zero or captured values");
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
            var task = StartMenuInvocation(main, host, "Workspace Runtime Progress Cancel");
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
        BudgetSeconds = 1440,
        ExeSha256 = hashes["exe"],
        Fixtures = new[] { fixture.Replace('\\', '/'), expectedPath.Replace('\\', '/'), actions.Replace('\\', '/'), templater.Replace('\\', '/') },
        Sha256 = hashes,
        ExitStatus = exitStatus,
        Steps = results
    }, new JsonSerializerOptions { WriteIndented = true }));

}

static int RunGeneratedHistory(string exe, string artifacts)
{
    Directory.CreateDirectory(artifacts);
    var startedUtc = DateTimeOffset.UtcNow;
    var templater = Path.Combine("automation", "autoload", "kara-templater.lua");
    var actions = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "generated-history-actions.lua");
    var includeRoot = Path.Combine("automation", "include");
    var driver = Path.Combine("tests", "gui-automation", "lua-workspace", "runtime-uia.cs");
    var hashes = new Dictionary<string, string>
    {
        ["exe"] = Hash(exe), ["templater"] = Hash(templater), ["actions"] = Hash(actions),
        ["driver_source"] = Hash(driver), ["executed_driver"] = Hash(Assembly.GetEntryAssembly()?.Location
            ?? throw new InvalidOperationException("Executed driver assembly is unavailable"))
    };
    var includeFiles = Directory.GetFiles(includeRoot, "*", SearchOption.AllDirectories);
    foreach (var file in includeFiles)
        hashes[Path.Combine(includeRoot, Path.GetRelativePath(includeRoot, file)).Replace('\\', '/')] = Hash(file);
    var steps = new[] { "branches", "capacity", "mutated-input", "cancel" };
    var results = new List<StepResult>();
    var exitStatus = "running";
    try
    {
        foreach (var name in steps)
        {
            var fixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", $"generated-history-{name}.ass");
            var expectedPath = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", $"generated-history-{name}.expected.json");
            hashes[$"{name}_fixture"] = Hash(fixture);
            hashes[$"{name}_expected"] = Hash(expectedPath);
            var caseDir = Path.Combine(artifacts, name);
            Directory.CreateDirectory(caseDir);
            var input = Path.Combine(caseDir, "input.ass");
            File.Copy(fixture, input);
            File.Copy(actions, Path.Combine(caseDir, "generated-history-actions.lua"));
            File.Copy(templater, Path.Combine(caseDir, "kara-templater.lua"));
            foreach (var file in includeFiles)
            {
                var destination = Path.Combine(caseDir, "include", Path.GetRelativePath(includeRoot, file));
                Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                File.Copy(file, destination);
            }
            var originalEvents = EventLines(File.ReadAllText(input));
            var originalBytes = File.ReadAllBytes(input);
            var profile = Path.Combine(caseDir, "profile");
            Directory.CreateDirectory(profile);
            Process? host = null;
            AutomationElement? workspace = null;
            try
            {
                var start = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe)! };
                foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", caseDir, "--open", input })
                    start.ArgumentList.Add(argument);
                host = Process.Start(start) ?? throw new InvalidOperationException($"Could not start {name} GUI host");
                File.WriteAllText(Path.Combine(caseDir, "host-identity.json"), JsonSerializer.Serialize(new HostIdentity(host.Id, host.StartTime.ToUniversalTime().ToString("O"))));
                var ready = AutomationProtocol.WaitForReadyArtifact(Path.Combine(caseDir, "ready.json"), host, TimeSpan.FromSeconds(15));
                Ensure(ready.ProcessId == host.Id, $"{name} ready artifact belongs to another process");
                var main = UiaDriver.WaitForMainWindow(host, TimeSpan.FromSeconds(15));
                InvokeMenu(main, host, "Generated History Select Code", TimeSpan.FromSeconds(8));
                WaitUntil(() => HasSelectedEffect(main, "code once"), TimeSpan.FromSeconds(5), $"{name} code source was not selected");
                InvokeMenu(main, host, "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(8));
                workspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(8));
                SelectTab(workspace, "Context");
                var contextControl = FindPanelElement(workspace, "Context");
                SelectTab(workspace, "Generated");
                var generatedControl = FindPanelElement(workspace, "Generated");
                var historyList = FindGeneratedHistoryList(workspace);
                if (name == "cancel")
                {
                    var expected = ReadExpected<HistoryCancelExpected>(expectedPath);
                    Ensure(originalEvents.Count == expected.OriginalEventCount, "Cancel fixture event count differs from its independent expectation");
                    var task = StartMenuInvocation(main, host, "Apply karaoke template");
                    var progress = WaitProgressPane(host, "Apply karaoke template", TimeSpan.FromSeconds(8));
                    WaitUntil(() => HasExactLine(ReadPanelValue(generatedControl), "Generated ASS state: provisional; not committed")
                        && ContextNumber(ReadPanelValue(generatedControl), "Retained generated lines") >= expected.MinimumProvisionalGenerated,
                        TimeSpan.FromSeconds(20), "No provisional generated line was observed before cancellation");
                    var provisional = ReadPanelValue(generatedControl);
                    var provisionalCount = ContextNumber(provisional, "Retained generated lines");
                    Ensure(provisionalCount <= expected.MaximumProvisionalGenerated, "Cancellation fixture completed before the required partial-output checkpoint");
                    Ensure(HasExactLine(provisional, $"Input source line index: {expected.InputSourceLineIndex}")
                        && HasExactLine(provisional, $"Template source line: {expected.TemplateSourceLine}")
                        && provisional.Contains($"Output text: {expected.TextPrefix}", StringComparison.Ordinal),
                        "Provisional output lacks independent source and text fields");
                    File.WriteAllText(Path.Combine(caseDir, "provisional-generated.txt"), provisional, new UTF8Encoding(false));
                    InvokeExactButton(progress, "Cancel");
                    if (!task.Wait(TimeSpan.FromSeconds(15))) throw new TimeoutException("Generated-output cancellation did not finish after exact Cancel");
                    WaitContextOutcome(contextControl, "Apply karaoke template", "cancelled", TimeSpan.FromSeconds(8));
                    var rolledBack = ReadPanelValue(generatedControl);
                    Ensure(HasExactLine(rolledBack, "Generated ASS state: rolled back")
                        && ContextNumber(rolledBack, "Retained generated lines") >= expected.MinimumProvisionalGenerated,
                        "Cancelled generated output was not retained as rolled-back evidence");
                    File.WriteAllText(Path.Combine(caseDir, "rolled-back-generated.txt"), rolledBack, new UTF8Encoding(false));
                    SaveRuntimeEvidence(workspace, contextControl, generatedControl, caseDir, "rolled-back");
                    Ensure(File.ReadAllBytes(input).SequenceEqual(originalBytes), "Cancelled macro changed physical ASS before verification");
                    InvokeMacroWithOptionalProgress(main, host, contextControl, "Generated History Verify Rollback", caseDir,
                        "rollback-verifier", "completed");
                    Ensure(File.ReadAllBytes(input).SequenceEqual(originalBytes), "Rollback verification changed physical ASS");
                }
                else
                {
                    InvokeMacroWithOptionalProgress(main, host, contextControl, "Apply karaoke template", caseDir, name, "completed");
                    WaitContextOutcome(contextControl, "Apply karaoke template", "completed", TimeSpan.FromSeconds(8));
                    var generated = ReadPanelValue(generatedControl);
                    Ensure(HasExactLine(generated, "Generated ASS state: committed by invocation"), $"{name} generated history did not reach committed state");
                    if (name == "branches")
                    {
                        var expected = ReadExpected<HistoryBranchesExpected>(expectedPath);
                        Ensure(expected.Generated.Count == expected.GeneratedCount && originalEvents.Count == 6, "Branch expectations or fixture count are inconsistent");
                        var latestContext = ReadPanelValue(contextControl);
                        Ensure(HasExactLine(latestContext, "Template type: syl")
                            && HasExactLine(latestContext, "Template authored kinds: syl, furi")
                            && HasExactLine(latestContext, "Scope: furi"),
                            "Shared syl/furi template lost authored kinds or actual applied scope");
                        Ensure(HistoryItems(historyList).Length == expected.GeneratedCount, "Branch history omitted or duplicated generated outputs");
                        for (var index = 0; index < expected.GeneratedCount; ++index)
                        {
                            var line = expected.Generated[index];
                            var detail = SelectGeneratedHistory(workspace, historyList, generatedControl, index + 1);
                            AssertGeneratedHistoryDetail(detail, index + 1, line, expected.InputSourceLineIndex, expected.InputText,
                                expected.InputStartMs, expected.InputEndMs, originalEvents[line.TemplateSourceLine - 9].Text);
                            if (index is 1 or 3 or 5 or 15)
                            {
                                File.WriteAllText(Path.Combine(caseDir, $"selected-{index + 1}.txt"), detail, new UTF8Encoding(false));
                                SaveUiEvidence(workspace, caseDir, $"selected-{index + 1}");
                            }
                        }
                    }
                    else if (name == "mutated-input")
                    {
                        var expected = ReadExpected<HistoryMutatedInputExpected>(expectedPath);
                        Ensure(originalEvents.Count == 4 && HistoryItems(historyList).Length == expected.GeneratedCount,
                            "Mutated-input fixture or generated history count differs from independent expectation");
                        var latestContext = ReadPanelValue(contextControl);
                        Ensure(HasExactLine(latestContext, $"Orgline index: {expected.InputSourceLineIndex}")
                            && HasExactLine(latestContext, $"Orgline text: {expected.InputText}")
                            && HasExactLine(latestContext, $"Orgline start (ms): {expected.InputStartMs}")
                            && HasExactLine(latestContext, $"Orgline end (ms): {expected.InputEndMs}"),
                            "Context attributed generated output to the code-mutated input instead of its captured source");
                        for (var index = 1; index <= expected.GeneratedCount; ++index)
                        {
                            var detail = SelectGeneratedHistory(workspace, historyList, generatedControl, index);
                            Ensure(HasExactLine(detail, $"Output index: {index}")
                                && HasExactLine(detail, $"Output text: {expected.OutputTextPrefix}{index}")
                                && HasExactLine(detail, $"Output layer: {expected.OutputLayer}")
                                && HasExactLine(detail, $"Output style: {expected.OutputStyle}")
                                && HasExactLine(detail, $"Output start (ms): {expected.OutputStartMs}")
                                && HasExactLine(detail, $"Output end (ms): {expected.OutputEndMs}")
                                && HasExactLine(detail, $"Input source line index: {expected.InputSourceLineIndex}")
                                && HasExactLine(detail, $"Input snapshot index: {expected.InputSourceLineIndex}")
                                && HasExactLine(detail, $"Input text: {expected.InputText}")
                                && HasExactLine(detail, $"Input start (ms): {expected.InputStartMs}")
                                && HasExactLine(detail, $"Input end (ms): {expected.InputEndMs}")
                                && HasExactLine(detail, $"Template source line: {expected.TemplateSourceLine}")
                                && HasExactLine(detail, $"Template debug ID: {expected.TemplateDebugId}")
                                && HasExactLine(detail, "Template primary authored kind: line")
                                && HasExactLine(detail, "Template authored kinds: line")
                                && HasExactLine(detail, $"Template source text: {expected.TemplateSourceText}")
                                && HasExactLine(detail, "Applied scope: line"),
                                $"Mutated-input output #{index} lost immutable input provenance");
                            File.WriteAllText(Path.Combine(caseDir, $"selected-{index}.txt"), detail, new UTF8Encoding(false));
                        }
                    }
                    else
                    {
                        var expected = ReadExpected<HistoryCapacityExpected>(expectedPath);
                        Ensure(originalEvents.Count == 3 && expected.GeneratedCount > expected.RetainedCount,
                            "Capacity expectations or fixture count are inconsistent");
                        Ensure(HasExactLine(generated, $"Reported generated count: {expected.GeneratedCount}")
                            && HasExactLine(generated, $"Retained generated lines: {expected.RetainedCount}")
                            && HasExactLine(generated, $"Omitted generated lines: {expected.OmittedCount} [oldest dropped]")
                            && HasExactLine(generated, "Unobserved generated indices: 0")
                            && RetainedPreviewBytes(generated) is > 0 and <= 262144,
                            "Capacity counts do not match independent 140/128/12 expectation");
                        var items = HistoryItems(historyList);
                        Ensure(items.Length == expected.RetainedCount && items[0].Current.Name.StartsWith($"#{expected.FirstRetainedIndex} ", StringComparison.Ordinal)
                            && items[^1].Current.Name.StartsWith($"#{expected.GeneratedCount} ", StringComparison.Ordinal)
                            && !items.Any(item => item.Current.Name.StartsWith($"#{expected.FirstRetainedIndex - 1} ", StringComparison.Ordinal)),
                            "Capacity history did not retain precisely the newest 128 indices");
                        foreach (var index in new[] { expected.FirstRetainedIndex, expected.GeneratedCount - 1, expected.GeneratedCount })
                        {
                            var detail = SelectGeneratedHistory(workspace, historyList, generatedControl, index);
                            Ensure(HasExactLine(detail, $"Output index: {index}")
                                && HasExactLine(detail, $"Output text: {expected.TextPrefix}{index}")
                                && HasExactLine(detail, $"Output layer: {expected.Layer}")
                                && HasExactLine(detail, $"Output style: {expected.Style}")
                                && HasExactLine(detail, $"Output start (ms): {expected.StartMs}")
                                && HasExactLine(detail, $"Output end (ms): {expected.EndMs}")
                                && HasExactLine(detail, $"Input source line index: {expected.InputSourceLineIndex}")
                                && HasExactLine(detail, $"Input text: {expected.InputText}")
                                && HasExactLine(detail, $"Template source line: {expected.TemplateSourceLine}"),
                                $"Capacity output #{index} lacks its independently expected provenance");
                            File.WriteAllText(Path.Combine(caseDir, $"selected-{index}.txt"), detail, new UTF8Encoding(false));
                        }
                    }
                    WaitUntil(() => UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles") is not null,
                        TimeSpan.FromSeconds(8), $"{name} main Save did not become available");
                    UiaDriver.Invoke(UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles")!);
                    var wantedCount = name == "branches" ? ReadExpected<HistoryBranchesExpected>(expectedPath).GeneratedCount
                        : name == "capacity" ? ReadExpected<HistoryCapacityExpected>(expectedPath).GeneratedCount
                        : ReadExpected<HistoryMutatedInputExpected>(expectedPath).GeneratedCount;
                    WaitUntil(() => EventLines(File.ReadAllText(input)).Count == originalEvents.Count + wantedCount,
                        TimeSpan.FromSeconds(8), $"{name} saved ASS did not contain every generated output");
                    var saved = File.ReadAllText(input);
                    File.WriteAllText(Path.Combine(caseDir, "output.ass"), saved, new UTF8Encoding(false));
                    var events = EventLines(saved);
                    for (var index = 0; index < originalEvents.Count; ++index)
                    {
                        var original = originalEvents[index];
                        var actual = events[index];
                        if (name == "mutated-input" && index == originalEvents.Count - 1)
                        {
                            var expected = ReadExpected<HistoryMutatedInputExpected>(expectedPath);
                            Ensure(actual.Kind == "Comment" && actual.Effect == "karaoke"
                                && actual.Text == expected.MutatedLiveInputText
                                && actual.StartMs == expected.MutatedLiveInputStartMs
                                && actual.EndMs == expected.MutatedLiveInputEndMs
                                && actual with { Kind = original.Kind, Effect = original.Effect, Text = original.Text,
                                    StartMs = original.StartMs, EndMs = original.EndMs } == original,
                                "Code-line mutation did not produce the independently expected live ASS input");
                        }
                        else
                            Ensure(actual with { Kind = original.Kind, Effect = original.Effect } == original
                                && (index == originalEvents.Count - 1 ? actual.Kind == "Comment" && actual.Effect == "karaoke"
                                    : actual.Kind == original.Kind && actual.Effect == original.Effect),
                                $"{name} original event {index + 1} changed unexpectedly");
                    }
                    for (var index = 0; index < wantedCount; ++index)
                    {
                        var actual = events[originalEvents.Count + index];
                        if (name == "branches")
                        {
                            var wanted = ReadExpected<HistoryBranchesExpected>(expectedPath).Generated[index];
                            Ensure(actual.Kind == "Dialogue" && actual.Effect == "fx" && actual.Text == wanted.Text
                                && actual.Layer == wanted.Layer && actual.Style == wanted.Style
                                && actual.StartMs == wanted.StartMs && actual.EndMs == wanted.EndMs,
                                $"Branch saved output #{index + 1} differs from independent expected fields");
                        }
                        else if (name == "capacity")
                        {
                            var wanted = ReadExpected<HistoryCapacityExpected>(expectedPath);
                            Ensure(actual.Kind == "Dialogue" && actual.Effect == "fx" && actual.Text == $"{wanted.TextPrefix}{index + 1}"
                                && actual.Layer == wanted.Layer && actual.Style == wanted.Style
                                && actual.StartMs == wanted.StartMs && actual.EndMs == wanted.EndMs,
                                $"Capacity saved output #{index + 1} differs from independent expected formula");
                        }
                        else
                        {
                            var wanted = ReadExpected<HistoryMutatedInputExpected>(expectedPath);
                            Ensure(actual.Kind == "Dialogue" && actual.Effect == "fx"
                                && actual.Text == $"{wanted.OutputTextPrefix}{index + 1}"
                                && actual.Layer == wanted.OutputLayer && actual.Style == wanted.OutputStyle
                                && actual.StartMs == wanted.OutputStartMs && actual.EndMs == wanted.OutputEndMs,
                                $"Mutated-input saved output #{index + 1} differs from independent expectation");
                        }
                    }
                }
                SaveRuntimeEvidence(workspace, contextControl, generatedControl, caseDir, "final");
                if (name == "branches")
                {
                    var expected = ReadExpected<HistoryBranchesExpected>(expectedPath);
                    var editor = FindWorkspaceSourceEditor(workspace, host);
                    editor.SetFocus();
                    if (PostMessageW(new nint(editor.Current.NativeWindowHandle), 0x0102, (nint)(int)'x', 0) == 0)
                        throw new InvalidOperationException("Could not post a source edit to the verified Workspace editor HWND");
                    WaitUntil(() => HasExactLine(ReadPanelValue(generatedControl), "Template source revision: not captured (saved Automation source)")
                        && ReadPanelValue(generatedControl).Contains(" (unrelated edit)", StringComparison.Ordinal),
                        TimeSpan.FromSeconds(5), "Unapplied Workspace edit did not update the unrelated-editor marker");
                    Ensure(HistoryItems(historyList).Length == expected.GeneratedCount,
                        "Unapplied Workspace editor edit discarded the ordinary Automation history");
                    var preserved = SelectGeneratedHistory(workspace, historyList, generatedControl, 2);
                    AssertGeneratedHistoryDetail(preserved, 2, expected.Generated[1], expected.InputSourceLineIndex,
                        expected.InputText, expected.InputStartMs, expected.InputEndMs,
                        originalEvents[expected.Generated[1].TemplateSourceLine - 9].Text);
                    File.WriteAllText(Path.Combine(caseDir, "history-after-unrelated-edit.txt"), preserved, new UTF8Encoding(false));
                    SaveRuntimeEvidence(workspace, contextControl, generatedControl, caseDir, "history-after-unrelated-edit");
                    var workspaceHandle = new nint(workspace.Current.NativeWindowHandle);
                    if (PostMessageW(workspaceHandle, 0x0010, 0, 0) == 0)
                        throw new InvalidOperationException("Could not close the exact Workspace after the editor-history check");
                    var discard = WaitWindow(host, "Unsaved Lua source", TimeSpan.FromSeconds(8));
                    InvokeExactButton(discard, "Discard");
                    WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(5),
                        "Workspace did not close after discarding the unrelated editor change");
                }
                if (!main.TryGetCurrentPattern(WindowPattern.Pattern, out var closePattern))
                    throw new InvalidOperationException($"{name} main window lacks WindowPattern.Close");
                ((WindowPattern)closePattern).Close();
                if (!host.WaitForExit(TimeSpan.FromSeconds(10))) throw new TimeoutException($"{name} GUI host did not exit normally");
                Ensure(host.ExitCode == 0, $"{name} GUI host exited nonzero");
                File.WriteAllText(Path.Combine(caseDir, "normal-shutdown.json"), JsonSerializer.Serialize(new { HostExitCode = host.ExitCode, Method = "WindowPattern.Close" }));
                results.Add(new StepResult(name, "passed", ""));
                WriteManifest();
            }
            finally
            {
                if (host is not null && !host.HasExited)
                {
                    TryProcessUiEvidence(host, caseDir, "failure-pid-tree");
                    host.Kill(entireProcessTree: true);
                    if (!host.WaitForExit(TimeSpan.FromSeconds(5))) throw new TimeoutException($"{name} GUI host could not be cleaned up");
                }
                host?.Dispose();
            }
        }
        exitStatus = "passed";
        WriteManifest();
        return 0;
    }
    catch (Exception error)
    {
        var failing = steps.FirstOrDefault(step => results.All(result => result.Name != step));
        if (failing is not null) results.Add(new StepResult(failing, "failed", error.Message));
        foreach (var remaining in steps.Where(step => results.All(result => result.Name != step)))
            results.Add(new StepResult(remaining, "not-run", "A preceding case failed"));
        exitStatus = "failed";
        WriteManifest();
        Console.Error.WriteLine(error);
        return 1;
    }

    void WriteManifest() => File.WriteAllText(Path.Combine(artifacts, "manifest.json"), JsonSerializer.Serialize(new
    {
        StartedUtc = startedUtc, FinishedUtc = DateTimeOffset.UtcNow, BudgetSeconds = 660,
        ExeSha256 = hashes["exe"],
        Fixtures = steps.SelectMany(name => new[]
        {
            $"tests/gui-automation/lua-workspace/fixtures/generated-history-{name}.ass",
            $"tests/gui-automation/lua-workspace/fixtures/generated-history-{name}.expected.json"
        }).Append("tests/gui-automation/lua-workspace/fixtures/generated-history-actions.lua"),
        Sha256 = hashes, ExitStatus = exitStatus, Steps = results
    }, new JsonSerializerOptions { WriteIndented = true }));
}

static int RetainedPreviewBytes(string generated)
{
    const string prefix = "Retained preview bytes: ";
    var line = generated.Split('\n').Select(value => value.TrimEnd('\r')).SingleOrDefault(value => value.StartsWith(prefix, StringComparison.Ordinal));
    Ensure(line is not null && line.EndsWith(" / 262144", StringComparison.Ordinal)
        && int.TryParse(line[prefix.Length..^9], NumberStyles.None, CultureInfo.InvariantCulture, out _),
        "Generated history did not report a bounded retained-string byte count");
    return int.Parse(line![prefix.Length..^9], CultureInfo.InvariantCulture);
}

static T ReadExpected<T>(string path) where T : class =>
    JsonSerializer.Deserialize<T>(File.ReadAllText(path), new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower })
    ?? throw new FormatException($"Expected fixture {path} is empty");

static AutomationElement FindGeneratedHistoryList(AutomationElement workspace)
{
    var notebook = FindRuntimeNotebook(workspace);
    var tab = FindRuntimeTab(notebook, "Generated");
    Ensure(tab.TryGetCurrentPattern(SelectionItemPattern.Pattern, out var selected)
        && ((SelectionItemPattern)selected).Current.IsSelected, "Generated tab was not selected before locating history");
    var lists = notebook.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.List))
        .Cast<AutomationElement>().Where(item => item.Current.ProcessId == workspace.Current.ProcessId
            && !item.Current.IsOffscreen && item.Current.ClassName == "ListBox" && item.Current.NativeWindowHandle != 0
            && item.TryGetCurrentPattern(SelectionPattern.Pattern, out _)).ToArray();
    Ensure(lists.Length == 1, $"Expected one visible native Generated ListBox in the runtime notebook, found {lists.Length}");
    return lists[0];
}

static AutomationElement FindWorkspaceSourceEditor(AutomationElement workspace, Process host)
{
    var panes = workspace.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Pane))
        .Cast<AutomationElement>().Where(item => item.Current.ProcessId == host.Id && !item.Current.IsOffscreen
            && item.Current.ClassName == "wxWindow" && item.Current.Name == "stcwindow"
            && item.Current.NativeWindowHandle != 0).ToArray();
    Ensure(panes.Length == 1, $"Expected one visible Workspace source editor, found {panes.Length}");
    GetWindowThreadProcessId(new nint(panes[0].Current.NativeWindowHandle), out var owner);
    Ensure(owner == (uint)host.Id, "Workspace source editor HWND no longer belongs to its host");
    return panes[0];
}

static AutomationElement[] HistoryItems(AutomationElement list) => list.FindAll(TreeScope.Children,
    new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ListItem)).Cast<AutomationElement>().ToArray();

static string SelectGeneratedHistory(AutomationElement workspace, AutomationElement list, AutomationElement output, int index)
{
    SelectTab(workspace, "Generated");
    var items = HistoryItems(list).Where(item => item.Current.Name.StartsWith($"#{index} ", StringComparison.Ordinal)).ToArray();
    Ensure(items.Length == 1, $"Generated history has {items.Length} entries for output #{index}");
    if (!items[0].TryGetCurrentPattern(SelectionItemPattern.Pattern, out var pattern))
        throw new InvalidOperationException($"Generated history #{index} lacks SelectionItemPattern");
    ((SelectionItemPattern)pattern).Select();
    string detail = "";
    WaitUntil(() => HasExactLine(detail = ReadPanelValue(output), $"Output index: {index}"), TimeSpan.FromSeconds(5),
        $"Generated history detail did not select output #{index}");
    return detail;
}

static void AssertGeneratedHistoryDetail(string detail, int index, HistoryExpectedLine wanted, int inputSourceLine,
    string inputText, int inputStartMs, int inputEndMs, string sourceText)
{
    Ensure(HasExactLine(detail, $"Output index: {index}") && HasExactLine(detail, $"Output text: {wanted.Text}")
        && HasExactLine(detail, $"Output style: {wanted.Style}") && HasExactLine(detail, $"Output layer: {wanted.Layer}")
        && HasExactLine(detail, "Output effect: fx") && HasExactLine(detail, $"Output start (ms): {wanted.StartMs}")
        && HasExactLine(detail, $"Output end (ms): {wanted.EndMs}")
        && HasExactLine(detail, $"Input source line index: {inputSourceLine}")
        && HasExactLine(detail, $"Input snapshot index: {inputSourceLine}")
        && HasExactLine(detail, $"Input text: {inputText}")
        && HasExactLine(detail, $"Input start (ms): {inputStartMs}")
        && HasExactLine(detail, $"Input end (ms): {inputEndMs}")
        && HasExactLine(detail, $"Template debug ID: {wanted.TemplateDebugId}")
        && HasExactLine(detail, $"Template primary authored kind: {wanted.TemplateType}")
        && HasExactLine(detail, $"Template authored kinds: {wanted.AuthoredKinds ?? wanted.TemplateType}")
        && HasExactLine(detail, $"Template source line: {wanted.TemplateSourceLine}")
        && HasExactLine(detail, "Template fragment kind: text-template")
        && HasExactLine(detail, $"Template source text: {sourceText}")
        && HasExactLine(detail, $"Template source fragments: {wanted.FragmentKind.Split(", ").Length} total, {wanted.FragmentKind.Split(", ").Length} shown")
        && HasExactLine(detail, $"  line: {wanted.TemplateSourceLine}")
        && wanted.FragmentKind.Split(", ").All(kind => HasExactLine(detail, $"  kind: {kind}"))
        && HasExactLine(detail, $"  text: {sourceText}")
        && HasExactLine(detail, $"Applied scope: {wanted.Scope}")
        && HasExactLine(detail, $"Syllable index: {wanted.SyllableIndex}")
        && HasExactLine(detail, $"Highlight index: {(wanted.HighlightIndex?.ToString() ?? "[not captured]")}")
        && HasExactLine(detail, $"Character index: {(wanted.CharIndex?.ToString() ?? "[not captured]")}"),
        $"Generated output #{index} lacks independent business fields or source provenance");
    Ensure(inputSourceLine != wanted.TemplateSourceLine, $"Output #{index} fixture does not distinguish input and template source positions");
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
    var script = macro == "Apply karaoke template" ? "kara-templater"
        : macro.StartsWith("Generated History ", StringComparison.Ordinal) ? "generated-history-actions"
        : "runtime-actions";
    return $"Feature: automation/lua/{script}/{macro}";
}

static bool HasExactLine(string text, string expected) => text.Split('\n').Any(line => line.TrimEnd('\r') == expected);

static double ContextNumber(string text, string label)
{
    var prefix = label + ": ";
    var line = text.Split('\n').Select(value => value.TrimEnd('\r'))
        .SingleOrDefault(value => value.StartsWith(prefix, StringComparison.Ordinal));
    Ensure(line is not null && double.TryParse(line[prefix.Length..], NumberStyles.Float, CultureInfo.InvariantCulture, out _),
        $"Context field {label} is missing or not numeric");
    return double.Parse(line![prefix.Length..], NumberStyles.Float, CultureInfo.InvariantCulture);
}

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
    var item = OpenAutomationMenuCommand(main, host, command, timeout, menuName);
    Ensure(item.Current.IsEnabled && item.TryGetCurrentPattern(InvokePattern.Pattern, out _),
        $"Menu command '{command}' was exposed but is not callable");
    UiaDriver.Invoke(item);
}

static Task StartMenuInvocation(AutomationElement main, Process host, string command)
{
    var item = OpenAutomationMenuCommand(main, host, command, TimeSpan.FromSeconds(8));
    Ensure(item.Current.IsEnabled && item.TryGetCurrentPattern(InvokePattern.Pattern, out _),
        $"Menu command '{command}' was exposed but is not callable");
    return Task.Run(() => UiaDriver.Invoke(item));
}

static void InvokeExactButton(AutomationElement window, string name)
{
    var button = window.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button))
        .Cast<AutomationElement>().FirstOrDefault(item => item.Current.Name == name && item.Current.IsEnabled && !item.Current.IsOffscreen
            && item.TryGetCurrentPattern(InvokePattern.Pattern, out _))
        ?? throw new InvalidOperationException($"Expected dialog has no visible enabled exact '{name}' button");
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
    var task = StartMenuInvocation(main, host, macro);
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
    var task = StartMenuInvocation(main, host, macro);
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
sealed record HistoryBranchesExpected(int GeneratedCount, int InputSourceLineIndex, string InputText,
    int InputStartMs, int InputEndMs, List<HistoryExpectedLine> Generated);
sealed record HistoryExpectedLine(string Text, int Layer, int StartMs, int EndMs, string Style,
    int TemplateSourceLine, int TemplateDebugId, string TemplateType, string FragmentKind,
    string Scope, int SyllableIndex, int? HighlightIndex = null, int? CharIndex = null, string? AuthoredKinds = null);
sealed record HistoryCapacityExpected(int GeneratedCount, int RetainedCount, int OmittedCount,
    int FirstRetainedIndex, int TemplateSourceLine, int InputSourceLineIndex, string InputText,
    int InputStartMs, int InputEndMs, string TextPrefix, int Layer, string Style, int StartMs, int EndMs);
sealed record HistoryMutatedInputExpected(int GeneratedCount, int InputSourceLineIndex, string InputText,
    int InputStartMs, int InputEndMs, int TemplateSourceLine, int TemplateDebugId, string TemplateSourceText,
    string MutatedLiveInputText, int MutatedLiveInputStartMs, int MutatedLiveInputEndMs,
    string OutputTextPrefix, int OutputLayer, string OutputStyle, int OutputStartMs, int OutputEndMs);
sealed record HistoryCancelExpected(int OriginalEventCount, int MinimumProvisionalGenerated,
    int MaximumProvisionalGenerated, int InputSourceLineIndex, int TemplateSourceLine, string TextPrefix);
sealed record HostIdentity(int ProcessId, string StartTimeUtc);
sealed record TemplateFailureCase(string Name, string ArtifactName, string ExpectedKind, int SourceLine, string SourceText, string? RuntimeMarker);
