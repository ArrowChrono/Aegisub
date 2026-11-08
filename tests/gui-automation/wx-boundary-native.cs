#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Xml.Linq;
using Aegisub.GuiAutomation.Driver;

if (args.Length != 5)
{
    Console.Error.WriteLine("Usage: wx-boundary-native EXE ARTIFACTS ARTIFACT_ROOT LABEL FIXTURE_DIR");
    return 2;
}
try
{
    return PrivateDesktop.Run(args, Path.GetFullPath(args[1]), desktop => new WxBoundaryScenario(args, desktop).Run(),
        Path.GetFullPath(args[2]), 170_000, guardDescendants: true);
}
catch (Exception error)
{
    Console.Error.WriteLine(error);
    return 1;
}

sealed class WxBoundaryScenario(string[] args, PrivateDesktop desktop)
{
    const string AssertionProbeMarker = "AEGISUB_WX_BOUNDARY_ASSERTION_PROBE";
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    static readonly string[] DocumentFields =
    [
        "document_generation", "document_revision", "modified", "undo_empty", "redo_empty", "undo_description",
        "redo_description", "active_id", "selected_ids", "rows", "fold_groups", "display_rows", "video_frame"
    ];
    static readonly string[] PointerFields =
    ["display_mouse_valid", "display_mouse_x", "display_mouse_y", "tool_mouse_valid", "tool_mouse_x", "tool_mouse_y"];
    readonly Dictionary<string, CaseResult> results = new[]
    {
        "HostAndFoldSetup", "AssertionChannelProbe", "NativeBoundaryTranslation", "NativeUnexpectedCaptureLossXYHold",
        "NativeUnexpectedCaptureLossXYOrigin", "ActiveWithoutCaptureHeldBoundaryXYHold",
        "ActiveWithoutCaptureHeldBoundaryXYOrigin", "ActiveWithoutCaptureReleasedButtonEnterXYHold",
        "ActiveWithoutCaptureReleasedButtonEnterXYOrigin", "RuntimeAndTraceEvidence"
    }.ToDictionary(name => name, _ => new CaseResult("not-run", 0, null));
    readonly string executable = Path.GetFullPath(args[0]);
    readonly string artifacts = Path.GetFullPath(args[1]);
    readonly string fixtureDirectory = Path.GetFullPath(args[4]);
    readonly long deadline = Stopwatch.GetTimestamp() + 150 * Stopwatch.Frequency;
    readonly List<long> completedInputs = [];
    Process? app;
    nint main, canvas;
    string profile = "", host = "", fixture = "", phase = "HostAndFoldSetup";
    int checks, stateSequence, boundarySequence;
    long inputSequence;
    JsonElement lastState;

    public int Run()
    {
        Directory.CreateDirectory(artifacts);
        using var dpi = new DriverThreadDpiScope(artifacts);
        dpi.RecordActivation();
        desktop.RecordDriver();
        using var watchdog = new Timer(_ =>
        {
            try { WriteReports("150-second business deadline expired"); }
            finally
            {
                try { if (app is not null) desktop.Stop(app); }
                finally { Environment.Exit(124); }
            }
        }, null, 150_000, Timeout.Infinite);
        try
        {
            Case("HostAndFoldSetup", () =>
            {
                Prepare();
                Start();
                Command("Reload Scripts");
                Wait(() => WxNative.MenuLabels(main, "Automation").Contains("Visual Port Select Sources"), "Autoload registration");
                Command("Visual Port Select Sources", "Automation");
                Wait(() => State().GetProperty("selected_ids").GetArrayLength() == 3, "Three-source selection");
                Command("Fold Sources");
                Wait(() => Folded(State()), "Actual collapsed source projection");
                Save("initial-normalized");
                Require(Folded(State()), "Three selected sources include two hidden rows plus one unselected sentinel");
            });
            Case("AssertionChannelProbe", AssertionChannelProbe);
            Case("NativeBoundaryTranslation", NativeBoundaryTranslation);
            Case("NativeUnexpectedCaptureLossXYHold", () => NativeCaptureLoss(false));
            Case("NativeUnexpectedCaptureLossXYOrigin", () => NativeCaptureLoss(true));
            Case("ActiveWithoutCaptureHeldBoundaryXYHold", () => WithoutCaptureHeld(false));
            Case("ActiveWithoutCaptureHeldBoundaryXYOrigin", () => WithoutCaptureHeld(true));
            Case("ActiveWithoutCaptureReleasedButtonEnterXYHold", () => WithoutCaptureReleased(false));
            Case("ActiveWithoutCaptureReleasedButtonEnterXYOrigin", () => WithoutCaptureReleased(true));
            Case("RuntimeAndTraceEvidence", () =>
            {
                Require(completedInputs.SequenceEqual(Enumerable.Range(1, checked((int)inputSequence)).Select(value => (long)value)),
                    "Every assigned native input has exactly one successful completion receipt");
                Save("final");
                Send(main, 0x0010, 0, 0);
                if (!app!.WaitForExit(Math.Min(5000, RemainingMilliseconds())))
                    throw new TimeoutException("Normal owned wx close exceeded remaining deadline");
                Require(desktop.ExitCode(app) == 0, "Owned wx host exited normally with code zero");
                var traces = Directory.GetFiles(Path.Combine(profile, "user", "perf-sessions"), "trace.ndjson", SearchOption.AllDirectories);
                Require(traces.Length == 1, "Exactly one completed host trace exists");
                var names = new HashSet<string>();
                var traceRecords = 0;
                foreach (var line in File.ReadLines(traces[0]))
                {
                    using var item = JsonDocument.Parse(line);
                    ++traceRecords;
                    if (item.RootElement.GetProperty("name").GetString() == "video_ui_duration")
                        names.Add(item.RootElement.GetProperty("payload").GetProperty("phase").GetString()!);
                }
                Require(traceRecords > 0 && names.Contains("visual_tool.commit") && names.Contains("visual_tool.feedback_request"),
                    "Trace contains actual geometry commits and feedback activity, without a performance claim");
                Write("runtime-evidence.json", new { trace_records = traceRecords, native_inputs = inputSequence,
                    boundary_commands = boundarySequence, normal_exit = true, exit_code = desktop.ExitCode(app), physical_input = false });
            });
            WriteReports(null);
            ValidateReports();
            return 0;
        }
        catch (Exception error)
        {
            Write("failure.json", new { phase, error = error.ToString(), host_pid = app?.Id,
                state = lastState.ValueKind == JsonValueKind.Object ? lastState : (object?)null, physical_input = false });
            WriteReports(error.ToString());
            ValidateReports();
            Console.Error.WriteLine(error);
            return 1;
        }
        finally
        {
            if (app is not null)
            {
                try
                {
                    desktop.Stop(app);
                    Write("owned-cleanup.json", new { pid = app.Id, has_exited = app.HasExited,
                        exit_code = app.HasExited ? desktop.ExitCode(app) : (int?)null });
                }
                finally { app.Dispose(); }
            }
        }
    }

    void Prepare()
    {
        profile = Path.Combine(artifacts, "profile");
        host = Path.Combine(artifacts, "host");
        Directory.CreateDirectory(Path.Combine(profile, "user", "automation", "autoload"));
        Directory.CreateDirectory(host);
        fixture = Path.Combine(artifacts, "fixture.ass");
        File.Copy(Path.Combine(fixtureDirectory, "fixture.ass"), fixture);
        File.Copy(Path.Combine(fixtureDirectory, "select-sources.lua"), Path.Combine(profile, "user", "automation", "autoload", "select-sources.lua"));
        File.WriteAllText(Path.Combine(profile, "user", "config.json"), """
          {"App":{"First Start":false,"Language":"en_US","Save UI State":true,"Auto":{"Backup":false,"Check For Updates":false,"Load Linked Files":0,"Save":false,"Save on Every Change":false}},"Subtitle":{"Provider":"libass","Use STC":true},"Video":{"Provider":"YUV4MPEG","Open Audio":false,"Default Zoom":7,"Scale with DPI":false,"Renderer":{"Backend":"opengl"}},"Tool":{"Visual":{"Autohide":true}},"Automation":{"Lua Workspace":{"Enable LuaLS":false}},"Path":{"Automation":{"Autoload":"?user/automation/autoload/"}}}
          """);
        AutomationTestProfile.WriteMenusWithoutClipboardValidation(profile, artifacts);
        var menuPath = Path.Combine(profile, "user", "menu.json");
        var menus = JsonNode.Parse(File.ReadAllText(menuPath))!.AsObject();
        menus["main"]!.AsArray().Add(new JsonObject { ["submenu"] = "wx-boundary", ["text"] = "WX Boundary Test" });
        JsonArray commands = [];
        foreach (var (name, label) in new[]
        {
            ("am/reload/autoload", "Reload Scripts"), ("subtitle/save", "Save"), ("edit/undo", "Undo"),
            ("grid/fold/create", "Fold Sources"), ("video/tool/geometry_linking", "Linking"),
            ("video/tool/cross", "Cross"), ("video/tool/rotate/xy", "XY")
        }) commands.Add(new JsonObject { ["command"] = name, ["text"] = label });
        menus["wx-boundary"] = commands;
        File.WriteAllText(menuPath, menus.ToJsonString(Json));
        using var video = File.Create(Path.Combine(artifacts, "fixture.y4m"));
        video.Write(Encoding.ASCII.GetBytes("YUV4MPEG2 W640 H480 F25:1 Ip A1:1 C420jpeg\n"));
        var luma = new byte[640 * 480];
        for (var y = 0; y < 480; ++y)
            for (var x = 0; x < 640; ++x)
                luma[y * 640 + x] = (byte)(40 + ((x / 32 + y / 32) % 2) * 20);
        var chroma = Enumerable.Repeat((byte)128, 320 * 240).ToArray();
        for (var i = 0; i < 40; ++i) { video.Write("FRAME\n"u8); video.Write(luma); video.Write(chroma); video.Write(chroma); }
        video.Flush();
        Write("contract.json", new { label = args[3], cases = results.Keys, business_seconds = 150,
            private_desktop_seconds = 170, application_starts = 1, retries = 0, video_frame = 0,
            renderer = "opengl", subtitle_provider = "libass", physical_input = false, global_input = false,
            input = "owned native messages plus explicitly labeled synthetic wx events", performance_acceptance = false,
            assertion_channel = "startup-opt-in with retained positive probe", expected_probe_marker = AssertionProbeMarker,
            ordinary_boundary_expected_assertions = 0 });
    }

    void Start()
    {
        var start = new ProcessStartInfo(executable) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        start.Environment["AEGISUB_ENABLE_DXGI_PRESENTATION"] = "0";
        start.Environment["AEGISUB_GUI_TEST_WX_ASSERTIONS"] = "1";
        start.Environment["AEGISUB_PERF_TRACE"] = "video,wx-boundary-native";
        foreach (var value in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", host,
            "--open", fixture, "--open", Path.Combine(artifacts, "fixture.y4m") }) start.ArgumentList.Add(value);
        app = desktop.Start(start, "wx-boundary-host", deadline);
        Wait(() => File.Exists(Path.Combine(host, "ready.json")), "Real wx host ready receipt");
        using var ready = JsonDocument.Parse(File.ReadAllText(Path.Combine(host, "ready.json")));
        Require(ready.RootElement.GetProperty("process_id").GetInt32() == app.Id, "Ready receipt belongs to exact owned host PID");
        main = desktop.MainWindow(app, Math.Max(1, Math.Min(10, RemainingMilliseconds() / 1000)), deadline);
        desktop.VerifyWindow(main, app);
        if (WxNative.SetWindowPos(main, 0, 40, 40, 1280, 960, 0x50) == 0)
            throw new Win32Exception(Marshal.GetLastPInvokeError(), "SetWindowPos");
        Wait(() =>
        {
            canvas = WxNative.Children(main).Where(WxNative.IsVisible).Where(h => PrivateDesktop.ClassName(h) == "wxGLCanvas")
                .OrderByDescending(h => { WxNative.GetClientRect(h, out var bounds); return (long)bounds.Right * bounds.Bottom; }).FirstOrDefault();
            return canvas != 0 && State().GetProperty("viewport").GetProperty("width").GetDouble() > 0;
        }, "Live owned wxGLCanvas and positive viewport");
        desktop.VerifyWindow(canvas, app);
        if (WxNative.GetClientRect(canvas, out var client) == 0)
            throw new Win32Exception(Marshal.GetLastPInvokeError(), "Canvas GetClientRect");
        Write("canvas-target.json", new { hwnd = (long)canvas, main_hwnd = (long)main, pid = app.Id,
            native_class = PrivateDesktop.ClassName(canvas), width = client.Right, height = client.Bottom });
        Require(!WxNative.MenuChecked(main, "WX Boundary Test", "Linking"), "Geometry Linking starts disabled");
    }

    void AssertionChannelProbe()
    {
        var baseline = State();
        Require(Folded(baseline), "Assertion probe starts with the representative folded source selection");
        var receipt = DispatchBoundary("assertion-channel-probe", Pixel(baseline, 320, 180));
        var assertions = receipt.GetProperty("assertions");
        Require(assertions.GetArrayLength() == 1, "Positive probe retains exactly one real wx assertion");
        var assertion = assertions[0];
        Require(assertion.GetProperty("message").GetString() == AssertionProbeMarker
            && assertion.GetProperty("condition").GetString() == "Assert failure"
            && assertion.GetProperty("file").GetString() == "gui_test_wx_boundary.cpp"
            && assertion.GetProperty("function").GetString()!.EndsWith("ProbeAssertionChannel", StringComparison.Ordinal)
            && assertion.GetProperty("line").GetInt32() > 0
            && assertion.GetProperty("step_index").GetInt64() == -1
            && assertion.GetProperty("step_name").GetString() == "outside-step",
            "Positive probe records the exact diagnostic marker, condition, source location and standalone context");
        Require(receipt.GetProperty("wx_events").GetArrayLength() == 0
            && receipt.GetProperty("native_events").GetArrayLength() == 0
            && receipt.GetProperty("steps").GetArrayLength() == 0,
            "Positive assertion probe emits no wx/native boundary events or transaction steps");
        var before = receipt.GetProperty("before");
        var after = receipt.GetProperty("after");
        SameDocument(baseline, before, "Positive probe starts at the observed document and history boundary");
        SameDocument(before, after, "Positive probe preserves all source rows, selection, folding and exposed history");
        SamePointers(before, after, "Positive probe preserves both pointer states");
        Require(Equal(before, after), "Positive probe preserves the complete observed state, including capture, interaction and pacing");
    }

    Gesture Begin(bool origin)
    {
        Command("XY");
        var original = Save(phase + "-baseline");
        var baseline = State();
        Require(Folded(baseline), "Gesture starts with the actual folded three-source selection");
        var start = Pixel(baseline, origin ? 220 : 320, 180);
        var middle = new Point(start.X + 23, start.Y - 11);
        Mouse(0x0200, start, 0);
        Mouse(0x0201, start, 1);
        Mouse(0x0200, middle, 1);
        var preview = State();
        Interaction(preview, captured: true, active: true, "Native press and motion establish a captured interaction");
        Require(preview.GetProperty("interaction_pacing_active").GetBoolean(), "Gesture starts interaction pacing");
        ChangedSources(baseline, preview);
        return new(original, baseline, preview, start, middle);
    }

    void NativeBoundaryTranslation()
    {
        Command("XY");
        var original = Save(phase + "-baseline");
        var baseline = State();
        Require(Folded(baseline), "Native boundary transaction starts with the folded three-source selection");
        var start = Pixel(baseline, 320, 180);
        var transaction = Boundary("native-boundary-transaction", start);
        var steps = NativeTransaction(transaction, start, ["align", "prime-leave", "enter", "down", "leave", "reenter"]);
        SameDocument(baseline, transaction.GetProperty("before"), "Native transaction begins at the saved document/history boundary");
        var enter = steps[2];
        NativeStep(enter, 0x0200, ["enter", "motion"], start, false, false, false, false);
        Pointer(enter.GetProperty("after"), start);
        SameDocument(baseline, enter.GetProperty("after"), "Uncaptured native Enter preserves document/history/selection");
        var down = steps[3];
        NativeStep(down, 0x0201, ["left-down"], start, true, false, false, false);
        var captured = down.GetProperty("after");
        Interaction(captured, true, true, "Native down starts held capture before translated boundaries");
        Require(captured.GetProperty("interaction_pacing_active").GetBoolean(), "Native transaction press starts interaction pacing");
        var leave = steps[4];
        var leavePoint = CursorPoint(leave.GetProperty("cursor_before"));
        NativeStep(leave, 0x02A3, ["leave"], leavePoint, false, true, true, false);
        SameDocument(captured, leave.GetProperty("after"), "Captured translated Leave preserves document/history/selection");
        SamePointers(captured, leave.GetProperty("after"), "Captured translated Leave preserves both pointers");
        Interaction(leave.GetProperty("after"), true, true, "False-button native Leave cannot end captured hold");
        Require(leave.GetProperty("after").GetProperty("interaction_pacing_active").GetBoolean(), "Captured native Leave keeps pacing active");
        var reenter = steps[5];
        NativeStep(reenter, 0x0200, ["enter", "motion"], start, true, false, false, false);
        var beforeMotion = reenter.GetProperty("wx_events")[1].GetProperty("state_before");
        SameDocument(captured, beforeMotion, "Captured translated Enter preserves document/history/selection before the following Motion");
        SamePointers(captured, beforeMotion, "Captured translated Enter preserves both pointers before the following Motion");
        Interaction(beforeMotion, true, true, "Captured translated Enter keeps hold alive before the following Motion");
        Require(beforeMotion.GetProperty("interaction_pacing_active").GetBoolean(), "Captured translated Enter keeps pacing active before the following Motion");
        Interaction(reenter.GetProperty("after"), true, true, "Native reentry and its following held Motion keep hold alive");
        Require(reenter.GetProperty("after").GetProperty("interaction_pacing_active").GetBoolean(), "Native reentry and its following held Motion keep pacing active");
        SameSelection(captured, reenter.GetProperty("after"));
        SameSentinel(captured, reenter.GetProperty("after"));
        Mouse(0x0200, new(start.X + 23, start.Y - 11), 1);
        Mouse(0x0202, new(start.X + 47, start.Y + 18), 0);
        var completed = State();
        Interaction(completed, false, false, "Native release ends the continued gesture");
        Require(!completed.GetProperty("interaction_pacing_active").GetBoolean(), "Native release stops interaction pacing");
        ChangedSources(baseline, completed);
        UndoExact(new(original, baseline, completed, start, start));
        var idle = Boundary("native-idle-boundary-transaction", start);
        var idleSteps = NativeTransaction(idle, start, ["align", "prime-leave", "enter", "leave"]);
        NativeStep(idleSteps[2], 0x0200, ["enter", "motion"], start, false, false, false, false);
        Pointer(idleSteps[2].GetProperty("after"), start);
        var idleLeave = idleSteps[3];
        NativeStep(idleLeave, 0x02A3, ["leave"], CursorPoint(idleLeave.GetProperty("cursor_before")), false, false, false, false);
        Pointer(idleLeave.GetProperty("after"), null);
        Interaction(idleLeave.GetProperty("after"), false, false, "Idle native Enter/Leave does not create an interaction");
        Require(!idleLeave.GetProperty("after").GetProperty("interaction_pacing_active").GetBoolean(), "Idle native boundaries do not restart pacing");
        SameDocument(idle.GetProperty("before"), idle.GetProperty("after"), "Fresh idle Enter/Leave transaction preserves the complete document/history");
        Boundary("native-restore", start);
    }

    JsonElement[] NativeTransaction(JsonElement receipt, Point point, string[] names)
    {
        var steps = receipt.GetProperty("steps").EnumerateArray().ToArray();
        Require(steps.Length == names.Length, "Native transaction includes every fixed step and no extra step");
        Require(Equal(receipt.GetProperty("before"), steps[0].GetProperty("before"))
            && Equal(receipt.GetProperty("after"), steps[^1].GetProperty("after")),
            "Transaction endpoint states exactly match its first and last step");
        Interaction(receipt.GetProperty("before"), false, false, "Native transaction alignment starts idle without capture");
        for (var i = 0; i < steps.Length; ++i)
        {
            var step = steps[i];
            Require(step.GetProperty("index").GetInt32() == i && step.GetProperty("name").GetString() == names[i],
                "Native transaction step order and identity match the fixed protocol");
            if (i > 0)
            {
                Require(Equal(steps[i - 1].GetProperty("after"), step.GetProperty("before")),
                    "Consecutive synchronous step snapshots have no unobserved state gap");
                SameCursor(steps[i - 1].GetProperty("cursor_after"), step.GetProperty("cursor_before"));
            }
            foreach (var field in new[] { "wx_events", "native_events" })
            {
                foreach (var item in step.GetProperty(field).EnumerateArray())
                {
                    Require(item.GetProperty("step_index").GetInt32() == i && item.GetProperty("step_name").GetString() == names[i],
                        "Every observed event belongs to its explicit continuous-observer step");
                    if (field == "native_events")
                        Require(item.GetProperty("canvas_identity").GetInt64() == (long)canvas,
                            "Every actual native observation belongs to the owned canvas");
                    else
                    {
                        var entry = item.GetProperty("state_before");
                        Require(item.GetProperty("native_capture").GetBoolean() == entry.GetProperty("native_capture").GetBoolean()
                            && item.GetProperty("visual_interacting").GetBoolean() == entry.GetProperty("visual_interacting").GetBoolean(),
                            "Every actual wx event records capture and interaction ownership consistently with its full entry state");
                    }
                }
            }
            CursorEvidence(step.GetProperty("cursor_before"), requireHit: i > 0);
            CursorEvidence(step.GetProperty("cursor_after"), requireHit: true);
            if (i > 0) SameCursor(step.GetProperty("cursor_before"), step.GetProperty("cursor_after"));
        }
        foreach (var field in new[] { "wx_events", "native_events" })
        {
            var all = receipt.GetProperty(field).EnumerateArray().ToArray();
            var segmented = steps.SelectMany(step => step.GetProperty(field).EnumerateArray()).ToArray();
            Require(all.Length == segmented.Length && all.Zip(segmented).All(pair => Equal(pair.First, pair.Second)),
                "Full continuous observation exactly matches ordered step slices without dropped or unassigned events");
        }
        var align = steps[0];
        Require(CursorPoint(align.GetProperty("cursor_after")) == point, "Alignment independent client coordinate equals the requested start");
        Require(align.GetProperty("native_events").EnumerateArray().All(item => item.GetProperty("message").GetInt32() is 0x0200 or 0x02A3)
            && align.GetProperty("wx_events").EnumerateArray().All(item => item.GetProperty("kind").GetString() is "enter" or "leave" or "motion"),
            "Alignment retains boundary feedback only, never capture changes or button input");
        Interaction(align.GetProperty("after"), false, false, "Alignment cannot create capture or a geometry interaction");
        SameDocument(align.GetProperty("before"), align.GetProperty("after"), "Alignment preserves document/history/selection");
        var prime = steps[1];
        var primeKinds = prime.GetProperty("wx_events").EnumerateArray().Select(item => item.GetProperty("kind").GetString()).ToArray();
        Require(primeKinds.Length == 0 || primeKinds.SequenceEqual(["leave"]),
            "Prime native Leave observes only the optional initial inside-state transition");
        NativeStep(prime, 0x02A3, primeKinds.Select(kind => kind!).ToArray(), CursorPoint(prime.GetProperty("cursor_before")), false, false, false, false);
        Interaction(prime.GetProperty("after"), false, false, "Prime native Leave stays idle and uncaptured");
        SameDocument(prime.GetProperty("before"), prime.GetProperty("after"), "Prime native Leave preserves document/history/selection");
        return steps;
    }

    void NativeStep(JsonElement step, int message, string[] kinds, Point point, bool left, bool shift, bool control, bool alt)
    {
        var native = step.GetProperty("native_events").EnumerateArray().ToArray();
        Require(native.Length == 1 && native[0].GetProperty("message").GetInt32() == message,
            "Native dispatch produces exactly its intended actual message without reentrant input");
        Require(native[0].GetProperty("wparam").GetInt64() == ((left ? 1 : 0) | (shift ? 4 : 0) | (control ? 8 : 0)),
            "Actual native button and modifier flags match the transaction step");
        Require(native[0].GetProperty("capture_owner").GetInt64() ==
            (step.GetProperty("before").GetProperty("native_capture").GetBoolean() ? (long)canvas : 0),
            "Native event entry capture owner matches the independently observed step state");
        var observedKinds = step.GetProperty("wx_events").EnumerateArray().Select(item => item.GetProperty("kind").GetString());
        Require(observedKinds.SequenceEqual(kinds), "Native dispatch yields the exact ordered actual wx event sequence");
        foreach (var kind in kinds) MouseEvent(step, kind, point, left, shift, control, alt);
    }

    void CursorEvidence(JsonElement cursor, bool requireHit)
    {
        var source = cursor.GetProperty("cursor_source").GetString();
        var actual = cursor.GetProperty("get_cursor_pos_succeeded").GetBoolean();
        Require(source == (actual ? "GetCursorPos" : "GetMessagePos"), "Cursor oracle identifies its actual observation source");
        if (actual) Require(cursor.GetProperty("get_cursor_pos_error").GetInt32() == 0, "Successful cursor observation records no Win32 error");
        else
        {
            var packed = cursor.GetProperty("message_position").GetInt64();
            Require(cursor.GetProperty("cursor_x").GetInt32() == unchecked((short)(packed & 0xffff))
                && cursor.GetProperty("cursor_y").GetInt32() == unchecked((short)((packed >> 16) & 0xffff)),
                "Fallback cursor screen coordinates independently decode the retained GUI message position");
        }
        _ = CursorPoint(cursor);
        if (requireHit) Require(cursor.GetProperty("cursor_window_matches").GetBoolean()
            && cursor.GetProperty("cursor_window").GetInt64() != 0, "Cursor oracle hits the actual owned canvas ancestry");
    }

    void SameCursor(JsonElement before, JsonElement after) =>
        Require(new[] { "cursor_source", "get_cursor_pos_succeeded", "get_cursor_pos_error", "cursor_x", "cursor_y", "client_x", "client_y", "cursor_window", "cursor_window_matches" }
            .All(name => Equal(before.GetProperty(name), after.GetProperty(name))),
            "Cursor provenance, screen/client coordinates and hit identity stay stable throughout native dispatch");

    static Point CursorPoint(JsonElement cursor) => new(cursor.GetProperty("client_x").GetInt32(), cursor.GetProperty("client_y").GetInt32());

    void NativeCaptureLoss(bool origin)
    {
        var gesture = Begin(origin);
        var loss = Boundary("native-capture-loss", gesture.Middle);
        NativeEvent(loss, 0x0215);
        Require(loss.GetProperty("native_events").EnumerateArray().Any(item => item.GetProperty("message").GetInt32() == 0x0215
            && item.GetProperty("capture_owner").GetInt64() == 0 && item.GetProperty("lparam").GetInt64() == 0),
            "Native capture loss records the actual release to no capture owner");
        Require(loss.GetProperty("wx_events").EnumerateArray().Any(item => item.GetProperty("kind").GetString() == "capture-lost"),
            "WM_CAPTURECHANGED produces an actual wx capture-lost callback");
        Interaction(loss.GetProperty("after"), false, false, "Unexpected native capture loss cancels the interaction");
        Require(!loss.GetProperty("after").GetProperty("interaction_pacing_active").GetBoolean(), "Native capture loss stops pacing");
        SameDocument(gesture.Preview, loss.GetProperty("after"), "Native cancellation retains preview without an additional commit");
        LateInputStaysInert(loss.GetProperty("after"), gesture.Start);
        UndoExact(gesture);
    }

    void WithoutCaptureHeld(bool origin)
    {
        var gesture = Begin(origin);
        var released = Boundary("release-capture", gesture.Middle);
        SameDocument(gesture.Preview, released.GetProperty("after"), "Expected release only changes capture ownership");
        Interaction(released.GetProperty("after"), false, true, "Expected release leaves an active uncaptured interaction");
        var outside = new Point(gesture.Middle.X + 9, gesture.Middle.Y + 7);
        var leave = Boundary("leave", outside, true);
        MouseEvent(leave, "leave", outside, true, false, false, false);
        Pointer(leave.GetProperty("after"), null);
        SameDocument(released.GetProperty("after"), leave.GetProperty("after"), "Uncaptured held Leave clears pointers without a commit");
        Interaction(leave.GetProperty("after"), false, true, "Uncaptured held Leave keeps interaction alive");
        var inside = new Point(gesture.Start.X + 47, gesture.Start.Y + 18);
        var enter = Boundary("enter", inside, true);
        MouseEvent(enter, "enter", inside, true, false, false, false);
        Pointer(enter.GetProperty("after"), inside);
        Interaction(enter.GetProperty("after"), false, true, "Uncaptured held Enter is processed without recapturing");
        ChangedSources(leave.GetProperty("after"), enter.GetProperty("after"));
        Require(enter.GetProperty("after").GetProperty("document_revision").GetInt64() > leave.GetProperty("after").GetProperty("document_revision").GetInt64(),
            "Uncaptured held Enter genuinely updates geometry rather than blanket ignoring the boundary");
        Command("Cross");
        var cancelled = State();
        SameDocument(enter.GetProperty("after"), cancelled, "Explicit tool cancellation creates no additional document commit");
        Interaction(cancelled, false, false, "Cross explicitly cancels the uncaptured interaction");
        Require(!cancelled.GetProperty("interaction_pacing_active").GetBoolean(), "Cross cancellation stops pacing");
        LateInputStaysInert(cancelled, gesture.Start);
        UndoExact(gesture);
    }

    void WithoutCaptureReleased(bool origin)
    {
        var gesture = Begin(origin);
        var released = Boundary("release-capture", gesture.Middle);
        Interaction(released.GetProperty("after"), false, true, "False-button edge starts active without capture");
        SameDocument(gesture.Preview, released.GetProperty("after"), "Expected release does not commit");
        var enter = Boundary("enter", gesture.Middle, false);
        MouseEvent(enter, "enter", gesture.Middle, false, false, false, false);
        Interaction(enter.GetProperty("after"), false, false, "False-button Enter ends the uncaptured interaction without wx assertions");
        Require(!enter.GetProperty("after").GetProperty("interaction_pacing_active").GetBoolean(), "False-button Enter stops pacing");
        SameSelection(gesture.Baseline, enter.GetProperty("after"));
        SameSentinel(gesture.Baseline, enter.GetProperty("after"));
        LateInputStaysInert(enter.GetProperty("after"), gesture.Start);
        UndoExact(gesture);
    }

    void LateInputStaysInert(JsonElement before, Point start)
    {
        Mouse(0x0200, new(start.X + 52, start.Y + 31), 1);
        Mouse(0x0202, new(start.X + 68, start.Y + 44), 0);
        var after = State();
        SameDocument(before, after, "Late native Motion/Up cannot revive a cancelled or ended gesture");
        Interaction(after, false, false, "Late Motion/Up cannot restore capture or active interaction");
        Require(!after.GetProperty("interaction_pacing_active").GetBoolean(), "Late Motion/Up cannot restart pacing");
    }

    void UndoExact(Gesture gesture)
    {
        Require(Save(phase + "-changed") != gesture.Original, "Gesture retained a genuine geometry edit");
        Command("Undo");
        Require(Save(phase + "-undo") == gesture.Original, "One Undo restores exact script/style/event/extradata serialization");
        var restored = State();
        SameSelection(gesture.Baseline, restored);
        Require(Equal(gesture.Baseline.GetProperty("rows"), restored.GetProperty("rows")), "Undo restores every source event and row identity exactly");
        Require(Equal(gesture.Baseline.GetProperty("undo_empty"), restored.GetProperty("undo_empty"))
            && Equal(gesture.Baseline.GetProperty("undo_description"), restored.GetProperty("undo_description")),
            "One Undo returns to the pre-gesture history boundary");
        Require(!restored.GetProperty("redo_empty").GetBoolean(), "Undone gesture remains a real redo entry");
    }

    JsonElement Boundary(string kind, Point point, bool left = false, bool shift = false, bool control = false, bool alt = false)
    {
        var reply = DispatchBoundary(kind, point, left, shift, control, alt);
        Require(reply.GetProperty("assertions").GetArrayLength() == 0, "Boundary dispatch emitted no wx assertions; inspect retained receipt");
        if (kind == "release-capture")
            Require(!reply.GetProperty("wx_events").EnumerateArray().Any(item => item.GetProperty("kind").GetString() == "capture-lost"),
                "Expected wx release suppresses unexpected-capture-loss notification");
        return reply;
    }

    JsonElement DispatchBoundary(string kind, Point point, bool left = false, bool shift = false, bool control = false, bool alt = false)
    {
        Check();
        desktop.VerifyWindow(canvas, app!);
        var sequence = ++boundarySequence;
        var token = Guid.NewGuid().ToString("N");
        var request = new { sequence, token, kind, canvas_identity = (long)canvas, x = point.X, y = point.Y,
            left_down = left, shift_down = shift, control_down = control, alt_down = alt };
        var name = "wx-boundary-" + sequence;
        var replyPath = Path.Combine(host, name + ".json");
        Require(!File.Exists(replyPath), "Boundary sequence has no stale receipt");
        Write(name + "-request.json", request);
        File.WriteAllText(Path.Combine(host, "wx-boundary-request.json"), JsonSerializer.Serialize(request, Json));
        Send(main, 0x0111, 0x7FF6, 0);
        var reply = ReadReply(replyPath, sequence);
        Write(name + "-receipt.json", reply);
        Require(reply.GetProperty("sequence").GetInt32() == sequence && reply.GetProperty("token").GetString() == token
            && reply.GetProperty("kind").GetString() == kind && reply.GetProperty("canvas_identity").GetInt64() == (long)canvas,
            "Boundary receipt matches exact sequence/token/kind/canvas identity");
        Require(reply.GetProperty("status").GetString() == "dispatched", "Boundary operation completed successfully");
        lastState = reply.GetProperty("after").Clone();
        foreach (var channelName in new[] { "assertion_channel_before", "assertion_channel_after" })
        {
            var channel = reply.GetProperty(channelName);
            Require(channel.GetProperty("wx_debug_level").GetInt32() >= 1
                && channel.GetProperty("mode").GetString() == "startup-opt-in"
                && channel.GetProperty("handler_enabled").GetBoolean()
                && channel.GetProperty("handler_matches_startup").GetBoolean(),
                channelName + " proves compiled assertions and the unchanged enabled startup handler");
        }
        Require(reply.GetProperty("wx_events").ValueKind == JsonValueKind.Array && reply.GetProperty("native_events").ValueKind == JsonValueKind.Array,
            "Boundary receipt explicitly provides actual wx and native event arrays");
        return reply;
    }

    void MouseEvent(JsonElement receipt, string kind, Point point, bool left, bool shift, bool control, bool alt)
    {
        var observed = receipt.GetProperty("wx_events").EnumerateArray().Where(item => item.GetProperty("kind").GetString() == kind).ToArray();
        Require(observed.Length == 1, "Exactly one actual wx " + kind + " was received");
        var item = observed[0];
        Require(item.GetProperty("x").GetInt32() == point.X && item.GetProperty("y").GetInt32() == point.Y
            && item.GetProperty("left_down").GetBoolean() == left && item.GetProperty("shift_down").GetBoolean() == shift
            && item.GetProperty("control_down").GetBoolean() == control && item.GetProperty("alt_down").GetBoolean() == alt,
            "Actual wx event coordinates/button/modifier payload matches the intended input");
        Require(item.GetProperty("native_capture").GetBoolean() == receipt.GetProperty("before").GetProperty("native_capture").GetBoolean()
            && item.GetProperty("visual_interacting").GetBoolean() == receipt.GetProperty("before").GetProperty("visual_interacting").GetBoolean(),
            "Event records capture and interaction ownership at entry");
    }

    void NativeEvent(JsonElement receipt, int message) =>
        Require(receipt.GetProperty("native_events").EnumerateArray().Any(item => item.GetProperty("message").GetInt32() == message),
            "Actual native message receipt includes " + message.ToString("X"));

    void Pointer(JsonElement state, Point? point)
    {
        foreach (var prefix in new[] { "display", "tool" })
        {
            Require(state.GetProperty(prefix + "_mouse_valid").GetBoolean() == point.HasValue, prefix + " pointer visibility matches boundary");
            if (point is Point value)
                Require(state.GetProperty(prefix + "_mouse_x").GetDouble() == value.X && state.GetProperty(prefix + "_mouse_y").GetDouble() == value.Y,
                    prefix + " pointer uses exact boundary coordinates");
        }
    }

    void Interaction(JsonElement state, bool captured, bool active, string label) =>
        Require(state.GetProperty("native_capture").GetBoolean() == captured && state.GetProperty("visual_interacting").GetBoolean() == active, label);

    void SameDocument(JsonElement before, JsonElement after, string label) => Require(DocumentFields.All(name => Equal(before.GetProperty(name), after.GetProperty(name))), label);
    void SamePointers(JsonElement before, JsonElement after, string label) => Require(PointerFields.All(name => Equal(before.GetProperty(name), after.GetProperty(name))), label);
    void SameSelection(JsonElement before, JsonElement after) => Require(new[] { "active_id", "fold_groups", "display_rows", "viewport", "video_frame" }
        .All(name => Equal(before.GetProperty(name), after.GetProperty(name)))
        && before.GetProperty("selected_ids").EnumerateArray().Select(item => item.GetInt64()).Order()
            .SequenceEqual(after.GetProperty("selected_ids").EnumerateArray().Select(item => item.GetInt64()).Order()),
        "Active/hidden selection, folding, viewport and fixed frame are preserved");
    void SameSentinel(JsonElement before, JsonElement after) => Require(Equal(before.GetProperty("rows")[3], after.GetProperty("rows")[3]), "Unselected sentinel remains exact");
    void ChangedSources(JsonElement before, JsonElement after)
    {
        SameSelection(before, after);
        SameSentinel(before, after);
        for (var i = 0; i < 3; ++i)
            Require(before.GetProperty("rows")[i].GetProperty("text").GetString() != after.GetProperty("rows")[i].GetProperty("text").GetString(),
                "Selected source " + i + " receives the genuine geometry edit, including hidden sources");
    }

    JsonElement State()
    {
        var sequence = ++stateSequence;
        var path = Path.Combine(host, "visual-state-" + sequence + ".json");
        Require(!File.Exists(path), "State sequence has no stale receipt");
        Append("observer-requests.ndjson", new { sequence, command = 0x7FFD, pid = app!.Id, hwnd = (long)main });
        desktop.VerifyWindow(main, app);
        if (!WxNative.PostMessageW(main, 0x0111, 0x7FFD, 0))
            throw new Win32Exception(Marshal.GetLastPInvokeError(), "Readonly state request");
        lastState = ReadReply(path, sequence);
        return lastState;
    }

    JsonElement ReadReply(string path, int sequence)
    {
        FileStream? file = null;
        try
        {
            Wait(() =>
            {
                if (!File.Exists(path)) return false;
                try { file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read); }
                catch (IOException error) when (error.HResult is unchecked((int)0x80070020) or unchecked((int)0x80070021))
                {
                    Append("observer-pending.ndjson", new { file = Path.GetFileName(path), sequence, hresult = error.HResult });
                    return false;
                }
                return true;
            }, "Observer publication " + sequence);
            using var document = JsonDocument.Parse(file ?? throw new InvalidOperationException("Observer stream not acquired"));
            var result = document.RootElement.Clone();
            Require(result.GetProperty("sequence").GetInt32() == sequence, "Observer reply is from the current request");
            return result;
        }
        finally { file?.Dispose(); }
    }

    string Save(string label)
    {
        Command("Save");
        Wait(() => !State().GetProperty("modified").GetBoolean(), "Actual save checkpoint");
        File.Copy(fixture, Path.Combine(artifacts, label + ".ass"));
        string section = "";
        List<string> retained = [];
        foreach (var line in File.ReadLines(fixture))
        {
            if (line.StartsWith('[')) section = line;
            if (section is "[Script Info]" or "[V4+ Styles]" or "[Events]" or "[Aegisub Extradata]") retained.Add(line);
        }
        return string.Join('\n', retained);
    }

    void Command(string label, string menu = "WX Boundary Test")
    {
        Check();
        var id = ObservedWindowsCommand.FindMenuCommand(main, app!.Id, menu, label);
        Append("commands.ndjson", new { phase, menu, label, id, pid = app.Id });
        Send(main, 0x0111, id, 0);
        if (menu == "WX Boundary Test" && label is "XY" or "Cross")
            Require(WxNative.MenuChecked(main, menu, label), "Actual native tool radio state matches the requested command");
    }

    void Mouse(uint message, Point point, int keys)
    {
        Check();
        desktop.VerifyWindow(canvas, app!);
        WxNative.VerifyPoint(canvas, point.X, point.Y);
        var sequence = ++inputSequence;
        Append("input.ndjson", new { sequence, phase, message, x = point.X, y = point.Y, keys, hwnd = (long)canvas });
        var completed = false;
        try
        {
            Send(canvas, message, keys, unchecked((nint)(((long)(ushort)(short)point.Y << 16) | (ushort)(short)point.X)));
            completedInputs.Add(sequence);
            completed = true;
        }
        finally { Append("input-completion.ndjson", new { sequence, phase, completed }); }
    }

    nint Send(nint window, uint message, nint wp, nint lp)
    {
        Check();
        desktop.VerifyWindow(window, app!);
        return WxNative.Send(window, message, wp, lp, (uint)Math.Min(1000, RemainingMilliseconds()));
    }
    void Wait(Func<bool> predicate, string label)
    {
        if (!SpinWait.SpinUntil(() => { Check(); return predicate(); }, Math.Min(10_000, RemainingMilliseconds())))
            throw new TimeoutException(label);
    }
    void Check()
    {
        if (Stopwatch.GetTimestamp() >= deadline) throw new TimeoutException("Non-resettable 150-second wx boundary deadline");
        if (app is not null && app.HasExited) throw new InvalidOperationException("Owned wx host exited early: " + desktop.ExitCode(app));
    }
    int RemainingMilliseconds() => Math.Max(1, checked((int)Math.Min(int.MaxValue, (deadline - Stopwatch.GetTimestamp()) * 1000 / Stopwatch.Frequency)));
    void Require(bool valid, string description) { ++checks; if (!valid) throw new InvalidOperationException(description); }
    void Write(string name, object value) => File.WriteAllText(Path.Combine(artifacts, name), JsonSerializer.Serialize(value, Json));
    void Append(string name, object value) => File.AppendAllText(Path.Combine(artifacts, name), JsonSerializer.Serialize(value) + Environment.NewLine);
    static bool Equal(JsonElement first, JsonElement second) => JsonNode.DeepEquals(JsonNode.Parse(first.GetRawText()), JsonNode.Parse(second.GetRawText()));
    static bool Folded(JsonElement state)
    {
        var rows = state.GetProperty("rows");
        return rows.GetArrayLength() == 4 && Enumerable.Range(0, 3).All(i => rows[i].GetProperty("selected").GetBoolean())
            && !rows[3].GetProperty("selected").GetBoolean() && rows[0].GetProperty("display_row").GetInt32() == 0
            && rows[1].GetProperty("display_row").GetInt32() == -1 && rows[2].GetProperty("display_row").GetInt32() == -1
            && state.GetProperty("display_rows").GetInt32() == 2 && state.GetProperty("fold_groups").GetArrayLength() == 1
            && state.GetProperty("fold_groups")[0].GetProperty("collapsed").GetBoolean()
            && state.GetProperty("active_id").GetInt64() == rows[0].GetProperty("source_id").GetInt64();
    }
    static Point Pixel(JsonElement state, double x, double y)
    {
        var view = state.GetProperty("viewport");
        return new((int)Math.Round(view.GetProperty("x").GetDouble() + x * view.GetProperty("width").GetDouble() / view.GetProperty("script_width").GetDouble()),
            (int)Math.Round(view.GetProperty("y").GetDouble() + y * view.GetProperty("height").GetDouble() / view.GetProperty("script_height").GetDouble()));
    }
    void Case(string name, Action action)
    {
        phase = name;
        var before = checks;
        try
        {
            action();
            Require(checks > before, "Case performs concrete assertions");
            lock (results) results[name] = new("passed", checks - before, null);
        }
        catch (Exception error)
        {
            lock (results) results[name] = new("failed", checks - before, error.Message);
            throw;
        }
        WriteReports(null);
    }
    void WriteReports(string? error)
    {
        lock (results)
        {
            if (error is not null && results[phase].Status != "failed")
                results[phase] = new("failed", results[phase].Checks, error);
            Write("result.json", new { label = args[3], passed = error is null && results.Values.All(item => item.Status == "passed"),
                error, cases = results, native_inputs = inputSequence, boundary_commands = boundarySequence,
                assertion_channel_probe_passed = results["AssertionChannelProbe"].Status == "passed",
                expected_probe_marker = AssertionProbeMarker, ordinary_boundary_expected_assertions = 0,
                performance_acceptance = false, physical_input = false });
            var suite = new XElement("testsuite", new XAttribute("name", "WxBoundaryNative"), new XAttribute("tests", results.Count),
                new XAttribute("failures", results.Values.Count(item => item.Status == "failed")), new XAttribute("skipped", results.Values.Count(item => item.Status == "not-run")));
            foreach (var (name, result) in results)
            {
                var item = new XElement("testcase", new XAttribute("name", name), new XAttribute("classname", "WxBoundaryNative"));
                if (result.Status == "failed") item.Add(new XElement("failure", result.Error));
                else if (result.Status == "not-run") item.Add(new XElement("skipped", new XAttribute("message", "not-run after earlier failure")));
                suite.Add(item);
            }
            new XDocument(new XElement("testsuites", suite)).Save(Path.Combine(artifacts, "junit.xml"));
        }
    }
    void ValidateReports()
    {
        using var document = JsonDocument.Parse(File.ReadAllText(Path.Combine(artifacts, "result.json")));
        var cases = document.RootElement.GetProperty("cases");
        var junit = XDocument.Load(Path.Combine(artifacts, "junit.xml"));
        Require(cases.EnumerateObject().Select(item => item.Name).SequenceEqual(results.Keys)
            && junit.Descendants("testcase").Select(item => (string?)item.Attribute("name")).SequenceEqual(results.Keys),
            "JSON and JUnit account for every declared case without omission");
        foreach (var (name, result) in results)
        {
            Require(cases.GetProperty(name).GetProperty("Status").GetString() == result.Status
                && cases.GetProperty(name).GetProperty("Checks").GetInt32() == result.Checks,
                "Serialized case status and check count match observed execution");
            var item = junit.Descendants("testcase").Single(value => (string?)value.Attribute("name") == name);
            Require((item.Element("failure") is not null) == (result.Status == "failed")
                && (item.Element("skipped") is not null) == (result.Status == "not-run")
                && (result.Status != "passed" || result.Checks > 0), "JUnit retains each failure, not-run case and meaningful pass");
        }
    }
    sealed record CaseResult(string Status, int Checks, string? Error);
    sealed record Gesture(string Original, JsonElement Baseline, JsonElement Preview, Point Start, Point Middle);
    readonly record struct Point(int X, int Y);
}

sealed class DriverThreadDpiScope : IDisposable
{
    static readonly nint PerMonitorV2 = -4;
    readonly nint previous;
    readonly string journal;
    readonly int ownerThread = Environment.CurrentManagedThreadId;
    bool disposed;

    public DriverThreadDpiScope(string artifacts)
    {
        journal = Path.Combine(artifacts, "dpi-awareness.ndjson");
        previous = SetThreadDpiAwarenessContext(PerMonitorV2);
        if (previous == 0) throw new Win32Exception(Marshal.GetLastPInvokeError(), "SetThreadDpiAwarenessContext PMv2");
    }

    public void RecordActivation()
    {
        Record("before", previous);
        var active = GetThreadDpiAwarenessContext();
        Record("active", active);
        if (AreDpiAwarenessContextsEqual(active, PerMonitorV2) == 0) throw new InvalidOperationException("Driver thread did not enter per-monitor-v2 DPI awareness");
    }

    void Record(string stage, nint context)
    {
        var awareness = GetAwarenessFromDpiAwarenessContext(context);
        if (awareness < 0) throw new InvalidOperationException("Invalid driver thread DPI awareness context");
        File.AppendAllText(journal, JsonSerializer.Serialize(new
        {
            stage, pid = Environment.ProcessId, managed_thread_id = Environment.CurrentManagedThreadId, context = $"0x{context:X}", awareness,
            per_monitor_v2 = AreDpiAwarenessContextsEqual(context, PerMonitorV2) != 0,
            matches_previous = AreDpiAwarenessContextsEqual(context, previous) != 0
        }) + Environment.NewLine);
    }

    public void Dispose()
    {
        if (disposed) return;
        if (Environment.CurrentManagedThreadId != ownerThread) throw new InvalidOperationException("DPI awareness lease must be restored on its owning driver thread");
        if (SetThreadDpiAwarenessContext(previous) == 0) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Restore driver thread DPI awareness");
        disposed = true;
        var restored = GetThreadDpiAwarenessContext();
        Record("restored", restored);
        if (AreDpiAwarenessContextsEqual(restored, previous) == 0) throw new InvalidOperationException("Driver thread DPI awareness restoration did not match its prior context");
    }

    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)] static extern nint SetThreadDpiAwarenessContext(nint context);
    [DllImport("user32.dll", ExactSpelling = true)] static extern nint GetThreadDpiAwarenessContext();
    [DllImport("user32.dll", ExactSpelling = true)] static extern int GetAwarenessFromDpiAwarenessContext(nint context);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int AreDpiAwarenessContextsEqual(nint first, nint second);
}

static class WxNative
{
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] struct Point { public int X, Y; }
    [return: MarshalAs(UnmanagedType.Bool)] delegate bool EnumWindow(nint window, nint parameter);
    [DllImport("user32.dll", ExactSpelling = true)] [return: MarshalAs(UnmanagedType.Bool)] static extern bool EnumChildWindows(nint parent, EnumWindow callback, nint parameter);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int IsWindowVisible(nint window);
    [DllImport("user32.dll", ExactSpelling = true)] static extern nint GetParent(nint window);
    [DllImport("user32.dll", ExactSpelling = true)] public static extern int GetClientRect(nint window, out Rect rect);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int ClientToScreen(nint window, ref Point point);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int ScreenToClient(nint window, ref Point point);
    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)] public static extern int SetWindowPos(nint window, nint after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", ExactSpelling = true, SetLastError = true)] static extern nint SendMessageTimeout(nint window, uint message, nint wp, nint lp, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] public static extern bool PostMessageW(nint window, uint message, nint wp, nint lp);
    [DllImport("user32.dll", ExactSpelling = true)] static extern nint GetMenu(nint window);
    [DllImport("user32.dll", ExactSpelling = true)] static extern nint GetSubMenu(nint menu, int position);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int GetMenuItemCount(nint menu);
    [DllImport("user32.dll", ExactSpelling = true)] static extern uint GetMenuState(nint menu, uint position, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetMenuStringW", ExactSpelling = true, CharSet = CharSet.Unicode)] static extern int GetMenuString(nint menu, uint position, [Out] char[] text, int length, uint flags);
    public static bool IsVisible(nint window) => IsWindowVisible(window) != 0;
    public static nint[] Children(nint parent)
    {
        List<nint> list = [];
        EnumWindow callback = (window, _) => { list.Add(window); return true; };
        if (!EnumChildWindows(parent, callback, 0)) throw new InvalidOperationException("EnumChildWindows");
        GC.KeepAlive(callback);
        return list.ToArray();
    }
    public static nint Send(nint window, uint message, nint wp, nint lp, uint timeout)
    {
        if (SendMessageTimeout(window, message, wp, lp, 0x22, timeout, out var result) == 0)
            throw new Win32Exception(Marshal.GetLastPInvokeError(), "Owned native message " + message.ToString("X"));
        return result;
    }
    public static void VerifyPoint(nint canvas, int x, int y)
    {
        if (GetClientRect(canvas, out var area) == 0 || x < 0 || y < 0 || x >= area.Right || y >= area.Bottom)
            throw new InvalidOperationException("Input is outside actual canvas client");
        var screen = new Point { X = x, Y = y };
        if (ClientToScreen(canvas, ref screen) == 0) throw new InvalidOperationException("ClientToScreen");
        for (var parent = GetParent(canvas); parent != 0; parent = GetParent(parent))
        {
            var point = screen;
            if (ScreenToClient(parent, ref point) == 0 || GetClientRect(parent, out var bounds) == 0
                || point.X < 0 || point.Y < 0 || point.X >= bounds.Right || point.Y >= bounds.Bottom)
                throw new InvalidOperationException("Input is clipped by its actual native ancestor");
        }
    }
    static string Label(nint menu, int index)
    {
        var text = new char[512];
        var length = GetMenuString(menu, (uint)index, text, text.Length, 0x400);
        if (length >= text.Length - 1) throw new InvalidOperationException("Native menu label truncated");
        return new string(text, 0, length).Split('\t')[0].Replace("&", "", StringComparison.Ordinal);
    }
    static nint Submenu(nint window, string name)
    {
        var menu = GetMenu(window);
        for (var i = 0; i < GetMenuItemCount(menu); ++i)
            if (Label(menu, i).Equals(name, StringComparison.OrdinalIgnoreCase)) return GetSubMenu(menu, i);
        throw new InvalidOperationException("Native menu absent: " + name);
    }
    public static string[] MenuLabels(nint window, string name)
    {
        var menu = Submenu(window, name);
        return Enumerable.Range(0, GetMenuItemCount(menu)).Select(index => Label(menu, index)).ToArray();
    }
    public static bool MenuChecked(nint window, string menuName, string label)
    {
        var menu = Submenu(window, menuName);
        for (var i = 0; i < GetMenuItemCount(menu); ++i)
        {
            if (!Label(menu, i).Equals(label, StringComparison.OrdinalIgnoreCase)) continue;
            var state = GetMenuState(menu, (uint)i, 0x400);
            if (state == uint.MaxValue) throw new InvalidOperationException("Native menu state query failed");
            return (state & 8) != 0;
        }
        throw new InvalidOperationException("Native menu item absent: " + label);
    }
}
