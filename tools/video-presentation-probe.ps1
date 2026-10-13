#requires -Version 7.0
[CmdletBinding()]
param(
    [string] $Executable = 'build-dir/RelWithDebInfo/Aegisub-dxgi-preview.exe',
    [Parameter(Mandatory)] [string] $Artifacts,
    [Parameter(Mandatory)] [string] $Ffmpeg,
    [ValidateSet('Dxgi', 'Native')] [string] $Presentation = 'Dxgi',
    [switch] $WithAudio,
    [string] $SourceVideo,
    [double] $StartSeconds = 0
)
$ErrorActionPreference = 'Stop'
$output = [IO.Path]::GetFullPath($Artifacts, (Get-Location).Path)
if ((Test-Path $output) -and (Get-ChildItem $output -Force | Select-Object -First 1)) {
    throw 'Artifacts must be a new empty directory.'
}
New-Item -ItemType Directory -Force "$output/profile/user", "$output/host" | Out-Null
$exe = (Resolve-Path $Executable).Path
$encoder = (Resolve-Path $Ffmpeg).Path
$media = "$output/fixture.mp4"
$encodeArguments = @('-hide_banner', '-loglevel', 'error')
if ($SourceVideo) {
    $source = (Resolve-Path -LiteralPath $SourceVideo).Path
    $encodeArguments += @('-ss', $StartSeconds.ToString([Globalization.CultureInfo]::InvariantCulture), '-i', $source, '-t', '8', '-map', '0:v:0', '-map', '0:a:0?', '-c', 'copy', '-y', $media)
} else {
    $encodeArguments += @('-f', 'lavfi', '-i', 'testsrc2=size=1920x1080:rate=24', '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000', '-t', '8', '-c:a', 'aac', '-c:v', 'libx265', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p10le', '-x265-params', 'pools=4:frame-threads=2', '-y', $media)
}
$encodeInfo = [Diagnostics.ProcessStartInfo]::new($encoder)
$encodeInfo.UseShellExecute = $false
$encodeInfo.CreateNoWindow = $true
$encodeInfo.RedirectStandardOutput = $true
$encodeInfo.RedirectStandardError = $true
foreach ($argument in $encodeArguments) { $encodeInfo.ArgumentList.Add($argument) }
$encoding = [Diagnostics.Process]::Start($encodeInfo)
$encoderOutput = $encoding.StandardOutput.ReadToEndAsync()
$encoderError = $encoding.StandardError.ReadToEndAsync()
if (!$encoding.WaitForExit(45000)) { $encoding.Kill(); throw 'Fixture preparation exceeded its 45-second timeout.' }
$encoderOutput.GetAwaiter().GetResult() | Set-Content "$output/encoder.stdout"
$encoderError.GetAwaiter().GetResult() | Set-Content "$output/encoder.stderr"
if ($encoding.ExitCode -ne 0) { throw 'Fixture preparation failed.' }
@{
    App = @{ Language = 'en'; Auto = @{ 'Check For Updates' = $false; 'Load Linked Files' = 0; 'Save on Every Change' = $false } }
    Subtitle = @{ Provider = 'libass'; Grid = @{ Skia = @{ Enabled = $false } } }
    Audio = @{ Provider = 'LsmasNative'; Spectrum = $true; Display = @{ Skia = @{ Enabled = [bool]$WithAudio } } }
    Video = @{ Provider = 'LsmasNative'; 'Open Audio' = [bool]$WithAudio; 'Default Zoom' = 3; 'Scale with DPI' = $false; Renderer = @{ Backend = 'libplacebo' }; 'Skia Tools' = @{ Enabled = $false } }
} | ConvertTo-Json -Depth 8 | Set-Content "$output/profile/user/config.json" -Encoding utf8NoBOM
@'
[Script Info]
Title: Video presentation probe
ScriptType: v4.00+
PlayResX: 1920
PlayResY: 1080
LayoutResX: 1920
LayoutResY: 1080

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,64,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,3,0,2,40,40,40,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
Dialogue: 0,0:00:00.00,0:00:08.00,Default,,0,0,0,,Presentation probe: HEVC 10-bit + libplacebo + libass
Dialogue: 1,0:00:00.00,0:00:08.00,Default,,0,0,0,,{\an5\move(200,300,1700,700)\c&H00FFFF&}Moving subtitle overlay
'@ | Set-Content "$output/fixture.ass" -Encoding utf8NoBOM

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class VideoPresentationProbe {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr data);
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr data);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr window, StringBuilder text, int count);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr SendMessageTimeout(IntPtr window, uint message, IntPtr wparam, IntPtr lparam, uint flags, uint timeout, out IntPtr result);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("user32.dll")] static extern IntPtr GetMenu(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr GetSubMenu(IntPtr menu, int index);
    [DllImport("user32.dll")] static extern int GetMenuItemCount(IntPtr menu);
    [DllImport("user32.dll")] static extern uint GetMenuItemID(IntPtr menu, int index);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetMenuString(IntPtr menu, uint index, StringBuilder text, int count, uint flags);
    public static IntPtr[] Windows(int process) {
        var result = new List<IntPtr>();
        EnumWindows((window, data) => { uint owner; GetWindowThreadProcessId(window,out owner); if(owner==process) result.Add(window); return true; }, IntPtr.Zero);
        return result.ToArray();
    }
    public static int Area(IntPtr window) { Rect r; GetWindowRect(window,out r); return (r.Right-r.Left)*(r.Bottom-r.Top); }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr window, StringBuilder text, int count);
    public static string Title(IntPtr window) {var text=new StringBuilder(512); GetWindowText(window,text,512); return text.ToString();}
    public static IntPtr VideoToolbar(IntPtr parent) {
        IntPtr found=IntPtr.Zero;
        var boxes=new HashSet<IntPtr>();
        EnumChildWindows(parent,(window,data)=>{
            var name=new StringBuilder(128); GetClassName(window,name,128);
            if(name.ToString()=="wxGLCanvas") boxes.Add(GetParent(window));
            return true;
        },IntPtr.Zero);
        EnumChildWindows(parent,(window,data)=>{
            var name=new StringBuilder(128); GetClassName(window,name,128);
            if(name.ToString()=="ToolbarWindow32" && boxes.Contains(GetParent(window))) {
                IntPtr count;
                Rect bounds; GetWindowRect(window,out bounds);
                if(SendMessageTimeout(window,0x418,IntPtr.Zero,IntPtr.Zero,2,500,out count)!=IntPtr.Zero && count.ToInt64()>=3 && count.ToInt64()<=6 && bounds.Right-bounds.Left>bounds.Bottom-bounds.Top) found=window;
            }
            return true;
        },IntPtr.Zero);
        if(found==IntPtr.Zero) throw new InvalidOperationException("Video toolbar was not found");
        return found;
    }
    public static void Playback(IntPtr window, bool play) {
        var toolbar=VideoToolbar(window);
        if(!PostMessage(GetParent(toolbar),0x111,new IntPtr(play?5000:5002),toolbar)) throw new InvalidOperationException("Playback command failed");
    }
    static uint FindMenu(IntPtr menu, string expected) {
        for(int i=0;i<GetMenuItemCount(menu);i++) {
            var text=new StringBuilder(256); GetMenuString(menu,(uint)i,text,256,0x400);
            if(text.ToString().Replace("&","").Split('\t')[0]==expected) return GetMenuItemID(menu,i);
            var sub=GetSubMenu(menu,i);
            if(sub!=IntPtr.Zero) {var id=FindMenu(sub,expected); if(id!=UInt32.MaxValue) return id;}
        }
        return UInt32.MaxValue;
    }
    public static void Detach(IntPtr window) {
        uint id=FindMenu(GetMenu(window),"Detach Video");
        if(id==UInt32.MaxValue || !PostMessage(window,0x111,new IntPtr(id),IntPtr.Zero)) throw new InvalidOperationException("Detach command failed");
    }
    public static void Save(IntPtr window) {
        uint id=FindMenu(GetMenu(window),"Save Subtitles");
        if(id==UInt32.MaxValue || !PostMessage(window,0x111,new IntPtr(id),IntPtr.Zero)) throw new InvalidOperationException("Save fixture command failed");
    }
    public static void Close(IntPtr window) {PostMessage(window,0x10,IntPtr.Zero,IntPtr.Zero);}
}
'@

$savedTrace = $env:AEGISUB_PERF_TRACE
$savedPresentation = $env:AEGISUB_ENABLE_DXGI_PRESENTATION
$oldDpi = [VideoPresentationProbe]::SetThreadDpiAwarenessContext([IntPtr]::new(-4))
$app = $null
$watch = [Diagnostics.Stopwatch]::StartNew()
function Assert-Time {
    if ($watch.Elapsed.TotalSeconds -gt 40) { throw 'GUI presentation probe exceeded its 40-second timeout.' }
    if ($app.HasExited) { throw 'GUI presentation probe exited unexpectedly.' }
}
function Wait-Probe([int]$milliseconds) { Assert-Time; Start-Sleep -Milliseconds $milliseconds; Assert-Time }
function Capture-Probe([IntPtr]$window, [string]$name) {
    $rect = [VideoPresentationProbe+Rect]::new()
    if (![VideoPresentationProbe]::GetWindowRect($window, [ref]$rect)) { throw 'Window bounds query failed.' }
    $bitmap = [Drawing.Bitmap]::new($rect.Right-$rect.Left, $rect.Bottom-$rect.Top)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
        $bitmap.Save("$output/$name.png", [Drawing.Imaging.ImageFormat]::Png)
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
try {
    $env:AEGISUB_PERF_TRACE = 'video,audio,log'
    $env:AEGISUB_ENABLE_DXGI_PRESENTATION = if ($Presentation -eq 'Dxgi') { '1' } else { '0' }
    $app = Start-Process $exe -ArgumentList @('gui-test','host','--profile-dir',"$output/profile",'--artifacts',"$output/host",'--open',"$output/fixture.ass",'--open',$media) -WindowStyle Normal -PassThru
    while (!(Test-Path "$output/host/ready.json")) { Wait-Probe 100 }
    $main = [VideoPresentationProbe]::Windows($app.Id) | Where-Object {[VideoPresentationProbe]::Title($_).Contains('fixture.ass')} | Sort-Object { [VideoPresentationProbe]::Area($_) } -Descending | Select-Object -First 1
    if (!$main) { throw 'Main GUI window was not found.' }
    [VideoPresentationProbe]::ShowWindow($main, 5) | Out-Null
    [VideoPresentationProbe]::SetWindowPos($main, [IntPtr]::Zero, 30, 30, 1600, 1100, 0x40) | Out-Null
    [VideoPresentationProbe]::SetForegroundWindow($main) | Out-Null
    Wait-Probe 200
    $toolbarReady = $false
    for ($attempt=0; $attempt -lt 50 -and !$toolbarReady; ++$attempt) {
        try { [VideoPresentationProbe]::VideoToolbar($main) | Out-Null; $toolbarReady = $true }
        catch { Wait-Probe 100 }
    }
    if (!$toolbarReady) { throw 'Video toolbar did not become ready within five seconds.' }
    [VideoPresentationProbe]::Playback($main,$true)
    Wait-Probe 2200
    Capture-Probe $main 'attached-playing'
    [VideoPresentationProbe]::Playback($main,$false)
    Wait-Probe 100
    [VideoPresentationProbe]::Detach($main)
    Wait-Probe 300
    $detached = [VideoPresentationProbe]::Windows($app.Id) | Where-Object {[VideoPresentationProbe]::Title($_).Contains('fixture.mp4')} | Sort-Object { [VideoPresentationProbe]::Area($_) } -Descending | Select-Object -First 1
    if (!$detached) { throw 'Detached video window was not found.' }
    [VideoPresentationProbe]::SetWindowPos($detached, [IntPtr]::Zero, 80, 80, 1100, 800, 0x40) | Out-Null
    [VideoPresentationProbe]::SetForegroundWindow($detached) | Out-Null
    [VideoPresentationProbe]::Playback($detached,$true)
    Wait-Probe 2000
    Capture-Probe $detached 'detached-playing'
    [VideoPresentationProbe]::Playback($detached,$false)
    Wait-Probe 100
    [VideoPresentationProbe]::Detach($main)
    Wait-Probe 300
    [VideoPresentationProbe]::SetForegroundWindow($main) | Out-Null
    [VideoPresentationProbe]::Playback($main,$true)
    Wait-Probe 1600
    [VideoPresentationProbe]::Playback($main,$false)
    Wait-Probe 100
    Capture-Probe $main 'reattached-paused'
    [VideoPresentationProbe]::Save($main)
    Wait-Probe 200
    [VideoPresentationProbe]::Close($main)
    if (!$app.WaitForExit(5000)) { throw 'GUI presentation probe did not exit within five seconds.' }
    if ($app.ExitCode -ne 0) { throw 'GUI presentation probe exited with an error.' }
    $backend = @(); $audioBackend = @(); $frames = @(); $warnings = @()
    foreach ($trace in Get-ChildItem "$output/profile/user/perf-sessions" -Recurse -Filter trace.ndjson) {
        foreach ($line in [IO.File]::ReadLines($trace.FullName)) {
            $entry = $line | ConvertFrom-Json
            if ($entry.payload.phase -eq 'audio_display.presentation_backend') { $audioBackend += $entry.payload.detail_a }
            if ($entry.name -eq 'video_ui_duration' -and $entry.payload.phase -eq 'video_display.presentation_backend') { $backend += $entry.payload.detail_a }
            if ($entry.name -eq 'video_ui_duration' -and $entry.payload.phase -eq 'video_display.render' -and $entry.payload.detail_a -eq 1) { $frames += $entry.payload.duration_ms }
            if ($entry.kind -eq 'log' -and $entry.name -in @('video/display/presentation','audio/display/presentation') -and $entry.payload.severity -eq 'warning') { $warnings += $entry.payload.message }
        }
    }
    $expected = if ($Presentation -eq 'Dxgi') { 1 } else { 0 }
    if ($backend.Count -lt 3 -or @($backend | Where-Object { $_ -ne $expected }).Count) { throw 'Expected presentation backend was not used across attach/detach.' }
    if ($WithAudio -and ($audioBackend.Count -lt 1 -or @($audioBackend | Where-Object { $_ -ne $expected }).Count)) { throw 'Expected audio presentation backend was not used.' }
    if ($frames.Count -lt 48) { throw 'Fewer than 48 new video frames were rendered during playback.' }
    if ($warnings.Count) { throw 'Presentation fallback warnings occurred.' }
    [ordered]@{status='passed';presentation=$Presentation;source_kind=$(if ($SourceVideo) {'remuxed excerpt'} else {'generated fixture'});backend_observations=$backend;audio_backend_observations=$audioBackend;audio_enabled=[bool]$WithAudio;rendered_new_frames=$frames.Count;render_max_ms=($frames | Measure-Object -Maximum).Maximum;gui_elapsed_ms=$watch.Elapsed.TotalMilliseconds;executable_sha256=(Get-FileHash $exe -Algorithm SHA256).Hash;visual_validation='screenshots require inspection'} | ConvertTo-Json -Depth 6 | Set-Content "$output/result.json"
} catch {
    if ($app -and !$app.HasExited) {
        $windows = [VideoPresentationProbe]::Windows($app.Id) | Where-Object {[VideoPresentationProbe]::Title($_).Contains('fixture.ass') -or [VideoPresentationProbe]::Title($_).Contains('fixture.mp4')}
        $windows | ForEach-Object {[ordered]@{title=[VideoPresentationProbe]::Title($_);area=[VideoPresentationProbe]::Area($_)}} | ConvertTo-Json | Set-Content "$output/failure-windows.json"
        $index = 0
        foreach ($window in $windows) {
            [VideoPresentationProbe]::ShowWindow($window,5) | Out-Null
            [VideoPresentationProbe]::SetForegroundWindow($window) | Out-Null
            Start-Sleep -Milliseconds 100
            Capture-Probe $window "failure-window-$index"
            ++$index
        }
    }
    throw
} finally {
    if ($app -and !$app.HasExited) { $app.Kill(); $app.WaitForExit(5000) | Out-Null }
    [VideoPresentationProbe]::SetThreadDpiAwarenessContext($oldDpi) | Out-Null
    $env:AEGISUB_PERF_TRACE = $savedTrace
    $env:AEGISUB_ENABLE_DXGI_PRESENTATION = $savedPresentation
}
