#:property TargetFramework=net10.0-windows
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using Aegisub.GuiAutomation.Driver;

if (args.Length != 2)
{
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/xaudio2-device-recovery-native.cs -- EXE ARTIFACTS");
    return 2;
}

try { return PrivateDesktop.Run(args, Path.GetFullPath(args[1]), desktop => new Scenario(args, desktop).Run()); }
catch (Exception error) { Console.Error.WriteLine(error); return 1; }

sealed class Scenario(string[] args, PrivateDesktop isolation)
{
    const uint WmDeviceChange = 0x0219;
    const nuint DbtDevnodesChanged = 0x0007;
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    static readonly string[] CaseNames = ["PlayingIgnoresDeviceNodeChange", "PlayingIgnoresDeviceNodeBurst", "QueuedChangeDoesNotStopNewPlayback", "QueuedChangeDoesNotRestartExplicitlyStoppedPlayback"];

    readonly string artifacts = Path.GetFullPath(args[1]);
    readonly List<Check> checks = [];
    readonly HashSet<string> completed = [];
    Process? app;
    nint main;
    string profile = "", phase = "setup", tracePath = "";
    int lastObservedTraceLines;

    public int Run()
    {
        Directory.CreateDirectory(artifacts);
        isolation.RecordDriver();
        using var driverLog = new StreamWriter(Path.Combine(artifacts, "driver.log")) { AutoFlush = true };
        Console.SetOut(driverLog);
        Console.SetError(driverLog);
        using var watchdog = new Timer(_ =>
        {
            WriteResult(false, "120-second watchdog expired");
            try { if (app is not null) isolation.Stop(app); }
            finally { Environment.Exit(124); }
        }, null, 120_000, Timeout.Infinite);

        try
        {
            Prepare();
            Start();
            OpenLongSilentAudio();
            StartPlayback();
            WaitForPlaying("XAudio2 accepted the long silent playback request");
            WaitFor(() => PlayerMessages().Any(message => message.Contains("Playback request handled", StringComparison.Ordinal)),
                TimeSpan.FromSeconds(10), "The real XAudio2 player handles playback, beyond the menu command being posted");
            WaitForCursorAdvance("Initial playback cursor advances before the device notification");

            Case(CaseNames[0], () =>
            {
                var before = LatestPlaybackPosition();
                var recoveryRunsBefore = RecoveryRunEntries().Count;
                SendDeviceChange("single-change");
                AssertPlaybackContinues(before, recoveryRunsBefore, "Single DBT_DEVNODES_CHANGED leaves playback active and advances its real audio cursor after the recovery callback");
            });

            Case(CaseNames[1], () =>
            {
                var before = LatestPlaybackPosition();
                var recoveryRunsBefore = RecoveryRunEntries().Count;
                SendDeviceChange("burst-1");
                SendDeviceChange("burst-2");
                SendDeviceChange("burst-3");
                AssertPlaybackContinues(before, recoveryRunsBefore, "A burst of DBT_DEVNODES_CHANGED notifications leaves playback active and advances its real audio cursor after the coalesced recovery callback");
            });

            Case(CaseNames[2], () =>
            {
                StopPlayback();
                var recoveryRunsBefore = RecoveryRunEntries().Count;
                SendDeviceChange("queued-before-play");
                StartPlayback();
                WaitForPlaying("Playback starts while device recovery is still inside its debounce window");
                Require(RecoveryRunEntries().Count == recoveryRunsBefore, "New playback starts before the queued recovery runs");
                var before = LatestPlaybackPosition();
                AssertPlaybackContinues(before, recoveryRunsBefore, "A previously queued notification does not stop newly started playback after its recovery callback");
                var start = ReadTrace().Last(entry => entry.Name == "audio_ui_duration" && entry.Payload.GetProperty("phase").GetString() == "audio_controller.play_to_end");
                var stop = ReadTrace().Last(entry => entry.Name == "audio_ui_duration" && entry.Payload.GetProperty("phase").GetString() == "audio_controller.stop");
                Require(stop.TimestampNs < start.TimestampNs && start.TimestampNs < RecoveryRunEntries()[recoveryRunsBefore].TimestampNs,
                    "Stop and new playback both complete before evaluation of the queued device change");
            });

            Case(CaseNames[3], () =>
            {
                EnsurePlaying();
                var recoveryRunsBefore = RecoveryRunEntries().Count;
                SendDeviceChange("queued-before-stop");
                StopPlayback();
                Require(RecoveryRunEntries().Count == recoveryRunsBefore, "Explicit stop precedes the queued recovery callback");
                WaitFor(() => RecoveryRunEntries().Count > recoveryRunsBefore, TimeSpan.FromSeconds(4), "The queued request is processed after the explicit stop");
                WaitForStopped("An explicit stop remains stopped after a queued device notification expires");
                var stop = ReadTrace().Last(entry => entry.Name == "audio_ui_duration" && entry.Payload.GetProperty("phase").GetString() == "audio_controller.stop");
                Require(stop.TimestampNs < RecoveryRunEntries()[recoveryRunsBefore].TimestampNs, "The completed Stop command predates recovery evaluation");
                Require(!RecoveryMessages().Any(message => message.Contains("Rebuilding XAudio2 audio player", StringComparison.Ordinal)),
                    "An idle player is not rebuilt when a queued generic device notification expires");
                Record("Stopped", new { explicit_stop_remains_idle = true, rebuild_count = RebuildMessages().Count });
            });

            Require(RecoveryRunEntries().Count >= CaseNames.Length,
                "Each generic device-notification scenario reaches its debounced recovery callback");
            Require(RebuildMessages().Count == 0,
                "No generic device-change hint rebuilds the healthy XAudio2 player");
            Native.Post(main, 0x0010, 0, 0);
            Require(app!.WaitForExit(10_000), "GUI test host closes normally within ten seconds");
            Require(isolation.ExitCode(app) == 0, "GUI test host exits with code zero");
            WriteResult(true, null);
            ValidateResult();
            return 0;
        }
        catch (Exception error)
        {
            try { Write("failure.json", new { phase, error = error.ToString(), trace_path = tracePath, checks }); }
            catch (Exception artifactError) { Console.Error.WriteLine("Could not write failure.json: " + artifactError); }
            WriteResult(false, error.ToString());
            Console.Error.WriteLine(error);
            return 1;
        }
        finally
        {
            if (app is not null)
            {
                isolation.Stop(app);
                app.Dispose();
            }
        }
    }

    void Prepare()
    {
        var profileRoot = Path.Combine(artifacts, "profile");
        profile = Path.GetFullPath(profileRoot);
        Directory.CreateDirectory(Path.Combine(profile, "user"));
        var config = JsonNode.Parse("""
            {"App":{"First Start":false,"Language":"en_US","Save UI State":false,"Auto":{"Backup":false,"Check For Updates":false,"Load Linked Files":1,"Save":false,"Save on Every Change":false}},"Audio":{"Player":"XAudio2"},"Subtitle":{"Use STC":false}}
            """)!;
        File.WriteAllText(Path.Combine(profile, "user", "config.json"), config.ToJsonString(Json));

        AutomationTestProfile.WriteMenusWithoutClipboardValidation(profile, artifacts);
        var menuPath = Path.Combine(profile, "user", "menu.json");
        var menus = JsonNode.Parse(File.ReadAllText(menuPath))!.AsObject();
        var audio = menus["main/audio"]!.AsArray();
        audio.Add(new JsonObject { ["command"] = "audio/play/to_end" });
        audio.Add(new JsonObject { ["command"] = "audio/stop" });
        File.WriteAllText(menuPath, menus.ToJsonString(Json));
        Write("profile.json", new
        {
            audio_player = "XAudio2",
            audio_source = "audio/open/blank (150 minute dummy silence)",
            menu_configuration = Path.GetRelativePath(artifacts, menuPath).Replace('\\', '/'),
            global_input = false,
            clipboard_operations = false
        });
    }

    void Start()
    {
        var hostArtifacts = Path.Combine(artifacts, "host");
        Directory.CreateDirectory(hostArtifacts);
        var start = new ProcessStartInfo(Path.GetFullPath(args[0])) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", hostArtifacts })
            start.ArgumentList.Add(argument);
        start.Environment["AEGISUB_PERF_TRACE"] = "audio,log,ops";
        app = isolation.Start(start, "xaudio2-device-recovery-native");
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(hostArtifacts, "ready.json"), app, TimeSpan.FromSeconds(20));
        main = isolation.MainWindow(app, 10);
        Wait(() => Native.IsWindowEnabled(main), "GUI test host enables its main window");
        tracePath = WaitForTracePath();
        Record("HostReady", new { process_id = app.Id, main_window = main.ToString("X"), trace_path = Path.GetRelativePath(artifacts, tracePath).Replace('\\', '/') });
    }

    void OpenLongSilentAudio()
    {
        Command("Audio", "Open 2h30 Blank Audio");
        WaitFor(() => ReadTrace().Any(entry => entry.Name == "audio_provider"), TimeSpan.FromSeconds(10), "The long silent audio provider loads");
    }

    void StartPlayback()
    {
        var previous = LatestPlayStart()?.TimestampNs;
        Command("Audio", "Play from selection start to end of file");
        WaitFor(() => LatestPlayStart() is { } current && current.TimestampNs != previous,
            TimeSpan.FromSeconds(5), "The new Play request is observed before reading its playback position");
    }
    void StopPlayback() => Command("Audio", "Stop playing");

    void EnsurePlaying()
    {
        if (!IsPlaying())
            StartPlayback();
        WaitForPlaying("Playback is active before the device notification");
    }

    void WaitForPlaying(string message) => WaitFor(IsPlaying, TimeSpan.FromSeconds(8), message);
    void WaitForStopped(string message) => WaitFor(() => !IsPlaying(), TimeSpan.FromSeconds(8), message);

    void Command(string menu, string label)
    {
        isolation.VerifyWindow(main, app!);
        var commandId = ObservedWindowsCommand.FindMenuCommand(main, app!.Id, menu, label);
        Native.SendCommand(main, commandId);
        Record("Command", new { menu, label });
    }

    void SendDeviceChange(string name)
    {
        isolation.VerifyWindow(main, app!);
        if (!Native.SendDeviceChange(main, WmDeviceChange, DbtDevnodesChanged))
            throw new InvalidOperationException("WM_DEVICECHANGE/DBT_DEVNODES_CHANGED was not delivered to the observed application main window");
        Record("DeviceChange", new { name, hwnd = main.ToString("X"), message = "WM_DEVICECHANGE", wparam = "DBT_DEVNODES_CHANGED" });
    }

    void AssertPlaybackContinues(int before, int recoveryRunsBefore, string description)
    {
        var recovery = WaitForValue(() => RecoveryRunEntries().Skip(recoveryRunsBefore).FirstOrDefault(),
            TimeSpan.FromSeconds(5), description + " (debounced recovery callback runs)");
        var firstPostRecovery = WaitForValue(() => TraceSnapshots().FirstOrDefault(entry =>
                entry.TimestampNs > recovery.TimestampNs),
            TimeSpan.FromSeconds(4), description + " (audio cursor remains in playback state after recovery)");
        WaitForPlaying(description + " (transport remains active after recovery)");
        WaitFor(() => TraceSnapshots().Any(entry => entry.TimestampNs > firstPostRecovery.TimestampNs
                && entry.Payload.GetProperty("detail_a").GetInt32() >= firstPostRecovery.Payload.GetProperty("detail_a").GetInt32() + 400),
            TimeSpan.FromSeconds(4), description + " (playback cursor advances at least 400 ms after recovery)");
        var after = LatestPlaybackPosition();
        Record("PlaybackAdvanced", new { before_ms = before, post_recovery_ms = firstPostRecovery.Payload.GetProperty("detail_a").GetInt32(), after_ms = after, delta_ms = after - before });
        Require(after >= before + 400, description);
    }

    int WaitForCursorAdvance(string message)
    {
        var before = LatestPlaybackPosition();
        WaitFor(() => LatestPlaybackPosition() >= before + 400, TimeSpan.FromSeconds(4), message);
        var after = LatestPlaybackPosition();
        Require(after >= before + 400, message);
        Record("PlaybackAdvanced", new { before_ms = before, after_ms = after, delta_ms = after - before });
        return after;
    }

    int LatestPlaybackPosition() => WaitFor<int?>(TryLatestPlaybackPosition, TimeSpan.FromSeconds(5),
        "Audio controller publishes an actual backend playback position")
        ?? throw new InvalidOperationException("Audio controller playback position was null");

    bool IsPlaying()
    {
        var transport = ReadTrace().LastOrDefault(entry => entry.Name == "audio_ui_duration"
            && entry.Payload.GetProperty("phase").GetString() is "audio_controller.play_to_end" or "audio_controller.stop");
        return transport?.Payload.GetProperty("phase").GetString() == "audio_controller.play_to_end";
    }

    TraceEntry? LatestPlayStart() => ReadTrace().LastOrDefault(entry => entry.Name == "audio_ui_duration"
        && entry.Payload.GetProperty("phase").GetString() == "audio_controller.play_to_end");

    List<TraceEntry> TraceSnapshots()
    {
        var entries = ReadTrace();
        var start = entries.LastOrDefault(entry => entry.Name == "audio_ui_duration"
            && entry.Payload.GetProperty("phase").GetString() == "audio_controller.play_to_end");
        if (start is null)
            return [];
        return entries.Where(entry => entry.TimestampNs > start.TimestampNs && entry.Name == "audio_ui_duration"
            && entry.Payload.GetProperty("phase").GetString() == "audio_controller.playback_timer"
            && entry.Payload.TryGetProperty("detail_a", out _)).ToList();
    }

    int? TryLatestPlaybackPosition()
    {
        var positions = TraceSnapshots()
            .Select(entry => entry.Payload.GetProperty("detail_a").GetInt32())
            .ToArray();
        if (positions.Length == 0)
            return null;
        return positions[^1];
    }

    List<TraceEntry> ReadTrace()
    {
        if (string.IsNullOrEmpty(tracePath) || !File.Exists(tracePath))
            return [];
        var entries = new List<TraceEntry>();
        using var stream = new FileStream(tracePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        using var reader = new StreamReader(stream);
        var contents = reader.ReadToEnd();
        var lines = contents.Split('\n');
        var hasTrailingNewline = contents.EndsWith('\n');
        for (var index = 0; index < lines.Length; index++)
        {
            var line = lines[index].TrimEnd('\r');
            if (line.Length == 0) continue;
            lastObservedTraceLines++;
            try
            {
                using var document = JsonDocument.Parse(line);
                var root = document.RootElement;
                if (root.TryGetProperty("name", out var name) && root.TryGetProperty("payload", out var payload))
                    entries.Add(new TraceEntry(name.GetString() ?? "", payload.Clone(), root.TryGetProperty("t_monotonic_ns", out var time) ? time.GetInt64() : 0));
            }
            catch (JsonException) when (!hasTrailingNewline && index == lines.Length - 1) { break; }
        }
        return entries;
    }

    List<TraceEntry> RecoveryRunEntries() => ReadTrace()
        .Where(entry => entry.Name == "audio/player/xaudio2/recovery"
            && entry.Payload.TryGetProperty("message", out var message)
            && message.GetString()?.Contains("Running queued XAudio2 recovery", StringComparison.Ordinal) == true)
        .ToList();

    List<string> RecoveryMessages() => ReadTrace()
        .Where(entry => entry.Name == "audio/player/xaudio2/recovery" && entry.Payload.TryGetProperty("message", out _))
        .Select(entry => entry.Payload.GetProperty("message").GetString() ?? "")
        .ToList();

    List<string> PlayerMessages() => ReadTrace()
        .Where(entry => entry.Name == "audio/player/xaudio2" && entry.Payload.TryGetProperty("message", out _))
        .Select(entry => entry.Payload.GetProperty("message").GetString() ?? "")
        .ToList();

    List<string> RebuildMessages() => RecoveryMessages()
        .Where(message => message.Contains("Rebuilding XAudio2 audio player", StringComparison.Ordinal))
        .ToList();

    string WaitForTracePath()
    {
        var sessionRoot = Path.Combine(profile, "user", "perf-sessions");
        var path = WaitFor(() => Directory.Exists(sessionRoot)
            ? Directory.GetFiles(sessionRoot, "trace.ndjson", SearchOption.AllDirectories).SingleOrDefault()
            : null, TimeSpan.FromSeconds(10), "Audio/log perf trace opens under the isolated profile");
        if (string.IsNullOrEmpty(path))
            throw new InvalidOperationException("Perf trace path was not published");
        return path;
    }

    void Case(string name, Action action)
    {
        phase = name;
        action();
        completed.Add(name);
        WriteResult(false, null);
    }

    void Require(bool passed, string description)
    {
        checks.Add(new Check(phase, description, passed));
        WriteResult(false, passed ? null : description);
        if (!passed)
            throw new InvalidOperationException(description);
    }

    void Record(string name, object details) => File.AppendAllText(Path.Combine(artifacts, "observations.ndjson"),
        JsonSerializer.Serialize(new { phase, name, details }) + Environment.NewLine);

    void Write(string name, object value) => File.WriteAllText(Path.Combine(artifacts, name), JsonSerializer.Serialize(value, Json));

    void WriteResult(bool passed, string? error) => Write("result.json", new
    {
        passed,
        error,
        phase,
        trace_path = string.IsNullOrEmpty(tracePath) ? null : Path.GetRelativePath(artifacts, tracePath).Replace('\\', '/'),
        observed_trace_lines = lastObservedTraceLines,
        cases = CaseNames.Select(name => new
        {
            name,
            status = (error is not null && name == phase) || checks.Any(check => check.Name == name && !check.Passed) ? "failed" : completed.Contains(name) ? "passed" : name == phase ? "in_progress" : "incomplete",
            checks = checks.Count(check => check.Name == name)
        }),
        checks
    });

    void ValidateResult()
    {
        using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(artifacts, "result.json")));
        var cases = document.RootElement.GetProperty("cases").EnumerateArray().ToArray();
        if (!document.RootElement.GetProperty("passed").GetBoolean()
            || !cases.Select(item => item.GetProperty("name").GetString()).SequenceEqual(CaseNames)
            || cases.Any(item => item.GetProperty("status").GetString() != "passed" || item.GetProperty("checks").GetInt32() == 0))
            throw new InvalidDataException("The final report is missing a passing named case or concrete assertion");
    }

    void Wait(Action condition, string message) => Wait(() => { condition(); return true; }, TimeSpan.FromSeconds(5), message);

    void WaitFor(Action condition, TimeSpan timeout, string message) => Wait(() => { condition(); return true; }, timeout, message);

    T WaitFor<T>(Func<T> condition, TimeSpan timeout, string message) => Wait(condition, timeout, message);

    T WaitForValue<T>(Func<T?> condition, TimeSpan timeout, string message) where T : class
    {
        var deadline = Stopwatch.GetTimestamp() + (long)(timeout.TotalSeconds * Stopwatch.Frequency);
        while (Stopwatch.GetTimestamp() < deadline)
        {
            var result = condition();
            if (result is not null)
                return result;
            Thread.Sleep(25);
        }
        throw new TimeoutException(message);
    }

    T Wait<T>(Func<T> condition, TimeSpan timeout, string message)
    {
        var deadline = Stopwatch.GetTimestamp() + (long)(timeout.TotalSeconds * Stopwatch.Frequency);
        while (Stopwatch.GetTimestamp() < deadline)
        {
            var result = condition();
            if (result is not null && !Equals(result, false))
                return result;
            Thread.Sleep(25);
        }
        throw new TimeoutException(message);
    }

    sealed record Check(string Name, string Description, bool Passed);
    sealed record TraceEntry(string Name, JsonElement Payload, long TimestampNs);
}

static class Native
{
    [DllImport("user32.dll", EntryPoint = "PostMessageW", ExactSpelling = true)]
    static extern int PostMessage(nint hwnd, uint message, nuint wParam, nint lParam);

    const uint SmtoAbortIfHung = 0x0002;
    const uint SmtoBlock = 0x0001;
    const uint SmtoErrorOnExit = 0x0020;

    [return: MarshalAs(UnmanagedType.Bool)]
    [DllImport("user32.dll", EntryPoint = "IsWindowEnabled", ExactSpelling = true)]
    internal static extern bool IsWindowEnabled(nint hwnd);

    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", ExactSpelling = true, SetLastError = true)]
    static extern nint SendMessageTimeout(nint hwnd, uint message, nuint wParam, nint lParam, uint flags, uint timeout, out nint result);

    internal static bool SendDeviceChange(nint hwnd, uint message, nuint eventType) =>
        SendMessageTimeout(hwnd, message, eventType, 0, SmtoAbortIfHung | SmtoBlock | SmtoErrorOnExit, 2_000, out _) != 0;

    internal static void SendCommand(nint window, int command)
    {
        if (SendMessageTimeout(window, 0x0111, (nuint)command, 0, SmtoAbortIfHung | SmtoBlock | SmtoErrorOnExit, 10_000, out _) == 0)
            throw new InvalidOperationException("The observed command did not complete");
    }

    internal static void Post(nint hwnd, uint message, nuint wParam, nint lParam)
    {
        if (PostMessage(hwnd, message, wParam, lParam) == 0)
            throw new InvalidOperationException("Failed to post a message to the observed app window");
    }
}
