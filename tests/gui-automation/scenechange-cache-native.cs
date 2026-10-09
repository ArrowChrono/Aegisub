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
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

if (args.Length != 4 || args[3] is not ("cancel" or "publication" or "publication-cancel" or "ownership"))
{
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/scenechange-cache-native.cs -- EXE VIDEO ARTIFACTS cancel|publication|publication-cancel|ownership");
    return 2;
}
try { return PrivateDesktop.Run(args, Path.GetFullPath(args[2]), isolation => new Scenario(args, isolation).Run()); }
catch (Exception error) { Console.Error.WriteLine(error); return 1; }

sealed class Scenario(string[] args, PrivateDesktop isolation)
{
    readonly string exe = Path.GetFullPath(args[0]), artifacts = Path.GetFullPath(args[2]);
    readonly string video = args[3] == "ownership" ? Path.GetFullPath(Path.Combine(args[2], Path.GetFileName(args[1]))) : Path.GetFullPath(args[1]);
    readonly List<object> checks = [];
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    Process? app;
    nint main;
    string cache = "", manifest = "";
    int[] generated = [];
    static readonly int[] Sentinel = [0, 7, 31, 101];

    public int Run()
    {
        Directory.CreateDirectory(artifacts); isolation.RecordDriver();
        if (args[3] == "ownership") File.Copy(args[1], video);
        using var log = new StreamWriter(Path.Combine(artifacts, "driver.log")) { AutoFlush = true };
        Console.SetOut(log); Console.SetError(log);
        using var watchdog = new Timer(_ => {
            try { Write("result.json", new { passed = false, error = "180-second total deadline exceeded", checks }); }
            finally {
                try { if (app is not null) isolation.Stop(app); }
                finally { Environment.Exit(124); }
            }
        }, null, 180_000, Timeout.Infinite);
        File.Copy("tests/gui-automation/scenechange-cache-native.cs", Path.Combine(artifacts, "producer.cs"));
        Write("inputs.json", new {
            scenario = args[3], backend = "wwxd",
            private_desktop = true, global_input = false, clipboard = false, total_timeout_seconds = 180
        });
        try
        {
            Start(); GenerateAndReuse();
            if (args[3] == "ownership") Ownership();
            else { StageSentinel(); if (args[3] == "cancel") Cancel(); else Publication(args[3] == "publication-cancel"); }
            Write("result.json", new { passed = true, checks }); return 0;
        }
        catch (Exception error)
        {
            string? captureError = null;
            try { if (app is not null && !app.HasExited) Capture(main, "failure"); }
            catch (Exception capture) { captureError = capture.ToString(); }
            Write("result.json", new { passed = false, checks, error = error.ToString(), capture_error = captureError });
            Console.Error.WriteLine(error); return 1;
        }
        finally { if (app is not null) { isolation.Stop(app); app.Dispose(); } }
    }

    void Write(string file, object value) => File.WriteAllText(Path.Combine(artifacts, file), JsonSerializer.Serialize(value, Json));
    bool Check(bool value, string description)
    {
        checks.Add(new { description, passed = value }); Console.WriteLine(description + ": " + value); return value;
    }
    void Require(bool value, string description)
    {
        if (!Check(value, description)) throw new InvalidOperationException(description);
    }
    void Wait(Func<bool> predicate, string description, int seconds = 10)
    {
        var watch = Stopwatch.StartNew();
        while (watch.ElapsedMilliseconds < seconds * 1000)
        {
            isolation.VerifyCurrent();
            if (app is { HasExited: true }) throw new InvalidOperationException("Host exited: " + description);
            try { if (predicate()) return; }
            catch (Win32Exception error) when (error.NativeErrorCode == 1400) { }
            Thread.Sleep(30);
        }
        throw new TimeoutException(description);
    }
    string Text(nint window) => isolation.ReadText(window, app!);
    nint[] Children(nint window) => Native.Children(window).Where(Native.IsWindowVisible).ToArray();
    nint[] Windows() => isolation.Windows(app!).Where(Native.IsWindowVisible).ToArray();
    nint Window(string title) => Windows().SingleOrDefault(h => Text(h) == title);
    nint Progress() => Windows().SingleOrDefault(h => h != main && PrivateDesktop.ClassName(h) == "#32770"
        && Children(h).Any(c => PrivateDesktop.ClassName(c) == "msctls_progress32"));
    nint Control(nint parent, string label) => Children(parent).Single(h => Text(h).Replace("&", "") == label);
    void Action(string action, string target)
        => File.AppendAllText(Path.Combine(artifacts, "actions.ndjson"), JsonSerializer.Serialize(new { action, target }) + Environment.NewLine);
    void Click(nint parent, string label)
    {
        var control = Control(parent, label); isolation.VerifyWindow(control, app!);
        if (!Native.IsWindowEnabled(control)) throw new InvalidOperationException("Cannot click a disabled native button: " + label);
        Action("button", label); ObservedWindowsCommand.PostButtonClick(control, app!.Id);
    }
    void Command(string label) => Menu("Video", label);
    void Menu(string menu, string label)
    {
        isolation.VerifyWindow(main, app!);
        long enabledSince = 0;
        Wait(() => {
            if (!Native.IsWindowEnabled(main)) { enabledSince = 0; return false; }
            if (enabledSince == 0) enabledSince = Stopwatch.GetTimestamp();
            return Stopwatch.GetElapsedTime(enabledSince).TotalMilliseconds >= 100;
        }, "Native main window is stably enabled before menu dispatch");
        var id = ObservedWindowsCommand.FindMenuCommand(main, app!.Id, menu, label);
        Action("menu", menu + "/" + label); ObservedWindowsCommand.PostMenuCommand(main, app.Id, id);
    }
    void Capture(nint window, string label)
    {
        Write(label + "-windows.json", Windows().Select(h => new {
            handle = h.ToString("X"), text = Text(h), class_name = PrivateDesktop.ClassName(h),
            children = Children(h).Select(c => new { handle = c.ToString("X"), text = Text(c), class_name = PrivateDesktop.ClassName(c), id = Native.GetDlgCtrlID(c) })
        }));
        isolation.VerifyWindow(window, app!);
        ScreenCapture.SaveWindowPng(window, Path.Combine(artifacts, label + ".png"));
    }
    void FileDialog(string title, string path)
    {
        nint dialog = 0;
        Wait(() => (dialog = Window(title)) != 0, "Native file dialog: " + title);
        nint edit = 0;
        Wait(() => (edit = Children(dialog).SingleOrDefault(h => PrivateDesktop.ClassName(h) == "Edit" && Native.GetDlgCtrlID(h) is 1148 or 1001)) != 0, "Native file name edit is ready");
        isolation.VerifyWindow(edit, app!); Native.SetText(edit, path);
        var button = Children(dialog).Single(h => PrivateDesktop.ClassName(h) == "Button" && Native.GetDlgCtrlID(h) == 1);
        isolation.VerifyWindow(button, app!);
        Action("file-dialog", title + "/" + Path.GetFileName(path));
        ObservedWindowsCommand.PostButtonClick(button, app!.Id);
        Wait(() => !Windows().Contains(dialog), "Native file dialog accepted: " + title);
    }
    int[] Export(string label)
    {
        var path = Path.Combine(artifacts, label + ".key.txt");
        Command("Save Keyframes..."); FileDialog("Save keyframes file", path);
        Wait(() => File.Exists(path), "Current in-memory keyframes saved: " + label);
        return Parse(path);
    }
    static int[] Parse(string path) => File.ReadAllLines(path).Skip(2).Where(line => line.Length > 0).Select(int.Parse).ToArray();
    void OpenVideo()
    {
        Command("Open Video..."); FileDialog("Open video file", video);
    }
    nint Confirm(bool existing, string label)
    {
        nint prompt = 0;
        Wait(() => (prompt = Window("Generate keyframes?")) != 0, "SceneChange generation confirmation", 30);
        Capture(prompt, label + "-confirmation");
        var elements = AutomationElement.FromHandle(prompt).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
        Write(label + "-confirmation-uia.json", elements.Select(e => new { name = e.Current.Name, id = e.Current.AutomationId, type = e.Current.ControlType.ProgrammaticName, handle = e.Current.NativeWindowHandle }));
        var expected = existing ? "A cached SceneChange keyframe file already exists" : "No cached SceneChange keyframe file was found";
        Require(elements.Any(e => e.Current.Name.Contains(expected, StringComparison.Ordinal)), "Confirmation identifies " + (existing ? "existing cache" : "missing cache"));
        var yes = elements.Single(e => e.Current.ControlType == ControlType.Button && e.Current.AutomationId == "CommandButton_6");
        var button = (nint)yes.Current.NativeWindowHandle; isolation.VerifyWindow(button, app!);
        Action("button", "Generate keyframes?/Yes"); Native.Post(button, 0xF5);
        Wait(() => !Windows().Contains(prompt), "Generation confirmation accepted");
        nint progress = 0;
        Wait(() => (progress = Progress()) != 0, "Actual SceneChange progress dialog appeared");
        return progress;
    }
    bool Complete(string label)
    {
        nint error = 0;
        var closedProgressLog = false;
        Wait(() => {
            error = Window("Error generating keyframes");
            if (error != 0) return true;
            var progress = Progress();
            if (progress != 0 && !closedProgressLog && Children(progress).Any(h => Text(h).Replace("&", "") == "Close"))
            {
                Capture(progress, label + "-progress-log"); Click(progress, "Close"); closedProgressLog = true;
            }
            return progress == 0 && Native.IsWindowEnabled(main);
        }, "SceneChange operation reached explicit error or idle", 75);
        if (error != 0)
        {
            Capture(error, label + "-error");
            var elements = AutomationElement.FromHandle(error).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
            var text = elements.Select(e => e.Current.Name).Where(s => s.Length > 0).ToArray();
            Write(label + "-error.json", new { title = Text(error), text, closed_progress_log = closedProgressLog });
            var button = Control(error, "OK"); isolation.VerifyWindow(button, app!);
            Action("button", "Error generating keyframes/OK"); Native.Post(button, 0xF5);
            Wait(() => !Windows().Contains(error) && Native.IsWindowEnabled(main), "Generation error acknowledged");
        }
        Capture(main, label + "-idle"); return error != 0;
    }
    void Start()
    {
        var root = Path.Combine(artifacts, "host"); var profile = Path.Combine(root, "profile");
        Directory.CreateDirectory(Path.Combine(profile, "user"));
        File.WriteAllText(Path.Combine(profile, "user", "config.json"), """
            {"Subtitle":{"Use STC":false},"Video":{"Provider":"LsmasNative","Open Audio":false},"Provider":{"SceneChange":{"Backend":"wwxd"}},"App":{"First Start":false,"Language":"en_US","Auto":{"Backup":false,"Check For Updates":false,"Load Linked Files":0,"Save":false,"Save on Every Change":false}}}
            """.Replace("\"Load Linked Files\":0", "\"Load Linked Files\":" + (args[3] == "ownership" ? 1 : 0)));
        var start = new ProcessStartInfo(exe) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var arg in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", root }) start.ArgumentList.Add(arg);
        app = isolation.Start(start, args[3]);
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(root, "ready.json"), app, TimeSpan.FromSeconds(20));
        main = isolation.MainWindow(app, 10);
        manifest = Path.Combine(profile, "local", "scenechangekeyframes", "manifest.json");
    }
    void GenerateAndReuse()
    {
        OpenVideo(); Confirm(false, "initial");
        Require(!Complete("initial"), "Initial SceneChange generation succeeds without error");
        var directory = Path.GetDirectoryName(manifest)!;
        cache = Directory.GetFiles(directory, "*.kf.txt").Single();
        Require(Path.GetFileName(cache).EndsWith("_wwxd.kf.txt", StringComparison.Ordinal), "Generated cache identity uses the selected wwxd backend");
        generated = Parse(cache);
        Require(generated.Length >= 4 && generated[0] == 0 && generated.SequenceEqual(generated.Distinct().Order()), "Real multi-scene scan publishes at least four ordered keyframes");
        Require(generated.SequenceEqual(Export("initial-memory")), "Initial published cache equals current in-memory keyframes");
        Require(File.Exists(manifest), "Initial publication records a manifest");
        using (var document = JsonDocument.Parse(File.ReadAllText(manifest)))
            Require(document.RootElement.EnumerateArray().Any(entry => entry.GetProperty("keyframes").GetString() == Path.GetFileName(cache)
                && entry.GetProperty("video_name").GetString() == Path.GetFileName(video)), "Manifest identifies generated cache and source video");
        Snapshot("initial");
        var cacheBytes = File.ReadAllBytes(cache); var manifestBytes = File.ReadAllBytes(manifest);
        Command("Close Video"); Wait(() => Native.IsWindowEnabled(main), "Video closed");
        OpenVideo();
        Wait(() => Window("Generate keyframes?") != 0 || (Native.IsWindowEnabled(main) && Progress() == 0), "Reopening video completed", 30);
        Require(Window("Generate keyframes?") == 0, "Reopening reuses cache without asking to regenerate");
        Require(generated.SequenceEqual(Export("reopened-memory")), "Reopening restores the complete cached keyframe list");
        Require(File.ReadAllBytes(cache).SequenceEqual(cacheBytes) && File.ReadAllBytes(manifest).SequenceEqual(manifestBytes), "Cache reuse preserves cache and manifest bytes");
        Command("Show Video Details"); nint details = 0;
        Wait(() => (details = Window("Video Details")) != 0, "Native Video Details opened");
        Require(Children(details).Any(h => Text(h) == "LsmasNative"), "Actual loaded decoder is LsmasNative");
        Capture(details, "decoder"); Click(details, "OK"); Wait(() => !Windows().Contains(details), "Video Details dismissed");
    }
    AutomationElement[] Elements(nint window) => AutomationElement.FromHandle(window).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
    string Backend()
    {
        using var config = JsonDocument.Parse(File.ReadAllText(Path.Combine(artifacts, "host", "profile", "user", "config.json")));
        return config.RootElement.GetProperty("Provider").GetProperty("SceneChange").GetProperty("Backend").GetString()!;
    }
    void SwitchBackend(string backend, string label)
    {
        var previous = Backend(); Require(previous != backend, "Preferences switch changes backend: " + label);
        Menu("View", "Options..."); nint dialog = 0;
        Wait(() => (dialog = Window("Preferences")) != 0, "Preferences opened");
        var advanced = Elements(dialog).Single(e => e.Current.ControlType == ControlType.TreeItem && e.Current.Name == "Advanced");
        ((ExpandCollapsePattern)advanced.GetCurrentPattern(ExpandCollapsePattern.Pattern)).Expand();
        var page = advanced.FindFirst(TreeScope.Children, new PropertyCondition(AutomationElement.NameProperty, "Video"));
        ((SelectionItemPattern)page.GetCurrentPattern(SelectionItemPattern.Pattern)).Select();
        nint grid = 0;
        Wait(() => {
            grid = (nint)(Elements(dialog).SingleOrDefault(e => e.Current.Name == "wxPropertyGrid" && e.Current.NativeWindowHandle != 0)?.Current.NativeWindowHandle ?? 0);
            return grid != 0;
        }, "Advanced Video property grid is ready");
        isolation.VerifyWindow(grid, app!);
        var gridPath = Path.Combine(artifacts, label + "-grid.png"); ScreenCapture.SaveWindowPng(grid, gridPath);
        using (var bitmap = new System.Drawing.Bitmap(gridPath))
        {
            var x = bitmap.Width * 3 / 4; var y = bitmap.Height - 10;
            while (y > 30 && bitmap.GetPixel(x, y).R > 245 && bitmap.GetPixel(x, y).G > 245 && bitmap.GetPixel(x, y).B > 245) --y;
            Require(y > 30, "Rendered final backend row has an observable lower border: " + label);
            Write(label + "-locator.json", new { grid_width = bitmap.Width, grid_height = bitmap.Height, click_x = x, click_y = y - 10, previous_backend = previous });
            Native.ClickAt(grid, x, y - 10);
        }
        AutomationElement? combo = null;
        Wait(() => (combo = Elements(dialog).SingleOrDefault(e => e.Current.Name == "wxOwnerDrawnComboBox" && e.Current.NativeWindowHandle != 0)) is not null, "SceneChange backend editor appeared");
        var comboHandle = (nint)combo!.Current.NativeWindowHandle; isolation.VerifyWindow(comboHandle, app!);
        Capture(dialog, label + "-current");
        Native.ClickAt(comboHandle, (int)combo.Current.BoundingRectangle.Width - 8, (int)combo.Current.BoundingRectangle.Height / 2);
        nint popup = 0;
        Wait(() => (popup = Windows().SingleOrDefault(h => Text(h) == "panel" && Children(h).Any(c => Text(c) == "wxVListBox"))) != 0, "Actual backend choices popup appeared");
        var list = Children(popup).Single(h => Text(h) == "wxVListBox"); isolation.VerifyWindow(list, app!);
        Wait(() => Native.Height(popup) >= combo.Current.BoundingRectangle.Height * 2.5, "Backend popup has expanded to all three actual choices");
        Capture(popup, label + "-choices");
        Native.PostKey(comboHandle, backend == "scxvid" ? 0x26 : 0x28); Native.PostKey(comboHandle, 0x0D);
        var apply = Control(dialog, "Apply");
        Wait(() => Native.IsWindowEnabled(apply), "Backend selection creates a pending Preferences change");
        Capture(dialog, label + "-selected"); Click(dialog, "Apply");
        Wait(() => {
            if (Window("Generate keyframes?") != 0 || Progress() != 0) throw new InvalidOperationException("Backend Apply unexpectedly prompted or scanned scene changes");
            return !Native.IsWindowEnabled(apply) && Backend() == backend;
        }, "Real Preferences Apply persists the selected backend without generation");
        Require(Backend() == backend, "Actual persisted backend after native Apply is " + backend + ": " + label);
        Capture(dialog, label + "-applied"); Click(dialog, "Cancel");
        Wait(() => !Windows().Contains(dialog) && Native.IsWindowEnabled(main), "Preferences closed after Apply");
    }
    string SaveAss(string label)
    {
        var path = Path.Combine(artifacts, label + ".ass");
        Menu("File", "Save Subtitles as..."); FileDialog("Save subtitles file", path);
        Wait(() => File.Exists(path) && !Text(main).StartsWith('*'), "Native ASS Save As completed: " + label);
        return path;
    }
    bool OwnershipState(string label, int[] expected, string expectedFile)
    {
        var actual = Export(label + "-memory"); var ass = SaveAss(label);
        var reference = File.ReadAllLines(ass).SingleOrDefault(line => line.StartsWith("Keyframes File: ", StringComparison.Ordinal))?[16..] ?? "";
        var resolved = reference.Length == 0 ? "" : Path.GetFullPath(Path.Combine(Path.GetDirectoryName(ass)!, reference));
        Write(label + "-ownership.json", new { expected_keyframes = expected, actual_keyframes = actual, keyframes_file = reference, expected_file = expectedFile.Length == 0 ? "" : Path.GetRelativePath(artifacts, expectedFile) });
        var memory = Check(actual.SequenceEqual(expected), label + ": full in-memory keyframe vector retains the expected owner");
        var owner = Check(string.Equals(resolved, expectedFile, StringComparison.OrdinalIgnoreCase), label + ": native ASS preserves the expected Keyframes File reference");
        return memory && owner;
    }
    static void WriteKeyframes(string path, int[] frames)
        => File.WriteAllText(path, "# keyframe format v1\nfps 0\n" + string.Join('\n', frames) + "\n", new UTF8Encoding(false));
    void OpenKeyframes(string path)
    {
        Command("Open Keyframes..."); FileDialog("Open keyframes file", path);
    }
    void Ownership()
    {
        var wwxd = cache;
        SwitchBackend("scxvid", "prepare-scxvid");
        Command("Generate Scene Change Keyframes..."); Confirm(false, "prepare-scxvid");
        Require(!Complete("prepare-scxvid"), "Second backend cache is prepared by real SceneChange generation");
        var scxvid = Directory.GetFiles(Path.GetDirectoryName(cache)!, "*_scxvid.kf.txt").Single();
        Require(Parse(scxvid).Length >= 4, "Real scxvid generation produced a multi-scene cache");
        int[] wwxdFrames = [0, 17, 83, 301], scxvidFrames = [0, 23, 137, 503], linkedFrames = [0, 11, 47, 257];
        WriteKeyframes(wwxd, wwxdFrames); WriteKeyframes(scxvid, scxvidFrames);
        var wwxdBytes = File.ReadAllBytes(wwxd); var scxvidBytes = File.ReadAllBytes(scxvid); var manifestBytes = File.ReadAllBytes(manifest);
        File.Copy(wwxd, Path.Combine(artifacts, "prepared-wwxd.kf.txt")); File.Copy(scxvid, Path.Combine(artifacts, "prepared-scxvid.kf.txt")); File.Copy(manifest, Path.Combine(artifacts, "prepared-manifest.json"));
        SwitchBackend("wwxd", "prepare-wwxd");
        Require(OwnershipState("prepared-wwxd", wwxdFrames, wwxd), "Setup selects the known WWXD cache through a real backend change");

        var manual = Path.Combine(artifacts, "manual.kf.txt"); WriteKeyframes(manual, Sentinel); OpenKeyframes(manual);
        Require(OwnershipState("manual-before", Sentinel, manual), "Manual external keyframes are natively loaded before switching");
        SwitchBackend("scxvid", "manual-to-scxvid");
        var manualPreserved = OwnershipState("manual-after", Sentinel, manual);

        var linked = Path.Combine(artifacts, "linked.kf.txt"); WriteKeyframes(linked, linkedFrames);
        var fixture = Path.Combine(artifacts, "linked-fixture.ass");
        var lines = File.ReadAllLines(Path.Combine(artifacts, "manual-before.ass"));
        File.WriteAllLines(fixture, lines.Select(line => line.StartsWith("Keyframes File: ", StringComparison.Ordinal) ? "Keyframes File: linked.kf.txt" : line), new UTF8Encoding(true));
        Menu("File", "Open Subtitles..."); FileDialog("Open subtitles file", fixture);
        Require(OwnershipState("linked-before", linkedFrames, linked), "Real ASS Open automatically loads its associated external keyframes");
        SwitchBackend("wwxd", "linked-to-wwxd");
        var linkedPreserved = OwnershipState("linked-after", linkedFrames, linked);

        Command("Close Keyframes"); var defaults = Export("provider-default-memory");
        Require(defaults.Length > 0 && !defaults.SequenceEqual(scxvidFrames), "Provider defaults are nonempty and distinguishable from cached sentinels");
        Require(OwnershipState("provider-default-before", defaults, ""), "Close Keyframes restores provider defaults and clears ownership path");
        SwitchBackend("scxvid", "default-to-scxvid");
        var defaultRefreshed = OwnershipState("provider-default-after", scxvidFrames, scxvid);
        SwitchBackend("wwxd", "scenechange-to-wwxd");
        var sceneChanged = OwnershipState("scenechange-hit", wwxdFrames, wwxd);
        var unchanged = Check(File.ReadAllBytes(wwxd).SequenceEqual(wwxdBytes) && File.ReadAllBytes(scxvid).SequenceEqual(scxvidBytes)
            && File.ReadAllBytes(manifest).SequenceEqual(manifestBytes), "Cache-hit ownership switches preserve both cache files and the complete manifest bytes");
        File.Copy(wwxd, Path.Combine(artifacts, "before-miss-wwxd.kf.txt")); File.Copy(scxvid, Path.Combine(artifacts, "before-miss-scxvid.kf.txt"));
        File.Copy(manifest, Path.Combine(artifacts, "before-miss-manifest.json"));
        using var beforeManifest = JsonDocument.Parse(manifestBytes);
        var wwxdEntry = beforeManifest.RootElement.EnumerateArray().Single(e => e.GetProperty("keyframes").GetString() == Path.GetFileName(wwxd)).Clone();
        var removed = Path.Combine(artifacts, "scxvid-cache-removed-for-miss.kf.txt"); File.Move(scxvid, removed);
        bool missDefault, missPreserved;
        try {
            SwitchBackend("scxvid", "scenechange-miss");
            missDefault = OwnershipState("scenechange-miss", defaults, "");
            missPreserved = Check(!File.Exists(scxvid) && File.ReadAllBytes(wwxd).SequenceEqual(wwxdBytes), "Cache miss does not generate a new target cache or alter the other backend cache");
            using var afterManifest = JsonDocument.Parse(File.ReadAllBytes(manifest));
            missPreserved &= Check(afterManifest.RootElement.EnumerateArray().Any(e => e.GetProperty("keyframes").GetString() == Path.GetFileName(wwxd) && JsonElement.DeepEquals(e, wwxdEntry)), "Cache miss retains the unremoved backend's manifest entry");
            File.Copy(manifest, Path.Combine(artifacts, "after-miss-manifest.json"));
        }
        finally { File.Move(removed, scxvid); }
        Require(manualPreserved && linkedPreserved && defaultRefreshed && sceneChanged && missDefault && unchanged && missPreserved, "Backend changes respect manual, linked, provider-default, and SceneChange keyframe ownership");
    }
    void StageSentinel()
    {
        Require(!generated.SequenceEqual(Sentinel), "Recognizable existing cache differs from the actual scan result");
        File.WriteAllText(cache, "# keyframe format v1\nfps 0\n" + string.Join('\n', Sentinel) + "\n", new UTF8Encoding(false));
        Command("Open Keyframes..."); FileDialog("Open keyframes file", cache);
        Require(Export("sentinel-memory").SequenceEqual(Sentinel), "Legal recognizable cache is loaded through the native keyframe command");
        Snapshot("before-operation");
    }
    void Snapshot(string label)
    {
        foreach (var pair in new[] { (cache, "cache.kf.txt"), (manifest, "manifest.json") })
            if (File.Exists(pair.Item1)) File.Copy(pair.Item1, Path.Combine(artifacts, label + "-" + pair.Item2), true);
        Write(label + "-state.json", new {
            cache_exists = File.Exists(cache), cache_copy = File.Exists(cache) ? label + "-cache.kf.txt" : null,
            manifest_exists = File.Exists(manifest), manifest_copy = File.Exists(manifest) ? label + "-manifest.json" : null,
            cache_files = Directory.GetFiles(Path.GetDirectoryName(manifest)!).Select(Path.GetFileName).Order().ToArray()
        });
    }
    bool Preserved(string label, byte[] cacheBytes, byte[] manifestBytes)
    {
        Snapshot(label);
        var bytes = Check(File.Exists(cache) && File.ReadAllBytes(cache).SequenceEqual(cacheBytes), label + ": existing cache bytes are preserved");
        var metadata = Check(File.Exists(manifest) && File.ReadAllBytes(manifest).SequenceEqual(manifestBytes), label + ": manifest bytes are preserved");
        var memory = Check(Export(label + "-memory").SequenceEqual(Sentinel), label + ": current in-memory keyframes are preserved");
        return bytes && metadata && memory;
    }
    void Cancel()
    {
        var cacheBytes = File.ReadAllBytes(cache); var manifestBytes = File.ReadAllBytes(manifest);
        Command("Generate Scene Change Keyframes..."); var progress = Confirm(true, "cancel");
        nint gauge = 0; long position = 0;
        Wait(() => {
            gauge = Children(progress).SingleOrDefault(h => PrivateDesktop.ClassName(h) == "msctls_progress32");
            if (gauge == 0) return false;
            isolation.VerifyWindow(gauge, app!); position = Native.Send(gauge, 0x408);
            return position > 0 && position < 270;
        }, "SceneChange has made nonzero progress before cancellation");
        Write("cancel-progress.json", new { value = position, maximum = 300 });
        Capture(progress, "cancel-in-progress");
        isolation.VerifyWindow(gauge, app!); var clickPosition = Native.Send(gauge, 0x408);
        Require(clickPosition > 0 && clickPosition < 270 && Native.IsWindowEnabled(Control(progress, "Cancel")), "Cancel is clicked while the observed scan progress is still active");
        Write("cancel-click-progress.json", new { value = clickPosition, maximum = 300, cancel_enabled = true });
        Click(progress, "Cancel");
        var error = Complete("cancel");
        var preserved = Preserved("after-cancel", cacheBytes, manifestBytes);
        Require(!error && preserved, "Cancellation is not a publication failure and preserves the last usable cache transaction");
        Retry("cancel-retry", cacheBytes, manifestBytes);
    }
    void CancelPublication(nint progress)
    {
        var button = Control(progress, "Cancel"); isolation.VerifyWindow(button, app!);
        var directory = Path.GetDirectoryName(cache)!;
        var pattern = Path.GetFileNameWithoutExtension(cache) + "_tmp_*" + Path.GetExtension(cache);
        string? temporary = null;
        Wait(() => {
            temporary = Directory.GetFiles(directory, pattern).SingleOrDefault();
            return temporary is not null && Parse(temporary).SequenceEqual(generated);
        }, "Completed scene scan reached the locked keyframe publication retry window", 75);
        var observed = Stopwatch.GetTimestamp();
        isolation.VerifyWindow(button, app!);
        var exists = File.Exists(temporary); var enabled = Native.IsWindowEnabled(button); var label = Text(button).Replace("&", "");
        if (exists && enabled && label == "Cancel") ObservedWindowsCommand.PostButtonClick(button, app!.Id);
        var clickMilliseconds = Stopwatch.GetElapsedTime(observed).TotalMilliseconds;
        Action("button", "Publication retry/Cancel");
        Write("publication-cancel-click.json", new {
            temporary = Path.GetFileName(temporary), complete_scan_output_observed = true,
            temporary_exists_at_click = exists, cancel_enabled_at_click = enabled,
            button_label_at_click = label, milliseconds_from_observation_to_post = clickMilliseconds
        });
        Require(exists && enabled && label == "Cancel", "Cancel is posted while complete output awaits publication and the native Cancel button is enabled");
        bool acceptedEnabled = true; string acceptedLabel = "";
        Wait(() => {
            acceptedEnabled = Native.IsWindowEnabled(button); acceptedLabel = Text(button);
            return !acceptedEnabled && acceptedLabel == "Cancelling...";
        }, "Native progress runner accepted cancellation during failed publication");
        Write("publication-cancel-accepted.json", new { cancel_enabled = acceptedEnabled, button_label = acceptedLabel });
        Capture(progress, "publication-cancelling");
    }
    void Publication(bool cancelDuringPublication)
    {
        var cacheBytes = File.ReadAllBytes(cache); var manifestBytes = File.ReadAllBytes(manifest);
        using (var blocker = new FileStream(cache, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            Write("publication-lock.json", new { access = "Read", share = "Read", allows_readers = true, allows_replace_or_delete = false });
            if (cancelDuringPublication)
                Require(!Directory.GetFiles(Path.GetDirectoryName(cache)!, "*_tmp_*").Any(), "No stale temporary output exists before the publication-cancel operation");
            Command("Generate Scene Change Keyframes..."); var progress = Confirm(true, "blocked");
            if (cancelDuringPublication) CancelPublication(progress);
            var error = Complete("blocked");
            var preserved = Preserved("after-blocked", cacheBytes, manifestBytes);
            var reported = Check(error, cancelDuringPublication
                ? "Publication failure remains an explicit Error generating keyframes even when the progress runner is cancelled"
                : "Blocked replacement reports an explicit Error generating keyframes rather than success");
            var cleaned = Check(!Directory.GetFiles(Path.GetDirectoryName(cache)!).Any(path => Path.GetFileName(path).Contains("_tmp_", StringComparison.Ordinal)), "Failed keyframe publication removes its unpublished temporary file");
            Require(reported && preserved && cleaned, "Failed publication preserves cache, manifest, and active keyframes");
        }
        Retry("unlocked-retry", cacheBytes, manifestBytes);
    }
    void Retry(string label, byte[] cacheBytes, byte[] manifestBytes)
    {
        Command("Generate Scene Change Keyframes..."); Confirm(true, label);
        Require(!Complete(label), "A new user-requested generation succeeds after the prior operation");
        Snapshot("after-" + label);
        Require(Parse(cache).SequenceEqual(generated) && !File.ReadAllBytes(cache).SequenceEqual(cacheBytes), "Retry replaces recognizable old bytes with actual SceneChange output");
        Require(Export(label + "-memory").SequenceEqual(generated), "Retry publishes the complete new keyframes into current memory");
        Require(File.ReadAllBytes(manifest).SequenceEqual(manifestBytes), "Retry retains the correct existing manifest identity");
    }
}

static class Native
{
    delegate bool Callback(nint window, nint data);
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] static extern bool GetWindowRect(nint window, out Rect rect);
    public static int Height(nint window) { if (!GetWindowRect(window, out var rect)) throw new Win32Exception(Marshal.GetLastPInvokeError()); return rect.Bottom - rect.Top; }
    [DllImport("user32.dll")] static extern bool EnumChildWindows(nint parent, Callback callback, nint data);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(nint window);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(nint window);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(nint window);
    [DllImport("user32.dll", EntryPoint = "PostMessageW")] static extern bool PostMessage(nint window, uint message, nint wp, nint lp);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")] static extern nint SendMessage(nint window, uint message, nint wp, nint lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint SendText(nint window, uint message, nint wp, string lp, uint flags, uint timeout, out nint result);
    public static nint[] Children(nint parent) { List<nint> found = []; Callback callback = (h, _) => { found.Add(h); return true; }; EnumChildWindows(parent, callback, 0); GC.KeepAlive(callback); return found.ToArray(); }
    public static void PostKey(nint window, int key) { if (!PostMessage(window, 0x100, key, 1) || !PostMessage(window, 0x101, key, unchecked((int)0xC0000001))) throw new InvalidOperationException("Native local key failed"); }
    public static void ClickAt(nint window, int x, int y) { var point = (nint)(x | y << 16); if (!PostMessage(window, 0x201, 1, point) || !PostMessage(window, 0x202, 0, point)) throw new InvalidOperationException("Native local mouse click failed"); }
    public static long Send(nint window, uint message) { if (SendMessage(window, message, 0, 0, 0x22, 1000, out var result) == 0) throw new TimeoutException("Native progress message"); return result.ToInt64(); }
    public static void Post(nint window, uint message) { if (!PostMessage(window, message, 0, 0)) throw new InvalidOperationException("Native button click failed"); }
    public static void SetText(nint window, string value) { if (SendText(window, 0xC, 0, value, 0x22, 1000, out _) == 0) throw new TimeoutException("Native file name entry"); }
}
