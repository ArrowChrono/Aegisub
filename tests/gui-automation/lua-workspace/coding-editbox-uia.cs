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
using System.Windows.Automation;
using System.Windows.Forms;
using Aegisub.GuiAutomation.Driver;

const string workerFlag = "--coding-editbox-worker";
try { return args.Contains(workerFlag) ? Run(args.Where(arg => arg != workerFlag).ToArray()) : Supervise(args); }
catch (Exception error) { Console.Error.WriteLine(error); return 1; }

static int Supervise(string[] args)
{
    var timer = Stopwatch.StartNew();
    var artifactsArg = Array.IndexOf(args, "--artifacts");
    var exeArg = Array.IndexOf(args, "--exe");
    if (artifactsArg < 0 || artifactsArg + 1 >= args.Length || exeArg < 0 || exeArg + 1 >= args.Length)
        throw new ArgumentException("--exe and --artifacts are required");
    var artifacts = Path.GetFullPath(args[artifactsArg + 1]);
    Ensure(!Directory.Exists(artifacts) && !File.Exists(artifacts), "Use a fresh artifact directory; E17 evidence is never overwritten");
    Directory.CreateDirectory(artifacts);
    string expectedExe;
    uint initialSequence;
    DataObject? clipboard;
    Process? launchedWorker;
    var preflightStep = "validate-executable";
    try
    {
        expectedExe = Path.GetFullPath(args[exeArg + 1]);
        Ensure(File.Exists(expectedExe), "GUI executable is missing");
        preflightStep = "capture-clipboard";
        initialSequence = Native.ClipboardSequence();
        clipboard = ClipboardSafety.OnSta(ClipboardSafety.Capture);
        Ensure(Native.ClipboardSequence() == initialSequence, "Clipboard changed during snapshot");
        preflightStep = "start-worker";
        var executable = Environment.ProcessPath ?? throw new InvalidOperationException("Driver process path is unavailable");
        var start = new ProcessStartInfo(executable) { UseShellExecute = false };
        if (Path.GetFileNameWithoutExtension(executable).Equals("dotnet", StringComparison.OrdinalIgnoreCase))
            start.ArgumentList.Add(Assembly.GetEntryAssembly()?.Location ?? throw new InvalidOperationException("Driver assembly is unavailable"));
        start.ArgumentList.Add(workerFlag);
        start.Environment["AEGISUB_E17_CLIPBOARD_SEQUENCE"] = initialSequence.ToString(System.Globalization.CultureInfo.InvariantCulture);
        foreach (var arg in args) start.ArgumentList.Add(arg);
        launchedWorker = Process.Start(start) ?? throw new InvalidOperationException("Could not start E17 worker");
    }
    catch (Exception error)
    {
        File.WriteAllText(Path.Combine(artifacts, "preflight.json"), JsonSerializer.Serialize(new
        {
            Step = preflightStep, Status = "failed", ErrorType = error.GetType().Name, Error = error.Message,
            WorkerStarted = false, GuiHostStarted = false, InputCreated = false
        }, new JsonSerializerOptions { WriteIndented = true }));
        throw;
    }
    using var worker = launchedWorker;
    var timedOut = false;
    var budget = TimeSpan.FromSeconds(230) - timer.Elapsed;
    if (budget <= TimeSpan.Zero || !worker.WaitForExit(budget))
    {
        timedOut = true;
        worker.Kill(entireProcessTree: true);
        Ensure(worker.WaitForExit(TimeSpan.FromSeconds(5)), "Timed-out E17 worker did not stop");
    }
    var hostLeftRunning = false;
    var readyPath = Path.Combine(artifacts, "ready.json");
    if (File.Exists(readyPath))
    {
        var readyPid = JsonDocument.Parse(File.ReadAllText(readyPath)).RootElement.GetProperty("process_id").GetInt32();
        var identityPath = Path.Combine(artifacts, "host-identity.json");
        var identity = JsonSerializer.Deserialize<HostIdentity>(File.ReadAllText(identityPath));
        Ensure(identity is not null && identity.ProcessId == readyPid, "Ready PID has no matching worker identity");
        try
        {
            using var host = Process.GetProcessById(readyPid);
            if (!host.HasExited)
            {
                hostLeftRunning = true;
                Ensure(host.StartTime.ToUniversalTime().ToString("O") == identity.StartTimeUtc
                    && Path.GetFullPath(host.MainModule?.FileName ?? "").Equals(expectedExe, StringComparison.OrdinalIgnoreCase),
                    "Host PID was reused by an unrelated process");
                host.Kill(entireProcessTree: true);
                Ensure(host.WaitForExit(TimeSpan.FromSeconds(5)), "Verified E17 host did not stop");
            }
        }
        catch (ArgumentException) { }
    }
    var receipt = ClipboardReceipt.Read(Path.Combine(artifacts, "clipboard-receipt.json"));
    var finalSequence = Native.ClipboardSequence();
    string clipboardStatus;
    if (receipt is not null && receipt.Sequence == finalSequence)
    {
        ClipboardSafety.Restore(clipboard, receipt.Sequence);
        clipboardStatus = "restored";
    }
    else if (receipt is null && finalSequence == initialSequence) clipboardStatus = "unchanged";
    else clipboardStatus = "external-or-unreceipted-change-preserved";
    File.WriteAllText(Path.Combine(artifacts, "supervisor.json"), JsonSerializer.Serialize(new
    {
        BudgetSeconds = 240, TimedOut = timedOut, HostLeftRunning = hostLeftRunning,
        WorkerExitCode = worker.ExitCode, ClipboardStatus = clipboardStatus,
        InitialSequence = initialSequence, FinalSequence = finalSequence
    }, new JsonSerializerOptions { WriteIndented = true }));
    Ensure(clipboardStatus != "external-or-unreceipted-change-preserved", "Unreceipted clipboard change was preserved");
    Ensure(!timedOut && !hostLeftRunning, "E17 did not complete normal GUI shutdown within 240 seconds");
    return worker.ExitCode;
}

static int Run(string[] args)
{
    string? exe = null, artifacts = null;
    var mode = "stc";
    for (var i = 0; i < args.Length; ++i)
        switch (args[i])
        {
            case "--exe": exe = Path.GetFullPath(args[++i]); break;
            case "--artifacts": artifacts = Path.GetFullPath(args[++i]); break;
            case "--plain": mode = "plain"; break;
            case "--stc": mode = "stc"; break;
            default: throw new ArgumentException($"Unknown argument: {args[i]}");
        }
    Ensure(exe is not null && File.Exists(exe) && artifacts is not null, "--exe and --artifacts are required");
    var fixtureRoot = Path.Combine("tests", "gui-automation", "lua-workspace", "fixtures");
    var fixture = Path.Combine(fixtureRoot, "coding-editbox.ass");
    var actions = Path.Combine(fixtureRoot, "coding-editbox-actions.lua");
    var verifier = Path.Combine(fixtureRoot, "coding-editbox-verify.lua");
    var driver = Path.Combine("tests", "gui-automation", "lua-workspace", "coding-editbox-uia.cs");
    var scenario = Path.Combine("tests", "gui-automation", "lua-workspace", "coding-editbox-verify.json");
    var sourceHashes = new Dictionary<string, string>();
    foreach (var path in new[] { fixture, actions, verifier, driver, scenario })
        sourceHashes[path.Replace('\\', '/')] = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path)));
    sourceHashes["GUI executable"] = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(exe)));
    var started = DateTimeOffset.UtcNow;
    var results = new List<StepResult>();
    var steps = new[] { "host-ready", "code-selection", "complex-multiline-paste", "invalid-paste-atomic", "single-line-paste", "code-shortcut-workspace", "code-editing-keys", "ordinary-ass", "classification-metadata", "undo-redo-save", "headless-independent-verification", "normal-close" };
    var status = "running";
    var finished = (DateTimeOffset?)null;
    Process? host = null;
    AutomationElement? main = null;
    var input = Path.Combine(artifacts, "input.ass");
    var original = File.ReadAllText(fixture);
    File.Copy(fixture, input);
    File.Copy(actions, Path.Combine(artifacts, "coding-editbox-actions.lua"));
    var profile = Path.Combine(artifacts, "profile");
    Directory.CreateDirectory(Path.Combine(profile, "user"));
    File.WriteAllText(Path.Combine(profile, "user", "config.json"), JsonSerializer.Serialize(new { Subtitle = new Dictionary<string, bool> { ["Use STC"] = mode == "stc" } }));
    ClipboardReceipt.Initialize(Path.Combine(artifacts, "clipboard-receipt.json"), uint.Parse(Environment.GetEnvironmentVariable("AEGISUB_E17_CLIPBOARD_SEQUENCE") ?? throw new InvalidOperationException("Clipboard baseline missing")));
    var complexSource = "local title = [=[漢字\nsecond]=]\n-- complex paste preserves a Lua comment\nlocal sum = 0\nfor i = 1, 4 do\n  sum = sum + i\nend\nreturn title .. ':' .. sum";
    File.WriteAllText(Path.Combine(artifacts, "complex-source.lua"), complexSource, new UTF8Encoding(false));
    try
    {
        var start = new ProcessStartInfo(exe) { UseShellExecute = false, WorkingDirectory = Path.GetDirectoryName(exe)! };
        foreach (var arg in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", artifacts, "--open", input }) start.ArgumentList.Add(arg);
        host = Process.Start(start) ?? throw new InvalidOperationException("Could not start GUI host");
        File.WriteAllText(Path.Combine(artifacts, "host-identity.json"), JsonSerializer.Serialize(new HostIdentity(host.Id, host.StartTime.ToUniversalTime().ToString("O"))));
        Step("host-ready", () =>
        {
            var ready = AutomationProtocol.WaitForReadyArtifact(Path.Combine(artifacts, "ready.json"), host, TimeSpan.FromSeconds(15));
            Ensure(ready.ProcessId == host.Id, "Ready PID mismatch");
            main = UiaDriver.WaitForMainWindow(host, TimeSpan.FromSeconds(15));
            SaveEvidence(main, artifacts, "initial");
        });
        Step("code-selection", () =>
        {
            Select(main!, host, "CodeA", "code once");
            Ensure(WorkspaceButton(main!).Current.IsEnabled, "Lua Workspace button disabled for first code line");
            Ensure(Editor(main!, mode).Current.IsEnabled, "Subtitle editor disabled");
        });
        Step("complex-multiline-paste", () =>
        {
            Paste(Editor(main!, mode), host, complexSource);
            WaitUntil(() => ReadEditor(Editor(main!, mode), host).Contains("second", StringComparison.Ordinal), TimeSpan.FromSeconds(5), "Complex paste was not accepted");
            var expected = Events(original).ToArray();
            expected[0] = expected[0] with { Text = ReadEditor(Editor(main!, mode), host) };
            Save(main!, input, expected);
            var saved = File.ReadAllText(input);
            AssertPhysicalEvents(saved, 5);
            Ensure(Events(saved)[0].Text.IndexOfAny(['\r', '\n']) < 0, "Saved Lua source is not one ASS line");
            File.WriteAllText(Path.Combine(artifacts, "complex-saved.ass"), saved);
        });
        Step("invalid-paste-atomic", () =>
        {
            var before = ReadEditor(Editor(main!, mode), host);
            var savedBefore = File.ReadAllText(input);
            Paste(Editor(main!, mode), host, "local unfinished = (\nreturn unfinished");
            var error = WaitWindow(host, "Cannot paste Lua code", TimeSpan.FromSeconds(5));
            SaveEvidence(error, artifacts, "invalid-paste-diagnostic");
            Ensure(VisibleText(error).Contains("Lua Workspace", StringComparison.OrdinalIgnoreCase), "Invalid Lua error omitted Workspace guidance");
            CloseDialog(error);
            WaitUntil(() => FindWindow(host, "Cannot paste Lua code") is null, TimeSpan.FromSeconds(5),
                "Invalid Lua paste dialog did not close after OK");
            Ensure(ReadEditor(Editor(main!, mode), host) == before, "Rejected Lua paste changed edit-box text");
            Ensure(File.ReadAllText(input) == savedBefore, "Rejected Lua paste changed saved ASS");
        });
        Step("single-line-paste", () =>
        {
            var prior = ReadEditor(Editor(main!, mode), host);
            Paste(Editor(main!, mode), host, " -- tail");
            Ensure(ReadEditor(Editor(main!, mode), host) == " -- tail", "Single-line paste was not literal");
            Native.Focus(Editor(main!, mode), host);
            Native.SendChord(Editor(main!, mode), host, 'Z');
            WaitUntil(() => ReadEditor(Editor(main!, mode), host) == prior, TimeSpan.FromSeconds(5), "Undo did not restore Lua source");
        });
        Step("code-shortcut-workspace", () =>
        {
            var before = File.ReadAllText(input);
            var edit = Editor(main!, mode);
            Native.Focus(edit, host);
            Native.Press(edit, host, 0x0D, shift: true);
            var workspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(8));
            SaveEvidence(workspace, artifacts, "code-workspace");
            var source = FindWorkspaceSource(workspace);
            Ensure(source is not null, "Workspace did not expose its source editor");
            var displayed = ReadEditor(source!, host);
            Ensure(displayed.Contains("for i = 1", StringComparison.Ordinal) && displayed.Contains('\n'), "Workspace did not reconstruct multiline Lua");
            File.WriteAllText(Path.Combine(artifacts, "workspace-source.lua"), displayed);
            ((WindowPattern)workspace.GetCurrentPattern(WindowPattern.Pattern)).Close();
            WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(5), "Workspace did not close");
            Ensure(File.ReadAllText(input) == before, "Opening Workspace changed saved ASS");
            var button = WorkspaceButton(main!);
            var frameBounds = main!.Current.BoundingRectangle;
            var buttonBounds = button.Current.BoundingRectangle;
            Ensure(!button.Current.IsOffscreen && button.Current.IsEnabled
                && !frameBounds.IsEmpty && !buttonBounds.IsEmpty
                && buttonBounds.Width > 0 && buttonBounds.Height > 0
                && frameBounds.Contains(buttonBounds),
                "Lua Workspace convenience button is not fully visible inside the main window");
            UiaDriver.Invoke(button);
            var buttonWorkspace = WaitWindow(host, "Lua Workspace", TimeSpan.FromSeconds(8));
            SaveEvidence(buttonWorkspace, artifacts, "code-workspace-button");
            var buttonSource = FindWorkspaceSource(buttonWorkspace)
                ?? throw new InvalidOperationException("Button-opened Workspace has no source editor");
            Ensure(ReadEditor(buttonSource, host) == displayed,
                "Convenience button opened a different Lua source from Shift+Enter");
            ((WindowPattern)buttonWorkspace.GetCurrentPattern(WindowPattern.Pattern)).Close();
            WaitUntil(() => FindWindow(host, "Lua Workspace") is null, TimeSpan.FromSeconds(5),
                "Button-opened Workspace did not close");
            Ensure(File.ReadAllText(input) == before, "Convenience button changed saved ASS");
        });
        Step("code-editing-keys", () =>
        {
            var edit = Editor(main!, mode);
            var prior = ReadEditor(edit, host);
            Native.Focus(edit, host);
            Native.SendChord(edit, host, 0x23);
            Native.SendChord(edit, host, 0x0D);
            var ctrlEnterActual = Native.CopyStyledText(edit, host).Replace("\r\n", "\n", StringComparison.Ordinal);
            File.WriteAllText(Path.Combine(artifacts, "code-ctrl-enter-observation.json"), JsonSerializer.Serialize(new
            {
                Mode = mode, Expected = prior, Actual = ctrlEnterActual
            }, new JsonSerializerOptions { WriteIndented = true }));
            Ensure(ctrlEnterActual == prior, "Ctrl+Enter changed the single-line code source");
            Native.Focus(edit, host);
            Native.Press(edit, host, 0x23);
            Native.Press(edit, host, 0x09);
            var tabActual = ReadEditor(edit, host);
            File.WriteAllText(Path.Combine(artifacts, "code-tab-observation.json"), JsonSerializer.Serialize(new
            {
                Prior = prior, Actual = tabActual, Expected = prior + "\t", Mode = mode
            }, new JsonSerializerOptions { WriteIndented = true }));
            if (mode == "stc") {
                Ensure(tabActual == prior + "\t", "Code Tab did not append exactly one tab");
                Native.Undo(edit, host);
                WaitUntil(() => ReadEditor(Editor(main!, mode), host) == prior, TimeSpan.FromSeconds(5), "Code Tab undo did not restore source");
                Native.Focus(edit, host);
                Native.SendChord(edit, host, 'A');
                Native.TypeText(edit, host, "{");
                Ensure(ReadEditor(edit, host) == "{", "Lua input used ASS automatic brace closure");
                Native.Undo(edit, host);
                WaitUntil(() => ReadEditor(edit, host) == prior, TimeSpan.FromSeconds(5), "Brace input Undo did not restore code");
                const string block = "return [[A{\\b1}BC]]";
                Paste(edit, host, block);
                WaitUntil(() => ReadEditor(edit, host) == block, TimeSpan.FromSeconds(5), "Lua long string input did not arrive");
                Native.SendCtrlHome(edit, host);
                for (var i = 0; i < 10; ++i) Native.Press(edit, host, 0x27);
                Native.Press(edit, host, 0x27, alt: true);
                Ensure(ReadEditor(edit, host) == block, "ASS block movement modified a Lua string");
                Native.Undo(edit, host);
                WaitUntil(() => ReadEditor(edit, host) == prior, TimeSpan.FromSeconds(5), "Lua block source Undo did not restore code");
                InspectContextMenu(main!, edit, host, false, artifacts, "lua-context-menu");
            }
            else Ensure(tabActual == prior, "Plain editor Tab changed source");
        });
        Step("ordinary-ass", () =>
        {
            Select(main!, host, "Ordinary", "");
            Ensure(!WorkspaceAvailable(main!), "Workspace button enabled for ordinary subtitle");
            SaveEvidence(main!, artifacts, "ordinary-ass-highlighting");
            if (mode == "stc") {
                InspectContextMenu(main!, Editor(main!, mode), host, true, artifacts, "ordinary-context-menu");
                var blockEdit = Editor(main!, mode);
                Paste(blockEdit, host, "A{\\b1}BC");
                WaitUntil(() => ReadEditor(blockEdit, host) == "A{\\b1}BC", TimeSpan.FromSeconds(5), "Ordinary ASS test input was not pasted");
                Native.SendCtrlHome(blockEdit, host);
                Native.Press(blockEdit, host, 0x27);
                Native.Press(blockEdit, host, 0x27, alt: true);
                WaitUntil(() => ReadEditor(blockEdit, host) == "AB{\\b1}C", TimeSpan.FromSeconds(5), "Ordinary ASS block movement regressed");
            }
            Paste(Editor(main!, mode), host, "First\nsecond {\\i1}subtitle");
            WaitUntil(() => ReadEditor(Editor(main!, mode), host) == "First\\Nsecond {\\i1}subtitle", TimeSpan.FromSeconds(5), "Ordinary multiline paste did not preserve exact ASS text");
            var prior = ReadEditor(Editor(main!, mode), host);
            var edit = Editor(main!, mode);
            Native.Focus(edit, host);
            Native.SendChord(edit, host, 0x23);
            Native.Press(edit, host, 0x0D, shift: true);
            var withNewline = prior + "\\N";
            var firstUndoExpected = mode == "stc" ? prior : Events(original)[1].Text;
            var afterShiftEnter = "";
            var firstUndoActual = "";
            var redoActual = "";
            var secondUndoActual = "";
            var restoredActual = "";
            try
            {
                WaitUntil(() => (afterShiftEnter = ReadEditor(edit, host)) == withNewline,
                    TimeSpan.FromSeconds(5), "Ordinary Shift+Enter did not append one ASS newline");
                Native.Undo(edit, host);
                WaitUntil(() => (firstUndoActual = ReadEditor(edit, host)) == firstUndoExpected,
                    TimeSpan.FromSeconds(5), "Ordinary first Undo did not reach its mode-specific exact source");
                Native.Focus(edit, host);
                Native.SendChord(edit, host, 'Y');
                WaitUntil(() => (redoActual = ReadEditor(edit, host)) == withNewline,
                    TimeSpan.FromSeconds(5), "Ordinary Redo did not restore the full paste and literal ASS newline");
                Native.Undo(edit, host);
                WaitUntil(() => (secondUndoActual = ReadEditor(edit, host)) == firstUndoExpected,
                    TimeSpan.FromSeconds(5), "Ordinary second Undo did not restore its exact mode-specific source");
                if (mode == "plain")
                {
                    Paste(edit, host, prior);
                    WaitUntil(() => (restoredActual = ReadEditor(edit, host)) == prior,
                        TimeSpan.FromSeconds(5), "Plain editor did not restore the expected ordinary final source");
                }
            }
            finally
            {
                File.WriteAllText(Path.Combine(artifacts, "ordinary-undo.json"), JsonSerializer.Serialize(new
                {
                    Mode = mode, Original = Events(original)[1].Text, Prior = prior,
                    WithNewlineExpected = withNewline, AfterShiftEnter = afterShiftEnter,
                    FirstUndoExpected = firstUndoExpected, FirstUndoActual = firstUndoActual,
                    RedoExpected = withNewline, RedoActual = redoActual,
                    SecondUndoExpected = firstUndoExpected, SecondUndoActual = secondUndoActual,
                    RestoredExpected = mode == "plain" ? prior : null,
                    RestoredActual = mode == "plain" ? restoredActual : null
                }, new JsonSerializerOptions { WriteIndented = true }));
            }
            SaveEvidence(main!, artifacts, "ordinary-ass-after-paste");
        });
        Step("classification-metadata", () =>
        {
            Select(main!, host, "FalseCode", "code once");
            Ensure(!WorkspaceAvailable(main!), "False-code dialogue enabled Workspace");
            var comment = CommentBox(main!);
            UiaDriver.Toggle(comment);
            WaitUntil(() => WorkspaceAvailable(main!), TimeSpan.FromSeconds(5), "Comment toggle did not reclassify same text");
            UiaDriver.Toggle(CommentBox(main!));
            WaitUntil(() => !WorkspaceAvailable(main!), TimeSpan.FromSeconds(5), "Uncomment did not disable Workspace");
            Select(main!, host, "Template", "template line");
            Ensure(!WorkspaceAvailable(main!), "Template line enabled Workspace");
            Select(main!, host, "CodeB", "code line");
            Ensure(WorkspaceButton(main!).Current.IsEnabled, "Second code line disabled Workspace");
            ChangeEffect(main!, host, mode, "template line");
            WaitUntil(() => !WorkspaceAvailable(main!), TimeSpan.FromSeconds(5), "Effect edit did not switch the unchanged source out of Lua mode");
            ChangeEffect(main!, host, mode, "code line");
            WaitUntil(() => WorkspaceAvailable(main!), TimeSpan.FromSeconds(5), "Effect edit did not restore Lua mode");
            Select(main!, host, "Ordinary", "");
            Select(main!, host, "CodeA", "code once");
            Ensure(WorkspaceButton(main!).Current.IsEnabled, "Repeated code/ordinary switch lost classification");
            SaveEvidence(main!, artifacts, "code-lua-highlighting");
        });
        Step("undo-redo-save", () =>
        {
            var saved = File.ReadAllText(input);
            var edit = Editor(main!, mode);
            var code = ReadEditor(edit, host);
            Paste(edit, host, "return 'temporary undo check'");
            WaitUntil(() => ReadEditor(edit, host) == "return 'temporary undo check'", TimeSpan.FromSeconds(5), "Undo source input did not arrive");
            Native.Undo(edit, host);
            WaitUntil(() => ReadEditor(edit, host) == code, TimeSpan.FromSeconds(5), "Final source Undo failed");
            Native.Focus(edit, host);
            Native.SendChord(edit, host, 'Y');
            WaitUntil(() => ReadEditor(edit, host) == "return 'temporary undo check'", TimeSpan.FromSeconds(5), "Final source Redo failed");
            Native.Undo(edit, host);
            WaitUntil(() => ReadEditor(edit, host) == code, TimeSpan.FromSeconds(5), "Final source second Undo failed");
            var expected = Events(original).ToArray();
            expected[0] = expected[0] with { Text = Events(saved)[0].Text };
            expected[1] = expected[1] with { Text = "First\\Nsecond {\\i1}subtitle" };
            Save(main!, input, expected);
            var final = File.ReadAllText(input);
            AssertPhysicalEvents(final, 5);
            File.WriteAllText(Path.Combine(artifacts, "output.ass"), final);
            Ensure(Events(final)[0].Text == Events(saved)[0].Text, "Final save changed first Lua source");
        });
        Step("headless-independent-verification", () => RunHeadless(exe, artifacts, input, scenario, verifier));
        Step("normal-close", () =>
        {
            Native.StabilizeOwnedClipboard(host.Id);
            ((WindowPattern)main!.GetCurrentPattern(WindowPattern.Pattern)).Close();
            Ensure(host.WaitForExit(TimeSpan.FromSeconds(10)) && host.ExitCode == 0, "GUI host did not close normally with exit code 0");
        });
        status = "passed";
        finished = DateTimeOffset.UtcNow;
        Manifest();
        return 0;
    }
    catch (Exception error)
    {
        if (host is not null && !host.HasExited) TryProcessEvidence(host, artifacts, "failure");
        var failed = steps.FirstOrDefault(name => results.All(step => step.Name != name));
        if (failed is not null) results.Add(new StepResult(failed, "failed", error.Message));
        status = "failed";
        finished = DateTimeOffset.UtcNow;
        Manifest();
        Console.Error.WriteLine(error);
        return 1;
    }
    finally
    {
        if (host is not null && !host.HasExited)
        {
            Native.StabilizeOwnedClipboard(host.Id);
            host.Kill(entireProcessTree: true);
            Ensure(host.WaitForExit(TimeSpan.FromSeconds(5)), "Host did not stop after failure");
        }
        host?.Dispose();
    }

    void Step(string name, Action action)
    {
        action();
        results.Add(new StepResult(name, "passed", ""));
        Manifest();
    }
    void Manifest()
    {
        var complete = results.Concat(steps.Where(name => results.All(step => step.Name != name)).Select(name => new StepResult(name, "not-run", "Preceding step failed")));
        File.WriteAllText(Path.Combine(artifacts, "manifest.json"), JsonSerializer.Serialize(new
        {
            StartedUtc = started, FinishedUtc = finished, BudgetSeconds = 240, Mode = mode,
            SourceHashes = sourceHashes, ExitStatus = status, Steps = complete
        }, new JsonSerializerOptions { WriteIndented = true }));
    }
}

static void Ensure([System.Diagnostics.CodeAnalysis.DoesNotReturnIf(false)] bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
static void WaitUntil(Func<bool> condition, TimeSpan timeout, string message)
{
    var timer = Stopwatch.StartNew();
    while (timer.Elapsed < timeout) { if (condition()) return; Thread.Sleep(75); }
    throw new TimeoutException(message);
}

static AutomationElement? FindWindow(Process host, string prefix)
{
    var roots = AutomationElement.RootElement.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ProcessIdProperty, host.Id));
    var matches = new Dictionary<int, AutomationElement>();
    foreach (AutomationElement root in roots)
    {
        foreach (var item in new[] { root }.Concat(root.FindAll(TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Window)).Cast<AutomationElement>()))
        {
            try
            {
                var current = item.Current;
                if (current.ProcessId == host.Id && !current.IsOffscreen && current.NativeWindowHandle != 0
                    && (current.Name ?? "").StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                    matches.TryAdd(current.NativeWindowHandle, item);
            }
            catch (ElementNotAvailableException) { }
        }
    }
    Ensure(matches.Count <= 1, $"Window prefix {prefix} matched {matches.Count} same-PID visible HWNDs");
    return matches.Count == 1 ? matches.Values.Single() : null;
}


static AutomationElement WaitWindow(Process host, string prefix, TimeSpan timeout)
{
    AutomationElement? result = null;
    WaitUntil(() => (result = FindWindow(host, prefix)) is not null, timeout, $"Window {prefix} was not found");
    return result!;
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


static AutomationElement? FindStyledText(AutomationElement workspace, bool insideNotebook)
{
    var candidates = new List<AutomationElement>();
    foreach (AutomationElement item in workspace.FindAll(TreeScope.Descendants, Condition.TrueCondition))
    {
        try
        {
            var current = item.Current;
            if (current.ProcessId != workspace.Current.ProcessId || current.IsOffscreen
                || current.ControlType != ControlType.Pane || current.ClassName != "wxWindow"
                || current.NativeWindowHandle == 0 || !item.TryGetCurrentPattern(ScrollPattern.Pattern, out _))
                continue;
            if (HasNotebookAncestor(item, workspace) == insideNotebook) candidates.Add(item);
        }
        catch (ElementNotAvailableException) { }
    }
    Ensure(candidates.Count <= 1, $"Workspace exposes {candidates.Count} visible STC controls for notebook={insideNotebook}");
    return candidates.Count == 1 ? candidates[0] : null;
}


static AutomationElement? FindVisibleMenuCommand(Process host, string name)
{
    var roots = AutomationElement.RootElement.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ProcessIdProperty, host.Id));
    foreach (AutomationElement root in roots)
    {
        foreach (AutomationElement item in root.FindAll(TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem)))
        {
            try
            {
                if (!item.Current.IsOffscreen && (item.Current.Name ?? "").Replace("&", "", StringComparison.Ordinal)
                    .Contains(name, StringComparison.OrdinalIgnoreCase)) return item;
            }
            catch (ElementNotAvailableException) { }
        }
    }
    return null;
}


static void InvokeMenu(AutomationElement main, Process host, string name, TimeSpan timeout, string menuName = "Automation")
{
    AutomationElement? menu = null;
    WaitUntil(() => main.Current.IsEnabled && (menu = main.FindAll(TreeScope.Descendants,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem))
        .Cast<AutomationElement>().FirstOrDefault(item => !item.Current.IsOffscreen
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
        var mnemonic = menuName == "Automation" ? 'u' : menuName == "Edit" ? 'e'
            : throw new InvalidOperationException($"Menu {menuName} has no observed mnemonic");
        Native.PostSystemMenu(new nint(main.Current.NativeWindowHandle), mnemonic);
    }
    AutomationElement? command = null;
    WaitUntil(() => (command = FindVisibleMenuCommand(host, name)) is not null, timeout, $"Menu command {name} was unavailable");
    UiaDriver.Invoke(command!);
}


static void SaveEvidence(AutomationElement window, string artifacts, string name)
{
    var lines = new List<string>();
    foreach (var item in new[] { window }.Concat(window.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>()).Take(700))
    {
        try
        {
            var current = item.Current;
            lines.Add($"{current.ControlType.ProgrammaticName}\t{current.Name}\t{current.AutomationId}\t{current.ClassName}\t{current.NativeWindowHandle}\t{current.IsEnabled}\t{current.IsOffscreen}\t{string.Join(',', item.GetSupportedPatterns().Select(pattern => pattern.ProgrammaticName))}");
        }
        catch (ElementNotAvailableException) { lines.Add("<stale UIA element>"); }
    }
    File.WriteAllLines(Path.Combine(artifacts, name + "-uia.txt"), lines);
    var capture = ScreenCapture.SaveWindowPng(window, Path.Combine(artifacts, name + ".png"));
    Ensure(capture.NonBlackPixelCount > 0, $"{name} screenshot was black");
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


static AutomationElement? FindProgressPane(Process host, string exactTitle)
{
    var roots = AutomationElement.RootElement.FindAll(TreeScope.Children, new PropertyCondition(AutomationElement.ProcessIdProperty, host.Id));
    var matches = new List<AutomationElement>();
    foreach (AutomationElement root in roots)
    {
        foreach (var item in new[] { root }.Concat(root.FindAll(TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Pane)).Cast<AutomationElement>()))
        {
            try
            {
                if (item.Current.ProcessId == host.Id && item.Current.ControlType == ControlType.Pane && item.Current.ClassName == "#32770"
                    && item.Current.Name == exactTitle && item.Current.NativeWindowHandle != 0 && !item.Current.IsOffscreen)
                    matches.Add(item);
            }
            catch (ElementNotAvailableException) { }
        }
    }
    Ensure(matches.Count <= 1, "More than one exact progress dialog was found");
    return matches.Count == 1 ? matches[0] : null;
}

static AutomationElement SubsBox(AutomationElement main)
{
    var matches = main.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>()
        .Where(item => item.Current.Name == "SubsEditBox" && !item.Current.IsOffscreen).ToArray();
    Ensure(matches.Length == 1, $"Expected one visible SubsEditBox, found {matches.Length}");
    return matches[0];
}

static AutomationElement Editor(AutomationElement main, string mode)
{
    var box = SubsBox(main);
    var candidates = box.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>()
        .Where(item => !item.Current.IsOffscreen && item.Current.NativeWindowHandle != 0
            && (mode == "stc"
                ? item.Current.Name == "stcwindow" && item.Current.ControlType == ControlType.Pane
                : (item.Current.ControlType == ControlType.Edit || item.Current.ControlType == ControlType.Document)
                    && item.Current.ClassName == "Edit"
                    && Native.IsWritableMultilineEdit(new nint(item.Current.NativeWindowHandle))))
        .ToArray();
    Ensure(candidates.Length == 1, $"Expected one visible {mode} subtitle editor, found {candidates.Length}");
    return candidates[0];
}

static string ReadEditor(AutomationElement editor, Process host)
{
    Ensure(editor.Current.ProcessId == host.Id, "Editor belongs to another process");
    if (editor.TryGetCurrentPattern(ValuePattern.Pattern, out var value))
        return ((ValuePattern)value).Current.Value;
    return Native.CopyStyledText(editor, host).Replace("\r\n", "\n", StringComparison.Ordinal);
}

static void Paste(AutomationElement editor, Process host, string text) => Native.PasteText(editor, host, text);

static AutomationElement? VisibleWorkspaceButton(AutomationElement main)
{
    var matches = SubsBox(main).FindAll(TreeScope.Descendants,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button)).Cast<AutomationElement>()
        .Where(item => !item.Current.IsOffscreen && (item.Current.Name ?? "").Contains("Lua Workspace", StringComparison.OrdinalIgnoreCase))
        .ToArray();
    Ensure(matches.Length <= 1, "More than one visible Lua Workspace convenience button");
    return matches.SingleOrDefault();
}

static bool WorkspaceAvailable(AutomationElement main)
{
    var button = VisibleWorkspaceButton(main);
    return button is not null && button.Current.IsEnabled && button.TryGetCurrentPattern(InvokePattern.Pattern, out _);
}

static AutomationElement WorkspaceButton(AutomationElement main)
    => VisibleWorkspaceButton(main) ?? throw new InvalidOperationException("Code line has no visible Lua Workspace convenience button");

static AutomationElement CommentBox(AutomationElement main)
{
    var checks = SubsBox(main).FindAll(TreeScope.Descendants,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.CheckBox)).Cast<AutomationElement>()
        .Where(item => !item.Current.IsOffscreen && (item.Current.Name ?? "").Replace("&", "", StringComparison.Ordinal)
            .Equals("Comment", StringComparison.OrdinalIgnoreCase)).ToArray();
    Ensure(checks.Length == 1, $"Expected one Comment checkbox, found {checks.Length}");
    return checks[0];
}

static void WaitForMacroIdle(AutomationElement main, Process host, TimeSpan timeout)
{
    WaitUntil(() =>
    {
        if (!main.Current.IsEnabled || FindProgressPane(host, "Automation") is not null) return false;
        var roots = AutomationElement.RootElement.FindAll(TreeScope.Children,
            new PropertyCondition(AutomationElement.ProcessIdProperty, host.Id));
        foreach (AutomationElement root in roots)
        {
            var current = root.Current;
            if (current.NativeWindowHandle != main.Current.NativeWindowHandle && !current.IsOffscreen
                && current.ControlType == ControlType.Window) return false;
        }
        return true;
    }, timeout, "Automation macro invocation left the main window disabled or a modal window visible");
}

static void Select(AutomationElement main, Process host, string actor, string expectedEffect)
{
    InvokeMenu(main, host, "Coding E2E Select " + actor, TimeSpan.FromSeconds(6));
    WaitForMacroIdle(main, host, TimeSpan.FromSeconds(6));
    AutomationElement[] Combos() => SubsBox(main).FindAll(TreeScope.Descendants,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ComboBox))
        .Cast<AutomationElement>().ToArray();
    WaitUntil(() =>
    {
        var combos = Combos();
        return combos.Any(item => item.Current.Name == actor)
            && combos.Any(item => item.Current.Name == (expectedEffect.Length == 0 ? "Effect" : expectedEffect));
    }, TimeSpan.FromSeconds(5), $"Macro did not activate actor {actor} and effect {expectedEffect}");
    if (expectedEffect.Length != 0) return;
    var effectCombo = Combos().Single(item => item.Current.Name == "Effect");
    var fields = effectCombo.FindAll(TreeScope.Descendants,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Edit)).Cast<AutomationElement>()
        .Where(item => !item.Current.IsOffscreen && item.Current.IsEnabled).ToArray();
    Ensure(fields.Length == 1, $"Empty Effect placeholder has {fields.Length} editable fields");
    var field = fields[0];
    Ensure(field.TryGetCurrentPattern(ValuePattern.Pattern, out _), "Effect edit field has no readable ValuePattern");
    Native.Focus(field, host);
    WaitUntil(() => field.TryGetCurrentPattern(ValuePattern.Pattern, out var value)
            && ((ValuePattern)value).Current.Value.Length == 0,
        TimeSpan.FromSeconds(5), "Focused Effect field contains real text rather than an empty placeholder");
}

static void ChangeEffect(AutomationElement main, Process host, string mode, string effect)
{
    var combo = SubsBox(main).FindAll(TreeScope.Descendants,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ComboBox)).Cast<AutomationElement>()
        .SingleOrDefault(item => item.Current.Name is "code line" or "template line")
        ?? throw new InvalidOperationException("Active Effect combo is unavailable");
    Ensure(combo.Current.IsEnabled, "Effect combo is disabled");
    var fields = combo.FindAll(TreeScope.Descendants,
        new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Edit)).Cast<AutomationElement>()
        .Where(item => !item.Current.IsOffscreen && item.Current.IsEnabled).ToArray();
    Ensure(fields.Length == 1, $"Effect combo has {fields.Length} writable edit fields");
    var field = fields[0];
    Ensure(field.TryGetCurrentPattern(ValuePattern.Pattern, out _), "Effect edit field has no ValuePattern");
    Native.Focus(field, host);
    Native.SendChord(field, host, 'A');
    Native.TypeText(field, host, effect);
    WaitUntil(() => field.TryGetCurrentPattern(ValuePattern.Pattern, out var value)
            && ((ValuePattern)value).Current.Value == effect,
        TimeSpan.FromSeconds(5), "Typed Effect value did not reach the native edit field");
    Native.Focus(Editor(main, mode), host);
    WaitUntil(() => combo.Current.Name == effect, TimeSpan.FromSeconds(5), "Effect UI did not accept its edited value");
}

static void Save(AutomationElement main, string path, IReadOnlyList<AssEvent> expected)
{
    var save = UiaDriver.FindEnabledInvokableButtonByAutomationId(main, "Item 5002", "Save the current subtitles")
        ?? throw new InvalidOperationException("Save Subtitles toolbar button unavailable");
    UiaDriver.Invoke(save);
    WaitUntil(() => Events(File.ReadAllText(path)).SequenceEqual(expected), TimeSpan.FromSeconds(8),
        "Physical ASS save did not persist every expected business field");
}

static List<AssEvent> Events(string ass) => ass.Split('\n').Select(line => line.TrimEnd('\r'))
    .Where(line => line.StartsWith("Comment:", StringComparison.Ordinal) || line.StartsWith("Dialogue:", StringComparison.Ordinal))
    .Select(line =>
    {
        var separator = line.IndexOf(':');
        var fields = line[(separator + 1)..].TrimStart().Split(',', 10);
        Ensure(fields.Length == 10, "ASS event has unexpected field count");
        return new AssEvent(line[..separator], int.Parse(fields[0]), fields[1], fields[2], fields[3], fields[4],
            int.Parse(fields[5]), int.Parse(fields[6]), int.Parse(fields[7]), fields[8], fields[9]);
    }).ToList();

static void AssertPhysicalEvents(string ass, int count)
{
    var lines = ass.Split('\n').Select(line => line.TrimEnd('\r')).ToArray();
    var section = Array.IndexOf(lines, "[Events]");
    Ensure(section >= 0, "Saved ASS lacks Events section");
    var physical = 0;
    foreach (var line in lines.Skip(section + 1))
    {
        if (line.StartsWith("[", StringComparison.Ordinal)) break;
        if (line.Length == 0 || line.StartsWith("Format:", StringComparison.Ordinal)) continue;
        Ensure(line.StartsWith("Comment:", StringComparison.Ordinal) || line.StartsWith("Dialogue:", StringComparison.Ordinal),
            "Saved ASS has an orphan physical continuation line");
        ++physical;
    }
    Ensure(physical == count, $"Saved ASS has {physical} physical events, expected {count}");
}

static AutomationElement? FindWorkspaceSource(AutomationElement workspace) => FindStyledText(workspace, false);

static string VisibleText(AutomationElement window)
{
    var lines = window.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>()
        .Where(item => !item.Current.IsOffscreen && item.Current.ControlType == ControlType.Text)
        .Select(item => item.Current.Name ?? "").ToArray();
    return string.Join("\n", lines);
}

static void CloseDialog(AutomationElement dialog)
{
    var button = UiaDriver.FindEnabledInvokableButton(dialog, "OK")
        ?? throw new InvalidOperationException("Expected error dialog has no enabled OK button");
    UiaDriver.Invoke(button);
}

static void InspectContextMenu(AutomationElement main, AutomationElement editor, Process host,
    bool ordinary, string artifacts, string artifactName)
{
    Native.Focus(editor, host);
    Native.Press(editor, host, 0x79, shift: true);
    IReadOnlyList<string>? names = null;
    WaitUntil(() =>
    {
        names = AutomationElement.RootElement.FindAll(TreeScope.Children,
                new PropertyCondition(AutomationElement.ProcessIdProperty, host.Id))
            .Cast<AutomationElement>().SelectMany(root => root.FindAll(TreeScope.Descendants,
                new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.MenuItem)).Cast<AutomationElement>())
            .Where(item => !item.Current.IsOffscreen && item.Current.ProcessId == host.Id)
            .Select(item => item.Current.Name ?? "").ToArray();
        return names.Any(name => name.Replace("&", "", StringComparison.Ordinal).Equals("Select All", StringComparison.OrdinalIgnoreCase));
    }, TimeSpan.FromSeconds(3), "Editor context menu did not open");
    File.WriteAllLines(Path.Combine(artifacts, artifactName + ".txt"), names!);
    var hasSplit = names!.Any(name => name.Contains("Split at cursor", StringComparison.OrdinalIgnoreCase));
    Ensure(hasSplit == ordinary, "ASS split context actions did not follow ordinary/code classification");
    var hasSpell = names!.Any(name => name.Contains("Spell checker language", StringComparison.OrdinalIgnoreCase));
    Ensure(hasSpell == ordinary, "Spell checker context action did not follow ordinary/code classification");
    Native.DismissMenu(host);
    WaitUntil(() => FindVisibleMenuCommand(host, "Select All") is null,
        TimeSpan.FromSeconds(3), "Context menu did not dismiss");
}

static void RunHeadless(string exe, string artifacts, string subtitle, string scenario, string verifier)
{
    var headless = Path.Combine(artifacts, "headless");
    Directory.CreateDirectory(headless);
    var verifierCopy = Path.Combine(headless, "coding-editbox-verify.lua");
    File.Copy(verifier, verifierCopy);
    File.Copy(scenario, Path.Combine(headless, "coding-editbox-verify.json"));
    var output = Path.Combine(headless, "output.ass");
    var processInfo = new ProcessStartInfo(exe)
    {
        UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden,
        RedirectStandardOutput = true, RedirectStandardError = true
    };
    foreach (var arg in new[] { "--headless", "run", "--scenario", scenario,
        "--input", "script=" + verifierCopy, "--input", "subtitle=" + subtitle,
        "--input", "output=" + output, "--artifacts", Path.Combine(headless, "run") })
        processInfo.ArgumentList.Add(arg);
    using var process = Process.Start(processInfo) ?? throw new InvalidOperationException("Headless verifier did not start");
    var stdout = process.StandardOutput.ReadToEndAsync();
    var stderr = process.StandardError.ReadToEndAsync();
    var timedOut = !process.WaitForExit(TimeSpan.FromSeconds(60));
    if (timedOut)
    {
        process.Kill(entireProcessTree: true);
        Ensure(process.WaitForExit(TimeSpan.FromSeconds(5)), "Headless verifier did not stop after timeout");
    }
    File.WriteAllText(Path.Combine(headless, "stdout.log"), stdout.GetAwaiter().GetResult());
    File.WriteAllText(Path.Combine(headless, "stderr.log"), stderr.GetAwaiter().GetResult());
    File.WriteAllText(Path.Combine(headless, "execution.json"), JsonSerializer.Serialize(new
    {
        TimedOut = timedOut, TimeoutSeconds = 60, process.ExitCode,
        InputSha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(subtitle))),
        OutputExists = File.Exists(output)
    }, new JsonSerializerOptions { WriteIndented = true }));
    Ensure(!timedOut && process.ExitCode == 0 && File.Exists(output),
        "Independent headless Lua verification did not complete successfully");
    Ensure(Events(File.ReadAllText(output)).SequenceEqual(Events(File.ReadAllText(subtitle))),
        "Headless verifier unexpectedly changed a saved ASS event");
    File.WriteAllText(Path.Combine(headless, "assertion.json"), JsonSerializer.Serialize(new
    {
        Status = "passed", ExpectedLuaResult = "漢字\nsecond:10", EventCount = Events(File.ReadAllText(output)).Count,
        InputSha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(subtitle))),
        OutputSha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(output))),
        VerifierSha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(verifierCopy)))
    }, new JsonSerializerOptions { WriteIndented = true }));
}


sealed record StepResult(string Name, string Status, string Detail);
sealed record HostIdentity(int ProcessId, string StartTimeUtc);
sealed record AssEvent(string Kind, int Layer, string Start, string End, string Style, string Actor, int MarginL, int MarginR, int MarginV, string Effect, string Text);
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

    public static string ReadText() => OnSta(() => Clipboard.GetText());

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
    [DllImport("user32.dll")] private static extern uint GetClipboardSequenceNumber();
    [DllImport("user32.dll")] private static extern nint GetClipboardOwner();
    [DllImport("user32.dll")] private static extern int IsClipboardFormatAvailable(uint format);
    [DllImport("user32.dll", SetLastError = true)] private static extern uint GetWindowThreadProcessId(nint hwnd, out uint processId);
    [DllImport("user32.dll", EntryPoint = "PostMessageW", ExactSpelling = true, SetLastError = true)]
    private static extern int PostMessageW(nint hwnd, uint message, nint wParam, nint lParam);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW", ExactSpelling = true, SetLastError = true)]
    private static extern nint GetWindowLongPtrW(nint hwnd, int index);

    public static uint ClipboardSequence() => GetClipboardSequenceNumber();

    public static bool IsWritableMultilineEdit(nint hwnd)
    {
        var style = GetWindowLongPtrW(hwnd, -16).ToInt64();
        return (style & 0x0004) != 0 && (style & 0x0800) == 0;
    }

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

    public static void StabilizeOwnedClipboard(int hostPid)
    {
        var receipt = ClipboardReceipt.ReadForCurrent();
        if (receipt is null || receipt.OwnerProcessId != hostPid) return;
        ClipboardReceipt.VerifyCurrent();
        AssertClipboardState(receipt.Sequence, hostPid);
        var text = ClipboardSafety.ReadText();
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

    private static Input Key(ushort virtualKey, bool up)
    {
        var extended = virtualKey is >= 0x21 and <= 0x28 or 0x2D or 0x2E;
        return new Input
        {
            Type = 1,
            Union = new InputUnion { Keyboard = new KeyboardInput
            {
                VirtualKey = virtualKey, Flags = (up ? 2u : 0u) | (extended ? 1u : 0u)
            } }
        };
    }

    private static void Send(AutomationElement editor, Process host, params Input[] inputs)
    {
        AssertFocus(editor, host);
        if (SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<Input>()) != inputs.Length)
        {
            var releases = new[] { Key(0x11, true), Key(0x10, true), Key(0x12, true) };
            var released = SendInput((uint)releases.Length, releases, Marshal.SizeOf<Input>());
            throw new InvalidOperationException(released == releases.Length ? "Guarded input was partial; Ctrl, Shift, and Alt released"
                : "Guarded input was partial and modifier release failed");
        }
    }

    public static void SendChord(AutomationElement editor, Process host, ushort key) => Send(editor, host,
        Key(0x11, false), Key(key, false), Key(key, true), Key(0x11, true));

    public static void SendCtrlHome(AutomationElement editor, Process host) => SendChord(editor, host, 0x24);

    public static void Press(AutomationElement editor, Process host, ushort key, bool shift = false, bool alt = false)
    {
        var inputs = new List<Input>();
        if (shift) inputs.Add(Key(0x10, false));
        if (alt) inputs.Add(Key(0x12, false));
        inputs.Add(Key(key, false));
        inputs.Add(Key(key, true));
        if (alt) inputs.Add(Key(0x12, true));
        if (shift) inputs.Add(Key(0x10, true));
        Send(editor, host, inputs.ToArray());
    }

    public static void DismissMenu(Process host)
    {
        GetWindowThreadProcessId(GetForegroundWindow(), out var owner);
        Ensure(owner == (uint)host.Id, "Context menu lost the launched host's foreground ownership");
        var inputs = new[] { Key(0x1B, false), Key(0x1B, true) };
        Ensure(SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<Input>()) == inputs.Length,
            "Escape could not dismiss the host context menu");
    }

    public static void TypeText(AutomationElement editor, Process host, string text)
    {
        PrepareFocus(editor, host);
        foreach (var ch in text)
        {
            var down = new Input { Type = 1, Union = new InputUnion { Keyboard = new KeyboardInput { ScanCode = ch, Flags = 4 } } };
            var up = new Input { Type = 1, Union = new InputUnion { Keyboard = new KeyboardInput { ScanCode = ch, Flags = 6 } } };
            Send(editor, host, down, up);
        }
    }

    public static void PasteText(AutomationElement editor, Process host, string text)
    {
        PrepareFocus(editor, host);
        ClipboardReceipt.VerifyCurrent();
        var published = ClipboardSafety.SetText(text);
        Ensure(ClipboardSequence() == published && IsClipboardFormatAvailable(13) != 0,
            "Published test text is not a stable CF_UNICODETEXT value");
        Ensure(ClipboardSafety.ReadText() == text, "Test clipboard value changed before guarded paste");
        AssertClipboardState(published, Environment.ProcessId);
        SendChord(editor, host, 'A');
        SendChord(editor, host, 'V');
    }

    public static void Focus(AutomationElement editor, Process host) => PrepareFocus(editor, host);
    public static void Undo(AutomationElement editor, Process host)
    {
        PrepareFocus(editor, host);
        SendChord(editor, host, 'Z');
    }

    public static string CopyStyledText(AutomationElement editor, Process host)
    {
        PrepareFocus(editor, host);
        ClipboardReceipt.VerifyCurrent();
        var before = ClipboardSequence();
        SendChord(editor, host, 'A');
        SendChord(editor, host, 'C');
        WaitUntil(() => ClipboardSequence() != before, TimeSpan.FromSeconds(3), "STC copy did not change the clipboard");
        var copiedSequence = ClipboardSequence();
        AssertClipboardState(copiedSequence, host.Id);
        var copied = ClipboardSafety.ReadText();
        AssertClipboardState(copiedSequence, host.Id);
        ClipboardReceipt.Record("host-stc-copy", host.Id, copiedSequence);
        return copied;
    }

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
