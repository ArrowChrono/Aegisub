#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:property AllowUnsafeBlocks=true
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
using Microsoft.Win32.SafeHandles;

if (args.Length is < 3 or > 5 || args.Skip(3).Any(option => option is not ("--inspect" or "--real-input"))
    || args.Skip(3).Distinct().Count() != args.Length - 3)
{
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/shear-static-uia.cs -- EXE VIDEO ARTIFACTS [--inspect] [--real-input]");
    return 2;
}
try
{
    if (!args.Skip(3).Contains("--real-input"))
        return PrivateDesktop.Run(args, Path.GetFullPath(args[2]), isolation => new Scenario(args, isolation, null).Run());
    using var restoration = new InputDesktopRestoration(Path.GetFullPath(args[2]));
    try
    {
        return PrivateDesktop.Run(args, Path.GetFullPath(args[2]), isolation => new Scenario(args, isolation, restoration).Run());
    }
    finally
    {
        restoration.Restore("launcher-or-driver-finally");
        restoration.CompleteLauncherEvidence();
    }
}
catch (Exception error)
{
    Console.Error.WriteLine(error);
    return 1;
}

sealed class Scenario
{
    const string ShearTooltip = "Shear subtitles on X and Y axes";
    const string ShearMigration = "visual_tool_shear";
    const string ShearBindings = """
        { "video/tool/nudge/left": ["Left"], "video/tool/nudge/right": ["Right"],
          "video/tool/nudge/up": ["Up"], "video/tool/nudge/down": ["Down"],
          "video/tool/nudge/left/large": ["Alt-Left"], "video/tool/nudge/right/large": ["Alt-Right"],
          "video/tool/nudge/up/large": ["Alt-Up"], "video/tool/nudge/down/large": ["Alt-Down"],
          "video/tool/shear/reset/x": ["Ctrl-Home"], "video/tool/shear/reset/y": ["Ctrl-End"] }
        """;
    const string Source = """
        [Script Info]
        Title: Static Shear E2E
        ScriptType: v4.00+
        PlayResX: 640
        PlayResY: 480
        LayoutResX: 640
        LayoutResY: 480
        ScaledBorderAndShadow: yes
        YCbCr Matrix: TV.601
        [V4+ Styles]
        Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
        Style: Default,Arial,30,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,7,10,10,10,1
        Style: Sentinel,Arial,23,&H0066DDFF,&H000000FF,&H00000000,&H00000000,0,-1,0,0,110,95,1.2,0,1,1,0,2,15,20,25,1
        [Events]
        Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
        Dialogue: 3,0:00:00.00,0:00:10.00,Default,primary,12,23,34,,{\an7\pos(320,280)\org(298,260)\fscx125\fscy85\frz17\frx12\fry-8\fax0.25\fay-0.1\clip(120,90,530,420)\bord2.5\shad1.25\1c&H74B2E8&\3c&H003344&\alpha&H10&}Shear isolated\NSecond visible line {\i1\2c&H80E8B2&}preserved
        Dialogue: 1,0:00:20.00,0:00:25.00,Sentinel,unselected,31,42,53,,{\pos(420,400)\frz-7\fax-0.3\fay0.2\bord1.5\1c&H80E8B2&}Unselected sentinel\NMust remain unchanged
        """;

    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };
    static readonly Operation[] Operations =
    [
        new("arrow-right", "fax", 0.01, 0x27),
        new("arrow-left", "fax", -0.01, 0x25),
        new("arrow-down", "fay", 0.01, 0x28),
        new("arrow-up", "fay", -0.01, 0x26),
        new("alt-arrow-right", "fax", 0.1, 0x27, 0x12),
        new("alt-arrow-left", "fax", -0.1, 0x25, 0x12),
        new("alt-arrow-down", "fay", 0.1, 0x28, 0x12),
        new("alt-arrow-up", "fay", -0.1, 0x26, 0x12),
        new("reset-x", "fax", -0.25, 0x24, 0x11),
        new("reset-y", "fay", 0.1, 0x23, 0x11),
        new("drag-x", "fax", 0.4, 0, 0, new(340, 180), new(372, 202)),
        new("drag-y", "fay", 0.3, 0, 0, new(420, 272), new(443, 296))
    ];

    readonly string exe, video, artifacts, profile;
    readonly PrivateDesktop isolation;
    readonly InputDesktopRestoration? restoration;
    readonly bool inspect;
    readonly List<Check> checks = [];
    readonly List<CaseEvidence> cases = [];
    readonly List<string> expectedCases = [];
    readonly List<ConfigurationEvidence> configurationEvidence = [];
    static readonly string[] ExpectedConfigurations = ["legacy-hotkeys-install", "legacy-hotkeys-restart", "existing-shear-custom-preserved", "existing-shear-unbound-preserved"];
    JsonNode? customVideo;
    Process? app;
    nint main, canvas;
    int launch;
    string fixture = "";
    string scenarioName = "";
    string source = "";

    public Scenario(string[] args, PrivateDesktop isolation, InputDesktopRestoration? restoration)
    {
        exe = Path.GetFullPath(args[0]);
        video = Path.GetFullPath(args[1]);
        artifacts = Path.GetFullPath(args[2]);
        profile = Path.Combine(artifacts, "profile");
        this.isolation = isolation;
        this.restoration = restoration;
        inspect = args.Skip(3).Contains("--inspect");
        foreach (var name in new[] { "static-text", "reject-animated-fax", "reject-animated-fay" })
            expectedCases.AddRange(Operations.Select(operation => name + "-" + operation.Name));
        expectedCases.AddRange(Operations.Where(operation => operation.Key == 0).Select(operation => "static-drawing-" + operation.Name));
        expectedCases.Add("static-text-reset-nudge-undo-boundary");
    }

    public int Run()
    {
        Native.SetThreadDpiAwarenessContext(-4);
        Directory.CreateDirectory(artifacts);
        isolation.RecordDriver();
        using var output = new StreamWriter(Path.Combine(artifacts, "driver.log")) { AutoFlush = true };
        Console.SetOut(output);
        Console.SetError(output);
        using var watchdog = new Timer(_ =>
        {
            try { restoration?.Restore("watchdog"); }
            catch (Exception error) { Console.Error.WriteLine(error); }
            finally { if (app is not null) isolation.Stop(app); }
            Environment.Exit(124);
        }, null, 180_000, Timeout.Infinite);
        try
        {
            Require("fixture-files", File.Exists(exe) && File.Exists(video), "Executable and video fixture exist");
            Profile();
            Start("static-text", Source);
            VerifyToolbarOrder();
            if (inspect)
            {
                Dump("tool-probe-uia");
                var capture = Capture("tool-probe.png");
                Close();
                File.WriteAllText(Path.Combine(artifacts, "probe.json"), JsonSerializer.Serialize(new
                {
                    passed = true,
                    scope = "tool-selection-only",
                    checks,
                    screenshot = "tool-probe.png",
                    capture,
                    input_method = restoration is null ? "command-dispatch" : "SendInput-on-private-input-desktop",
                    physical_keyboard_verified = false,
                    screenshot_method = restoration is null ? "PrintWindow" : "screen-copy-on-private-input-desktop",
                    center = new Point(320, 280),
                    fax_handle = new Point(340, 180),
                    fay_handle = new Point(420, 272)
                }, Json));
                return 0;
            }
            var firstLoaded = Configuration("legacy-hotkeys-install");
            Require("legacy-hotkeys-installed", JsonNode.DeepEquals(firstLoaded.Shear, JsonNode.Parse(ShearBindings)), "An existing hotkey file without the Shear context gains all ten default bindings");
            Require("legacy-hotkeys-video-custom-preserved", JsonNode.DeepEquals(firstLoaded.Video, customVideo), "Migration preserves the complete customized Video context");
            RunStatic(Operations);
            RunResetNudgeBoundary();
            Close();
            var installed = Configuration("legacy-hotkeys-install");
            Require("legacy-hotkeys-marker-persisted", installed.Markers.Count(marker => marker == ShearMigration) == 1, "The independent Shear migration marker is persisted exactly once");
            var drawing = Source.Replace(
                @"Shear isolated\NSecond visible line {\i1\2c&H80E8B2&}preserved",
                @"{\p1}m 0 0 l 104 0 104 46 64 46 b 48 70 20 70 0 46 l 0 0{\p0}Drawing caption",
                StringComparison.Ordinal);
            Start("static-drawing", drawing);
            RunStatic(Operations.Where(operation => operation.Key == 0));
            Close();
            var restarted = Configuration("legacy-hotkeys-restart");
            Require("legacy-hotkeys-restart-idempotent", JsonNode.DeepEquals(installed.Shear, restarted.Shear)
                && JsonNode.DeepEquals(installed.Video, restarted.Video) && installed.Markers.SequenceEqual(restarted.Markers), "A second startup preserves installed bindings, Video customization and the complete migration list");
            Start("reject-animated-fax", Source.Replace(@"\alpha&H10&}", @"\alpha&H10&\t(0,700,1,\fax0.5\1c&HFF8020&)}", StringComparison.Ordinal));
            RunRejected();
            Close();
            Start("reject-animated-fay", Source.Replace(@"\alpha&H10&}", @"\alpha&H10&\t(50,950,0.7,\fay0.25\bord4.5)}", StringComparison.Ordinal));
            RunRejected();
            Close();
            VerifyExistingCustomization();
            var reportPath = Path.Combine(artifacts, "result.json");
            File.WriteAllText(reportPath, JsonSerializer.Serialize(new
            {
                passed = true,
                timeout_seconds = 180,
                expected_case_count = expectedCases.Count,
                expected_case_names = expectedCases,
                completed_case_count = cases.Count,
                screenshot_method = restoration is null ? "PrintWindow" : "screen-copy-on-private-input-desktop",
                input_method = restoration is null ? "command-dispatch" : "SendInput-on-private-input-desktop",
                physical_keyboard_verified = restoration is not null,
                expected_configuration_count = ExpectedConfigurations.Length,
                configuration_evidence = configurationEvidence,
                checks,
                cases
            }, Json));
            ValidateReport(reportPath);
            return 0;
        }
        catch (Exception error)
        {
            try
            {
                if (main != 0 && app is { HasExited: false })
                {
                    Capture("failure.png");
                    Dump("failure-uia");
                }
            }
            catch (Exception captureError) { Console.Error.WriteLine(captureError); }
            File.WriteAllText(Path.Combine(artifacts, "result.json"), JsonSerializer.Serialize(new
            {
                passed = false,
                expected_case_count = expectedCases.Count,
                expected_case_names = expectedCases,
                completed_case_count = cases.Count,
                input_method = restoration is null ? "command-dispatch" : "SendInput-on-private-input-desktop",
                physical_keyboard_verified = false,
                expected_configuration_count = ExpectedConfigurations.Length,
                configuration_evidence = configurationEvidence,
                checks,
                cases,
                error = error.Message
            }, Json));
            Console.Error.WriteLine(error);
            return 1;
        }
        finally
        {
            restoration?.Restore("scenario-finally");
            if (app is not null)
            {
                isolation.Stop(app);
                app.Dispose();
            }
        }
    }

    void Profile()
    {
        Directory.CreateDirectory(Path.Combine(profile, "user"));
        var config = JsonNode.Parse("""
            { "App": { "Auto": { "Check For Updates": false, "Load Linked Files": 0 },
                       "Hotkey Migrations": [ {"string":"cj"}, {"string":"7035"}, {"string":"7070"},
                           {"string":"edit/line/duplicate/shift_back"}, {"string":"grid/selection/history"},
                           {"string":"lua/code-edit-box/open-workspace"}, {"string":"visual_tool_nudge"},
                           {"string":"visual_tool_nudge_alt_large"}, {"string":"duplicate -> split"} ] },
              "Subtitle": { "Provider": "libass" },
              "Video": { "Open Audio": false, "Scale with DPI": false, "Default Zoom": 7,
                         "Renderer": { "Backend": "opengl" }, "Skia Tools": { "Enabled": false } },
              "Tool": { "Visual": { "Autohide": false } } }
            """)!;
        File.WriteAllText(Path.Combine(profile, "user", "config.json"), config.ToJsonString(Json));
        File.WriteAllText(Path.Combine(artifacts, "legacy-config-input.json"), config.ToJsonString(Json));
        var menu = JsonNode.Parse(File.ReadAllText(Path.Combine("src", "libresrc", "default_menu.json")))!;
        menu["main"]!.AsArray().Add(new JsonObject { ["submenu"] = "shear-e2e", ["text"] = "Shear E2E" });
        var commands = new JsonArray();
        foreach (var (command, label) in new[] { ("subtitle/save", "Save"), ("edit/undo", "Undo"), ("edit/redo", "Redo") })
            commands.Add(new JsonObject { ["command"] = command, ["text"] = label });
        foreach (var operation in Operations.Where(operation => operation.Key != 0))
            commands.Add(new JsonObject { ["command"] = InputCommand(operation), ["text"] = "Input " + operation.Name });
        menu["shear-e2e"] = commands;
        File.WriteAllText(Path.Combine(profile, "user", "menu.json"), menu.ToJsonString(Json));
        var legacy = JsonNode.Parse(File.ReadAllText(Path.Combine("src", "libresrc", "default_hotkey.json")))!;
        legacy.AsObject().Remove("Visual Shear");
        var videoKeys = legacy["Video"]!.AsObject();
        videoKeys["video/frame/next"] = new JsonArray("Alt-Right", "Ctrl-Alt-Right");
        videoKeys["video/zoom/in"] = new JsonArray("Ctrl-Alt-Up", "Up");
        customVideo = videoKeys.DeepClone();
        File.WriteAllText(Path.Combine(profile, "user", "hotkey.json"), legacy.ToJsonString(Json));
        File.WriteAllText(Path.Combine(artifacts, "legacy-hotkey-input.json"), legacy.ToJsonString(Json));
        File.WriteAllText(Path.Combine(artifacts, "legacy-hotkey-fixture.json"), JsonSerializer.Serialize(new
        {
            source = "src/libresrc/default_hotkey.json",
            removed_context = "Visual Shear",
            repository_legacy_revision = "4d41f1d56",
            completed_prior_migrations = MigrationMarkers(config),
            customized_video_commands = new[] { "video/frame/next", "video/zoom/in" },
            physical_keyboard_verified = false
        }, Json));
    }

    ConfigurationEvidence Configuration(string name)
    {
        var hotkeyFile = Path.Combine(profile, "user", "hotkey.json");
        var configFile = Path.Combine(profile, "user", "config.json");
        var hotkeys = JsonNode.Parse(File.ReadAllText(hotkeyFile))!;
        var config = JsonNode.Parse(File.ReadAllText(configFile))!;
        var hotkeyExport = name + "-hotkey.json";
        var configExport = name + "-config.json";
        File.Copy(hotkeyFile, Path.Combine(artifacts, hotkeyExport), true);
        File.Copy(configFile, Path.Combine(artifacts, configExport), true);
        var evidence = new ConfigurationEvidence(name, hotkeyExport, configExport, hotkeys.AsObject().ContainsKey("Visual Shear"),
            hotkeys["Visual Shear"]?.DeepClone(), hotkeys["Video"]?.DeepClone(), MigrationMarkers(config));
        configurationEvidence.RemoveAll(item => item.Name == name);
        configurationEvidence.Add(evidence);
        return evidence;
    }

    void VerifyExistingCustomization()
    {
        var hotkeyFile = Path.Combine(profile, "user", "hotkey.json");
        var configFile = Path.Combine(profile, "user", "config.json");
        var hotkeys = JsonNode.Parse(File.ReadAllText(hotkeyFile))!;
        var customized = JsonNode.Parse("""{ "video/tool/nudge/left/large": ["Shift-Left"] }""")!;
        hotkeys["Visual Shear"] = customized.DeepClone();
        File.WriteAllText(hotkeyFile, hotkeys.ToJsonString(Json));
        var config = JsonNode.Parse(File.ReadAllText(configFile))!;
        var markers = MigrationMarkers(config).Where(marker => marker != ShearMigration && marker != "visual_tool_nudge_alt_large").ToArray();
        Require("custom-shear-old-nudge-marker-present", markers.Contains("visual_tool_nudge"), "The custom fixture retains the earlier nudge migration marker");
        config["App"]!["Hotkey Migrations"] = new JsonArray(markers.Select(marker => (JsonNode)new JsonObject { ["string"] = marker }).ToArray());
        File.WriteAllText(configFile, config.ToJsonString(Json));
        File.Copy(hotkeyFile, Path.Combine(artifacts, "custom-shear-hotkey-input.json"), false);
        File.Copy(configFile, Path.Combine(artifacts, "custom-shear-config-input.json"), false);
        Start("custom-shear-profile", Source);
        Close();
        var preserved = Configuration("existing-shear-custom-preserved");
        Require("existing-shear-custom-layout-preserved", JsonNode.DeepEquals(preserved.Shear, customized), "A pre-existing one-key Shear layout retains Shift-Left and gains no missing defaults");
        Require("existing-shear-custom-video-preserved", JsonNode.DeepEquals(preserved.Video, customVideo), "Customized Video bindings remain exact after the custom-layout startup");
        Require("existing-shear-custom-markers-persisted", preserved.Markers.Count(marker => marker == ShearMigration) == 1
            && preserved.Markers.Count(marker => marker == "visual_tool_nudge_alt_large") == 1, "Shear and large-nudge migrations are recorded exactly once without rewriting the Shear key");
        hotkeys = JsonNode.Parse(File.ReadAllText(hotkeyFile))!;
        hotkeys.AsObject().Remove("Visual Shear");
        File.WriteAllText(hotkeyFile, hotkeys.ToJsonString(Json));
        File.Copy(hotkeyFile, Path.Combine(artifacts, "unbound-shear-hotkey-input.json"), false);
        Start("unbound-shear-profile", Source);
        Close();
        var unbound = Configuration("existing-shear-unbound-preserved");
        Require("existing-shear-unbound-preserved", !unbound.HasShearContext, "Removing every Shear binding after migration remains an intentional unbinding on startup");
        Require("existing-shear-unbound-video-preserved", JsonNode.DeepEquals(unbound.Video, customVideo), "Unbinding Shear does not alter the complete custom Video context");
        Require("existing-shear-unbound-markers-stable", unbound.Markers.SequenceEqual(preserved.Markers), "Completed migration markers remain unchanged and are not duplicated");
    }

    static string[] MigrationMarkers(JsonNode config) => config["App"]?["Hotkey Migrations"]?.AsArray()
        .Select(item => item is JsonValue value ? value.GetValue<string>() : item!["string"]!.GetValue<string>()).ToArray() ?? [];

    void Start(string name, string input)
    {
        scenarioName = name;
        source = input;
        fixture = Path.Combine(artifacts, name + "-scenario.ass");
        File.WriteAllText(fixture, input, new UTF8Encoding(false));
        File.WriteAllText(Path.Combine(artifacts, name + "-input.ass"), input, new UTF8Encoding(false));
        var host = Path.Combine(artifacts, "host-" + ++launch);
        Directory.CreateDirectory(host);
        var start = new ProcessStartInfo(exe) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        start.Environment["AEGISUB_ENABLE_SKIA_VIDEO_TOOLS"] = "0";
        foreach (var arg in new[] { "--gui-test", "host", "--profile-dir", profile, "--artifacts", host, "--open", fixture, "--open", video })
            start.ArgumentList.Add(arg);
        app = isolation.Start(start, "host-" + launch);
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(host, "ready.json"), app, TimeSpan.FromSeconds(20));
        main = isolation.MainWindow(app, 20);
        isolation.VerifyWindow(main, app);
        Native.ShowWindow(main, 4);
        Native.Check(Native.SetWindowPos(main, 0, 20, 20, 1400, 950, 0x0050), "SetWindowPos");
        Wait(() =>
        {
            canvas = Elements(main).Select(element => new nint(element.Current.NativeWindowHandle))
                .FirstOrDefault(handle => handle != 0 && PrivateDesktop.ClassName(handle) == "wxGLCanvas" && Native.IsWindowVisible(handle) != 0);
            return canvas != 0 && Native.GetClientRect(canvas, out var rect) != 0 && rect.Right == 640 && rect.Bottom == 480 && Native.IsWindowEnabled(main) != 0;
        }, "640x480 video canvas at 100% zoom");
        Wait(() => Elements(main).Any(element => element.Current.ControlType == ControlType.Button
            && element.Current.Name.Contains(ShearTooltip, StringComparison.OrdinalIgnoreCase) && element.Current.IsEnabled), "Shear tool button enabled");
        ClickToolbar(Toolbar(ShearTooltip));
        Require(name + "-tool-selected", ToolbarChecked(Toolbar(ShearTooltip)), "Shear main tool is selected");
        var initial = Save(name + "-initial");
        Require(name + "-initial-events", initial.Dialogues.SequenceEqual(Lines(source, "Dialogue:")), "Loaded and saved events match the complete input events");
        Require(name + "-initial-styles", initial.Styles.SequenceEqual(Lines(source, "Style:")), "Loaded and saved styles match both input styles");
    }

    void VerifyToolbarOrder()
    {
        var tools = new[]
        {
            Toolbar("Clip subtitles to a rectangle"),
            Toolbar("Clip subtitles to a vectorial area"),
            Toolbar(ShearTooltip),
            Toolbar("Measure with temporary guides")
        };
        var indices = tools.Select(tool => checked((int)Native.Send(tool.Toolbar, 0x0419, (nuint)tool.Id, 0))).ToArray();
        Require("main-tool-order", tools.All(tool => tool.Toolbar == tools[0].Toolbar)
            && indices[0] >= 0 && indices.SequenceEqual(Enumerable.Range(indices[0], 4)),
            "Observed main toolbar has consecutive clip, vector clip, shear, measure buttons");
        File.WriteAllText(Path.Combine(artifacts, "main-toolbar.json"), JsonSerializer.Serialize(tools.Select((tool, index) => new
        {
            name = tool.Element.Current.Name,
            command_id = tool.Id,
            toolbar_index = indices[index],
            rectangle = tool.Element.Current.BoundingRectangle.ToString()
        }), Json));
    }

    void RunStatic(IEnumerable<Operation> operations)
    {
        foreach (var operation in operations)
        {
            var name = scenarioName + "-" + operation.Name;
            var before = Save(name + "-before");
            Dispatch(operation);
            Wait(Modified, name + " modified subtitle");
            var after = Save(name + "-after");
            var actualDelta = Tag(after.Dialogues[0], operation.Axis) - Tag(before.Dialogues[0], operation.Axis);
            Require(name + "-axis-value", Near(actualDelta, operation.ExpectedDelta), "Edited axis has the expected coefficient delta");
            Require(name + "-single-static-axis-tag", AxisTags(after.Dialogues[0], operation.Axis).Count == 1, "Edited axis has exactly one static shear tag");
            Require(name + "-single-axis-preservation", StripAxis(after.Dialogues[0], operation.Axis) == StripAxis(before.Dialogues[0], operation.Axis),
                "Removing only the edited tag leaves the complete active event unchanged");
            var other = operation.Axis == "fax" ? "fay" : "fax";
            Require(name + "-other-axis", Tag(after.Dialogues[0], other) == Tag(before.Dialogues[0], other), "The other shear axis is unchanged");
            Require(name + "-unselected-events", after.Dialogues.Skip(1).SequenceEqual(before.Dialogues.Skip(1)), "Every unselected event is exactly unchanged");
            Require(name + "-styles", after.Styles.SequenceEqual(before.Styles), "Both styles are exactly unchanged");
            Command("Undo");
            var undo = Save(name + "-undo");
            Require(name + "-undo", Same(undo, before), "One undo restores every event and style exactly");
            Command("Redo");
            var redo = Save(name + "-redo");
            Require(name + "-redo", Same(redo, after), "One redo restores the saved edited events and styles exactly");
            Command("Undo");
            var baseline = Save(name + "-restored");
            Require(name + "-baseline", Same(baseline, before), "The next independent operation starts from the same baseline");
            cases.Add(new(name, operation.Axis, true, operation.ExpectedDelta, actualDelta, before, after, undo, redo, baseline));
        }
    }

    void RunRejected()
    {
        foreach (var operation in Operations)
        {
            var name = scenarioName + "-" + operation.Name;
            var before = Save(name + "-before");
            Dispatch(operation);
            Require(name + "-clean-state", !Modified(), "Rejected operation does not mark subtitles modified");
            var after = Save(name + "-after");
            Require(name + "-complete-preservation", Same(after, before), "Animation rejection preserves every event and style exactly");
            var delta = Tag(after.Dialogues[0], operation.Axis) - Tag(before.Dialogues[0], operation.Axis);
            Require(name + "-axis-value", delta == 0, "Animated shear is unchanged");
            cases.Add(new(name, operation.Axis, false, 0, delta, before, after, null, null, null));
        }
    }

    void RunResetNudgeBoundary()
    {
        const string name = "static-text-reset-nudge-undo-boundary";
        var before = Save(name + "-before");
        Dispatch(Operations.Single(operation => operation.Name == "reset-x"));
        Wait(Modified, name + " reset modified subtitle");
        var reset = Save(name + "-reset");
        Require(name + "-reset-value", Tag(reset.Dialogues[0], "fax") == 0, "Reset produces zero fax before the following nudge");
        Require(name + "-reset-preservation", StripAxis(reset.Dialogues[0], "fax") == StripAxis(before.Dialogues[0], "fax")
            && reset.Dialogues.Skip(1).SequenceEqual(before.Dialogues.Skip(1)) && reset.Styles.SequenceEqual(before.Styles),
            "Reset preserves the complete other-axis content, every unselected event and both styles");
        var resetSaved = Stopwatch.GetTimestamp();
        Wait(() => Stopwatch.GetElapsedTime(resetSaved).TotalMilliseconds >= 600, "reset boundary after more than 500 milliseconds");
        Dispatch(Operations.Single(operation => operation.Name == "arrow-right"));
        Wait(Modified, name + " nudge modified subtitle");
        var after = Save(name + "-after");
        Require(name + "-nudge-value", Near(Tag(after.Dialogues[0], "fax"), 0.01), "Right nudge starts from the reset zero value");
        Require(name + "-nudge-preservation", StripAxis(after.Dialogues[0], "fax") == StripAxis(reset.Dialogues[0], "fax")
            && after.Dialogues.Skip(1).SequenceEqual(reset.Dialogues.Skip(1)) && after.Styles.SequenceEqual(reset.Styles),
            "Following nudge preserves every other field of the reset snapshot");
        Command("Undo");
        var undo = Save(name + "-undo-nudge");
        Require(name + "-undo-nudge-only", Same(undo, reset) && Tag(undo.Dialogues[0], "fax") == 0,
            "The first undo removes only the nudge and retains the earlier reset");
        Command("Undo");
        var undoReset = Save(name + "-undo-reset");
        Require(name + "-undo-reset", Same(undoReset, before), "The second undo restores the complete original baseline");
        Command("Redo");
        var redoReset = Save(name + "-redo-reset");
        Require(name + "-redo-reset", Same(redoReset, reset), "The first redo restores the reset as a separate history entry");
        Command("Redo");
        var redo = Save(name + "-redo-nudge");
        Require(name + "-redo-nudge", Same(redo, after), "The second redo restores the following nudge as a separate entry");
        Command("Undo");
        Command("Undo");
        var restored = Save(name + "-restored");
        Require(name + "-restored", Same(restored, before), "Boundary case finishes on the original baseline");
        cases.Add(new(name, "fax", true, -0.24, Tag(after.Dialogues[0], "fax") - Tag(before.Dialogues[0], "fax"), before, after, undo, redo, restored, reset));
    }

    void Dispatch(Operation operation)
    {
        isolation.VerifyWindow(canvas, app!);
        if (operation.Key != 0)
        {
            if (restoration is null)
            {
                Command("Input " + operation.Name);
                File.AppendAllText(Path.Combine(artifacts, "command-input-evidence.ndjson"), JsonSerializer.Serialize(new
                {
                    scenario = scenarioName,
                    operation = operation.Name,
                    command_id = InputCommand(operation),
                    input_method = "command-dispatch",
                    physical_keyboard_verified = false
                }) + Environment.NewLine);
                return;
            }
            using (restoration.Activate(isolation.Name))
            {
                Native.Key(main, canvas, app!.Id, operation.Key, operation.Modifier, restoration,
                    evidence => File.AppendAllText(Path.Combine(artifacts, "keyboard-evidence.ndjson"), JsonSerializer.Serialize(new
                    {
                        scenario = scenarioName,
                        operation = operation.Name,
                        evidence
                    }) + Environment.NewLine));
            }
            isolation.VerifyWindow(canvas, app!);
            return;
        }
        var from = operation.From ?? throw new InvalidOperationException("Drag origin missing");
        var to = operation.To ?? throw new InvalidOperationException("Drag destination missing");
        Native.Mouse(canvas, 0x0200, from, 0);
        Native.Mouse(canvas, 0x0201, from, 1);
        Native.Mouse(canvas, 0x0200, new Point((from.X + to.X) / 2, (from.Y + to.Y) / 2), 1);
        Native.Mouse(canvas, 0x0200, to, 1);
        Native.Mouse(canvas, 0x0202, to, 0);
        Native.Send(canvas, 0, 0, 0);
    }

    Snapshot Save(string name)
    {
        Command("Save");
        Wait(() => !Modified(), name + " saved subtitle");
        var export = name + ".ass";
        File.Copy(fixture, Path.Combine(artifacts, export), false);
        var snapshot = new Snapshot(export, name + ".png", File.ReadLines(fixture).Where(line => line.StartsWith("Dialogue:", StringComparison.Ordinal)).ToArray(),
            File.ReadLines(fixture).Where(line => line.StartsWith("Style:", StringComparison.Ordinal)).ToArray(), Capture(name + ".png"));
        Require(name + "-save-events", snapshot.Dialogues.Length == 2, "Saved ASS contains both complete dialogue events");
        return snapshot;
    }

    WindowCaptureEvidence Capture(string name)
    {
        isolation.VerifyWindow(main, app!);
        if (restoration is null) return ScreenCapture.SaveWindowPng(main, Path.Combine(artifacts, name));
        using (restoration.Activate(isolation.Name))
        {
            Native.FocusOwned(main, canvas, app!.Id);
            Native.Send(canvas, 0x000F, 0, 0);
            Native.FlushDesktop();
            Native.Check(Native.GetWindowRect(main, out var rect), "GetWindowRect for real screen capture");
            using var bitmap = new System.Drawing.Bitmap(rect.Right - rect.Left, rect.Bottom - rect.Top);
            using var graphics = System.Drawing.Graphics.FromImage(bitmap);
            graphics.CopyFromScreen(rect.Left, rect.Top, 0, 0, bitmap.Size, System.Drawing.CopyPixelOperation.SourceCopy);
            bitmap.Save(Path.Combine(artifacts, name), System.Drawing.Imaging.ImageFormat.Png);
        }
        isolation.VerifyWindow(main, app!);
        Native.GetWindowRect(main, out var savedRect);
        return ScreenCapture.ValidateSavedPng(Path.Combine(artifacts, name), savedRect.Right - savedRect.Left, savedRect.Bottom - savedRect.Top);
    }

    void ValidateReport(string path)
    {
        using var report = JsonDocument.Parse(File.ReadAllText(path));
        var root = report.RootElement;
        if (!root.GetProperty("passed").GetBoolean() || root.GetProperty("expected_case_count").GetInt32() != expectedCases.Count
            || root.GetProperty("completed_case_count").GetInt32() != expectedCases.Count)
            throw new InvalidOperationException("Persisted report has incomplete case counts");
        var reportedCases = root.GetProperty("cases").EnumerateArray().ToArray();
        var names = reportedCases.Select(item => item.GetProperty("Name").GetString()).ToArray();
        if (names.Length != expectedCases.Count || names.Distinct().Count() != names.Length || !names.Order().SequenceEqual(expectedCases.Order()))
            throw new InvalidOperationException("Persisted report omits or duplicates a required behavior");
        var reportedChecks = root.GetProperty("checks").EnumerateArray().ToArray();
        if (reportedChecks.Length == 0 || reportedChecks.Any(check => !check.GetProperty("Passed").GetBoolean()))
            throw new InvalidOperationException("Persisted report has a missing or failed assertion");
        var configurations = root.GetProperty("configuration_evidence").EnumerateArray().ToArray();
        if (root.GetProperty("expected_configuration_count").GetInt32() != ExpectedConfigurations.Length
            || configurations.Length != ExpectedConfigurations.Length
            || !configurations.Select(item => item.GetProperty("Name").GetString()).Order().SequenceEqual(ExpectedConfigurations.Order()))
            throw new InvalidOperationException("Persisted report omits a required hotkey upgrade configuration");
        foreach (var item in configurations)
        {
            var hotkeys = JsonNode.Parse(File.ReadAllText(Path.Combine(artifacts, item.GetProperty("HotkeyExport").GetString()!)))!;
            var config = JsonNode.Parse(File.ReadAllText(Path.Combine(artifacts, item.GetProperty("ConfigExport").GetString()!)))!;
            if (hotkeys.AsObject().ContainsKey("Visual Shear") != item.GetProperty("HasShearContext").GetBoolean()
                || !JsonNode.DeepEquals(hotkeys["Visual Shear"], JsonNode.Parse(item.GetProperty("Shear").GetRawText()))
                || !JsonNode.DeepEquals(hotkeys["Video"], JsonNode.Parse(item.GetProperty("Video").GetRawText()))
                || !MigrationMarkers(config).SequenceEqual(item.GetProperty("Markers").EnumerateArray().Select(marker => marker.GetString())))
                throw new InvalidOperationException("Saved hotkey/configuration artifacts differ from the persisted upgrade report");
        }
        foreach (var item in reportedCases)
        {
            foreach (var field in new[] { "Before", "After", "Undo", "Redo", "Restored", "Reset" })
            {
                var snapshot = item.GetProperty(field);
                if (snapshot.ValueKind == JsonValueKind.Null)
                {
                    if (field is "Before" or "After" || (field != "Reset" && item.GetProperty("MutationExpected").GetBoolean())
                        || (field == "Reset" && item.GetProperty("Name").GetString() == "static-text-reset-nudge-undo-boundary"))
                        throw new InvalidOperationException("Persisted report omits required snapshot evidence");
                    continue;
                }
                var export = Path.Combine(artifacts, snapshot.GetProperty("Export").GetString()!);
                var dialogues = snapshot.GetProperty("Dialogues").EnumerateArray().Select(line => line.GetString()).ToArray();
                var styles = snapshot.GetProperty("Styles").EnumerateArray().Select(line => line.GetString()).ToArray();
                if (!File.ReadLines(export).Where(line => line.StartsWith("Dialogue:", StringComparison.Ordinal)).SequenceEqual(dialogues)
                    || !File.ReadLines(export).Where(line => line.StartsWith("Style:", StringComparison.Ordinal)).SequenceEqual(styles))
                    throw new InvalidOperationException("Saved ASS artifact differs from the persisted report");
                var capture = snapshot.GetProperty("Capture");
                ScreenCapture.ValidateSavedPng(Path.Combine(artifacts, snapshot.GetProperty("Screenshot").GetString()!),
                    capture.GetProperty("Width").GetInt32(), capture.GetProperty("Height").GetInt32());
            }
        }
        File.WriteAllText(Path.Combine(artifacts, "report-validation.json"), JsonSerializer.Serialize(new
        {
            passed = true,
            parsed_case_count = reportedCases.Length,
            parsed_check_count = reportedChecks.Length,
            parsed_configuration_count = configurations.Length,
            saved_ass_and_screenshots_verified = true
        }, Json));
    }

    bool Modified() => isolation.ReadText(main, app!).TrimStart().StartsWith("*", StringComparison.Ordinal);

    void Command(string label)
    {
        isolation.VerifyWindow(main, app!);
        var id = ObservedWindowsCommand.FindMenuCommand(main, app!.Id, "Shear E2E", label);
        Native.Send(main, 0x0111, (nuint)id, 0);
    }

    ToolButton Toolbar(string tooltip)
    {
        var element = Elements(main).SingleOrDefault(candidate => candidate.Current.ControlType == ControlType.Button
            && candidate.Current.Name.Contains(tooltip, StringComparison.OrdinalIgnoreCase))
            ?? throw new InvalidOperationException("Observed toolbar tooltip absent or ambiguous: " + tooltip);
        var match = Regex.Match(element.Current.AutomationId, @"(\d+)$");
        if (!match.Success) throw new InvalidOperationException("Observed toolbar button has no command ID");
        var id = int.Parse(match.Value, CultureInfo.InvariantCulture);
        for (var parent = TreeWalker.RawViewWalker.GetParent(element); parent is not null; parent = TreeWalker.RawViewWalker.GetParent(parent))
            if (parent.Current.ControlType == ControlType.ToolBar && parent.Current.NativeWindowHandle != 0)
                return new(element, new nint(parent.Current.NativeWindowHandle), id);
        throw new InvalidOperationException("Observed tool has no native toolbar ancestor");
    }

    bool ToolbarChecked(ToolButton tool) => (Native.Send(tool.Toolbar, 0x0412, (nuint)tool.Id, 0) & 1) != 0;

    void ClickToolbar(ToolButton tool)
    {
        isolation.VerifyWindow(tool.Toolbar, app!);
        var state = Native.Send(tool.Toolbar, 0x0412, (nuint)tool.Id, 0);
        if ((state & 4) == 0) throw new InvalidOperationException("Observed tool button is disabled");
        var rectangle = tool.Element.Current.BoundingRectangle;
        var point = new NativePoint { X = (int)(rectangle.Left + rectangle.Width / 2), Y = (int)(rectangle.Top + rectangle.Height / 2) };
        Native.Check(Native.ScreenToClient(tool.Toolbar, ref point), "ScreenToClient");
        var local = new Point(point.X, point.Y);
        Native.Mouse(tool.Toolbar, 0x0200, local, 0);
        Native.Mouse(tool.Toolbar, 0x0201, local, 1);
        Native.Mouse(tool.Toolbar, 0x0202, local, 0);
    }

    void Wait(Func<bool> predicate, string name)
    {
        var deadline = Stopwatch.GetTimestamp() + 15 * Stopwatch.Frequency;
        while (Stopwatch.GetTimestamp() < deadline)
        {
            if (app is { HasExited: true }) throw new InvalidOperationException("Host exited while waiting for " + name);
            isolation.VerifyCurrent();
            if (predicate()) return;
            Thread.Sleep(50);
        }
        throw new TimeoutException("Timed out waiting for " + name);
    }

    void Close()
    {
        isolation.VerifyWindow(main, app!);
        Native.Check(Native.PostMessage(main, 0x0010, 0, 0), "PostMessage close");
        if (!app!.WaitForExit(10_000)) throw new TimeoutException("Host failed to close within ten seconds");
        Require(scenarioName + "-clean-exit", isolation.ExitCode(app) == 0, "Host exits cleanly");
        app.Dispose();
        app = null;
        main = canvas = 0;
    }

    void Require(string name, bool passed, string description)
    {
        checks.Add(new(name, passed, description));
        Console.WriteLine(name + ": " + passed);
        if (!passed) throw new InvalidOperationException(name + ": " + description);
    }

    void Dump(string name) => File.WriteAllText(Path.Combine(artifacts, name + ".json"), JsonSerializer.Serialize(Elements(main).Select(element => new
    {
        name = element.Current.Name,
        type = element.Current.ControlType.ProgrammaticName,
        id = element.Current.AutomationId,
        hwnd = element.Current.NativeWindowHandle,
        rectangle = element.Current.BoundingRectangle.ToString(),
        value = UiaDriver.ReadElementState(element).Value
    }), Json));

    static AutomationElement[] Elements(nint handle) => AutomationElement.FromHandle(handle)
        .FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();

    static string[] Lines(string input, string prefix) => input.Split('\n').Select(line => line.TrimEnd('\r'))
        .Where(line => line.StartsWith(prefix, StringComparison.Ordinal)).ToArray();

    static string StripAxis(string text, string axis) => Regex.Replace(text, @"\\" + axis + @"[-+]?(?:\d+(?:\.\d*)?|\.\d+)", "");

    static double Tag(string text, string axis)
    {
        var matches = AxisTags(text, axis);
        if (matches.Count == 0) throw new InvalidOperationException("Required shear tag is missing: " + axis);
        return double.Parse(matches[0].Groups[1].Value, CultureInfo.InvariantCulture);
    }

    static bool Same(Snapshot left, Snapshot right) => left.Dialogues.SequenceEqual(right.Dialogues) && left.Styles.SequenceEqual(right.Styles);
    static MatchCollection AxisTags(string text, string axis) => Regex.Matches(text, @"\\" + axis + @"([-+]?(?:\d+(?:\.\d*)?|\.\d+))");
    static bool Near(double left, double right) => Math.Abs(left - right) < 0.000001;
    static string InputCommand(Operation operation) => operation.Name switch
    {
        "arrow-right" => "video/tool/nudge/right",
        "arrow-left" => "video/tool/nudge/left",
        "arrow-down" => "video/tool/nudge/down",
        "arrow-up" => "video/tool/nudge/up",
        "alt-arrow-right" => "video/tool/nudge/right/large",
        "alt-arrow-left" => "video/tool/nudge/left/large",
        "alt-arrow-down" => "video/tool/nudge/down/large",
        "alt-arrow-up" => "video/tool/nudge/up/large",
        "reset-x" => "video/tool/shear/reset/x",
        "reset-y" => "video/tool/shear/reset/y",
        _ => throw new InvalidOperationException("Operation has no keyboard-equivalent tool command")
    };
}

sealed record Point(int X, int Y);
sealed record Operation(string Name, string Axis, double ExpectedDelta, uint Key, uint Modifier = 0, Point? From = null, Point? To = null);
sealed record Check(string Name, bool Passed, string Description);
sealed record Snapshot(string Export, string Screenshot, string[] Dialogues, string[] Styles, WindowCaptureEvidence Capture);
sealed record CaseEvidence(string Name, string Axis, bool MutationExpected, double ExpectedDelta, double ActualDelta,
    Snapshot Before, Snapshot After, Snapshot? Undo, Snapshot? Redo, Snapshot? Restored, Snapshot? Reset = null);
sealed record ConfigurationEvidence(string Name, string HotkeyExport, string ConfigExport, bool HasShearContext,
    JsonNode? Shear, JsonNode? Video, string[] Markers);
sealed record ToolButton(AutomationElement Element, nint Toolbar, int Id);
[StructLayout(LayoutKind.Sequential)] struct NativePoint { public int X, Y; }
[StructLayout(LayoutKind.Sequential)] struct Rect { public int Left, Top, Right, Bottom; }
[StructLayout(LayoutKind.Sequential)] struct GuiThreadInfo
{
    public uint Size, Flags;
    public nint Active, Focus, Capture, MenuOwner, MoveSize, Caret;
    public Rect CaretRectangle;
}
[StructLayout(LayoutKind.Sequential)] struct KeyboardInput
{
    public ushort Key, Scan;
    public uint Flags, Time;
    public nint ExtraInfo;
}
[StructLayout(LayoutKind.Sequential)] struct MouseInput
{
    public int X, Y;
    public uint Data, Flags, Time;
    public nint ExtraInfo;
}
[StructLayout(LayoutKind.Explicit)] struct InputUnion
{
    [FieldOffset(0)] public KeyboardInput Keyboard;
    [FieldOffset(0)] public MouseInput Mouse;
}
[StructLayout(LayoutKind.Sequential)] struct Input { public uint Type; public InputUnion Union; }

sealed class DesktopHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public DesktopHandle() : base(true) { }
    protected override bool ReleaseHandle() => Native.CloseDesktop(handle) != 0;
}

sealed class InputDesktopRestoration : IDisposable
{
    readonly DesktopHandle original;
    readonly string originalName, artifacts;
    readonly NativePoint cursor;
    readonly nint foreground, focus;
    uint[] heldKeys = [];
    string? activeName;

    public InputDesktopRestoration(string artifacts)
    {
        this.artifacts = artifacts;
        original = Native.OpenInputDesktop(0, 0, 0x101);
        if (original.IsInvalid) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Open original input desktop failed");
        originalName = Native.ObjectName(original);
        if (Environment.GetEnvironmentVariable("AEGISUB_SHEAR_RESTORE_CURSOR_X") is { } x)
        {
            cursor = new() { X = int.Parse(x, CultureInfo.InvariantCulture), Y = int.Parse(Environment.GetEnvironmentVariable("AEGISUB_SHEAR_RESTORE_CURSOR_Y")!, CultureInfo.InvariantCulture) };
            foreground = nint.Parse(Environment.GetEnvironmentVariable("AEGISUB_SHEAR_RESTORE_FOREGROUND")!, CultureInfo.InvariantCulture);
            focus = nint.Parse(Environment.GetEnvironmentVariable("AEGISUB_SHEAR_RESTORE_FOCUS")!, CultureInfo.InvariantCulture);
        }
        else
        {
            Native.Check(Native.GetCursorPos(out cursor), "Capture original cursor");
            foreground = Native.GetForegroundWindow();
            var thread = foreground == 0 ? 0 : Native.WindowThread(foreground, out _);
            var info = new GuiThreadInfo { Size = (uint)Marshal.SizeOf<GuiThreadInfo>() };
            if (thread != 0) Native.Check(Native.GetGUIThreadInfo(thread, ref info), "Capture original focus");
            focus = info.Focus;
            Environment.SetEnvironmentVariable("AEGISUB_SHEAR_RESTORE_CURSOR_X", cursor.X.ToString(CultureInfo.InvariantCulture));
            Environment.SetEnvironmentVariable("AEGISUB_SHEAR_RESTORE_CURSOR_Y", cursor.Y.ToString(CultureInfo.InvariantCulture));
            Environment.SetEnvironmentVariable("AEGISUB_SHEAR_RESTORE_FOREGROUND", foreground.ToString(CultureInfo.InvariantCulture));
            Environment.SetEnvironmentVariable("AEGISUB_SHEAR_RESTORE_FOCUS", focus.ToString(CultureInfo.InvariantCulture));
        }
    }

    public IDisposable Activate(string desktopName)
    {
        if (Native.InputDesktopName() != originalName) throw new InvalidOperationException("Input desktop changed before the private desktop lease");
        var target = Native.OpenDesktop(desktopName, 0, 0, 0x101);
        try
        {
            if (target.IsInvalid || Native.ObjectName(target) != desktopName) throw new InvalidOperationException("Private input desktop handle does not match the owned desktop");
            Native.Check(Native.SwitchDesktop(target), "Switch to private input desktop");
            activeName = desktopName;
            VerifyActive();
            Record("activated", desktopName);
            return new Lease(this, target);
        }
        catch { target.Dispose(); Restore("failed-activation"); throw; }
    }

    public void VerifyActive()
    {
        if (activeName is null || Native.InputDesktopName() != activeName) throw new InvalidOperationException("Synthetic input target desktop is no longer active");
    }

    public void TrackKeys(uint key, uint modifier) => heldKeys = modifier == 0 ? [key] : [key, modifier];
    public void ClearKeys() => heldKeys = [];

    public void Restore(string phase)
    {
        if (activeName is { } expected && Native.InputDesktopName() == expected && heldKeys.Length != 0)
            Native.SendKeys(heldKeys.Select(key => (key, true)).ToArray());
        heldKeys = [];
        Native.Check(Native.SwitchDesktop(original), "Restore original input desktop");
        activeName = null;
        Exception? failure = null;
        var restoreThread = new Thread(() =>
        {
            try
            {
                Native.Check(Native.SetThreadDesktop(original), "Bind restoration thread to original desktop");
                Native.Check(Native.SetCursorPos(cursor.X, cursor.Y), "Restore original cursor");
                Native.RestoreFocus(foreground, focus);
            }
            catch (Exception error) { failure = error; }
        }) { IsBackground = true };
        restoreThread.Start();
        if (!restoreThread.Join(5000)) throw new TimeoutException("Original desktop focus/cursor restoration exceeded five seconds");
        if (failure is not null) throw new InvalidOperationException("Original desktop restoration failed", failure);
        if (Native.InputDesktopName() != originalName) throw new InvalidOperationException("Original input desktop did not restore");
        Record(phase, originalName);
    }

    void Record(string phase, string current)
    {
        if (!Directory.Exists(artifacts)) return;
        File.AppendAllText(Path.Combine(artifacts, "input-desktop-evidence.ndjson"), JsonSerializer.Serialize(new
        {
            phase,
            process_id = Environment.ProcessId,
            original_desktop = originalName,
            input_desktop = current,
            original_restored = current == originalName
        }) + Environment.NewLine);
    }

    public void Dispose() => original.Dispose();

    public void CompleteLauncherEvidence()
    {
        var path = Path.Combine(artifacts, "launcher-identity.json");
        var history = Path.Combine(artifacts, "input-desktop-evidence.ndjson");
        if (!File.Exists(path) || !File.Exists(history)) return;
        var switched = false;
        foreach (var line in File.ReadLines(history))
        {
            using var item = JsonDocument.Parse(line);
            if (item.RootElement.GetProperty("phase").GetString() == "activated") switched = true;
        }
        var identity = JsonNode.Parse(File.ReadAllText(path))!;
        identity["switched_desktop"] = switched;
        identity["input_desktop_restored"] = Native.InputDesktopName() == originalName;
        File.WriteAllText(path, identity.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
    }

    sealed class Lease(InputDesktopRestoration owner, DesktopHandle desktop) : IDisposable
    {
        public void Dispose()
        {
            try { owner.Restore("lease-finally"); }
            finally { desktop.Dispose(); }
        }
    }
}

static partial class Native
{
    [LibraryImport("user32.dll", SetLastError = true)] public static partial nint SetThreadDpiAwarenessContext(nint context);
    [LibraryImport("user32.dll")] public static partial int ShowWindow(nint window, int command);
    [LibraryImport("user32.dll", SetLastError = true)] public static partial int SetWindowPos(nint window, nint after, int x, int y, int width, int height, uint flags);
    [LibraryImport("user32.dll", SetLastError = true)] public static partial int GetClientRect(nint window, out Rect rect);
    [LibraryImport("user32.dll", SetLastError = true)] public static partial int GetWindowRect(nint window, out Rect rect);
    [LibraryImport("user32.dll", SetLastError = true)] public static partial int ScreenToClient(nint window, ref NativePoint point);
    [LibraryImport("user32.dll")] public static partial int IsWindowVisible(nint window);
    [LibraryImport("user32.dll")] public static partial int IsWindowEnabled(nint window);
    [LibraryImport("user32.dll", EntryPoint = "PostMessageW", SetLastError = true)] public static partial int PostMessage(nint window, uint message, nuint wParam, nint lParam);
    [LibraryImport("user32.dll", EntryPoint = "SendMessageTimeoutW", SetLastError = true)]
    private static partial nint SendMessageTimeout(nint window, uint message, nuint wParam, nint lParam, uint flags, uint timeout, out nuint result);
    [LibraryImport("user32.dll", EntryPoint = "GetWindowThreadProcessId", SetLastError = true)] public static partial uint WindowThread(nint window, out uint processId);
    [LibraryImport("kernel32.dll")] private static partial uint GetCurrentThreadId();
    [LibraryImport("user32.dll", SetLastError = true)] private static partial int AttachThreadInput(uint from, uint to, int attach);
    [LibraryImport("user32.dll", SetLastError = true)] private static unsafe partial int GetKeyboardState(byte* state);
    [LibraryImport("user32.dll")] private static partial nint SetFocus(nint window);
    [LibraryImport("user32.dll")] private static partial nint GetFocus();
    [LibraryImport("user32.dll", SetLastError = true)] public static partial nint GetForegroundWindow();
    [LibraryImport("user32.dll", SetLastError = true)] private static partial int SetForegroundWindow(nint window);
    [LibraryImport("user32.dll", SetLastError = true)] public static partial int GetCursorPos(out NativePoint point);
    [LibraryImport("user32.dll", SetLastError = true)] public static partial int SetCursorPos(int x, int y);
    [LibraryImport("user32.dll", SetLastError = true)] public static partial int GetGUIThreadInfo(uint thread, ref GuiThreadInfo info);
    [LibraryImport("user32.dll", SetLastError = true)] public static partial DesktopHandle OpenInputDesktop(uint flags, int inherit, uint access);
    [LibraryImport("user32.dll", EntryPoint = "OpenDesktopW", StringMarshalling = StringMarshalling.Utf16, SetLastError = true)]
    public static partial DesktopHandle OpenDesktop(string name, uint flags, int inherit, uint access);
    [LibraryImport("user32.dll", SetLastError = true)] public static partial int SwitchDesktop(DesktopHandle desktop);
    [LibraryImport("user32.dll", SetLastError = true)] public static partial int SetThreadDesktop(DesktopHandle desktop);
    [LibraryImport("user32.dll")] public static partial int CloseDesktop(nint desktop);
    [LibraryImport("user32.dll", EntryPoint = "GetUserObjectInformationW", SetLastError = true)]
    private static unsafe partial int GetUserObjectInformation(DesktopHandle desktop, int index, char* output, uint bytes, out uint needed);
    [LibraryImport("user32.dll", SetLastError = true)] private static unsafe partial uint SendInput(uint count, Input* inputs, int size);
    [LibraryImport("dwmapi.dll")] private static partial int DwmFlush();

    public static void FlushDesktop() => Marshal.ThrowExceptionForHR(DwmFlush());

    public static void Check(int result, string operation)
    {
        if (result == 0) throw new Win32Exception(Marshal.GetLastPInvokeError(), operation + " failed");
    }

    public static nuint Send(nint window, uint message, nuint wParam, nint lParam)
    {
        if (SendMessageTimeout(window, message, wParam, lParam, 3, 5000, out var result) == 0)
            throw new Win32Exception(Marshal.GetLastPInvokeError(), "Bounded native window message failed");
        return result;
    }

    public static void Mouse(nint window, uint message, Point point, uint state) =>
        Send(window, message, state, unchecked((nint)((point.Y << 16) | (point.X & 0xffff))));

    public static unsafe string ObjectName(DesktopHandle desktop)
    {
        var output = new char[256];
        fixed (char* text = output) Check(GetUserObjectInformation(desktop, 2, text, (uint)(output.Length * 2), out _), "Read desktop identity");
        return new string(output).TrimEnd('\0');
    }

    public static string InputDesktopName()
    {
        using var desktop = OpenInputDesktop(0, 0, 1);
        if (desktop.IsInvalid) throw new Win32Exception(Marshal.GetLastPInvokeError(), "Open input desktop for verification failed");
        return ObjectName(desktop);
    }

    public static void FocusOwned(nint main, nint window, int processId)
    {
        var thread = WindowThread(window, out var observedProcess);
        if (thread == 0 || observedProcess != processId || WindowThread(main, out var mainOwner) != thread || mainOwner != processId)
            throw new InvalidOperationException("Input focus target no longer belongs to the owned GUI host thread");
        var current = GetCurrentThreadId();
        Check(AttachThreadInput(current, thread, 1), "Attach owned private focus threads");
        try
        {
            SetForegroundWindow(main);
            SetFocus(window);
            WaitNative(() => GetForegroundWindow() == main && GetFocus() == window, "owned foreground canvas");
        }
        finally { Check(AttachThreadInput(current, thread, 0), "Detach owned private focus threads"); }
    }

    public static void RestoreFocus(nint foreground, nint focus)
    {
        if (foreground == 0 || IsWindowVisible(foreground) == 0) return;
        if (GetForegroundWindow() != foreground) SetForegroundWindow(foreground);
        WaitNative(() => GetForegroundWindow() == foreground, "original foreground window");
        var target = WindowThread(foreground, out _);
        var info = new GuiThreadInfo { Size = (uint)Marshal.SizeOf<GuiThreadInfo>() };
        Check(GetGUIThreadInfo(target, ref info), "Read restored foreground focus");
        if (focus == 0 || info.Focus == focus || IsWindowVisible(focus) == 0) return;
        var current = GetCurrentThreadId();
        Check(AttachThreadInput(current, target, 1), "Attach original focus restoration thread");
        try
        {
            SetFocus(focus);
            if (GetFocus() != focus) throw new InvalidOperationException("Original focus window did not restore");
        }
        finally { Check(AttachThreadInput(current, target, 0), "Detach original focus restoration thread"); }
    }

    public static unsafe void SendKeys((uint Key, bool Up)[] keys)
    {
        var inputs = keys.Select(item => new Input
        {
            Type = 1,
            Union = new InputUnion
            {
                Keyboard = new KeyboardInput { Key = checked((ushort)item.Key), Flags = (item.Up ? 2u : 0) | (item.Key is >= 0x21 and <= 0x28 ? 1u : 0) }
            }
        }).ToArray();
        fixed (Input* input = inputs)
            if (SendInput((uint)inputs.Length, input, Marshal.SizeOf<Input>()) != inputs.Length)
                throw new Win32Exception(Marshal.GetLastPInvokeError(), "Owned private-desktop SendInput did not enqueue every key");
    }

    public static unsafe void Key(nint main, nint window, int processId, uint key, uint modifier, InputDesktopRestoration restoration, Action<object> evidence)
    {
        restoration.VerifyActive();
        FocusOwned(main, window, processId);
        var targetThread = WindowThread(window, out var owner);
        if (owner != processId) throw new InvalidOperationException("Keyboard canvas owner changed");
        var currentThread = GetCurrentThreadId();
        Check(AttachThreadInput(currentThread, targetThread, 1), "Attach owned keyboard state observation");
        try
        {
            if (!KeyboardState(key, false) || (modifier != 0 && !KeyboardState(modifier, false)))
                throw new InvalidOperationException("Requested synthetic key was already held before the case");
            restoration.TrackKeys(key, modifier);
            restoration.VerifyActive();
            SendKeys(modifier == 0 ? [(key, false)] : [(modifier, false), (key, false)]);
            WaitNative(() => KeyboardState(key, true) && (modifier == 0 || KeyboardState(modifier, true)), "GUI consumed key and modifier down");
            restoration.VerifyActive();
            SendKeys(modifier == 0 ? [(key, true)] : [(key, true), (modifier, true)]);
            WaitNative(() => KeyboardState(key, false) && (modifier == 0 || KeyboardState(modifier, false)), "GUI consumed key and modifier up");
            restoration.ClearKeys();
            evidence(new { key, modifier, owner, gui_thread = targetThread, key_down_consumed = true, key_up_consumed = true, input_method = "SendInput" });
        }
        finally { Check(AttachThreadInput(currentThread, targetThread, 0), "Detach owned keyboard state observation"); }
    }

    static unsafe bool KeyboardState(uint key, bool pressed)
    {
        var keys = new byte[256];
        fixed (byte* state = keys) Check(GetKeyboardState(state), "Observe consumed GUI keyboard state");
        return ((keys[key] & 0x80) != 0) == pressed;
    }

    static void WaitNative(Func<bool> predicate, string name)
    {
        var deadline = Stopwatch.GetTimestamp() + 4 * Stopwatch.Frequency;
        while (Stopwatch.GetTimestamp() < deadline)
        {
            if (predicate()) return;
            Thread.Sleep(10);
        }
        throw new TimeoutException("Timed out waiting for " + name);
    }
}
