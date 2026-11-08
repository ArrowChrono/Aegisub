#requires -Version 7.0
[CmdletBinding()]
param(
    [string] $Executable = 'build-dir/RelWithDebInfo/Aegisub.exe',
    [Parameter(Mandatory)] [string] $Artifacts,
    [ValidateSet('Empty', 'Video', 'Waveform', 'Spectrum')] [string] $Mode = 'Empty',
    [ValidateSet('Width', 'Height', 'Both')] [string] $Axis = 'Width',
    [ValidateSet('Short', 'Long', 'Ass')] [string] $TextCase = 'Short',
    [ValidateRange(800, 2400)] [int] $BaseWidth = 1280,
    [ValidateRange(1, 1000)] [int] $RowCount = 100,
    [switch] $ScrollGridPage,
    [switch] $ShowOriginal,
    [switch] $PlainTextEditor,
    [ValidateRange(1, 600)] [int] $Steps = 120,
    [ValidateRange(10, 100)] [int] $IntervalMs = 20,
    [ValidateRange(15, 180)] [int] $TimeoutSeconds = 90,
    [switch] $DisableSceneCache,
    [switch] $WithoutTrace,
    [switch] $DisableSystemCompositing,
    [switch] $DisableCompositingAtCreation,
    [switch] $DetachedVideo,
    [switch] $Diagnostics,
    [switch] $CaptureSurface,
    [switch] $VerifyGridSurface,
    [switch] $VerifyLayoutTransition,
    [switch] $AssertContainerPaintPolicy,
    [switch] $AssertEditorPaintPolicy,
    [switch] $VerifyVideoRepaint,
    [switch] $AssertNoRedundantVideoRender
)

$ErrorActionPreference = 'Stop'
if ($AssertNoRedundantVideoRender -and ($Mode -ne 'Video' -or $WithoutTrace)) {
    throw 'The redundant-render assertion requires Video mode with tracing.'
}
if ($DetachedVideo -and ($Mode -ne 'Video' -or $AssertNoRedundantVideoRender -or $WithoutTrace)) {
    throw 'DetachedVideo requires traced Video mode without the unchanged-canvas assertion.'
}
if ($AssertContainerPaintPolicy -and ($DetachedVideo -or $DisableCompositingAtCreation -or $DisableSystemCompositing)) {
    throw 'The container-policy assertion requires the main window without compositing ablations.'
}
if ($VerifyVideoRepaint -and ($Mode -ne 'Video' -or $WithoutTrace)) {
    throw 'The repaint verification requires Video mode with tracing.'
}
if ($VerifyGridSurface -and $DetachedVideo) { throw 'Grid surface verification requires the main window.' }
if ($VerifyLayoutTransition -and (!$Diagnostics -or $DetachedVideo)) { throw 'Layout transition verification requires main-window diagnostics.' }
$repo = (Get-Location).Path
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$outputPath = [IO.Path]::GetFullPath($Artifacts, $repo)
if (Test-Path -LiteralPath $outputPath) {
    if (Get-ChildItem -LiteralPath $outputPath -Force | Select-Object -First 1) {
        throw 'Artifacts must be empty; use a new directory for each run.'
    }
}
$profile = Join-Path $outputPath 'profile'
$hostOutput = Join-Path $outputPath 'host'
New-Item -ItemType Directory -Path (Join-Path $profile 'user'), $hostOutput -Force | Out-Null

# The test desktop is never switched to the input desktop. All window operations
# target the process created here; no global input or focus APIs are used.
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Win32.SafeHandles;

public sealed class ResizeProbeDesktop : IDisposable {
    sealed class DesktopHandle : SafeHandleZeroOrMinusOneIsInvalid {
        public DesktopHandle() : base(true) { }
        protected override bool ReleaseHandle() { return CloseDesktop(handle) != 0; }
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct StartupInfo {
        public uint Size;
        public string Reserved, Desktop, Title;
        public uint X, Y, Width, Height, CharsX, CharsY, Fill, Flags;
        public ushort ShowWindow, ReservedSize;
        public IntPtr ReservedData, Input, Output, Error;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct ProcessInfo { public IntPtr Process, Thread; public uint ProcessId, ThreadId; }
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)]
    struct ScrollInfo {
        public uint Size, Mask;
        public int Min, Max;
        public uint Page;
        public int Position, TrackPosition;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct BitmapInfo {
        public uint Size;
        public int Width, Height;
        public ushort Planes, BitCount;
        public uint Compression, SizeImage;
        public int XPelsPerMeter, YPelsPerMeter;
        public uint ClrUsed, ClrImportant;
    }
    public sealed class WindowInfo {
        public long Handle { get; set; }
        public string Class { get; set; }
        public string Text { get; set; }
        public long Style { get; set; }
        public long ExtendedStyle { get; set; }
        public long ClassStyle { get; set; }
        public Rect Bounds { get; set; }
        public long Parent { get; set; }
        public bool Visible { get; set; }
        public Rect PendingPaint { get; set; }
    }
    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    delegate int EnumWindowProc(IntPtr window, IntPtr argument);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    static extern DesktopHandle CreateDesktopW(string name, IntPtr device, IntPtr mode, uint flags, uint access, IntPtr security);
    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)] static extern int SetThreadDesktop(DesktopHandle desktop);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int CloseDesktop(IntPtr desktop);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    static extern int CreateProcessW(string application, [In, Out] char[] commandLine, IntPtr processSecurity, IntPtr threadSecurity,
        int inheritHandles, uint flags, IntPtr environment, string directory, ref StartupInfo startup, out ProcessInfo process);
    [DllImport("kernel32.dll", ExactSpelling = true)] static extern uint WaitForSingleObject(SafeWaitHandle handle, uint milliseconds);
    [DllImport("kernel32.dll", ExactSpelling = true, SetLastError = true)] static extern int GetExitCodeProcess(SafeWaitHandle process, out uint exitCode);
    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)]
    static extern int EnumDesktopWindows(DesktopHandle desktop, EnumWindowProc callback, IntPtr argument);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int EnumChildWindows(IntPtr window, EnumWindowProc callback, IntPtr argument);
    [DllImport("user32.dll", ExactSpelling = true)] static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int IsWindowVisible(IntPtr window);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll", ExactSpelling = true)] static extern IntPtr GetParent(IntPtr window);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int GetUpdateRect(IntPtr window, out Rect rect, int erase);
    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)] static extern int GetScrollInfo(IntPtr window, int bar, ref ScrollInfo info);
    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)] static extern int InvalidateRect(IntPtr window, IntPtr rect, int erase);
    [DllImport("user32.dll", ExactSpelling = true)] static extern IntPtr GetDC(IntPtr window);
    [DllImport("user32.dll", ExactSpelling = true)] static extern int ReleaseDC(IntPtr window, IntPtr dc);
    [DllImport("user32.dll", ExactSpelling = true)] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
    [DllImport("gdi32.dll", ExactSpelling = true)] static extern IntPtr CreateCompatibleDC(IntPtr dc);
    [DllImport("gdi32.dll", ExactSpelling = true)] static extern int DeleteDC(IntPtr dc);
    [DllImport("gdi32.dll", ExactSpelling = true)] static extern int DeleteObject(IntPtr value);
    [DllImport("gdi32.dll", ExactSpelling = true)] static extern int GdiFlush();
    [DllImport("gdi32.dll", ExactSpelling = true)] static extern IntPtr SelectObject(IntPtr dc, IntPtr value);
    [DllImport("gdi32.dll", ExactSpelling = true)] static extern IntPtr CreateDIBSection(IntPtr dc, ref BitmapInfo info, uint usage, out IntPtr bits, IntPtr section, uint offset);
    [DllImport("gdi32.dll", ExactSpelling = true)] static extern int BitBlt(IntPtr destination, int x, int y, int width, int height, IntPtr source, int sourceX, int sourceY, uint operation);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)] static extern int GetClassNameW(IntPtr window, [Out] char[] name, int count);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)] static extern int GetWindowTextW(IntPtr window, [Out] char[] text, int count);
    [DllImport("user32.dll", ExactSpelling = true)] static extern IntPtr GetWindowLongPtrW(IntPtr window, int index);
    [DllImport("user32.dll", ExactSpelling = true)] static extern UIntPtr GetClassLongPtrW(IntPtr window, int index);
    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)] static extern IntPtr SetWindowLongPtrW(IntPtr window, int index, IntPtr value);
    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)] static extern int SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)] static extern int PostMessageW(IntPtr window, uint message, UIntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll", ExactSpelling = true, SetLastError = true)] static extern IntPtr SendMessageTimeoutW(IntPtr window, uint message, UIntPtr wparam, IntPtr lparam, uint flags, uint timeout, out UIntPtr result);

    readonly DesktopHandle desktop;
    readonly SafeWaitHandle processHandle;
    public Process Process { get; private set; }
    public IntPtr MainWindow { get; private set; }
    IntPtr resizeWindow;
    public ResizeProbeDesktop(string application, string[] arguments, string directory) {
        string name = "AegisubResizeProbe_" + Guid.NewGuid().ToString("N");
        desktop = CreateDesktopW(name, IntPtr.Zero, IntPtr.Zero, 0, 0x00ff, IntPtr.Zero);
        if (desktop.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateDesktop failed");
        try {
            var command = new List<string> { Quote(application) };
            foreach (string argument in arguments) command.Add(Quote(argument));
            var startup = new StartupInfo { Size = (uint)Marshal.SizeOf<StartupInfo>(), Desktop = name, Flags = 1, ShowWindow = 4 };
            ProcessInfo info;
            if (CreateProcessW(application, (string.Join(" ", command) + "\0").ToCharArray(), IntPtr.Zero, IntPtr.Zero,
                    0, 0, IntPtr.Zero, directory, ref startup, out info) == 0)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "CreateProcess failed");
            processHandle = new SafeWaitHandle(info.Process, true);
            using (var threadHandle = new SafeWaitHandle(info.Thread, true))
                Process = Process.GetProcessById((int)info.ProcessId);
        } catch { if (processHandle != null) processHandle.Dispose(); desktop.Dispose(); throw; }
    }
    static string Quote(string value) {
        var quoted = new System.Text.StringBuilder("\"");
        int slashes = 0;
        foreach (char ch in value) {
            if (ch == '\\') { ++slashes; continue; }
            quoted.Append('\\', ch == '"' ? slashes * 2 + 1 : slashes);
            quoted.Append(ch);
            slashes = 0;
        }
        quoted.Append('\\', slashes * 2);
        return quoted.Append('"').ToString();
    }
    public bool FindMainWindow() {
        IntPtr found = IntPtr.Zero;
        long largest = 0;
        EnumWindowProc callback = (window, argument) => {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            Rect bounds;
            if (processId == Process.Id && IsWindowVisible(window) != 0 && GetWindowRect(window, out bounds) != 0) {
                long area = (long)(bounds.Right - bounds.Left) * (bounds.Bottom - bounds.Top);
                if (area > largest) { found = window; largest = area; }
            }
            return 1;
        };
        if (EnumDesktopWindows(desktop, callback, IntPtr.Zero) == 0)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "EnumDesktopWindows failed");
        GC.KeepAlive(callback);
        MainWindow = found;
        resizeWindow = found;
        return found != IntPtr.Zero;
    }
    public void SelectDetachedVideo() {
        IntPtr found = IntPtr.Zero;
        EnumWindowProc callback = (window, argument) => {
            uint processId;
            GetWindowThreadProcessId(window, out processId);
            var text = new char[256];
            if (processId == Process.Id && IsWindowVisible(window) != 0
                && new string(text, 0, GetWindowTextW(window, text, text.Length)).Contains("fixture.y4m", StringComparison.Ordinal))
                found = window;
            return 1;
        };
        EnumDesktopWindows(desktop, callback, IntPtr.Zero);
        GC.KeepAlive(callback);
        if (found == IntPtr.Zero) throw new InvalidOperationException("Detached video window was not created");
        resizeWindow = found;
    }
    public WindowInfo[] Windows() {
        var windows = new List<WindowInfo>();
        EnumWindowProc callback = (window, argument) => {
            var className = new char[256];
            var text = new char[256];
            Rect bounds;
            Rect pendingPaint;
            GetWindowRect(window, out bounds);
            GetUpdateRect(window, out pendingPaint, 0);
            windows.Add(new WindowInfo {
                Handle = window.ToInt64(), Class = new string(className, 0, GetClassNameW(window, className, className.Length)),
                Text = new string(text, 0, GetWindowTextW(window, text, text.Length)), Bounds = bounds,
                Style = GetWindowLongPtrW(window, -16).ToInt64(), ExtendedStyle = GetWindowLongPtrW(window, -20).ToInt64(),
                ClassStyle = (long)GetClassLongPtrW(window, -26).ToUInt64(),
                Parent = GetParent(window).ToInt64(), Visible = IsWindowVisible(window) != 0, PendingPaint = pendingPaint
            });
            return 1;
        };
        callback(resizeWindow, IntPtr.Zero);
        EnumChildWindows(resizeWindow, callback, IntPtr.Zero);
        GC.KeepAlive(callback);
        return windows.ToArray();
    }
    public int Capture(string path) {
        return CaptureWindow(path, resizeWindow);
    }
    int CaptureWindow(string path, IntPtr window) {
        int colors = 0;
        Exception failure = null;
        var worker = new Thread(() => {
            try {
                if (SetThreadDesktop(desktop) == 0) throw new Win32Exception(Marshal.GetLastWin32Error(), "SetThreadDesktop failed");
                colors = CaptureOnDesktop(path, window);
            } catch (Exception error) { failure = error; }
        }) { IsBackground = true };
        worker.Start();
        if (!worker.Join(5000)) throw new TimeoutException("Surface capture exceeded five seconds");
        if (failure != null) throw new InvalidOperationException(failure.Message, failure);
        return colors;
    }
    int CaptureOnDesktop(string path, IntPtr window) {
        // Copy only this private-desktop window's existing surface. Printing a
        // window would repaint it and could conceal the stale pixels under test.
        IntPtr previousDpi = SetThreadDpiAwarenessContext(new IntPtr(-4));
        IntPtr source = IntPtr.Zero, destination = IntPtr.Zero, bitmap = IntPtr.Zero, previousBitmap = IntPtr.Zero;
        try {
            Rect bounds;
            if (GetClientRect(window, out bounds) == 0) throw new InvalidOperationException("GetClientRect failed");
            int width = bounds.Right, height = bounds.Bottom;
            int bytes = checked(width * height * 4);
            if (width <= 0 || height <= 0 || bytes > 128 * 1024 * 1024) throw new InvalidOperationException("Invalid capture dimensions");
            source = GetDC(window);
            if (source == IntPtr.Zero) throw new InvalidOperationException("GetDC failed");
            destination = CreateCompatibleDC(source);
            if (destination == IntPtr.Zero) throw new InvalidOperationException("CreateCompatibleDC failed");
            var info = new BitmapInfo { Size = (uint)Marshal.SizeOf<BitmapInfo>(), Width = width, Height = -height, Planes = 1, BitCount = 32 };
            IntPtr pixels;
            bitmap = CreateDIBSection(source, ref info, 0, out pixels, IntPtr.Zero, 0);
            if (bitmap == IntPtr.Zero) throw new InvalidOperationException("CreateDIBSection failed");
            previousBitmap = SelectObject(destination, bitmap);
            if (previousBitmap == IntPtr.Zero || previousBitmap == new IntPtr(-1)) throw new InvalidOperationException("SelectObject failed");
            var data = new byte[bytes];
            // A known sentinel distinguishes unavailable pixels from real black.
            for (int i = 0; i < bytes; i += 4) { data[i] = 255; data[i + 2] = 255; }
            Marshal.Copy(data, 0, pixels, bytes);
            if (BitBlt(destination, 0, 0, width, height, source, 0, 0, 0x00cc0020) == 0)
                throw new InvalidOperationException("BitBlt could not read the inactive desktop surface");
            if (GdiFlush() == 0) throw new InvalidOperationException("GdiFlush failed before reading the capture bitmap");
            Marshal.Copy(pixels, data, 0, bytes);
            var colors = new HashSet<int>();
            for (int i = 0; i < bytes; i += 4) colors.Add(data[i] | data[i + 1] << 8 | data[i + 2] << 16);
            using (var output = new BinaryWriter(File.Create(path))) {
                output.Write((ushort)0x4d42); output.Write(54 + bytes); output.Write(0); output.Write(54);
                output.Write(40); output.Write(width); output.Write(-height); output.Write((ushort)1); output.Write((ushort)32);
                output.Write(0); output.Write(bytes); output.Write(0); output.Write(0); output.Write(0); output.Write(0);
                output.Write(data);
            }
            return colors.Count;
        } finally {
            if (previousBitmap != IntPtr.Zero && previousBitmap != new IntPtr(-1)) SelectObject(destination, previousBitmap);
            if (bitmap != IntPtr.Zero) DeleteObject(bitmap);
            if (destination != IntPtr.Zero) DeleteDC(destination);
            if (source != IntPtr.Zero) ReleaseDC(window, source);
            if (previousDpi != IntPtr.Zero) SetThreadDpiAwarenessContext(previousDpi);
        }
    }
    public long GridWindow() {
        var windows = Windows();
        var splitter = Array.Find(windows, window => window.Text == "splitter" && window.Visible);
        var editor = Array.Find(windows, window => window.Text == "SubsEditBox");
        if (splitter == null || editor == null) throw new InvalidOperationException("Main splitter/editor unavailable");
        var grids = Array.FindAll(windows, window => window.Parent == splitter.Handle && window.Handle != editor.Parent && window.Visible);
        if (grids.Length != 1) throw new InvalidOperationException("Expected exactly one subtitle grid");
        return grids[0].Handle;
    }
    public int CaptureGrid(string path) { return CaptureWindow(path, new IntPtr(GridWindow())); }
    public int ScrollGridPageDown() {
        long grid = GridWindow();
        var scrollbars = Array.FindAll(Windows(), window => window.Parent == grid && window.Class == "ScrollBar");
        if (scrollbars.Length != 1) throw new InvalidOperationException("Expected the grid scrollbar");
        var scrollbar = new IntPtr(scrollbars[0].Handle);
        var info = new ScrollInfo { Size = (uint)Marshal.SizeOf<ScrollInfo>(), Mask = 4 };
        if (GetScrollInfo(scrollbar, 2, ref info) == 0) throw new Win32Exception(Marshal.GetLastWin32Error(), "GetScrollInfo failed");
        int before = info.Position;
        UIntPtr result;
        if (SendMessageTimeoutW(new IntPtr(grid), 0x115, new UIntPtr(3), scrollbar, 0x22, 2000, out result) == IntPtr.Zero)
            throw new TimeoutException("Grid page scroll exceeded two seconds");
        if (GetScrollInfo(scrollbar, 2, ref info) == 0) throw new Win32Exception(Marshal.GetLastWin32Error(), "GetScrollInfo failed");
        if (info.Position <= before) throw new InvalidOperationException("The grid did not scroll down");
        return info.Position;
    }
    public void InvalidateGrid() {
        if (InvalidateRect(new IntPtr(GridWindow()), IntPtr.Zero, 0) == 0)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Grid invalidation failed");
    }
    public static int CompareSurfaces(string firstPath, string secondPath) {
        // These are the fixed 32-bit BMP files emitted by CaptureOnDesktop.
        var first = File.ReadAllBytes(firstPath);
        var second = File.ReadAllBytes(secondPath);
        if (first.Length != second.Length || first.Length < 54) throw new InvalidOperationException("Surface dimensions changed");
        for (int i = 0; i < 54; ++i)
            if (first[i] != second[i]) throw new InvalidOperationException("Surface formats changed");
        int different = 0;
        for (int i = 54; i < first.Length; i += 4)
            if (first[i] != second[i] || first[i + 1] != second[i + 1] || first[i + 2] != second[i + 2]) ++different;
        return different;
    }
    public void Resize(int width, int height) {
        // ASYNCWINDOWPOS avoids an unbounded cross-thread SendMessage wait.
        if (SetWindowPos(resizeWindow, IntPtr.Zero, 40, 40, width, height, 0x4054) == 0)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "SetWindowPos failed");
    }
    public int DisableSystemCompositing() {
        int changed = 0;
        foreach (var window in Windows()) {
            if ((window.ExtendedStyle & 0x02000000) == 0) continue;
            if (SetWindowLongPtrW(new IntPtr(window.Handle), -20, new IntPtr(window.ExtendedStyle & ~0x02000000L)) == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "SetWindowLongPtr failed");
            ++changed;
        }
        return changed;
    }
    public double Ping() {
        var watch = Stopwatch.StartNew();
        UIntPtr result;
        if (SendMessageTimeoutW(MainWindow, 0, UIntPtr.Zero, IntPtr.Zero, 0x22, 2000, out result) == IntPtr.Zero)
            throw new TimeoutException("UI did not respond within two seconds");
        return watch.Elapsed.TotalMilliseconds;
    }
    public void InvalidateVideo() {
        var canvases = Array.FindAll(Windows(), window => window.Class == "wxGLCanvas");
        if (canvases.Length != 1) throw new InvalidOperationException("Expected exactly one video canvas");
        if (InvalidateRect(new IntPtr(canvases[0].Handle), IntPtr.Zero, 0) == 0)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "InvalidateRect failed");
    }
    public void Close() {
        if (PostMessageW(MainWindow, 0x10, UIntPtr.Zero, IntPtr.Zero) == 0)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "WM_CLOSE failed");
        if (WaitForSingleObject(processHandle, 10000) != 0) throw new TimeoutException("Aegisub did not exit within ten seconds");
        uint exitCode;
        if (GetExitCodeProcess(processHandle, out exitCode) == 0)
            throw new Win32Exception(Marshal.GetLastWin32Error(), "GetExitCodeProcess failed");
        if (exitCode != 0) throw new InvalidOperationException("Aegisub exit code: " + exitCode);
    }
    public void Dispose() {
        try {
            if (Process != null) {
                if (!Process.HasExited) { Process.Kill(true); Process.WaitForExit(5000); }
                Process.Dispose();
            }
        } finally { processHandle.Dispose(); desktop.Dispose(); }
    }
    public static long ClockNs() {
        long ticks = Stopwatch.GetTimestamp();
        return ticks / Stopwatch.Frequency * 1000000000L + ticks % Stopwatch.Frequency * 1000000000L / Stopwatch.Frequency;
    }
    public static void WriteVideo(string path) {
        using (var output = new BinaryWriter(File.Create(path))) {
            output.Write(System.Text.Encoding.ASCII.GetBytes("YUV4MPEG2 W640 H360 F24:1 Ip A1:1 C420jpeg\n"));
            var y = new byte[640 * 360]; var uv = new byte[640 * 360 / 4];
            for (int i = 0; i < y.Length; ++i) y[i] = (byte)(32 + i % 192);
            Array.Fill(uv, (byte)128);
            for (int frame = 0; frame < 24; ++frame) {
                output.Write(System.Text.Encoding.ASCII.GetBytes("FRAME\n"));
                output.Write(y); output.Write(uv); output.Write(uv);
            }
        }
    }
    public static void WriteAudio(string path) {
        const int samples = 48000 * 12;
        using (var output = new BinaryWriter(File.Create(path))) {
            output.Write(System.Text.Encoding.ASCII.GetBytes("RIFF")); output.Write(36 + samples * 2);
            output.Write(System.Text.Encoding.ASCII.GetBytes("WAVEfmt ")); output.Write(16);
            output.Write((short)1); output.Write((short)1); output.Write(48000); output.Write(96000);
            output.Write((short)2); output.Write((short)16);
            output.Write(System.Text.Encoding.ASCII.GetBytes("data")); output.Write(samples * 2);
            for (int i = 0; i < samples; ++i) output.Write((short)(12000 * Math.Sin(i * 2 * Math.PI * 440 / 48000)));
        }
    }
}
'@

$config = @{
    App = @{ Auto = @{ 'Check For Updates' = $false; 'Load Linked Files' = 0; 'Save on Every Change' = $false } }
    Subtitle = @{ Provider = 'libass'; 'Show Original' = [bool]$ShowOriginal; 'Use STC' = !$PlainTextEditor; Grid = @{ Skia = @{ Enabled = $false } } }
    Audio = @{ Spectrum = ($Mode -eq 'Spectrum'); Display = @{ Skia = @{ Enabled = $false } }; Cache = @{ Type = 1 } }
    Video = @{ 'Open Audio' = $false; 'Default Zoom' = 7; 'Scale with DPI' = $false; Renderer = @{ Backend = 'opengl' } }
}
if ($DetachedVideo) { $config.Video.Detached = @{ Enabled = $true } }
$config | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $profile 'user/config.json') -Encoding utf8NoBOM
$fixture = @'
[Script Info]
Title: Resize probe
ScriptType: v4.00+
PlayResX: 640
PlayResY: 360
LayoutResX: 640
LayoutResY: 360
YCbCr Matrix: TV.601

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Default,Arial,24,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,2,0,2,10,10,10,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
'@
$fixturePath = Join-Path $outputPath 'fixture.ass'
$lines = @(1..$RowCount | ForEach-Object { "Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,Resize row $_" })
if ($TextCase -ne 'Short') {
    $text = if ($TextCase -eq 'Long') { 'A longer subtitle with repeated words for wrapping, punctuation, and selection. ' * 96 } else { '{\bord2\c&H44CCFF&}Tagged subtitle {\b1}bold{\b0}\N' * 96 }
    $lines[0] = 'Dialogue: 0,0:00:00.00,0:00:10.00,Default,,0,0,0,,' + $text
}
[IO.File]::WriteAllText($fixturePath, $fixture + "`n" + ($lines -join "`n") + "`n")
$arguments = [Collections.Generic.List[string]]::new()
$arguments.AddRange([string[]]@('--gui-test', 'host', '--profile-dir', $profile, '--artifacts', $hostOutput, '--open', $fixturePath))
if ($Mode -eq 'Video') {
    $mediaPath = Join-Path $outputPath 'fixture.y4m'
    [ResizeProbeDesktop]::WriteVideo($mediaPath)
    $arguments.AddRange([string[]]@('--open', $mediaPath))
} elseif ($Mode -ne 'Empty') {
    $mediaPath = Join-Path $outputPath 'fixture.wav'
    [ResizeProbeDesktop]::WriteAudio($mediaPath)
    $arguments.AddRange([string[]]@('--open', $mediaPath))
}
$environmentNames = @('AEGISUB_PERF_TRACE', 'AEGISUB_ENABLE_VIDEO_SCENE_CACHE', 'AEGISUB_ENABLE_SKIA_AUDIO_DISPLAY', 'wx_msw_window_no_composited')
$savedEnvironment = @{}
foreach ($name in $environmentNames) { $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name) }
$session = $null
$watch = [Diagnostics.Stopwatch]::StartNew()
$result = [ordered]@{
    status = 'running'; mode = $Mode; axis = $Axis; steps = $Steps; interval_ms = $IntervalMs
    text_case = $TextCase; show_original = [bool]$ShowOriginal; base_width = $BaseWidth
    plain_text_editor = [bool]$PlainTextEditor
    row_count = $RowCount
    isolated_desktop = $true; global_input = $false; scene_cache = !$DisableSceneCache; trace = !$WithoutTrace
    redundant_video_render_assertion = [bool]$AssertNoRedundantVideoRender
    detached_video = [bool]$DetachedVideo; diagnostics = [bool]$Diagnostics
    compositing_disabled_at_creation = [bool]$DisableCompositingAtCreation
    container_paint_policy_assertion = [bool]$AssertContainerPaintPolicy
    executable_sha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash
}
$stepResults = [Collections.Generic.List[object]]::new()
try {
    $env:AEGISUB_PERF_TRACE = if ($WithoutTrace) { '' } else { 'video,audio,log' }
    $env:AEGISUB_ENABLE_VIDEO_SCENE_CACHE = if ($DisableSceneCache) { '0' } else { '1' }
    $env:AEGISUB_ENABLE_SKIA_AUDIO_DISPLAY = '0'
    $env:wx_msw_window_no_composited = if ($DisableCompositingAtCreation) { '1' } else { '0' }
    $session = [ResizeProbeDesktop]::new($executablePath, $arguments.ToArray(), $repo)
    $readyPath = Join-Path $hostOutput 'ready.json'
    while (!(Test-Path -LiteralPath $readyPath) -or !$session.FindMainWindow()) {
        if ($session.Process.HasExited) { throw 'Aegisub exited before becoming ready.' }
        if ($watch.Elapsed.TotalSeconds -ge $TimeoutSeconds) { throw 'Startup timed out.' }
        Start-Sleep -Milliseconds 100
    }
    if ($DetachedVideo) { $session.SelectDetachedVideo() }
    $session.Resize($BaseWidth, 900)
    Start-Sleep -Milliseconds 1500
    $null = $session.Ping()
    if ($ScrollGridPage) {
        $result.grid_scroll_position = $session.ScrollGridPageDown()
        Start-Sleep -Milliseconds 200
        $null = $session.Ping()
    }
    $compositingChanges = if ($DisableSystemCompositing) { $session.DisableSystemCompositing() } else { 0 }
    $result.system_compositing_disabled_windows = $compositingChanges
    $initialWindows = $session.Windows()
    $initialWindows | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputPath 'windows.json') -Encoding utf8NoBOM
    if ($AssertContainerPaintPolicy) {
        $splitter = @($initialWindows | Where-Object { $_.Text -eq 'splitter' -and $_.Visible })
        $editor = @($initialWindows | Where-Object Text -eq 'SubsEditBox')
        if ($splitter.Count -ne 1 -or $editor.Count -ne 1) { throw 'Expected the main edit/grid splitter and subtitle editor.' }
        $containers = @($initialWindows | Where-Object { $_.Handle -in @($splitter[0].Handle, $splitter[0].Parent, $editor[0].Parent) })
        if ($containers.Count -ne 3) { throw 'Expected three distinct main layout containers.' }
        foreach ($container in $containers) {
            if (($container.ClassStyle -band 3) -ne 0 -or ($container.ExtendedStyle -band 0x02000000) -ne 0) {
                throw 'A main layout container still uses full-resize redraw or system compositing.'
            }
        }
        if (!$PlainTextEditor) {
            $textControls = @($initialWindows | Where-Object { $_.Parent -eq $editor[0].Handle -and $_.Text -eq 'stcwindow' })
            if ($textControls.Count -ne 1 -or ($textControls[0].ClassStyle -band 3) -ne 3) { throw 'The container policy leaked into text-control creation.' }
        }
        $result.container_paint_policy = 'passed'
    }
    if ($AssertEditorPaintPolicy) {
        $editors = @($initialWindows | Where-Object Text -eq 'SubsEditBox')
        if ($editors.Count -ne 1 -or ($editors[0].ClassStyle -band 3) -ne 0 -or ($editors[0].ExtendedStyle -band 0x02000000) -ne 0) {
            throw 'The subtitle editor still uses full-resize redraw or system compositing.'
        }
        $result.editor_paint_policy = 'passed'
    }
    $cpuStart = $session.Process.TotalProcessorTime.TotalMilliseconds
    $startNs = [ResizeProbeDesktop]::ClockNs()
    $result.start_ns = $startNs
    $pings = [Collections.Generic.List[double]]::new()
    for ($step = 0; $step -lt $Steps; ++$step) {
        if ($watch.Elapsed.TotalSeconds -ge $TimeoutSeconds) { throw 'Resize probe timed out.' }
        $offset = 4 * [Math]::Abs(($step % 80) - 40)
        $width = if ($Axis -eq 'Height') { $BaseWidth } else { $BaseWidth + $offset }
        $height = if ($Axis -eq 'Width') { 900 } else { 900 + $offset }
        $stepStart = [ResizeProbeDesktop]::ClockNs()
        $session.Resize($width, $height)
        Start-Sleep -Milliseconds $IntervalMs
        $pings.Add($session.Ping())
        if ($Diagnostics) {
            $stepResults.Add([ordered]@{
                step = $step; start_ns = $stepStart; end_ns = [ResizeProbeDesktop]::ClockNs()
                width = $width; height = $height; ping_ms = $pings[-1]; windows = $session.Windows()
            })
        }
    }
    Start-Sleep -Milliseconds 300
    $null = $session.Ping()
    $endNs = [ResizeProbeDesktop]::ClockNs()
    $cpuMs = $session.Process.TotalProcessorTime.TotalMilliseconds - $cpuStart
    $result.end_ns = $endNs
    $result.elapsed_ms = ($endNs - $startNs) / 1e6
    $result.cpu_ms = $cpuMs
    $result.max_ui_ping_ms = ($pings | Measure-Object -Maximum).Maximum
    $finalWindows = $session.Windows()
    $finalWindows | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outputPath 'windows-after.json') -Encoding utf8NoBOM
    $frameBounds = $finalWindows[0].Bounds
    if ($frameBounds.Right - $frameBounds.Left -ne $width -or $frameBounds.Bottom - $frameBounds.Top -ne $height) {
        throw 'The main window did not reach the requested final dimensions.'
    }
    if ($VerifyGridSurface) {
        $beforePath = Join-Path $outputPath 'grid-before.bmp'
        $afterPath = Join-Path $outputPath 'grid-full-repaint.bmp'
        $colors = $session.CaptureGrid($beforePath)
        if ($colors -le 16) { throw 'The captured grid surface is blank or unavailable.' }
        $session.InvalidateGrid()
        $gridWatch = [Diagnostics.Stopwatch]::StartNew()
        do {
            Start-Sleep -Milliseconds 50
            $null = $session.Ping()
            $grid = @($session.Windows() | Where-Object Handle -eq $session.GridWindow())[0]
            if ($gridWatch.Elapsed.TotalSeconds -ge 2) { throw 'Grid repaint exceeded two seconds.' }
        } while ($grid.PendingPaint.Right -gt $grid.PendingPaint.Left)
        $null = $session.CaptureGrid($afterPath)
        $different = [ResizeProbeDesktop]::CompareSurfaces($beforePath, $afterPath)
        $result.grid_surface = @{ different_pixels = $different; unique_colors = $colors }
        if ($different -ne 0) { throw "Partial and full grid repaint differ at $different pixels." }
        $result.grid_surface.status = 'passed'
    }
    if ($CaptureSurface) {
        try {
            $colors = $session.Capture((Join-Path $outputPath 'surface.bmp'))
            $result.surface_capture = @{ status = $(if ($colors -gt 16) { 'requires_visual_inspection' } else { 'unavailable' }); unique_colors = $colors; forces_repaint = $false }
        } catch {
            $result.surface_capture = @{ status = 'unavailable'; error = $_.Exception.Message; forces_repaint = $false }
        }
    }
    if ($VerifyVideoRepaint) {
        $repaintStartNs = [ResizeProbeDesktop]::ClockNs()
        $session.InvalidateVideo()
        $repaintWatch = [Diagnostics.Stopwatch]::StartNew()
        do {
            if ($repaintWatch.Elapsed.TotalSeconds -ge 2) { throw 'The video update region was not painted within two seconds.' }
            Start-Sleep -Milliseconds 50
            $null = $session.Ping()
            $canvas = @($session.Windows() | Where-Object Class -eq 'wxGLCanvas')[0]
        } while ($canvas.PendingPaint.Right -gt $canvas.PendingPaint.Left)
        if ($repaintWatch.Elapsed.TotalSeconds -ge 2) { throw 'The video repaint verification exceeded two seconds.' }
        $repaintEndNs = [ResizeProbeDesktop]::ClockNs()
    }
    $session.Close()
    $entries = @()
    if (!$WithoutTrace) {
        $trace = @(Get-ChildItem -LiteralPath (Join-Path $profile 'user/perf-sessions') -Recurse -Filter trace.ndjson)
        if ($trace.Count -ne 1) { throw 'Expected exactly one performance trace.' }
        $allEntries = @(Get-Content -LiteralPath $trace[0].FullName | ForEach-Object { $_ | ConvertFrom-Json })
        $entries = @($allEntries | Where-Object { $_.t_monotonic_ns -ge $startNs -and $_.t_monotonic_ns -le $endNs })
        if ($entries.Count -eq 0) { throw 'No trace events were recorded during the resize interval.' }
        if ($Mode -eq 'Video' -and !($allEntries | Where-Object { $_.name -eq 'video_ui_duration' -and $_.payload.phase -eq 'video_display.render' -and $_.payload.detail_b -eq 1 })) {
            throw 'The video fixture did not complete a successful buffer swap.'
        }
    }
    $scopes = @($entries | Where-Object { $_.name -in @('video_ui_duration', 'audio_ui_duration') } | Group-Object { $_.payload.phase } | ForEach-Object {
        $values = @($_.Group | ForEach-Object { [double]$_.payload.duration_ms } | Sort-Object)
        [pscustomobject]@{ phase = $_.Name; count = $values.Count; total_ms = ($values | Measure-Object -Sum).Sum; max_ms = $values[-1]; p95_ms = $values[[Math]::Max(0, [int][Math]::Ceiling($values.Count * 0.95) - 1)] }
    })
    $result.trace_entries = $entries.Count
    $result.grid_paints = @($entries | Where-Object { $_.name -eq 'subtitle/grid/host_frame' }).Count
    $result.scopes = $scopes
    if ($Diagnostics) {
        $result.pending_video_paint_steps = @($stepResults | Where-Object {
            $_.windows | Where-Object { $_.Class -eq 'wxGLCanvas' -and $_.PendingPaint.Right -gt $_.PendingPaint.Left }
        }).Count
    }
    if ($VerifyLayoutTransition) {
        $tops = @($stepResults | ForEach-Object {
            $buttons = @($_.windows | Where-Object { $_.Text -eq 'Toggle Bold' -and $_.Visible })
            if ($buttons.Count -ne 1) { throw 'Expected one visible formatting button.' }
            $buttons[0].Bounds.Top
        })
        $changes = 0
        for ($i = 1; $i -lt $tops.Count; ++$i) { if ($tops[$i] -ne $tops[$i - 1]) { ++$changes } }
        $result.layout_transitions = $changes
        if ($changes -ne 2 -or $tops[0] -ne $tops[-1]) { throw 'Expected one collapse and one expansion of the formatting row.' }
    }
    if ($VerifyVideoRepaint) {
        $repaints = @($allEntries | Where-Object { $_.t_monotonic_ns -ge $repaintStartNs -and $_.t_monotonic_ns -le $repaintEndNs })
        $paintCount = @($repaints | Where-Object { $_.payload.phase -eq 'video_display.paint' }).Count
        $swapCount = @($repaints | Where-Object { $_.payload.phase -eq 'video_display.swap' -and $_.payload.detail_a -eq 1 }).Count
        $result.video_repaint = @{ paints = $paintCount; swaps = $swapCount; elapsed_ms = ($repaintEndNs - $repaintStartNs) / 1e6 }
        if ($paintCount -lt 1 -or $swapCount -lt 1) { throw 'An explicit video invalidation did not repaint and present the paused frame.' }
        $result.video_repaint.status = 'passed'
    }
    if ($AssertNoRedundantVideoRender) {
        $beforeCanvas = @($initialWindows | Where-Object Class -eq 'wxGLCanvas')
        $afterCanvas = @($finalWindows | Where-Object Class -eq 'wxGLCanvas')
        if ($beforeCanvas.Count -ne 1 -or $afterCanvas.Count -ne 1) { throw 'Expected exactly one video canvas.' }
        $before = $beforeCanvas[0].Bounds
        $after = $afterCanvas[0].Bounds
        if ($before.Right - $before.Left -ne $after.Right - $after.Left -or $before.Bottom - $before.Top -ne $after.Bottom - $after.Top) {
            throw 'The video canvas dimensions changed during the probe.'
        }
        if ($Diagnostics) {
            foreach ($sample in $stepResults) {
                $canvases = @($sample.windows | Where-Object Class -eq 'wxGLCanvas')
                if ($canvases.Count -ne 1) { throw 'A resize step did not have exactly one video canvas.' }
                $bounds = $canvases[0].Bounds
                if ($bounds.Left -ne $before.Left -or $bounds.Top -ne $before.Top -or $bounds.Right -ne $before.Right -or $bounds.Bottom -ne $before.Bottom) {
                    throw 'The video canvas moved or resized during an intermediate step.'
                }
            }
        }
        $resizes = @($entries | Where-Object { $_.name -eq 'video_ui_duration' -and $_.payload.phase -eq 'video_display.resize' })
        $sizes = @($resizes | ForEach-Object { '{0}x{1}' -f $_.payload.detail_a, $_.payload.detail_b } | Sort-Object -Unique)
        if ($sizes.Count -gt 1) { throw 'The video size events did not describe one unchanged canvas size.' }
        if ($entries | Where-Object { $_.name -eq 'video_frame_delivered' }) { throw 'The video changed during the paused resize interval.' }
        $renders = @($entries | Where-Object { $_.name -eq 'video_ui_duration' -and $_.payload.phase -eq 'video_display.render' }).Count
        $paints = @($entries | Where-Object { $_.name -eq 'video_ui_duration' -and $_.payload.phase -eq 'video_display.paint' }).Count
        if ($renders -gt $paints) { throw "Unchanged size events scheduled extra renders: $renders renders, $paints paint events." }
        if ($renders -gt 1) { throw "An unchanged video canvas repainted $renders times during the resize interval." }
    }
    if ($DetachedVideo) {
        $sizes = @($entries | Where-Object { $_.name -eq 'video_ui_duration' -and $_.payload.phase -eq 'video_display.resize' } | ForEach-Object { '{0}x{1}' -f $_.payload.detail_a, $_.payload.detail_b } | Sort-Object -Unique)
        $swaps = @($entries | Where-Object { $_.name -eq 'video_ui_duration' -and $_.payload.phase -eq 'video_display.swap' -and $_.payload.detail_a -eq 1 })
        if ($sizes.Count -lt 2 -or $swaps.Count -lt 2) { throw 'Actual video resizes did not produce updated dimensions and successful buffer swaps.' }
    }
    $result.status = 'passed'
    $result | ConvertTo-Json -Depth 8
} catch {
    $result.status = 'failed'
    $result.error = $_.Exception.Message
    throw
} finally {
    if ($session) { $session.Dispose() }
    foreach ($name in $environmentNames) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name]) }
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outputPath 'result.json') -Encoding utf8NoBOM
    if ($Diagnostics) { $stepResults | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $outputPath 'steps.json') -Encoding utf8NoBOM }
}
