#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:property InvariantGlobalization=false
#:project ../driver/Aegisub.GuiAutomation.Driver.csproj

using System.Collections.Specialized;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Windows.Automation;
using System.Windows.Forms;
using Aegisub.GuiAutomation.Driver;

const string workerFlag = "--debug-worker";
try
{
    return args.Contains(workerFlag) ? Run(args.Where(arg => arg != workerFlag).ToArray()) : Supervise(args);
}
catch (Exception error)
{
    Console.Error.WriteLine($"debug-uia.error={error.GetType().Name}:{error.Message}");
    return 1;
}

static int Supervise(string[] args)
{
    var timer = Stopwatch.StartNew();
    var artifactIndex = Array.IndexOf(args, "--artifacts");
    var exeIndex = Array.IndexOf(args, "--exe");
    var scenarioIndex = Array.IndexOf(args, "--scenario");
    var scenario = scenarioIndex >= 0 && scenarioIndex + 1 < args.Length ? args[scenarioIndex + 1] : "basic";
    var budgetSeconds = scenario == "jit" ? 360 : scenario == "preparation" ? 300 : 240;
    if (artifactIndex < 0 || artifactIndex + 1 >= args.Length || exeIndex < 0 || exeIndex + 1 >= args.Length)
        throw new ArgumentException("--exe and --artifacts are required");
    var artifacts = Path.GetFullPath(args[artifactIndex + 1]);
    var expectedExe = Path.GetFullPath(args[exeIndex + 1]);
    if (Directory.Exists(artifacts) || File.Exists(artifacts))
        throw new InvalidOperationException("Use a fresh debug UIA artifact directory; existing evidence is never overwritten");
    Directory.CreateDirectory(artifacts);
    var originalSequence = Native.ClipboardSequence();
    var originalClipboard = ClipboardSafety.OnSta(ClipboardSafety.Capture);
    Ensure(Native.ClipboardSequence() == originalSequence, "Clipboard changed during its materialized snapshot");
    var executable = Environment.ProcessPath ?? throw new InvalidOperationException("Driver process path is unavailable");
    var start = new ProcessStartInfo(executable) { UseShellExecute = false };
    if (Path.GetFileNameWithoutExtension(executable).Equals("dotnet", StringComparison.OrdinalIgnoreCase))
        start.ArgumentList.Add(Assembly.GetEntryAssembly()?.Location ?? throw new InvalidOperationException("Executed assembly is unavailable"));
    start.ArgumentList.Add(workerFlag);
    start.Environment["AEGISUB_DEBUG_CLIPBOARD_SEQUENCE"] = originalSequence.ToString(System.Globalization.CultureInfo.InvariantCulture);
    foreach (var argument in args) start.ArgumentList.Add(argument);
    using var worker = Process.Start(start) ?? throw new InvalidOperationException("Could not start debug UIA worker");
    var timedOut = false;
    var workerBudget = TimeSpan.FromSeconds(budgetSeconds - 10) - timer.Elapsed;
    if (workerBudget <= TimeSpan.Zero || !worker.WaitForExit(workerBudget))
    {
        timedOut = true;
        worker.Kill(entireProcessTree: true);
        if (!worker.WaitForExit(TimeSpan.FromSeconds(5))) throw new TimeoutException("Debug UIA worker did not stop after timeout");
    }
    var hostLeftRunning = false;
    var readyPath = Path.Combine(artifacts, "ready.json");
    var identityPath = Path.Combine(artifacts, "host-identity.json");
    if (File.Exists(readyPath))
    {
        var hostId = JsonDocument.Parse(File.ReadAllText(readyPath)).RootElement.GetProperty("process_id").GetInt32();
        var identity = File.Exists(identityPath) ? JsonSerializer.Deserialize<HostIdentity>(File.ReadAllText(identityPath)) : null;
        var verifiedIdentity = identity ?? throw new InvalidOperationException("GUI host PID has no matching worker identity");
        Ensure(verifiedIdentity.ProcessId == hostId, "GUI host PID has no matching worker identity");
        try
        {
            using var host = Process.GetProcessById(hostId);
            if (!host.HasExited)
            {
                hostLeftRunning = true;
                Ensure(host.StartTime.ToUniversalTime().ToString("O") == verifiedIdentity.StartTimeUtc
                    && Path.GetFullPath(host.MainModule?.FileName ?? "").Equals(expectedExe, StringComparison.OrdinalIgnoreCase),
                    "Ready PID no longer belongs to this GUI host; unrelated process was preserved");
                host.Kill(entireProcessTree: true);
                if (!host.WaitForExit(TimeSpan.FromSeconds(5))) throw new TimeoutException("GUI host did not stop before clipboard cleanup");
            }
        }
        catch (ArgumentException) { }
    }
    var receipt = ClipboardReceipt.Read(Path.Combine(artifacts, "clipboard-receipt.json"));
    var finalSequence = Native.ClipboardSequence();
    string clipboardStatus;
    if (receipt is not null && finalSequence == receipt.Sequence)
    {
        ClipboardSafety.Restore(originalClipboard, receipt.Sequence);
        clipboardStatus = "restored";
    }
    else if (receipt is null && finalSequence == originalSequence)
        clipboardStatus = "unchanged";
    else
        clipboardStatus = "external-or-unreceipted-change-preserved";
    File.WriteAllText(Path.Combine(artifacts, "supervisor.json"), JsonSerializer.Serialize(new
    {
        BudgetSeconds = budgetSeconds, TimedOut = timedOut, HostLeftRunning = hostLeftRunning, WorkerExitCode = worker.ExitCode,
        ClipboardStatus = clipboardStatus, InitialSequence = originalSequence, FinalSequence = finalSequence
    }, new JsonSerializerOptions { WriteIndented = true }));
    Ensure(clipboardStatus != "external-or-unreceipted-change-preserved", "Clipboard changed outside the verified test transaction; external data was preserved");
    Ensure(!timedOut, $"Lua Workspace debug GUI E2E exceeded its {budgetSeconds}-second total limit");
    return hostLeftRunning ? 1 : worker.ExitCode;
}

static int Run(string[] args)
{
    string? exe = null;
    string? artifacts = null;
    var scenario = "basic";
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
    MenuObservation.Initialize(artifacts);
    Ensure(scenario is "basic" or "launch" or "controls" or "files" or "dap" or "jit" or "preparation" or "language" or "language-unavailable"
        or "entry-late" or "entry-zh" or "entry-once" or "entry-missing" or "entry-ambiguous", "Unknown debug scenario");
    var entryScenario = scenario.StartsWith("entry-", StringComparison.Ordinal);
    var fixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "debug.ass");
    var actions = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "debug-actions.lua");
    var expectedFile = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "debug.expected.json");
    var stepsFile = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "debug-steps.lua");
    var loopFile = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "debug-loop.lua");
    var infiniteLoopFile = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "debug-infinite-loop.lua");
    var entryFixture = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "workspace-entry.ass");
    var entryActions = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "workspace-entry-actions.lua");
    var entryDuplicate = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "workspace-entry-duplicate.lua");
    var preparationFixtures = new[] { "preparation-top-level-loop.lua", "preparation-include-loop.lua",
        "preparation-include-loop-body.lua", "preparation-validation-loop.lua", "preparation-include-failure.lua",
        "preparation-validation-failure.lua", "preparation-validation-error.lua", "preparation-sethook-loop.lua",
        "preparation-gc-deferred.lua", "preparation-choice.lua" };
    var templater = Path.Combine("automation", "autoload", "kara-templater.lua");
    var includeRoot = Path.Combine("automation", "include");
    var driver = Path.Combine("tests", "gui-automation", "lua-workspace", "debug-uia.cs");
    var hashes = new Dictionary<string, string> { ["exe"] = Hash(exe), ["fixture"] = Hash(fixture), ["actions"] = Hash(actions),
        ["expected"] = Hash(expectedFile), ["steps"] = Hash(stepsFile), ["loop"] = Hash(loopFile),
        ["infinite_loop"] = Hash(infiniteLoopFile),
        ["entry_fixture"] = Hash(entryFixture), ["entry_actions"] = Hash(entryActions), ["entry_duplicate"] = Hash(entryDuplicate),
        ["templater"] = Hash(templater), ["driver_source"] = Hash(driver),
        ["clipboard_sta_source"] = Hash(Path.Combine("tests", "gui-automation", "driver", "ClipboardSta.cs")),
        ["executed_driver_library"] = Hash(typeof(ClipboardSta).Assembly.Location),
        ["executed_driver"] = Hash(Assembly.GetEntryAssembly()?.Location ?? throw new InvalidOperationException("Executed assembly unavailable")) };
    var includeFiles = Directory.GetFiles(includeRoot, "*", SearchOption.AllDirectories);
    foreach (var file in includeFiles)
        hashes[Path.Combine(includeRoot, Path.GetRelativePath(includeRoot, file)).Replace('\\', '/')] = Hash(file);
    var expected = JsonSerializer.Deserialize<ExpectedOutput>(File.ReadAllText(expectedFile), new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower })
        ?? throw new FormatException("Debug expected JSON is empty");
    Ensure(expected.Generated is not null && expected.GeneratedCount == expected.Generated.Count && expected.GeneratedCount > 0,
        "Debug expected generated list/count is inconsistent");
    var input = Path.Combine(artifacts, "input.ass");
    File.Copy(entryScenario ? entryFixture : fixture, input);
    File.Copy(actions, Path.Combine(artifacts, "debug-actions.lua"));
    File.Copy(stepsFile, Path.Combine(artifacts, "debug-steps.lua"));
    File.Copy(loopFile, Path.Combine(artifacts, "debug-loop.lua"));
    File.Copy(infiniteLoopFile, Path.Combine(artifacts, "debug-infinite-loop.lua"));
    File.Copy(entryActions, Path.Combine(artifacts, "workspace-entry-actions.lua"));
    File.Copy(entryDuplicate, Path.Combine(artifacts, "workspace-entry-duplicate.lua"));
    File.Copy(templater, Path.Combine(artifacts, "kara-templater.lua"));
    foreach (var file in includeFiles)
    {
        var destination = Path.Combine(artifacts, "include", Path.GetRelativePath(includeRoot, file));
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        File.Copy(file, destination);
    }
    if (entryScenario) PrepareEntryInput(input, scenario);
    var baseline = EventLines(File.ReadAllText(input));
    Ensure(entryScenario
        ? baseline.Count == (scenario == "entry-once" ? 53 : 54) && baseline[0].Effect == "code once"
        : baseline.Count == 7 && baseline[0].Text.Contains("OLD:", StringComparison.Ordinal),
        "Scenario ASS fixture baseline has changed");
    Directory.CreateDirectory(Path.Combine(artifacts, "profile"));
    if (scenario == "files") PrepareFiles();
    if (scenario == "jit") PrepareJit();
    if (scenario == "preparation") PreparePreparation();
    if (scenario == "entry-zh")
    {
        PrepareChineseLocale(exe, artifacts, hashes);
        var user = Path.Combine(artifacts, "profile", "user");
        Directory.CreateDirectory(user);
        File.WriteAllText(Path.Combine(user, "config.json"), JsonSerializer.Serialize(new
        {
            App = new { Language = "zh_CN" }
        }));
    }
    var languageScenario = scenario is "language" or "language-unavailable";
    var runtimeExe = Path.Combine(Path.GetDirectoryName(exe)!, "runtimes", "LuaLS", "bin", "lua-language-server.exe");
    if (languageScenario)
    {
        Ensure(File.Exists(runtimeExe), "The complete real LuaLS release is required even for unavailable-path isolation");
        hashes["luals_exe"] = Hash(runtimeExe);
        if (scenario == "language-unavailable")
        {
            var user = Path.Combine(artifacts, "profile", "user");
            Directory.CreateDirectory(user);
            File.WriteAllText(Path.Combine(user, "config.json"), JsonSerializer.Serialize(new
            {
                Automation = new Dictionary<string, object> { ["Lua Workspace"] = new Dictionary<string, object>
                {
                    ["Enable LuaLS"] = true, ["LuaLS Directory"] = "?user/missing-luals"
                } }
            }));
        }
    }
    var dapToken = "lua-workspace-e2e";
    var dapPort = scenario == "dap" ? PrepareDap() : 0;
    ClipboardReceipt.Initialize(Path.Combine(artifacts, "clipboard-receipt.json"), uint.Parse(
        Environment.GetEnvironmentVariable("AEGISUB_DEBUG_CLIPBOARD_SEQUENCE") ?? throw new InvalidOperationException("Clipboard baseline is missing"),
        System.Globalization.CultureInfo.InvariantCulture));
    var startedUtc = DateTimeOffset.UtcNow;
    DateTimeOffset? finishedUtc = null;
    var exitStatus = "running";
    var steps = scenario == "controls"
        ? new[] { "host-ready", "controls-open-steps", "step-in-out-over", "step-save-undo", "loop-seed", "loop-pause-stop",
            "loop-live-rollback", "loop-undo-depth", "close-stop", "close-stop-rollback", "close-detach-stop",
            "close-detach-stop-rollback", "close-detach-complete", "close-detach-save-undo", "normal-shutdown" }
        : scenario == "launch" ? LaunchSteps()
        : scenario == "files" ? FileSteps()
        : scenario == "dap" ? DapSteps()
        : scenario == "jit" ? JitSteps()
        : scenario == "preparation" ? PreparationSteps()
        : entryScenario ? EntrySteps()
        : new[] { "host-ready", "open-workspace", "edit-unapplied", "breakpoint-debug-pause", "stale-source", "stop-rollback",
            "stop-live-rollback", "run-generated", "undo-restores", "debug-detach", "basic-normal-shutdown" };
    if (languageScenario)
        steps = steps.Take(2).Concat(new[] { "language-initial-state" }).Concat(steps.Skip(2)).ToArray();
    if (scenario == "language")
    {
        var index = Array.IndexOf(steps, "stop-rollback");
        steps = steps.Take(index).Concat(new[] { "language-current-paused-revision", "language-process-exit" }).Concat(steps.Skip(index)).ToArray();
    }
    var results = new List<StepResult>();
    Process? host = null;
    AutomationElement? main = null;
    AutomationElement? workspace = null;
    try
    {
        var start = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe)! };
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", Path.Combine(artifacts, "profile"), "--artifacts", artifacts, "--open", input })
            start.ArgumentList.Add(argument);
        host = Process.Start(start) ?? throw new InvalidOperationException("Could not start GUI host");
        File.WriteAllText(Path.Combine(artifacts, "host-identity.json"), JsonSerializer.Serialize(new HostIdentity(host.Id, host.StartTime.ToUniversalTime().ToString("O"))));
        Step("host-ready", () =>
        {
            var ready = AutomationProtocol.WaitForReadyArtifact(Path.Combine(artifacts, "ready.json"), host, TimeSpan.FromSeconds(15));
            Ensure(ready.ProcessId == host.Id, "ready.json belongs to another host");
            _ = UiaDriver.WaitForMainWindow(host, TimeSpan.FromSeconds(15));
        });
        main = AutomationElement.FromHandle(host.MainWindowHandle);
        SaveEvidence(main, artifacts, "main-before-open");
        if (scenario is not ("basic" or "language" or "language-unavailable"))
        {
            if (scenario == "launch") RunLaunch();
            else if (scenario == "controls") RunControls();
            else if (scenario == "files") RunFiles();
            else if (scenario == "dap") RunDap();
            else if (entryScenario) RunEntry();
            else if (scenario == "jit") RunJit();
            else RunPreparation();
            foreach (var step in steps.Where(name => results.All(result => result.Name != name)))
                results.Add(new StepResult(step, "not-run", "Scenario step was not executed"));
            finishedUtc = DateTimeOffset.UtcNow;
            exitStatus = results.Count == steps.Length && results.All(result => result.Status == "passed") ? "passed" : "incomplete";
            WriteManifest();
            return exitStatus == "passed" ? 0 : 1;
        }
        AutomationElement? editor = null;
        Step("open-workspace", () =>
        {
            InvokeMenu(main, host, "Workspace Debug Select First Code", TimeSpan.FromSeconds(8));
            InvokeMenu(main, host, "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(8));
            workspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(8));
            SetPauseOnEntry(workspace, true);
            SaveEvidence(workspace, artifacts, "workspace-opened");
            editor = FindStyledText(workspace, insideNotebook: false);
            Ensure(editor is not null, "Workspace source editor has no unique UIA STC descendant");
        });
        if (languageScenario) Step("language-initial-state", () =>
        {
            SelectTab(workspace!, "Language");
            var expectedState = scenario == "language" ? ": ready" : "LuaLS unavailable:";
            WaitUntil(() => ReadLanguageStatus(workspace!, host).Contains(expectedState, StringComparison.Ordinal),
                TimeSpan.FromSeconds(15), "LuaLS did not report the expected initial state");
            File.WriteAllText(Path.Combine(artifacts, "language-initial-status.txt"), ReadLanguageStatus(workspace!, host));
            SaveEvidence(workspace!, artifacts, "language-initial");
        });
        const string edited = "decorate = function(value)\n  return \"DBG:\" .. string.upper(value)\nend";
        File.WriteAllText(Path.Combine(artifacts, "edited.lua"), edited, new UTF8Encoding(false));
        Step("edit-unapplied", () =>
        {
            Native.ReplaceWithClipboard(editor!, host, edited);
            Ensure(Normalize(CopySource(workspace!, host)) == edited, "Workspace editor did not retain exact multiline un-applied source");
            Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), "Un-applied source changed physical ASS events");
            SaveEvidence(workspace!, artifacts, "edited-unapplied");
        });
        Step("breakpoint-debug-pause", () =>
        {
            Native.Focus(editor!, host);
            Native.SendCtrlHome(editor!, host);
            Native.SendKey(editor!, host, 0x28);
            InvokeButton(workspace!, "Toggle Breakpoint");
            var debugTask = Task.Run(() => InvokeButton(workspace!, "Debug"));
            PauseStatus? entry = null;
            WaitUntil(() => (entry = ParsePauseStatus(ReadRunStatus(workspace!)))?.Reason == "entry", TimeSpan.FromSeconds(15),
                "Debug did not report its initial entry pause");
            File.WriteAllText(Path.Combine(artifacts, "debug-entry-status.txt"), ReadRunStatus(workspace!));
            SaveEvidence(workspace!, artifacts, "debug-entry-paused");
            AssertLivePauseControls(main, workspace!);
            InvokeButton(workspace!, "Continue");
            SelectTab(workspace!, "Stack and Variables");
            PauseStatus? breakpoint = null;
            WaitUntil(() => (breakpoint = ParsePauseStatus(ReadRunStatus(workspace!))) is { Reason: "breakpoint" } candidate
                && candidate.Sequence > entry!.Sequence && StackHasSourceLine(workspace!, "Code line ID ", 2),
                TimeSpan.FromSeconds(20), "Continue did not stop at a new code-source line 2 breakpoint");
            SaveEvidence(workspace!, artifacts, "debug-breakpoint-paused");
            File.WriteAllText(Path.Combine(artifacts, "debug-paused-status.txt"), ReadRunStatus(workspace!));
            AssertLivePauseControls(main, workspace!);
            DebugInvocation.Set(debugTask);
        });
        Step("stale-source", () =>
        {
            SelectTab(workspace!, "Execution Source");
            AutomationElement? execution = null;
            WaitUntil(() => (execution = FindStyledText(workspace!, insideNotebook: true)) is not null,
                TimeSpan.FromSeconds(3), "Paused execution source has no unique readonly STC descendant");
            var captured = Native.CopyStyledText(execution!, host);
            File.WriteAllText(Path.Combine(artifacts, "paused-source.lua"), captured, new UTF8Encoding(false));
            Ensure(Normalize(captured) == edited, "Paused readonly source did not match the immutable invocation revision");
            Native.ReplaceWithClipboard(editor!, host, edited + (scenario == "language"
                ? "\nlocal language_probe = missing_paused_revision" : "\n-- next revision only"));
            var after = Native.CopyStyledText(execution!, host);
            Ensure(Normalize(after) == edited, "Paused readonly source changed with a newer editor buffer");
            Ensure(HasWorkspaceStaticText(workspace!, text => text.Contains("[stale editor revision]", StringComparison.Ordinal)),
                "Execution identity did not mark the captured revision stale after editing");
            File.WriteAllText(Path.Combine(artifacts, "stale-editor.lua"), CopySource(workspace!, host), new UTF8Encoding(false));
            SaveEvidence(workspace!, artifacts, "stale-paused-source");
        });
        if (scenario == "language")
        {
            Step("language-current-paused-revision", () =>
            {
                var pause = ParsePauseStatus(ReadRunStatus(workspace!));
                Ensure(pause is { Reason: "breakpoint" }, "Language refresh must run while the original breakpoint stays paused");
                SelectTab(workspace!, "Language");
                WaitUntil(() => ReadLanguageDiagnostics(workspace!, host).Contains("missing_paused_revision", StringComparison.Ordinal),
                    TimeSpan.FromSeconds(10), "Language diagnostics did not analyze the new editable revision during pause");
                File.WriteAllText(Path.Combine(artifacts, "paused-current-diagnostics.txt"), ReadLanguageDiagnostics(workspace!, host));
                SaveEvidence(workspace!, artifacts, "language-paused-current");
                SelectTab(workspace!, "Execution Source");
                var execution = FindStyledText(workspace!, insideNotebook: true)!;
                Ensure(Normalize(Native.CopyStyledText(execution, host)) == edited,
                    "Language refresh changed the captured execution source");
                Ensure(ParsePauseStatus(ReadRunStatus(workspace!)) == pause, "Language refresh advanced the suspended invocation");
                File.WriteAllText(Path.Combine(artifacts, "language-paused-execution.lua"), Native.CopyStyledText(execution, host));
            });
            Step("language-process-exit", () =>
            {
                LuaLanguageChild.TerminateVerified(host, runtimeExe, Path.Combine(artifacts, "language-terminated-child.json"));
                SelectTab(workspace!, "Language");
                WaitUntil(() => ReadLanguageStatus(workspace!, host).Contains("LuaLS unavailable:", StringComparison.Ordinal),
                    TimeSpan.FromSeconds(8), "The terminated real LuaLS was not reported unavailable");
                Ensure(ReadLanguageDiagnostics(workspace!, host).Contains("No version-confirmed diagnostics", StringComparison.Ordinal),
                    "Failed language service retained stale diagnostic text");
                Ensure(Normalize(CopySource(workspace!, host)) == edited + "\nlocal language_probe = missing_paused_revision",
                    "LuaLS termination changed the source buffer");
                File.WriteAllText(Path.Combine(artifacts, "language-exit-status.txt"), ReadLanguageStatus(workspace!, host));
                SaveEvidence(workspace!, artifacts, "language-exit-paused");
            });
        }
        Step("stop-rollback", () =>
        {
            InvokeButton(workspace!, "Stop");
            Ensure(DebugInvocation.Wait(TimeSpan.FromSeconds(20)), "Stopped debug command did not return");
            WaitUntil(() => ReadRunStatus(workspace!).Contains("cancelled", StringComparison.OrdinalIgnoreCase), TimeSpan.FromSeconds(8),
                "Stop did not reach a cancelled terminal state");
            Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), "Stopped debug invocation modified the physical ASS baseline");
            Native.Undo(editor!, host);
            Ensure(Normalize(CopySource(workspace!, host)) == edited, "One Workspace-local Undo did not restore the invocation source after stale edit");
            if (languageScenario)
            {
                SelectTab(workspace!, "Language");
                WaitUntil(() => ReadLanguageStatus(workspace!, host).Contains("LuaLS unavailable:", StringComparison.Ordinal),
                    TimeSpan.FromSeconds(5), "Undo silently restarted or replaced the unavailable language server");
                File.WriteAllText(Path.Combine(artifacts, "language-after-undo-status.txt"), ReadLanguageStatus(workspace!, host));
            }
            SaveEvidence(workspace!, artifacts, "debug-stopped");
        });
        Step("stop-live-rollback", () =>
        {
            WaitUntil(() => main.Current.IsEnabled, TimeSpan.FromSeconds(5), "Stop did not release the main window");
            var before = File.ReadAllBytes(input);
            var task = Task.Run(() => InvokeMenu(main, host, "Workspace Debug Verify Original", TimeSpan.FromSeconds(8)));
            var dialog = WaitWindowContainingText(host, "Debug original live baseline verified", TimeSpan.FromSeconds(8));
            SaveEvidence(dialog, artifacts, "basic-live-rollback");
            InvokeButton(dialog, "OK");
            Ensure(task.Wait(TimeSpan.FromSeconds(10)), "Basic live rollback verifier did not return");
            WaitUntil(() => main.Current.IsEnabled && FindProgressPane(host, "Workspace Debug Verify Original") is null,
                TimeSpan.FromSeconds(8), "Basic live rollback verifier did not release its execution UI");
            Ensure(File.ReadAllBytes(input).SequenceEqual(before) && EventLines(File.ReadAllText(input)).SequenceEqual(baseline),
                "Basic live rollback verifier changed or disagreed with the physical baseline");
        });
        Step("run-generated", () =>
        {
            var task = Task.Run(() => InvokeButton(workspace!, "Run"));
            WaitForInvocation(workspace!, host, task, "completed", artifacts, "run-generated");
            Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), "Run wrote the physical ASS before explicit main Save");
            var save = UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles");
            WaitUntil(() => (save = UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles")) is not null,
                TimeSpan.FromSeconds(8), "Main Save did not become available after Workspace Run");
            UiaDriver.Invoke(save!);
            WaitUntil(() => EventLines(File.ReadAllText(input)).Count == baseline.Count + expected.GeneratedCount,
                TimeSpan.FromSeconds(8), "Saved ASS did not contain independent generated count");
            var saved = File.ReadAllText(input);
            File.WriteAllText(Path.Combine(artifacts, "run-output.ass"), saved);
            AssertGenerated(saved, baseline, expected);
            SaveEvidence(workspace!, artifacts, "run-completed");
        });
        Step("undo-restores", () =>
        {
            InvokeMenu(main, host, "Undo", TimeSpan.FromSeconds(8), "Edit");
            AutomationElement? save = null;
            WaitUntil(() => (save = UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles")) is not null,
                TimeSpan.FromSeconds(8), "Main Save did not become available after one Undo");
            UiaDriver.Invoke(save!);
            WaitUntil(() => EventLines(File.ReadAllText(input)).SequenceEqual(baseline), TimeSpan.FromSeconds(8),
                "One main Undo did not restore the exact seven-event baseline");
            File.Copy(input, Path.Combine(artifacts, "undone.ass"));
        });
        Step("debug-detach", () =>
        {
            var task = Task.Run(() => InvokeButton(workspace!, "Debug"));
            PauseStatus? entry = null;
            WaitUntil(() => (entry = ParsePauseStatus(ReadRunStatus(workspace!))) is { Reason: "entry", Sequence: 1 },
                TimeSpan.FromSeconds(15), "Second Debug did not reach its new entry #1 pause");
            AssertLivePauseControls(main, workspace!);
            SaveEvidence(workspace!, artifacts, "detach-paused");
            InvokeButton(workspace!, "Detach");
            WaitForInvocation(workspace!, host, task, "completed", artifacts, "detach-completed");
            AutomationElement? save = null;
            WaitUntil(() => (save = UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles")) is not null,
                TimeSpan.FromSeconds(8), "Main Save did not become available after detached invocation committed");
            UiaDriver.Invoke(save!);
            WaitUntil(() => EventLines(File.ReadAllText(input)).Count == baseline.Count + expected.GeneratedCount,
                TimeSpan.FromSeconds(8), "Detached invocation did not persist expected generated count");
            var saved = File.ReadAllText(input);
            File.WriteAllText(Path.Combine(artifacts, "detach-output.ass"), saved);
            AssertGenerated(saved, baseline, expected);
        });
        Step("basic-normal-shutdown", () =>
        {
            Native.PostClose(workspace!, host);
            var dialog = WaitWindow(host, "Unsaved Lua source", TimeSpan.FromSeconds(5));
            SaveEvidence(dialog, artifacts, "basic-discard-unsaved-source");
            InvokeButton(dialog, "Discard");
            WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(5), "Basic Workspace did not close after Discard");
            AssertGenerated(File.ReadAllText(input), baseline, expected);
            Native.StabilizeOwnedClipboard(host.Id);
            if (!main.TryGetCurrentPattern(WindowPattern.Pattern, out var pattern))
                throw new InvalidOperationException("Basic main window lacks WindowPattern.Close");
            ((WindowPattern)pattern).Close();
            Ensure(host.WaitForExit(TimeSpan.FromSeconds(10)) && host.ExitCode == 0, "Basic host did not exit normally with code zero");
            File.WriteAllText(Path.Combine(artifacts, "basic-normal-shutdown.json"), JsonSerializer.Serialize(new
            {
                HostExitCode = host.ExitCode, Method = "WindowPattern.Close"
            }));
        });
        foreach (var step in steps.Where(name => results.All(result => result.Name != name)))
            results.Add(new StepResult(step, "not-run", "Scenario step was not executed"));
        finishedUtc = DateTimeOffset.UtcNow;
        exitStatus = results.Count == steps.Length && results.All(result => result.Status == "passed") ? "passed" : "incomplete";
        WriteManifest();
        return exitStatus == "passed" ? 0 : 1;
    }
    catch (Exception error)
    {
        if (host is not null && !host.HasExited) TryProcessEvidence(host, artifacts, "failure");
        var failing = steps.FirstOrDefault(name => results.All(result => result.Name != name));
        if (failing is not null) results.Add(new StepResult(failing, "failed", error.Message));
        foreach (var step in steps.Where(name => results.All(result => result.Name != name)))
            results.Add(new StepResult(step, "not-run", "A preceding step failed"));
        finishedUtc = DateTimeOffset.UtcNow;
        exitStatus = "failed";
        WriteManifest();
        Console.Error.WriteLine(error);
        return 1;
    }
    finally
    {
        if (workspace is not null) TryEvidence(workspace, artifacts, "final-workspace");
        if (host is not null && !host.HasExited)
        {
            Native.StabilizeOwnedClipboard(host.Id);
            host.Kill(entireProcessTree: true);
            if (!host.WaitForExit(TimeSpan.FromSeconds(5))) throw new TimeoutException("GUI host did not stop before clipboard cleanup");
        }
        host?.Dispose();
    }

    string[] LaunchSteps() => new[] { "host-ready", "launch-open-code", "launch-default-breakpoint", "launch-last-pause",
        "launch-no-breakpoint-completes", "launch-entry-opt-in", "launch-normal-shutdown" };

    void RunLaunch()
    {
        var gui = main!;
        var process = host!;
        AutomationElement? sourceEditor = null;
        Task? breakpointInvocation = null;
        const string source = "local prefix = \"DBG:\"\ndecorate = function(value)\n  return prefix .. string.upper(value)\nend";

        Step("launch-open-code", () =>
        {
            InvokeMenu(gui, process, "Workspace Debug Select First Code", TimeSpan.FromSeconds(8));
            InvokeMenu(gui, process, "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(8));
            workspace = WaitWindow(process, "Lua Workspace", TimeSpan.FromSeconds(8));
            sourceEditor = FindStyledText(workspace, insideNotebook: false)
                ?? throw new InvalidOperationException("Launch scenario has no unique source editor");
            Ensure(ReadPauseOnEntry(workspace) == ToggleState.Off, "Pause on entry is not disabled by default");
            Native.ReplaceWithClipboard(sourceEditor, process, source);
            Ensure(Normalize(CopySource(workspace, process)) == source, "Launch source edit was not retained exactly");
            Native.Focus(sourceEditor, process);
            Native.SendCtrlHome(sourceEditor, process);
            Native.SendKey(sourceEditor, process, 0x28);
            Native.SendKey(sourceEditor, process, 0x28);
            Native.ClickBreakpointMargin(sourceEditor, process, 3);
            SaveEvidence(workspace, artifacts, "launch-default-ready");
        });
        Step("launch-default-breakpoint", () =>
        {
            breakpointInvocation = Task.Run(() => InvokeButton(workspace!, "Debug"));
            WaitUntil(() => ParsePauseStatus(ReadRunStatus(workspace!)) is { Reason: "breakpoint", Sequence: 1, Line: 3 },
                TimeSpan.FromSeconds(20), "Default Debug did not run directly to code-source breakpoint #1");
            AssertLivePauseControls(gui, workspace!);
            Ensure(ReadPauseOnEntry(workspace!) == ToggleState.Off, "Default Debug changed Pause on entry");
            Ensure(IsTabSelected(workspace!, "Stack and Variables"),
                "Matching editable source did not keep the current-pause navigation on Stack and Variables");
            Ensure(StackHasSourceLine(workspace!, "Code line ID ", 3),
                "Default breakpoint pause did not expose its captured code-source stack frame");
            WaitUntil(() => ReadDebugLocation(workspace!).StartsWith("Paused at ", StringComparison.Ordinal)
                    && ReadDebugLocation(workspace!).Contains("[editable source]", StringComparison.Ordinal),
                TimeSpan.FromSeconds(5), "Current debug location did not identify the mapped live editable source");
            AssertVariableTreePause(workspace!, process);
            File.WriteAllText(Path.Combine(artifacts, "launch-breakpoint-status.txt"), ReadRunStatus(workspace!));
            File.WriteAllText(Path.Combine(artifacts, "launch-paused-location.txt"), ReadDebugLocation(workspace!));
            File.WriteAllText(Path.Combine(artifacts, "launch-paused-variable-details.txt"), ReadVariableDetails(workspace!, process));
            SaveEvidence(workspace!, artifacts, "launch-breakpoint-direct");
            InvokeButton(workspace!, "Clear Breakpoints");
            Ensure(ParsePauseStatus(ReadRunStatus(workspace!)) is { Reason: "breakpoint", Sequence: 1, Line: 3 },
                "Clearing breakpoints changed the current pause identity");
            SaveEvidence(workspace!, artifacts, "launch-current-marker-with-breakpoint-cleared");
        });
        Step("launch-last-pause", () =>
        {
            InvokeButton(workspace!, "Continue");
            WaitUntil(() => ParsePauseStatus(ReadRunStatus(workspace!)) is { Reason: "breakpoint", Sequence: 2, Line: 3 },
                TimeSpan.FromSeconds(15), "Continue did not reach the next captured line-3 breakpoint");
            AssertLivePauseControls(gui, workspace!);
            WaitUntil(() => ReadVariableDetails(workspace!, process).StartsWith("Paused value", StringComparison.Ordinal)
                    && ReadVariableDetails(workspace!, process).Contains("\"beta\"", StringComparison.Ordinal),
                TimeSpan.FromSeconds(5), "Second breakpoint did not refresh the selected local to beta");
            SaveEvidence(workspace!, artifacts, "launch-second-breakpoint");
            InvokeButton(workspace!, "Detach");
            WaitUntil(() => ReadDebugLocation(workspace!).StartsWith("Last pause [last pause, not current]", StringComparison.Ordinal),
                TimeSpan.FromSeconds(5), "Resumed debug location did not mark the retained snapshot as non-current");
            WaitUntil(() => ReadVariableDetails(workspace!, process).StartsWith("Last pause [not live]", StringComparison.Ordinal),
                TimeSpan.FromSeconds(5), "Resumed variable details did not mark the retained value as not live");
            File.WriteAllText(Path.Combine(artifacts, "launch-last-pause-location.txt"), ReadDebugLocation(workspace!));
            File.WriteAllText(Path.Combine(artifacts, "launch-last-pause-variable-details.txt"), ReadVariableDetails(workspace!, process));
            SaveEvidence(workspace!, artifacts, "launch-last-pause");
            WaitForInvocation(workspace!, process, breakpointInvocation!, "completed", artifacts, "launch-continued-detached");
            InvokeMenu(gui, process, "Undo", TimeSpan.FromSeconds(8), "Edit");
            VerifyOriginalBaseline("launch-breakpoint-undone");
        });
        Step("launch-no-breakpoint-completes", () =>
        {
            Ensure(ReadPauseOnEntry(workspace!) == ToggleState.Off, "No-breakpoint Debug did not start from the default entry policy");
            var invocation = Task.Run(() => InvokeButton(workspace!, "Debug"));
            WaitForInvocationWithoutPause(workspace!, process, invocation, artifacts, "launch-no-breakpoint");
            Ensure(ReadRunStatus(workspace!).Contains("completed", StringComparison.OrdinalIgnoreCase),
                "No-breakpoint Debug did not reach its natural completed terminal state");
            File.WriteAllText(Path.Combine(artifacts, "launch-no-breakpoint-status.txt"), ReadRunStatus(workspace!));
            SaveEvidence(workspace!, artifacts, "launch-no-breakpoint-completed");
            InvokeMenu(gui, process, "Undo", TimeSpan.FromSeconds(8), "Edit");
            VerifyOriginalBaseline("launch-no-breakpoint-undone");
        });
        Step("launch-entry-opt-in", () =>
        {
            SetPauseOnEntry(workspace!, true);
            var invocation = Task.Run(() => InvokeButton(workspace!, "Debug"));
            WaitUntil(() => ParsePauseStatus(ReadRunStatus(workspace!)) is { Reason: "entry", Sequence: 1 } pause
                    && pause.SourcePath.EndsWith("/kara-templater.lua", StringComparison.OrdinalIgnoreCase),
                TimeSpan.FromSeconds(15), "Explicit Pause on entry did not retain the entry #1 behavior");
            AssertLivePauseControls(gui, workspace!);
            var runStatus = CaptureRunStatusHandle(workspace!, process);
            SaveEvidence(workspace!, artifacts, "launch-entry-opt-in");
            InvokeButton(workspace!, "Stop");
            var closedProgress = false;
            WaitUntil(() =>
            {
                var progress = FindProgressPane(process, "Apply karaoke template");
                if (!closedProgress && progress is not null)
                {
                    var close = progress.FindAll(TreeScope.Descendants,
                        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button)).Cast<AutomationElement>()
                        .FirstOrDefault(item => item.Current.Name == "Close" && item.Current.IsEnabled
                            && item.TryGetCurrentPattern(InvokePattern.Pattern, out _));
                    if (close is not null)
                    {
                        SaveEvidence(progress, artifacts, "launch-entry-stop-progress-close");
                        UiaDriver.Invoke(close);
                        closedProgress = true;
                    }
                }
                return invocation.IsCompleted && gui.Current.IsEnabled;
            }, TimeSpan.FromSeconds(25), "Entry opt-in Stop did not return and release its progress UI");
            Ensure(invocation.Wait(TimeSpan.FromSeconds(5)), "Entry opt-in Stop task did not finish after terminal UI release");
            Ensure(Native.ReadStaticText(runStatus, process).Contains("cancelled", StringComparison.OrdinalIgnoreCase),
                "Entry opt-in Stop did not reach cancelled");
            Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline),
                "Entry opt-in cancellation changed the physical ASS baseline");
        });
        Step("launch-normal-shutdown", () =>
        {
            Native.PostClose(workspace!, process);
            var dialog = WaitWindow(process, "Unsaved Lua source", TimeSpan.FromSeconds(5));
            SaveEvidence(dialog, artifacts, "launch-discard-unsaved-source");
            InvokeButton(dialog, "Discard");
            WaitUntil(() => FindWindow(process, "Lua Workspace") is null, TimeSpan.FromSeconds(5),
                "Launch Workspace did not close after Discard");
            Native.StabilizeOwnedClipboard(process.Id);
            if (!gui.TryGetCurrentPattern(WindowPattern.Pattern, out var pattern))
                throw new InvalidOperationException("Launch main window lacks WindowPattern.Close");
            ((WindowPattern)pattern).Close();
            Ensure(process.WaitForExit(TimeSpan.FromSeconds(10)) && process.ExitCode == 0,
                "Launch host did not exit normally with code zero");
            File.WriteAllText(Path.Combine(artifacts, "launch-normal-shutdown.json"), JsonSerializer.Serialize(new
            {
                HostExitCode = process.ExitCode, Method = "WindowPattern.Close"
            }));
        });

        void VerifyOriginalBaseline(string evidence)
        {
            WaitUntil(() => gui.Current.IsEnabled, TimeSpan.FromSeconds(5), "Debug completion did not release the main frame");
            var before = File.ReadAllBytes(input);
            var verifier = Task.Run(() => InvokeMenu(gui, process, "Workspace Debug Verify Original", TimeSpan.FromSeconds(8)));
            var dialog = WaitWindowContainingText(process, "Debug original live baseline verified", TimeSpan.FromSeconds(8));
            SaveEvidence(dialog, artifacts, evidence);
            InvokeButton(dialog, "OK");
            Ensure(verifier.Wait(TimeSpan.FromSeconds(10)), "Launch live-baseline verifier did not return");
            WaitUntil(() => gui.Current.IsEnabled && FindProgressPane(process, "Workspace Debug Verify Original") is null,
                TimeSpan.FromSeconds(8), "Launch live-baseline verifier did not release its execution UI");
            Ensure(File.ReadAllBytes(input).SequenceEqual(before) && EventLines(File.ReadAllText(input)).SequenceEqual(baseline),
                "Launch live-baseline verifier changed or disagreed with the physical ASS baseline");
        }
    }

    string[] EntrySteps() => new[] { "host-ready", "entry-menu-before", "entry-open-workspace", "entry-run",
        "entry-result", "entry-menu-after", "entry-normal-shutdown" };

    void RunEntry()
    {
        var gui = main ?? throw new InvalidOperationException("Main GUI frame is unavailable");
        var process = host ?? throw new InvalidOperationException("GUI host is unavailable");
        var menuName = scenario == "entry-zh" ? "自动化(U)" : "Automation";
        var hasTemplater = scenario != "entry-missing";

        void CheckMenu(string evidence)
        {
            var (probe, templater) = ObserveEntryMenu(gui, process, menuName);
            Ensure(probe is { IsEnabled: false }, "Ordinary macro validation was bypassed by the Workspace entry policy");
            Ensure(hasTemplater ? templater is { IsEnabled: false } : templater is null,
                "The regular Automation menu did not retain its independent quick-validation behavior");
            if (scenario == "entry-zh")
                Ensure(templater!.Name.Contains("应用", StringComparison.Ordinal), "The Chinese locale did not translate the templater display label");
            File.WriteAllText(Path.Combine(artifacts, evidence), JsonSerializer.Serialize(new { Probe = probe, Templater = templater },
                new JsonSerializerOptions { WriteIndented = true }));
        }

        Step("entry-menu-before", () => CheckMenu("entry-menu-before.json"));
        Step("entry-open-workspace", () =>
        {
            InvokeMenu(gui, process, "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(8), menuName);
            workspace = WaitWindow(process, "Lua Workspace", TimeSpan.FromSeconds(8));
            Ensure(FindButton(workspace, "Run").Current.IsEnabled, "Workspace Run is unavailable for the selected code line");
            SaveEvidence(workspace, artifacts, "entry-workspace-open");
        });
        Step("entry-run", () =>
        {
            if (scenario is "entry-missing" or "entry-ambiguous")
            {
                InvokeButton(workspace!, "Run");
                WaitUntil(() => ReadRunStatus(workspace!).Contains(
                    "Exactly one loaded karaoke templater macro is required", StringComparison.Ordinal),
                    TimeSpan.FromSeconds(5), "Missing or ambiguous templater identity was not rejected explicitly");
                Ensure(gui.Current.IsEnabled && FindButton(workspace!, "Run").Current.IsEnabled,
                    "Rejected Workspace entry retained a subtitle transaction or active run");
            }
            else
            {
                var invocation = Task.Run(() => InvokeButton(workspace!, "Run"));
                WaitUntil(() => invocation.IsCompleted && ReadRunStatus(workspace!).Contains("completed", StringComparison.OrdinalIgnoreCase)
                    && gui.Current.IsEnabled, TimeSpan.FromSeconds(25), "Workspace templater entry did not complete");
                Ensure(invocation.Wait(TimeSpan.FromSeconds(5)), "Workspace templater invocation did not return");
                WaitUntil(() => ReadRunLog(workspace!, process).Contains("workspace-entry-code-once", StringComparison.Ordinal),
                    TimeSpan.FromSeconds(5), "The code-once source did not execute through the selected templater");
            }
            SaveEvidence(workspace!, artifacts, "entry-run-result");
            File.WriteAllText(Path.Combine(artifacts, "entry-run-log.txt"), ReadRunLog(workspace!, process));
        });
        Step("entry-result", () =>
        {
            if (scenario is "entry-late" or "entry-zh")
            {
                var expected = baseline.ToList();
                var targetIndex = expected.FindIndex(line => line.Kind == "Dialogue" && line.Effect == "");
                Ensure(targetIndex == expected.Count - 1, "Late-template input has no unique final karaoke dialogue");
                expected[targetIndex] = expected[targetIndex] with { Kind = "Comment", Effect = "karaoke" };
                expected.Add(new AssEvent("Dialogue", 0, 1000, 2000, "Default", "", 0, 0, 0, "fx", "R1-LATER1-LATE"));
                SaveEntryAss(gui, input, expected, artifacts, "entry-generated.ass");
            }
            else
            {
                Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline),
                    "A code-only or rejected entry changed the physical subtitle document");
                var save = UiaDriver.FindDescendantByAutomationId(gui, "Item 5002", ControlType.Button);
                if (save is not null && save.Current.IsEnabled)
                    SaveEntryAss(gui, input, baseline, artifacts, "entry-unchanged.ass");
                else
                    File.Copy(input, Path.Combine(artifacts, "entry-unchanged.ass"));
            }
        });
        Step("entry-menu-after", () => CheckMenu("entry-menu-after.json"));
        Step("entry-normal-shutdown", () =>
        {
            Ensure(gui.TryGetCurrentPattern(WindowPattern.Pattern, out var pattern), "Main frame lacks WindowPattern.Close");
            Native.StabilizeOwnedClipboard(process.Id);
            ((WindowPattern)pattern).Close();
            Ensure(process.WaitForExit(TimeSpan.FromSeconds(10)) && process.ExitCode == 0,
                "GUI host did not exit normally after Workspace entry acceptance");
            File.WriteAllText(Path.Combine(artifacts, "entry-normal-shutdown.json"), JsonSerializer.Serialize(new
            {
                HostExitCode = process.ExitCode, Method = "WindowPattern.Close"
            }));
        });
    }

    string[] FileSteps() => new[] { "host-ready", "files-open-noentry", "files-noentry-first-run", "files-noentry-reload",
        "files-noentry-recover", "files-noentry-undo", "files-readonly-failure", "files-readonly-live-baseline",
        "files-readonly-recovery", "files-readonly-undo", "files-managed-beta", "files-managed-beta-undo",
        "files-managed-gamma", "files-managed-gamma-undo", "files-normal-shutdown" };

    void PrepareFiles()
    {
        foreach (var name in new[] { "debug-file-noentry.lua", "debug-file-save.lua", "debug-file-save-edited.lua",
            "debug-file-macros.lua", "debug-file-macros-v2.lua" })
        {
            var source = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", name);
            hashes[name] = Hash(source);
            File.Copy(source, Path.Combine(artifacts, name));
        }
        var physical = File.ReadAllText(input);
        const string before = "Automation Scripts: ~debug-actions.lua|~kara-templater.lua";
        Ensure(physical.Split(before, StringSplitOptions.None).Length == 2, "ASS fixture has no unique Automation Scripts metadata to extend");
        physical = physical.Replace(before, before + "|~debug-file-macros.lua", StringComparison.Ordinal);
        File.WriteAllText(input, physical, new UTF8Encoding(false));
        hashes["files-prepared-input-ass"] = Hash(input);
        Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), "File-scenario script metadata changed event baseline");
        Ensure(!File.Exists(Path.Combine(artifacts, "debug-noentry-loads.txt")), "No-entry loader marker predates GUI host startup");
    }

    void RunFiles()
    {
        var gui = main ?? throw new InvalidOperationException("Main GUI frame is unavailable");
        var process = host ?? throw new InvalidOperationException("GUI host is unavailable");
        var noentry = Path.Combine(artifacts, "debug-file-noentry.lua");
        var marker = Path.Combine(artifacts, "debug-noentry-loads.txt");
        var saveFile = Path.Combine(artifacts, "debug-file-save.lua");
        var savedEdited = File.ReadAllText(Path.Combine(artifacts, "debug-file-save-edited.lua"));
        var managedFile = Path.Combine(artifacts, "debug-file-macros.lua");
        var managedEdited = File.ReadAllText(Path.Combine(artifacts, "debug-file-macros-v2.lua"));
        var recovered = baseline.ToArray();
        recovered[0] = recovered[0] with { Effect = "noentry-recovered" };
        var saved = baseline.ToArray();
        saved[0] = saved[0] with { Effect = "file-saved" };
        var beta = baseline.ToArray();
        beta[0] = beta[0] with { Effect = "managed-beta" };
        var gamma = baseline.ToArray();
        gamma[0] = gamma[0] with { Effect = "managed-gamma" };
        AutomationElement? contextControl = null;

        int MarkerCount() => File.Exists(marker) ? File.ReadAllLines(marker).Length : 0;

        void WaitNoEntry(int expectedCount)
        {
            WaitUntil(() => MarkerCount() == expectedCount
                && ReadRunStatus(workspace!).Contains("The reloaded Lua file has no registered macro to run", StringComparison.Ordinal),
                TimeSpan.FromSeconds(8), "No-entry Run did not report the exact missing-macro condition after source load");
            Ensure(!FindButton(workspace!, "Run").Current.IsEnabled && !FindButton(workspace!, "Debug").Current.IsEnabled,
                "No-entry revision left Run or Debug available");
            Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), "No-entry source load modified physical ASS");
        }

        void WaitFileRun(Task invocation, string script, string macro)
        {
            var feature = "Feature: automation/lua/" + script + "/" + macro;
            WaitUntil(() =>
            {
                var view = Native.ReadEditText(contextControl ?? throw new InvalidOperationException("File Context Edit is unavailable"), process);
                return invocation.IsCompleted && view.Split('\n').Any(line => line.TrimEnd('\r') == feature)
                    && view.Split('\n').Any(line => line.TrimEnd('\r') == "Status: completed")
                    && ReadRunStatus(workspace!).Contains("completed", StringComparison.OrdinalIgnoreCase)
                    && gui.Current.IsEnabled;
            }, TimeSpan.FromSeconds(15), $"{macro} did not reach its exact completed Workspace observation");
            Ensure(invocation.Wait(TimeSpan.FromSeconds(5)), $"{macro} GUI invocation task did not return");
        }

        AutomationElement FindContextEdit()
        {
            SelectTab(workspace!, "Context");
            var candidates = workspace!.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>()
                .Where(item => item.Current.ProcessId == process.Id && !item.Current.IsOffscreen
                    && item.Current.ClassName == "Edit" && item.Current.ControlType == ControlType.Document
                    && item.Current.NativeWindowHandle != 0 && HasNotebookAncestor(item, workspace!)).ToArray();
            Ensure(candidates.Length == 1, $"Context tab exposes {candidates.Length} visible native Edit controls");
            return candidates[0];
        }

        void VerifyOriginal(string evidence)
        {
            var bytes = File.ReadAllBytes(input);
            var commandReady = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var task = Task.Run(() => InvokeMenu(gui, process, "Workspace Debug Verify Original", TimeSpan.FromSeconds(8),
                commandReady: () => commandReady.TrySetResult()));
            WaitUntil(() => commandReady.Task.IsCompleted || task.IsCompleted, TimeSpan.FromSeconds(12),
                "Live baseline verifier command did not become invokable");
            if (!commandReady.Task.IsCompleted) task.GetAwaiter().GetResult();
            var dialog = WaitWindowContainingText(process, "Debug original live baseline verified", TimeSpan.FromSeconds(8));
            SaveEvidence(dialog, artifacts, evidence + "-live-dialog");
            InvokeButton(dialog, "OK");
            Ensure(task.Wait(TimeSpan.FromSeconds(10)), "Independent live ASS verifier did not finish after exact OK");
            WaitUntil(() => gui.Current.IsEnabled && FindProgressPane(process, "Workspace Debug Verify Original") is null,
                TimeSpan.FromSeconds(8), "Independent live ASS verifier did not release its execution UI");
            Ensure(File.ReadAllBytes(input).SequenceEqual(bytes) && EventLines(File.ReadAllText(input)).SequenceEqual(baseline),
                "Independent live ASS verifier changed the physical baseline");
        }

        void UndoToOriginal(string evidence)
        {
            InvokeMenu(gui, process, "Undo", TimeSpan.FromSeconds(8), "Edit");
            SaveMainAss(gui, input, baseline, artifacts, evidence);
        }

        void ChooseManagedMacro(string selected, string absent, string evidence)
        {
            var task = Task.Run(() => InvokeButton(workspace!, "Run"));
            var dialog = WaitWindow(process, "Lua Workspace macro", TimeSpan.FromSeconds(8));
            SaveEvidence(dialog, artifacts, evidence + "-choice");
            var choices = dialog.FindAll(TreeScope.Descendants,
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ListItem)).Cast<AutomationElement>()
                .Where(item => !item.Current.IsOffscreen).ToArray();
            Ensure(choices.Length == 2 && choices.Any(item => item.Current.Name == "Workspace File Alpha")
                && choices.Any(item => item.Current.Name == selected)
                && choices.All(item => item.Current.Name != absent), "Managed macro choice did not reflect the saved/reloaded registry");
            var choice = choices.Single(item => item.Current.Name == selected);
            if (!choice.TryGetCurrentPattern(SelectionItemPattern.Pattern, out var select))
                throw new InvalidOperationException("Exact managed macro choice has no SelectionItemPattern");
            ((SelectionItemPattern)select).Select();
            var okay = UiaDriver.FindDescendantByAutomationId(dialog, "5100", ControlType.Button)
                ?? throw new InvalidOperationException("Macro-choice dialog has no observed wx OK ID 5100");
            Ensure(okay.Current.IsEnabled && okay.TryGetCurrentPattern(InvokePattern.Pattern, out _), "Macro choice OK cannot be invoked");
            UiaDriver.Invoke(okay);
            WaitUntil(() => FindWindow(process, "Lua Workspace macro") is null, TimeSpan.FromSeconds(5),
                "Macro-choice dialog remained after its exact selection");
            WaitFileRun(task, "debug-file-macros", selected);
        }

        void VerifyManagedMenuRegistry()
        {
            var menus = gui.FindAll(TreeScope.Descendants,
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem)).Cast<AutomationElement>()
                .Where(item => !item.Current.IsOffscreen
                    && (item.Current.Name ?? "").Replace("&", "", StringComparison.Ordinal) == "Automation").ToArray();
            Ensure(menus.Length == 1, "Main frame has no unique visible Automation menu after managed reload");
            var opened = false;
            if (menus[0].TryGetCurrentPattern(ExpandCollapsePattern.Pattern, out var expand))
            {
                try { ((ExpandCollapsePattern)expand).Expand(); opened = true; }
                catch (InvalidOperationException) { }
            }
            if (!opened && menus[0].TryGetCurrentPattern(InvokePattern.Pattern, out var invoke))
            {
                ((InvokePattern)invoke).Invoke();
                opened = true;
            }
            if (!opened) Native.PostSystemMenu(new nint(gui.Current.NativeWindowHandle), 'u');
            WaitUntil(() => FindVisibleMenuCommand(process, "Workspace File Gamma") is not null, TimeSpan.FromSeconds(5),
                "Reloaded managed macro Gamma did not appear in the real Automation menu");
            Ensure(FindVisibleMenuCommand(process, "Workspace File Beta") is null,
                "Stale managed macro Beta remained in the real Automation menu after saved reload");
            TryProcessEvidence(process, artifacts, "files-managed-registry");
        }

        Step("files-open-noentry", () =>
        {
            InvokeMenu(gui, process, "Workspace Debug Select First Code", TimeSpan.FromSeconds(8));
            InvokeMenu(gui, process, "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(8));
            workspace = WaitWindow(process, "Lua Workspace", TimeSpan.FromSeconds(8));
            SaveEvidence(workspace, artifacts, "files-workspace-opened");
            OpenLuaFile(workspace, process, noentry, artifacts, "files-noentry-picker");
            contextControl = FindContextEdit();
            Ensure(MarkerCount() == 0, "Opening a Lua source file executed its top-level loader");
            InvokeButton(workspace, "Save");
            Ensure(MarkerCount() == 0 && EventLines(File.ReadAllText(input)).SequenceEqual(baseline),
                "Saving a Lua source file executed its top-level loader or changed ASS");
            SaveEvidence(workspace, artifacts, "files-noentry-opened");
        });
        Step("files-noentry-first-run", () =>
        {
            InvokeButton(workspace!, "Run");
            WaitNoEntry(1);
            File.WriteAllText(Path.Combine(artifacts, "noentry-first-run.txt"), File.ReadAllText(marker));
            SaveEvidence(workspace!, artifacts, "files-noentry-disabled");
        });
        Step("files-noentry-reload", () =>
        {
            InvokeButton(workspace!, "Reload");
            WaitUntil(() => FindButton(workspace!, "Run").Current.IsEnabled && FindButton(workspace!, "Debug").Current.IsEnabled,
                TimeSpan.FromSeconds(5), "Explicit Reload did not clear the no-macro revision lock");
            Ensure(MarkerCount() == 1, "Explicit Reload executed the Lua source unexpectedly");
            InvokeButton(workspace!, "Run");
            WaitNoEntry(2);
            File.WriteAllText(Path.Combine(artifacts, "noentry-second-run.txt"), File.ReadAllText(marker));
        });
        Step("files-noentry-recover", () =>
        {
            var source = File.ReadAllText(noentry) + "\naegisub.register_macro(\"Workspace File Recovered\", \"Recovered entry\", function(subs, selected, active)\n"
                + "  local line = subs[active]\n  line.effect = \"noentry-recovered\"\n  subs[active] = line\n  return selected, active\nend)\n";
            var recoveredPath = Path.Combine(artifacts, "noentry-recovered.lua");
            File.WriteAllText(recoveredPath, source, new UTF8Encoding(false));
            hashes["files-generated-noentry-recovered"] = Hash(recoveredPath);
            var editor = FindStyledText(workspace!, insideNotebook: false)
                ?? throw new InvalidOperationException("No-entry source editor was not found");
            Native.ReplaceWithClipboard(editor, process, source);
            Ensure(Normalize(CopySource(workspace!, process)) == Normalize(source), "Recovered macro edit was not retained in the Workspace buffer");
            WaitUntil(() => FindButton(workspace!, "Run").Current.IsEnabled && FindButton(workspace!, "Debug").Current.IsEnabled,
                TimeSpan.FromSeconds(5), "A new editor revision did not re-enable Run/Debug after no-entry");
            Ensure(MarkerCount() == 2, "Editing the no-entry buffer executed the top-level loader");
            var task = Task.Run(() => InvokeButton(workspace!, "Run"));
            WaitFileRun(task, "debug-file-noentry", "Workspace File Recovered");
            Ensure(MarkerCount() == 3 && Normalize(File.ReadAllText(noentry)) == Normalize(source),
                "Recovered Run did not save/reload the new macro source exactly once");
            SaveMainAss(gui, input, recovered, artifacts, "noentry-recovered.ass");
        });
        Step("files-noentry-undo", () => UndoToOriginal("noentry-undone.ass"));
        Step("files-readonly-failure", () =>
        {
            OpenLuaFile(workspace!, process, saveFile, artifacts, "files-readonly-picker");
            var originalBytes = File.ReadAllBytes(saveFile);
            Ensure(originalBytes.Length >= 3 && originalBytes[0] == 0xef && originalBytes[1] == 0xbb && originalBytes[2] == 0xbf,
                "Read-only fixture lost its UTF-8 BOM before the GUI run");
            var originalAttributes = File.GetAttributes(saveFile);
            try
            {
                File.SetAttributes(saveFile, originalAttributes | FileAttributes.ReadOnly);
                var editor = FindStyledText(workspace!, insideNotebook: false)
                    ?? throw new InvalidOperationException("Read-only Lua-file editor was not found");
                Native.ReplaceWithClipboard(editor, process, savedEdited);
                Ensure(Normalize(CopySource(workspace!, process)) == Normalize(savedEdited), "Read-only source edit was not retained before Run");
                InvokeButton(workspace!, "Run");
                WaitUntil(() => HasWorkspaceStaticText(workspace!, value => value.Contains("Save the Lua file successfully before Run/Debug", StringComparison.Ordinal)),
                    TimeSpan.FromSeconds(6), "Run did not report the failed Lua-file Save boundary");
                Ensure(File.ReadAllBytes(saveFile).SequenceEqual(originalBytes)
                    && Normalize(CopySource(workspace!, process)) == Normalize(savedEdited)
                    && workspace!.Current.Name.Contains(" *", StringComparison.Ordinal)
                    && EventLines(File.ReadAllText(input)).SequenceEqual(baseline),
                    "Failed read-only Save modified disk, ASS, or the dirty editor buffer");
                SaveEvidence(workspace!, artifacts, "files-readonly-failed");
            }
            finally { File.SetAttributes(saveFile, originalAttributes); }
        });
        Step("files-readonly-live-baseline", () => VerifyOriginal("files-readonly"));
        Step("files-readonly-recovery", () =>
        {
            var task = Task.Run(() => InvokeButton(workspace!, "Run"));
            WaitFileRun(task, "debug-file-save", "Workspace File Saved");
            var bytes = File.ReadAllBytes(saveFile);
            Ensure(bytes.Length >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf
                && Normalize(File.ReadAllText(saveFile)) == Normalize(savedEdited),
                "Recovered Lua-file Run did not save the edited macro while preserving its BOM");
            SaveMainAss(gui, input, saved, artifacts, "files-readonly-recovered.ass");
        });
        Step("files-readonly-undo", () => UndoToOriginal("files-readonly-undone.ass"));
        Step("files-managed-beta", () =>
        {
            InvokeMenu(gui, process, "Workspace Debug Select First Code", TimeSpan.FromSeconds(8));
            OpenLuaFile(workspace!, process, managedFile, artifacts, "files-managed-picker");
            ChooseManagedMacro("Workspace File Beta", "Workspace File Gamma", "files-managed-beta");
            SaveMainAss(gui, input, beta, artifacts, "files-managed-beta.ass");
        });
        Step("files-managed-beta-undo", () => UndoToOriginal("files-managed-beta-undone.ass"));
        Step("files-managed-gamma", () =>
        {
            var oldBytes = File.ReadAllBytes(managedFile);
            var editor = FindStyledText(workspace!, insideNotebook: false)
                ?? throw new InvalidOperationException("Managed Lua-file editor was not found");
            Native.ReplaceWithClipboard(editor, process, managedEdited);
            Ensure(Normalize(CopySource(workspace!, process)) == Normalize(managedEdited)
                && File.ReadAllBytes(managedFile).SequenceEqual(oldBytes),
                "Managed macro edit was applied to disk before the explicit Run Save/Reload");
            ChooseManagedMacro("Workspace File Gamma", "Workspace File Beta", "files-managed-gamma");
            Ensure(Normalize(File.ReadAllText(managedFile)) == Normalize(managedEdited),
                "Managed script Reload did not use the newly saved registry source");
            SaveMainAss(gui, input, gamma, artifacts, "files-managed-gamma.ass");
            VerifyManagedMenuRegistry();
        });
        Step("files-managed-gamma-undo", () => UndoToOriginal("files-managed-gamma-undone.ass"));
        Step("files-normal-shutdown", () =>
        {
            Native.StabilizeOwnedClipboard(process.Id);
            if (!gui.TryGetCurrentPattern(WindowPattern.Pattern, out var pattern))
                throw new InvalidOperationException("Main frame lacks real WindowPattern.Close");
            ((WindowPattern)pattern).Close();
            Ensure(process.WaitForExit(TimeSpan.FromSeconds(10)), "File-scenario GUI host did not exit normally within ten seconds");
            Ensure(process.ExitCode == 0, "File-scenario GUI host returned nonzero after normal close");
            File.WriteAllText(Path.Combine(artifacts, "files-normal-shutdown.json"), JsonSerializer.Serialize(new
            {
                HostExitCode = process.ExitCode, Method = "WindowPattern.Close"
            }, new JsonSerializerOptions { WriteIndented = true }));
        });
    }

    string[] PreparationSteps() => ["host-ready", "preparation-open", "preparation-top-level-stop",
        "preparation-top-level-recovery", "preparation-include-stop", "preparation-include-recovery",
        "preparation-validation-stop", "preparation-validation-recovery", "preparation-include-failure",
        "preparation-validation-failure", "preparation-validation-error", "preparation-sethook-stop",
        "preparation-sethook-recovery", "preparation-gc-deferred", "preparation-choice-cancel", "preparation-choice-recovery",
        "preparation-normal-shutdown"];

    void PreparePreparation()
    {
        foreach (var name in preparationFixtures)
        {
            var source = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", name);
            hashes[name] = Hash(source);
            File.Copy(source, Path.Combine(artifacts, name));
        }
    }

    void RunPreparation()
    {
        var gui = main ?? throw new InvalidOperationException("Main GUI frame is unavailable");
        var process = host ?? throw new InvalidOperationException("GUI host is unavailable");
        const string healthyTemplate = "script_name = \"Preparation recovery\"\nscript_description = \"Preparation recovery\"\n"
            + "script_author = \"Aegisub E2E\"\nscript_version = \"1\"\n\n"
            + "aegisub.register_macro(\"Preparation Recovery TAG\", \"Confirm preparation cleanup\", function(subs, selected, active)\n"
            + "  aegisub.debug.out(\"preparation-recovery|TAG\\n\")\n  return selected, active\nend)\n";
        string? currentFile = null;

        void Open(string file, string evidence)
        {
            OpenLuaFile(workspace!, process, Path.Combine(artifacts, file), artifacts, evidence);
            currentFile = Path.Combine(artifacts, file);
            Ensure(workspace!.Current.Name.Contains(file, StringComparison.Ordinal),
                $"Preparation fixture {file} was not bound to Workspace");
        }

        void AssertPreparing(string label)
        {
            var stop = FindButton(workspace!, "Stop");
            Ensure(ReadRunStatus(workspace!).StartsWith("Preparing", StringComparison.Ordinal),
                $"{label} did not expose the Preparing status");
            Ensure(!stop.Current.IsOffscreen && stop.Current.IsEnabled,
                $"{label} did not expose a visible enabled Stop control while Preparing");
            Ensure(!gui.Current.IsEnabled, $"{label} did not lock the main frame while Preparing");
            foreach (var name in new[] { "Run", "Debug", "Open File", "Reload" })
                Ensure(!FindButton(workspace!, name).Current.IsEnabled, $"{label} left unsafe {name} enabled while Preparing");
        }

        void VerifyOriginal(string evidence)
        {
            Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline),
                $"{evidence} changed the physical ASS baseline");
            var bytes = File.ReadAllBytes(input);
            var commandReady = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var verifier = Task.Run(() => InvokeMenu(gui, process, "Workspace Debug Verify Original", TimeSpan.FromSeconds(8),
                commandReady: () => commandReady.TrySetResult()));
            WaitUntil(() => commandReady.Task.IsCompleted || verifier.IsCompleted, TimeSpan.FromSeconds(12),
                $"{evidence} live ASS verifier command did not become invokable");
            if (!commandReady.Task.IsCompleted) verifier.GetAwaiter().GetResult();
            var dialog = WaitWindowContainingText(process, "Debug original live baseline verified", TimeSpan.FromSeconds(8));
            SaveEvidence(dialog, artifacts, evidence + "-live-baseline");
            InvokeButton(dialog, "OK");
            Ensure(verifier.Wait(TimeSpan.FromSeconds(10)), $"{evidence} live ASS verifier did not return");
            WaitUntil(() => gui.Current.IsEnabled && FindProgressPane(process, "Workspace Debug Verify Original") is null,
                TimeSpan.FromSeconds(8), $"{evidence} live ASS verifier did not release its execution UI");
            Ensure(File.ReadAllBytes(input).SequenceEqual(bytes) && EventLines(File.ReadAllText(input)).SequenceEqual(baseline),
                $"{evidence} live ASS verifier changed or disagreed with the physical baseline");
        }

        void AssertIdle(string label)
        {
            Ensure(gui.Current.IsEnabled, $"{label} did not release the main frame");
            Ensure(!FindButton(workspace!, "Stop").Current.IsEnabled, $"{label} retained an enabled Stop control");
            foreach (var name in new[] { "Run", "Debug", "Open File", "Reload" })
                Ensure(FindButton(workspace!, name).Current.IsEnabled, $"{label} did not restore {name}");
            Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), $"{label} changed the physical ASS baseline");
        }

        void CancelPreparation(string label, string evidence)
        {
            var invocation = Task.Run(() => InvokeButton(workspace!, "Run"));
            WaitUntil(() => ReadRunStatus(workspace!).StartsWith("Preparing", StringComparison.Ordinal)
                    && FindButton(workspace!, "Stop").Current.IsEnabled,
                TimeSpan.FromSeconds(12), $"{label} did not reach a stoppable Preparing state");
            AssertPreparing(label);
            File.WriteAllText(Path.Combine(artifacts, evidence + "-preparing-status.txt"), ReadRunStatus(workspace!));
            SaveEvidence(workspace!, artifacts, evidence + "-preparing");
            InvokeButton(workspace!, "Stop");
            Ensure(invocation.Wait(TimeSpan.FromSeconds(15)), $"{label} did not terminate through Stop");
            WaitUntil(() => ReadRunStatus(workspace!).Contains("cancelled", StringComparison.OrdinalIgnoreCase)
                    && gui.Current.IsEnabled,
                TimeSpan.FromSeconds(8), $"{label} did not reach a cancelled terminal state and release the main frame");
            AssertIdle(label);
            File.WriteAllText(Path.Combine(artifacts, evidence + "-cancelled-status.txt"), ReadRunStatus(workspace!));
            SaveEvidence(workspace!, artifacts, evidence + "-cancelled");
            VerifyOriginal(evidence + "-cancelled");
        }

        void Recover(string tag, string evidence)
        {
            var source = healthyTemplate.Replace("TAG", tag, StringComparison.Ordinal);
            Ensure(currentFile is not null, $"{tag} recovery has no bound source file");
            File.WriteAllText(currentFile!, source, new UTF8Encoding(false));
            Ensure(File.ReadAllText(currentFile!) == source, $"{tag} recovery source was not written exactly");
            InvokeButton(workspace!, "Reload");
            Ensure(workspace!.Current.Name.Contains(Path.GetFileName(currentFile!), StringComparison.Ordinal),
                $"{tag} recovery Reload lost the bound file identity");
            var invocation = Task.Run(() => InvokeButton(workspace!, "Run"));
            WaitForInvocation(workspace!, process, invocation, "completed", artifacts, evidence);
            WaitUntil(() => ReadRunLog(workspace!, process).Contains("preparation-recovery|" + tag, StringComparison.Ordinal),
                TimeSpan.FromSeconds(5), $"{tag} recovery macro did not execute");
            AssertIdle(tag + " recovery");
            VerifyOriginal(evidence);
        }

        void RejectAndRecover(string file, string expected, string tag, string evidence, string? logMarker = null)
        {
            Open(file, evidence + "-picker");
            var invocation = Task.Run(() => InvokeButton(workspace!, "Run"));
            WaitUntil(() => invocation.IsCompleted && ReadRunStatus(workspace!).Contains(expected, StringComparison.Ordinal),
                TimeSpan.FromSeconds(12), $"{tag} did not report its exact preparation failure");
            Ensure(invocation.Wait(TimeSpan.FromSeconds(3)), $"{tag} failure task did not return");
            if (logMarker is not null)
                Ensure(ReadRunLog(workspace!, process).Contains(logMarker, StringComparison.Ordinal),
                    $"{tag} did not report the exact Lua runtime error");
            AssertIdle(tag);
            SaveEvidence(workspace!, artifacts, evidence + "-rejected");
            VerifyOriginal(evidence + "-rejected");
            Recover(tag, evidence + "-recovery");
        }

        Step("preparation-open", () =>
        {
            InvokeMenu(gui, process, "Workspace Debug Select First Code", TimeSpan.FromSeconds(8));
            InvokeMenu(gui, process, "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(8));
            workspace = WaitWindow(process, "Lua Workspace", TimeSpan.FromSeconds(8));
            Open("preparation-top-level-loop.lua", "preparation-top-level-picker");
            SaveEvidence(workspace, artifacts, "preparation-opened");
        });
        Step("preparation-top-level-stop", () => CancelPreparation("Top-level loop", "preparation-top-level"));
        Step("preparation-top-level-recovery", () => Recover("top-level", "preparation-top-level-recovery"));
        Step("preparation-include-stop", () =>
        {
            Open("preparation-include-loop.lua", "preparation-include-picker");
            CancelPreparation("Included-source loop", "preparation-include");
        });
        Step("preparation-include-recovery", () => Recover("include", "preparation-include-recovery"));
        Step("preparation-validation-stop", () =>
        {
            Open("preparation-validation-loop.lua", "preparation-validation-picker");
            CancelPreparation("Macro-validation loop", "preparation-validation");
        });
        Step("preparation-validation-recovery", () => Recover("validation", "preparation-validation-recovery"));
        Step("preparation-include-failure", () => RejectAndRecover("preparation-include-failure.lua",
            "preparation-missing-include.lua", "include-failure", "preparation-include-failure"));
        Step("preparation-validation-failure", () => RejectAndRecover("preparation-validation-failure.lua",
            "The selected Automation macro is not available for this subtitle document", "validation-failure", "preparation-validation-failure"));
        Step("preparation-validation-error", () => RejectAndRecover("preparation-validation-error.lua",
            "Lua macro validation failed", "validation-error", "preparation-validation-error", "preparation-validation-runtime-error"));
        Step("preparation-sethook-stop", () =>
        {
            Open("preparation-sethook-loop.lua", "preparation-sethook-picker");
            CancelPreparation("Lua debug.sethook override", "preparation-sethook");
        });
        Step("preparation-sethook-recovery", () => Recover("sethook", "preparation-sethook-recovery"));
        Step("preparation-gc-deferred", () =>
        {
            Open("preparation-gc-deferred.lua", "preparation-gc-picker");
            var invocation = Task.Run(() => InvokeButton(workspace!, "Run"));
            WaitForInvocation(workspace!, process, invocation, "completed", artifacts, "preparation-gc-deferred");
            Ensure(ReadRunLog(workspace!, process).Contains("preparation-gc-deferred|validated", StringComparison.Ordinal),
                "Workspace forced a full GC between guarded validation and macro execution");
            AssertIdle("Deferred Workspace GC");
            VerifyOriginal("preparation-gc-deferred");
        });
        Step("preparation-choice-cancel", () =>
        {
            Open("preparation-choice.lua", "preparation-choice-picker");
            var invocation = Task.Run(() => InvokeButton(workspace!, "Run"));
            var dialog = WaitWindow(process, "Lua Workspace macro", TimeSpan.FromSeconds(10));
            SaveEvidence(dialog, artifacts, "preparation-choice-dialog");
            InvokeButton(dialog, "Cancel");
            Ensure(invocation.Wait(TimeSpan.FromSeconds(8)), "Cancelling macro selection did not return the Run action");
            WaitUntil(() => FindWindow(process, "Lua Workspace macro") is null && gui.Current.IsEnabled,
                TimeSpan.FromSeconds(5), "Cancelling macro selection did not restore the Workspace owner");
            AssertIdle("Macro-selection cancellation");
            VerifyOriginal("preparation-choice-cancelled");
        });
        Step("preparation-choice-recovery", () =>
        {
            var invocation = Task.Run(() => InvokeButton(workspace!, "Run"));
            var dialog = WaitWindow(process, "Lua Workspace macro", TimeSpan.FromSeconds(10));
            var choice = dialog.FindAll(TreeScope.Descendants,
                    new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ListItem)).Cast<AutomationElement>()
                .Single(item => !item.Current.IsOffscreen && item.Current.Name == "Preparation Choice Alpha");
            Ensure(choice.TryGetCurrentPattern(SelectionItemPattern.Pattern, out var selection),
                "Preparation recovery choice has no SelectionItemPattern");
            ((SelectionItemPattern)selection).Select();
            var okay = UiaDriver.FindDescendantByAutomationId(dialog, "5100", ControlType.Button)
                ?? throw new InvalidOperationException("Preparation macro-choice dialog has no observed wx OK ID 5100");
            UiaDriver.Invoke(okay);
            WaitForInvocation(workspace!, process, invocation, "completed", artifacts, "preparation-choice-recovery");
            AssertIdle("Macro-selection recovery");
            VerifyOriginal("preparation-choice-recovery");
        });
        Step("preparation-normal-shutdown", () =>
        {
            Native.StabilizeOwnedClipboard(process.Id);
            Ensure(gui.TryGetCurrentPattern(WindowPattern.Pattern, out var pattern), "Main frame lacks WindowPattern.Close");
            ((WindowPattern)pattern).Close();
            Ensure(process.WaitForExit(TimeSpan.FromSeconds(10)) && process.ExitCode == 0,
                "Preparation host did not exit normally with code zero");
            File.WriteAllText(Path.Combine(artifacts, "preparation-normal-shutdown.json"), JsonSerializer.Serialize(new
            {
                HostExitCode = process.ExitCode, Method = "WindowPattern.Close"
            }, new JsonSerializerOptions { WriteIndented = true }));
        });
    }

    string[] DapSteps() => ["host-ready", "dap-enable-service", "dap-open-workspace", "dap-edit-unapplied",
        "dap-remote-attach", "dap-remote-reject", "dap-remote-disconnect", "dap-local-entry",
        "dap-local-disconnect", "dap-local-terminate", "dap-paused-open-reject", "dap-paused-run-reject",
        "dap-paused-save-reject", "dap-local-stop",
        "dap-loop-seed", "dap-loop-stop-rollback", "dap-loop-undo", "dap-disable-service", "dap-normal-shutdown"];

    int PrepareDap()
    {
        var listener = new System.Net.Sockets.TcpListener(System.Net.IPAddress.Loopback, 0);
        listener.Start();
        var port = ((System.Net.IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        var user = Path.Combine(artifacts, "profile", "user");
        Directory.CreateDirectory(user);
        var configPath = Path.Combine(user, "config.json");
        File.WriteAllText(configPath, JsonSerializer.Serialize(new
        {
            Automation = new { Debug = new Dictionary<string, object>
            {
                ["Listen Port"] = port, ["Require Token"] = true, ["Token"] = dapToken
            } }
        }), new UTF8Encoding(false));
        hashes["dap_config"] = Hash(configPath);
        hashes["dap_client_source"] = Hash(Path.Combine("tests", "gui-automation", "driver", "AutomationDebugClient.cs"));
        hashes["dap_command_source"] = Hash(Path.Combine("tests", "gui-automation", "driver", "ObservedWindowsCommand.cs"));
        return port;
    }

    void RunDap()
    {
        var gui = main ?? throw new InvalidOperationException("Main GUI frame is unavailable");
        var process = host ?? throw new InvalidOperationException("GUI host is unavailable");
        AutomationElement? editor = null;
        AutomationDebugClient? remote = null;
        string? originalEditorSource = null;
        Task? localInvocation = null;
        PauseStatus? localPause = null;
        List<string>? pausedStack = null;
        string? pausedSource = null;
        string? pausedMainTitle = null;
        string[]? pausedWindows = null;
        const string edited = "decorate = function(value)\n  return \"DAP:\" .. string.upper(value)\nend";
        var seeded = baseline.ToArray();
        seeded[0] = seeded[0] with { Effect = "debug-seed" };
        try
        {
            Step("dap-enable-service", () =>
            {
                if (!gui.TryGetCurrentPattern(WindowPattern.Pattern, out var pattern))
                    throw new InvalidOperationException("DAP main window lacks WindowPattern for full-width status evidence");
                ((WindowPattern)pattern).SetWindowVisualState(WindowVisualState.Maximized);
                WaitUntil(() => ((WindowPattern)pattern).Current.WindowVisualState == WindowVisualState.Maximized,
                    TimeSpan.FromSeconds(5), "DAP main window did not maximize for full rejection status evidence");
                DapToggleService(true);
            });
            Step("dap-open-workspace", () =>
            {
                InvokeMenu(gui, process, "Workspace Debug Select First Code", TimeSpan.FromSeconds(8));
                InvokeMenu(gui, process, "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(8));
                workspace = WaitWindow(process, "Lua Workspace", TimeSpan.FromSeconds(8));
                SetPauseOnEntry(workspace, true);
                editor = FindStyledText(workspace, insideNotebook: false)
                    ?? throw new InvalidOperationException("Workspace source editor is unavailable");
                originalEditorSource = Normalize(CopySource(workspace, process));
                File.WriteAllText(Path.Combine(artifacts, "dap-original-editor.lua"), originalEditorSource, new UTF8Encoding(false));
                SaveEvidence(workspace, artifacts, "dap-workspace-opened");
            });
            Step("dap-edit-unapplied", () =>
            {
                Native.ReplaceWithClipboard(editor!, process, edited);
                Ensure(Normalize(CopySource(workspace!, process)) == edited, "DAP source edit did not remain in the Workspace buffer");
                Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), "Un-applied DAP source changed physical ASS");
                File.WriteAllText(Path.Combine(artifacts, "dap-edited.lua"), edited, new UTF8Encoding(false));
            });
            Step("dap-remote-attach", () =>
            {
                remote = new AutomationDebugClient(dapPort, Path.Combine(artifacts, "dap-remote.ndjson"), TimeSpan.FromSeconds(5));
                DapAssertResponse(remote.Request("initialize", new { adapterID = "aegisub" }, TimeSpan.FromSeconds(5)), true, "initialize");
                DapAssertResponse(remote.Request("attach", new { token = dapToken }, TimeSpan.FromSeconds(5)), true, "attach");
                Ensure(remote.WaitEvent("initialized", TimeSpan.FromSeconds(5)).GetProperty("event").GetString() == "initialized",
                    "DAP attach did not publish initialized");
                DapAssertResponse(remote.Request("configurationDone", null, TimeSpan.FromSeconds(5)), true, "configurationDone");
            });
            Step("dap-remote-reject", () =>
            {
                InvokeButton(workspace!, "Debug");
                WaitUntil(() => HasWorkspaceStaticText(workspace!,
                        text => text.Contains("remote automation debug client", StringComparison.OrdinalIgnoreCase)),
                    TimeSpan.FromSeconds(5), "Workspace Debug did not reject the connected remote owner");
                Ensure(FindButton(workspace!, "Debug").Current.IsEnabled && gui.Current.IsEnabled,
                    "Rejected local Debug left a live invocation or locked the main frame");
                Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), "Remote ownership rejection changed physical ASS");
                SaveEvidence(workspace!, artifacts, "dap-remote-rejected");
            });
            Step("dap-remote-disconnect", () =>
            {
                DapAssertResponse(remote!.Request("disconnect", null, TimeSpan.FromSeconds(5)), true, "disconnect");
                DapWaitClosed(remote);
                remote.Dispose();
                remote = null;
            });
            Step("dap-local-entry", () =>
            {
                Native.Focus(editor!, process);
                Native.SendCtrlHome(editor!, process);
                Native.SendKey(editor!, process, 0x28);
                InvokeButton(workspace!, "Toggle Breakpoint");
                localInvocation = Task.Run(() => InvokeButton(workspace!, "Debug"));
                WaitUntil(() => ParsePauseStatus(ReadRunStatus(workspace!)) is
                    { Reason: "entry", Sequence: 1 } pause
                    && pause.SourcePath.Equals(Path.Combine(artifacts, "kara-templater.lua").Replace('\\', '/'), StringComparison.OrdinalIgnoreCase),
                    TimeSpan.FromSeconds(15), "Local Debug did not reach entry while the remote listener remained enabled");
                AssertLivePauseControls(gui, workspace!);
                SaveEvidence(workspace!, artifacts, "dap-local-entry");
                InvokeButton(workspace!, "Continue");
                WaitUntil(() => (localPause = ParsePauseStatus(ReadRunStatus(workspace!))) is
                    { Reason: "breakpoint", Sequence: 2, Line: 2 } pause
                    && pause.SourcePath.StartsWith("aegisub-workspace://", StringComparison.Ordinal),
                    TimeSpan.FromSeconds(15), "Local Debug did not reach the exact captured coding-source breakpoint");
                AssertLivePauseControls(gui, workspace!);
                SaveEvidence(workspace!, artifacts, "dap-local-breakpoint");
            });
            Step("dap-local-disconnect", () => DapProbeLocalOwner("disconnect", localPause!));
            Step("dap-local-terminate", () => DapProbeLocalOwner("terminate", localPause!));
            Step("dap-paused-open-reject", () =>
            {
                pausedMainTitle = gui.Current.Name;
                pausedWindows = DapWindowNames();
                pausedSource = DapExecutionSource();
                pausedStack = StackItems(workspace!);
                Ensure(StackHasSourceLine(workspace!, "Code line ID ", localPause!.Line),
                    "Initial DAP pause stack lost the captured source breakpoint");
                var mainHwnd = new nint(gui.Current.NativeWindowHandle);
                var commandId = ObservedWindowsCommand.FindMenuCommand(mainHwnd, process.Id, "File", "Open Subtitles...");
                ObservedWindowsCommand.PostMenuCommand(mainHwnd, process.Id, commandId);
                const string rejection = "A Lua Workspace invocation is active. Use its Stop or Detach controls.";
                WaitUntil(() => UiaDriver.FindDescendantByAutomationId(gui, "StatusBar.Pane1", ControlType.Edit)?.Current.Name == rejection,
                    TimeSpan.FromSeconds(5),
                    "Observed subtitle/open command did not reach the production active-invocation rejection");
                File.WriteAllText(Path.Combine(artifacts, "dap-paused-open-command.json"), JsonSerializer.Serialize(new
                {
                    MainHwnd = mainHwnd.ToInt64(), CommandId = commandId, Rejection = rejection
                }));
                DapAssertPausedUnchanged("subtitle/open");
                SaveEvidence(gui, artifacts, "dap-paused-open-main");
                SaveEvidence(workspace!, artifacts, "dap-paused-open-workspace");
            });
            Step("dap-paused-run-reject", () =>
            {
                var run = FindButton(workspace!, "Run");
                Ensure(run.Current.ProcessId == process.Id && run.Current.NativeWindowHandle != 0,
                    "Exact disabled Workspace Run button has no observed host HWND");
                var buttonHwnd = new nint(run.Current.NativeWindowHandle);
                var (parentHwnd, commandId) = ObservedWindowsCommand.PostButtonClick(buttonHwnd, process.Id);
                const string rejection = "A Workspace invocation is already active or no source is open";
                WaitUntil(() => HasWorkspaceStaticText(workspace!, text => text == rejection), TimeSpan.FromSeconds(5),
                    "Observed Run BN_CLICKED did not reach the production StartRun rejection");
                File.WriteAllText(Path.Combine(artifacts, "dap-paused-run-command.json"), JsonSerializer.Serialize(new
                {
                    ButtonHwnd = buttonHwnd.ToInt64(), ParentHwnd = parentHwnd.ToInt64(), CommandId = commandId,
                    Rejection = rejection
                }));
                DapAssertPausedUnchanged("second Run");
                SaveEvidence(workspace!, artifacts, "dap-paused-run-workspace");
            });
            Step("dap-paused-save-reject", () =>
            {
                Native.SaveShortcut(editor!, process);
                WaitUntil(() => HasWorkspaceStaticText(workspace!, text => text.Contains("Apply/Save is unavailable", StringComparison.Ordinal)),
                    TimeSpan.FromSeconds(5), "Ctrl+S did not explicitly reject Apply during the local pause");
                Ensure(Normalize(CopySource(workspace!, process)) == edited, "Rejected Ctrl+S changed the dirty Workspace buffer");
                Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), "Rejected Ctrl+S changed physical ASS");
                AssertLivePauseControls(gui, workspace!);
                SelectTab(workspace!, "Execution Source");
                var execution = FindStyledText(workspace!, insideNotebook: true)
                    ?? throw new InvalidOperationException("Paused readonly execution source is unavailable");
                Ensure(Normalize(Native.CopyStyledText(execution, process)) == edited,
                    "Rejected Ctrl+S lost the captured readonly source revision");
                SaveEvidence(workspace!, artifacts, "dap-paused-save-rejected");
            });
            Step("dap-local-stop", () =>
            {
                InvokeButton(workspace!, "Continue");
                WaitUntil(() => ParsePauseStatus(ReadRunStatus(workspace!)) is { Reason: "breakpoint" } pause
                    && pause.Sequence > localPause!.Sequence && pause.SourcePath == localPause.SourcePath && pause.Line == 2,
                    TimeSpan.FromSeconds(15), "Local Continue did not preserve the line 2 breakpoint after DAP rejection");
                InvokeButton(workspace!, "Stop");
                Ensure(localInvocation!.Wait(TimeSpan.FromSeconds(20)), "Local Stop did not release the GUI invocation");
                WaitUntil(() => ReadRunStatus(workspace!).Contains("cancelled", StringComparison.OrdinalIgnoreCase)
                    && gui.Current.IsEnabled, TimeSpan.FromSeconds(8), "Local Stop did not reach a cancelled terminal state");
                Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), "Local Stop changed the physical ASS baseline");
                SaveEvidence(workspace!, artifacts, "dap-local-stopped");
            });
            Step("dap-loop-seed", () =>
            {
                Native.Undo(editor!, process);
                Ensure(Normalize(CopySource(workspace!, process)) == originalEditorSource,
                    "Workspace Undo did not restore the original code before switching to the loop fixture");
                InvokeMenu(gui, process, "Workspace Debug Seed", TimeSpan.FromSeconds(8));
                SaveMainAss(gui, input, seeded, artifacts, "dap-seeded.ass");
                OpenLuaFile(workspace!, process, Path.Combine(artifacts, "debug-loop.lua"), artifacts, "dap-loop-picker");
            });
            Step("dap-loop-stop-rollback", () =>
            {
                var invocation = Task.Run(() => InvokeButton(workspace!, "Debug"));
                WaitPauseAt(workspace!, "entry", 1, "debug-loop.lua", 2, TimeSpan.FromSeconds(15));
                InvokeButton(workspace!, "Continue");
                WaitLoopStarted(workspace!, process, TimeSpan.FromSeconds(5));
                InvokeButton(workspace!, "Stop");
                Ensure(invocation.Wait(TimeSpan.FromSeconds(12)), "Loop Stop did not terminate the DAP-on invocation");
                WaitUntil(() => ReadRunStatus(workspace!).Contains("cancelled", StringComparison.OrdinalIgnoreCase),
                    TimeSpan.FromSeconds(5), "Loop Stop did not report cancelled");
                Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(seeded), "Loop Stop altered the physical seed baseline");
                VerifySeed(gui, process, input, seeded, artifacts, "dap-loop-live-rollback");
            });
            Step("dap-loop-undo", () =>
            {
                InvokeMenu(gui, process, "Undo", TimeSpan.FromSeconds(8), "Edit");
                SaveMainAss(gui, input, baseline, artifacts, "dap-loop-undone.ass");
            });
            Step("dap-disable-service", () => DapToggleService(false));
            Step("dap-normal-shutdown", () =>
            {
                if (!gui.TryGetCurrentPattern(WindowPattern.Pattern, out var pattern))
                    throw new InvalidOperationException("Main frame lacks real WindowPattern.Close");
                Native.StabilizeOwnedClipboard(process.Id);
                ((WindowPattern)pattern).Close();
                Ensure(process.WaitForExit(TimeSpan.FromSeconds(10)) && process.ExitCode == 0,
                    "DAP GUI host did not exit normally with code zero");
                File.WriteAllText(Path.Combine(artifacts, "dap-normal-shutdown.json"), JsonSerializer.Serialize(new
                {
                    HostExitCode = process.ExitCode, Method = "WindowPattern.Close"
                }));
            });
        }
        finally { remote?.Dispose(); }

        void DapToggleService(bool enable)
        {
            InvokeMenu(gui, process, "Automation...", TimeSpan.FromSeconds(8));
            var manager = WaitWindow(process, "Automation Manager", TimeSpan.FromSeconds(8));
            InvokeButton(manager, enable ? "Enable Debug Mode" : "Disable Debug Mode");
            WaitUntil(() => UiaDriver.FindEnabledInvokableButton(manager, enable ? "Disable Debug Mode" : "Enable Debug Mode") is not null,
                TimeSpan.FromSeconds(8), "Automation Manager did not reflect the requested debug mode");
            SaveEvidence(manager, artifacts, enable ? "dap-service-enabled" : "dap-service-disabled");
            InvokeButton(manager, "Close");
            WaitUntil(() => FindWindow(process, "Automation Manager") is null, TimeSpan.FromSeconds(5),
                "Automation Manager did not close after debug-mode change");
        }

        string[] DapWindowNames() => AutomationElement.RootElement.FindAll(TreeScope.Children,
            new PropertyCondition(AutomationElement.ProcessIdProperty, process.Id)).Cast<AutomationElement>()
            .Where(item => item.Current.ControlType == ControlType.Window && !item.Current.IsOffscreen)
            .Select(item => item.Current.Name).OrderBy(name => name, StringComparer.Ordinal).ToArray();

        string DapExecutionSource()
        {
            SelectTab(workspace!, "Execution Source");
            var execution = FindStyledText(workspace!, insideNotebook: true)
                ?? throw new InvalidOperationException("Paused execution source is unavailable");
            var source = Normalize(Native.CopyStyledText(execution, process));
            SelectTab(workspace!, "Stack and Variables");
            return source;
        }

        void DapAssertPausedUnchanged(string action)
        {
            Ensure(gui.Current.Name == pausedMainTitle, action + " changed the active subtitle title");
            Ensure(DapWindowNames().SequenceEqual(pausedWindows!), action + " opened a new GUI window or file picker");
            Ensure(DapExecutionSource() == pausedSource, action + " changed the exact paused execution source");
            Ensure(StackItems(workspace!).SequenceEqual(pausedStack!), action + " changed the paused stack frames");
            Ensure(StackHasSourceLine(workspace!, "Code line ID ", localPause!.Line),
                action + " lost the captured coding-source breakpoint");
            Ensure(Normalize(CopySource(workspace!, process)) == edited,
                action + " changed the unapplied Workspace source");
            Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), action + " changed the physical ASS");
            AssertLivePauseControls(gui, workspace!);
        }

        void DapProbeLocalOwner(string finalCommand, PauseStatus expectedPause)
        {
            using var client = new AutomationDebugClient(dapPort, Path.Combine(artifacts, "dap-local-" + finalCommand + ".ndjson"), TimeSpan.FromSeconds(5));
            DapAssertResponse(client.Request("initialize", new { adapterID = "aegisub" }, TimeSpan.FromSeconds(5)), true, "initialize");
            DapAssertResponse(client.Request("attach", new { token = dapToken }, TimeSpan.FromSeconds(5)), false, "attach");
            DapAssertResponse(client.Request("continue", new { threadId = 1 }, TimeSpan.FromSeconds(5)), false, "continue");
            DapAssertResponse(client.Request("setBreakpoints", new { source = new { path = expectedPause.SourcePath },
                breakpoints = new[] { new { line = expectedPause.Line + 1 } } }, TimeSpan.FromSeconds(5)), false, "setBreakpoints");
            DapAssertResponse(client.Request(finalCommand, null, TimeSpan.FromSeconds(5)), true, finalCommand);
            DapWaitClosed(client);
            Ensure(ParsePauseStatus(ReadRunStatus(workspace!)) == expectedPause,
                "DAP " + finalCommand + " changed the local pause sequence or exact source location");
            AssertLivePauseControls(gui, workspace!);
            SaveEvidence(workspace!, artifacts, "dap-local-" + finalCommand + "-retained");
        }
    }

    void DapAssertResponse(JsonElement response, bool success, string command)
    {
        Ensure(response.GetProperty("type").GetString() == "response"
            && response.GetProperty("command").GetString() == command
            && response.GetProperty("success").GetBoolean() == success,
            "DAP " + command + " response did not report the expected success state");
        if (!success)
            Ensure(response.GetProperty("message").GetString()?.Contains("local automation debug session", StringComparison.OrdinalIgnoreCase) == true,
                "DAP " + command + " rejection did not identify the local owner");
    }

    void DapWaitClosed(AutomationDebugClient client)
    {
        try
        {
            client.WaitEvent("__dap_connection_closed__", TimeSpan.FromSeconds(5));
            throw new InvalidOperationException("DAP connection remained open after disconnect/terminate");
        }
        catch (EndOfStreamException) { }
        catch (IOException error) when (error.InnerException is System.Net.Sockets.SocketException socket
            && socket.SocketErrorCode is System.Net.Sockets.SocketError.ConnectionReset or System.Net.Sockets.SocketError.ConnectionAborted) { }
    }

    string[] JitSteps()
    {
        var names = new List<string> { "host-ready", "jit-open", "jit-initial-prewarm" };
        foreach (var mode in new[] { "on", "off" })
            foreach (var outcome in new[] { "completed", "failed", "cancelled" })
                foreach (var stage in new[] { "source", "run", "restored", "ass" })
                    names.Add($"jit-{mode}-{outcome}-{stage}");
        names.Add("jit-normal-shutdown");
        return names.ToArray();
    }

    void PrepareJit()
    {
        var source = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", "debug-jit.lua");
        var destination = Path.Combine(artifacts, "debug-jit.lua");
        File.Copy(source, destination);
        hashes["jit-fixture"] = Hash(source);
        hashes["jit-managed-initial"] = Hash(destination);
        const string scripts = "Automation Scripts: ~debug-actions.lua|~kara-templater.lua";
        var text = File.ReadAllText(input);
        Ensure(text.Split(scripts, StringSplitOptions.None).Length == 2, "JIT input lacks its unique managed Automation Scripts declaration");
        File.WriteAllText(input, text.Replace(scripts, scripts + "|~debug-jit.lua", StringComparison.Ordinal), new UTF8Encoding(false));
        Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), "Attaching the JIT fixture changed the seven-event ASS baseline");
        hashes["jit-input-initial"] = Hash(input);
    }

    void RunJit()
    {
        var gui = main ?? throw new InvalidOperationException("JIT main frame is unavailable");
        var process = host ?? throw new InvalidOperationException("JIT host process is unavailable");
        var managedFile = Path.Combine(artifacts, "debug-jit.lua");
        var template = Normalize(File.ReadAllText(managedFile));
        const string modeMarker = "local original_mode = \"on\"";
        const string outcomeMarker = "local outcome = \"completed\"";
        Ensure(template.Split(modeMarker, StringSplitOptions.None).Length == 2
            && template.Split(outcomeMarker, StringSplitOptions.None).Length == 2, "JIT source mode/outcome markers are not unique");
        var lines = template.Split('\n');
        int JitLine(string statement)
        {
            var matches = lines.Select((line, index) => (line, index)).Where(item => item.line.Trim() == statement).ToArray();
            Ensure(matches.Length == 1, "JIT source statement is not unique: " + statement);
            return matches[0].index + 1;
        }
        var entryLine = JitLine("local workspace_mode = jit.status()");
        var loopLines = new[] { JitLine("for index = 1, limit do"), JitLine("total = total + index % 97") };
        Step("jit-open", () =>
        {
            InvokeMenu(gui, process, "Workspace Debug Select First Code", TimeSpan.FromSeconds(8));
            InvokeMenu(gui, process, "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(8));
            workspace = WaitWindow(process, "Lua Workspace", TimeSpan.FromSeconds(8));
            SetPauseOnEntry(workspace, true);
            OpenLuaFile(workspace, process, managedFile, artifacts, "jit-file-picker");
            Ensure(workspace.Current.Name.Contains("debug-jit.lua", StringComparison.Ordinal), "Workspace did not open the managed JIT file");
        });
        Step("jit-initial-prewarm", () =>
            JitAcknowledgeMenu(gui, process, "Workspace JIT Initial Prewarm", "Initial JIT trace verified", "jit-initial-prewarm"));
        foreach (var mode in new[] { "on", "off" })
        {
            foreach (var outcome in new[] { "completed", "failed", "cancelled" })
            {
                var name = $"jit-{mode}-{outcome}";
                var original = mode == "on" ? "true" : "false";
                var source = template.Replace(modeMarker, $"local original_mode = \"{mode}\"", StringComparison.Ordinal)
                    .Replace(outcomeMarker, $"local outcome = \"{outcome}\"", StringComparison.Ordinal);
                var casePath = Path.Combine(artifacts, name + ".json");
                var evidence = new Dictionary<string, object?>
                {
                    ["mode"] = mode, ["expected_outcome"] = outcome, ["fixture"] = "debug-jit.lua",
                    ["entry_line"] = entryLine, ["manual_pause_lines"] = loopLines, ["passed"] = false
                };
                void JitWriteCase() => File.WriteAllText(casePath, JsonSerializer.Serialize(evidence, new JsonSerializerOptions { WriteIndented = true }));
                Step(name + "-source", () =>
                {
                    InvokeMenu(gui, process, "Workspace Debug Select First Code", TimeSpan.FromSeconds(8));
                    var sourcePath = Path.Combine(artifacts, name + ".lua");
                    File.WriteAllText(sourcePath, source, new UTF8Encoding(false));
                    var editor = FindStyledText(workspace!, insideNotebook: false)
                        ?? throw new InvalidOperationException("JIT source editor was not found");
                    Native.ReplaceWithClipboard(editor, process, source);
                    Ensure(Normalize(CopySource(workspace!, process)) == source, "JIT source edit differs from the exact case revision");
                    InvokeButton(workspace!, "Save");
                    WaitUntil(() => Normalize(File.ReadAllText(managedFile)) == source, TimeSpan.FromSeconds(5),
                        "Workspace Save did not persist the exact JIT case source");
                    hashes[name + "-source"] = Hash(sourcePath);
                    hashes[name + "-managed"] = Hash(managedFile);
                    evidence["source_sha256"] = hashes[name + "-source"];
                    evidence["managed_sha256"] = hashes[name + "-managed"];
                    Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), "Saving JIT source modified physical ASS events");
                    JitWriteCase();
                });
                Step(name + "-run", () =>
                {
                    var invocation = Task.Run(() => InvokeButton(workspace!, outcome == "cancelled" ? "Debug" : "Run"));
                    JitChooseExercise(process, name);
                    if (outcome == "cancelled")
                    {
                        WaitPauseAt(workspace!, "entry", 1, "debug-jit.lua", entryLine, TimeSpan.FromSeconds(12));
                        Ensure(ParsePauseStatus(ReadRunStatus(workspace!))!.SourcePath.Equals(managedFile.Replace('\\', '/'), StringComparison.OrdinalIgnoreCase),
                            "JIT entry pause resolved a different managed file");
                        AssertLivePauseControls(gui, workspace!);
                        evidence["entry_pause"] = ParsePauseStatus(ReadRunStatus(workspace!));
                        SaveEvidence(workspace!, artifacts, name + "-entry");
                        InvokeButton(workspace!, "Continue");
                        WaitUntil(() => ReadRunStatus(workspace!).StartsWith("Debug: running", StringComparison.Ordinal)
                            && ReadRunLog(workspace!, process).Contains($"jit-loop-started|{mode}|cancelled", StringComparison.Ordinal),
                            TimeSpan.FromSeconds(8), "JIT cancellation loop did not report its unique post-write handshake");
                        InvokeButton(workspace!, "Pause");
                        PauseStatus? pause = null;
                        WaitUntil(() => (pause = ParsePauseStatus(ReadRunStatus(workspace!))) is { Reason: "pause", Sequence: 2 }
                            && pause.SourcePath.Equals(managedFile.Replace('\\', '/'), StringComparison.OrdinalIgnoreCase)
                            && loopLines.Contains(pause.Line), TimeSpan.FromSeconds(8),
                            "Manual Pause did not interrupt the previously traced hot_loop at an exact loop source line");
                        AssertLivePauseControls(gui, workspace!);
                        SelectTab(workspace!, "Stack and Variables");
                        var stack = StackItems(workspace!);
                        Ensure(stack[0] == $"hot_loop - debug-jit.lua:{pause!.Line}", "Manual Pause top stack frame is not the target hot_loop source");
                        evidence["manual_pause"] = pause;
                        evidence["paused_stack"] = stack;
                        SaveEvidence(workspace!, artifacts, name + "-manual-pause");
                        InvokeButton(workspace!, "Continue");
                        WaitUntil(() => ReadRunStatus(workspace!).StartsWith("Debug: running", StringComparison.Ordinal), TimeSpan.FromSeconds(5),
                            "JIT Continue did not resume the paused hot_loop before Stop");
                        InvokeButton(workspace!, "Stop");
                    }
                    var enter = $"jit-enter|{mode}|{outcome}|original={original}|workspace=false|prewarmed=false|trace=";
                    WaitUntil(() => invocation.IsCompleted && gui.Current.IsEnabled
                        && ReadRunStatus(workspace!).Contains(outcome, StringComparison.OrdinalIgnoreCase)
                        && ReadRunLog(workspace!, process).Contains(enter, StringComparison.Ordinal), TimeSpan.FromSeconds(15),
                        "JIT invocation did not reach its fresh expected terminal state with scoped engine evidence");
                    Ensure(invocation.Wait(TimeSpan.FromSeconds(5)), "JIT UIA invocation did not return after termination");
                    var log = ReadRunLog(workspace!, process);
                    var traceMatch = Regex.Match(log, Regex.Escape(enter) + @"(\d+)(?:\r?\n|$)", RegexOptions.CultureInvariant);
                    Ensure(traceMatch.Success, "JIT log lacks the exact original/workspace/guarded-preparation observation");
                    var trace = int.Parse(traceMatch.Groups[1].Value);
                    Ensure(trace == 0, "Workspace preparation retained a JIT trace before Macro Run");
                    if (outcome == "failed")
                        Ensure(log.Contains($"workspace-jit-{mode}-intentional-failure", StringComparison.Ordinal), "JIT failed for a reason other than its deliberate error");
                    Ensure(Hash(managedFile) == hashes[name + "-managed"], "JIT managed source changed between Save and execution");
                    Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(baseline), "JIT invocation wrote the physical ASS before main Save");
                    evidence["terminal"] = ReadRunStatus(workspace!);
                    evidence["prepared_trace"] = trace;
                    evidence["prepared_without_trace"] = true;
                    evidence["workspace_engine"] = false;
                    File.WriteAllText(Path.Combine(artifacts, name + "-run.log"), log);
                    SaveEvidence(workspace!, artifacts, name + "-terminal");
                    JitWriteCase();
                });
                Step(name + "-restored", () =>
                {
                    var label = $"JIT restored|{mode}|{outcome}|engine={original}|prepared_without_trace=true|workspace=false|runs=1";
                    JitAcknowledgeMenu(gui, process, "Workspace JIT Verify", label, name + "-restored");
                    Ensure(Hash(managedFile) == hashes[name + "-managed"], "JIT verifier changed its managed script source");
                    evidence["same_state_restoration_label"] = label;
                    evidence["restoration_entry"] = "Automation menu / Workspace JIT Verify";
                    JitWriteCase();
                });
                Step(name + "-ass", () =>
                {
                    if (outcome == "completed")
                    {
                        var committed = baseline.ToArray();
                        committed[0] = committed[0] with { Effect = $"jit-{mode}-completed-153" };
                        SaveMainAss(gui, input, committed, artifacts, name + "-committed.ass");
                        evidence["committed_events"] = committed;
                        InvokeMenu(gui, process, "Undo", TimeSpan.FromSeconds(8), "Edit");
                        SaveMainAss(gui, input, baseline, artifacts, name + "-undone.ass");
                    }
                    var bytes = File.ReadAllBytes(input);
                    JitAcknowledgeMenu(gui, process, "Workspace Debug Verify Original", "Debug original live baseline verified", name + "-live-baseline");
                    Ensure(File.ReadAllBytes(input).SequenceEqual(bytes) && EventLines(File.ReadAllText(input)).SequenceEqual(baseline),
                        "JIT independent live verifier changed or disagreed with the physical baseline");
                    AssertPhysicalEventLines(File.ReadAllText(input), 7);
                    File.WriteAllText(Path.Combine(artifacts, name + "-baseline.ass"), File.ReadAllText(input), new UTF8Encoding(false));
                    evidence["final_events"] = baseline;
                    evidence["live_baseline_verified"] = true;
                    evidence["passed"] = true;
                    JitWriteCase();
                });
            }
        }
        Step("jit-normal-shutdown", () =>
        {
            if (!gui.TryGetCurrentPattern(WindowPattern.Pattern, out var pattern))
                throw new InvalidOperationException("JIT main frame lacks WindowPattern.Close");
            Native.StabilizeOwnedClipboard(process.Id);
            ((WindowPattern)pattern).Close();
            Ensure(process.WaitForExit(TimeSpan.FromSeconds(10)) && process.ExitCode == 0, "JIT host did not exit normally with code zero");
            File.WriteAllText(Path.Combine(artifacts, "jit-normal-shutdown.json"), JsonSerializer.Serialize(new
            {
                HostExitCode = process.ExitCode, Method = "WindowPattern.Close"
            }, new JsonSerializerOptions { WriteIndented = true }));
        });
    }

    void JitChooseExercise(Process process, string evidence)
    {
        var dialog = WaitWindow(process, "Lua Workspace macro", TimeSpan.FromSeconds(8));
        var choices = dialog.FindAll(TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ListItem)).Cast<AutomationElement>()
            .Where(item => !item.Current.IsOffscreen).ToArray();
        Ensure(choices.Length == 3 && choices.Any(item => item.Current.Name == "Workspace JIT Initial Prewarm")
            && choices.Any(item => item.Current.Name == "Workspace JIT Exercise")
            && choices.Any(item => item.Current.Name == "Workspace JIT Verify"), "JIT Save/Reload macro selection does not expose the exact three fixture macros");
        var choice = choices.Single(item => item.Current.Name == "Workspace JIT Exercise");
        if (!choice.TryGetCurrentPattern(SelectionItemPattern.Pattern, out var select))
            throw new InvalidOperationException("JIT Exercise choice lacks SelectionItemPattern");
        ((SelectionItemPattern)select).Select();
        SaveEvidence(dialog, artifacts, evidence + "-macro-choice");
        var okay = UiaDriver.FindDescendantByAutomationId(dialog, "5100", ControlType.Button)
            ?? throw new InvalidOperationException("JIT macro choice has no observed wx OK ID 5100");
        Ensure(okay.Current.IsEnabled && okay.TryGetCurrentPattern(InvokePattern.Pattern, out _), "JIT macro choice OK is not invokable");
        UiaDriver.Invoke(okay);
        WaitUntil(() => FindWindow(process, "Lua Workspace macro") is null, TimeSpan.FromSeconds(5), "JIT macro choice did not close after exact selection");
    }

    void JitAcknowledgeMenu(AutomationElement gui, Process process, string macro, string label, string evidence)
    {
        var commandReady = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var invocation = Task.Run(() => InvokeMenu(gui, process, macro, TimeSpan.FromSeconds(8),
            commandReady: () => commandReady.TrySetResult()));
        WaitUntil(() => commandReady.Task.IsCompleted || invocation.IsCompleted, TimeSpan.FromSeconds(12),
            $"JIT verifier {macro} did not become invokable");
        if (!commandReady.Task.IsCompleted) invocation.GetAwaiter().GetResult();
        var dialog = WaitWindowContainingText(process, label, TimeSpan.FromSeconds(8));
        SaveEvidence(dialog, artifacts, evidence + "-dialog");
        InvokeButton(dialog, "OK");
        Ensure(invocation.Wait(TimeSpan.FromSeconds(10)), "JIT ordinary-menu verifier did not return after exact acknowledgement");
        WaitUntil(() => gui.Current.IsEnabled && FindProgressPane(process, macro) is null, TimeSpan.FromSeconds(8),
            "JIT ordinary-menu verifier did not release its execution UI");
    }

    void RunControls()
    {
        var gui = main ?? throw new InvalidOperationException("Main GUI frame is unavailable");
        var process = host ?? throw new InvalidOperationException("GUI host is unavailable");
        var stepped = baseline.ToArray();
        stepped[0] = stepped[0] with { Effect = "step:8" };
        var seeded = baseline.ToArray();
        seeded[0] = seeded[0] with { Effect = "debug-seed" };
        var completed = seeded.ToArray();
        completed[0] = completed[0] with { Effect = "loop-complete" };
        var stepsArtifact = Path.Combine(artifacts, "debug-steps.lua");
        var loopArtifact = Path.Combine(artifacts, "debug-loop.lua");
        var infiniteLoopArtifact = Path.Combine(artifacts, "debug-infinite-loop.lua");

        Step("controls-open-steps", () =>
        {
            InvokeMenu(gui, process, "Workspace Debug Select First Code", TimeSpan.FromSeconds(8));
            InvokeMenu(gui, process, "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(8));
            workspace = WaitWindow(process, "Lua Workspace", TimeSpan.FromSeconds(8));
            SetPauseOnEntry(workspace, true);
            SaveEvidence(workspace, artifacts, "controls-workspace-opened");
            OpenLuaFile(workspace, process, stepsArtifact, artifacts, "steps-file-picker");
            Ensure(workspace.Current.Name.Contains("debug-steps.lua", StringComparison.Ordinal), "Workspace did not bind the selected steps Lua file");
        });
        Step("step-in-out-over", () =>
        {
            var invocation = Task.Run(() => InvokeButton(workspace!, "Debug"));
            WaitPauseAt(workspace!, "entry", 1, "debug-steps.lua", 10, TimeSpan.FromSeconds(15));
            AssertLivePauseControls(gui, workspace!);
            SelectTab(workspace!, "Stack and Variables");
            var callbackDepth = StackItems(workspace!).Count;
            SaveEvidence(workspace!, artifacts, "steps-entry");

            InvokeButton(workspace!, "Step In");
            WaitPauseAt(workspace!, "step", 2, "debug-steps.lua", 6, TimeSpan.FromSeconds(8));
            var outerDepth = StackItems(workspace!).Count;
            Ensure(outerDepth > callbackDepth && StackHasSourceLine(workspace!, "debug-steps.lua", 6), "Step In did not enter outer at line 6");
            SaveEvidence(workspace!, artifacts, "steps-outer");

            InvokeButton(workspace!, "Step In");
            WaitPauseAt(workspace!, "step", 3, "debug-steps.lua", 2, TimeSpan.FromSeconds(8));
            var innerDepth = StackItems(workspace!).Count;
            Ensure(innerDepth > outerDepth && StackHasSourceLine(workspace!, "debug-steps.lua", 2), "Second Step In did not enter inner at line 2");
            SaveEvidence(workspace!, artifacts, "steps-inner");

            InvokeButton(workspace!, "Step Out");
            WaitPauseAt(workspace!, "step", 4, "debug-steps.lua", 7, TimeSpan.FromSeconds(8));
            Ensure(StackItems(workspace!).Count == outerDepth, "Step Out did not return to outer's stack depth");
            SaveEvidence(workspace!, artifacts, "steps-out");

            InvokeButton(workspace!, "Step Over");
            WaitPauseAt(workspace!, "step", 5, "debug-steps.lua", 11, TimeSpan.FromSeconds(8));
            Ensure(StackItems(workspace!).Count == callbackDepth, "Step Over did not return to the macro callback depth");
            SelectTab(workspace!, "Execution Source");
            var execution = FindStyledText(workspace!, insideNotebook: true)
                ?? throw new InvalidOperationException("Steps execution source is unavailable");
            Ensure(Normalize(Native.CopyStyledText(execution, process)) == Normalize(File.ReadAllText(stepsArtifact)),
                "Step source did not come from the exact saved Lua file revision");
            SaveEvidence(workspace!, artifacts, "steps-over");
            InvokeButton(workspace!, "Continue");
            WaitForInvocation(workspace!, process, invocation, "completed", artifacts, "steps-completed");
        });
        Step("step-save-undo", () =>
        {
            SaveMainAss(gui, input, stepped, artifacts, "steps-saved.ass");
            InvokeMenu(gui, process, "Undo", TimeSpan.FromSeconds(8), "Edit");
            SaveMainAss(gui, input, baseline, artifacts, "steps-undone.ass");
        });
        Step("loop-seed", () =>
        {
            InvokeMenu(gui, process, "Workspace Debug Seed", TimeSpan.FromSeconds(8));
            SaveMainAss(gui, input, seeded, artifacts, "loop-seed.ass");
            OpenLuaFile(workspace!, process, loopArtifact, artifacts, "loop-file-picker");
            Ensure(workspace!.Current.Name.Contains("debug-loop.lua", StringComparison.Ordinal), "Workspace did not bind the loop Lua file");
        });
        Step("loop-pause-stop", () =>
        {
            var invocation = Task.Run(() => InvokeButton(workspace!, "Debug"));
            WaitPauseAt(workspace!, "entry", 1, "debug-loop.lua", 2, TimeSpan.FromSeconds(15));
            AssertLivePauseControls(gui, workspace!);
            InvokeButton(workspace!, "Continue");
            WaitLoopStarted(workspace!, process, TimeSpan.FromSeconds(5));
            InvokeButton(workspace!, "Pause");
            WaitPauseAt(workspace!, "pause", 2, "debug-loop.lua", 8, TimeSpan.FromSeconds(8));
            SaveEvidence(workspace!, artifacts, "loop-manual-pause");
            InvokeButton(workspace!, "Continue");
            WaitUntil(() => ReadRunStatus(workspace!).StartsWith("Debug: running", StringComparison.Ordinal), TimeSpan.FromSeconds(5),
                "Continue did not resume the long loop before Stop");
            InvokeButton(workspace!, "Stop");
            Ensure(invocation.Wait(TimeSpan.FromSeconds(12)), "Long-loop Stop did not terminate its invocation");
            WaitUntil(() => ReadRunStatus(workspace!).Contains("cancelled", StringComparison.OrdinalIgnoreCase), TimeSpan.FromSeconds(5),
                "Long-loop Stop did not report cancelled");
            Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(seeded), "Stop altered the physically saved seed baseline");
            SaveEvidence(workspace!, artifacts, "loop-stopped");
        });
        Step("loop-live-rollback", () => VerifySeed(gui, process, input, seeded, artifacts, "loop-stop-verify"));
        Step("loop-undo-depth", () =>
        {
            InvokeMenu(gui, process, "Undo", TimeSpan.FromSeconds(8), "Edit");
            SaveMainAss(gui, input, baseline, artifacts, "loop-undo-depth.ass");
        });
        Step("close-stop", () =>
        {
            InvokeMenu(gui, process, "Workspace Debug Seed", TimeSpan.FromSeconds(8));
            SaveMainAss(gui, input, seeded, artifacts, "close-stop-seed.ass");
            var invocation = Task.Run(() => InvokeButton(workspace!, "Debug"));
            WaitPauseAt(workspace!, "entry", 1, "debug-loop.lua", 2, TimeSpan.FromSeconds(15));
            InvokeButton(workspace!, "Continue");
            WaitLoopStarted(workspace!, process, TimeSpan.FromSeconds(5));
            var statusHandle = CaptureRunStatusHandle(workspace!, process);
            Native.PostClose(workspace!, process);
            var decision = WaitWindow(process, "Active Lua Workspace invocation", TimeSpan.FromSeconds(5));
            SaveEvidence(decision, artifacts, "close-stop-choice");
            InvokeButton(decision, "Stop then close");
            Ensure(invocation.Wait(TimeSpan.FromSeconds(12)), "Close/Stop did not terminate the active invocation");
            WaitUntil(() => FindWindow(process, "Lua Workspace") is null && gui.Current.IsEnabled, TimeSpan.FromSeconds(6),
                "Close/Stop did not hide Workspace and re-enable the main frame");
            Ensure(Native.ReadStaticText(statusHandle, process).Contains("cancelled", StringComparison.OrdinalIgnoreCase),
                "Close/Stop did not retain a cancelled terminal status");
            File.WriteAllText(Path.Combine(artifacts, "close-stop-terminal.txt"), Native.ReadStaticText(statusHandle, process));
            Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(seeded), "Close/Stop altered physical seed baseline");
            TryProcessEvidence(process, artifacts, "close-stop-hidden");
        });
        Step("close-stop-rollback", () =>
        {
            VerifySeed(gui, process, input, seeded, artifacts, "close-stop-verify");
            InvokeMenu(gui, process, "Undo", TimeSpan.FromSeconds(8), "Edit");
            SaveMainAss(gui, input, baseline, artifacts, "close-stop-undone.ass");
        });
        Step("close-detach-stop", () =>
        {
            InvokeMenu(gui, process, "Workspace Debug Select First Code", TimeSpan.FromSeconds(8));
            InvokeMenu(gui, process, "Open Code Line in Lua Workspace", TimeSpan.FromSeconds(8));
            workspace = WaitWindow(process, "Lua Workspace", TimeSpan.FromSeconds(8));
            OpenLuaFile(workspace, process, infiniteLoopArtifact, artifacts, "detach-stop-loop-file-picker");
            InvokeMenu(gui, process, "Workspace Debug Seed", TimeSpan.FromSeconds(8));
            SaveMainAss(gui, input, seeded, artifacts, "close-detach-stop-seed.ass");
            var invocation = Task.Run(() => InvokeButton(workspace!, "Run"));
            WaitLoopStarted(workspace!, process, TimeSpan.FromSeconds(5));
            Ensure(FindButton(workspace!, "Detach").Current.IsEnabled,
                "A running non-debug invocation did not expose Detach before the close decision");
            Native.PostClose(workspace!, process);
            var decision = WaitWindow(process, "Active Lua Workspace invocation", TimeSpan.FromSeconds(5));
            SaveEvidence(decision, artifacts, "close-detach-stop-choice");
            InvokeButton(decision, "Detach and keep open");
            workspace = WaitWindow(process, "Lua Workspace", TimeSpan.FromSeconds(5));
            WaitUntil(() => ReadRunStatus(workspace).StartsWith("Detached;", StringComparison.Ordinal)
                && FindButton(workspace, "Stop").Current.IsEnabled, TimeSpan.FromSeconds(5),
                "Detach did not retain a visible enabled Stop control");
            var stopButton = FindButton(workspace, "Stop");
            var detachedStatus = ReadRunStatus(workspace);
            File.WriteAllText(Path.Combine(artifacts, "close-detach-stop-running.json"), JsonSerializer.Serialize(new
            {
                Status = detachedStatus, MainEnabled = gui.Current.IsEnabled, WorkspaceOffscreen = workspace.Current.IsOffscreen,
                StopEnabled = stopButton.Current.IsEnabled, StopOffscreen = stopButton.Current.IsOffscreen
            }, new JsonSerializerOptions { WriteIndented = true }));
            Ensure(!gui.Current.IsEnabled && !workspace.Current.IsOffscreen && stopButton.Current.IsEnabled && !stopButton.Current.IsOffscreen
                && detachedStatus.StartsWith("Detached", StringComparison.Ordinal)
                && !detachedStatus.Contains("completed", StringComparison.OrdinalIgnoreCase),
                "Detach did not leave the visible Workspace as the usable control surface");
            SaveEvidence(workspace, artifacts, "close-detach-stop-visible");
            InvokeButton(workspace, "Stop");
            Ensure(invocation.Wait(TimeSpan.FromSeconds(12)), "Detached nonterminating loop did not stop through the visible Stop control");
            WaitUntil(() => ReadRunStatus(workspace).Contains("cancelled", StringComparison.OrdinalIgnoreCase)
                && gui.Current.IsEnabled, TimeSpan.FromSeconds(5), "Detached Stop did not reach a cancelled terminal state");
            Ensure(EventLines(File.ReadAllText(input)).SequenceEqual(seeded), "Detached Stop altered the physical seed baseline");
            SaveEvidence(workspace, artifacts, "close-detach-stop-cancelled");
        });
        Step("close-detach-stop-rollback", () =>
        {
            VerifySeed(gui, process, input, seeded, artifacts, "close-detach-stop-verify");
            InvokeMenu(gui, process, "Undo", TimeSpan.FromSeconds(8), "Edit");
            SaveMainAss(gui, input, baseline, artifacts, "close-detach-stop-undone.ass");
        });
        Step("close-detach-complete", () =>
        {
            OpenLuaFile(workspace!, process, loopArtifact, artifacts, "detach-complete-loop-file-picker");
            InvokeMenu(gui, process, "Workspace Debug Seed", TimeSpan.FromSeconds(8));
            SaveMainAss(gui, input, seeded, artifacts, "close-detach-complete-seed.ass");
            var invocation = Task.Run(() => InvokeButton(workspace!, "Debug"));
            WaitPauseAt(workspace!, "entry", 1, "debug-loop.lua", 2, TimeSpan.FromSeconds(15));
            InvokeButton(workspace!, "Continue");
            WaitLoopStarted(workspace!, process, TimeSpan.FromSeconds(5));
            Native.PostClose(workspace!, process);
            var decision = WaitWindow(process, "Active Lua Workspace invocation", TimeSpan.FromSeconds(5));
            SaveEvidence(decision, artifacts, "close-detach-complete-choice");
            InvokeButton(decision, "Detach and keep open");
            workspace = WaitWindow(process, "Lua Workspace", TimeSpan.FromSeconds(5));
            WaitUntil(() => ReadRunStatus(workspace).StartsWith("Detached;", StringComparison.Ordinal)
                && FindButton(workspace, "Stop").Current.IsEnabled, TimeSpan.FromSeconds(5),
                "Natural-completion Detach did not retain a visible enabled Stop control");
            Ensure(!gui.Current.IsEnabled && !workspace.Current.IsOffscreen && !FindButton(workspace, "Stop").Current.IsOffscreen,
                "Natural-completion Detach lost its visible control surface while the commit boundary was active");
            SaveEvidence(workspace, artifacts, "close-detach-complete-running");
            WaitUntil(() => invocation.IsCompleted && ReadRunStatus(workspace).Contains("completed", StringComparison.OrdinalIgnoreCase)
                && gui.Current.IsEnabled, TimeSpan.FromSeconds(30), "Detached bounded loop did not reach its real completed terminal state");
            Ensure(invocation.Wait(TimeSpan.FromSeconds(5)), "Detached bounded invocation task did not return after terminal state");
            SaveEvidence(workspace, artifacts, "close-detach-completed");
        });
        Step("close-detach-save-undo", () =>
        {
            SaveMainAss(gui, input, completed, artifacts, "close-detach-completed.ass");
            File.Copy(input, Path.Combine(artifacts, "close-detach-before-first-undo.ass"));
            SaveMainSaveState(gui, artifacts, "close-detach-save-before-first-undo.json");
            InvokeMenu(gui, process, "Undo", TimeSpan.FromSeconds(8), "Edit");
            File.Copy(input, Path.Combine(artifacts, "close-detach-after-first-undo-before-save.ass"));
            SaveMainSaveState(gui, artifacts, "close-detach-save-after-first-undo.json");
            try { SaveMainAss(gui, input, seeded, artifacts, "close-detach-undone-once.ass"); }
            finally { File.Copy(input, Path.Combine(artifacts, "close-detach-after-first-undo-save-attempt.ass"), overwrite: true); }
            InvokeMenu(gui, process, "Undo", TimeSpan.FromSeconds(8), "Edit");
            SaveMainAss(gui, input, baseline, artifacts, "close-detach-undone-twice.ass");
        });
        Step("normal-shutdown", () =>
        {
            if (!gui.TryGetCurrentPattern(WindowPattern.Pattern, out var pattern))
                throw new InvalidOperationException("Main frame lacks real WindowPattern.Close");
            Native.StabilizeOwnedClipboard(process.Id);
            ((WindowPattern)pattern).Close();
            Ensure(process.WaitForExit(TimeSpan.FromSeconds(10)), "GUI host did not exit normally within ten seconds");
            Ensure(process.ExitCode == 0, "GUI host returned nonzero after normal close");
            File.WriteAllText(Path.Combine(artifacts, "normal-shutdown.json"), JsonSerializer.Serialize(new
            {
                HostExitCode = process.ExitCode, Method = "WindowPattern.Close"
            }, new JsonSerializerOptions { WriteIndented = true }));
        });
    }

    void Step(string name, Action action)
    {
        File.AppendAllText(Path.Combine(artifacts, "stages.log"), $"{DateTimeOffset.UtcNow:O}\t{name}\tstart{Environment.NewLine}");
        action();
        results.Add(new StepResult(name, "passed", ""));
        File.AppendAllText(Path.Combine(artifacts, "stages.log"), $"{DateTimeOffset.UtcNow:O}\t{name}\tpassed{Environment.NewLine}");
        WriteManifest();
    }
    void WriteManifest() => File.WriteAllText(Path.Combine(artifacts, "manifest.json"), JsonSerializer.Serialize(new
    {
        StartedUtc = startedUtc, FinishedUtc = finishedUtc, BudgetSeconds = scenario == "jit" ? 360 : scenario == "preparation" ? 300 : 240,
        ExeSha256 = hashes["exe"],
        Scenario = scenario,
        AcceptanceScope = entryScenario ? "R1 Workspace templater entry identity, full-document qualification, and unchanged Automation-menu validation; this scenario only."
            : languageScenario ? "S6 language refresh/source isolation and unavailable-service Run/Debug/Undo/Save; not full S6 on its own."
            : scenario == "preparation" ? "R3 cancellable Lua-file load/include/validation preparation and cleanup; this scenario only."
            : "This scenario only. Full S4 requires basic, launch, controls, files, dap, jit, native virtual-source E2E, and S2/S3 regressions.",
        Fixtures = new[] { fixture.Replace('\\', '/'), actions.Replace('\\', '/'), expectedFile.Replace('\\', '/'),
            stepsFile.Replace('\\', '/'), loopFile.Replace('\\', '/'), infiniteLoopFile.Replace('\\', '/'), templater.Replace('\\', '/'),
            entryFixture.Replace('\\', '/'), entryActions.Replace('\\', '/'), entryDuplicate.Replace('\\', '/') }
            .Concat(scenario == "preparation"
                ? preparationFixtures.Select(name => Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures", name).Replace('\\', '/'))
                : []),
        Sha256 = hashes, ExitStatus = exitStatus, Steps = results
    }, new JsonSerializerOptions { WriteIndented = true }));
}

static void PrepareChineseLocale(string exe, string artifacts, Dictionary<string, string> hashes)
{
    var buildDir = Directory.GetParent(Path.GetDirectoryName(exe)!)?.FullName
        ?? throw new InvalidOperationException("The executable has no configured CMake build directory");
    var cache = Path.Combine(buildDir, "CMakeCache.txt");
    Ensure(File.Exists(cache), "The Chinese-locale scenario requires the configured CMake cache");
    var setting = File.ReadLines(cache).SingleOrDefault(line => line.StartsWith("GETTEXT_MSGFMT_EXECUTABLE:FILEPATH=", StringComparison.Ordinal))
        ?? throw new InvalidOperationException("The CMake build has no configured gettext msgfmt executable");
    var msgfmt = setting[(setting.IndexOf('=') + 1)..];
    Ensure(File.Exists(msgfmt), "The configured gettext msgfmt executable is unavailable");
    var catalog = Path.Combine(Path.GetDirectoryName(exe)!, "locale", "zh_CN", "LC_MESSAGES", "aegisub.mo");
    Directory.CreateDirectory(Path.GetDirectoryName(catalog)!);
    var start = new ProcessStartInfo(msgfmt) { UseShellExecute = false };
    start.ArgumentList.Add("-o");
    start.ArgumentList.Add(catalog);
    start.ArgumentList.Add(Path.GetFullPath(Path.Combine("po", "zh_CN.po")));
    using var compiler = Process.Start(start) ?? throw new InvalidOperationException("Could not start the configured gettext compiler");
    if (!compiler.WaitForExit(TimeSpan.FromSeconds(10)))
    {
        compiler.Kill(entireProcessTree: true);
        compiler.WaitForExit(TimeSpan.FromSeconds(5));
        throw new TimeoutException("Chinese gettext catalog compilation exceeded its finite deadline");
    }
    Ensure(compiler.ExitCode == 0 && File.Exists(catalog), "The Chinese gettext catalog failed to compile");
    hashes["zh_catalog"] = Hash(catalog);
    File.Copy(catalog, Path.Combine(artifacts, "entry-zh-catalog.mo"));
}

static void PrepareEntryInput(string input, string scenario)
{
    var source = File.ReadAllText(input);
    const string template = "Comment: 0,0:00:00.00,0:00:00.00,Default,,0,0,0,template line notext,R1-LATE";
    const string templaterScript = "|~kara-templater.lua";
    Ensure(source.Split(template, StringSplitOptions.None).Length == 2
        && source.Split(templaterScript, StringSplitOptions.None).Length == 2,
        "Entry fixture has no unique template or templater script declaration");
    var newline = source.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n";
    var filler = string.Concat(Enumerable.Range(1, 51).Select(index =>
        $"Dialogue: 0,0:00:00.00,0:00:00.50,Default,,0,0,0,skip,ignored-{index:D2}{newline}"));
    source = source.Replace(template, filler + template, StringComparison.Ordinal);
    if (scenario == "entry-once")
        source = source.Replace(template + newline, "", StringComparison.Ordinal);
    if (scenario == "entry-missing")
        source = source.Replace(templaterScript, "", StringComparison.Ordinal);
    if (scenario == "entry-ambiguous")
        source = source.Replace(templaterScript, templaterScript + "|~workspace-entry-duplicate.lua", StringComparison.Ordinal);
    File.WriteAllText(input, source, new UTF8Encoding(false));
}

static (MenuObservation.Snapshot? Probe, MenuObservation.Snapshot? Templater) ObserveEntryMenu(
    AutomationElement main, Process host, string menuName)
{
    AutomationElement? menu = null;
    WaitUntil(() => main.Current.IsEnabled && (menu = main.FindAll(TreeScope.Descendants,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem))
        .Cast<AutomationElement>().SingleOrDefault(item => item.Current.ProcessId == host.Id && !item.Current.IsOffscreen
            && (item.Current.Name ?? "").Replace("&", "", StringComparison.Ordinal)
                .Equals(menuName, StringComparison.OrdinalIgnoreCase))) is not null,
        TimeSpan.FromSeconds(8), $"Main menu {menuName} was not exposed for entry validation");
    var opened = false;
    if (menu!.TryGetCurrentPattern(ExpandCollapsePattern.Pattern, out var expand))
    {
        try { ((ExpandCollapsePattern)expand).Expand(); opened = true; }
        catch (InvalidOperationException) { }
    }
    if (!opened && menu.TryGetCurrentPattern(InvokePattern.Pattern, out var invoke))
    {
        ((InvokePattern)invoke).Invoke();
        opened = true;
    }
    if (!opened && menuName is "Automation" or "自动化(U)")
    {
        Native.PostSystemMenu(new nint(main.Current.NativeWindowHandle), 'u');
        opened = true;
    }
    Ensure(opened, $"Main menu {menuName} could not be opened for entry validation");
    WaitUntil(() => FindVisibleMenuCommand(host, "Workspace Entry Validation Probe") is not null,
        TimeSpan.FromSeconds(5), "Entry validation probe did not appear in the opened Automation menu");
    MenuObservation.Snapshot? Snapshot(string commandName)
    {
        var command = FindVisibleMenuCommand(host, commandName);
        if (command is null) return null;
        var snapshot = MenuObservation.Capture(menuName, commandName, command.Current,
            command.TryGetCurrentPattern(InvokePattern.Pattern, out _));
        MenuObservation.Record(snapshot, "entry-validation-observed", null);
        return snapshot;
    }
    var probe = Snapshot("Workspace Entry Validation Probe");
    var templater = Snapshot("Apply karaoke template");
    var select = FindVisibleMenuCommand(host, "Workspace Entry Select Code")
        ?? throw new InvalidOperationException("Entry code selector was not present to dismiss the observed menu");
    Ensure(select.Current.IsEnabled && !select.Current.IsOffscreen && select.TryGetCurrentPattern(InvokePattern.Pattern, out _),
        "Entry code selector was not available after menu validation observations");
    UiaDriver.Invoke(select);
    return (probe, templater);
}

static void SaveEntryAss(AutomationElement main, string input, IReadOnlyList<AssEvent> expected, string artifacts, string evidenceName)
{
    AutomationElement? save = null;
    WaitUntil(() => (save = UiaDriver.FindDescendantByAutomationId(main, "Item 5002", ControlType.Button)) is not null
        && save.Current.IsEnabled && !save.Current.IsOffscreen && save.TryGetCurrentPattern(InvokePattern.Pattern, out _),
        TimeSpan.FromSeconds(8), "Main Save did not become visibly invokable after entry completion");
    UiaDriver.Invoke(save!);
    WaitUntil(() => EventLines(File.ReadAllText(input)).SequenceEqual(expected), TimeSpan.FromSeconds(8),
        "Saved entry ASS does not match its independent generated-event expectation");
    var physical = File.ReadAllText(input);
    AssertPhysicalEventLines(physical, expected.Count);
    File.WriteAllText(Path.Combine(artifacts, evidenceName), physical, new UTF8Encoding(false));
}

static AutomationElement LanguageStatusElement(AutomationElement workspace)
{
    var matches = workspace.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Text))
        .Cast<AutomationElement>().Where(item => item.Current.ClassName == "Static" && !item.Current.IsOffscreen
            && item.Current.NativeWindowHandle != 0 && item.Current.Name.StartsWith("LuaLS", StringComparison.Ordinal)).ToArray();
    Ensure(matches.Length == 1, $"Expected one visible LuaLS status, found {matches.Length}");
    return matches[0];
}

static string ReadLanguageStatus(AutomationElement workspace, Process host)
    => Native.ReadStaticText(new nint(LanguageStatusElement(workspace).Current.NativeWindowHandle), host);

static string ReadLanguageDiagnostics(AutomationElement workspace, Process host)
{
    var panel = TreeWalker.ControlViewWalker.GetParent(LanguageStatusElement(workspace))
        ?? throw new InvalidOperationException("Language status has no containing panel");
    var edits = panel.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Document))
        .Cast<AutomationElement>().Where(item => item.Current.ClassName == "Edit" && !item.Current.IsOffscreen).ToArray();
    Ensure(edits.Length == 1, "Language panel does not have exactly one diagnostics Edit");
    return Native.ReadEditText(edits[0], host);
}

static string Hash(string path) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
static string Normalize(string text) => text.Replace("\r\n", "\n", StringComparison.Ordinal);
static void Ensure(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }

static void WaitUntil(Func<bool> predicate, TimeSpan timeout, string failure)
{
    var timer = Stopwatch.StartNew();
    while (timer.Elapsed < timeout)
    {
        if (predicate()) return;
        Thread.Sleep(75);
    }
    throw new TimeoutException(failure);
}

static AutomationElement? FindWindow(Process host, string prefix)
{
    var matches = Native.HostWindows(host.Id).Where(handle => Native.WindowText(handle).StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
        .Select(AutomationElement.FromHandle).Where(item => item.Current.ProcessId == host.Id
            && item.Current.ControlType == ControlType.Window && !item.Current.IsOffscreen).ToArray();
    Ensure(matches.Length <= 1, $"More than one visible {prefix} window belongs to the host");
    return matches.SingleOrDefault();
}

static AutomationElement WaitWindow(Process host, string prefix, TimeSpan timeout)
{
    AutomationElement? result = null;
    WaitUntil(() => (result = FindWindow(host, prefix)) is not null, timeout, $"Window {prefix} was not found");
    return result!;
}

static bool HasWorkspaceStaticText(AutomationElement workspace, Func<string, bool> matches, bool allowHidden = false)
{
    using var host = Process.GetProcessById(workspace.Current.ProcessId);
    var controls = Native.ChildTextControls(workspace, host, "Static", matches, allowHidden);
    if (controls.Count == 0) return false;
    Ensure(controls.Select(control => control.Text).Distinct(StringComparer.Ordinal).Count() == 1,
        "Workspace exposed conflicting matching Static text");
    return true;
}

static AutomationElement? FindWorkspaceStaticText(AutomationElement workspace, Func<string, bool> matches, bool allowHidden = false)
{
    using var host = Process.GetProcessById(workspace.Current.ProcessId);
    var controls = Native.ChildTextControls(workspace, host, "Static", matches, allowHidden);
    Ensure(controls.Count <= 1, $"More than one Workspace Static matched the requested text ({controls.Count})");
    return controls.Count == 1 ? controls[0].Element : null;
}

static string ReadRunStatus(AutomationElement workspace, bool allowHidden = false)
{
    using var host = Process.GetProcessById(workspace.Current.ProcessId);
    var controls = Native.ChildTextControls(workspace, host, "Static", value => value.StartsWith("Debug:", StringComparison.Ordinal)
        || value.StartsWith("Preparing", StringComparison.Ordinal)
        || value.StartsWith("Invocation failed", StringComparison.Ordinal)
        || value.StartsWith("Invocation cancelled", StringComparison.Ordinal)
        || value.StartsWith("Invocation completed", StringComparison.Ordinal)
        || value.StartsWith("Invocation ended without an observed terminal result", StringComparison.Ordinal)
        || value.StartsWith("Stopping", StringComparison.Ordinal)
        || value.StartsWith("Detached;", StringComparison.Ordinal)
        || value.StartsWith("Run active", StringComparison.Ordinal), allowHidden);
    if (controls.Count == 0) return "";
    var values = controls.Select(control => control.Text).Distinct(StringComparer.Ordinal).ToArray();
    Ensure(values.Length == 1, "Workspace exposed conflicting invocation status text: " + string.Join(" | ", values));
    return values[0];
}

static nint CaptureRunStatusHandle(AutomationElement workspace, Process host)
{
    var status = FindWorkspaceStaticText(workspace, value => value.StartsWith("Debug:", StringComparison.Ordinal))
        ?? throw new InvalidOperationException("Visible debug status was not found before hiding Workspace");
    var current = status.Current;
    Ensure(current.ClassName == "Static" && current.ProcessId == host.Id && current.NativeWindowHandle != 0,
        "Debug status is not an observed native Static control in this host");
    return new nint(current.NativeWindowHandle);
}

static PauseStatus? ParsePauseStatus(string text)
{
    var match = Regex.Match(text, @"^Debug: paused \[(entry|breakpoint|step|pause) #(\d+)\] (.+):(\d+)$", RegexOptions.CultureInvariant);
    return match.Success ? new PauseStatus(match.Groups[1].Value, int.Parse(match.Groups[2].Value),
        match.Groups[3].Value.Replace('\\', '/'), int.Parse(match.Groups[4].Value)) : null;
}

static void WaitPauseAt(AutomationElement workspace, string reason, int sequence, string file, int line, TimeSpan timeout)
{
    WaitUntil(() => ParsePauseStatus(ReadRunStatus(workspace)) is { } pause && pause.Reason == reason
        && pause.Sequence == sequence && pause.Line == line
        && pause.SourcePath.EndsWith("/" + file, StringComparison.OrdinalIgnoreCase), timeout,
        $"Debug did not report {reason} #{sequence} at {file}:{line}");
}

static List<string> StackItems(AutomationElement workspace)
{
    var names = new List<string>();
    var lists = Native.ChildElements(workspace, "ListBox").Where(item => item.Current.ProcessId == workspace.Current.ProcessId
        && item.Current.ControlType == ControlType.List && !item.Current.IsOffscreen).ToArray();
    Ensure(lists.Length == 1, $"Expected one visible Lua stack frame list, found {lists.Length}");
    foreach (AutomationElement item in lists[0].FindAll(TreeScope.Children,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ListItem)))
    {
        try { if (!item.Current.IsOffscreen) names.Add(item.Current.Name); }
        catch (ElementNotAvailableException) { }
    }
    Ensure(names.Count > 0, "Visible paused stack has no UIA ListItem frames");
    return names;
}

static string ReadRunLog(AutomationElement workspace, Process host)
{
    var edits = workspace.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>()
        .Where(item => item.Current.ProcessId == host.Id && !item.Current.IsOffscreen && item.Current.ClassName == "Edit"
            && item.Current.ControlType == ControlType.Document && item.Current.NativeWindowHandle != 0
            && !HasNotebookAncestor(item, workspace)).ToArray();
    Ensure(edits.Length == 1, $"Expected one visible run-log Edit outside notebook, found {edits.Length}");
    return Native.ReadEditText(edits[0], host);
}

static void WaitLoopStarted(AutomationElement workspace, Process host, TimeSpan timeout)
{
    WaitUntil(() =>
    {
        var status = ReadRunStatus(workspace);
        return (status.StartsWith("Debug: running", StringComparison.Ordinal) || status.StartsWith("Run active", StringComparison.Ordinal))
            && ReadRunLog(workspace, host).Contains("workspace-debug-loop-started", StringComparison.Ordinal);
    }, timeout,
        "Long loop did not enter running state and report its unique progress handshake");
}

static bool HasNotebookAncestor(AutomationElement item, AutomationElement workspace)
{
    var parent = TreeWalker.ControlViewWalker.GetParent(item);
    while (parent is not null && parent != workspace)
    {
        if (parent.Current.ClassName == "_wx_SysTabCtl32") return true;
        parent = TreeWalker.ControlViewWalker.GetParent(parent);
    }
    return false;
}

static void AssertLivePauseControls(AutomationElement main, AutomationElement workspace)
{
    Ensure(!main.Current.IsEnabled, "Main frame remained enabled during a live paused Workspace invocation");
    foreach (var name in new[] { "Continue", "Step In", "Step Over", "Step Out", "Stop" })
        Ensure(FindButton(workspace, name).Current.IsEnabled, $"Live pause control {name} is disabled");
    foreach (var name in new[] { "Run", "Debug", "Open File", "Reload" })
        Ensure(!FindButton(workspace, name).Current.IsEnabled, $"Unsafe {name} stayed enabled during a live pause");
    var save = UiaDriver.FindDescendantByAutomationId(workspace, "5003", ControlType.Button)
        ?? throw new InvalidOperationException("Workspace has no exact Apply/Save control");
    Ensure(save.Current.Name is "Apply" or "Save" && !save.Current.IsEnabled,
        "Workspace Apply/Save control was unexpected or enabled during a live pause");
}

static AutomationElement PauseOnEntry(AutomationElement workspace)
{
    var matches = Native.ChildElements(workspace, "Button").Where(item => item.Current.ProcessId == workspace.Current.ProcessId
        && item.Current.ControlType == ControlType.CheckBox && item.Current.Name == "Pause on entry" && !item.Current.IsOffscreen).ToArray();
    Ensure(matches.Length == 1, $"Expected one visible Pause on entry checkbox, found {matches.Length}");
    Ensure(matches[0].TryGetCurrentPattern(TogglePattern.Pattern, out _), "Pause on entry has no TogglePattern");
    return matches[0];
}

static ToggleState ReadPauseOnEntry(AutomationElement workspace)
    => ((TogglePattern)PauseOnEntry(workspace).GetCurrentPattern(TogglePattern.Pattern)).Current.ToggleState;

static void SetPauseOnEntry(AutomationElement workspace, bool enabled)
{
    var checkbox = PauseOnEntry(workspace);
    var toggle = (TogglePattern)checkbox.GetCurrentPattern(TogglePattern.Pattern);
    var expected = enabled ? ToggleState.On : ToggleState.Off;
    if (toggle.Current.ToggleState != expected) toggle.Toggle();
    WaitUntil(() => toggle.Current.ToggleState == expected, TimeSpan.FromSeconds(2),
        "Pause on entry did not reach the explicitly requested state");
}

static string ReadDebugLocation(AutomationElement workspace)
{
    var location = FindWorkspaceStaticText(workspace, name => name.StartsWith("Paused at ", StringComparison.Ordinal)
        || name.StartsWith("Last pause [last pause, not current]", StringComparison.Ordinal));
    Ensure(location is not null, "Expected one visible Lua debug location, found none");
    return location!.Current.Name;
}

static AutomationElement VariableTree(AutomationElement workspace)
{
    var trees = Native.ChildElements(workspace, "SysTreeView32").Where(item => item.Current.ProcessId == workspace.Current.ProcessId
        && item.Current.ControlType == ControlType.Tree && !item.Current.IsOffscreen).ToArray();
    Ensure(trees.Length == 1, $"Expected one visible Lua variables tree, found {trees.Length}");
    return trees[0];
}

static AutomationElement FindTreeItem(AutomationElement root, Func<string, bool> predicate, string description)
{
    var items = root.FindAll(TreeScope.Descendants,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.TreeItem)).Cast<AutomationElement>()
        .Where(item => predicate(item.Current.Name ?? "")).ToArray();
    Ensure(items.Length == 1, $"Expected one {description} tree item, found {items.Length}");
    return items[0];
}

static ExpandCollapseState TreeExpansion(AutomationElement item)
{
    Ensure(item.TryGetCurrentPattern(ExpandCollapsePattern.Pattern, out var pattern),
        $"Tree item {item.Current.Name} has no ExpandCollapsePattern");
    return ((ExpandCollapsePattern)pattern).Current.ExpandCollapseState;
}

static string SelectedTreeItemName(AutomationElement tree)
{
    Ensure(tree.TryGetCurrentPattern(SelectionPattern.Pattern, out var pattern), "Lua variables tree has no SelectionPattern");
    var selected = ((SelectionPattern)pattern).Current.GetSelection();
    Ensure(selected.Length == 1, $"Expected one selected Lua variable item, found {selected.Length}");
    return selected[0].Current.Name;
}

static string ReadVariableDetails(AutomationElement workspace, Process host)
{
    var edits = Native.ChildElements(workspace, "Edit").Where(item => item.Current.ProcessId == host.Id && !item.Current.IsOffscreen
        && item.Current.ControlType == ControlType.Document && HasNotebookAncestor(item, workspace)).ToArray();
    Ensure(edits.Length == 1, $"Expected one visible Lua variable details control, found {edits.Length}");
    return Native.ReadEditText(edits[0], host);
}

static void AssertVariableTreePause(AutomationElement workspace, Process host)
{
    var tree = VariableTree(workspace);
    var locals = FindTreeItem(tree, name => Regex.IsMatch(name, @"^Locals \([1-9][0-9]*\)$"), "nonempty Locals scope");
    var upvalues = FindTreeItem(tree, name => Regex.IsMatch(name, @"^Upvalues \([1-9][0-9]*\)$"), "nonempty Upvalues scope");
    Ensure(TreeExpansion(locals) == ExpandCollapseState.Expanded, "Locals scope is not initially expanded");
    Ensure(TreeExpansion(upvalues) == ExpandCollapseState.Expanded, "Upvalues scope is not initially expanded");
    _ = FindTreeItem(upvalues, name => name == "prefix = \"DBG:\"", "captured prefix upvalue");

    var largeScopes = tree.FindAll(TreeScope.Descendants,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.TreeItem)).Cast<AutomationElement>()
        .Where(item => Regex.IsMatch(item.Current.Name ?? "", @"^(Globals|Functions|Runtime Globals) \([1-9][0-9]*\)$")).ToArray();
    Ensure(largeScopes.Length == 3, $"Expected three nonempty large variable scopes, found {largeScopes.Length}");
    var largeStates = largeScopes.Select(item => (item.Current.Name, State: TreeExpansion(item))).ToArray();
    Ensure(largeStates.All(item => item.State is not (ExpandCollapseState.Expanded or ExpandCollapseState.PartiallyExpanded)),
        "A large variable scope was expanded by default: " + string.Join(", ", largeStates.Select(item => $"{item.Name}={item.State}")));

    var frame = FindTreeItem(locals, name => name == "__frame = decorate",
        "decorate frame variable");
    Ensure(TreeExpansion(frame) == ExpandCollapseState.Collapsed, "Nested frame variable was expanded by default");
    Ensure(SelectedTreeItemName(tree) == "value = \"alpha\"", "Variable tree did not initially select its first local value");
    Native.SendKeyToControl(tree, host, 0x28);
    WaitUntil(() => SelectedTreeItemName(VariableTree(workspace)) == "__frame = decorate", TimeSpan.FromSeconds(2),
        "Down navigation did not select the nested frame variable");
    Native.SendKeyToControl(VariableTree(workspace), host, 0x27);
    WaitUntil(() => TreeExpansion(FindTreeItem(VariableTree(workspace), name => name == "__frame = decorate",
            "expanded decorate frame variable")) == ExpandCollapseState.Expanded,
        TimeSpan.FromSeconds(2), "Right navigation did not expand the nested frame variable");
    Native.SendKeyToControl(VariableTree(workspace), host, 0x28);
    WaitUntil(() => SelectedTreeItemName(VariableTree(workspace)) == "function_name = decorate", TimeSpan.FromSeconds(2),
        "Down navigation did not select the nested function_name value");
    WaitUntil(() => ReadVariableDetails(workspace, host).StartsWith("Paused value", StringComparison.Ordinal),
        TimeSpan.FromSeconds(2), "Selected variable details did not identify a live paused value");
    var details = ReadVariableDetails(workspace, host);
    Ensure(details.Contains("function_name", StringComparison.Ordinal) && details.Contains("decorate", StringComparison.Ordinal),
        "Selected nested variable details lost its exact name or value");
}

static AutomationElement FindButton(AutomationElement window, string name)
{
    var buttons = Native.ChildElements(window, "Button").Where(item => item.Current.ControlType == ControlType.Button
        && item.Current.Name == name).ToArray();
    Ensure(buttons.Length == 1, $"Expected one exact {name} button, found {buttons.Length}");
    return buttons[0];
}

static AutomationElement? FindStyledText(AutomationElement workspace, bool insideNotebook)
{
    var candidates = new List<AutomationElement>();
    foreach (var item in Native.ChildElements(workspace, "wxWindow"))
    {
        try
        {
            var current = item.Current;
            if (current.ProcessId != workspace.Current.ProcessId || current.IsOffscreen
                || current.ControlType != ControlType.Pane || current.ClassName != "wxWindow"
                || current.NativeWindowHandle == 0
                || Native.WindowText(new nint(current.NativeWindowHandle)) != "stcwindow")
                continue;
            if (HasNotebookAncestor(item, workspace) == insideNotebook) candidates.Add(item);
        }
        catch (ElementNotAvailableException) { }
    }
    Ensure(candidates.Count <= 1, $"Workspace exposes {candidates.Count} visible STC controls for notebook={insideNotebook}");
    return candidates.Count == 1 ? candidates[0] : null;
}

static void SelectTab(AutomationElement workspace, string name)
{
    var notebooks = Native.ChildElements(workspace, "_wx_SysTabCtl32").ToArray();
    var tabs = notebooks.SelectMany(notebook => notebook.FindAll(TreeScope.Children,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.TabItem)).Cast<AutomationElement>())
        .Where(item => item.Current.Name == name).ToArray();
    Ensure(tabs.Length == 1 && tabs[0].TryGetCurrentPattern(SelectionItemPattern.Pattern, out _), $"Exact tab {name} was unavailable");
    var pattern = (SelectionItemPattern)tabs[0].GetCurrentPattern(SelectionItemPattern.Pattern);
    pattern.Select();
    WaitUntil(() => pattern.Current.IsSelected, TimeSpan.FromSeconds(3), $"Tab {name} did not select");
}

static bool IsTabSelected(AutomationElement workspace, string name)
{
    var notebooks = Native.ChildElements(workspace, "_wx_SysTabCtl32").ToArray();
    var tabs = notebooks.SelectMany(notebook => notebook.FindAll(TreeScope.Children,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.TabItem)).Cast<AutomationElement>())
        .Where(item => item.Current.Name == name).ToArray();
    Ensure(tabs.Length == 1 && tabs[0].TryGetCurrentPattern(SelectionItemPattern.Pattern, out _),
        $"Exact tab {name} was unavailable for selected-state observation");
    return ((SelectionItemPattern)tabs[0].GetCurrentPattern(SelectionItemPattern.Pattern)).Current.IsSelected;
}

static bool StackHasSourceLine(AutomationElement workspace, string sourcePrefix, int line)
{
    var lists = Native.ChildElements(workspace, "ListBox").Where(item => item.Current.ProcessId == workspace.Current.ProcessId
        && item.Current.ControlType == ControlType.List && !item.Current.IsOffscreen).ToArray();
    Ensure(lists.Length == 1, $"Expected one visible Lua stack frame list, found {lists.Length}");
    foreach (AutomationElement item in lists[0].FindAll(TreeScope.Children,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ListItem)))
    {
        try
        {
            var current = item.Current;
            if (!current.IsOffscreen && current.Name.Contains(sourcePrefix, StringComparison.Ordinal)
                && current.Name.EndsWith(":" + line, StringComparison.Ordinal)) return true;
        }
        catch (ElementNotAvailableException) { }
    }
    return false;
}

static void InvokeButton(AutomationElement window, string name)
{
    var button = FindButton(window, name);
    Ensure(button.Current.IsEnabled && !button.Current.IsOffscreen && button.TryGetCurrentPattern(InvokePattern.Pattern, out _),
        $"Exact {name} button is not visibly enabled and invokable");
    UiaDriver.Invoke(button);
}

static void OpenLuaFile(AutomationElement workspace, Process host, string path, string artifacts, string evidenceName)
{
    var task = Task.Run(() => InvokeButton(workspace, "Open File"));
    var picker = WaitWindow(host, "Open Lua source", TimeSpan.FromSeconds(6));
    SaveEvidence(picker, artifacts, evidenceName);
    var filename = UiaDriver.FindDescendantByAutomationId(picker, "1148", ControlType.Edit);
    if (filename is null || !filename.TryGetCurrentPattern(ValuePattern.Pattern, out var pattern))
        throw new InvalidOperationException("Open Lua source picker has no exact filename ValuePattern");
    ((ValuePattern)pattern).SetValue(path);
    var open = UiaDriver.FindDescendantByAutomationId(picker, "1", ControlType.Button)
        ?? throw new InvalidOperationException("Open Lua source picker has no observed stock Open ID 1");
    Ensure(open.Current.IsEnabled && open.TryGetCurrentPattern(InvokePattern.Pattern, out _), "Picker Open button is not invokable");
    UiaDriver.Invoke(open);
    Ensure(task.Wait(TimeSpan.FromSeconds(8)), "Open Lua source picker did not complete after exact Open");
    WaitUntil(() => FindWindow(host, "Open Lua source") is null
        && workspace.Current.Name.Contains(Path.GetFileName(path), StringComparison.Ordinal), TimeSpan.FromSeconds(6),
        "Open Lua source did not close its picker and bind the requested saved file");
}

static void SaveMainSaveState(AutomationElement main, string artifacts, string evidenceName)
{
    var save = UiaDriver.FindDescendantByAutomationId(main, "Item 5002", ControlType.Button)
        ?? throw new InvalidOperationException("Main Save control was unavailable for state evidence");
    var current = save.Current;
    File.WriteAllText(Path.Combine(artifacts, evidenceName), JsonSerializer.Serialize(new
    {
        current.Name,
        current.AutomationId,
        current.ProcessId,
        current.NativeWindowHandle,
        current.IsEnabled,
        current.IsOffscreen,
        HasInvokePattern = save.TryGetCurrentPattern(InvokePattern.Pattern, out _)
    }, new JsonSerializerOptions { WriteIndented = true }));
}

static void SaveMainAss(AutomationElement main, string input, IReadOnlyList<AssEvent> expected, string artifacts, string evidenceName)
{
    AutomationElement? save = null;
    WaitUntil(() => (save = UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles")) is not null,
        TimeSpan.FromSeconds(8), "Main Save did not become available for a committed ASS change");
    UiaDriver.Invoke(save!);
    WaitUntil(() => EventLines(File.ReadAllText(input)).SequenceEqual(expected), TimeSpan.FromSeconds(8),
        "Saved ASS events did not exactly match the independent scenario baseline");
    var text = File.ReadAllText(input);
    AssertPhysicalEventLines(text, expected.Count);
    File.WriteAllText(Path.Combine(artifacts, evidenceName), text, new UTF8Encoding(false));
}

static void AssertPhysicalEventLines(string ass, int expected)
{
    var lines = ass.Split('\n').Select(line => line.TrimEnd('\r')).ToArray();
    var section = Array.IndexOf(lines, "[Events]");
    Ensure(section >= 0, "Saved ASS has no Events section");
    var count = 0;
    foreach (var line in lines.Skip(section + 1))
    {
        if (line.Length == 0) continue;
        if (line.StartsWith('[')) break;
        if (line.StartsWith("Format:", StringComparison.Ordinal)) continue;
        Ensure(line.StartsWith("Comment:", StringComparison.Ordinal) || line.StartsWith("Dialogue:", StringComparison.Ordinal),
            "Saved ASS has an orphan physical continuation line");
        ++count;
    }
    Ensure(count == expected, "Saved ASS physical event count differs from scenario expectation");
}

static AutomationElement WaitWindowContainingText(Process host, string exactText, TimeSpan timeout)
{
    AutomationElement? found = null;
    WaitUntil(() =>
    {
        foreach (var handle in Native.HostWindows(host.Id))
        {
            if (Native.WindowClass(handle) != "#32770") continue;
            var dialog = AutomationElement.FromHandle(handle);
            if (!dialog.Current.IsEnabled || dialog.Current.IsOffscreen) continue;
            var labels = Native.ChildElements(dialog, "Static").Where(label => label.Current.ControlType == ControlType.Text
                && label.Current.Name == exactText && !label.Current.IsOffscreen).ToArray();
            if (labels.Length != 1) continue;
            found = dialog;
            return true;
        }
        return false;
    }, timeout, $"Exact dialog label {exactText} was not exposed");
    return found!;
}

static void VerifySeed(AutomationElement main, Process host, string input, IReadOnlyList<AssEvent> seeded, string artifacts, string evidenceName)
{
    var before = File.ReadAllBytes(input);
    var commandReady = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
    var task = Task.Run(() => InvokeMenu(main, host, "Workspace Debug Verify Seed Rollback", TimeSpan.FromSeconds(8),
        commandReady: () => commandReady.TrySetResult()));
    WaitUntil(() => commandReady.Task.IsCompleted || task.IsCompleted, TimeSpan.FromSeconds(12),
        "Live rollback verifier command did not become invokable");
    if (!commandReady.Task.IsCompleted) task.GetAwaiter().GetResult();
    var dialog = WaitWindowContainingText(host, "Debug seed rollback verified", TimeSpan.FromSeconds(8));
    SaveEvidence(dialog, artifacts, evidenceName + "-dialog");
    InvokeButton(dialog, "OK");
    Ensure(task.Wait(TimeSpan.FromSeconds(10)), "Live rollback verifier did not finish after its exact acknowledgement");
    WaitUntil(() => main.Current.IsEnabled && FindProgressPane(host, "Workspace Debug Verify Seed Rollback") is null,
        TimeSpan.FromSeconds(8), "Live rollback verifier did not release its real modal execution UI");
    Ensure(File.ReadAllBytes(input).SequenceEqual(before) && EventLines(File.ReadAllText(input)).SequenceEqual(seeded),
        "Live rollback verifier changed or disagreed with physical seed ASS");
}

static AutomationElement? FindVisibleMenuCommand(Process host, string name)
{
    var matches = Native.HostWindows(host.Id).Where(handle => Native.WindowClass(handle) == "#32768")
        .Select(AutomationElement.FromHandle).SelectMany(root => root.FindAll(TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem)).Cast<AutomationElement>())
        .Where(item => !item.Current.IsOffscreen && (item.Current.Name ?? "").Replace("&", "", StringComparison.Ordinal)
            .Contains(name, StringComparison.OrdinalIgnoreCase)).ToArray();
    var exact = matches.Where(item => item.Current.Name.Replace("&", "", StringComparison.Ordinal).Split('\t')[0]
        .Equals(name, StringComparison.OrdinalIgnoreCase)).ToArray();
    if (exact.Length == 1) return exact[0];
    Ensure(exact.Length == 0 && matches.Length <= 1, $"Ambiguous visible menu command: {name}");
    return matches.SingleOrDefault();
}

static void InvokeMenu(AutomationElement main, Process host, string name, TimeSpan timeout, string menuName = "Automation",
    Action? commandReady = null)
{
    AutomationElement? menu = null;
    WaitUntil(() => main.Current.IsEnabled && (menu = main.FindAll(TreeScope.Descendants,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem))
        .Cast<AutomationElement>().SingleOrDefault(item => item.Current.ProcessId == host.Id && !item.Current.IsOffscreen
            && (item.Current.Name ?? "").Replace("&", "", StringComparison.Ordinal)
                .Equals(menuName, StringComparison.OrdinalIgnoreCase))) is not null,
        timeout, $"Main menu {menuName} was not exposed after execution UI completed");
    var opened = false;
    if (menu!.TryGetCurrentPattern(ExpandCollapsePattern.Pattern, out var expand))
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
        var mnemonic = menuName is "Automation" or "自动化(U)" ? 'u' : menuName == "Edit" ? 'e'
            : throw new InvalidOperationException($"Menu {menuName} has no observed mnemonic");
        Native.PostSystemMenu(new nint(main.Current.NativeWindowHandle), mnemonic);
    }
    AutomationElement? command = null;
    WaitUntil(() => (command = FindVisibleMenuCommand(host, name)) is not null, timeout, $"Menu command {name} was unavailable");
    var current = command!.Current;
    var hasInvoke = command.TryGetCurrentPattern(InvokePattern.Pattern, out _);
    var menuSnapshot = MenuObservation.Capture(menuName, name, current, hasInvoke);
    MenuObservation.Record(menuSnapshot, "before-invoke", null);
    Ensure(current.IsEnabled && !current.IsOffscreen && hasInvoke,
        $"Exact visible menu command {name} was not enabled and invokable");
    commandReady?.Invoke();
    try
    {
        UiaDriver.Invoke(command!);
        MenuObservation.Record(menuSnapshot, "invoke-returned", null);
    }
    catch (Exception error)
    {
        MenuObservation.Record(menuSnapshot, "invoke-failed", error.Message);
        throw;
    }
}

static string CopySource(AutomationElement workspace, Process host)
{
    ClipboardReceipt.VerifyCurrent();
    var before = Native.ClipboardSequence();
    InvokeButton(workspace, "Copy source");
    WaitUntil(() => Native.ClipboardSequence() != before, TimeSpan.FromSeconds(3), "Copy source did not publish clipboard text");
    var sequence = Native.ClipboardSequence();
    Native.AssertClipboardState(sequence, host.Id);
    var text = ClipboardText.ReadUnicodeText(sequence, host.Id);
    Native.AssertClipboardState(sequence, host.Id);
    ClipboardReceipt.Record("workspace-copy-source", host.Id, sequence);
    return text;
}

static void SaveEvidence(AutomationElement window, string artifacts, string name)
{
    var timer = Stopwatch.StartNew();
    try
    {
        var current = window.Current;
        var lines = new List<string>
        {
            $"root\thandle={current.NativeWindowHandle}\tprocess={current.ProcessId}\ttype={current.ControlType.ProgrammaticName}\tclass={current.ClassName}\tenabled={current.IsEnabled}\toffscreen={current.IsOffscreen}\tname={current.Name}"
        };
        lines.AddRange(Native.DescribeChildWindows(new nint(current.NativeWindowHandle), current.ProcessId));
        File.WriteAllLines(Path.Combine(artifacts, name + "-uia.txt"), lines);
        var capture = ScreenCapture.SaveWindowPng(window, Path.Combine(artifacts, name + ".png"));
        Ensure(capture.NonBlackPixelCount > 0, $"{name} screenshot was black");
    }
    finally
    {
        timer.Stop();
        File.AppendAllText(Path.Combine(artifacts, "evidence-timing.tsv"),
            $"{DateTimeOffset.UtcNow:O}\t{name}\t{timer.Elapsed.TotalMilliseconds:F0}{Environment.NewLine}");
    }
}

static void TryEvidence(AutomationElement window, string artifacts, string name)
{
    try { SaveEvidence(window, artifacts, name); }
    catch (Exception error) { File.WriteAllText(Path.Combine(artifacts, name + "-error.txt"), error.Message); }
}

static void TryProcessEvidence(Process host, string artifacts, string name)
{
    try
    {
        var roots = AutomationElement.RootElement.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ProcessIdProperty, host.Id));
        var index = 0;
        foreach (AutomationElement root in roots)
            TryEvidence(root, artifacts, name + "-root-" + index++);
    }
    catch (Exception error) { File.WriteAllText(Path.Combine(artifacts, name + "-error.txt"), error.Message); }
}

static int ParseTime(string text)
{
    var parts = text.Split(':', '.');
    Ensure(parts.Length == 4, "ASS time has an unexpected form");
    return ((int.Parse(parts[0]) * 60 + int.Parse(parts[1])) * 60 + int.Parse(parts[2])) * 1000 + int.Parse(parts[3]) * 10;
}

static List<AssEvent> EventLines(string ass) => ass.Split('\n').Select(line => line.TrimEnd('\r'))
    .Where(line => line.StartsWith("Comment:", StringComparison.Ordinal) || line.StartsWith("Dialogue:", StringComparison.Ordinal))
    .Select(line =>
    {
        var split = line.IndexOf(':');
        var fields = line[(split + 1)..].TrimStart().Split(',', 10);
        Ensure(fields.Length == 10, "ASS event has an unexpected field count");
        return new AssEvent(line[..split], int.Parse(fields[0]), ParseTime(fields[1]), ParseTime(fields[2]), fields[3].Trim(),
            fields[4].Trim(), int.Parse(fields[5]), int.Parse(fields[6]), int.Parse(fields[7]), fields[8].Trim(), fields[9]);
    }).ToList();

static void AssertGenerated(string saved, List<AssEvent> baseline, ExpectedOutput expected)
{
    var lines = saved.Split('\n').Select(line => line.TrimEnd('\r')).ToArray();
    var section = Array.IndexOf(lines, "[Events]");
    Ensure(section >= 0, "Saved ASS lost its Events section");
    var physicalEvents = 0;
    foreach (var line in lines.Skip(section + 1))
    {
        if (line.Length == 0) continue;
        if (line.StartsWith('[')) break;
        if (line.StartsWith("Format:", StringComparison.Ordinal)) continue;
        Ensure(line.StartsWith("Comment:", StringComparison.Ordinal) || line.StartsWith("Dialogue:", StringComparison.Ordinal),
            "Saved ASS has a physical orphan continuation line");
        ++physicalEvents;
    }
    Ensure(physicalEvents == baseline.Count + expected.GeneratedCount, "Saved ASS physical event count differs from independent expectation");
    var actual = EventLines(saved);
    Ensure(actual.Count == baseline.Count + expected.GeneratedCount, "Saved ASS event count differs from independent expectation");
    for (var i = 0; i < 5; ++i)
        Ensure(actual[i] == baseline[i], $"Coding/template event {i} changed during un-applied Run");
    for (var i = 5; i < 7; ++i)
        Ensure(actual[i] with { Kind = baseline[i].Kind, Effect = baseline[i].Effect } == baseline[i]
            && actual[i].Kind == "Comment" && actual[i].Effect == "karaoke", $"Original karaoke event {i} changed unexpectedly");
    for (var i = 0; i < expected.GeneratedCount; ++i)
    {
        var found = actual[baseline.Count + i];
        var wanted = expected.Generated[i];
        Ensure(found.Kind == "Dialogue" && found.Effect == "fx" && found.Style == wanted.Style && found.Name == ""
            && found.MarginL == 0 && found.MarginR == 0 && found.MarginV == 0 && found.Layer == wanted.Layer
            && found.StartMs == wanted.StartMs && found.EndMs == wanted.EndMs && found.Text == wanted.Text,
            $"Generated event {i + 1} differs from independently authored output fields");
    }
}

static AutomationElement? FindProgressPane(Process host, string exactTitle)
{
    var matches = Native.HostWindows(host.Id).Where(handle => Native.WindowClass(handle) == "#32770"
        && Native.WindowText(handle) == exactTitle).Select(AutomationElement.FromHandle)
        .Where(item => item.Current.ProcessId == host.Id && item.Current.ControlType == ControlType.Pane
            && !item.Current.IsOffscreen).ToArray();
    Ensure(matches.Length <= 1, "More than one exact progress dialog was found");
    return matches.SingleOrDefault();
}

static void WaitForInvocation(AutomationElement workspace, Process host, Task invocation, string outcome, string artifacts, string label)
{
    var closedProgress = false;
    WaitUntil(() =>
    {
        var progress = FindProgressPane(host, "Apply karaoke template");
        if (!closedProgress && progress is not null)
        {
            var close = progress.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button))
                .Cast<AutomationElement>().FirstOrDefault(item => item.Current.Name == "Close" && item.Current.IsEnabled
                    && item.TryGetCurrentPattern(InvokePattern.Pattern, out _));
            if (close is not null)
            {
                SaveEvidence(progress, artifacts, label + "-progress-close");
                UiaDriver.Invoke(close);
                closedProgress = true;
            }
        }
        return invocation.IsCompleted && ReadRunStatus(workspace).Contains(outcome, StringComparison.OrdinalIgnoreCase);
    }, TimeSpan.FromSeconds(25), $"Invocation did not reach {outcome} and release its progress UI");
    Ensure(invocation.Wait(TimeSpan.FromSeconds(5)), "UIA invocation did not return after terminal state");
}

static void WaitForInvocationWithoutPause(AutomationElement workspace, Process host, Task invocation, string artifacts, string label)
{
    var timer = Stopwatch.StartNew();
    var closedProgress = false;
    while (timer.Elapsed < TimeSpan.FromSeconds(25))
    {
        var progress = FindProgressPane(host, "Apply karaoke template");
        if (!closedProgress && progress is not null)
        {
            var close = progress.FindAll(TreeScope.Descendants,
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button)).Cast<AutomationElement>()
                .FirstOrDefault(item => item.Current.Name == "Close" && item.Current.IsEnabled
                    && item.TryGetCurrentPattern(InvokePattern.Pattern, out _));
            if (close is not null)
            {
                SaveEvidence(progress, artifacts, label + "-progress-close");
                UiaDriver.Invoke(close);
                closedProgress = true;
            }
        }
        if (invocation.IsCompleted)
        {
            Ensure(invocation.Wait(TimeSpan.FromSeconds(5)), "No-breakpoint UIA invocation did not return after terminal state");
            Ensure(ReadRunStatus(workspace).Contains("completed", StringComparison.OrdinalIgnoreCase),
                "No-breakpoint Debug returned without a completed terminal status");
            return;
        }
        Thread.Sleep(25);
    }
    throw new TimeoutException("Debug without breakpoints did not complete naturally within its bounded deadline");
}

static class MenuObservation
{
    private static readonly object gate = new();
    private static string? path;
    public sealed record Snapshot(string Menu, string Command, string Name, string AutomationId, int ProcessId,
        int NativeWindowHandle, bool IsEnabled, bool IsOffscreen, bool HasInvokePattern);

    public static void Initialize(string artifacts)
        => path = Path.Combine(artifacts, "menu-invocations.ndjson");

    public static Snapshot Capture(string menu, string command,
        AutomationElement.AutomationElementInformation current, bool hasInvoke)
        => new(menu, command, current.Name, current.AutomationId, current.ProcessId, current.NativeWindowHandle,
            current.IsEnabled, current.IsOffscreen, hasInvoke);

    public static void Record(Snapshot snapshot, string phase, string? error)
    {
        if (path is null) throw new InvalidOperationException("Menu observation path was not initialized");
        var line = JsonSerializer.Serialize(new
        {
            Utc = DateTimeOffset.UtcNow,
            snapshot.Menu,
            snapshot.Command,
            Phase = phase,
            snapshot.Name,
            snapshot.AutomationId,
            snapshot.ProcessId,
            snapshot.NativeWindowHandle,
            snapshot.IsEnabled,
            snapshot.IsOffscreen,
            snapshot.HasInvokePattern,
            Error = error
        });
        lock (gate) File.AppendAllText(path, line + Environment.NewLine);
    }
}

sealed record AssEvent(string Kind, int Layer, int StartMs, int EndMs, string Style, string Name, int MarginL, int MarginR, int MarginV, string Effect, string Text);

sealed record ClipboardReceiptData(uint Sequence, int OwnerProcessId, string Stage);

static class ClipboardReceipt
{
    private static string? path;
    private static uint sequence;

    public static void Initialize(string receiptPath, uint originalSequence)
    {
        path = receiptPath;
        sequence = originalSequence;
    }

    public static void VerifyCurrent() => Ensure(Native.ClipboardSequence() == sequence,
        "Clipboard changed outside this GUI test; preserving external data");

    public static void Record(string stage, int ownerProcessId, uint verifiedSequence)
    {
        if (path is null) throw new InvalidOperationException("Clipboard receipt path was not initialized");
        Native.AssertClipboardState(verifiedSequence, ownerProcessId);
        var pending = path + ".pending";
        File.WriteAllText(pending, JsonSerializer.Serialize(new ClipboardReceiptData(verifiedSequence, ownerProcessId, stage)));
        File.Move(pending, path, overwrite: true);
        Native.AssertClipboardState(verifiedSequence, ownerProcessId);
        sequence = verifiedSequence;
    }

    public static ClipboardReceiptData? Read(string receiptPath) => File.Exists(receiptPath)
        ? JsonSerializer.Deserialize<ClipboardReceiptData>(File.ReadAllText(receiptPath)) : null;

    public static ClipboardReceiptData? ReadForCurrent() => path is null ? null : Read(path);

    private static void Ensure(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

static class ClipboardSafety
{
    public static T OnSta<T>(Func<T> operation) => ClipboardSta.Invoke(operation);

    public static uint SetText(string text) => OnSta(() =>
    {
        Clipboard.SetText(text);
        var verified = Native.ClipboardSequence();
        ClipboardReceipt.Record("test-paste", Environment.ProcessId, verified);
        return verified;
    });

    public static void Restore(DataObject? snapshot, uint expectedSequence) => OnSta(() =>
    {
        Ensure(Native.ClipboardSequence() == expectedSequence, "Clipboard changed before restoration; preserving external data");
        if (snapshot is null) Clipboard.Clear();
        else Clipboard.SetDataObject(snapshot, true);
        return true;
    });

    public static DataObject? Capture()
    {
        var source = Clipboard.GetDataObject();
        if (source is null) return null;
        var snapshot = new DataObject();
        foreach (var format in source.GetFormats(autoConvert: false))
        {
            var value = source.GetData(format, autoConvert: false)
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
        var copy = new StringCollection();
        copy.AddRange(source.Cast<string>().ToArray());
        return copy;
    }

    private static MemoryStream CopyStream(Stream source)
    {
        var copy = new MemoryStream();
        var original = source.CanSeek ? source.Position : (long?)null;
        if (source.CanSeek) source.Position = 0;
        try { source.CopyTo(copy); }
        finally { if (original is not null) source.Position = original.Value; }
        copy.Position = 0;
        return copy;
    }

    private static void Ensure(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}

static class Native
{

    [StructLayout(LayoutKind.Sequential)] private struct Input { public uint Type; public InputUnion Union; }
    [StructLayout(LayoutKind.Explicit)] private struct InputUnion { [FieldOffset(0)] public MouseInput Mouse; [FieldOffset(0)] public KeyboardInput Keyboard; }
    [StructLayout(LayoutKind.Sequential)] private struct MouseInput { public int X, Y; public uint Data, Flags, Time; public nint ExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] private struct KeyboardInput { public ushort VirtualKey, ScanCode; public uint Flags, Time; public nint ExtraInfo; }

    [DllImport("user32.dll", SetLastError = true)] private static extern uint SendInput(uint count, Input[] inputs, int size);
    [DllImport("user32.dll")] private static extern nint GetForegroundWindow();
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(nint hwnd);
    [DllImport("user32.dll")] private static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll", EntryPoint = "WindowFromPoint", ExactSpelling = true)] private static extern nint WindowFromPoint(Point point);
    [DllImport("user32.dll")] private static extern uint GetClipboardSequenceNumber();
    [DllImport("user32.dll")] private static extern nint GetClipboardOwner();
    [DllImport("user32.dll")] private static extern int IsClipboardFormatAvailable(uint format);
    [DllImport("user32.dll", SetLastError = true)] private static extern uint GetWindowThreadProcessId(nint hwnd, out uint processId);
    [DllImport("user32.dll", EntryPoint = "PostMessageW", ExactSpelling = true, SetLastError = true)]
    private static extern int PostMessageW(nint hwnd, uint message, nint wParam, nint lParam);
    [DllImport("user32.dll", EntryPoint = "GetClassNameW", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int GetClassNameW(nint hwnd, [Out] char[] className, int maxCount);
    [DllImport("user32.dll", EntryPoint = "GetParent", ExactSpelling = true)] private static extern nint GetParent(nint hwnd);
    [DllImport("user32.dll", EntryPoint = "GetDlgCtrlID", ExactSpelling = true)] private static extern int GetDlgCtrlID(nint hwnd);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern nint SendMessageTimeoutW(nint hwnd, uint message, nint wParam, [Out] char[] buffer,
        uint flags, uint timeout, out nint result);

    private delegate bool EnumWindowCallback(nint window, nint data);
    [DllImport("user32.dll")] private static extern int EnumWindows(EnumWindowCallback callback, nint data);
    [DllImport("user32.dll")] private static extern int EnumChildWindows(nint root, EnumWindowCallback callback, nint data);
    [DllImport("user32.dll")] private static extern int IsWindowVisible(nint window);
    [DllImport("user32.dll", EntryPoint = "GetWindowTextW", ExactSpelling = true, CharSet = CharSet.Unicode)]
    private static extern int GetWindowTextW(nint window, [Out] char[] text, int capacity);

    public static string WindowClass(nint handle)
    {
        var text = new char[128];
        return new string(text, 0, GetClassNameW(handle, text, text.Length));
    }
    public static string WindowText(nint handle)
    {
        var text = new char[2048];
        return new string(text, 0, GetWindowTextW(handle, text, text.Length));
    }
    public static IReadOnlyList<nint> HostWindows(int processId)
    {
        var result = new HashSet<nint>();
        EnumWindowCallback child = (handle, _) =>
        {
            GetWindowThreadProcessId(handle, out var owner);
            if (owner == processId && IsWindowVisible(handle) != 0) result.Add(handle);
            return true;
        };
        EnumWindows((handle, _) =>
        {
            GetWindowThreadProcessId(handle, out var owner);
            if (owner == processId)
            {
                child(handle, 0);
                EnumChildWindows(handle, child, 0);
            }
            return true;
        }, 0);
        return result.ToArray();
    }
    public static IEnumerable<AutomationElement> ChildElements(AutomationElement parent, string className)
    {
        var current = parent.Current;
        var handles = new List<nint>();
        EnumChildWindows(new nint(current.NativeWindowHandle), (handle, _) =>
        {
            GetWindowThreadProcessId(handle, out var owner);
            if (owner == current.ProcessId && WindowClass(handle) == className) handles.Add(handle);
            return true;
        }, 0);
        foreach (var handle in handles)
        {
            var element = AutomationElement.FromHandle(handle);
            Ensure(element.Current.ProcessId == current.ProcessId && element.Current.NativeWindowHandle == handle,
                "Discovered HWND no longer belongs to the expected host");
            yield return element;
        }
    }

    public static IReadOnlyList<string> DescribeChildWindows(nint parent, int expectedProcessId)
    {
        var lines = new List<string>();
        var visited = 0;
        EnumChildWindows(parent, (handle, _) =>
        {
            if (++visited > 512) return false;
            GetWindowThreadProcessId(handle, out var owner);
            Ensure(owner == (uint)expectedProcessId, "Native evidence discovered a child owned by another process");
            lines.Add($"child\thandle={handle}\tparent={GetParent(handle)}\tprocess={owner}\tclass={WindowClass(handle)}\tcontrol-id={GetDlgCtrlID(handle)}\tvisible={IsWindowVisible(handle) != 0}\ttitle={WindowText(handle)}");
            return true;
        }, 0);
        Ensure(visited <= 512, "Native evidence exceeded its 512-window bound");
        return lines;
    }

    public static IReadOnlyList<(AutomationElement Element, string Text)> ChildTextControls(
        AutomationElement parent,
        Process host,
        string className,
        Func<string, bool> predicate,
        bool allowHidden)
    {
        var current = parent.Current;
        Ensure(current.ProcessId == host.Id && current.NativeWindowHandle != 0,
            "Native child-text root does not belong to the expected host");
        var matches = new List<(AutomationElement, string)>();
        EnumChildWindows(new nint(current.NativeWindowHandle), (handle, _) =>
        {
            GetWindowThreadProcessId(handle, out var owner);
            if (owner != (uint)host.Id || WindowClass(handle) != className || (!allowHidden && IsWindowVisible(handle) == 0))
                return true;
            var element = AutomationElement.FromHandle(handle);
            var observed = element.Current;
            if (!predicate(observed.Name ?? "")) return true;
            Ensure(observed.ProcessId == host.Id && observed.NativeWindowHandle == handle && observed.ClassName == className
                && observed.ControlType == ControlType.Text && (allowHidden || !observed.IsOffscreen),
                "Matched native text control failed its UIA identity or visibility check");
            var text = ReadControlText(handle, host, className);
            Ensure(predicate(text), "Matched native text control changed before its bounded text read");
            matches.Add((element, text));
            return true;
        }, 0);
        return matches;
    }

    public static uint ClipboardSequence() => GetClipboardSequenceNumber();

    public static void AssertClipboardState(uint expectedSequence, int expectedOwnerPid)
    {
        Ensure(ClipboardSequence() == expectedSequence, "Clipboard sequence changed during provenance verification");
        GetWindowThreadProcessId(GetClipboardOwner(), out var owner);
        Ensure(owner == (uint)expectedOwnerPid, $"Clipboard owner PID {owner} differs from expected {expectedOwnerPid}");
        Ensure(ClipboardSequence() == expectedSequence, "Clipboard sequence changed after owner verification");
    }

    public static void PostSystemMenu(nint hwnd, char mnemonic)
    {
        if (PostMessageW(hwnd, 0x0112, 0xF100, mnemonic) == 0)
            throw new InvalidOperationException("Observed menu mnemonic could not be posted to the GUI host");
    }

    public static void PostClose(AutomationElement workspace, Process host)
    {
        var current = workspace.Current;
        Ensure(current.ControlType == ControlType.Window && current.ProcessId == host.Id && current.NativeWindowHandle != 0,
            "Workspace close target is not the exact launched host window");
        var handle = new nint(current.NativeWindowHandle);
        GetWindowThreadProcessId(handle, out var owner);
        Ensure(owner == (uint)host.Id, "Workspace close HWND changed ownership");
        if (PostMessageW(handle, 0x0010, 0, 0) == 0)
            throw new InvalidOperationException("Queued OS close could not be posted to the verified Workspace HWND");
    }

    public static string ReadEditText(AutomationElement edit, Process host)
    {
        var current = edit.Current;
        Ensure(current.ProcessId == host.Id && current.ClassName == "Edit" && current.NativeWindowHandle != 0
            && current.ControlType == ControlType.Document, "Run log is not the observed native Edit in this host");
        var handle = new nint(current.NativeWindowHandle);
        return ReadControlText(handle, host, "Edit");
    }

    public static string ReadStaticText(nint handle, Process host) => ReadControlText(handle, host, "Static");

    private static string ReadControlText(nint handle, Process host, string expectedClass)
    {
        GetWindowThreadProcessId(handle, out var owner);
        Ensure(owner == (uint)host.Id, "Observed text HWND changed ownership");
        var className = new char[64];
        var classLength = GetClassNameW(handle, className, className.Length);
        Ensure(classLength > 0 && classLength < className.Length - 1 && new string(className, 0, classLength) == expectedClass,
            "Observed text HWND no longer has its expected native class");
        const int capacity = 65_536;
        var buffer = new char[capacity];
        if (SendMessageTimeoutW(handle, 0x000D, capacity, buffer, 0x0002, 2000, out var copied) == 0)
            throw new TimeoutException("Bounded WM_GETTEXT for the observed control timed out or failed");
        var length = copied.ToInt64();
        Ensure(length >= 0 && length < capacity - 1, "Control text exceeded the fixed Unicode buffer; refusing truncated text");
        return new string(buffer, 0, (int)length);
    }

    public static void StabilizeOwnedClipboard(int hostPid)
    {
        var receipt = ClipboardReceipt.ReadForCurrent();
        if (receipt is null || receipt.OwnerProcessId != hostPid) return;
        ClipboardReceipt.VerifyCurrent();
        AssertClipboardState(receipt.Sequence, hostPid);
        var text = ClipboardText.ReadUnicodeText(receipt.Sequence, hostPid);
        AssertClipboardState(receipt.Sequence, hostPid);
        _ = ClipboardSafety.SetText(text);
    }

    private static void PrepareFocus(AutomationElement editor, Process host)
    {
        var frame = editor;
        while (frame.Current.ControlType != ControlType.Window)
            frame = TreeWalker.ControlViewWalker.GetParent(frame) ?? throw new InvalidOperationException("STC has no top-level frame");
        UiaDriver.FocusAndVerify(frame, host, TimeSpan.FromSeconds(3));
        editor.SetFocus();
        var timer = Stopwatch.StartNew();
        while (timer.Elapsed < TimeSpan.FromSeconds(3))
        {
            GetWindowThreadProcessId(GetForegroundWindow(), out var owner);
            if (owner == (uint)host.Id && AutomationElement.FocusedElement.Current.NativeWindowHandle == editor.Current.NativeWindowHandle)
                return;
            Thread.Sleep(50);
        }
        throw new TimeoutException("Guarded input did not gain exact host STC focus");
    }

    private static void AssertFocus(AutomationElement editor, Process host)
    {
        GetWindowThreadProcessId(GetForegroundWindow(), out var owner);
        Ensure(owner == (uint)host.Id && AutomationElement.FocusedElement.Current.NativeWindowHandle == editor.Current.NativeWindowHandle,
            "Guarded input stopped after focus left the host STC");
    }

    private static Input Key(ushort virtualKey, bool up) => new()
    {
        Type = 1, Union = new InputUnion { Keyboard = new KeyboardInput { VirtualKey = virtualKey, Flags = up ? 2u : 0u } }
    };

    private static void Send(AutomationElement editor, Process host, params Input[] inputs)
    {
        AssertFocus(editor, host);
        if (SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<Input>()) != inputs.Length)
        {
            var released = SendInput(2, new[] { Key(0x11, true), Key(0x10, true) }, Marshal.SizeOf<Input>());
            throw new InvalidOperationException(released == 2 ? "Guarded input was partial; modifiers released"
                : "Guarded input was partial and modifier release failed");
        }
    }

    private static void Chord(AutomationElement editor, Process host, ushort key) => Send(editor, host,
        Key(0x11, false), Key(key, false), Key(key, true), Key(0x11, true));

    public static void SendCtrlHome(AutomationElement editor, Process host) => Chord(editor, host, 0x24);
    public static void SendKey(AutomationElement editor, Process host, ushort key) => Send(editor, host, Key(key, false), Key(key, true));

    public static void ClickBreakpointMargin(AutomationElement editor, Process host, int line)
    {
        PrepareFocus(editor, host);
        var target = editor.Current;
        Ensure(line > 0 && target.ProcessId == host.Id && target.ControlType == ControlType.Pane
            && target.ClassName == "wxWindow" && target.NativeWindowHandle != 0 && !target.IsOffscreen,
            "Breakpoint margin target is not the visible host STC");
        var hwnd = new nint(target.NativeWindowHandle);
        var dpi = GetDpiForWindow(hwnd);
        Ensure(dpi >= 96, "Breakpoint margin target has no usable DPI");
        var scale = dpi / 96.0;
        var rect = target.BoundingRectangle;
        var point = new Point((int)Math.Round(rect.Left + 8 * scale),
            (int)Math.Round(rect.Top + 10 + (line - 1) * 20));
        Console.WriteLine($"debug.margin-click=dpi:{dpi};rect:{rect};point:{point};requested-line:{line}");
        Ensure(rect.Contains(point.X, point.Y), "Breakpoint margin click point left the source editor");
        var hit = WindowFromPoint(point);
        GetWindowThreadProcessId(hit, out var owner);
        Ensure(hit == hwnd && owner == (uint)host.Id, "Breakpoint margin point does not hit the exact host STC");
        GetWindowThreadProcessId(GetForegroundWindow(), out var foregroundOwner);
        Ensure(foregroundOwner == (uint)host.Id, "Breakpoint margin host is not foreground");

        var left = GetSystemMetrics(76);
        var top = GetSystemMetrics(77);
        var width = GetSystemMetrics(78);
        var height = GetSystemMetrics(79);
        Ensure(width > 1 && height > 1, "Virtual desktop metrics are invalid for breakpoint click");
        Input Mouse(uint action) => new()
        {
            Type = 0,
            Union = new InputUnion { Mouse = new MouseInput
            {
                X = checked((int)Math.Round((point.X - left) * 65535.0 / (width - 1))),
                Y = checked((int)Math.Round((point.Y - top) * 65535.0 / (height - 1))),
                Flags = 0x0001u | 0x4000u | 0x8000u | action
            } }
        };
        void SendMouse(uint action)
        {
            Ensure(SendInput(1, new[] { Mouse(action) }, Marshal.SizeOf<Input>()) == 1,
                "Guarded breakpoint margin click was partial");
        }
        var pressed = false;
        try
        {
            SendMouse(0);
            SendMouse(0x0002);
            pressed = true;
            SendMouse(0x0004);
            pressed = false;
        }
        finally
        {
            if (pressed) _ = SendInput(1, new[] { Mouse(0x0004) }, Marshal.SizeOf<Input>());
        }
    }

    public static void Focus(AutomationElement editor, Process host) => PrepareFocus(editor, host);
    public static void SendKeyToControl(AutomationElement control, Process host, ushort virtualKey)
    {
        var frame = control;
        while (frame.Current.ControlType != ControlType.Window)
            frame = TreeWalker.ControlViewWalker.GetParent(frame)
                ?? throw new InvalidOperationException("Native control has no top-level frame");
        UiaDriver.FocusAndVerify(frame, host, TimeSpan.FromSeconds(3));
        control.SetFocus();
        var timer = Stopwatch.StartNew();
        while (timer.Elapsed < TimeSpan.FromSeconds(3))
        {
            GetWindowThreadProcessId(GetForegroundWindow(), out var owner);
            var focused = AutomationElement.FocusedElement.Current;
            if (owner == (uint)host.Id && focused.ProcessId == host.Id
                && (focused.ControlType == ControlType.Tree || focused.ControlType == ControlType.TreeItem))
                break;
            Thread.Sleep(50);
        }
        Ensure(timer.Elapsed < TimeSpan.FromSeconds(3), "Guarded input did not focus the Lua variables tree");
        var inputs = new[] { Key(virtualKey, false), Key(virtualKey, true) };
        if (SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<Input>()) != inputs.Length)
            throw new InvalidOperationException("Guarded native-control key input was partial");
    }

    public static void SaveShortcut(AutomationElement editor, Process host)
    {
        PrepareFocus(editor, host);
        Chord(editor, host, 'S');
    }
    public static void Undo(AutomationElement editor, Process host)
    {
        PrepareFocus(editor, host);
        Chord(editor, host, 'Z');
    }

    public static string CopyStyledText(AutomationElement editor, Process host)
    {
        PrepareFocus(editor, host);
        ClipboardReceipt.VerifyCurrent();
        var before = ClipboardSequence();
        Chord(editor, host, 'A');
        Chord(editor, host, 'C');
        WaitUntil(() => ClipboardSequence() != before, TimeSpan.FromSeconds(3), "STC copy did not change the clipboard");
        var copiedSequence = ClipboardSequence();
        AssertClipboardState(copiedSequence, host.Id);
        var copied = ClipboardText.ReadUnicodeText(copiedSequence, host.Id);
        AssertClipboardState(copiedSequence, host.Id);
        ClipboardReceipt.Record("host-stc-copy", host.Id, copiedSequence);
        return copied;
    }

    public static void ReplaceWithClipboard(AutomationElement editor, Process host, string text)
    {
        PrepareFocus(editor, host);
        ClipboardReceipt.VerifyCurrent();
        var publishedSequence = ClipboardSafety.SetText(text);
        Ensure(ClipboardSequence() == publishedSequence && IsClipboardFormatAvailable(13) != 0,
            "Published test text is not a stable CF_UNICODETEXT clipboard value");
        Ensure(ClipboardText.ReadUnicodeText(publishedSequence, Environment.ProcessId) == text && ClipboardSequence() == publishedSequence,
            "Published test paste source was not preserved before guarded input");
        AssertClipboardState(publishedSequence, Environment.ProcessId);
        Chord(editor, host, 'A');
        Chord(editor, host, 'V');
        Chord(editor, host, 'A');
        Chord(editor, host, 'C');
        try
        {
            WaitUntil(() => ClipboardSequence() != publishedSequence, TimeSpan.FromSeconds(3), "STC did not prove paste consumption by host copy");
        }
        catch (TimeoutException)
        {
            var sequence = ClipboardSequence();
            GetWindowThreadProcessId(GetClipboardOwner(), out var owner);
            Console.Error.WriteLine($"paste.failure.sequence={sequence};published={publishedSequence};owner={owner};unicode={IsClipboardFormatAvailable(13)}");
            if (sequence == publishedSequence && owner == (uint)Environment.ProcessId)
            {
                var retained = ClipboardText.ReadUnicodeText(sequence, Environment.ProcessId);
                AssertClipboardState(sequence, Environment.ProcessId);
                Console.Error.WriteLine($"paste.failure.retained_length={retained.Length};expected_length={text.Length};retained_equals_expected={retained == text}");
            }
            throw;
        }
        var copiedSequence = ClipboardSequence();
        AssertClipboardState(copiedSequence, host.Id);
        var copied = ClipboardText.ReadUnicodeText(copiedSequence, host.Id);
        AssertClipboardState(copiedSequence, host.Id);
        ClipboardReceipt.Record("host-copy-after-paste", host.Id, copiedSequence);
        Ensure(Normalize(copied) == Normalize(text), "Editor host copy differs from the pasted source");
    }

    private static string Normalize(string text) => text.Replace("\r\n", "\n", StringComparison.Ordinal);

    private static void Ensure(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static void WaitUntil(Func<bool> predicate, TimeSpan timeout, string failure)
    {
        var timer = Stopwatch.StartNew();
        while (timer.Elapsed < timeout)
        {
            if (predicate()) return;
            Thread.Sleep(25);
        }
        throw new TimeoutException(failure);
    }
}

sealed record StepResult(string Name, string Status, string Detail);
sealed record HostIdentity(int ProcessId, string StartTimeUtc);
sealed record PauseStatus(string Reason, int Sequence, string SourcePath, int Line);
sealed record ExpectedOutput(int GeneratedCount, List<ExpectedGenerated> Generated);
sealed record ExpectedGenerated(string Text, int Layer, int StartMs, int EndMs, string Style);
static class DebugInvocation
{
    private static Task? active;
    public static void Set(Task task) => active = task;
    public static bool Wait(TimeSpan timeout) => active is not null && active.Wait(timeout);
}

static class LuaLanguageChild
{
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct ProcessEntry
    {
        public uint Size, Usage, ProcessId;
        public nuint DefaultHeap;
        public uint ModuleId, Threads, ParentId;
        public int Priority;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 260)] public string Executable;
    }
    [DllImport("kernel32.dll", SetLastError = true)] private static extern nint CreateToolhelp32Snapshot(uint flags, uint processId);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern int Process32FirstW(nint snapshot, ref ProcessEntry entry);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern int Process32NextW(nint snapshot, ref ProcessEntry entry);
    [DllImport("kernel32.dll")] private static extern int CloseHandle(nint handle);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern int TerminateProcess(Microsoft.Win32.SafeHandles.SafeProcessHandle process, uint exitCode);

    public static void TerminateVerified(Process host, string expectedExe, string receipt)
    {
        var snapshotUtc = DateTime.UtcNow;
        var snapshot = CreateToolhelp32Snapshot(2, 0);
        if (snapshot == -1) throw new System.ComponentModel.Win32Exception(Marshal.GetLastPInvokeError());
        var candidates = new List<int>();
        try
        {
            var entry = new ProcessEntry { Size = (uint)Marshal.SizeOf<ProcessEntry>() };
            if (Process32FirstW(snapshot, ref entry) == 0) throw new System.ComponentModel.Win32Exception(Marshal.GetLastPInvokeError());
            do
            {
                if (entry.ParentId == host.Id && entry.Executable.Equals("lua-language-server.exe", StringComparison.OrdinalIgnoreCase))
                    candidates.Add((int)entry.ProcessId);
            } while (Process32NextW(snapshot, ref entry) != 0);
        }
        finally { CloseHandle(snapshot); }
        if (candidates.Count != 1) throw new InvalidOperationException($"Expected one real LuaLS child of the test host, found {candidates.Count}");
        using var process = Process.GetProcessById(candidates[0]);
        var retainedHandle = process.SafeHandle;
        var observedExe = process.MainModule?.FileName ?? "";
        if (!Path.GetFullPath(observedExe).Equals(Path.GetFullPath(expectedExe), StringComparison.OrdinalIgnoreCase)
            || process.StartTime < host.StartTime || process.StartTime.ToUniversalTime() > snapshotUtc)
            throw new InvalidOperationException("LuaLS candidate did not match this host's executable and lifetime; preserved");
        var started = process.StartTime.ToUniversalTime();
        if (TerminateProcess(retainedHandle, 19) == 0) throw new System.ComponentModel.Win32Exception(Marshal.GetLastPInvokeError());
        if (!process.WaitForExit(TimeSpan.FromSeconds(5))) throw new TimeoutException("Verified LuaLS child did not stop");
        File.WriteAllText(receipt, JsonSerializer.Serialize(new { ProcessId = process.Id, ParentId = host.Id,
            StartedUtc = started, ExecutableSha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(expectedExe))),
            ExitCode = process.ExitCode }, new JsonSerializerOptions { WriteIndented = true }));
    }
}
