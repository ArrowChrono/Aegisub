#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:property InvariantGlobalization=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

try
{
    var options = DriverOptions.Parse(args);
    return options.InputContract.StartsWith("geometry-", StringComparison.Ordinal)
        ? PrivateDesktop.Run(args, options.Artifacts, isolation => RunGeometryContract(options, isolation))
        : options.InputContract == "none" ? Run(options) : RunInputContract(options);
}
catch (Exception error)
{
    Console.Error.WriteLine($"visual_tool_drag.error={error.GetType().Name}:{error.Message}");
    Console.Error.WriteLine(error);
    return 1;
}

static int Run(DriverOptions options)
{
    PrepareArtifactsDirectory(options.Artifacts);
    var fixturePath = Path.Combine(options.Artifacts, "scenario.ass");
    var originalPath = Path.Combine(options.Artifacts, "fixture-original.ass");
    var afterDragPath = Path.Combine(options.Artifacts, "after-drag.ass");
    var afterUndoPath = Path.Combine(options.Artifacts, "after-undo.ass");
    var resultPath = Path.Combine(options.Artifacts, "drag-result.json");
    var profilePath = Path.Combine(options.Artifacts, "profile");
    var hostArtifactsPath = Path.Combine(options.Artifacts, "host");

    if (!File.Exists(options.Executable))
        throw new FileNotFoundException("Aegisub executable was not found", options.Executable);
    if (!File.Exists(options.Video))
        throw new FileNotFoundException("Video fixture was not found", options.Video);

    var eventCount = options.Size == "large" ? options.LargeEventCount : options.SmallEventCount;
    var fixture = Fixture.Build(eventCount, options.Selection == "multi");
    File.WriteAllText(fixturePath, fixture, new UTF8Encoding(false));
    File.Copy(fixturePath, originalPath, overwrite: true);
    WriteProfileConfig(profilePath, options.SubtitleProvider);
    Directory.CreateDirectory(hostArtifactsPath);

    Process? process = null;
    var succeeded = false;
    try
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = options.Executable,
            WorkingDirectory = Path.GetDirectoryName(options.Executable) ?? Environment.CurrentDirectory,
            UseShellExecute = false,
            WindowStyle = ProcessWindowStyle.Minimized,
        };
        startInfo.Environment.Remove("AEGISUB_PERF_TRACE");
        if (options.Trace == "on")
            startInfo.Environment["AEGISUB_PERF_TRACE"] = "video,visual-tool-drag-benchmark";
        startInfo.Environment["AEGISUB_ENABLE_SKIA_VIDEO_TOOLS"] = "0";
        AddArgument(startInfo, "--gui-test");
        AddArgument(startInfo, "host");
        AddArgument(startInfo, "--profile-dir");
        AddArgument(startInfo, profilePath);
        AddArgument(startInfo, "--artifacts");
        AddArgument(startInfo, hostArtifactsPath);
        AddArgument(startInfo, "--open");
        AddArgument(startInfo, fixturePath);
        AddArgument(startInfo, "--open");
        AddArgument(startInfo, options.Video);

        process = Process.Start(startInfo)
            ?? throw new InvalidOperationException("Could not start Aegisub");
        Console.WriteLine($"visual_tool_drag.pid={process.Id}");
        AutomationProtocol.WaitForReadyArtifact(
            Path.Combine(hostArtifactsPath, "ready.json"),
            process,
            TimeSpan.FromSeconds(options.TimeoutSeconds));

        var mainWindow = UiaDriver.WaitForMainWindow(
            process,
            TimeSpan.FromSeconds(options.TimeoutSeconds));
        var mainHwnd = new nint(mainWindow.Current.NativeWindowHandle);
        if (mainHwnd == 0)
            throw new InvalidOperationException("Aegisub main window has no native handle");
        ShowWindow(mainHwnd, NativeConstants.ShowNoActivate);
        if (!SetWindowPos(
            mainHwnd,
            0,
            40,
            40,
            options.WindowWidth,
            options.WindowHeight,
            SetWindowPosFlags.NoActivate | SetWindowPosFlags.ShowWindow))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "SetWindowPos failed");

        var videoCanvas = WaitForVideoCanvas(process, options.TimeoutSeconds);

        Thread.Sleep(options.SettleMilliseconds);
        ThrowIfProcessFailed(process);
        EnsureSaved(mainWindow, process, fixturePath, options.TimeoutSeconds);
        var initial = AssSnapshot.Read(fixturePath);
        if (initial.Dialogues.Count != eventCount)
            throw new InvalidOperationException(
                $"Expected {eventCount} loaded dialogue lines, found {initial.Dialogues.Count}");

        var selectedLineCount = 1;
        SelectVisibleDialogueLines(mainWindow);
        if (options.Selection == "multi")
            selectedLineCount = ConfirmMultiSelection(mainWindow, process, options.TimeoutSeconds, "after_select_visible");
        SelectDragTool(mainWindow);
        if (options.Selection == "multi")
            selectedLineCount = ConfirmMultiSelection(mainWindow, process, options.TimeoutSeconds, "after_select_drag_tool");
        Thread.Sleep(250);
        videoCanvas = WaitForVideoCanvas(process, options.TimeoutSeconds);
        if (!GetClientRect(videoCanvas, out var videoClient)
            || videoClient.Width < 160
            || videoClient.Height < 90)
            throw new InvalidOperationException("Video Display client rectangle is invalid");
        var canvasAspect = videoClient.Width / (double)videoClient.Height;
        if (Math.Abs(canvasAspect - 640.0 / 480.0) > 0.005)
            throw new InvalidOperationException(
                $"Video Display aspect ratio must match the 640x480 fixture; got " +
                $"{videoClient.Width}x{videoClient.Height}");
        Console.WriteLine(
            $"visual_tool_drag.canvas={videoClient.Width}x{videoClient.Height}");

        var start = new PointI(videoClient.Width / 2, videoClient.Height / 2);
        var final = new PointI(
            Math.Clamp(start.X + options.DeltaX, 16, videoClient.Width - 16),
            Math.Clamp(start.Y + options.DeltaY, 16, videoClient.Height - 16));
        var actualDeltaX = final.X - start.X;
        var actualDeltaY = final.Y - start.Y;
        var queueMarker = options.InputMode == "paced-hold"
            ? FindQueueMarkerButton(mainHwnd, videoCanvas)
            : 0;

        long started;
        long posted;
        long released;
        var motionBarrierReached = false;
        var captureRetainedDuringHold = false;
        var holdMilliseconds = 0.0;
        if (options.InputMode == "double-click")
        {
            started = Stopwatch.GetTimestamp();
            SendMouseToWindow(videoCanvas, WindowMessage.MouseMove, final, 0);
            SendMouseToWindow(videoCanvas, WindowMessage.LeftButtonDown, final, NativeConstants.MouseKeyLeftButton);
            SendMouseToWindow(videoCanvas, WindowMessage.LeftButtonUp, final, 0);
            SendMouseToWindow(videoCanvas, WindowMessage.LeftButtonDoubleClick, final, NativeConstants.MouseKeyLeftButton);
            SendMouseToWindow(videoCanvas, WindowMessage.LeftButtonUp, final, 0);
            SendWindowMessage(videoCanvas, WindowMessage.Null, 0, 0);
            posted = released = Stopwatch.GetTimestamp();
        }
        else
        {
            SendMouseToWindow(videoCanvas, WindowMessage.MouseMove, start, 0);
            SendMouseToWindow(videoCanvas, WindowMessage.LeftButtonDown, start, NativeConstants.MouseKeyLeftButton);
            WaitForCapture(videoCanvas, captured: true, process, options.TimeoutSeconds);

            var points = BuildMotionPath(
                start,
                final,
                options.MotionCount,
                options.MotionRepeat,
                videoClient.Width,
                videoClient.Height);
            started = Stopwatch.GetTimestamp();
            for (var pointIndex = 0; pointIndex < points.Count; ++pointIndex)
            {
                var mouseKeys = NativeConstants.MouseKeyLeftButton;
                if (options.MotionRepeat > 1 && pointIndex < points.Count - 1)
                    mouseKeys |= NativeConstants.MouseKeyShift;
                PostMouseToWindow(videoCanvas, WindowMessage.MouseMove, points[pointIndex], mouseKeys);
                if (options.MotionIntervalMilliseconds > 0 && pointIndex < points.Count - 1)
                    Thread.Sleep(options.MotionIntervalMilliseconds);
            }
            posted = Stopwatch.GetTimestamp();

            if (options.InputMode == "paced-hold")
            {
                WaitForPostedMotions(queueMarker, process, options.TimeoutSeconds);
                WaitForCapture(videoCanvas, captured: true, process, options.TimeoutSeconds);
                motionBarrierReached = true;

                var holdStarted = Stopwatch.GetTimestamp();
                Thread.Sleep(options.HoldMilliseconds);
                var holdEnded = Stopwatch.GetTimestamp();
                holdMilliseconds = ElapsedMilliseconds(holdStarted, holdEnded);
                WaitForCapture(videoCanvas, captured: true, process, options.TimeoutSeconds);
                captureRetainedDuringHold = true;
            }

            var capture = GuiThreadInfo.Create();
            var videoThread = GetWindowThreadProcessId(videoCanvas, out _);
            if (videoThread == 0 || !GetGUIThreadInfo(videoThread, ref capture))
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not inspect capture before release");
            if (capture.Capture != videoCanvas)
                throw new InvalidOperationException("Mouse capture was already lost before the driver posted its release");
            PostMouseToWindow(videoCanvas, WindowMessage.LeftButtonUp, final, 0);
            WaitForCapture(videoCanvas, captured: false, process, options.TimeoutSeconds);
            // Capture is released before the final synchronous render. A cross-process
            // WM_NULL cannot run until the mouse-up handler has returned.
            SendWindowMessage(videoCanvas, WindowMessage.Null, 0, 0);
            released = Stopwatch.GetTimestamp();
        }

        Thread.Sleep(500);
        EnsureSaved(mainWindow, process, fixturePath, options.TimeoutSeconds);
        File.Copy(fixturePath, afterDragPath, overwrite: true);
        var afterDrag = AssSnapshot.Read(afterDragPath);

        var expectedDelta = new Position(
            actualDeltaX * 640.0 / videoClient.Width,
            actualDeltaY * 480.0 / videoClient.Height);
        ValidateAfterDrag(initial, afterDrag, options.Selection, expectedDelta);

        InvokeUndo(mainWindow, process, options.TimeoutSeconds);
        EnsureSaved(mainWindow, process, fixturePath, options.TimeoutSeconds);
        File.Copy(fixturePath, afterUndoPath, overwrite: true);
        var afterUndo = AssSnapshot.Read(afterUndoPath);
        ValidateUndo(initial, afterUndo);

        RequestCleanClose(process, mainHwnd, options.TimeoutSeconds);
        var drainMilliseconds = ElapsedMilliseconds(started, released);
        var trace = options.Trace == "on"
            ? TraceMetrics.Read(profilePath, options.MotionCount, options.TraceWindow)
            : null;
        var actualProvider = trace?.Environment.SubtitleProvider
            ?? ReadSelectedSubtitleProvider(profilePath);
        if (!string.Equals(actualProvider, options.SubtitleProvider, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException(
                $"Requested subtitles provider '{options.SubtitleProvider}', observed '{actualProvider}'");
        if (options.InputMode == "paced-hold"
            && trace!.PresentedRendersAfterFinalCommitBeforeInteractionEnd < 1)
            throw new InvalidOperationException(
                "Paced-hold trace contained no presented render after the final commit " +
                "and before visual_tool.interaction_end");

        var result = new
        {
            version = 4,
            scenario = options.InputMode == "throughput"
                ? $"{options.Size}-{options.Selection}"
                : $"{options.Size}-{options.Selection}-{options.InputMode}",
            input_mode = options.InputMode,
            size = options.Size,
            selection = options.Selection,
            trace = options.Trace,
            trace_window_mode = options.TraceWindow,
            event_count = eventCount,
            selected_line_count = selectedLineCount,
            motion_count = options.MotionCount,
            motion_repeat = options.MotionRepeat,
            motion_interval_requested_ms = options.MotionIntervalMilliseconds,
            requested_subtitle_provider = options.SubtitleProvider,
            canvas = new { width = videoClient.Width, height = videoClient.Height },
            client_delta = new { x = actualDeltaX, y = actualDeltaY },
            expected_script_delta = new { x = expectedDelta.X, y = expectedDelta.Y },
            environment = trace?.Environment ?? new TraceEnvironment(
                "unknown",
                "opengl",
                actualProvider,
                640,
                480,
                true),
            input = new
            {
                post_ms = ElapsedMilliseconds(started, posted),
                drain_ms = drainMilliseconds,
                motions_per_second = options.MotionCount * 1000.0 / drainMilliseconds,
                capture_released = true,
                motion_barrier_reached = motionBarrierReached,
                hold_requested_ms = options.InputMode == "paced-hold" ? options.HoldMilliseconds : 0,
                hold_elapsed_ms = holdMilliseconds,
                capture_retained_during_hold = captureRetainedDuringHold,
            },
            correctness = new
            {
                final_positions = afterDrag.Dialogues.Take(2).Select(line => line.Position).ToArray(),
                drag_valid = true,
                undo_restored = true,
            },
            trace_metrics = trace,
        };
        File.WriteAllText(
            resultPath,
            JsonSerializer.Serialize(result, new JsonSerializerOptions { WriteIndented = true }),
            new UTF8Encoding(false));
        Console.WriteLine($"visual_tool_drag.scenario={result.scenario}");
        Console.WriteLine($"visual_tool_drag.input_mode={options.InputMode}");
        Console.WriteLine($"visual_tool_drag.trace={options.Trace}");
        Console.WriteLine($"visual_tool_drag.motions={options.MotionCount}");
        Console.WriteLine($"visual_tool_drag.drain_ms={drainMilliseconds.ToString("F3", CultureInfo.InvariantCulture)}");
        Console.WriteLine("visual_tool_drag.correctness=ok");
        succeeded = true;
        return 0;
    }
    finally
    {
        if (!succeeded && process is not null)
        {
            try
            {
                if (!process.HasExited)
                {
                    process.Kill(entireProcessTree: true);
                    if (!process.WaitForExit(5000))
                        Console.Error.WriteLine(
                            $"visual_tool_drag.cleanup_timeout=pid:{process.Id}");
                }
            }
            catch (Exception cleanupError)
            {
                Console.Error.WriteLine(
                    $"visual_tool_drag.cleanup_error={cleanupError.GetType().Name}:{cleanupError.Message}");
            }
        }
        process?.Dispose();
    }
}


static int RunGeometryContract(DriverOptions options, PrivateDesktop isolation)
{
    var name = options.InputContract[9..];
    var available = new[]
    {
        "translate", "off", "move", "scaled-curve",
        "scale", "scale-shear", "normalize", "rotate",
        "origin", "origin-xy", "nudge-origin", "reject-animation",
        "reject-mixed", "reject-xy", "xy-no-clip", "empty",
        "reject-animated-only", "reject-endpoint", "endpoint-no-geometry", "mixed-static-endpoint", "reject-perspective-scale",
        "drawing-tail", "scale-layout", "reject-vertical-font", "reject-reset",
        "reject-multiclip", "reject-zero-scale", "reject-malformed", "no-motion",
        "return-to-start", "clip-only", "scale-nudge", "scale-curve",
        "decimal-curve", "reject-decimal-curve", "reject-effect", "reject-pos-move",
        "reject-duplicate-xy", "drawing-tail-vertical", "last-valid-preview", "reject-animated-spaced", "implicit-integer", "reject-implicit-fractional", "reject-implicit-reset", "reject-malformed-position", "implicit-no-geometry"
    };
    if (!available.Contains(name))
        throw new ArgumentException("geometry case must be one of: " + string.Join(", ", available));
    PrepareArtifactsDirectory(options.Artifacts);
    isolation.RecordDriver();
    var output = new StreamWriter(Path.Combine(options.Artifacts, "driver-output.log")) { AutoFlush = true };
    Console.SetOut(output);
    Console.SetError(output);
    var fixturePath = Path.Combine(options.Artifacts, "scenario.ass");
    var profilePath = Path.Combine(options.Artifacts, "profile");
    var host = Path.Combine(options.Artifacts, "host");
    Directory.CreateDirectory(host);
    var firstTags = @"\pos(300,200)\org(260,180)\frz90\fscx100\fscy100\clip(260,100,340,180)";
    var secondTags = @"\pos(420,320)\org(390,300)\frz90\fscx150\fscy80\iclip(370,230,450,320)";
    const string curve = "m 480 320 l 640 320 640 480 n 500 340 b 520 300 580 300 600 340 s 500 340 540 380 580 340 p 620 360 c";
    if (name is "move" or "reject-endpoint" or "endpoint-no-geometry")
    {
        firstTags = firstTags.Replace(@"\pos(300,200)", @"\move(300,200,360,250,400,1600)");
        secondTags = secondTags.Replace(@"\pos(420,320)", @"\move(420,320,470,370,200,1800)");
    }
    if (name == "mixed-static-endpoint")
        secondTags = @"\move(420,320,470,370,400,1600)\frz90\fscx150\fscy80";
    if (name is "scaled-curve" or "scale-curve")
    {
        firstTags = Regex.Replace(firstTags, @"\\clip\([^)]*\)", @"\clip(2," + curve + ")");
        secondTags = Regex.Replace(secondTags, @"\\iclip\([^)]*\)", @"\iclip(2," + curve + ")");
    }
    if (name == "normalize")
        firstTags = firstTags.Replace(@"\fscx100\fscy100", @"\fscx200\fscy50");
    if (name == "scale-shear")
    {
        firstTags = firstTags.Replace(@"\frz90", @"\frz30\fax0.2\fay0.1");
        secondTags = secondTags.Replace(@"\frz90", @"\frz30\fax0.1\fay0.2");
    }
    if (name == "reject-animation")
        secondTags += @"\t(0,1800,\clip(390,230,470,320))";
    if (name == "reject-animated-only")
        secondTags = Regex.Replace(secondTags, @"\\iclip\([^)]*\)", "") + @"\t(0,1800,\clip(390,230,470,320))";
    if (name == "reject-animated-spaced")
        secondTags = Regex.Replace(secondTags, @"\\iclip\([^)]*\)", "") + @"\t(0,1800,\ clip(390,230,470,320))";
    if (name == "reject-perspective-scale")
        secondTags += @"\frx25";
    if (name == "reject-vertical-font")
        secondTags += @"\fn@Arial";
    if (name == "reject-reset")
        secondTags += @"\rDefault";
    if (name == "reject-multiclip")
        secondTags += @"\clip(m 350 200 l 460 200 460 350)";
    if (name == "reject-pos-move")
        secondTags += @"\move(420,320,470,370,200,1800)";
    if (name == "reject-duplicate-xy")
        secondTags += @"\frx0\frx25";
    if (name is "decimal-curve" or "reject-decimal-curve")
    {
        var coordinate = name == "decimal-curve" ? "0.1" : "0.01";
        firstTags = Regex.Replace(firstTags, @"\\clip\([^)]*\)", @"\clip(2,m " + coordinate + " 200 l 600 200 600 480)");
        secondTags = Regex.Replace(secondTags, @"\\iclip\([^)]*\)", @"\iclip(2,m " + coordinate + " 200 l 600 200 600 480)");
    }
    if (name == "implicit-integer")
    {
        firstTags = firstTags.Replace(@"\pos(300,200)", "").Replace(@"\org(260,180)", @"\org(295,426)").Replace(@"\clip(260,100,340,180)", @"\clip(260,330,340,470)");
        secondTags = secondTags.Replace(@"\pos(420,320)", "").Replace(@"\org(390,300)", @"\org(294,407)").Replace(@"\iclip(370,230,450,320)", @"\iclip(270,300,360,450)");
    }
    if (name is "reject-implicit-fractional" or "reject-implicit-reset")
        secondTags = secondTags.Replace(@"\pos(420,320)", "");
    if (name == "reject-implicit-reset")
        secondTags += @"\rAlternate";
    if (name == "implicit-no-geometry")
    {
        firstTags = Regex.Replace(firstTags, @"\\(?:pos|org|clip)\([^)]*\)", "");
        secondTags = Regex.Replace(secondTags, @"\\(?:pos|org|iclip)\([^)]*\)", "");
    }
    if (name == "reject-malformed-position")
        secondTags = secondTags.Replace(@"\pos(420,320)", @"\pos(420)");
    if (name == "reject-malformed")
        secondTags = Regex.Replace(secondTags, @"\\iclip\([^)]*\)", @"\iclip(m 1 2 l invalid)");
    if (name == "endpoint-no-geometry")
    {
        firstTags = Regex.Replace(firstTags, @"\\(?:org|clip)\([^)]*\)", "");
        secondTags = Regex.Replace(secondTags, @"\\(?:org|iclip)\([^)]*\)", "");
    }
    if (name == "empty")
    {
        firstTags = Regex.Replace(firstTags, @"\\clip\([^)]*\)", @"\clip(0,0,0,0)");
        secondTags = Regex.Replace(secondTags, @"\\iclip\([^)]*\)", @"\iclip(m 0 0)");
    }
    if (name == "xy-no-clip")
    {
        firstTags = Regex.Replace(firstTags, @"\\clip\([^)]*\)", "");
        secondTags = Regex.Replace(secondTags, @"\\iclip\([^)]*\)", "");
    }
    var lines = Fixture.Build(48, true).Split('\n');
    var firstEvent = Array.FindIndex(lines, line => line.StartsWith("Dialogue:", StringComparison.Ordinal));
    lines[firstEvent] = @"Dialogue: 3,0:00:00.00,0:00:10.00,Default,linked-primary,12,23,34,,{" + firstTags + @"\bord2.5\shad1.25\1c&H74B2E8&\t(200,1800,\blur1.2)}Geometry primary";
    lines[firstEvent + 1] = @"Dialogue: 1,0:00:00.00,0:00:10.00,Default,linked-secondary,31,42,53,,{" + secondTags + @"\bord1.5\shad0.5\1c&H80E8B2&}Geometry secondary" + (name == "reject-mixed" ? @"{\fscx70}mixed run" : "");
    if (name is "drawing-tail" or "drawing-tail-vertical")
    {
        lines[firstEvent] = lines[firstEvent].Replace("Geometry primary", @"{\p1}m 0 0 l 100 0 100 60 0 60{\p0}");
        lines[firstEvent + 1] = lines[firstEvent + 1].Replace("Geometry secondary", @"{\p1}m 0 0 b 20 -10 90 10 100 50 l 0 60{\p0}");
    }
    if (name == "implicit-integer")
    {
        lines[firstEvent] = lines[firstEvent].Replace("12,23,34,,{", "12,22,34,,{");
        lines[firstEvent + 1] = lines[firstEvent + 1].Replace("31,42,53,,{", "31,43,53,,{");
    }
    if (name == "reject-implicit-reset")
    {
        lines[firstEvent + 1] = lines[firstEvent + 1].Replace("31,42,53,,{", "31,43,53,,{");
        var style = lines.Single(line => line.StartsWith("Style: Default,"));
        var alternate = style.Replace("Style: Default,", "Style: Alternate,").Replace(",2,10,10,10,1", ",1,70,20,20,1");
        lines = lines.SelectMany(line => line == style ? new[] { line, alternate } : new[] { line }).ToArray();
    }
    if (name == "drawing-tail-vertical")
        lines = lines.Select(line => line.Replace("Style: Default,Arial,", "Style: Default,@Arial,")).ToArray();
    if (name == "reject-effect")
        lines[firstEvent + 1] = lines[firstEvent + 1].Replace("31,42,53,,{", "31,42,53,Banner;0;0;5,{");
    if (name == "scale-layout")
        lines = lines.Select(line => line.Replace("LayoutResX: 640", "LayoutResX: 1280")).ToArray();
    File.WriteAllText(fixturePath, string.Join('\n', lines), new UTF8Encoding(false));
    WriteProfileConfig(profilePath, options.SubtitleProvider);
    var menu = JsonNode.Parse(File.ReadAllText(Path.Combine("src", "libresrc", "default_menu.json")))!.AsObject();
    menu["main"]!.AsArray().Add(new JsonObject { ["submenu"] = "geometry-test", ["text"] = "Geometry Test" });
    var testMenu = new JsonArray();
    foreach (var (command, text) in new[]
    {
        ("video/tool/scale/normalize/x", "Normalize X"),
        ("video/tool/nudge/right/large", "Nudge Right"),
        ("video/tool/nudge/up/large", "Nudge Up"),
        ("subtitle/save", "Save Subtitles"), ("edit/undo", "Undo"), ("edit/redo", "Redo"),
        ("video/frame/save", "Save Frame"),
        ("video/tool/geometry_linking", "Geometry Linking")
    }) testMenu.Add(new JsonObject { ["command"] = command, ["text"] = text });
    menu["geometry-test"] = testMenu;
    File.WriteAllText(Path.Combine(profilePath, "user", "menu.json"), menu.ToJsonString());
    Process? process = null;
    nint failureMain = 0;
    using var watchdog = new Timer(_ => { try { if (process is not null) isolation.Stop(process); } finally { Environment.Exit(124); } }, null, TimeSpan.FromSeconds(180), Timeout.InfiniteTimeSpan);
    try
    {
        var start = new ProcessStartInfo(options.Executable) { UseShellExecute = false, WorkingDirectory = Environment.CurrentDirectory, WindowStyle = ProcessWindowStyle.Minimized };
        start.Environment["AEGISUB_ENABLE_SKIA_VIDEO_TOOLS"] = "0";
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profilePath, "--artifacts", host, "--open", fixturePath, "--open", options.Video })
            AddArgument(start, argument);
        process = isolation.Start(start, "actual-host");
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(host, "ready.json"), process, TimeSpan.FromSeconds(options.TimeoutSeconds));
        var hwnd = isolation.MainWindow(process, options.TimeoutSeconds);
        failureMain = hwnd;
        var window = AutomationElement.FromHandle(hwnd);
        isolation.VerifyWindow(hwnd, process);
        ShowWindow(hwnd, NativeConstants.ShowNoActivate);
        SetWindowPos(hwnd, 0, 40, 40, options.WindowWidth, options.WindowHeight, SetWindowPosFlags.NoActivate | SetWindowPosFlags.ShowWindow);
        Thread.Sleep(options.SettleMilliseconds);
        ClickToolbar("Select all dialogue lines that are visible");
        ConfirmMultiSelection(window, process, options.TimeoutSeconds, "geometry-before", CheckAlive);
        File.AppendAllText(Path.Combine(options.Artifacts, "toolbar-evidence.ndjson"), JsonSerializer.Serialize(new
        {
            phase = "post-selection",
            selected_lines = 2
        }) + Environment.NewLine);
        var scaleCases = new[]
        {
            "scale", "scale-shear", "normalize", "reject-animation",
            "reject-mixed", "reject-animated-only", "reject-perspective-scale", "drawing-tail",
            "scale-layout", "reject-vertical-font", "reject-reset", "reject-multiclip",
            "reject-zero-scale", "reject-malformed", "scale-nudge", "scale-curve",
            "reject-decimal-curve", "reject-effect", "reject-pos-move", "reject-duplicate-xy",
            "drawing-tail-vertical", "last-valid-preview", "reject-animated-spaced", "implicit-integer", "reject-implicit-fractional", "reject-implicit-reset", "reject-malformed-position", "implicit-no-geometry"
        };
        var tool = scaleCases.Contains(name) ? "Scale subtitles on X and Y axes"
            : name == "clip-only" ? "Clip subtitles to a rectangle"
            : name is "origin-xy" or "reject-xy" or "xy-no-clip" ? "Rotate subtitles on their X and Y axes"
            : name is "rotate" or "origin" or "nudge-origin" ? "Rotate subtitles on their Z axis" : "Drag subtitles";
        ClickToolbar(tool);
        ConfirmToolRadio(tool);
        Thread.Sleep(250);
        var canvas = Canvas();
        if (!GetClientRect(canvas, out var rect) || Math.Abs(rect.Width / (double)rect.Height - 4.0 / 3) > .005)
            throw new InvalidOperationException("Geometry fixture requires a 4:3 canvas");
        PointI Pixel(double x, double y) => new((int)Math.Round(x * rect.Width / 640), (int)Math.Round(y * rect.Height / 480));
        string WindowText(nint target) => isolation.ReadText(target, process);
        void CheckAlive()
        {
            isolation.VerifyCurrent();
            process.Refresh();
            if (process.HasExited) throw new InvalidOperationException("Private host exited: " + isolation.ExitCode(process));
        }
        nint Canvas()
        {
            var deadline = Deadline(options.TimeoutSeconds);
            while (Stopwatch.GetTimestamp() < deadline)
            {
                CheckAlive();
                var candidate = EnumerateChildWindows(hwnd).Where(IsWindowVisible).Where(handle => NativeClassName(handle) == "wxGLCanvas").OrderByDescending(handle => NativeWindowRect(handle).Area).FirstOrDefault();
                if (candidate != 0 && IsWindowEnabled(hwnd)) { isolation.VerifyWindow(candidate, process); return candidate; }
                Thread.Sleep(50);
            }
            throw new TimeoutException("Private host video canvas was not ready");
        }
        void ClickToolbar(string tooltip) => ClickGeometryToolbar(window, process, tooltip, options.Artifacts, isolation);
        void Command(string label)
        {
            isolation.VerifyWindow(hwnd, process);
            var id = ObservedWindowsCommand.FindMenuCommand(hwnd, process.Id, "Geometry Test", label);
            File.AppendAllText(Path.Combine(options.Artifacts, "command-evidence.ndjson"), JsonSerializer.Serialize(new { label, id, pid = process.Id, desktop = isolation.Name }) + Environment.NewLine);
            ObservedWindowsCommand.PostMenuCommand(hwnd, process.Id, id);
            Thread.Sleep(500);
        }
        void Mouse(nint target, WindowMessage message, PointI point, int state)
        {
            isolation.VerifyWindow(target, process);
            SendMouseToWindow(target, message, point, state);
        }
        void PostMouse(nint target, WindowMessage message, PointI point, int state)
        {
            isolation.VerifyWindow(target, process);
            PostMouseToWindow(target, message, point, state);
        }
        void CaptureState(nint target, bool captured)
        {
            isolation.VerifyWindow(target, process);
            WaitForCapture(target, captured, process, options.TimeoutSeconds, CheckAlive);
        }
        void Undo()
        {
            Command("Undo");
            WaitUntil(() => WindowText(hwnd).TrimStart().StartsWith("* ", StringComparison.Ordinal), "Undo modified state");
        }
        void Close()
        {
            isolation.VerifyWindow(hwnd, process);
            if (!PostMessage(hwnd, (uint)WindowMessage.Close, 0, 0)) throw new Win32Exception("Private WM_CLOSE failed");
            if (!process.WaitForExit(options.TimeoutSeconds * 1000)) throw new TimeoutException("Private host did not close");
            var code = isolation.ExitCode(process);
            if (code != 0) throw new InvalidOperationException("Private host exit code: " + code);
        }
        void WaitUntil(Func<bool> condition, string description)
        {
            var deadline = Deadline(options.TimeoutSeconds);
            while (Stopwatch.GetTimestamp() < deadline)
            {
                CheckAlive();
                if (condition()) return;
                Thread.Sleep(50);
            }
            throw new TimeoutException(description);
        }
        void Capture(string label)
        {
            var destination = Path.GetFullPath(Path.Combine(options.Artifacts, label + "-frame.png"));
            if (!destination.StartsWith(Path.GetFullPath(options.Artifacts) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || File.Exists(destination))
                throw new InvalidOperationException("Frame destination must be a new file inside this case's artifacts");
            Command("Save Frame");
            SaveFrameDialog(destination);
            WaitUntil(() => File.Exists(destination) && new FileInfo(destination).Length > 0, "Frame PNG was not exported");
            using var image = new System.Drawing.Bitmap(destination);
            if (image.Width != 640 || image.Height != 480) throw new InvalidOperationException("Exported frame dimensions differ from the fixture");
            var colors = new HashSet<int>();
            for (var y = 0; y < image.Height; ++y)
                for (var x = 0; x < image.Width; ++x) colors.Add(image.GetPixel(x, y).ToArgb());
            File.AppendAllText(Path.Combine(options.Artifacts, "frame-evidence.ndjson"), JsonSerializer.Serialize(new { label, image.Width, image.Height, distinct_colors = colors.Count, command = "video/frame/save", pid = process.Id, desktop = isolation.Name }) + Environment.NewLine);
            if (name == "scale" && colors.Count < 8) throw new InvalidOperationException("Pixel-oracle frame is empty or background-only");
            isolation.VerifyWindow(hwnd, process);
            ScreenCapture.SaveWindowPng(hwnd, Path.Combine(options.Artifacts, label + "-window.png"));
        }
        nint Dialog(string title)
        {
            nint dialog = 0;
            WaitUntil(() =>
            {
                dialog = isolation.Windows(process).SingleOrDefault(target => NativeClassName(target) == "#32770" && WindowText(target) == title);
                return dialog != 0;
            }, "Owned private-desktop dialog not found: " + title);
            return dialog;
        }
        void Button(nint dialog, string label)
        {
            string Caption(nint target) => Regex.Replace(WindowText(target).Replace("&", "", StringComparison.Ordinal), @"\([A-Za-z]\)$", "");
            var button = EnumerateChildWindows(dialog).Single(target => NativeClassName(target) == "Button" && IsWindowVisible(target) && (Caption(target) == label || (label == "Save" && Caption(target) == "保存")));
            isolation.VerifyWindow(button, process);
            var observed = WindowText(button);
            var visible = IsWindowVisible(button);
            var enabled = IsWindowEnabled(button);
            if (!enabled) throw new InvalidOperationException("Observed dialog button is disabled");
            var parent = NativeGetParent(button);
            var id = NativeGetDlgCtrlId(button);
            isolation.VerifyWindow(parent, process);
            var route = label == "OK" ? "post-bm-click" : "post-parent-command";
            File.AppendAllText(Path.Combine(options.Artifacts, "dialog-evidence.ndjson"), JsonSerializer.Serialize(new { phase = "button-observed-before-dispatch", route, label, observed_caption = observed, dialog = $"0x{dialog:X}", button = $"0x{button:X}", parent = $"0x{parent:X}", id, visible, enabled, pid = process.Id, desktop = isolation.Name }) + Environment.NewLine);
            isolation.VerifyWindow(button, process);
            if (label == "OK")
            {
                if (!PostMessage(button, 0x00F5, 0, 0)) throw new Win32Exception("Owned warning BM_CLICK could not be posted");
            }
            else ObservedWindowsCommand.PostButtonClick(button, process.Id);
        }
        void SaveFrameDialog(string destination)
        {
            var dialog = Dialog("Save PNG snapshot");
            var edit = EnumerateChildWindows(dialog).Single(target => NativeClassName(target) == "Edit" && IsWindowVisible(target) && WindowText(target).EndsWith(".png", StringComparison.OrdinalIgnoreCase));
            isolation.VerifyWindow(edit, process);
            var value = new StringBuilder(destination);
            if (SendMessageStringTimeout(edit, 0x000C, 0, value, SendMessageTimeoutFlags.AbortIfHung | SendMessageTimeoutFlags.Block, 1000, out var result) == 0 || result == 0)
                throw new InvalidOperationException("Observed File name edit rejected the artifact path");
            if (WindowText(edit) != destination) throw new InvalidOperationException("File name edit did not retain the exact artifact path");
            File.AppendAllText(Path.Combine(options.Artifacts, "dialog-evidence.ndjson"), JsonSerializer.Serialize(new { title = "Save PNG snapshot", dialog = $"0x{dialog:X}", edit = $"0x{edit:X}", pid = process.Id, desktop = isolation.Name, destination = Path.GetRelativePath(options.Artifacts, destination) }) + Environment.NewLine);
            Button(dialog, "Save");
            WaitUntil(() => !isolation.Windows(process).Contains(dialog), "Frame save dialog did not close");
        }
        string Save(string label)
        {
            isolation.VerifyWindow(hwnd, process);
            if (WindowText(hwnd).TrimStart().StartsWith("* ", StringComparison.Ordinal))
            {
                var previousWrite = File.GetLastWriteTimeUtc(fixturePath);
                Command("Save Subtitles");
                WaitUntil(() => !WindowText(hwnd).TrimStart().StartsWith("* ", StringComparison.Ordinal) && File.GetLastWriteTimeUtc(fixturePath) >= previousWrite, "Save Subtitles did not reach a clean state");
            }
            File.Copy(fixturePath, Path.Combine(options.Artifacts, label + ".ass"));
            return ContractDocument(fixturePath);
        }
        void ConfirmToolRadio(string tooltip)
        {
            var deadline = Deadline(5);
            while (Stopwatch.GetTimestamp() < deadline)
            {
                var radio = window.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().First(element => element.Current.Name.Contains(tooltip, StringComparison.OrdinalIgnoreCase));
                if (radio.TryGetCurrentPattern(TogglePattern.Pattern, out var toggle) && ((TogglePattern)toggle).Current.ToggleState == ToggleState.On)
                {
                    File.AppendAllText(Path.Combine(options.Artifacts, "toolbar-evidence.ndjson"), JsonSerializer.Serialize(new
                    {
                        phase = "post-state",
                        target = radio.Current.Name,
                        state = "On"
                    }) + Environment.NewLine);
                    return;
                }
                if (radio.TryGetCurrentPattern(SelectionItemPattern.Pattern, out var selection) && ((SelectionItemPattern)selection).Current.IsSelected)
                    return;
                Thread.Sleep(50);
            }
            throw new InvalidOperationException("Single tool command did not select the intended radio: " + tooltip);
        }
        const string linkingTooltip = "Link origins and clips to supported subtitle geometry edits";
        var baseline = Save("before");
        Capture("before");
        if (name != "off")
        {
            ClickToolbar(linkingTooltip);
            ConfirmToolRadio(linkingTooltip);
            var intermediate = tool == "Drag subtitles" ? "Scale subtitles on X and Y axes" : "Drag subtitles";
            ClickToolbar(intermediate);
            ConfirmToolRadio(intermediate);
            ConfirmToolRadio(linkingTooltip);
            ClickToolbar(tool);
            ConfirmToolRadio(tool);
            ConfirmToolRadio(linkingTooltip);
        }
        var checkHeldLink = name is "translate" or "off" or "scale" or "origin" or "origin-xy" or "clip-only";
        var heldLinkFailures = new List<string>();
        var linkingToolbar = nint.Zero;
        var linkingButtonId = 0;
        var linkingMenuId = 0;
        if (checkHeldLink)
        {
            var target = window.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>()
                .Single(element => element.Current.ControlType == ControlType.Button && element.Current.Name.Contains(linkingTooltip, StringComparison.OrdinalIgnoreCase));
            var automationId = target.Current.AutomationId;
            var observedId = Regex.Match(automationId, @"(?:^|\D)(\d+)$");
            if (!observedId.Success || !int.TryParse(observedId.Groups[1].Value, out linkingButtonId))
                throw new InvalidOperationException("Geometry Linking has no observed numeric toolbar command ID: " + automationId);
            for (var parent = TreeWalker.RawViewWalker.GetParent(target); parent is not null; parent = TreeWalker.RawViewWalker.GetParent(parent))
                if (parent.Current.ControlType == ControlType.ToolBar && parent.Current.NativeWindowHandle != 0)
                {
                    linkingToolbar = new nint(parent.Current.NativeWindowHandle);
                    break;
                }
            isolation.VerifyWindow(linkingToolbar, process);
            if (NativeClassName(linkingToolbar) != "ToolbarWindow32"
                || SendWindowMessage(linkingToolbar, WindowMessage.ToolbarCommandToIndex, linkingButtonId, 0) < 0)
                throw new InvalidOperationException("Observed Geometry Linking command is not in its native toolbar");
            linkingMenuId = ObservedWindowsCommand.FindMenuCommand(hwnd, process.Id, "Geometry Test", "Geometry Linking");
            File.AppendAllText(Path.Combine(options.Artifacts, "toolbar-evidence.ndjson"), JsonSerializer.Serialize(new
            {
                phase = "held-link-target-observed", automation_id = automationId, button_id = linkingButtonId,
                menu_id = linkingMenuId, toolbar = $"0x{linkingToolbar:X}", pid = process.Id, desktop = isolation.Name
            }) + Environment.NewLine);
            RecordHeldLink("before", false);
        }
        void RecordHeldLink(string phase, bool captured)
        {
            Thread.Sleep(200);
            CaptureState(canvas, captured);
            isolation.VerifyWindow(linkingToolbar, process);
            var state = SendWindowMessage(linkingToolbar, WindowMessage.ToolbarGetState, linkingButtonId, 0).ToInt64();
            if (state < 0) throw new InvalidOperationException("Observed Geometry Linking toolbar command disappeared");
            var enabled = (state & 4) != 0;
            var selected = (state & 1) != 0;
            var expectedSelected = name != "off";
            var screenshot = "link-" + phase + "-window.png";
            isolation.VerifyWindow(hwnd, process);
            ScreenCapture.SaveWindowPng(hwnd, Path.Combine(options.Artifacts, screenshot));
            CaptureState(canvas, captured);
            File.AppendAllText(Path.Combine(options.Artifacts, "link-state-evidence.ndjson"), JsonSerializer.Serialize(new
            {
                phase, state, enabled, selected, expected_selected = expectedSelected, expected_enabled = true,
                captured, screenshot, route = "TB_GETSTATE", button_id = linkingButtonId,
                toolbar = $"0x{linkingToolbar:X}", pid = process.Id, desktop = isolation.Name
            }) + Environment.NewLine);
            if (!enabled || selected != expectedSelected)
                heldLinkFailures.Add($"{phase}: expected enabled=true, selected={expectedSelected}; got enabled={enabled}, selected={selected}");
        }
        var rejectionRelease = Pixel(180, 120);
        void Drag(PointI from, PointI to, bool rejecting = false)
        {
            Mouse(canvas, WindowMessage.MouseMove, from, 0);
            Mouse(canvas, WindowMessage.LeftButtonDown, from, NativeConstants.MouseKeyLeftButton);
            CaptureState(canvas, true);
            if (checkHeldLink) RecordHeldLink("down", true);
            if (rejecting)
            {
                rejectionRelease = to;
                PostMouse(canvas, WindowMessage.MouseMove, to, NativeConstants.MouseKeyLeftButton);
                return;
            }
            Mouse(canvas, WindowMessage.MouseMove, new PointI((from.X + to.X) / 2, (from.Y + to.Y) / 2), NativeConstants.MouseKeyLeftButton);
            if (checkHeldLink)
            {
                RecordHeldLink("held", true);
                isolation.VerifyWindow(hwnd, process);
                ObservedWindowsCommand.PostMenuCommand(hwnd, process.Id, linkingMenuId);
                File.AppendAllText(Path.Combine(options.Artifacts, "command-evidence.ndjson"), JsonSerializer.Serialize(new
                {
                    label = "Geometry Linking", id = linkingMenuId, phase = "held", posts = 1,
                    pid = process.Id, desktop = isolation.Name
                }) + Environment.NewLine);
                RecordHeldLink("held-after-command", true);
            }
            Mouse(canvas, WindowMessage.LeftButtonUp, to, 0);
            CaptureState(canvas, false);
            if (checkHeldLink)
            {
                RecordHeldLink("released", false);
                if (heldLinkFailures.Count != 0)
                    throw new InvalidOperationException("Geometry Linking held-state contract failed: " + string.Join("; ", heldLinkFailures));
            }
        }
        var delta = new PointI(40, 30);
        string? lastValidDocument = null;
        void Phase(string value) => File.AppendAllText(Path.Combine(options.Artifacts, "gesture-phases.txt"), value + Environment.NewLine);
        if (name == "last-valid-preview")
        {
            var from = Pixel(100, 80);
            Drag(from, new PointI(from.X + 40, from.Y + 20));
            lastValidDocument = Save("last-valid-preview");
            if (lastValidDocument == baseline)
                throw new InvalidOperationException("Legal reference gesture did not change either selected line");
            var referenceTexts = File.ReadLines(fixturePath).Where(line => line.StartsWith("Dialogue:")).Take(2).Select(line => line.Split(',', 10)[9]).ToArray();
            for (var index = 0; index < 2; ++index)
            {
                AssertGeometryTag(referenceTexts[index], "fscx", 150);
                AssertGeometryTag(referenceTexts[index], "fscy", 75);
                var ax = index == 0 ? 280d : 410d;
                var ay = index == 0 ? 140d : 270d;
                var screenXRatio = index == 0 ? .75d : 75d / 80;
                var screenYRatio = index == 0 ? 1.5d : 1d;
                AssertGeometryClip(index == 0 ? firstTags : secondTags, referenceTexts[index], (x, y) => (ax + (x - ax) * screenXRatio, ay + (y - ay) * screenYRatio));
            }
            Undo();
            AssertContractEqual(baseline, Save("last-valid-reference-undo"), "Reference preparation restores the original selected events");
            Phase("reference-restored");
            Mouse(canvas, WindowMessage.LeftButtonDown, from, NativeConstants.MouseKeyLeftButton);
            Phase("second-down-returned");
            CaptureState(canvas, true);
            Phase("second-captured");
            Mouse(canvas, WindowMessage.MouseMove, new PointI(from.X + 40, from.Y + 20), NativeConstants.MouseKeyLeftButton);
            Phase("valid-preview-returned");
            rejectionRelease = new PointI(from.X + 80, from.Y + 80);
            PostMouse(canvas, WindowMessage.MouseMove, rejectionRelease, NativeConstants.MouseKeyLeftButton);
            Phase("invalid-preview-posted");
        }
        else if (name == "mixed-static-endpoint")
        {
            var from = Pixel(420, 320);
            Drag(from, new PointI(from.X + delta.X, from.Y + delta.Y));
        }
        else if (name == "move")
        {
            var target = Pixel(360, 240);
            Mouse(canvas, WindowMessage.LeftButtonDown, target, NativeConstants.MouseKeyLeftButton);
            Mouse(canvas, WindowMessage.LeftButtonUp, target, 0);
            Mouse(canvas, (WindowMessage)0x203, target, NativeConstants.MouseKeyLeftButton);
            Mouse(canvas, WindowMessage.LeftButtonUp, target, 0);
        }
        else if (name == "normalize")
        {
            File.WriteAllText(Path.Combine(options.Artifacts, "normalize-uia.txt"), string.Join('\n', window.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().Select(element => element.Current.ControlType.ProgrammaticName + ":" + element.Current.Name)));
            Command("Normalize X");
        }
        else if (name is "rotate" or "reject-xy" or "xy-no-clip" or "scale-nudge")
            Command("Nudge Right");
        else if (name == "clip-only")
        {
            var from = Pixel(260, 100);
            Drag(from, new PointI(from.X + 40, from.Y + 30));
        }
        else if (name == "nudge-origin")
            Command("Nudge Up");
        else if (name is "origin" or "origin-xy")
        {
            var from = Pixel(260, 180);
            Drag(from, new PointI(from.X + 30, from.Y + 20));
        }
        else if (scaleCases.Contains(name))
        {
            var from = Pixel(100, 80);
            Drag(from, new PointI(from.X + 80, from.Y + (name == "reject-zero-scale" ? 80 : 40)), name.StartsWith("reject-"));
        }
        else if (name is "no-motion" or "return-to-start")
        {
            var from = Pixel(300, 200);
            Mouse(canvas, WindowMessage.LeftButtonDown, from, NativeConstants.MouseKeyLeftButton);
            CaptureState(canvas, true);
            if (name == "return-to-start")
                Mouse(canvas, WindowMessage.MouseMove, new PointI(from.X + 40, from.Y + 30), NativeConstants.MouseKeyLeftButton);
            Mouse(canvas, WindowMessage.LeftButtonUp, from, 0);
            CaptureState(canvas, false);
        }
        else
        {
            var from = Pixel(300, 200);
            Drag(from, new PointI(from.X + delta.X, from.Y + delta.Y), name == "reject-endpoint");
        }
        if (name.StartsWith("reject-") || name == "last-valid-preview")
        {
            Phase("rejection-release-start");
            PostMouse(canvas, WindowMessage.LeftButtonUp, rejectionRelease, 0);
            Phase("rejection-release-posted");
            CaptureState(canvas, false);
            Phase("rejection-released");
            var dialog = Dialog("Geometry Linking");
            nint[] children = [];
            var message = "";
            var route = "native-static";
            string[] lastUiTree = [];
            void WarningEvidence() => File.WriteAllText(Path.Combine(options.Artifacts, "warning-provider.json"), JsonSerializer.Serialize(new
            {
                route, dialog = $"0x{dialog:X}", native_class = NativeClassName(dialog),
                child_classes = children.Select(NativeClassName).Distinct().ToArray(), actual_message = message,
                last_empty_uia_tree = lastUiTree, pid = process.Id, desktop = isolation.Name
            }, new JsonSerializerOptions { WriteIndented = true }));
            WaitUntil(() =>
            {
                isolation.VerifyWindow(dialog, process);
                children = EnumerateChildWindows(dialog).ToArray();
                message = string.Join(" ", children.Where(target => NativeClassName(target) == "Static").Select(WindowText)).Trim();
                if (message.Length == 0 && children.Any(target => NativeClassName(target) == "DirectUIHWND"))
                {
                    var exactDialog = AutomationElement.FromHandle(dialog);
                    var elements = exactDialog.FindAll(TreeScope.Descendants, new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Text)).Cast<AutomationElement>().ToArray();
                    if (elements.Any(element => element.Current.ProcessId != process.Id))
                        throw new InvalidOperationException("TaskDialog text provider escaped the owned dialog process");
                    message = string.Join(" ", elements.Select(element => element.Current.Name)).Trim();
                    route = "directui-taskdialog-exact-hwnd-uia-text";
                    if (message.Length == 0)
                        lastUiTree = exactDialog.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().Select(element => element.Current.ControlType.ProgrammaticName + ":" + element.Current.Name).ToArray();
                }
                WarningEvidence();
                return message.Length != 0;
            }, "Owned warning provider did not publish nonempty text");
            File.WriteAllText(Path.Combine(options.Artifacts, "rejection.txt"), message);
            if (!message.Contains("No selected line was changed", StringComparison.Ordinal))
                throw new InvalidOperationException("Rejection did not state batch atomicity");
            var expectedReason = name switch
            {
                "reject-animation" or "reject-animated-only" or "reject-animated-spaced" => "Animated",
                "reject-endpoint" => "single move endpoint",
                "reject-xy" or "reject-perspective-scale" => "Perspective",
                "reject-mixed" => "Visible runs",
                "reject-reset" => "style resets",
                "reject-multiclip" => "clip precedence",
                "reject-zero-scale" or "last-valid-preview" => "Non-positive",
                "reject-malformed" => "Malformed",
                "reject-decimal-curve" => "libass and VSFilter",
                "reject-effect" => "Scrolling",
                "reject-pos-move" => "Conflicting position",
                "reject-duplicate-xy" => "Duplicate pose",
                "reject-vertical-font" => "Vertical fonts",
                "reject-implicit-fractional" => "default position",
                "reject-implicit-reset" => "style resets",
                "reject-malformed-position" => "Malformed position",
                _ => throw new InvalidOperationException("No expected failure boundary configured")
            };
            if (!message.Contains(expectedReason, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Expected rejection boundary " + expectedReason + ", observed: " + message);
            isolation.VerifyWindow(dialog, process);
            ScreenCapture.SaveWindowPng(dialog, Path.Combine(options.Artifacts, "rejected-dialog.png"));
            Button(dialog, "OK");
            WaitUntil(() => !isolation.Windows(process).Contains(dialog), "Warning dialog did not close");
            AssertContractEqual(lastValidDocument ?? baseline, Save("rejected"), "Unsupported update leaves every selected line at the last valid preview");
            if (lastValidDocument is not null)
            {
                Capture("after");
                Undo();
                AssertContractEqual(baseline, Save("undo"), "One Undo restores the original selection after an invalid final update");

                Command("Redo");
                Thread.Sleep(300);
                AssertContractEqual(lastValidDocument, Save("redo"), "Redo restores the last valid preview, not the invalid update");
            }
        }
        else if (name is "no-motion" or "return-to-start")
        {
            AssertContractEqual(baseline, Save("after"), "No movement or return to start preserves the original ASS text exactly");
            Capture("after");
        }
        else
        {
            var actual = Save("after");
            string Neutral(string document)
            {
                var documentLines = document.Split('\n');
                var selectedCount = 0;
                for (var i = 0; i < documentLines.Length && selectedCount < 2; ++i)
                {
                    if (!documentLines[i].StartsWith("Dialogue:", StringComparison.Ordinal))
                        continue;
                    documentLines[i] = Regex.Replace(documentLines[i], @"\\(?:pos|move|org|fscx|fscy|frz|frx|fry)(?:\([^)]*\)|[-+\d.eE]+)|\\i?clip\([^)]*\)", "");
                    ++selectedCount;
                }
                return string.Join('\n', documentLines);
            }
            AssertContractEqual(Neutral(baseline), Neutral(actual), "Both selected lines preserve all unrelated tags, styles, text and other dialogues");
            var texts = File.ReadLines(fixturePath).Where(line => line.StartsWith("Dialogue:")).Take(2).Select(line => line.Split(',', 10)[9]).ToArray();
            for (var index = 0; index < 2; ++index)
            {
                var old = index == 0 ? firstTags : secondTags;
                var px = index == 0 ? 300d : 420d;
                var py = index == 0 ? 200d : 320d;
                var ox = index == 0 ? 260d : 390d;
                var oy = index == 0 ? 180d : 300d;
                if (name == "implicit-integer")
                {
                    px = index == 0 ? 315 : 314;
                    py = index == 0 ? 446 : 427;
                    ox = index == 0 ? 295 : 294;
                    oy = index == 0 ? 426 : 407;
                }
                Func<double, double, (double X, double Y)> mapping = (x, y) => (x, y);
                if (name is "translate" or "scaled-curve" or "empty" or "decimal-curve")
                {
                    var dx = 40d * 640 / rect.Width;
                    var dy = 30d * 480 / rect.Height;
                    mapping = (x, y) => (x + dx, y + dy);
                    AssertGeometryTag(texts[index], "pos", px + dx, py + dy);
                    AssertGeometryTag(texts[index], "org", ox + dx, oy + dy);
                }
                if (name == "off")
                {
                    AssertGeometryTag(texts[index], "pos", px + 40d * 640 / rect.Width, py + 30d * 480 / rect.Height);
                    AssertGeometryTag(texts[index], "org", ox, oy);
                }
                if (name == "move")
                {
                    var dx = Pixel(360, 240).X * 640d / rect.Width - 300;
                    var dy = Pixel(360, 240).Y * 480d / rect.Height - 200;
                    mapping = (x, y) => (x + dx, y + dy);
                    AssertGeometryTag(texts[index], "move", px + dx, py + dy, (index == 0 ? 360 : 470) + dx, (index == 0 ? 250 : 370) + dy, index == 0 ? 400 : 200, index == 0 ? 1600 : 1800);
                }
                if (name is "origin" or "origin-xy")
                {
                    var dx = 30d * 640 / rect.Width;
                    var dy = 20d * 480 / rect.Height;
                    mapping = (x, y) => (x + dx - dy, y + dx + dy);
                    AssertGeometryTag(texts[index], "org", ox + dx, oy + dy);
                }
                if (name == "endpoint-no-geometry")
                {
                    var dx = 40d * 640 / rect.Width;
                    var dy = 30d * 480 / rect.Height;
                    AssertGeometryTag(texts[index], "move", px + dx, py + dy, index == 0 ? 360 : 470, index == 0 ? 250 : 370, index == 0 ? 0 : -200, index == 0 ? 1600 : 1800);
                }
                if (name == "mixed-static-endpoint")
                {
                    var dx = 40d * 640 / rect.Width;
                    var dy = 30d * 480 / rect.Height;
                    if (index == 0)
                    {
                        mapping = (x, y) => (x + dx, y + dy);
                        AssertGeometryTag(texts[index], "pos", px + dx, py + dy);
                        AssertGeometryTag(texts[index], "org", ox + dx, oy + dy);
                    }
                    else
                    {
                        AssertGeometryTag(texts[index], "move", px + dx, py + dy, 470, 370, 0, 1600);
                        if (Regex.IsMatch(texts[index], @"\\(?:pos|org|i?clip)\("))
                            throw new InvalidOperationException("Bare move endpoint unexpectedly gained attached geometry or position");
                    }
                }
                if (name == "clip-only")
                {
                    AssertGeometryTag(texts[index], "pos", px, py);
                    AssertGeometryTag(texts[index], "org", ox, oy);
                    AssertGeometryTag(texts[index], "frz", 90);
                    AssertGeometryTag(texts[index], "fscx", index == 0 ? 100 : 150);
                    AssertGeometryTag(texts[index], "fscy", index == 0 ? 100 : 80);
                }
                if (name is "scale" or "scale-shear" or "normalize" or "drawing-tail" or "scale-layout" or "scale-nudge" or "scale-curve" or "drawing-tail-vertical" or "implicit-integer" or "implicit-no-geometry")
                {
                    var sx = index == 0 ? (name == "normalize" ? 200d : 100d) : 150d;
                    var sy = index == 0 ? (name == "normalize" ? 50d : 100d) : 80d;
                    var nx = name == "normalize" ? 100d : name == "scale-nudge" ? 110d : 200d;
                    var ny = name == "normalize" ? sy / sx * 100 : name == "scale-nudge" ? 100d : 50d;
                    AssertGeometryTag(texts[index], "fscx", nx);
                    AssertGeometryTag(texts[index], "fscy", ny);
                    if (name is "implicit-integer" or "implicit-no-geometry")
                    {
                        if (Regex.IsMatch(texts[index], @"\\(?:pos|move)\("))
                            throw new InvalidOperationException("Scale unexpectedly materialized an implicit position");
                    }
                    else
                        AssertGeometryTag(texts[index], "pos", px, py);
                    if (name == "implicit-no-geometry")
                    {
                        if (Regex.IsMatch(texts[index], @"\\(?:org|i?clip)\("))
                            throw new InvalidOperationException("Ordinary scale unexpectedly added attached geometry");
                    }
                    else
                        AssertGeometryTag(texts[index], "org", ox, oy);
                    var a = (name == "scale-shear" ? -30d : -90d) * Math.PI / 180;
                    var cos = Math.Cos(a);
                    var sin = Math.Sin(a);
                    var metric = name == "scale-layout" ? 2d : 1d;
                    var ax = ox + cos * (px - ox) - sin * (py - oy) / metric;
                    var ay = oy + sin * (px - ox) * metric + cos * (py - oy);
                    mapping = (x, y) => { var vx = (x - ax) * metric; var vy = y - ay; var lx = (cos * vx + sin * vy) * nx / sx; var ly = (-sin * vx + cos * vy) * ny / sy; return (ax + (cos * lx - sin * ly) / metric, ay + sin * lx + cos * ly); };
                }
                if (name == "rotate")
                {
                    var angle = -15d * Math.PI / 180;
                    mapping = (x, y) => (ox + Math.Cos(angle) * (x - ox) - Math.Sin(angle) * (y - oy), oy + Math.Sin(angle) * (x - ox) + Math.Cos(angle) * (y - oy));
                    AssertGeometryTag(texts[index], "frz", 105);
                }
                if (name == "nudge-origin")
                {
                    mapping = (x, y) => (x + 10, y - 10);
                    AssertGeometryTag(texts[index], "org", ox, oy - 10);
                }
                if (name == "xy-no-clip")
                    AssertGeometryTag(texts[index], "fry", 15);
                if (name is not "xy-no-clip" and not "endpoint-no-geometry" and not "clip-only" and not "implicit-no-geometry" && !(name == "mixed-static-endpoint" && index == 1))
                    AssertGeometryClip(old, texts[index], mapping);
            }
            Capture("after");
            Undo();
            AssertContractEqual(baseline, Save("undo"), "One Undo restores all selected lines exactly");

            Command("Redo");
            Thread.Sleep(300);
            AssertContractEqual(actual, Save("redo"), "One Redo restores the complete linked edit");
        }
        isolation.VerifyWindow(hwnd, process);
        Close();
        var actualProviderEvidence = ReadGeometryProviderEvidence(profilePath);
        if (actualProviderEvidence.Count == 0 || actualProviderEvidence.Any(evidence => evidence.Provider != options.SubtitleProvider))
            throw new InvalidOperationException("Actual host did not exclusively select the requested renderer");
        var actualEvidenceKeys = actualProviderEvidence.Select(evidence => (evidence.LogFile, evidence.LineNumber)).ToHashSet();
        IReadOnlyList<GeometryProviderEvidence> expectedProviderEvidence = [];
        if (name == "scale")
        {
            var expectedTags1 = @"\pos(300,200)\org(260,180)\frz90\fscx200\fscy50\clip(270,60,310,220)";
            var expectedTags2 = @"\pos(420,320)\org(390,300)\frz90\fscx200\fscy50\iclip(6,m 12320 6933 l 13920 6933 13920 10773 12320 10773)";
            var expectedAss = Path.Combine(options.Artifacts, "expected.ass");
            File.WriteAllText(expectedAss, File.ReadAllText(Path.Combine(options.Artifacts, "before.ass")).Replace(firstTags, expectedTags1, StringComparison.Ordinal).Replace(secondTags, expectedTags2, StringComparison.Ordinal), new UTF8Encoding(false));
            var expectedHost = Path.Combine(options.Artifacts, "expected-host");
            Directory.CreateDirectory(expectedHost);
            var expectedStart = new ProcessStartInfo(options.Executable) { UseShellExecute = false, WorkingDirectory = Environment.CurrentDirectory, WindowStyle = ProcessWindowStyle.Minimized };
            expectedStart.Environment["AEGISUB_ENABLE_SKIA_VIDEO_TOOLS"] = "0";
            foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", profilePath, "--artifacts", expectedHost, "--open", expectedAss, "--open", options.Video })
                AddArgument(expectedStart, argument);
            process = isolation.Start(expectedStart, "expected-host");
            AutomationProtocol.WaitForReadyArtifact(Path.Combine(expectedHost, "ready.json"), process, TimeSpan.FromSeconds(options.TimeoutSeconds));
            hwnd = isolation.MainWindow(process, options.TimeoutSeconds);
            failureMain = hwnd;
            window = AutomationElement.FromHandle(hwnd);
            isolation.VerifyWindow(hwnd, process);
            ShowWindow(hwnd, NativeConstants.ShowNoActivate);
            SetWindowPos(hwnd, 0, 40, 40, options.WindowWidth, options.WindowHeight, SetWindowPosFlags.NoActivate | SetWindowPosFlags.ShowWindow);
            Thread.Sleep(options.SettleMilliseconds);
            canvas = Canvas();
            Capture("expected");
            isolation.VerifyWindow(hwnd, process);
            Close();
            using var afterImage = new System.Drawing.Bitmap(Path.Combine(options.Artifacts, "after-frame.png"));
            using var expectedImage = new System.Drawing.Bitmap(Path.Combine(options.Artifacts, "expected-frame.png"));
            if (afterImage.Size != expectedImage.Size)
                throw new InvalidOperationException("Expected render dimensions differ");
            var differingPixels = 0;
            for (var y = 0; y < afterImage.Height; ++y)
                for (var x = 0; x < afterImage.Width; ++x)
                    if (afterImage.GetPixel(x, y) != expectedImage.GetPixel(x, y))
                        ++differingPixels;
            File.WriteAllText(Path.Combine(options.Artifacts, "pixel-oracle.json"), JsonSerializer.Serialize(new
            {
                differing_pixels = differingPixels,
                allowed_pixels = 0,
                oracle = "independently authored fixed expected ASS"
            }));
            if (differingPixels != 0)
                throw new InvalidOperationException("Independent expected render differs at " + differingPixels + " pixels");
        }
        if (name == "scale")
        {
            expectedProviderEvidence = ReadGeometryProviderEvidence(profilePath).Where(evidence => !actualEvidenceKeys.Contains((evidence.LogFile, evidence.LineNumber))).ToArray();
            if (expectedProviderEvidence.Count == 0 || expectedProviderEvidence.Any(evidence => evidence.Provider != options.SubtitleProvider))
                throw new InvalidOperationException("Expected-render host did not exclusively select the requested renderer");
        }
        var provider = ReadSelectedSubtitleProvider(profilePath);
        if (!provider.Equals(options.SubtitleProvider, StringComparison.Ordinal))
            throw new InvalidOperationException("Unexpected renderer: " + provider);
        File.WriteAllText(Path.Combine(options.Artifacts, "geometry-result.json"), JsonSerializer.Serialize(new
        {
            scenario = name,
            passed = true,
            provider,
            provider_evidence = new
            {
                actual_host = actualProviderEvidence,
                expected_host = expectedProviderEvidence
            },
            canvas = new
            {
                rect.Width,
                rect.Height
            },
            total_timeout_seconds = 180
        }, new JsonSerializerOptions { WriteIndented = true }));
        return 0;
    }
    catch (Exception error)
    {
        File.WriteAllText(Path.Combine(options.Artifacts, "geometry-result.json"), JsonSerializer.Serialize(new
        {
            scenario = name,
            passed = false,
            error = error.ToString(),
            total_timeout_seconds = 180
        }, new JsonSerializerOptions { WriteIndented = true }));
        if (process is { HasExited: false })
        {
            try
            {
                var windows = isolation.Windows(process);
                File.WriteAllText(Path.Combine(options.Artifacts, "failure-native.txt"), string.Join('\n', windows.SelectMany(target => EnumerateChildWindows(target).Prepend(target)).Select(target => NativeClassName(target) + ":" + isolation.ReadText(target, process))));
            }
            catch (Exception secondary) { File.AppendAllText(Path.Combine(options.Artifacts, "secondary-diagnostics.txt"), secondary + Environment.NewLine); }
            try
            {
                isolation.VerifyWindow(failureMain, process);
                ScreenCapture.SaveWindowPng(failureMain, Path.Combine(options.Artifacts, "failure-main-window.png"));
            }
            catch (Exception secondary) { File.AppendAllText(Path.Combine(options.Artifacts, "secondary-diagnostics.txt"), secondary + Environment.NewLine); }
        }
        throw;
    }
    finally { if (process is { HasExited: false }) isolation.Stop(process); }
}

static void ClickGeometryToolbar(AutomationElement window, Process process, string tooltip, string artifacts, PrivateDesktop isolation)
{
    isolation.VerifyWindow(new nint(window.Current.NativeWindowHandle), process);
    var target = window.FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().Single(element => element.Current.IsEnabled && element.Current.ControlType == ControlType.Button && element.Current.Name.Contains(tooltip, StringComparison.OrdinalIgnoreCase));
    var rectangle = target.Current.BoundingRectangle;
    if (rectangle.IsEmpty || rectangle.Width <= 0 || rectangle.Height <= 0 || target.Current.IsOffscreen)
        throw new InvalidOperationException("Toolbar target is not visible: " + tooltip);
    nint toolbar = 0;
    for (var parent = TreeWalker.RawViewWalker.GetParent(target); parent is not null; parent = TreeWalker.RawViewWalker.GetParent(parent))
    {
        if (parent.Current.ControlType != ControlType.ToolBar || parent.Current.NativeWindowHandle == 0)
            continue;
        toolbar = new nint(parent.Current.NativeWindowHandle);
        break;
    }
    if (toolbar == 0 || NativeClassName(toolbar) != "ToolbarWindow32")
        throw new InvalidOperationException("Target does not belong to a native toolbar: " + tooltip);
    if (GetWindowThreadProcessId(toolbar, out var owner) == 0 || owner != process.Id)
        throw new InvalidOperationException("Toolbar is not owned by the current host");
    var point = new NativePoint { X = (int)Math.Round(rectangle.Left + rectangle.Width / 2), Y = (int)Math.Round(rectangle.Top + rectangle.Height / 2) };
    if (!ScreenToClient(toolbar, ref point) || !GetClientRect(toolbar, out var client) || point.X < client.Left || point.X >= client.Right || point.Y < client.Top || point.Y >= client.Bottom)
        throw new InvalidOperationException("Observed toolbar center is outside its client range");
    File.AppendAllText(Path.Combine(artifacts, "toolbar-evidence.ndjson"), JsonSerializer.Serialize(new
    {
        phase = "single-actuation",
        target = target.Current.Name,
        owner,
        toolbar = $"0x{toolbar.ToInt64():X}",
        screen = new
        {
            rectangle.Left,
            rectangle.Top,
            rectangle.Width,
            rectangle.Height
        },
        client = new
        {
            point.X,
            point.Y
        }
    }) + Environment.NewLine);
    var location = new PointI(point.X, point.Y);
    isolation.VerifyWindow(toolbar, process);
    SendMouseToWindow(toolbar, WindowMessage.MouseMove, location, 0);
    isolation.VerifyWindow(toolbar, process);
    SendMouseToWindow(toolbar, WindowMessage.LeftButtonDown, location, NativeConstants.MouseKeyLeftButton);
    isolation.VerifyWindow(toolbar, process);
    SendMouseToWindow(toolbar, WindowMessage.LeftButtonUp, location, 0);
}

static IReadOnlyList<GeometryProviderEvidence> ReadGeometryProviderEvidence(string profilePath)
{
    var records = new List<GeometryProviderEvidence>();
    var logRoot = Path.Combine(profilePath, "user", "log");
    const string prefix = "Selected subtitles provider: ";
    foreach (var path in Directory.EnumerateFiles(logRoot, "*.ndjson"))
    {
        var lineNumber = 0;
        foreach (var line in File.ReadLines(path))
        {
            ++lineNumber;
            using var entry = JsonDocument.Parse(line);
            if (entry.RootElement.TryGetProperty("section", out var section) && section.GetString() == "subtitle/provider/select"
                && entry.RootElement.TryGetProperty("message", out var message) && message.GetString() is { } text && text.StartsWith(prefix, StringComparison.Ordinal))
                records.Add(new GeometryProviderEvidence(Path.GetFileName(path), lineNumber, text[prefix.Length..]));
        }
    }
    return records;
}

static void AssertGeometryTag(string text, string tag, params double[] expected)
{
    var match = Regex.Match(text, @"\\" + tag + @"(?:\((?<args>[^)]*)\)|(?<args>[-+\d.eE]+))");
    if (!match.Success || Regex.Matches(text, @"\\" + tag + @"(?:\(|[-+\d])").Count != 1)
        throw new InvalidOperationException("Missing or duplicated geometry tag: " + tag);
    var values = match.Groups["args"].Value.Split(',').Select(value => double.Parse(value, CultureInfo.InvariantCulture)).ToArray();
    if (values.Length != expected.Length || values.Zip(expected).Any(pair => Math.Abs(pair.First - pair.Second) > .001))
        throw new InvalidOperationException(tag + " expected " + string.Join(',', expected) + " actual " + string.Join(',', values));
}

static void AssertGeometryClip(string source, string actual, Func<double, double, (double X, double Y)> mapping)
{
    (bool Inverse, string Commands, List<(double X, double Y)> Points) Read(string text)
    {
        var match = Regex.Match(text, @"\\(?<inverse>i?)clip\((?<args>[^)]*)\)");
        if (!match.Success)
            throw new InvalidOperationException("Missing linked clip");
        var args = match.Groups["args"].Value;
        var pieces = args.Split(',');
        var points = new List<(double, double)>();
        var numeric = pieces.Select(value => double.TryParse(value, NumberStyles.Float, CultureInfo.InvariantCulture, out var number) ? (double?)number : null).ToArray();
        if (pieces.Length == 4 && numeric.All(value => value.HasValue))
        {
            var x1 = numeric[0]!.Value;
            var y1 = numeric[1]!.Value;
            var x2 = numeric[2]!.Value;
            var y2 = numeric[3]!.Value;
            if (x2 > x1 && y2 > y1)
                points.AddRange([(x1, y1), (x2, y1), (x2, y2), (x1, y2)]);
            return (match.Groups["inverse"].Value == "i", "rectangle", points);
        }
        var scale = pieces.Length == 2 ? int.Parse(pieces[0], CultureInfo.InvariantCulture) : 1;
        var shape = pieces[^1];
        var coordinates = Regex.Matches(shape, @"[-+]?\d+(?:\.\d+)?").Select(value =>
        {
            var raw = double.Parse(value.Value, CultureInfo.InvariantCulture) * 64;
            var nearest = Math.Round(raw, MidpointRounding.ToEven);
            var truncated = Math.Truncate(raw);
            if (nearest != truncated)
                throw new InvalidOperationException("Oracle source has renderer-divergent fixed-point coordinates");
            return nearest / 64 * Math.Pow(2, 1 - scale);
        }).ToArray();
        for (var i = 0; i < coordinates.Length; i += 2)
            points.Add((coordinates[i], coordinates[i + 1]));
        return (match.Groups["inverse"].Value == "i", string.Join(' ', Regex.Matches(shape, "[mnlbspc]").Select(value => value.Value)), points);
    }
    var before = Read(source);
    var after = Read(actual);
    if (before.Inverse != after.Inverse || before.Points.Count != after.Points.Count)
        throw new InvalidOperationException("Clip polarity or point count changed");
    if (before.Commands != "rectangle" && before.Commands != after.Commands)
        throw new InvalidOperationException("Drawing contour/curve commands changed");
    for (var i = 0; i < before.Points.Count; ++i)
    {
        var target = mapping(before.Points[i].X, before.Points[i].Y);
        var found = after.Points[i];
        if (Math.Abs(target.X - found.X) > .016 || Math.Abs(target.Y - found.Y) > .016)
            throw new InvalidOperationException($"Clip point {i}: expected {target}, actual {found}");
    }
}








static int RunInputContract(DriverOptions options)
{
    var rapid = options.InputContract.StartsWith("rapid-", StringComparison.Ordinal);
    var scenario = InputContractCase.Get(rapid ? options.InputContract[6..] : options.InputContract);
    if (rapid && (scenario.Hold || scenario.Freehand || scenario.Name == "vector-node"))
        throw new ArgumentException("Rapid regrab requires a paired-snapshot feature tool");
    PrepareArtifactsDirectory(options.Artifacts);
    var fixturePath = Path.Combine(options.Artifacts, "scenario.ass");
    var profilePath = Path.Combine(options.Artifacts, "profile");
    var hostArtifactsPath = Path.Combine(options.Artifacts, "host");
    var resultPath = Path.Combine(options.Artifacts, "input-contract-result.json");
    var fixture = scenario.BuildFixture();
    if (rapid)
        fixture = fixture.Replace(@"\bord2.5\shad1.25", @"\frx63\fry86\fs64\blur6\bord6\shad3", StringComparison.Ordinal);
    File.WriteAllText(fixturePath, fixture, new UTF8Encoding(false));
    WriteProfileConfig(profilePath, options.SubtitleProvider);
    Directory.CreateDirectory(hostArtifactsPath);
    Process? process = null;
    var results = new List<string>();
    var rapidChecks = new List<(string Expected, string Actual, string Label)>();
    var rapidAttempted = false;
    var rapidWitnessChecked = false;
    var totalTimeout = TimeSpan.FromSeconds(Math.Max(120, options.TimeoutSeconds * 6));
    using var watchdog = new Timer(_ =>
    {
        File.WriteAllText(resultPath, JsonSerializer.Serialize(new
        {
            scenario = scenario.Name, passed = false, error = "total-timeout",
            total_timeout_seconds = totalTimeout.TotalSeconds,
        }));
        try { process?.Kill(entireProcessTree: true); }
        finally { Environment.Exit(124); }
    }, null, totalTimeout, Timeout.InfiniteTimeSpan);
    try
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = options.Executable,
            WorkingDirectory = Environment.CurrentDirectory,
            UseShellExecute = false,
            WindowStyle = ProcessWindowStyle.Minimized,
        };
        startInfo.Environment["AEGISUB_ENABLE_SKIA_VIDEO_TOOLS"] = "0";
        startInfo.Environment["AEGISUB_PERF_TRACE"] = "video,visual-tool-input-contract";
        foreach (var value in new[] { "--gui-test", "host", "--profile-dir", profilePath,
            "--artifacts", hostArtifactsPath, "--open", fixturePath, "--open", options.Video })
            AddArgument(startInfo, value);
        process = Process.Start(startInfo) ?? throw new InvalidOperationException("Could not start Aegisub");
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(hostArtifactsPath, "ready.json"),
            process, TimeSpan.FromSeconds(options.TimeoutSeconds));
        var mainWindow = UiaDriver.WaitForMainWindow(process, TimeSpan.FromSeconds(options.TimeoutSeconds));
        var mainHwnd = new nint(mainWindow.Current.NativeWindowHandle);
        ShowWindow(mainHwnd, NativeConstants.ShowNoActivate);
        if (!SetWindowPos(mainHwnd, 0, 40, 40, options.WindowWidth, options.WindowHeight,
                SetWindowPosFlags.NoActivate | SetWindowPosFlags.ShowWindow))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "SetWindowPos failed");
        Thread.Sleep(rapid ? Math.Max(2500, options.SettleMilliseconds) : options.SettleMilliseconds);
        SelectVisibleDialogueLines(mainWindow);
        SelectVisualTool(mainWindow, scenario.Tooltip);
        Thread.Sleep(250);
        var canvas = WaitForVideoCanvas(process, options.TimeoutSeconds);
        if (!GetClientRect(canvas, out var rect) || rect.Width < 320 || rect.Height < 240
            || Math.Abs(rect.Width / (double)rect.Height - 640.0 / 480.0) > 0.005)
            throw new InvalidOperationException("Input contracts require a 4:3 canvas of at least 320x240");
        PointI Pixel(PointI point) => new(
            (int)Math.Round(point.X * rect.Width / 640.0),
            (int)Math.Round(point.Y * rect.Height / 480.0));
        var start = Pixel(scenario.Start);
        var middle = new PointI(start.X + 23, start.Y - 11);
        var terminal = new PointI(start.X + 47, start.Y + 18);
        EnsureSaved(mainWindow, process, fixturePath, options.TimeoutSeconds);
        var initial = ContractDocument(fixturePath);
        File.Copy(fixturePath, Path.Combine(options.Artifacts, "initial.ass"));
        if (rapid)
        {
            ScreenCapture.SaveWindowPng(mainWindow, Path.Combine(options.Artifacts, "rapid-initial-window.png"));
            Console.WriteLine("visual_tool_input_contract.initial_capture=setup_diagnostic_only_not_pixel_coherence_evidence");
        }

        string Save(string name)
        {
            SendWindowMessage(canvas, WindowMessage.Null, 0, 0);
            EnsureSaved(mainWindow, process, fixturePath, options.TimeoutSeconds);
            File.Copy(fixturePath, Path.Combine(options.Artifacts, name + ".ass"));
            return ContractDocument(fixturePath);
        }
        void Restore(string name)
        {
            InvokeUndo(mainWindow, process, options.TimeoutSeconds);
            var restored = Save(name + "-undo");
            AssertContractEqual(initial, restored, name + ": one Undo restores original document");
            Thread.Sleep(250);
        }
        void Gesture(PointI end, bool endpointMotion, bool shift)
        {
            var modifiers = shift ? NativeConstants.MouseKeyShift : 0;
            SendMouseToWindow(canvas, WindowMessage.MouseMove, start, modifiers);
            SendMouseToWindow(canvas, WindowMessage.LeftButtonDown, start,
                NativeConstants.MouseKeyLeftButton | modifiers);
            WaitForCapture(canvas, true, process, options.TimeoutSeconds);
            SendMouseToWindow(canvas, WindowMessage.MouseMove, middle,
                NativeConstants.MouseKeyLeftButton | modifiers);
            if (endpointMotion)
                SendMouseToWindow(canvas, WindowMessage.MouseMove, end,
                    NativeConstants.MouseKeyLeftButton | modifiers);
            SendMouseToWindow(canvas, WindowMessage.LeftButtonUp, end, modifiers);
            WaitForCapture(canvas, false, process, options.TimeoutSeconds);
        }

        if (rapid)
        {
            var firstEnd = new PointI(start.X + 81, start.Y + 35);
            var secondEnd = new PointI(start.X + 13, start.Y - 6);
            void CompletedDrag(PointI from, PointI to)
            {
                SendMouseToWindow(canvas, WindowMessage.MouseMove, from, 0);
                SendMouseToWindow(canvas, WindowMessage.LeftButtonDown, from, NativeConstants.MouseKeyLeftButton);
                WaitForCapture(canvas, true, process, options.TimeoutSeconds);
                SendMouseToWindow(canvas, WindowMessage.MouseMove, to, NativeConstants.MouseKeyLeftButton);
                SendMouseToWindow(canvas, WindowMessage.LeftButtonUp, to, 0);
                WaitForCapture(canvas, false, process, options.TimeoutSeconds);
            }
            CompletedDrag(start, firstEnd);
            var firstEdit = Save("sequential-first");
            AssertContractUnrelated(initial, firstEdit, scenario.TagPattern);
            if (firstEdit == initial)
                throw new InvalidOperationException("Reference first drag did not change the intended feature");
            Thread.Sleep(2500);
            CompletedDrag(firstEnd, new PointI(firstEnd.X + secondEnd.X - start.X,
                firstEnd.Y + secondEnd.Y - start.Y));
            var secondEdit = Save("sequential-second");
            if (secondEdit == firstEdit)
                throw new InvalidOperationException("Reference second drag did not accumulate a delta");
            InvokeUndo(mainWindow, process, options.TimeoutSeconds);
            AssertContractEqual(firstEdit, Save("sequential-undo-second"), "Reference second Undo boundary");
            Restore("sequential-first");
            Thread.Sleep(2500);

            rapidAttempted = true;
            SendMouseToWindow(canvas, WindowMessage.MouseMove, start, 0);
            SendMouseToWindow(canvas, WindowMessage.LeftButtonDown, start, NativeConstants.MouseKeyLeftButton);
            SendMouseToWindow(canvas, WindowMessage.MouseMove, firstEnd, NativeConstants.MouseKeyLeftButton);
            SendMouseToWindow(canvas, WindowMessage.LeftButtonUp, firstEnd, 0);
            SendMouseToWindow(canvas, WindowMessage.LeftButtonDown, start, NativeConstants.MouseKeyLeftButton);
            SendMouseToWindow(canvas, WindowMessage.MouseMove, secondEnd, NativeConstants.MouseKeyLeftButton);
            SendMouseToWindow(canvas, WindowMessage.LeftButtonUp, secondEnd, 0);
            WaitForCapture(canvas, false, process, options.TimeoutSeconds);
            rapidChecks.Add((secondEdit, Save("rapid-second"), "Displayed feature regrab accumulates against latest model"));
            InvokeUndo(mainWindow, process, options.TimeoutSeconds);
            var afterFirstUndo = Save("rapid-undo-second");
            rapidChecks.Add((firstEdit, afterFirstUndo, "Rapid second drag has its own Undo boundary"));
            if (afterFirstUndo != initial)
            {
                InvokeUndo(mainWindow, process, options.TimeoutSeconds);
                rapidChecks.Add((initial, Save("rapid-first-undo"), "Rapid first drag has its own Undo boundary"));
            }
        }
        else if (scenario.Freehand)
        {
            SelectVisualTool(mainWindow, scenario.Submode!);
            var points = new[] { start, new PointI(start.X + 75, start.Y + 8),
                new PointI(start.X + 80, start.Y + 84), new PointI(start.X + 160, start.Y + 70) };
            var end = new PointI(points[^1].X + 11, points[^1].Y - 7);
            SendMouseToWindow(canvas, WindowMessage.MouseMove, start, 0);
            SendMouseToWindow(canvas, WindowMessage.LeftButtonDown, start, NativeConstants.MouseKeyLeftButton);
            WaitForCapture(canvas, true, process, options.TimeoutSeconds);
            foreach (var point in points.Skip(1))
                SendMouseToWindow(canvas, WindowMessage.MouseMove, point, NativeConstants.MouseKeyLeftButton);
            SendMouseToWindow(canvas, WindowMessage.LeftButtonUp, end, 0);
            WaitForCapture(canvas, false, process, options.TimeoutSeconds);
            var actual = Save("freehand-short-release-tail");
            AssertContractUnrelated(initial, actual, scenario.TagPattern);
            AssertFreehandEndpoint(actual, end.X * 640.0 / rect.Width, end.Y * 480.0 / rect.Height);
            Restore("freehand-short-release-tail");
            results.Add("FreehandShortReleaseTailPreservesEndpointAndUndo");
        }
        else
        {
            foreach (var variant in scenario.Hold
                ? new[] { "endpoint" }
                : new[] { "endpoint", "return-to-start", "shift-endpoint" })
            {
                var end = variant == "return-to-start" ? start : terminal;
                var shift = variant == "shift-endpoint";
                Gesture(end, endpointMotion: true, shift);
                var reference = Save(variant + "-reference");
                AssertContractUnrelated(initial, reference, scenario.TagPattern);
                if (variant != "return-to-start" && reference == initial)
                    throw new InvalidOperationException(variant + ": reference did not edit the intended subtitle");
                Restore(variant + "-reference");
                Gesture(end, endpointMotion: false, shift);
                var actual = Save(variant + "-release-gap");
                AssertContractEqual(reference, actual, variant + ": distinct release equals explicit terminal motion");
                Restore(variant + "-release-gap");
                results.Add(variant switch
                {
                    "endpoint" => "DistinctReleaseEqualsTerminalMotionAndUndo",
                    "return-to-start" => "ReleaseBackToStartEqualsTerminalMotionAndUndo",
                    _ => "ShiftReleaseEqualsTerminalMotionAndUndo",
                });
            }
            if (!scenario.Hold)
            {
                SendMouseToWindow(canvas, WindowMessage.MouseMove, start, 0);
                SendMouseToWindow(canvas, WindowMessage.LeftButtonDown, start, NativeConstants.MouseKeyLeftButton);
                WaitForCapture(canvas, true, process, options.TimeoutSeconds);
                SendMouseToWindow(canvas, WindowMessage.LeftButtonUp, start, 0);
                WaitForCapture(canvas, false, process, options.TimeoutSeconds);
                AssertContractEqual(initial, Save("no-motion-click"), "No-motion feature click preserves ASS exactly");
                results.Add("NoMotionFeatureClickPreservesDocument");
            }
        }
        RequestCleanClose(process, mainHwnd, options.TimeoutSeconds);
        if (rapid)
        {
            rapidWitnessChecked = true;
            AssertRapidRegrabWitness(profilePath, options.Artifacts);
            foreach (var check in rapidChecks)
                AssertContractEqual(check.Expected, check.Actual, check.Label);
            if (rapidChecks.Count != 3)
                throw new InvalidOperationException("Rapid regrab did not produce two independent Undo boundaries");
            results.Add("DisplayedFeatureRapidRegrabAccumulatesLatestModelWithTwoUndos");
        }
        var provider = ReadSelectedSubtitleProvider(profilePath);
        if (!string.Equals(provider, options.SubtitleProvider, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException($"Requested provider {options.SubtitleProvider}, observed {provider}");
        File.WriteAllText(resultPath, JsonSerializer.Serialize(new
        {
            version = 1, scenario = scenario.Name, passed = true, tests = results,
            subtitle_provider = provider, canvas = new { width = rect.Width, height = rect.Height },
            total_timeout_seconds = totalTimeout.TotalSeconds,
        }, new JsonSerializerOptions { WriteIndented = true }));
        Console.WriteLine($"visual_tool_input_contract.scenario={scenario.Name}");
        Console.WriteLine($"visual_tool_input_contract.passed={string.Join(',', results)}");
        return 0;
    }
    catch (Exception error)
    {
        if (process is not null && !process.HasExited && process.MainWindowHandle != 0)
        {
            try { RequestCleanClose(process, process.MainWindowHandle, 3); }
            catch (Exception closeError) { Console.Error.WriteLine("visual_tool_input_contract.failure_close=" + closeError.Message); }
        }
        var reportedError = error;
        if (rapid && rapidAttempted && !rapidWitnessChecked && process is { HasExited: true })
        {
            try { AssertRapidRegrabWitness(profilePath, options.Artifacts); }
            catch (Exception witnessError) { reportedError = witnessError; }
        }
        File.WriteAllText(resultPath, JsonSerializer.Serialize(new
        {
            version = 1, scenario = scenario.Name, passed = false, completed_tests = results,
            error = reportedError.Message,
            failure_kind = reportedError.Message.StartsWith("Rapid regrab prerequisite:", StringComparison.Ordinal)
                ? "prerequisite-inconclusive" : "contract-or-harness-failure",
        }, new JsonSerializerOptions { WriteIndented = true }));
        if (!ReferenceEquals(reportedError, error))
            throw new InvalidOperationException(reportedError.Message, error);
        throw;
    }
    finally
    {
        if (process is not null)
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
                if (!process.WaitForExit(5000))
                    Console.Error.WriteLine("visual_tool_input_contract.cleanup_timeout=true");
            }
            process.Dispose();
        }
    }
}

static string ContractDocument(string path)
{
    var lines = File.ReadAllLines(path);
    var start = Array.FindIndex(lines, line => line == "[V4+ Styles]");
    if (start < 0)
        throw new InvalidOperationException("Saved ASS lacks its style/event document");
    return string.Join('\n', lines.Skip(start));
}

static void AssertContractEqual(string expected, string actual, string label)
{
    if (expected == actual)
        return;
    var referenceLines = expected.Split('\n');
    var actualLines = actual.Split('\n');
    var line = Enumerable.Range(0, Math.Min(referenceLines.Length, actualLines.Length))
        .FirstOrDefault(index => referenceLines[index] != actualLines[index], -1);
    throw new InvalidOperationException($"{label}: document differs at line {line + 1}" +
        (line < 0 ? " (different line count)" : $"; expected [{referenceLines[line]}], actual [{actualLines[line]}]"));
}

static void AssertContractUnrelated(string initial, string actual, string tagPattern)
{
    var expectedTagCount = 0;
    string Neutral(string document)
    {
        var lines = document.Split('\n');
        var first = Array.FindIndex(lines, line => line.StartsWith("Dialogue:", StringComparison.Ordinal));
        if (first < 0)
            throw new InvalidOperationException("Input fixture has no dialogue");
        var count = Regex.Matches(lines[first], tagPattern, RegexOptions.CultureInvariant).Count;
        if (count == 0 || (expectedTagCount != 0 && count != expectedTagCount))
            throw new InvalidOperationException("Edited tag was removed or duplicated");
        expectedTagCount = count;
        lines[first] = Regex.Replace(lines[first], tagPattern, "", RegexOptions.CultureInvariant);
        return string.Join('\n', lines);
    }
    AssertContractEqual(Neutral(initial), Neutral(actual), "Unrelated tags, styles, and dialogues are preserved");
}

static void AssertFreehandEndpoint(string document, double x, double y)
{
    var primary = document.Split('\n').First(line => line.StartsWith("Dialogue:", StringComparison.Ordinal));
    var clip = Regex.Match(primary, @"\\i?clip\((?<path>[^)]*)\)", RegexOptions.CultureInvariant);
    var numbers = Regex.Matches(clip.Groups["path"].Value, @"-?\d+(?:\.\d+)?", RegexOptions.CultureInvariant)
        .Select(match => double.Parse(match.Value, CultureInfo.InvariantCulture)).ToArray();
    if (!clip.Success || numbers.Length < 8 || Math.Abs(numbers[^2] - x) > 1.0 || Math.Abs(numbers[^1] - y) > 1.0)
        throw new InvalidOperationException($"Freehand release endpoint was omitted: expected ({x:F2},{y:F2}), clip [{clip.Value}]");
}

static void AssertRapidRegrabWitness(string profilePath, string artifacts)
{
    var evidence = new Dictionary<string, object?> { ["passed"] = false };
    void Publish() => File.WriteAllText(Path.Combine(artifacts, "rapid-witness.json"),
        JsonSerializer.Serialize(evidence, new JsonSerializerOptions { WriteIndented = true }));
    void Require(bool condition, string message)
    {
        if (condition) return;
        evidence["error"] = message;
        Publish();
        throw new InvalidOperationException("Rapid regrab prerequisite: " + message);
    }
    var traces = Directory.GetFiles(Path.Combine(profilePath, "user", "perf-sessions"),
        "trace.ndjson", SearchOption.AllDirectories);
    Require(traces.Length == 1, $"expected exactly one trace, found {traces.Length}");
    var entries = File.ReadLines(traces[0]).Select(line =>
    {
        using var document = JsonDocument.Parse(line);
        var root = document.RootElement;
        var payload = root.GetProperty("payload");
        return new RapidTraceEntry
        {
            time = root.GetProperty("t_monotonic_ns").GetInt64(),
            phase = payload.TryGetProperty("phase", out var phase) ? phase.GetString() : null,
            stage = payload.TryGetProperty("stage", out var stage) ? stage.GetString() : null,
            interaction = payload.TryGetProperty("interaction_id", out var interaction) ? interaction.GetUInt64() : 0UL,
            delivery = payload.TryGetProperty("delivery_class", out var delivery) ? delivery.GetInt32() : -1,
            provider = payload.TryGetProperty("provider_version", out var provider) ? provider.GetUInt64() : 0UL,
            content = payload.TryGetProperty("content_version", out var content) ? content.GetUInt64() : 0UL,
            request = payload.TryGetProperty("request_version", out var request) ? request.GetUInt64() : 0UL,
        };
    }).ToArray();
    var downs = entries.Where(entry => entry.phase == "visual_tool.input.down").ToArray();
    var ups = entries.Where(entry => entry.phase == "visual_tool.input.up").ToArray();
    evidence["input_down_ns"] = downs.Select(entry => entry.time).ToArray();
    evidence["input_up_ns"] = ups.Select(entry => entry.time).ToArray();
    evidence["interaction_begins"] = entries.Count(entry => entry.phase == "visual_tool.interaction_begin");
    Require(downs.Length == 4 && ups.Length == 4,
        $"expected two reference and two rapid input gestures, got {downs.Length} downs/{ups.Length} ups");
    Require(Enumerable.Range(0, 4).All(i => downs[i].time < ups[i].time && (i == 3 || ups[i].time < downs[i + 1].time)),
        "input gestures were not ordered down/up pairs");

    bool WasPresentedBefore(RapidTraceEntry submitted, long before) => entries.Any(entry =>
        entry.stage == "display_present" && entry.time >= submitted.time && entry.time < before
        && entry.interaction == submitted.interaction && entry.delivery == submitted.delivery
        && entry.provider == submitted.provider && entry.content == submitted.content && entry.request == submitted.request);

    var initialLoad = entries.LastOrDefault(entry => entry.stage == "worker_take" && entry.interaction == 0 && entry.time < downs[0].time);
    Require(initialLoad is not null, "initial subtitle load has no trace witness");
    Require(WasPresentedBefore(initialLoad!, downs[0].time), "initial subtitle picture was not presented before the reference press");
    var referenceFinal = entries.SingleOrDefault(entry => entry.stage == "submit_update" && entry.delivery == 2
        && entry.interaction != 0 && entry.time > downs[0].time && entry.time < downs[1].time);
    Require(referenceFinal is not null, "first sequential reference gesture lacks its unique Final submission");
    evidence["reference_final"] = referenceFinal;
    Require(WasPresentedBefore(referenceFinal!, downs[1].time), "first sequential Final was not presented before the second reference press");
    var restoredLoad = entries.LastOrDefault(entry => entry.stage == "submit_load" && entry.time > ups[1].time && entry.time < downs[2].time);
    Require(restoredLoad is not null, "restored original subtitles lack a load witness before rapid input");
    var restoredRender = entries.LastOrDefault(entry => entry.stage == "worker_take" && entry.interaction == 0
        && entry.time >= restoredLoad!.time && entry.time < downs[2].time
        && entry.provider == restoredLoad.provider && entry.content == restoredLoad.content);
    Require(restoredRender is not null && WasPresentedBefore(restoredRender, downs[2].time),
        "restored original picture was not presented before rapid input");
    var rapidFinal = entries.SingleOrDefault(entry => entry.stage == "submit_update" && entry.delivery == 2
        && entry.interaction != 0 && entry.time > downs[2].time && entry.time < downs[3].time);
    Require(rapidFinal is not null, "first rapid gesture lacks its unique Final submission before the fourth input down");
    evidence["rapid_final"] = rapidFinal;
    Require(!WasPresentedBefore(rapidFinal!, downs[3].time), "first rapid Final was already presented before the fourth input down");
    Require(!entries.Any(entry => entry.stage == "display_present" && entry.interaction == rapidFinal!.interaction
        && entry.time > downs[2].time && entry.time < downs[3].time),
        "a first rapid Intermediate already moved the displayed feature before the fourth input down");
    evidence["passed"] = true;
    Publish();
    Console.WriteLine($"visual_tool_input_contract.rapid_witness=prior_interaction_{rapidFinal!.interaction}_not_presented_at_fourth_input_down");
}
static void PrepareArtifactsDirectory(string artifacts)
{
    var allowedRoot = Path.GetFullPath(
        Path.Combine(Environment.CurrentDirectory, "build-dir", "artifacts"));
    var relative = Path.GetRelativePath(allowedRoot, artifacts);
    if (relative == "."
        || Path.IsPathRooted(relative)
        || relative == ".."
        || relative.StartsWith($"..{Path.DirectorySeparatorChar}", StringComparison.Ordinal)
        || relative.StartsWith($"..{Path.AltDirectorySeparatorChar}", StringComparison.Ordinal))
        throw new InvalidOperationException("--artifacts must be a subdirectory of build-dir/artifacts");
    if (Directory.Exists(artifacts) && Directory.EnumerateFileSystemEntries(artifacts).Any())
        throw new InvalidOperationException("--artifacts must not contain files from an earlier run");
    Directory.CreateDirectory(artifacts);
}

static void AddArgument(ProcessStartInfo startInfo, string value) =>
    startInfo.ArgumentList.Add(value);

static void WriteProfileConfig(string profilePath, string subtitleProvider)
{
    var userPath = Path.Combine(profilePath, "user");
    Directory.CreateDirectory(userPath);
    var config = new Dictionary<string, object>
    {
        ["App"] = new Dictionary<string, object>
        {
            ["Auto"] = new Dictionary<string, object>
            {
                ["Check For Updates"] = false,
                ["Load Linked Files"] = 0,
            },
        },
        ["Subtitle"] = new Dictionary<string, object>
        {
            ["Provider"] = subtitleProvider,
            ["Use STC"] = true,
        },
        ["Tool"] = new Dictionary<string, object>
        {
            ["Visual"] = new Dictionary<string, object> { ["Autohide"] = false },
        },
        ["Video"] = new Dictionary<string, object>
        {
            ["Open Audio"] = false,
            ["Scale with DPI"] = false,
            ["Renderer"] = new Dictionary<string, object> { ["Backend"] = "opengl" },
            ["Skia Tools"] = new Dictionary<string, object> { ["Enabled"] = false },
        },
    };
    var json = JsonSerializer.Serialize(config, new JsonSerializerOptions { WriteIndented = true });
    File.WriteAllText(Path.Combine(userPath, "config.json"), json, new UTF8Encoding(false));
}

static string ReadSelectedSubtitleProvider(string profilePath)
{
    var logRoot = Path.Combine(profilePath, "user", "log");
    if (!Directory.Exists(logRoot))
        return "unknown";

    const string prefix = "Selected subtitles provider: ";
    var providers = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    foreach (var path in Directory.EnumerateFiles(logRoot, "*.ndjson"))
    {
        foreach (var line in File.ReadLines(path))
        {
            using var entry = JsonDocument.Parse(line);
            if (entry.RootElement.TryGetProperty("section", out var section)
                && section.GetString() == "subtitle/provider/select"
                && entry.RootElement.TryGetProperty("message", out var message)
                && message.GetString() is { } text
                && text.StartsWith(prefix, StringComparison.Ordinal))
                providers.Add(text[prefix.Length..]);
        }
    }
    return providers.Count == 1 ? providers.Single() : "unknown";
}

static nint FindQueueMarkerButton(nint mainWindow, nint videoCanvas)
{
    var videoThread = GetWindowThreadProcessId(videoCanvas, out _);
    foreach (var hwnd in EnumerateChildWindows(mainWindow).Where(IsWindowVisible))
    {
        if (!NativeClassName(hwnd).Equals("Button", StringComparison.OrdinalIgnoreCase))
            continue;
        var text = new char[256];
        if (SendMessageTextTimeout(
                hwnd, 0x000D, text.Length, text,
                SendMessageTimeoutFlags.AbortIfHung | SendMessageTimeoutFlags.Block,
                1000, out var length) == 0)
            throw new TimeoutException("Could not read a queue-marker button label");
        if (new string(text, 0, length.ToInt32()).Replace("&", "", StringComparison.Ordinal) != "Show Original")
            continue;
        if (videoThread == 0 || GetWindowThreadProcessId(hwnd, out _) != videoThread)
            throw new InvalidOperationException("Queue-marker button is not on the video GUI thread");
        return hwnd;
    }
    throw new InvalidOperationException("Could not locate the Show Original queue-marker button");
}

static void WaitForPostedMotions(nint marker, Process process, int timeoutSeconds)
{
    var initialCheck = SendWindowMessage(marker, WindowMessage.ButtonGetCheck, 0, 0);
    if ((SendWindowMessage(marker, WindowMessage.ButtonGetState, 0, 0) & 4) != 0)
        throw new InvalidOperationException("Queue-marker button was already visually pressed");
    var deadline = Deadline(timeoutSeconds);
    var reached = false;
    try
    {
        // Unlike a sent WM_NULL, this marker follows the posted mouse moves on
        // the same GUI thread. BM_SETSTATE changes only the button's visual
        // pressed state: it neither checks the checkbox nor sends a click.
        if (!PostMessage(marker, (uint)WindowMessage.ButtonSetState, 1, 0))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not post the motion queue marker");
        WaitForButtonPushState(marker, true, process, deadline);
        reached = true;
    }
    finally
    {
        // Keep reset behind the marker even if a timeout leaves it queued.
        if (!PostMessage(marker, (uint)WindowMessage.ButtonSetState, 0, 0))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Could not post the queue-marker reset");
        if (reached)
        {
            WaitForButtonPushState(marker, false, process, deadline);
            if (SendWindowMessage(marker, WindowMessage.ButtonGetCheck, 0, 0) != initialCheck)
                throw new InvalidOperationException("Queue marker changed the checkbox value");
        }
    }
}

static void WaitForButtonPushState(nint marker, bool pushed, Process process, long deadline)
{
    while (Stopwatch.GetTimestamp() < deadline)
    {
        ThrowIfProcessFailed(process);
        var remainingMs = (deadline - Stopwatch.GetTimestamp()) * 1000.0 / Stopwatch.Frequency;
        if (remainingMs <= 0)
            break;
        var state = SendWindowMessage(
            marker, WindowMessage.ButtonGetState, 0, 0,
            (uint)Math.Clamp(Math.Ceiling(remainingMs), 1, 2000));
        if (Stopwatch.GetTimestamp() < deadline && ((state & 4) != 0) == pushed)
            return;
        Thread.Sleep(10);
    }
    throw new TimeoutException($"Posted queue marker did not reach pushed={pushed}");
}

static nint WaitForVideoCanvas(Process process, int timeoutSeconds)
{
    var deadline = Deadline(timeoutSeconds);
    while (Stopwatch.GetTimestamp() < deadline)
    {
        ThrowIfProcessFailed(process);
        UiaDriver.ThrowIfFatalDialog(process);
        process.Refresh();
        var candidate = EnumerateChildWindows(process.MainWindowHandle)
            .Where(IsWindowVisible)
            .Select(hwnd => (Hwnd: hwnd, Class: NativeClassName(hwnd), Rect: NativeWindowRect(hwnd)))
            .Where(item => item.Class.Equals("wxGLCanvas", StringComparison.OrdinalIgnoreCase)
                && item.Rect.Width >= 160
                && item.Rect.Height >= 90)
            .OrderByDescending(item => item.Rect.Area)
            .FirstOrDefault();
        if (candidate.Hwnd != 0 && IsWindowEnabled(process.MainWindowHandle))
            return candidate.Hwnd;
        Thread.Sleep(100);
    }
    throw new TimeoutException("Could not locate a ready Video Display wxGLCanvas");
}

static void SelectVisibleDialogueLines(AutomationElement mainWindow)
{
    var button = UiaDriver.FindEnabledInvokableButton(
        mainWindow,
        "Select all dialogue lines that are visible",
        "Select all dialogue lines visible",
        "visible on the current video frame")
        ?? throw new InvalidOperationException("Select-visible-lines toolbar command was not found");
    UiaDriver.Invoke(button);
    Thread.Sleep(200);
}

static int ConfirmMultiSelection(AutomationElement mainWindow, Process process, int timeoutSeconds, string stage, Action? checkAlive = null)
{
    var deadline = Deadline(timeoutSeconds);
    var status = "<StatusBar unavailable>";
    do
    {
        if (checkAlive is null) ThrowIfProcessFailed(process); else checkAlive();
        var names = new List<string>();
        var descriptions = new List<string>();
        var statusBars = mainWindow.FindAll(
            TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.StatusBar));
        foreach (AutomationElement statusBar in statusBars)
        {
            var fields = statusBar.FindAll(TreeScope.Descendants, Condition.TrueCondition);
            foreach (var field in new[] { statusBar }.Concat(fields.Cast<AutomationElement>()))
            {
                names.Add(field.Current.Name ?? string.Empty);
                descriptions.Add($"{field.Current.ControlType.ProgrammaticName}: {JsonSerializer.Serialize(field.Current.Name)}");
            }
        }
        if (names.Any(name => Regex.IsMatch(name, @"(?<!\d)2 lines selected(?!\w)", RegexOptions.CultureInvariant)))
        {
            Console.WriteLine($"visual_tool_drag.selection_confirmed={stage}:2");
            return 2;
        }
        status = statusBars.Count == 0 ? "<StatusBar unavailable>" : string.Join("; ", descriptions);
        Thread.Sleep(50);
    } while (Stopwatch.GetTimestamp() < deadline);
    throw new InvalidOperationException(
        $"Multi-selection was not confirmed at {stage}: expected '2 lines selected'; status={status}");
}

static void SelectDragTool(AutomationElement mainWindow) =>
    SelectVisualTool(mainWindow, "Drag subtitles");

static void SelectVisualTool(AutomationElement mainWindow, string tooltip)
{
    var descendants = mainWindow.FindAll(TreeScope.Descendants, Condition.TrueCondition);
    var candidates = new List<string>();
    foreach (AutomationElement element in descendants)
    {
        try
        {
            var name = element.Current.Name ?? string.Empty;
            if (!name.Contains(tooltip, StringComparison.OrdinalIgnoreCase))
                continue;
            var patterns = element.GetSupportedPatterns()
                .Select(pattern => Automation.PatternName(pattern) ?? pattern.Id.ToString(CultureInfo.InvariantCulture))
                .ToArray();
            candidates.Add(
                $"{element.Current.ControlType.ProgrammaticName}:{name} [{string.Join(",", patterns)}]");
            if (!element.Current.IsEnabled)
                continue;
            if (element.TryGetCurrentPattern(SelectionItemPattern.Pattern, out var selectionPattern))
            {
                var selection = (SelectionItemPattern)selectionPattern;
                selection.Select();
                if (!selection.Current.IsSelected)
                    throw new InvalidOperationException($"'{tooltip}' toolbar item did not become selected");
                return;
            }
            if (element.TryGetCurrentPattern(TogglePattern.Pattern, out var togglePattern))
            {
                var toggle = (TogglePattern)togglePattern;
                var state = toggle.Current.ToggleState;
                if (state != ToggleState.On)
                {
                    UiaDriver.Toggle(element);
                    state = UiaDriver.WaitForToggleStateChange(
                        element,
                        state,
                        TimeSpan.FromSeconds(5));
                }
                if (state != ToggleState.On)
                    throw new InvalidOperationException($"'{tooltip}' toolbar item did not toggle on");
                return;
            }
            if (element.TryGetCurrentPattern(InvokePattern.Pattern, out var invokePattern))
            {
                ((InvokePattern)invokePattern).Invoke();
                return;
            }
        }
        catch (ElementNotAvailableException)
        {
            // The video toolbar can be rebuilt while the video settles.
        }
    }
    throw new InvalidOperationException(
        candidates.Count == 0
            ? $"'{tooltip}' toolbar item was not found"
            : $"'{tooltip}' toolbar item exposes no usable UIA pattern: {string.Join("; ", candidates)}");
}

static IReadOnlyList<PointI> BuildMotionPath(
    PointI start,
    PointI final,
    int count,
    int repeat,
    int clientWidth,
    int clientHeight)
{
    var result = new List<PointI>(count);
    for (var index = 0; index < count - 1; ++index)
    {
        var logicalIndex = index / repeat;
        var phase = logicalIndex % 160;
        var xOffset = phase <= 80 ? phase - 40 : 120 - phase;
        var yOffset = ((logicalIndex / 20) % 2 == 0 ? -12 : 12)
            + logicalIndex % 3
            + index % repeat;
        result.Add(new PointI(
            Math.Clamp(start.X + xOffset, 16, clientWidth - 16),
            Math.Clamp(start.Y + yOffset, 16, clientHeight - 16)));
    }
    result.Add(final);
    return result;
}

static void ValidateAfterDrag(
    AssSnapshot initial,
    AssSnapshot actual,
    string selection,
    Position expectedDelta)
{
    if (actual.Dialogues.Count != initial.Dialogues.Count)
        throw new InvalidOperationException("Dialogue count changed during drag");

    var changedCount = selection == "multi" ? 2 : 1;
    for (var index = 0; index < actual.Dialogues.Count; ++index)
    {
        var before = initial.Dialogues[index];
        var after = actual.Dialogues[index];
        if (!string.Equals(before.PlainText, after.PlainText, StringComparison.Ordinal))
            throw new InvalidOperationException($"Dialogue text changed at row {index}");
        if (!string.Equals(
            before.PositionNeutralDialogue,
            after.PositionNeutralDialogue,
            StringComparison.Ordinal))
            throw new InvalidOperationException($"Dialogue overrides other than \\pos changed at row {index}");
        if (index < changedCount)
        {
            AssertNear(
                before.Position.X + expectedDelta.X,
                after.Position.X,
                $"row {index} final X");
            AssertNear(
                before.Position.Y + expectedDelta.Y,
                after.Position.Y,
                $"row {index} final Y");
        }
        else
        {
            if (!string.Equals(before.SerializedDialogue, after.SerializedDialogue, StringComparison.Ordinal))
                throw new InvalidOperationException($"Unselected dialogue changed at row {index}");
            AssertNear(before.Position.X, after.Position.X, $"row {index} unchanged X");
            AssertNear(before.Position.Y, after.Position.Y, $"row {index} unchanged Y");
        }
    }

    if (selection == "multi")
    {
        var firstDelta = actual.Dialogues[0].Position - initial.Dialogues[0].Position;
        var secondDelta = actual.Dialogues[1].Position - initial.Dialogues[1].Position;
        AssertNear(firstDelta.X, secondDelta.X, "multi-selection X delta");
        AssertNear(firstDelta.Y, secondDelta.Y, "multi-selection Y delta");
    }
}

static void ValidateUndo(AssSnapshot initial, AssSnapshot actual)
{
    if (actual.Dialogues.Count != initial.Dialogues.Count)
        throw new InvalidOperationException("Dialogue count changed after Undo");
    for (var index = 0; index < actual.Dialogues.Count; ++index)
    {
        var before = initial.Dialogues[index];
        var after = actual.Dialogues[index];
        if (!string.Equals(before.SerializedDialogue, after.SerializedDialogue, StringComparison.Ordinal))
            throw new InvalidOperationException($"Undo did not restore dialogue row {index}");
    }
}

static void AssertNear(double expected, double actual, string label)
{
    if (Math.Abs(expected - actual) > 0.02)
        throw new InvalidOperationException(
            $"{label} expected {expected.ToString("F4", CultureInfo.InvariantCulture)}, " +
            $"got {actual.ToString("F4", CultureInfo.InvariantCulture)}");
}

static void EnsureSaved(
    AutomationElement mainWindow,
    Process process,
    string projectPath,
    int timeoutSeconds)
{
    process.Refresh();
    if (!process.MainWindowTitle.TrimStart().StartsWith("* ", StringComparison.Ordinal))
        return;

    var save = UiaDriver.FindEnabledInvokableButton(
        mainWindow,
        "Save the current subtitles",
        "Save Subtitles",
        "Save")
        ?? throw new InvalidOperationException("Save Subtitles toolbar command was not found");
    var previousWrite = File.GetLastWriteTimeUtc(projectPath);
    UiaDriver.Invoke(save);
    var deadline = Deadline(timeoutSeconds);
    while (Stopwatch.GetTimestamp() < deadline)
    {
        ThrowIfProcessFailed(process);
        process.Refresh();
        if (!process.MainWindowTitle.TrimStart().StartsWith("* ", StringComparison.Ordinal)
            && File.GetLastWriteTimeUtc(projectPath) >= previousWrite)
            return;
        Thread.Sleep(50);
    }
    throw new TimeoutException("Save Subtitles did not reach a clean state");
}

static void InvokeUndo(AutomationElement mainWindow, Process process, int timeoutSeconds)
{
    UiaDriver.FocusAndVerify(mainWindow, process, TimeSpan.FromSeconds(5));
    SendChord(NativeConstants.VirtualKeyControl, (ushort)'Z');
    var deadline = Deadline(timeoutSeconds);
    while (Stopwatch.GetTimestamp() < deadline)
    {
        ThrowIfProcessFailed(process);
        process.Refresh();
        if (process.MainWindowTitle.TrimStart().StartsWith("* ", StringComparison.Ordinal))
            return;
        Thread.Sleep(50);
    }
    throw new TimeoutException("Undo did not mark the subtitle file as modified");
}

static void RequestCleanClose(Process process, nint mainWindow, int timeoutSeconds)
{
    if (!PostMessage(mainWindow, (uint)WindowMessage.Close, 0, 0))
        throw new Win32Exception(Marshal.GetLastWin32Error(), "WM_CLOSE failed");
    if (!process.WaitForExit(timeoutSeconds * 1000))
        throw new TimeoutException("Aegisub did not exit after WM_CLOSE");
    process.Refresh();
    if (process.ExitCode != 0)
        throw new InvalidOperationException($"Aegisub exited with code {process.ExitCode}");
}

static void WaitForCapture(
    nint videoCanvas,
    bool captured,
    Process process,
    int timeoutSeconds,
    Action? checkAlive = null)
{
    var threadId = GetWindowThreadProcessId(videoCanvas, out _);
    if (threadId == 0)
        throw new Win32Exception(Marshal.GetLastWin32Error(), "GetWindowThreadProcessId failed");
    var deadline = Deadline(timeoutSeconds);
    while (Stopwatch.GetTimestamp() < deadline)
    {
        var info = GuiThreadInfo.Create();
        if (!GetGUIThreadInfo(threadId, ref info))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "GetGUIThreadInfo failed");
        if (captured ? info.Capture == videoCanvas : info.Capture == 0)
            return;
        if (checkAlive is null) ThrowIfProcessFailed(process); else checkAlive();
        Thread.Sleep(5);
    }
    throw new TimeoutException(captured
        ? "Visual tool did not capture the mouse; the expected feature was not hit"
        : "Visual tool did not release mouse capture after WM_LBUTTONUP");
}

static void ThrowIfProcessFailed(Process process)
{
    process.Refresh();
    if (process.HasExited)
        throw new InvalidOperationException($"Aegisub exited early with code {process.ExitCode}");
    UiaDriver.ThrowIfFatalDialog(process);
}

static void SendMouseToWindow(nint hwnd, WindowMessage message, PointI point, int keyState) =>
    SendWindowMessage(hwnd, message, keyState, PackPoint(point));

static void PostMouseToWindow(nint hwnd, WindowMessage message, PointI point, int keyState)
{
    if (!PostMessage(hwnd, (uint)message, keyState, PackPoint(point)))
        throw new Win32Exception(Marshal.GetLastWin32Error(), $"PostMessage failed for {message}");
}

static nint SendWindowMessage(nint hwnd, WindowMessage message, nint wParam, nint lParam, uint timeoutMilliseconds = 3000)
{
    var sent = SendMessageTimeout(
        hwnd,
        (uint)message,
        wParam,
        lParam,
        SendMessageTimeoutFlags.AbortIfHung | SendMessageTimeoutFlags.Block,
        timeoutMilliseconds,
        out var result);
    if (sent == 0)
        throw new Win32Exception(Marshal.GetLastWin32Error(), $"SendMessageTimeout failed for {message}");
    return result;
}

static nint PackPoint(PointI point) =>
    new(((long)(ushort)(short)point.Y << 16) | (ushort)(short)point.X);

static void SendChord(ushort modifier, ushort key)
{
    var inputs = new[]
    {
        Input.KeyboardInput(modifier, keyUp: false),
        Input.KeyboardInput(key, keyUp: false),
        Input.KeyboardInput(key, keyUp: true),
        Input.KeyboardInput(modifier, keyUp: true),
    };
    var sent = SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<Input>());
    if (sent != inputs.Length)
        throw new Win32Exception(Marshal.GetLastWin32Error(), "SendInput failed for keyboard chord");
}

static long Deadline(int seconds) =>
    Stopwatch.GetTimestamp() + (long)seconds * Stopwatch.Frequency;

static double ElapsedMilliseconds(long start, long end) =>
    (end - start) * 1000.0 / Stopwatch.Frequency;

static IEnumerable<nint> EnumerateChildWindows(nint parent)
{
    var result = new List<nint>();
    EnumChildWindows(parent, (hwnd, _) =>
    {
        result.Add(hwnd);
        return true;
    }, 0);
    return result;
}

static string NativeClassName(nint hwnd)
{
    var buffer = new StringBuilder(256);
    return GetClassName(hwnd, buffer, buffer.Capacity) == 0
        ? string.Empty
        : buffer.ToString();
}

static RectI NativeWindowRect(nint hwnd)
{
    if (!GetWindowRect(hwnd, out var rect))
        return default;
    return new RectI(rect.Left, rect.Top, rect.Right - rect.Left, rect.Bottom - rect.Top);
}

[DllImport("user32.dll", SetLastError = true)]
static extern bool EnumChildWindows(nint parent, EnumWindowsProc callback, nint lParam);

[DllImport("user32.dll")]
static extern bool IsWindowVisible(nint hwnd);

[DllImport("user32.dll")]
static extern bool IsWindowEnabled(nint hwnd);

[DllImport("user32.dll", CharSet = CharSet.Unicode)]
static extern int GetClassName(nint hwnd, StringBuilder className, int maxCount);

[DllImport("user32.dll")]
static extern bool GetWindowRect(nint hwnd, out NativeRect rect);

[DllImport("user32.dll")]
static extern bool GetClientRect(nint hwnd, out NativeRect rect);

[DllImport("user32.dll",SetLastError=true)]
static extern bool ScreenToClient(nint window,ref NativePoint point);

[DllImport("user32.dll", EntryPoint = "GetParent")]
static extern nint NativeGetParent(nint window);

[DllImport("user32.dll", EntryPoint = "GetDlgCtrlID")]
static extern int NativeGetDlgCtrlId(nint window);

[DllImport("user32.dll")]
static extern bool ShowWindow(nint hwnd, int command);

[DllImport("user32.dll", SetLastError = true)]
static extern bool SetWindowPos(
    nint hwnd,
    nint insertAfter,
    int x,
    int y,
    int width,
    int height,
    SetWindowPosFlags flags);

[DllImport("user32.dll", SetLastError = true)]
static extern nint SendMessageTimeout(
    nint hwnd,
    uint message,
    nint wParam,
    nint lParam,
    SendMessageTimeoutFlags flags,
    uint timeoutMilliseconds,
    out nint result);

[DllImport("user32.dll", SetLastError = true)]
static extern bool PostMessage(nint hwnd, uint message, nint wParam, nint lParam);

[DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
static extern nint SendMessageStringTimeout(nint hwnd, uint message, nint parameter, StringBuilder text, SendMessageTimeoutFlags flags, uint timeout, out nint result);

[DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
static extern nint SendMessageTextTimeout(
    nint hwnd,
    uint message,
    nint wParam,
    [Out] char[] text,
    SendMessageTimeoutFlags flags,
    uint timeoutMilliseconds,
    out nint result);

[DllImport("user32.dll", SetLastError = true)]
static extern uint GetWindowThreadProcessId(nint hwnd, out uint processId);

[DllImport("user32.dll", SetLastError = true)]
static extern bool GetGUIThreadInfo(uint threadId, ref GuiThreadInfo info);

[DllImport("user32.dll", SetLastError = true)]
static extern uint SendInput(uint inputCount, Input[] inputs, int size);

delegate bool EnumWindowsProc(nint hwnd, nint lParam);

static class NativeConstants
{
    public const int ShowNoActivate = 4;
    public const int MouseKeyLeftButton = 0x0001;
    public const int MouseKeyShift = 0x0004;
    public const ushort VirtualKeyControl = 0x11;
}

sealed record DriverOptions(
    string Executable,
    string Video,
    string Artifacts,
    string InputMode,
    string Size,
    string Selection,
    string Trace,
    string TraceWindow,
    string SubtitleProvider,
    int MotionCount,
    int MotionRepeat,
    int MotionIntervalMilliseconds,
    int HoldMilliseconds,
    int SmallEventCount,
    int LargeEventCount,
    int TimeoutSeconds,
    int SettleMilliseconds,
    int WindowWidth,
    int WindowHeight,
    int DeltaX,
    int DeltaY,
    string InputContract)
{
    public static DriverOptions Parse(string[] args)
    {
        string? executable = null;
        string? video = null;
        string? artifacts = null;
        var inputMode = "throughput";
        var size = "small";
        var selection = "single";
        var trace = "off";
        var traceWindow = "markers";
        var subtitleProvider = "libass";
        var motions = 128;
        var motionRepeat = 1;
        var motionIntervalMilliseconds = 0;
        var holdMilliseconds = 150;
        var smallEvents = 32;
        var largeEvents = 10000;
        var timeout = 45;
        var settle = 1500;
        var width = 1280;
        var height = 900;
        var deltaX = 48;
        var deltaY = -24;
        var inputContract = "none";

        for (var index = 0; index < args.Length; ++index)
        {
            var option = args[index];
            string Value()
            {
                if (++index >= args.Length)
                    throw new ArgumentException($"Missing value for {option}");
                return args[index];
            }

            switch (option)
            {
                case "--help":
                case "-h":
                    PrintHelp();
                    Environment.Exit(0);
                    break;
                case "--exe": executable = Value(); break;
                case "--video": video = Value(); break;
                case "--artifacts": artifacts = Value(); break;
                case "--input-contract": inputContract = Value(); break;
                case "--input-mode": inputMode = Choice(Value(), option, "throughput", "paced-hold", "double-click"); break;
                case "--size": size = Choice(Value(), option, "small", "large"); break;
                case "--selection": selection = Choice(Value(), option, "single", "multi"); break;
                case "--trace": trace = Choice(Value(), option, "off", "on"); break;
                case "--trace-window": traceWindow = Choice(Value(), option, "markers", "legacy"); break;
                case "--subtitle-provider": subtitleProvider = Value(); break;
                case "--motions": motions = Positive(Value(), option); break;
                case "--motion-repeat": motionRepeat = Positive(Value(), option); break;
                case "--motion-interval-ms": motionIntervalMilliseconds = NonNegative(Value(), option); break;
                case "--hold-ms": holdMilliseconds = NonNegative(Value(), option); break;
                case "--small-events": smallEvents = Positive(Value(), option); break;
                case "--large-events": largeEvents = Positive(Value(), option); break;
                case "--timeout-seconds": timeout = Positive(Value(), option); break;
                case "--settle-ms": settle = NonNegative(Value(), option); break;
                case "--width": width = Positive(Value(), option); break;
                case "--height": height = Positive(Value(), option); break;
                case "--delta-x": deltaX = Integer(Value(), option); break;
                case "--delta-y": deltaY = Integer(Value(), option); break;
                default: throw new ArgumentException($"Unknown option: {option}");
            }
        }

        if (string.IsNullOrWhiteSpace(executable))
            throw new ArgumentException("--exe is required");
        if (string.IsNullOrWhiteSpace(video))
            throw new ArgumentException("--video is required");
        if (string.IsNullOrWhiteSpace(artifacts))
            throw new ArgumentException("--artifacts is required");
        if (string.IsNullOrWhiteSpace(subtitleProvider))
            throw new ArgumentException("--subtitle-provider must name a provider");
        if (motions < 100)
            throw new ArgumentException("--motions must be at least 100");
        if (smallEvents < 2 || largeEvents < 2)
            throw new ArgumentException("ASS fixtures must contain at least two events");
        if (inputMode == "paced-hold")
        {
            if (trace != "on" || traceWindow != "markers")
                throw new ArgumentException(
                    "--input-mode paced-hold requires --trace on --trace-window markers");
            if (holdMilliseconds is < 100 or > 250)
                throw new ArgumentException(
                    "--hold-ms must be between 100 and 250 for paced-hold input");
        }
        if (inputMode == "double-click" && trace != "off")
            throw new ArgumentException("--input-mode double-click requires --trace off");

        return new DriverOptions(
            Path.GetFullPath(executable),
            Path.GetFullPath(video),
            Path.GetFullPath(artifacts),
            inputMode,
            size,
            selection,
            trace,
            traceWindow,
            subtitleProvider,
            motions,
            motionRepeat,
            motionIntervalMilliseconds,
            holdMilliseconds,
            smallEvents,
            largeEvents,
            timeout,
            settle,
            width,
            height,
            deltaX,
            deltaY,
            inputContract);
    }

    private static string Choice(string value, string option, params string[] choices) =>
        choices.FirstOrDefault(choice => choice.Equals(value, StringComparison.OrdinalIgnoreCase))
        ?? throw new ArgumentException($"{option} must be one of: {string.Join(", ", choices)}");

    private static int Positive(string value, string option) =>
        int.TryParse(value, out var result) && result > 0
            ? result
            : throw new ArgumentException($"{option} must be a positive integer");

    private static int NonNegative(string value, string option) =>
        int.TryParse(value, out var result) && result >= 0
            ? result
            : throw new ArgumentException($"{option} must be a non-negative integer");

    private static int Integer(string value, string option) =>
        int.TryParse(value, out var result)
            ? result
            : throw new ArgumentException($"{option} must be an integer");

    private static void PrintHelp()
    {
        Console.WriteLine("Legacy visual-tool drag GUI benchmark and native input contracts");
        Console.WriteLine("  [--input-contract " + string.Join("|", InputContractCase.Names) + "]");
        Console.WriteLine("  Prefix paired feature contract names with rapid- to test regrab before presentation.");
        Console.WriteLine("  --exe PATH --video PATH --artifacts PATH");
        Console.WriteLine("  [--input-mode throughput|paced-hold|double-click] [--hold-ms N]");
        Console.WriteLine("  [--size small|large] [--selection single|multi] [--trace off|on]");
        Console.WriteLine("  [--trace-window markers|legacy]");
        Console.WriteLine("  [--subtitle-provider NAME] requires the exact runtime provider (default: libass)");
        Console.WriteLine("  [--motions N>=100] [--small-events N] [--large-events N]");
        Console.WriteLine("  [--motion-repeat N] repeats each effective position with suppressed-axis jitter");
        Console.WriteLine("  [--motion-interval-ms N] requested spacing for synthetic PostMessage motions (default: 0)");
        Console.WriteLine("  [--timeout-seconds N] [--settle-ms N] [--width N --height N]");
        Console.WriteLine("  Synthetic PostMessage input does not hold the OS left mouse button down.");
        Console.WriteLine("  Real/system mouse events can interrupt capture and fail a run.");
        Console.WriteLine("  Early capture loss alone does not establish a product regression.");
    }
}

sealed record RapidTraceEntry
{
    public long time { get; init; }
    public string? phase { get; init; }
    public string? stage { get; init; }
    public ulong interaction { get; init; }
    public int delivery { get; init; }
    public ulong provider { get; init; }
    public ulong content { get; init; }
    public ulong request { get; init; }
}
sealed record InputContractCase(
    string Name, string Tooltip, string Tags, PointI Start, string TagPattern,
    bool Hold = false, bool Freehand = false, string? Submode = null)
{
    private const string Drag = "Drag subtitles";
    private const string RotateZ = "Rotate subtitles on their Z axis";
    private const string RotateXY = "Rotate subtitles on their X and Y axes";
    private const string Clip = "Clip subtitles to a rectangle";
    private const string Vector = "Clip subtitles to a vectorial area";
    private const string Position = @"\\pos\([^)]*\)";
    private const string Move = @"\\move\([^)]*\)";
    private const string Origin = @"\\org\([^)]*\)";
    private const string ClipTag = @"\\i?clip\([^)]*\)";
    private static readonly InputContractCase[] Cases =
    [
        new("drag-pos", Drag, @"\pos(160,120)", new(160, 120), Position),
        new("drag-move-start", Drag, @"\move(160,120,420,320,0,8000)", new(160, 120), Move),
        new("drag-move-end", Drag, @"\move(160,120,420,320,0,8000)", new(420, 320), Move),
        new("drag-org", Drag, @"\pos(280,230)\org(160,120)", new(160, 120), Origin),
        new("rotate-z-origin", RotateZ, @"\pos(280,230)\org(160,120)", new(160, 120), Origin),
        new("rotate-xy-origin", RotateXY, @"\pos(280,230)\org(160,120)", new(160, 120), Origin),
        new("rect-corner", Clip, @"\pos(280,230)\iclip(160,120,460,350)", new(160, 120), ClipTag),
        new("vector-node", Vector, @"\pos(280,230)\iclip(m 160 120 l 430 110 b 470 150 470 300 410 340 l 120 310)", new(160, 120), ClipTag),
        new("hold-rotate-z", RotateZ, @"\pos(280,230)\org(280,230)\frz17", new(390, 310), @"\\frz-?[\d.]+", Hold: true),
        new("hold-rotate-xy", RotateXY, @"\pos(280,230)\org(280,230)\frx10\fry-8", new(390, 310), @"\\fr[xy]-?[\d.]+", Hold: true),
        new("hold-scale", "Scale subtitles on X and Y axes", @"\pos(280,230)\fscx110\fscy95", new(390, 310), @"\\fsc[xy]-?[\d.]+", Hold: true),
        new("hold-rect", Clip, @"\pos(280,230)\clip(280,200,500,380)", new(100, 80), ClipTag, Hold: true),
        new("freehand", Vector, @"\pos(280,230)\iclip(m 160 120 l 430 110 410 340 120 310)", new(80, 90), ClipTag,
            Freehand: true, Submode: "Draws a freehand shape"),
        new("freehand-smooth", Vector, @"\pos(280,230)\iclip(m 160 120 l 430 110 410 340 120 310)", new(80, 90), ClipTag,
            Freehand: true, Submode: "Draws a smoothed freehand shape"),
    ];
    public static IEnumerable<string> Names => Cases.Select(value => value.Name);
    public static InputContractCase Get(string name) => Cases.SingleOrDefault(value => value.Name == name)
        ?? throw new ArgumentException("--input-contract must be one of: " + string.Join(", ", Names));

    public string BuildFixture()
    {
        var lines = Fixture.Build(48, false).Split('\n');
        var first = Array.FindIndex(lines, line => line.StartsWith("Dialogue:", StringComparison.Ordinal));
        lines[first] = @"Dialogue: 3,0:00:00.00,0:00:10.00,Default,input-contract,12,23,34,,{" + Tags
            + @"\bord2.5\shad1.25\1c&H74B2E8&\t(200,1800,\blur1.2)}Primary {\i1}曲線\Ncontrol {\i0}line";
        lines[first + 1] = @"Dialogue: 1,1:00:00.00,1:00:10.00,Default,unrelated,31,42,53,retained,{\pos(410,330)\frz23\clip(20,30,590,440)\1c&HABCDEF&}Unrelated {\b1}preserved{\b0}";
        return string.Join('\n', lines);
    }
}

static class Fixture
{
    public static string Build(int eventCount, bool multi)
    {
        var output = new StringBuilder();
        output.AppendLine("[Script Info]");
        output.AppendLine("Title: Visual tool drag benchmark");
        output.AppendLine("ScriptType: v4.00+");
        output.AppendLine("WrapStyle: 0");
        output.AppendLine("PlayResX: 640");
        output.AppendLine("PlayResY: 480");
        output.AppendLine("LayoutResX: 640");
        output.AppendLine("LayoutResY: 480");
        output.AppendLine("ScaledBorderAndShadow: yes");
        output.AppendLine("YCbCr Matrix: TV.601");
        output.AppendLine();
        output.AppendLine("[V4+ Styles]");
        output.AppendLine("Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding");
        output.AppendLine("Style: Default,Arial,32,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1");
        output.AppendLine();
        output.AppendLine("[Events]");
        output.AppendLine("Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text");
        for (var index = 0; index < eventCount; ++index)
        {
            var visible = index == 0 || (multi && index == 1);
            var start = visible ? "0:00:00.00" : "1:00:00.00";
            var end = visible ? "0:00:10.00" : "1:00:05.00";
            var x = index == 0 ? 320 : index == 1 ? 400 : 20 + index % 600;
            var y = index < 2 ? 240 : 20 + index % 440;
            var text = index == 0
                ? "drag-primary"
                : index == 1
                    ? "drag-secondary"
                    : $"bulk-{index:D5}";
            output.Append("Dialogue: 0,")
                .Append(start).Append(',')
                .Append(end)
                .Append(",Default,,0,0,0,,{\\pos(")
                .Append(x).Append(',').Append(y).Append(" )}")
                .AppendLine(text);
        }
        return output.ToString().Replace(" )}", ")}", StringComparison.Ordinal);
    }
}

sealed record GeometryProviderEvidence(string LogFile,int LineNumber,string Provider);

sealed record AssDialogueSnapshot(
    string SerializedDialogue,
    string PlainText,
    string PositionNeutralDialogue,
    Position Position);

sealed record AssSnapshot(IReadOnlyList<AssDialogueSnapshot> Dialogues)
{
    private static readonly Regex PositionPattern = new(
        @"\\pos\(\s*(?<x>-?\d+(?:\.\d+)?)\s*,\s*(?<y>-?\d+(?:\.\d+)?)\s*\)",
        RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);
    private static readonly Regex OverridePattern = new(
        @"\{[^}]*\}",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);

    public static AssSnapshot Read(string path)
    {
        var dialogues = new List<AssDialogueSnapshot>();
        foreach (var line in File.ReadLines(path))
        {
            if (!line.StartsWith("Dialogue:", StringComparison.OrdinalIgnoreCase))
                continue;
            var fields = line[(line.IndexOf(':') + 1)..].Split(',', 10);
            if (fields.Length != 10)
                throw new InvalidOperationException("Malformed Dialogue line in saved ASS");
            var text = fields[9];
            var match = PositionPattern.Match(text);
            if (!match.Success)
                throw new InvalidOperationException($"Dialogue has no \\pos tag: {text}");
            var position = new Position(
                double.Parse(match.Groups["x"].Value, CultureInfo.InvariantCulture),
                double.Parse(match.Groups["y"].Value, CultureInfo.InvariantCulture));
            dialogues.Add(new AssDialogueSnapshot(
                line,
                OverridePattern.Replace(text, string.Empty),
                PositionPattern.Replace(line, @"\pos(<position>)"),
                position));
        }
        return new AssSnapshot(dialogues);
    }
}

sealed record TraceScope(
    int Count,
    double TotalMilliseconds,
    double P50Milliseconds,
    double P95Milliseconds,
    double MaxMilliseconds);

sealed record TraceEnvironment(
    string BuildLabel,
    string Renderer,
    string SubtitleProvider,
    int VideoWidth,
    int VideoHeight,
    bool SubtitleUseStc);

sealed record TraceMetrics(
    string WindowSource,
    double WindowMilliseconds,
    TraceScope Commit,
    int CoalescedCommitCount,
    int SkippedNoChangeCommitCount,
    double CommitRatio,
    int MarkedLineCount,
    int ActualTextChangeCount,
    int UnchangedTextCount,
    TraceScope SubtitleUpdate,
    TraceScope DisplayRender,
    int PresentedRenderCount,
    int PresentedRendersAfterFinalCommitBeforeInteractionEnd,
    int RepaintRenderCount,
    int DeliveredPackets,
    int DroppedPackets,
    int ProcessImmediateFlushes,
    int ProcessBufferedFlushes,
    int WindowSlowScopeCount,
    TraceScope SceneCacheFill,
    TraceScope SceneCacheReuse,
    TraceScope SceneCacheDirect,
    TraceScope SceneCacheDirectWaiting,
    TraceScope SceneCacheDirectWarmup,
    TraceScope SceneCacheDirectPlayback,
    TraceScope SceneCacheDirectPolicy,
    TraceScope SceneCacheDirectFallback,
    TraceScope OverlayFullUpload,
    TraceScope OverlayDirtyUpload,
    TraceScope OverlayReuse,
    TraceScope OverlayHide,
    long OverlayEstimatedUploadBytes,
    TraceEnvironment Environment)
{
    public static TraceMetrics Read(
        string profilePath,
        int expectedMotionCount,
        string windowMode)
    {
        var sessionRoot = Path.Combine(profilePath, "user", "perf-sessions");
        var traces = Directory.Exists(sessionRoot)
            ? Directory.GetFiles(sessionRoot, "trace.ndjson", SearchOption.AllDirectories)
            : Array.Empty<string>();
        if (traces.Length != 1)
            throw new InvalidOperationException($"Expected one perf trace, found {traces.Length}");

        var entries = File.ReadLines(traces[0])
            .Select(TraceEntry.Parse)
            .ToArray();
        var commits = entries
            .Where(entry => entry.Name == "video_ui_duration"
                && entry.Phase == "visual_tool.commit")
            .OrderBy(entry => entry.TimestampNanoseconds)
            .ToArray();
        if (commits.Length < 1 || commits.Length > expectedMotionCount)
            throw new InvalidOperationException(
                $"Expected between 1 and {expectedMotionCount} visual_tool.commit samples, " +
                $"found {commits.Length}");

        var firstCommitStart = commits.Min(entry =>
            entry.TimestampNanoseconds
                - (long)Math.Round(entry.DurationMilliseconds * 1_000_000.0));
        var finalCommitEnd = commits.Max(entry => entry.TimestampNanoseconds);

        long start;
        long end;
        string windowSource;
        if (windowMode == "markers")
        {
            var beginMarkers = entries
                .Where(entry => entry.Name == "video_ui_duration"
                    && entry.Phase == "visual_tool.interaction_begin")
                .ToArray();
            var endMarkers = entries
                .Where(entry => entry.Name == "video_ui_duration"
                    && entry.Phase == "visual_tool.interaction_end")
                .ToArray();
            if (beginMarkers.Length != 1 || endMarkers.Length != 1)
                throw new InvalidOperationException(
                    $"Expected exactly one visual-tool interaction marker pair, found " +
                    $"begin={beginMarkers.Length}, end={endMarkers.Length}");
            if (beginMarkers[0].DurationMilliseconds != 0.0
                || endMarkers[0].DurationMilliseconds != 0.0)
                throw new InvalidOperationException("Visual-tool interaction markers must have zero duration");
            start = beginMarkers[0].TimestampNanoseconds;
            end = endMarkers[0].TimestampNanoseconds;
            windowSource = "interaction-markers";
        }
        else
        {
            start = firstCommitStart;
            var finalRender = entries.FirstOrDefault(entry =>
                entry.TimestampNanoseconds >= finalCommitEnd
                && entry.Name == "video_ui_duration"
                && entry.Phase == "video_display.render");
            if (finalRender is null)
                throw new InvalidOperationException(
                    "Legacy trace window has no synchronous render after the final commit");
            end = finalRender.TimestampNanoseconds;
            windowSource = "legacy-first-render-after-final-commit";
        }
        if (start >= end
            || firstCommitStart < start
            || finalCommitEnd > end)
            throw new InvalidOperationException("Visual-tool trace window does not contain all commits");

        var window = entries
            .Where(entry => entry.TimestampNanoseconds >= start
                && entry.TimestampNanoseconds <= end)
            .ToArray();
        var summaryPath = Path.Combine(Path.GetDirectoryName(traces[0])!, "summary.txt");
        var summary = File.ReadLines(summaryPath)
            .Select(line => line.Split('=', 2))
            .Where(parts => parts.Length == 2)
            .ToDictionary(parts => parts[0], parts => parts[1], StringComparer.Ordinal);
        var slowScopes = window.Count(entry =>
            entry.Name == "video_ui_duration" && entry.DurationMilliseconds >= 8.0);

        var commitScope = Scope(window, "visual_tool.commit");
        var displayRenderScope = Scope(window, "video_display.render");
        var memorySnapshot = entries.LastOrDefault(entry =>
            entry.Name == "video_memory_snapshot"
                && entry.Reason == "frame_presented");
        var videoOpen = entries.LastOrDefault(entry => entry.Name == "video_open");
        var environment = new TraceEnvironment(
            summary.TryGetValue("build", out var buildLabel) ? buildLabel : "unknown",
            memorySnapshot?.RendererPrimary ?? "unknown",
            memorySnapshot?.SubtitlesProvider ?? "unknown",
            videoOpen?.Width ?? 0,
            videoOpen?.Height ?? 0,
            true);
        var presentedRenderCount = window.Count(entry =>
            entry.Name == "video_ui_duration"
                && entry.Phase == "video_display.render"
                && entry.DetailA == 1);
        var presentedRendersAfterFinalCommitBeforeInteractionEnd = window.Count(entry =>
            entry.TimestampNanoseconds > finalCommitEnd
                && entry.TimestampNanoseconds < end
                && entry.Name == "video_ui_duration"
                && entry.Phase == "video_display.render"
                && entry.DetailA == 1);
        return new TraceMetrics(
            windowSource,
            (end - start) / 1_000_000.0,
            commitScope,
            expectedMotionCount - commitScope.Count,
            window.Count(entry => entry.Name == "video_ui_duration"
                && entry.Phase == "visual_tool.commit.skipped_no_change"),
            commitScope.Count / (double)expectedMotionCount,
            commits.Sum(entry => entry.DetailB ?? 0),
            window.Count(entry => entry.Name == "video_ui_duration"
                && entry.Phase == "visual_tool.override"
                && entry.DetailA == 1),
            window.Count(entry => entry.Name == "video_ui_duration"
                && entry.Phase == "visual_tool.override"
                && entry.DetailA == 0),
            Scope(window, "video_controller.subtitle_update"),
            displayRenderScope,
            presentedRenderCount,
            presentedRendersAfterFinalCommitBeforeInteractionEnd,
            displayRenderScope.Count - presentedRenderCount,
            window.Count(entry => entry.Name == "video_frame_delivered"),
            window.Count(entry => entry.Name == "video_frame_dropped"),
            SummaryInt(summary, "trace.flushes.immediate"),
            SummaryInt(summary, "trace.flushes.buffered"),
            slowScopes,
            Scope(window, "video_display.scene_cache.fill"),
            Scope(window, "video_display.scene_cache.reuse"),
            ScopePrefix(window, "video_display.scene_cache.direct."),
            Scope(window, "video_display.scene_cache.direct.waiting"),
            Scope(window, "video_display.scene_cache.direct.warmup"),
            Scope(window, "video_display.scene_cache.direct.playback"),
            Scope(window, "video_display.scene_cache.direct.policy"),
            Scope(window, "video_display.scene_cache.direct.fallback"),
            Scope(window, "video_overlay.upload.full"),
            Scope(window, "video_overlay.upload.dirty"),
            Scope(window, "video_overlay.upload.reuse"),
            Scope(window, "video_overlay.upload.hide"),
            window.Where(entry => entry.Name == "video_ui_duration"
                    && (entry.Phase == "video_overlay.upload.full"
                        || entry.Phase == "video_overlay.upload.dirty"))
                .Sum(entry => (long)(entry.DetailA ?? 0)),
            environment);
    }

    private static TraceScope Scope(IEnumerable<TraceEntry> entries, string phase)
    {
        var values = entries
            .Where(entry => entry.Name == "video_ui_duration" && entry.Phase == phase)
            .Select(entry => entry.DurationMilliseconds)
            .OrderBy(value => value)
            .ToArray();
        if (values.Length == 0)
            return new TraceScope(0, 0.0, 0.0, 0.0, 0.0);
        return new TraceScope(
            values.Length,
            values.Sum(),
            Percentile(values, 0.50),
            Percentile(values, 0.95),
            values[^1]);
    }

    private static TraceScope ScopePrefix(IEnumerable<TraceEntry> entries, string phasePrefix)
    {
        var values = entries
            .Where(entry => entry.Name == "video_ui_duration"
                && entry.Phase is not null
                && entry.Phase.StartsWith(phasePrefix, StringComparison.Ordinal))
            .Select(entry => entry.DurationMilliseconds)
            .OrderBy(value => value)
            .ToArray();
        if (values.Length == 0)
            return new TraceScope(0, 0.0, 0.0, 0.0, 0.0);
        return new TraceScope(
            values.Length,
            values.Sum(),
            Percentile(values, 0.50),
            Percentile(values, 0.95),
            values[^1]);
    }

    private static double Percentile(IReadOnlyList<double> sorted, double percentile)
    {
        var rank = Math.Max(1, (int)Math.Ceiling(percentile * sorted.Count));
        return sorted[rank - 1];
    }

    private static int SummaryInt(IReadOnlyDictionary<string, string> summary, string key) =>
        summary.TryGetValue(key, out var value)
            && int.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed)
                ? parsed
                : 0;
}

sealed record TraceEntry(
    long TimestampNanoseconds,
    string Name,
    string? Phase,
    double DurationMilliseconds,
    int? DetailA,
    int? DetailB,
    string? Reason,
    string? RendererPrimary,
    string? SubtitlesProvider,
    int? Width,
    int? Height)
{
    public static TraceEntry Parse(string line)
    {
        using var document = JsonDocument.Parse(line);
        var root = document.RootElement;
        var payload = root.GetProperty("payload");
        return new TraceEntry(
            root.GetProperty("t_monotonic_ns").GetInt64(),
            root.GetProperty("name").GetString() ?? string.Empty,
            payload.TryGetProperty("phase", out var phase) ? phase.GetString() : null,
            payload.TryGetProperty("duration_ms", out var duration) ? duration.GetDouble() : 0.0,
            payload.TryGetProperty("detail_a", out var detailA) ? detailA.GetInt32() : null,
            payload.TryGetProperty("detail_b", out var detailB) ? detailB.GetInt32() : null,
            payload.TryGetProperty("reason", out var reason) ? reason.GetString() : null,
            payload.TryGetProperty("renderer_primary", out var renderer) ? renderer.GetString() : null,
            payload.TryGetProperty("subtitles_provider", out var provider) ? provider.GetString() : null,
            payload.TryGetProperty("width", out var width) ? width.GetInt32() : null,
            payload.TryGetProperty("height", out var height) ? height.GetInt32() : null);
    }
}

readonly record struct Position(double X, double Y)
{
    public static Position operator -(Position left, Position right) =>
        new(left.X - right.X, left.Y - right.Y);
}

readonly record struct PointI(int X, int Y);

readonly record struct RectI(int Left, int Top, int Width, int Height)
{
    public long Area => (long)Width * Height;
}

[Flags]
enum SetWindowPosFlags : uint
{
    NoActivate = 0x0010,
    ShowWindow = 0x0040,
}

[Flags]
enum SendMessageTimeoutFlags : uint
{
    Block = 0x0001,
    AbortIfHung = 0x0002,
}

enum WindowMessage : uint
{
    Null = 0x0000,
    Close = 0x0010,
    ButtonGetCheck = 0x00F0,
    ButtonGetState = 0x00F2,
    ButtonSetState = 0x00F3,
    MouseMove = 0x0200,
    LeftButtonDown = 0x0201,
    LeftButtonUp = 0x0202,
    LeftButtonDoubleClick = 0x0203,
    ToolbarGetState = 0x0412,
    ToolbarCommandToIndex = 0x0419,
}

[Flags]
enum KeyboardInputFlags : uint
{
    KeyUp = 0x0002,
}

[StructLayout(LayoutKind.Sequential)]
struct NativePoint
{
    public int X;
    public int Y;
}

[StructLayout(LayoutKind.Sequential)]
struct NativeRect
{
    public int Left;
    public int Top;
    public int Right;
    public int Bottom;
    public int Width => Right - Left;
    public int Height => Bottom - Top;
}

[StructLayout(LayoutKind.Sequential)]
struct GuiThreadInfo
{
    public uint Size;
    public uint Flags;
    public nint Active;
    public nint Focus;
    public nint Capture;
    public nint MenuOwner;
    public nint MoveSize;
    public nint Caret;
    public NativeRect CaretRect;

    public static GuiThreadInfo Create() => new()
    {
        Size = (uint)Marshal.SizeOf<GuiThreadInfo>(),
    };
}

[StructLayout(LayoutKind.Sequential)]
struct KeyboardInput
{
    public ushort VirtualKey;
    public ushort ScanCode;
    public KeyboardInputFlags Flags;
    public uint Time;
    public nint ExtraInfo;
}

[StructLayout(LayoutKind.Sequential)]
struct MouseInput
{
    public int X;
    public int Y;
    public uint Data;
    public uint Flags;
    public uint Time;
    public nint ExtraInfo;
}

[StructLayout(LayoutKind.Explicit)]
struct InputUnion
{
    [FieldOffset(0)] public MouseInput Mouse;
    [FieldOffset(0)] public KeyboardInput Keyboard;
}

[StructLayout(LayoutKind.Sequential)]
struct Input
{
    public uint Type;
    public InputUnion Union;

    public static Input KeyboardInput(ushort key, bool keyUp) => new()
    {
        Type = 1,
        Union = new InputUnion
        {
            Keyboard = new KeyboardInput
            {
                VirtualKey = key,
                Flags = keyUp ? KeyboardInputFlags.KeyUp : 0,
            },
        },
    };
}
