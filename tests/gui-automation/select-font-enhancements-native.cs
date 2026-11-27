#:property TargetFramework=net10.0-windows
#:property UseWPF=true
#:property ImplicitUsings=enable
#:property Nullable=enable
#:property PublishAot=false
#:project driver/Aegisub.GuiAutomation.Driver.csproj

using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Windows.Automation;
using Aegisub.GuiAutomation.Driver;

if (args.Length != 2)
{
    Console.Error.WriteLine("Usage: dotnet run --file tests/gui-automation/select-font-enhancements-native.cs -- EXE NEW_ARTIFACT_DIRECTORY");
    return 2;
}
try
{
    return PrivateDesktop.Run(args, Path.GetFullPath(args[1]), desktop => new Scenario(args, desktop).Run(), guardDescendants: true);
}
catch (Exception error)
{
    Console.Error.WriteLine(error);
    return 1;
}

sealed class Scenario(string[] args, PrivateDesktop desktop)
{
    const string Face = "Microsoft YaHei";
    const string VerticalFace = "@Microsoft YaHei";
    const string CustomFace = "Uninstalled E2E Face 世界";
    const string HorizontalScale = "Horizontal scale";
    const string VerticalScale = "Vertical scale";
    const string CharacterSpacing = "Character spacing";
    const string ScaleA = @"A{\fs34\b0\i0\u0}{\fscy80\fsp-2.75}WIDE SAMPLE";
    const string ScaleB = @"A{\fs48\b1\i1\u1}{\fscx91.2345\fscy135.6789\fsp-1.125}Second scale sample";
    const string BlockedA = @"A{\bord0}{\fscx110.25\fscy82}Writable scale sample";
    const string BlockedB = @"A{\t(0,1000,\fscx200)}{\fscy90}Animated scale sample";
    const string QuickVertical = @"{\fn@Microsoft YaHei\fs32\b0\i0\u0\fscx100}Quick family sample";
    const string QuickHorizontal = @"{\fnMicrosoft YaHei\fs32\b0\i0\u0\fscx100}Quick family sample";
    static readonly string[] LegacyRecent = [VerticalFace, Face, CustomFace];
    static readonly string[] RecentFamilies = [Face, CustomFace];
    static readonly (string Prefix, string Text)[] Seed =
    [
        ("Dialogue: 1,0:00:00.00,0:00:02.00,Default,scale-active,11,12,13,fx-one,", ScaleA),
        ("Dialogue: 2,0:00:03.00,0:00:04.00,Alt,scale-second,21,22,23,fx-two,", ScaleB),
        ("Dialogue: 3,0:00:03.00,0:00:04.00,Default,blocked-active,31,32,33,fx-three,", BlockedA),
        ("Dialogue: 4,0:00:03.00,0:00:04.00,Alt,blocked-second,41,42,43,fx-four,", BlockedB),
        ("Dialogue: 5,0:00:03.00,0:00:04.00,Default,quick-font,51,52,53,fx-five,", QuickVertical),
        ("Comment: 9,0:00:00.00,0:00:02.00,Alt,untouched,91,92,93,sentinel,", @"{\rAlt\fscx123.4567}untouched café 😀, comma")
    ];
    static readonly (int Row, string Text)[] XOnly =
    [
        (0, @"A{\fs34\b0\i0\u0}{\fscy80\fsp-2.75\fscx160}WIDE SAMPLE"),
        (1, @"A{\fs48\b1\i1\u1}{\fscx160\fscy135.6789\fsp-1.125}Second scale sample")
    ];
    static readonly (int Row, string Text)[] BothScales =
    [
        (0, @"A{\fs34\b0\i0\u0}{\fscy120\fsp-2.75\fscx160}WIDE SAMPLE"),
        (1, @"A{\fs48\b1\i1\u1}{\fscx160\fscy120\fsp-1.125}Second scale sample")
    ];
    static readonly (int Row, string Text)[] WithSpacing =
    [
        (0, @"A{\fs34\b0\i0\u0}{\fscy120\fsp5.7\fscx160}WIDE SAMPLE"),
        (1, @"A{\fs48\b1\i1\u1}{\fscx160\fscy120\fsp5.7}Second scale sample")
    ];
    static readonly (int Row, string Text)[] FinalScales =
    [
        (0, @"A{\fs34\b0\i0\u0}{\fscy120\fsp5.8\fscx160}WIDE SAMPLE"),
        (1, @"A{\fs48\b1\i1\u1}{\fscx160\fscy120\fsp5.8}Second scale sample")
    ];
    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };

    readonly string artifacts = Path.GetFullPath(args[1]);
    string Profile => Path.Combine(artifacts, "profile");
    string Config => Path.Combine(Profile, "user", "config.json");
    string Fixture => Path.Combine(artifacts, "fixture.ass");
    Process? app;
    string host = "", phase = "startup", decimalSeparator = "";
    nint main, dialog, fontCombo, fontEdit, scaleX, scaleY, spacing, star, information, quickChoice, quickList;
    int launch, stateSequence, captureSequence;
    string[] savedHeader = [];
    sealed record VideoSample(int Width, int Height, Rectangle Crop, int BlackPixels, int LightPixels, string Hash);

    public int Run()
    {
        Directory.CreateDirectory(artifacts);
        desktop.RecordDriver();
        using var log = new StreamWriter(Path.Combine(artifacts, "driver.log")) { AutoFlush = true };
        Console.SetOut(log);
        Console.SetError(log);
        try
        {
            Require(Native.SetThreadDpiAwarenessContext(-4) != 0, "Driver uses PerMonitorV2 coordinates");
            Prepare();
            Start();
            phase = "grid-empty";
            GridMenu(layer: true, verticalMargin: true, toggle: true);
            RunScales();
            RunQuickSelection();
            NormalExit();

            phase = "quick-restart";
            ExpectStoredGrid(layer: true);
            Start();
            GridMenu(layer: true, verticalMargin: false, toggle: false);
            CheckRestartAndClearLists();
            NormalExit();
            ExpectStoredFonts([], []);

            phase = "grid-short";
            // Keep the actual empty font arrays from the previous shutdown.
            var config = ReadConfig();
            config["Subtitle"]!["Grid"]!["Column"] = JsonNode.Parse("""[{"bool":true},{"bool":false}]""");
            File.WriteAllText(Config, config.ToJsonString(Json));
            Start();
            GridMenu(layer: false, verticalMargin: true, toggle: true);
            SelectLines("quick-font", 4);
            OpenFont();
            ExpectQuickList("Recent Fonts", []);
            Require(!Enabled(Control(dialog, "Show only favorite fonts")), "Empty favorites leave the filter disabled");
            Require(Candidates().Contains(Face), "Empty saved favorites still expose all installed families");
            ExpectEmptyListActions();
            ChooseQuickPage("Favorite Fonts");
            ExpectQuickList("Favorite Fonts", []);
            ExpectEmptyListActions();
            CloseDialog("Cancel");
            NormalExit();
            ExpectStoredGrid(layer: false);
            ExpectStoredFonts([], []);

            phase = "grid-short-restart";
            Start();
            GridMenu(layer: false, verticalMargin: false, toggle: false);
            CheckVariantFamilyChanges();
            NormalExit();

            phase = "styled-editor-cancel";
            config = ReadConfig();
            config["Subtitle"]!["Use STC"] = true;
            File.WriteAllText(Config, config.ToJsonString(Json));
            Start();
            CheckStyledEditorCancel();
            NormalExit();
            WriteJson("result.json", new { passed = true });
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            WriteJson("result.json", new { passed = false, phase, error = error.ToString() });
            if (app is { HasExited: false })
            {
                try { Inventory("failure"); }
                catch (Exception inventoryError) { Console.Error.WriteLine(inventoryError); }
                try { CaptureWindow("failure", dialog != 0 && Native.IsWindowVisible(dialog) ? dialog : main); }
                catch (Exception captureError) { Console.Error.WriteLine(captureError); }
            }
            return 1;
        }
        finally
        {
            if (app is not null)
            {
                desktop.Stop(app);
                app.Dispose();
            }
        }
    }

    void CheckStyledEditorCancel()
    {
        SelectLines("scale-", 0, 1);
        var edit = Children(main).Single(h => Class(h).StartsWith("wxWindow", StringComparison.Ordinal) && Text(h) == "stcwindow");
        ClickPoint(edit, Native.Point(4, 6));
        Key(edit, 0x24); // Home
        Wait(() => ReadStyledRange() == (0, 0), "The styled editor caret moves to the start of the first subtitle");
        var width = Native.ClientBounds(edit).Width;
        var end = Native.Point(width - 6, 6);
        Send(edit, Native.LeftDown, 1, Native.Point(4, 6));
        Send(edit, Native.MouseMove, 1, end);
        Send(edit, Native.LeftUp, 0, end);
        Wait(() => ReadStyledRange() == (0, ScaleA.Length), "The native drag selects the full first subtitle in the styled editor");
        var before = ReadStyledRange();
        OpenFont();
        TypeText(spacing, LocalNumber("3.5"));
        Wait(() => Document().GetProperty("rows")[0].GetProperty("text").GetString() != ScaleA,
            "Spacing preview reaches the selected lines in styled-editor mode");
        CloseDialog("Cancel");
        var reverted = ExpectDocument("Styled-editor Cancel restores the original ASS events");
        Require(reverted.GetProperty("undo_empty").GetBoolean(), "Styled-editor Cancel leaves Undo empty");
        var restored = ReadStyledRange();
        Require(restored == before, $"Styled-editor Cancel restores the native selection {before}, observed {restored}");

        (int Start, int End) ReadStyledRange()
        {
            var state = Document();
            return (state.GetProperty("editor_selection_start").GetInt32(), state.GetProperty("editor_selection_end").GetInt32());
        }
    }

    void RunScales()
    {
        phase = "scale-initial";
        SelectLines("scale-", 0, 1);
        AssertSaved("baseline", initializeHeader: true);
        var anchor = ScaleA.IndexOf(@"\fscy", StringComparison.Ordinal) + @"\fscy".Length;
        SetRange("WIDE SAMPLE", anchor, anchor + 1);
        OpenFont();
        var initialX = Text(scaleX).Trim();
        Require(initialX.StartsWith("100", StringComparison.Ordinal) && initialX.EndsWith('0') && initialX.Length == 5,
            "Initial horizontal scale displays 100 with one decimal digit");
        decimalSeparator = initialX[3..^1];
        FocusEdit(scaleY);
        FocusEdit(scaleX);
        CloseDialog("OK");
        var untouched = ExpectDocument("Opening, focusing and accepting preserves the original precision, spacing and styles");
        Require(untouched.GetProperty("undo_empty").GetBoolean(), "An unchanged dialog creates no Undo entry");

        phase = "spacing-only";
        SetRange("WIDE SAMPLE", anchor, anchor + 1);
        OpenFont();
        TypeText(spacing, LocalNumber("3.5"));
        ExpectPreview("WIDE SAMPLE",
            (0, @"A{\fs34\b0\i0\u0}{\fscy80\fsp3.5}WIDE SAMPLE"),
            (1, @"A{\fs48\b1\i1\u1}{\fscx91.2345\fscy135.6789\fsp3.5}Second scale sample"));
        CloseDialog("Cancel");
        ExpectDocument("Cancel restores both original spacing values");

        phase = "scale-preview-cancel";
        SetRange("WIDE SAMPLE", anchor, anchor + 1);
        var before = CaptureVideo("video-before");
        OpenFont();
        AdjustScales(capture: true, before);
        CloseDialog("Cancel");
        Wait(() => ReadRange("WIDE SAMPLE") == (anchor, anchor + 1), "Cancel restores the original native editor range");
        var reverted = ExpectDocument("Cancel restores every original ASS event");
        Require(reverted.GetProperty("undo_empty").GetBoolean(), "Cancelling preview leaves Undo empty");
        CompareVideo(before, CaptureVideo("video-cancel"), equal: true);
        AssertSaved("scale-cancel");

        phase = "scale-accept-undo-redo";
        SetRange("WIDE SAMPLE", anchor, anchor + 1);
        OpenFont();
        AdjustScales(capture: false, before);
        CloseDialog("OK");
        ExpectDocument("OK retains the final multi-line preview", FinalScales);
        var accepted = CaptureVideo("video-ok");
        CompareVideo(before, accepted, equal: false);
        AssertSaved("scale-ok", changes: FinalScales);
        History("Undo set font", "WIDE SAMPLE");
        Require(ExpectDocument("One Undo restores all original lines").GetProperty("undo_empty").GetBoolean(),
            "The multi-line font edit added exactly one Undo operation");
        Require(ReadRange("WIDE SAMPLE") == (anchor, anchor + 1), "Undo restores the original native editor range");
        CompareVideo(before, CaptureVideo("video-undo"), equal: true);
        AssertSaved("scale-undo");
        History("Redo set font", "WIDE SAMPLE");
        ExpectDocument("Redo restores all final values", FinalScales);
        CompareVideo(accepted, CaptureVideo("video-redo"), equal: true);
        AssertSaved("scale-redo", changes: FinalScales);
        History("Undo set font", "WIDE SAMPLE");

        phase = "numeric-finalization";
        SetRange("WIDE SAMPLE", anchor, anchor + 1);
        OpenFont();
        FocusEdit(scaleY);
        TypeText(scaleY, "");
        FocusEdit(scaleX);
        ExpectPreview("WIDE SAMPLE",
            (0, @"A{\fs34\b0\i0\u0}{\fscy100\fsp-2.75}WIDE SAMPLE"),
            (1, @"A{\fs48\b1\i1\u1}{\fscx91.2345\fscy100\fsp-1.125}Second scale sample"));
        CloseDialog("Cancel");
        ExpectDocument("Blur uses the default vertical scale in preview, and Cancel reverts it");

        OpenFont();
        FocusEdit(scaleX);
        TypeText(scaleX, "12" + decimalSeparator);
        CloseDialog("OK");
        (int Row, string Text)[] trailing =
        [
            (0, @"A{\fs34\b0\i0\u0}{\fscy80\fsp-2.75\fscx12}WIDE SAMPLE"),
            (1, @"A{\fs48\b1\i1\u1}{\fscx12\fscy135.6789\fsp-1.125}Second scale sample")
        ];
        ExpectDocument("Direct OK accepts a number with a trailing decimal separator", trailing);
        AssertSaved("scale-trailing-decimal", changes: trailing);
        History("Undo set font", "WIDE SAMPLE");

        SetRange("WIDE SAMPLE", anchor, anchor + 1);
        OpenFont();
        FocusEdit(spacing);
        TypeText(spacing, "");
        CloseDialog("OK");
        (int Row, string Text)[] blank =
        [
            (0, @"A{\fs34\b0\i0\u0}{\fscy80\fsp0}WIDE SAMPLE"),
            (1, @"A{\fs48\b1\i1\u1}{\fscx91.2345\fscy135.6789\fsp0}Second scale sample")
        ];
        ExpectDocument("Direct OK defaults blank spacing to zero", blank);
        AssertSaved("scale-blank-ok", changes: blank);
        History("Undo set font", "WIDE SAMPLE");

        phase = "group-animation";
        SelectLines("blocked-active", 2);
        SelectLines("blocked-", 2, 3);
        anchor = BlockedA.IndexOf(@"\fscy", StringComparison.Ordinal) + 5;
        SetRange("Writable scale sample", anchor, anchor);
        OpenFont();
        Require(!Enabled(scaleX) && Enabled(scaleY) && Enabled(spacing), "One inherited X animation disables X for the whole group and leaves Y and spacing enabled");
        _ = Control(dialog, "Horizontal scale cannot be edited for 1 selected line(s) because it is animated in an earlier block. Edit it in the block with the animation.");
        CaptureWindow("group-animation-warning", dialog);
        TypeText(scaleY, LocalNumber("125.0"));
        (int Row, string Text)[] animated =
        [
            (2, @"A{\bord0}{\fscx110.25\fscy125}Writable scale sample"),
            (3, @"A{\t(0,1000,\fscx200)}{\fscy125}Animated scale sample")
        ];
        ExpectPreview("Writable scale sample", animated);
        CloseDialog("OK");
        ExpectDocument("Both lines receive Y and retain the original X and animation bytes", animated);
        AssertSaved("scale-animation-guard", changes: animated);
        History("Undo set font", "Writable scale sample");
        ExpectDocument("The numeric cases leave the original fixture ready for the font list scenario");
    }

    void AdjustScales(bool capture, VideoSample before)
    {
        TypeText(scaleX, LocalNumber("160.0"));
        ExpectPreview("WIDE SAMPLE", XOnly);
        if (capture) CompareVideo(before, CaptureVideo("video-preview-x"), equal: false);
        TypeText(scaleY, LocalNumber("120.0"));
        ExpectPreview("WIDE SAMPLE", BothScales);
        TypeText(spacing, LocalNumber("5.7"));
        ExpectPreview("WIDE SAMPLE", WithSpacing);
        var typedSpacing = capture ? CaptureVideo("video-preview-typed-spacing") : null;
        StepSpacing();
        ExpectPreview("WIDE SAMPLE", FinalScales);
        if (typedSpacing is not null) CompareVideo(typedSpacing, CaptureVideo("video-preview-final"), equal: false);
    }

    void RunQuickSelection()
    {
        phase = "quick-filter-and-cancel";
        SelectLines("quick-font", 4);
        SetRange("Quick family sample", 4, 4);
        OpenFont();
        ExpectFont(VerticalFace, vertical: true);
        ExpectQuickList("Recent Fonts", RecentFamilies);
        var all = Candidates();
        Require(all.Contains(Face) && all.Contains("Arial") && all.Contains("Tahoma") && !all.Contains(VerticalFace),
            "Compact mode exposes real installed bare family candidates");
        var filter = Control(dialog, "Show only favorite fonts");
        Require(Enabled(filter) && !Checked(filter), "Each dialog opens with the complete font catalog");

        FocusEdit(fontEdit);
        TypeText(fontEdit, "YaHe");
        Wait(() => Send(fontCombo, Native.ComboDropped) != 0, "A substring query automatically opens the actual font dropdown");
        var candidatesList = Native.ComboChildren(fontCombo).List;
        Wait(() =>
        {
            int index = checked((int)Send(candidatesList, Native.ListSelection));
            return index >= 0 && index < all.Length && all[index].StartsWith(Face, StringComparison.Ordinal);
        }, "Contains matching highlights the installed YaHei family for a query that is not a prefix");
        Require(Text(fontEdit) == "YaHe", "The highlighted candidate leaves the typed query in the edit");
        Send(fontEdit, Native.EditSetSelection, 4, 4);
        // Let the filter dismiss the open dropdown while matching for the new query is pending.
        Post(fontEdit, Native.Character, 'i', 1);
        Post(filter, Native.ButtonClick);
        Wait(() => Checked(filter), "Queued native click enables the favorites filter");
        ExpectFontAfterCallbacks("YaHei");
        EqualItems("Favorite-only candidate list", ["Arial"], Candidates());
        // The first click also moved Windows focus. With focus now on the filter,
        // isolate selection preservation across the next candidate refresh.
        Require(Native.FocusedWindow(filter) == filter, "The native filter owns focus after the click");
        Send(fontEdit, Native.EditSetSelection, 1, 3);
        ClickCheck(filter, false);
        ExpectFontAfterCallbacks("YaHei");
        var refreshedRange = ReadEditRange(fontEdit);
        Require(refreshedRange == (1, 3), $"Candidate refresh preserves selection [1, 3], observed {refreshedRange}");
        ClickCheck(filter, true);
        EqualItems("Favorite-only candidate list after toggling again", ["Arial"], Candidates());
        ExpectStoredFonts(LegacyRecent, ["Arial"]);

        ChooseQuickPage("Favorite Fonts");
        ExpectQuickList("Favorite Fonts", ["Arial"]);
        ClickQuickItem("Arial");
        ExpectFontAfterCallbacks("Arial", vertical: false);
        ExpectPreview("Quick family sample", (4, @"{\fnArial\fs32\b0\i0\u0\fscx100}Quick family sample"));
        Click(Control(dialog, "Remove"));
        Wait(() => !Enabled(filter) && !Checked(filter), "Removing the last favorite clears and disables the filter");
        Require(Candidates().Contains(Face) && Candidates().Contains("Tahoma"), "Removing the last favorite restores the full candidates");
        ChooseQuickPage("Favorite Fonts");
        ExpectQuickList("Favorite Fonts", []);
        ExpectEmptyListActions();
        ExpectStoredFonts(LegacyRecent, []);

        FocusEdit(fontEdit);
        TypeText(fontEdit, "Tahom");
        Wait(() => Send(fontCombo, Native.ComboDropped) != 0, "Typing after the refresh still auto-expands the native list");
        Send(fontEdit, Native.EditSetSelection, 5, 5);
        Post(fontEdit, Native.Character, 'a', 1);
        Post(star, Native.ButtonClick);
        ExpectQuickList("Favorite Fonts", ["Tahoma"]);
        ExpectFontAfterCallbacks("Tahoma", vertical: false);
        ExpectStoredFonts(LegacyRecent, ["Tahoma"]);
        CloseDialog("Cancel");
        var cancelled = ExpectDocument("Cancel discards all quick-list and typed font previews");
        Require(cancelled.GetProperty("undo_empty").GetBoolean(), "Cancelled font previews leave no Undo entry");
        Require(ReadRange("Quick family sample") == (4, 4), "Font Cancel restores the exact native editor caret");
        ExpectStoredFonts(LegacyRecent, ["Tahoma"]);

        phase = "quick-filtered-recent-double-click";
        OpenFont();
        filter = Control(dialog, "Show only favorite fonts");
        Require(!Checked(filter), "A new dialog shows all candidate fonts again");
        ClickCheck(filter, true);
        EqualItems("Restored favorite-only candidates", ["Tahoma"], Candidates());
        ExpectFontAfterCallbacks(VerticalFace, vertical: true);
        ExpectQuickList("Recent Fonts", RecentFamilies);
        FocusEdit(fontEdit);
        TypeText(fontEdit, "ahom");
        Wait(() => Send(fontCombo, Native.ComboDropped) != 0, "The filtered combo opens for a pending Tahoma match");
        ClickQuickItem(Face);
        ExpectFontAfterCallbacks(Face, vertical: false);
        ExpectPreview("Quick family sample", (4, QuickHorizontal));
        EqualItems("Choosing a nonfavorite recent leaves the filter in effect", ["Tahoma"], Candidates());
        ExpectStoredFonts(LegacyRecent, ["Tahoma"]);
        ClickCheck(Control(dialog, "Vertical"), true);
        ExpectFontAfterCallbacks(VerticalFace, vertical: true);
        ExpectPreview("Quick family sample", (4, QuickVertical));
        ClickQuickItem(Face);
        TypeText(scaleX, LocalNumber("135.0"));
        const string combined = @"{\fnMicrosoft YaHei\fs32\b0\i0\u0\fscx135}Quick family sample";
        ExpectPreview("Quick family sample", (4, combined));
        var old = dialog;
        var point = QuickItemPoint(Face);
        Console.WriteLine("Double-click recent family " + Face);
        ClickPoint(quickList, point);
        Post(quickList, Native.LeftDoubleClick, 1, point);
        WaitForDialogClosed(old);
        ExpectDocument("Double-click accepts one final face-and-scale edit", (4, combined));
        AssertSaved("quick-double-click", changes: [(4, combined)]);
        ExpectStoredFonts(RecentFamilies, ["Tahoma"]);
        History("Undo set font", "Quick family sample");
        var undone = ExpectDocument("One Undo restores the vertical face and original scale");
        Require(undone.GetProperty("undo_empty").GetBoolean(), "Double-click added exactly one Undo operation");
        History("Redo set font", "Quick family sample");
        ExpectDocument("Redo restores the confirmed quick selection", (4, combined));

        phase = "quick-vertical-confirm";
        SetRange("Quick family sample", 4, 4);
        OpenFont();
        ExpectFont(Face, vertical: false);
        ClickCheck(Control(dialog, "Vertical"), true);
        ExpectFontAfterCallbacks(VerticalFace, vertical: true);
        const string verticalScaled = @"{\fn@Microsoft YaHei\fs32\b0\i0\u0\fscx135}Quick family sample";
        ExpectPreview("Quick family sample", (4, verticalScaled));
        CloseDialog("OK");
        ExpectDocument("Confirming the vertical checkbox writes the @ face to the subtitle", (4, verticalScaled));
        AssertSaved("quick-vertical-confirm", changes: [(4, verticalScaled)]);
        ExpectStoredFonts(RecentFamilies, ["Tahoma"]);

        SetRange("Quick family sample", 4, 4);
        OpenFont();
        ExpectFont(VerticalFace, vertical: true);
        ExpectQuickList("Recent Fonts", RecentFamilies);
        ClickQuickItem(Face);
        ExpectFontAfterCallbacks(Face, vertical: false);
        ExpectPreview("Quick family sample", (4, combined));
        CloseDialog("Cancel");
        ExpectDocument("Cancelling the horizontal recent restores the vertical subtitle", (4, verticalScaled));
    }

    void CheckVariantFamilyChanges()
    {
        phase = "variant-typed-family";
        var firstAnchor = ScaleA.IndexOf(@"\b0", StringComparison.Ordinal) + 2;
        SelectLines("scale-", 0, 1);
        SetRange("WIDE SAMPLE", firstAnchor, firstAnchor);
        OpenFont();
        ExpectFont("Arial");
        ClickCheck(Control(dialog, "Bold"), true);
        ExpectPreview("WIDE SAMPLE", (0, @"A{\fs34\b1\i0\u0}{\fscy80\fsp-2.75}WIDE SAMPLE"));
        FocusEdit(fontEdit);
        TypeText(fontEdit, "Tahoma");
        (int Row, string Text)[] typed =
        [
            (0, @"A{\fs34\b1\i0\u0\fnTahoma}{\fscy80\fsp-2.75}WIDE SAMPLE"),
            (1, @"A{\fs48\b1\i1\u1}{\fscx91.2345\fscy135.6789\fsp-1.125\fnTahoma}Second scale sample")
        ];
        ExpectPreview("WIDE SAMPLE", typed);
        CloseDialog("OK");
        ExpectDocument("OK preserves Bold and each line's own Italic value after typing a family", typed);
        AssertSaved("variant-typed-family", changes: typed);
        History("Undo set font", "WIDE SAMPLE");
        Require(ExpectDocument("Undo restores each line's original font and variants").GetProperty("undo_empty").GetBoolean(),
            "Typing and confirming a family with Bold adds one Undo operation");

        phase = "variant-recent-family";
        SelectLines("scale-second", 1);
        var secondAnchor = ScaleB.IndexOf(@"\i1", StringComparison.Ordinal) + 2;
        SetRange("Second scale sample", secondAnchor, secondAnchor);
        OpenFont();
        ExpectQuickList("Recent Fonts", ["Tahoma"]);
        ClickCheck(Control(dialog, "Italic"), false);
        ExpectPreview("Second scale sample", (1, @"A{\fs48\b1\i0\u1}{\fscx91.2345\fscy135.6789\fsp-1.125}Second scale sample"));
        ClickQuickItem("Tahoma");
        const string recent = @"A{\fs48\b1\i0\u1\fnTahoma}{\fscx91.2345\fscy135.6789\fsp-1.125}Second scale sample";
        ExpectPreview("Second scale sample", (1, recent));
        Click(star);
        Wait(() => Text(star) == "★", "The selected family is marked as a favorite");
        ExpectStoredFonts(["Tahoma"], ["Tahoma"]);
        CloseDialog("OK");
        ExpectDocument("Choosing a recent family preserves the explicit Italic removal", (1, recent));
        AssertSaved("variant-recent-family", changes: [(1, recent)]);
        History("Undo set font", "Second scale sample");

        phase = "variant-favorite-family";
        SelectLines("scale-active", 0);
        SetRange("WIDE SAMPLE", firstAnchor, firstAnchor);
        OpenFont();
        var font = Native.Bounds(Control(dialog, "Font"));
        var fontStyle = Children(dialog).Single(h => h != fontCombo && Class(h) == "ComboBox" && font.Contains(Native.Bounds(h)));
        ChooseComboItem(fontStyle, "Bold Italic");
        ExpectPreview("WIDE SAMPLE", (0, @"A{\fs34\b1\i1\u0}{\fscy80\fsp-2.75}WIDE SAMPLE"));
        ChooseQuickPage("Favorite Fonts");
        ClickQuickItem("Tahoma");
        const string favorite = @"A{\fs34\b1\i1\u0\fnTahoma}{\fscy80\fsp-2.75}WIDE SAMPLE";
        ExpectPreview("WIDE SAMPLE", (0, favorite));
        CloseDialog("OK");
        ExpectDocument("Choosing a favorite family preserves both Font Style edits", (0, favorite));
        AssertSaved("variant-favorite-family", changes: [(0, favorite)]);
    }

    void CheckRestartAndClearLists()
    {
        SelectLines("quick-font", 4);
        OpenFont();
        ExpectQuickList("Recent Fonts", RecentFamilies);
        ChooseQuickPage("Favorite Fonts");
        ExpectQuickList("Favorite Fonts", ["Tahoma"]);
        ChooseQuickPage("Recent Fonts");
        ClickQuickItem(CustomFace);
        ExpectFontAfterCallbacks(CustomFace, vertical: false);
        Click(Control(dialog, "Remove"));
        ExpectQuickList("Recent Fonts", [Face]);
        ExpectStoredFonts([Face], ["Tahoma"]);
        Click(Control(dialog, "Clear Recent Fonts"));
        ChooseQuickPage("Recent Fonts");
        ExpectQuickList("Recent Fonts", []);
        ExpectEmptyListActions();
        ExpectStoredFonts([], ["Tahoma"]);
        CloseDialog("Cancel");
        ExpectDocument("List maintenance remains independent of cancelled subtitle edits");

        OpenFont();
        ExpectQuickList("Favorite Fonts", ["Tahoma"]);
        ClickQuickItem("Tahoma");
        ExpectFontAfterCallbacks("Tahoma", vertical: false);
        Click(Control(dialog, "Remove from Favorites"));
        ChooseQuickPage("Favorite Fonts");
        ExpectQuickList("Favorite Fonts", []);
        ExpectEmptyListActions();
        ExpectStoredFonts([], []);
        CloseDialog("Cancel");
        ExpectDocument("The empty lists persist without committing a subtitle edit");
    }

    void Prepare()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Config)!);
        var config = JsonNode.Parse("""
            {
                "App": {"First Start": false, "Language": "en_US",
                    "Auto": {"Backup": false, "Check For Updates": false, "Load Linked Files": 1, "Save": false, "Save on Every Change": false}},
                "Subtitle": {"Use STC": false, "Provider": "libass", "Grid": {"Column": []}, "Font": {
                    "Prefer Localized Family Names": false, "Use Contains Matching": true, "Auto Expand List On Input": true,
                    "Compact Vertical Font List": true, "Recent Limit": 16,
                    "Recent": [{"string": "@Microsoft YaHei"}, {"string": "Microsoft YaHei"}, {"string": "Uninstalled E2E Face 世界"}],
                    "Favorites": [{"string": "Arial"}]}},
                "Video": {"Open Audio": false, "Subtitle Sync": false, "Scale with DPI": false, "Renderer": {"Backend": "opengl"}},
                "Automation": {"Lua Workspace": {"Enable LuaLS": false}}
            }
            """)!;
        File.WriteAllText(Config, config.ToJsonString(Json));
        File.Copy(Config, Path.Combine(artifacts, "seed-config.json"));
        AutomationTestProfile.WriteMenusWithoutClipboardValidation(Profile, artifacts);
        const string header = """
            [Script Info]
            Title: Native Select Font Enhancements
            Original Script: untouched metadata café
            ScriptType: v4.00+
            PlayResX: 640
            PlayResY: 480
            WrapStyle: 2
            ScaledBorderAndShadow: yes
            [Aegisub Project Garbage]
            Video File: ?dummy:24.000000:240:640:480:0:0:0:
            Video Zoom Percent: 1
            [V4+ Styles]
            Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
            Style: Default,Arial,34,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,5,10,10,10,1
            Style: Alt,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,-1,-1,-1,0,102,97,1,0,1,1,0,5,10,10,10,1
            [Events]
            Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
            """;
        File.WriteAllText(Fixture, header + "\n" + string.Join('\n', ExpectedEvents([])) + "\n", new UTF8Encoding(true));
    }

    void Start()
    {
        host = Path.Combine(artifacts, "launch-" + ++launch);
        Directory.CreateDirectory(host);
        stateSequence = captureSequence = 0;
        var start = new ProcessStartInfo(Path.GetFullPath(args[0])) { UseShellExecute = false, WindowStyle = ProcessWindowStyle.Hidden };
        start.Environment["AEGISUB_ENABLE_SKIA_VIDEO_TOOLS"] = "0";
        start.Environment["AEGISUB_ENABLE_DXGI_PRESENTATION"] = "0";
        foreach (var argument in new[] { "--gui-test", "host", "--profile-dir", Profile, "--artifacts", host, "--open", Fixture })
            start.ArgumentList.Add(argument);
        app = desktop.Start(start, "select-font-" + launch);
        AutomationProtocol.WaitForReadyArtifact(Path.Combine(host, "ready.json"), app, TimeSpan.FromSeconds(20));
        main = desktop.MainWindow(app, 10);
        desktop.VerifyWindow(main, app);
        Require(Native.SetWindowPos(main, 0, 20, 20, 1200, 860, 0x54), "Set a repeatable native main-window size");
        Wait(() => Children(main).Any(h => Class(h) == "wxGLCanvas"), "Host opens the black dummy video in a visible VideoBox canvas");
    }

    void NormalExit()
    {
        Post(main, Native.Close);
        Require(app!.WaitForExit(10_000), "WM_CLOSE exits normally within ten seconds");
        Require(desktop.ExitCode(app) == 0, "Private host exits with status zero");
        app.Dispose();
        app = null;
        main = dialog = 0;
    }

    JsonNode ReadConfig() => JsonNode.Parse(File.ReadAllText(Config))!;

    void ExpectStoredFonts(string[] recent, string[] favorites)
    {
        var font = ReadConfig()["Subtitle"]!["Font"]!;
        EqualItems("Saved recent font option", recent, Strings("Recent"));
        EqualItems("Saved favorites option", favorites, Strings("Favorites"));

        string[] Strings(string option) => font[option]!.AsArray().Select(item => item!["string"]!.GetValue<string>()).ToArray();
    }

    void ExpectStoredGrid(bool layer)
    {
        var columns = ReadConfig()["Subtitle"]!["Grid"]!["Column"]!.AsArray();
        Require(columns.Count > 10 && columns[1]!["bool"]!.GetValue<bool>() == layer && !columns[10]!["bool"]!.GetValue<bool>(),
            "Normal exit stores the original Layer flag and the toggled Vertical Margin flag");
    }

    JsonElement Observe(ushort command, string name, int sequence)
    {
        // These two observer commands exist only in --gui-test host mode.
        var path = Path.Combine(host, name + "-" + sequence + ".json");
        Post(main, Native.Command, command);
        JsonDocument? document = null;
        Wait(() =>
        {
            if (!File.Exists(path)) return false;
            try { document = JsonDocument.Parse(File.ReadAllText(path)); return true; }
            catch (IOException error) when ((error.HResult & 0xffff) is 32 or 33) { return false; }
        }, "Host publishes " + Path.GetFileName(path));
        using (document)
        {
            var result = document!.RootElement;
            Require(result.GetProperty("sequence").GetInt32() == sequence, "Observer response matches the owned request: " + name + "/" + sequence);
            return result.Clone();
        }
    }

    JsonElement Document() => Observe(0x7FFD, "visual-state", ++stateSequence);

    VideoSample CaptureVideo(string name)
    {
        for (int attempt = 0; attempt < 10; ++attempt)
        {
            var capture = Observe(0x7FFE, "viewport", ++captureSequence);
            if (capture.TryGetProperty("error", out var error) && error.GetString() == "captured frame is not the current provider delivery")
            {
                Thread.Sleep(30);
                continue;
            }
            Require(capture.GetProperty("captured").GetBoolean() && capture.GetProperty("current").GetBoolean()
                && capture.GetProperty("swapped").GetBoolean() && capture.GetProperty("frame").GetInt32() == 0,
                "VideoBox captured its current rendered and swapped frame 0: " + capture);
            using var image = new Bitmap(Path.Combine(host, "viewport-" + captureSequence + ".png"));
            Require(image.Width >= 320 && image.Height >= 240, "VideoBox canvas is large enough for the subtitle sample");
            var crop = new Rectangle(image.Width / 10, image.Height / 4, image.Width * 8 / 10, image.Height / 2);
            using var region = image.Clone(crop, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
            region.Save(Path.Combine(artifacts, name + ".png"), System.Drawing.Imaging.ImageFormat.Png);
            var rgb = new byte[crop.Width * crop.Height * 3];
            int black = 0, light = 0, index = 0;
            for (int y = 0; y < region.Height; ++y)
            for (int x = 0; x < region.Width; ++x)
            {
                var pixel = region.GetPixel(x, y);
                rgb[index++] = pixel.R;
                rgb[index++] = pixel.G;
                rgb[index++] = pixel.B;
                if (pixel.R <= 12 && pixel.G <= 12 && pixel.B <= 12) ++black;
                if (pixel.R >= 100 && pixel.G >= 100 && pixel.B >= 100) ++light;
            }
            var sample = new VideoSample(image.Width, image.Height, crop, black, light, Convert.ToHexString(SHA256.HashData(rgb)));
            WriteJson(name + "-crop.json", sample);
            Require(black > crop.Width * crop.Height / 2 && light >= 100, "The same central crop contains black video and visible white subtitle glyphs");
            return sample;
        }
        throw new TimeoutException("Ten bounded VideoBox captures did not see the current subtitle delivery: " + name);
    }

    void CompareVideo(VideoSample before, VideoSample after, bool equal)
    {
        Require((before.Width, before.Height, before.Crop) == (after.Width, after.Height, after.Crop), "VideoBox comparison uses the same pixel coordinates");
        Require((before.Hash == after.Hash) == equal, equal ? "VideoBox pixels return exactly to the expected appearance" : "The subtitle edit visibly changes the VideoBox pixels");
    }

    static string[] ExpectedEvents((int Row, string Text)[] changes)
    {
        var results = Seed.Select(row => row.Prefix + row.Text).ToArray();
        foreach (var (row, text) in changes) results[row] = Seed[row].Prefix + text;
        return results;
    }

    void ExpectPreview(string suffix, params (int Row, string Text)[] changes)
    {
        Wait(() => Text(ActiveEdit(suffix)) == changes[0].Text, "Active native edit reaches the literal live preview: " + changes[0].Text);
        ExpectDocument("Every live preview row matches the literal ASS event", changes);
    }

    JsonElement ExpectDocument(string description, params (int Row, string Text)[] changes)
    {
        var document = Document();
        var events = document.GetProperty("rows").EnumerateArray().Select(row => row.GetProperty("event").GetString()!).ToArray();
        EqualItems(description, ExpectedEvents(changes), events);
        return document;
    }

    void AssertSaved(string label, bool initializeHeader = false, params (int Row, string Text)[] changes)
    {
        var path = Path.Combine(artifacts, label + ".ass");
        Command("File", "Save Subtitles as...");
        var save = Window(h => Class(h) == "#32770" && Native.Caption(h).Contains("Save", StringComparison.OrdinalIgnoreCase), "Native Save As");
        var controls = AutomationElement.FromHandle(save).FindAll(TreeScope.Descendants, Condition.TrueCondition).Cast<AutomationElement>().ToArray();
        var edit = (nint)controls.Single(e => e.Current.ControlType == ControlType.Edit && e.Current.AutomationId == "1001").Current.NativeWindowHandle;
        desktop.VerifyWindow(edit, app!);
        Native.SetText(edit, path);
        var button = (nint)controls.Single(e => e.Current.ControlType == ControlType.Button && e.Current.AutomationId == "1" && e.Current.NativeWindowHandle != 0).Current.NativeWindowHandle;
        Click(button);
        Wait(() => File.Exists(path) && !Native.IsWindowVisible(save) && !Native.Caption(main).StartsWith('*'),
            "Native Save As completes " + label + ".ass");
        var lines = File.ReadAllLines(path);
        var expected = ExpectedEvents(changes);
        File.WriteAllLines(Path.Combine(artifacts, label + "-expected-events.ass"), expected, new UTF8Encoding(true));
        EqualItems("Full saved ASS events for " + label, expected,
            lines.Where(line => line.StartsWith("Dialogue:", StringComparison.Ordinal) || line.StartsWith("Comment:", StringComparison.Ordinal)));
        var header = lines.Where(line => line.StartsWith("Style:", StringComparison.Ordinal)
            || line.StartsWith("Original Script:", StringComparison.Ordinal) || line.StartsWith("Title:", StringComparison.Ordinal)).ToArray();
        if (initializeHeader)
        {
            Require(header.Length == 4, "Saved baseline includes both styles and both metadata sentinels");
            savedHeader = header;
        }
        else EqualItems("Saved styles and metadata remain byte-for-byte equal to baseline", savedHeader, header);
    }

    void SelectLines(string actor, params int[] rows)
    {
        Command("Subtitle", "Select Lines...");
        dialog = Window(h => Native.Caption(h) == "Select", "Select Lines dialog");
        TypeText(Children(dialog).Single(h => Class(h) == "Edit"), actor);
        foreach (var name in new[] { "Matches", "Contains", "Actor", "Set selection", "Dialogues" })
            ClickCheck(Control(dialog, name), true);
        ClickCheck(Control(dialog, "Comments"), false);
        CloseDialog("OK");
        var state = Document();
        var selected = state.GetProperty("rows").EnumerateArray().Where(row => row.GetProperty("selected").GetBoolean()).ToArray();
        Require(selected.Select(row => row.GetProperty("source_row").GetInt32()).SequenceEqual(rows), "Actual grid selects only the intended rows: " + string.Join(", ", rows));
        Require(selected[0].GetProperty("source_id").GetInt32() == state.GetProperty("active_id").GetInt32(), "Expected active row anchors the operation");
    }

    nint ActiveEdit(string suffix) => Children(main).Single(h => Class(h) == "Edit" && Text(h).EndsWith(suffix, StringComparison.Ordinal));

    void SetRange(string suffix, int start, int end)
    {
        var edit = ActiveEdit(suffix);
        Send(edit, Native.EditSetSelection, start, end);
        Send(edit, Native.KeyUp, 0x10, unchecked((int)0xC0000001));
        Require(ReadEditRange(edit) == (start, end), $"Native editor selects UTF-16 [{start}, {end}] inside the intended anchor");
    }

    (int Start, int End) ReadRange(string suffix) => ReadEditRange(ActiveEdit(suffix));

    (int Start, int End) ReadEditRange(nint edit)
    {
        var range = Send(edit, Native.EditGetSelection);
        return ((int)(range & 0xFFFF), (int)((range >> 16) & 0xFFFF));
    }

    void OpenFont()
    {
        Click(Control(main, "Font Face"));
        dialog = Window(h => Native.Caption(h) == "Select Font", "Select Font dialog");
        Require(Native.GetWindow(dialog, 4) == main, "Select Font belongs to the isolated main window");
        var font = Native.Bounds(Control(dialog, "Font"));
        fontCombo = Children(dialog).Where(h => Class(h) == "ComboBox" && font.Contains(Native.Bounds(h)))
            .Single(h => Native.ComboChildren(h).Edit is var edit && edit != 0 && Class(edit) == "Edit");
        fontEdit = Native.ComboChildren(fontCombo).Edit;
        var fontRow = Native.Bounds(fontCombo);
        star = Children(dialog).Single(h => Class(h) == "Button" && Native.Bounds(h) is var bounds && font.Contains(bounds)
            && bounds.Left >= fontRow.Right && bounds.Top < fontRow.Bottom && bounds.Bottom > fontRow.Top);
        var numeric = Native.Bounds(Control(dialog, "Scale and Spacing"));
        scaleX = NumericEdit(HorizontalScale);
        scaleY = NumericEdit(VerticalScale);
        spacing = NumericEdit(CharacterSpacing);
        Require(new[] { scaleX, scaleY, spacing }.Distinct().Count() == 3, "Distinct native numeric edits match all three labels");
        var quick = Native.Bounds(Control(dialog, "Quick Font Selection"));
        quickChoice = Children(dialog).Single(h => Class(h) == "ComboBox" && quick.Contains(Native.Bounds(h)));
        quickList = Children(dialog).Single(h => Class(h) == "ListBox" && quick.Contains(Native.Bounds(h)));
        var info = Native.Bounds(Control(dialog, "Selected Font Information"));
        information = Children(dialog).Single(h => info.Contains(Native.Bounds(h)) && Text(h).StartsWith("Selected ASS name:", StringComparison.Ordinal));

        nint NumericEdit(string name)
        {
            var label = Native.Bounds(Control(dialog, name));
            return Children(dialog).Where(h => Class(h) == "Edit" && numeric.Contains(Native.Bounds(h)))
                .Single(h => Native.Bounds(h) is var edit && edit.Top >= label.Bottom && edit.Left < label.Right && edit.Right > label.Left);
        }
    }

    string LocalNumber(string number) => number.Replace(".", decimalSeparator, StringComparison.Ordinal);

    void FocusEdit(nint edit)
    {
        var bounds = Native.ClientBounds(edit);
        ClickPoint(edit, Native.Point(bounds.Width / 2, bounds.Height / 2));
        Wait(() => Native.FocusedWindow(edit) == edit, "Actual edit receives keyboard focus");
    }

    void TypeText(nint edit, string value)
    {
        desktop.VerifyWindow(edit, app!);
        Require(Enabled(edit), "Native edit is enabled for input " + JsonSerializer.Serialize(value));
        // One replacement lets 12. reach wx as an incomplete number without a preceding valid 12 event.
        Native.ReplaceText(edit, value);
        Require(Text(edit) == value, "Exact native edit text reads back " + JsonSerializer.Serialize(value));
    }

    void StepSpacing()
    {
        var edit = Native.Bounds(spacing);
        var spin = Children(dialog).Single(h => Class(h).Equals("msctls_updown32", StringComparison.OrdinalIgnoreCase)
            && Native.Bounds(h) is var rect && Math.Abs(rect.Top - edit.Top) < edit.Height / 2
            && rect.Left >= edit.Right - 4 && rect.Left - edit.Right <= edit.Height);
        var bounds = Native.ClientBounds(spin);
        ClickPoint(spin, Native.Point(bounds.Width / 2, bounds.Height / 4));
        Wait(() => Text(spacing).Trim() == LocalNumber("5.8"), "Upper spin-arrow click increases spacing by one decimal step");
    }

    string[] Candidates() => Items(fontCombo, combo: true);
    string[] QuickItems() => Items(quickList, combo: false);

    string[] Items(nint hwnd, bool combo)
    {
        int count = checked((int)Send(hwnd, combo ? Native.ComboCount : Native.ListCount));
        return Enumerable.Range(0, count).Select(i => Native.ItemText(hwnd, i, combo)).ToArray();
    }

    string QuickPage() => Native.ItemText(quickChoice, checked((int)Send(quickChoice, Native.ComboSelection)), combo: true);

    void ExpectQuickList(string page, string[] names)
    {
        Wait(() => QuickPage() == page && QuickItems().SequenceEqual(names, StringComparer.Ordinal),
            "Quick list updates to " + page + " with the expected entries");
        EqualItems("Actual " + page + " entries", names, QuickItems());
    }

    void ChooseQuickPage(string name)
    {
        if (QuickPage() != name) ChooseComboItem(quickChoice, name);
    }

    void ChooseComboItem(nint combo, string name)
    {
        int index = Array.IndexOf(Items(combo, combo: true), name);
        Require(index >= 0, "Actual native choice contains " + name);
        Send(combo, Native.ComboShow, 1);
        Wait(() => Send(combo, Native.ComboDropped) != 0, "Native choice opens its dropdown");
        var list = Native.ComboChildren(combo).List;
        ClickPoint(list, ItemPoint(list, index));
        Wait(() => Send(combo, Native.ComboDropped) == 0 && Send(combo, Native.ComboSelection) == index,
            "Native choice switches to " + name);
    }

    void ClickQuickItem(string name)
    {
        ClickPoint(quickList, QuickItemPoint(name));
        Wait(() =>
        {
            int selected = checked((int)Send(quickList, Native.ListSelection));
            return selected >= 0 && Native.ItemText(quickList, selected, combo: false) == name;
        }, "Actual quick list selects " + name);
    }

    long QuickItemPoint(string name)
    {
        int index = Array.IndexOf(QuickItems(), name);
        Require(index >= 0, "Actual quick list contains " + name);
        return ItemPoint(quickList, index);
    }

    long ItemPoint(nint list, int index)
    {
        desktop.VerifyWindow(list, app!);
        var rect = Native.ItemBounds(list, index);
        var point = Native.Point(rect.Left + Math.Min(8, rect.Width / 2), rect.Top + rect.Height / 2);
        long hit = Send(list, Native.ListItemFromPoint, 0, point);
        Require(Native.ClientBounds(list).Contains(rect) && hit == index, "Native list item is visible and its click point hits the expected row");
        return point;
    }

    void ExpectEmptyListActions()
    {
        Control(dialog, QuickPage() == "Recent Fonts" ? "No recent fonts" : "No favorite fonts");
        Require(!Enabled(quickList) && !Enabled(Control(dialog, "Remove")) && !Enabled(Control(dialog, "Add to Favorites")),
            "An empty quick list disables both row actions and the list itself");
        if (QuickPage() == "Recent Fonts")
            Require(!Enabled(Control(dialog, "Clear Recent Fonts")), "An empty recent list disables Clear Recent Fonts");
    }

    void ExpectFont(string name, bool? vertical = null)
    {
        string edit = "", description = "";
        try
        {
            Wait(() =>
            {
                edit = Text(fontEdit);
                description = Text(information).Split('\n')[0].TrimEnd('\r');
                return edit == name && description == "Selected ASS name: " + name;
            }, "Font field and visible information both display " + name);
        }
        catch (TimeoutException error)
        {
            throw new InvalidOperationException($"{error.Message}. Font field was '{edit}', information was '{description}'.", error);
        }
        Require(Enabled(Control(dialog, "OK")), "The font dialog can confirm the displayed name");
        if (vertical.HasValue)
        {
            var toggle = Control(dialog, "Vertical");
            Require(Checked(toggle) == vertical.Value, "Compact Vertical checkbox agrees with the exact ASS spelling");
            if (name is Face or VerticalFace) Require(Enabled(toggle), "Microsoft YaHei has an installed vertical GDI face");
        }
    }

    void ExpectFontAfterCallbacks(string name, bool? vertical = null)
    {
        // Allow both the 16 ms preview and the 50 ms matching callback to run.
        Thread.Sleep(200);
        ExpectFont(name, vertical);
        Require(Send(fontCombo, Native.ComboDropped) == 0, "Delayed combo work leaves the refreshed/refilled font dropdown closed");
    }

    void GridMenu(bool layer, bool verticalMargin, bool toggle)
    {
        // The existing folding-column native scenario identifies the grid by its owned scrollbar.
        var scroll = Native.Children(main).Single(h => Class(h) == "ScrollBar" && Native.GetDlgCtrlID(h) == 1730);
        var grid = Native.GetParent(scroll);
        desktop.VerifyWindow(grid, app!);
        var point = Native.ScreenPoint(grid, 8, 4);
        Post(grid, Native.ContextMenu, grid, point);
        var popup = Window(h => Class(h) == "#32768", "native grid column header menu");
        var menu = (nint)Send(popup, Native.PopupMenuHandle);
        var items = Native.MenuItems(menu);
        WriteJson(phase + "-grid-" + launch + (toggle ? "-before.json" : "-observed.json"),
            items.Select(item => new { item.Position, item.Label, item.Checked, item.Enabled }));
        var margin = items.Single(item => item.Label == "Vertical Margin");
        Require(items.Any(item => item.Label == "Folding column") && items.Single(item => item.Label == "Layer").Checked == layer,
            "Grid header opens with the expected existing Layer flag");
        Require(margin.Enabled && margin.Checked == verticalMargin, "Late Vertical Margin option defaults or reloads correctly");
        if (toggle)
        {
            ActivateMenuItem(popup, menu, margin);
            Wait(() => !Windows().Any(h => Class(h) == "#32768"), "The native grid menu processes the toggle");
            GridMenu(layer, !verticalMargin, toggle: false);
        }
        else
        {
            Post(grid, Native.CancelMode);
            Wait(() => !Windows().Any(h => Class(h) == "#32768"), "Native grid popup closes");
        }
    }

    void History(string label, string activeSuffix)
    {
        var edit = ActiveEdit(activeSuffix);
        var previous = Text(edit);
        var menu = Native.MenuItems(Native.GetMenu(main)).Single(item => item.Label == "Edit").Submenu;
        Post(main, Native.SysCommand, 0xF100, 'e');
        var popup = Window(h => Class(h) == "#32768" && (nint)Send(h, Native.PopupMenuHandle) == menu, "actual Edit popup for " + label);
        var item = Native.MenuItems(menu).Single(item => item.Label == label);
        Require(item.Enabled, "Refreshed history item is enabled: " + label);
        ActivateMenuItem(popup, menu, item);
        Wait(() => !Windows().Any(h => Class(h) == "#32768") && Text(edit) != previous,
            "Native history command updates the subtitle after " + label);
    }

    void ActivateMenuItem(nint popup, nint menu, Native.MenuItem item)
    {
        Key(popup, 0x24); // Home
        int count = Native.MenuItems(menu).Length;
        for (int i = 0; i < count && !Native.IsHighlighted(menu, item.Position); ++i)
        {
            Thread.Sleep(35);
            if (!Native.IsHighlighted(menu, item.Position)) Key(popup, 0x28); // Down
        }
        Wait(() => Native.IsHighlighted(menu, item.Position), "Native popup highlights " + item.Label);
        Post(popup, Native.KeyDown, 0x0D, 1); // Enter closes the popup.
    }

    void Key(nint hwnd, long key)
    {
        Post(hwnd, Native.KeyDown, key, 1);
        Post(hwnd, Native.KeyUp, key, unchecked((int)0xC0000001));
    }

    void Command(string menu, string label)
    {
        desktop.VerifyWindow(main, app!);
        Console.WriteLine(menu + " / " + label);
        var id = ObservedWindowsCommand.FindMenuCommand(main, app!.Id, menu, label);
        ObservedWindowsCommand.PostMenuCommand(main, app.Id, id);
    }

    void Click(nint hwnd)
    {
        desktop.VerifyWindow(hwnd, app!);
        Require(Enabled(hwnd), "Observed native button is enabled: " + Text(hwnd));
        ObservedWindowsCommand.PostButtonClick(hwnd, app!.Id);
    }

    void ClickCheck(nint hwnd, bool value)
    {
        if (Checked(hwnd) == value) return;
        Require(Enabled(hwnd), "Observed native check control is enabled: " + Text(hwnd));
        Post(hwnd, Native.ButtonClick);
        Wait(() => Checked(hwnd) == value, "Actual check state changes to " + value + " for " + Text(hwnd));
    }

    bool Checked(nint hwnd) => Send(hwnd, Native.ButtonCheck) == 1;
    bool Enabled(nint hwnd) => Native.IsWindowEnabled(hwnd);

    void ClickPoint(nint hwnd, long point)
    {
        Send(hwnd, Native.LeftDown, 1, point);
        Send(hwnd, Native.LeftUp, 0, point);
    }

    void CloseDialog(string button)
    {
        var old = dialog;
        Click(Control(old, button));
        WaitForDialogClosed(old);
    }

    void WaitForDialogClosed(nint old)
    {
        Wait(() => !Native.IsWindowVisible(old) && Enabled(main), "Actual dialog closes and releases its owner");
        dialog = 0;
    }

    nint[] Children(nint hwnd) => Native.Children(hwnd).Where(Native.IsWindowVisible).ToArray();
    nint[] Windows() => desktop.Windows(app!).Where(Native.IsWindowVisible).ToArray();
    static string Class(nint hwnd) => PrivateDesktop.ClassName(hwnd);
    string Text(nint hwnd) => desktop.ReadText(hwnd, app!);

    nint Control(nint parent, string name)
    {
        var expected = NormalizeLabel(name);
        var matches = Children(parent).Where(h => NormalizeLabel(Text(h)) == expected).ToArray();
        Require(matches.Length == 1, "Found exactly one native control labeled " + name);
        return matches[0];
    }

    static string NormalizeLabel(string value) => Regex.Replace(value.Replace("&", "", StringComparison.Ordinal), @"\s+", " ").Trim();

    nint Window(Func<nint, bool> predicate, string description)
    {
        nint result = 0;
        Wait(() =>
        {
            result = Windows().SingleOrDefault(h => Native.IsWindowVisible(h) && predicate(h));
            return result != 0;
        }, description);
        desktop.VerifyWindow(result, app!);
        return result;
    }

    void Inventory(string label) => WriteJson(label + "-controls.json", Windows().Select(window => new
    {
        title = Native.Caption(window), class_name = Class(window),
        controls = Children(window).Select(h => new { hwnd = $"0x{h:X}", text = Text(h), class_name = Class(h), bounds = Native.Bounds(h), enabled = Enabled(h) })
    }).ToArray());

    void CaptureWindow(string label, nint hwnd)
    {
        desktop.VerifyWindow(hwnd, app!);
        ScreenCapture.SaveWindowPng(hwnd, Path.Combine(artifacts, label + ".png"));
    }

    long Send(nint hwnd, uint message, long first = 0, long second = 0)
    {
        desktop.VerifyWindow(hwnd, app!);
        return Native.Send(hwnd, message, first, second);
    }

    void Post(nint hwnd, uint message, long first = 0, long second = 0)
    {
        desktop.VerifyWindow(hwnd, app!);
        Native.Post(hwnd, message, first, second);
    }

    void WriteJson(string name, object value) => File.WriteAllText(Path.Combine(artifacts, name), JsonSerializer.Serialize(value, Json));

    void Wait(Func<bool> predicate, string description)
    {
        var watch = Stopwatch.StartNew();
        while (watch.Elapsed < TimeSpan.FromSeconds(10))
        {
            desktop.VerifyCurrent();
            if (app is { HasExited: true }) throw new InvalidOperationException("Host exited while waiting for " + description);
            if (predicate()) return;
            Thread.Sleep(25);
        }
        throw new TimeoutException(description);
    }

    void Require(bool condition, string description)
    {
        Console.WriteLine(phase + ": " + description + ": " + (condition ? "PASS" : "FAIL"));
        if (!condition) throw new InvalidOperationException(description);
    }

    void EqualItems(string description, string[] expected, IEnumerable<string> actual)
    {
        var observed = actual.ToArray();
        if (!expected.SequenceEqual(observed, StringComparer.Ordinal))
            throw new InvalidOperationException(description + "\nExpected:\n" + string.Join('\n', expected) + "\nActual:\n" + string.Join('\n', observed));
        Require(true, description);
    }
}

static class Native
{
    public const uint Close = 0x0010, CancelMode = 0x001F, ContextMenu = 0x007B;
    public const uint EditGetSelection = 0x00B0, EditSetSelection = 0x00B1, ButtonCheck = 0x00F0, ButtonClick = 0x00F5;
    public const uint KeyDown = 0x0100, KeyUp = 0x0101, Character = 0x0102, Command = 0x0111, SysCommand = 0x0112;
    public const uint ComboSelection = 0x0147, ComboCount = 0x0146, ComboShow = 0x014F, ComboDropped = 0x0157;
    public const uint ListSelection = 0x0188, ListCount = 0x018B, ListItemFromPoint = 0x01A9, PopupMenuHandle = 0x01E1;
    public const uint MouseMove = 0x0200, LeftDown = 0x0201, LeftUp = 0x0202, LeftDoubleClick = 0x0203;
    const uint AbortIfHungOrDestroyed = 0x22;

    delegate bool WindowCallback(nint hwnd, nint parameter);
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] struct Coordinate { public int X, Y; }
    [StructLayout(LayoutKind.Sequential)] struct ComboInfo
    {
        public int Size;
        public Rect Item, Button;
        public uint State;
        public nint Combo, Edit, List;
    }
    [StructLayout(LayoutKind.Sequential)] struct GuiThreadInfo
    {
        public int Size;
        public uint Flags;
        public nint Active, Focus, Capture, MenuOwner, MoveSize, Caret;
        public Rect CaretBounds;
    }
    public sealed record MenuItem(int Position, string Label, bool Checked, bool Enabled, nint Submenu);

    [DllImport("user32.dll")] public static extern bool IsWindowVisible(nint hwnd);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(nint hwnd);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(nint hwnd);
    [DllImport("user32.dll")] public static extern nint GetParent(nint hwnd);
    [DllImport("user32.dll")] public static extern nint GetWindow(nint hwnd, uint relation);
    [DllImport("user32.dll")] public static extern nint GetMenu(nint hwnd);
    [DllImport("user32.dll")] public static extern nint SetThreadDpiAwarenessContext(nint context);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(nint hwnd, nint after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(nint hwnd, WindowCallback callback, nint data);
    [DllImport("user32.dll")] static extern bool GetWindowRect(nint hwnd, out Rect rect);
    [DllImport("user32.dll")] static extern bool GetClientRect(nint hwnd, out Rect rect);
    [DllImport("user32.dll")] static extern bool ClientToScreen(nint hwnd, ref Coordinate point);
    [DllImport("user32.dll")] static extern bool GetComboBoxInfo(nint hwnd, ref ComboInfo info);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(nint hwnd, out uint pid);
    [DllImport("user32.dll")] static extern bool GetGUIThreadInfo(uint thread, ref GuiThreadInfo info);
    [DllImport("user32.dll", EntryPoint = "GetWindowTextW", CharSet = CharSet.Unicode)] static extern int GetWindowText(nint hwnd, StringBuilder text, int capacity);
    [DllImport("user32.dll")] static extern nint GetSubMenu(nint menu, int position);
    [DllImport("user32.dll")] static extern int GetMenuItemCount(nint menu);
    [DllImport("user32.dll")] static extern uint GetMenuState(nint menu, uint position, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetMenuStringW", CharSet = CharSet.Unicode)] static extern int GetMenuString(nint menu, uint position, StringBuilder text, int capacity, uint flags);
    [DllImport("user32.dll", EntryPoint = "PostMessageW", SetLastError = true)] static extern bool PostMessage(nint hwnd, uint message, nint first, nint second);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", SetLastError = true)] static extern nint Message(nint hwnd, uint message, nint first, nint second, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint WriteMessage(nint hwnd, uint message, nint first, string text, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode)] static extern nint ReadMessage(nint hwnd, uint message, nint first, StringBuilder text, uint flags, uint timeout, out nint result);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW")] static extern nint RectangleMessage(nint hwnd, uint message, nint first, ref Rect rect, uint flags, uint timeout, out nint result);

    public static nint[] Children(nint hwnd)
    {
        List<nint> found = [];
        WindowCallback callback = (window, _) => { found.Add(window); return true; };
        EnumChildWindows(hwnd, callback, 0);
        return found.ToArray();
    }

    public static string Caption(nint hwnd)
    {
        // Cross-process top-level captions are cached by Windows while the host creates controls.
        StringBuilder text = new(512);
        GetWindowText(hwnd, text, text.Capacity);
        return text.ToString();
    }

    public static Rectangle Bounds(nint hwnd) => GetWindowRect(hwnd, out var rect) ? FromRect(rect) : throw new InvalidOperationException("Window bounds unavailable");
    public static Rectangle ClientBounds(nint hwnd) => GetClientRect(hwnd, out var rect) ? FromRect(rect) : throw new InvalidOperationException("Client bounds unavailable");
    static Rectangle FromRect(Rect rect) => Rectangle.FromLTRB(rect.Left, rect.Top, rect.Right, rect.Bottom);

    public static long Point(int x, int y) => unchecked((int)((ushort)x | (uint)(ushort)y << 16));

    public static long ScreenPoint(nint hwnd, int x, int y)
    {
        var point = new Coordinate { X = x, Y = y };
        if (!ClientToScreen(hwnd, ref point)) throw new InvalidOperationException("ClientToScreen failed");
        return Point(point.X, point.Y);
    }

    public static nint FocusedWindow(nint hwnd)
    {
        var thread = GetWindowThreadProcessId(hwnd, out _);
        if (thread == 0) throw new InvalidOperationException("Cannot find owning GUI thread");
        var info = new GuiThreadInfo { Size = Marshal.SizeOf<GuiThreadInfo>() };
        if (!GetGUIThreadInfo(thread, ref info)) throw new InvalidOperationException("Cannot read focus from the owning GUI thread");
        return info.Focus;
    }

    public static (nint Edit, nint List) ComboChildren(nint hwnd)
    {
        ComboInfo info = new() { Size = Marshal.SizeOf<ComboInfo>() };
        if (!GetComboBoxInfo(hwnd, ref info)) throw new InvalidOperationException("Cannot read native ComboBox child HWNDs");
        return (info.Edit, info.List);
    }

    public static string ItemText(nint hwnd, int index, bool combo)
    {
        int length = checked((int)Send(hwnd, combo ? 0x0149u : 0x018Au, index));
        if (length < 0) throw new InvalidOperationException("Native item text length is unavailable");
        StringBuilder text = new(length + 1);
        if (ReadMessage(hwnd, combo ? 0x0148u : 0x0189u, index, text, AbortIfHungOrDestroyed, 1000, out var copied) == 0 || copied < 0)
            throw new InvalidOperationException("Cannot read native item text");
        return text.ToString();
    }

    public static Rectangle ItemBounds(nint hwnd, int index)
    {
        Rect rect = default;
        if (RectangleMessage(hwnd, 0x0198, index, ref rect, AbortIfHungOrDestroyed, 1000, out var result) == 0 || result < 0)
            throw new InvalidOperationException("Cannot read native list item rectangle");
        return FromRect(rect);
    }

    public static MenuItem[] MenuItems(nint menu)
    {
        int count = GetMenuItemCount(menu);
        if (count < 0) throw new InvalidOperationException("Observed menu is no longer available");
        return Enumerable.Range(0, count).Select(i =>
        {
            StringBuilder text = new(256);
            GetMenuString(menu, (uint)i, text, text.Capacity, 0x400);
            var state = GetMenuState(menu, (uint)i, 0x400);
            return new MenuItem(i, text.ToString().Split('\t')[0].Replace("&", "", StringComparison.Ordinal), (state & 8) != 0, (state & 3) == 0, GetSubMenu(menu, i));
        }).ToArray();
    }

    public static bool IsHighlighted(nint menu, int position) => (GetMenuState(menu, (uint)position, 0x400) & 0x80) != 0;

    public static void SetText(nint hwnd, string text) => TextMessage(hwnd, 0x000C, 0, text);

    public static void ReplaceText(nint hwnd, string text)
    {
        Send(hwnd, EditSetSelection, 0, -1);
        TextMessage(hwnd, 0x00C2, 1, text);
    }

    static void TextMessage(nint hwnd, uint message, nint first, string text)
    {
        if (WriteMessage(hwnd, message, first, text, AbortIfHungOrDestroyed, 1000, out _) == 0)
            throw new TimeoutException($"Native text message 0x{message:X} timed out");
    }

    public static long Send(nint hwnd, uint message, long first = 0, long second = 0)
    {
        if (Message(hwnd, message, (nint)first, (nint)second, AbortIfHungOrDestroyed, 1000, out var result) == 0)
            throw new TimeoutException($"Native message 0x{message:X} failed (Win32 {Marshal.GetLastPInvokeError()})");
        return result.ToInt64();
    }

    public static void Post(nint hwnd, uint message, long first, long second)
    {
        if (!PostMessage(hwnd, message, (nint)first, (nint)second))
            throw new InvalidOperationException($"Native post 0x{message:X} failed (Win32 {Marshal.GetLastPInvokeError()})");
    }
}
