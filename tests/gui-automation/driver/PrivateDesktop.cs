using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;

namespace Aegisub.GuiAutomation.Driver;

public sealed class PrivateDesktop : IDisposable
{
    private const string DesktopVariable = "AEGISUB_GEOMETRY_PRIVATE_DESKTOP";
    private const string ParentVariable = "AEGISUB_GEOMETRY_LAUNCHER_PID";
    private const string ParentStartVariable = "AEGISUB_GEOMETRY_LAUNCHER_START";
    private const string InputVariable = "AEGISUB_GEOMETRY_INPUT_DESKTOP";
    private const uint Access = 0xC7;
    private readonly DesktopHandle desktop;
    private readonly Dictionary<int, long> owned = new();
    private readonly Dictionary<int, SafeWaitHandle> processHandles = new();
    private readonly string artifacts;
    public string Name { get; }
    private readonly string inputName;

    private sealed class DesktopHandle : SafeHandleZeroOrMinusOneIsInvalid
    {
        public DesktopHandle() : base(true) { }
        protected override bool ReleaseHandle() => CloseDesktop(handle);
    }

    private PrivateDesktop(DesktopHandle desktop, string name, string inputName, string artifacts)
    {
        this.desktop = desktop;
        Name = name;
        this.inputName = inputName;
        this.artifacts = artifacts;
    }

    public static int Run(string[] arguments, string artifacts, Func<PrivateDesktop, int> operation)
    {
        var name = Environment.GetEnvironmentVariable(DesktopVariable);
        if (name is null)
        {
            var allowed = Path.GetFullPath(Path.Combine(Environment.CurrentDirectory, "build-dir", "artifacts"));
            var relative = Path.GetRelativePath(allowed, artifacts);
            if (relative == "." || Path.IsPathRooted(relative) || relative == ".." || relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal)
                || (Directory.Exists(artifacts) && Directory.EnumerateFileSystemEntries(artifacts).Any()))
                throw new InvalidOperationException("Private geometry artifacts must be a new subdirectory of build-dir/artifacts");
            var unique = "AegisubGeometry_" + Guid.NewGuid().ToString("N");
            using var input = OpenInputDesktop(0, false, 1);
            if (input.IsInvalid) throw Error("OpenInputDesktop");
            var inputName = ObjectName(input.DangerousGetHandle());
            using var desktop = CreateDesktop(unique, 0, 0, 0, Access, 0);
            if (desktop.IsInvalid) throw Error("CreateDesktop");
            using var isolation = new PrivateDesktop(desktop, unique, inputName, artifacts);
            var launcher = Process.GetCurrentProcess();
            var start = new ProcessStartInfo(Environment.ProcessPath ?? throw new InvalidOperationException("Driver executable path unavailable"));
            foreach (var argument in arguments) start.ArgumentList.Add(argument);
            start.Environment[DesktopVariable] = unique;
            start.Environment[ParentVariable] = launcher.Id.ToString();
            start.Environment[ParentStartVariable] = launcher.StartTime.ToUniversalTime().Ticks.ToString();
            start.Environment[InputVariable] = inputName;
            using var driver = isolation.Start(start, "private-driver");
            if (!driver.WaitForExit(210_000))
            {
                isolation.Stop(driver);
                throw new TimeoutException("Private geometry driver exceeded 210 seconds");
            }
            driver.Refresh();
            isolation.VerifyInput();
            Directory.CreateDirectory(artifacts);
            File.WriteAllText(Path.Combine(artifacts, "launcher-identity.json"), JsonSerializer.Serialize(new
            {
                launcher_pid = launcher.Id,
                launcher_start_utc = launcher.StartTime.ToUniversalTime(),
                driver_pid = driver.Id,
                driver_start_utc = new DateTime(isolation.owned[driver.Id], DateTimeKind.Utc),
                window_station = "WinSta0",
                desktop = unique,
                input_desktop = inputName,
                driver_exit_code = isolation.ExitCode(driver),
                switched_desktop = false
            }, new JsonSerializerOptions { WriteIndented = true }));
            return isolation.ExitCode(driver);
        }
        Directory.CreateDirectory(Path.GetDirectoryName(artifacts)!);
        var bootstrap = new StreamWriter(artifacts + ".bootstrap.log") { AutoFlush = true };
        Console.SetOut(bootstrap);
        Console.SetError(bootstrap);
        if (!name.StartsWith("AegisubGeometry_", StringComparison.Ordinal) || !Guid.TryParseExact(name[16..], "N", out _))
            throw new InvalidOperationException("Unrecognized private desktop identity");
        var launcherPid = int.Parse(Environment.GetEnvironmentVariable(ParentVariable) ?? throw new InvalidOperationException("Launcher PID unavailable"));
        var launcherStart = long.Parse(Environment.GetEnvironmentVariable(ParentStartVariable) ?? throw new InvalidOperationException("Launcher start unavailable"));
        using var parent = Process.GetProcessById(launcherPid);
        if (ParentPid() != launcherPid || parent.StartTime.ToUniversalTime().Ticks != launcherStart || parent.MainModule?.FileName != Environment.ProcessPath)
            throw new InvalidOperationException("Private driver is not owned by its recorded launcher");
        var handle = OpenDesktop(name, 0, false, Access);
        if (handle.IsInvalid) throw Error("OpenDesktop");
        using var child = new PrivateDesktop(handle, name, Environment.GetEnvironmentVariable(InputVariable) ?? throw new InvalidOperationException("Input desktop identity unavailable"), artifacts);
        child.VerifyCurrent();
        return operation(child);
    }

    public void RecordDriver()
    {
        VerifyCurrent();
        using var current = Process.GetCurrentProcess();
        var identity = Record("driver", current, GetCurrentThreadId());
        var path = Path.Combine(artifacts, "driver-identity.json");
        File.WriteAllText(path + ".tmp", identity);
        File.Move(path + ".tmp", path);
    }

    public Process Start(ProcessStartInfo start, string phase)
    {
        VerifyInput();
        if (ObjectName(GetProcessWindowStation()) != "WinSta0") throw new InvalidOperationException("Geometry isolation requires WinSta0");
        var command = new StringBuilder(Quote(Path.GetFullPath(start.FileName)));
        foreach (var argument in start.ArgumentList) command.Append(' ').Append(Quote(argument));
        var environment = string.Join('\0', start.Environment.OrderBy(pair => pair.Key, StringComparer.OrdinalIgnoreCase).Select(pair => pair.Key + "=" + pair.Value)) + "\0\0";
        var memory = Marshal.StringToHGlobalUni(environment);
        SafeWaitHandle? unclaimed = null;
        Process? process = null;
        var createdId = 0;
        try
        {
            var startup = new StartupInfo { Size = (uint)Marshal.SizeOf<StartupInfo>(), Desktop = "WinSta0\\" + Name, Flags = 1, ShowWindow = 4 };
            if (!CreateProcess(Path.GetFullPath(start.FileName), command, 0, 0, false, 0x08000400, memory, Environment.CurrentDirectory, ref startup, out var info)) throw Error("CreateProcess");
            unclaimed = new SafeWaitHandle(info.Process, true);
            using var threadHandle = new SafeWaitHandle(info.Thread, true);
            createdId = checked((int)info.ProcessId);
            process = Process.GetProcessById(createdId);
            var started = process.StartTime.ToUniversalTime().Ticks;
            if (phase != "private-driver")
            {
                var deadline = Stopwatch.GetTimestamp() + 10 * Stopwatch.Frequency;
                var connected = false;
                while (KernelExitCode(unclaimed) == 259 && Stopwatch.GetTimestamp() < deadline)
                {
                    var current = GetThreadDesktop(info.ThreadId);
                    if (current != 0 && ObjectName(current) == Name) { connected = true; break; }
                    Thread.Sleep(20);
                }
                if (KernelExitCode(unclaimed) != 259)
                    throw new InvalidOperationException("Created private host exited during startup: " + KernelExitCode(unclaimed));
                if (!connected)
                    throw new InvalidOperationException("Created process did not connect to the private desktop");
                Record(phase, process, info.ThreadId);
            }
            owned.Add(createdId, started);
            processHandles.Add(createdId, unclaimed);
            unclaimed = null;
            return process;
        }
        catch
        {
            if (unclaimed is not null) Cleanup(unclaimed);
            owned.Remove(createdId);
            processHandles.Remove(createdId);
            process?.Dispose();
            throw;
        }
        finally { unclaimed?.Dispose(); Marshal.FreeHGlobal(memory); }
    }

    public void VerifyCurrent()
    {
        VerifyInput();
        if (ObjectName(GetProcessWindowStation()) != "WinSta0" || ObjectName(GetThreadDesktop(GetCurrentThreadId())) != Name)
            throw new InvalidOperationException("Driver is not on its private WinSta0 desktop");
    }

    private void VerifyInput()
    {
        using var input = OpenInputDesktop(0, false, 1);
        if (input.IsInvalid) throw Error("OpenInputDesktop");
        var actual = ObjectName(input.DangerousGetHandle());
        if (actual != inputName || actual == Name) throw new InvalidOperationException("Input desktop changed or isolation desktop became interactive");
    }

    public void VerifyWindow(nint window, Process process)
    {
        VerifyCurrent();
        var thread = GetWindowThreadProcessId(window, out var owner);
        if (window == 0 || owner != process.Id || thread == 0 || !owned.TryGetValue(process.Id, out var start)
            || process.HasExited || process.StartTime.ToUniversalTime().Ticks != start || ObjectName(GetThreadDesktop(thread)) != Name)
            throw new InvalidOperationException("Target HWND is not owned by the live private-desktop host");
    }

    public nint[] Windows(Process process)
    {
        VerifyCurrent();
        var windows = new List<nint>();
        if (!EnumDesktopWindows(desktop, (window, _) => { GetWindowThreadProcessId(window, out var owner); if (owner == process.Id) windows.Add(window); return true; }, 0)) throw Error("EnumDesktopWindows");
        return windows.Where(window => GetWindowThreadProcessId(window, out var owner) != 0 && owner == process.Id).ToArray();
    }

    public nint MainWindow(Process process, int timeoutSeconds)
    {
        var deadline = Stopwatch.GetTimestamp() + timeoutSeconds * Stopwatch.Frequency;
        while (Stopwatch.GetTimestamp() < deadline)
        {
            if (process.HasExited) throw new InvalidOperationException("Private host exited before main-window discovery");
            var window = Windows(process).SingleOrDefault(window => ClassName(window).StartsWith("wxWindow", StringComparison.Ordinal) && ReadText(window, process).Contains("Aegisub", StringComparison.Ordinal));
            if (window != 0) { Record("main-window", process, GetWindowThreadProcessId(window, out _)); return window; }
            Thread.Sleep(50);
        }
        throw new TimeoutException("Private host main window was not discovered");
    }

    public void Stop(Process process)
    {
        if (owned.TryGetValue(process.Id, out var start) && !process.HasExited && process.StartTime.ToUniversalTime().Ticks == start)
        { process.Kill(entireProcessTree: true); process.WaitForExit(5000); }
    }

    public int ExitCode(Process process)
    {
        if (!processHandles.TryGetValue(process.Id, out var handle)) throw new InvalidOperationException("Process handle was not transferred to the private desktop owner");
        return KernelExitCode(handle);
    }

    private static int KernelExitCode(SafeWaitHandle handle)
    {
        if (!GetExitCodeProcess(handle, out var code)) throw Error("GetExitCodeProcess");
        return unchecked((int)code);
    }

    private void Cleanup(SafeWaitHandle handle)
    {
        try
        {
            if (KernelExitCode(handle) != 259) return;
            if (!TerminateProcess(handle, 124) && KernelExitCode(handle) == 259) throw Error("TerminateProcess");
            if (WaitForSingleObject(handle, 5000) != 0) throw new TimeoutException("Owned process cleanup exceeded five seconds");
        }
        catch (Exception error)
        {
            try { File.AppendAllText(artifacts + ".cleanup-errors.log", error + Environment.NewLine); }
            catch
            {
                try { Console.Error.WriteLine("Secondary owned-process cleanup error: " + error); }
                catch { }
            }
        }
    }

    public static string ClassName(nint window)
    {
        var text = new StringBuilder(256);
        if (GetClassName(window, text, text.Capacity) == 0) throw Error("GetClassName");
        return text.ToString();
    }

    public static string Text(nint window)
    {
        var text = new StringBuilder(32_768);
        if (SendMessageTimeout(window, 0x000D, text.Capacity, text, 0x22, 1000, out _) == 0) throw Error("WM_GETTEXT");
        return text.ToString();
    }

    public string ReadText(nint window, Process process)
    {
        VerifyWindow(window, process);
        return Text(window);
    }

    private string Record(string phase, Process process, uint thread)
    {
        Directory.CreateDirectory(artifacts);
        var identity = JsonSerializer.Serialize(new
        {
            phase, pid = process.Id, start_utc = process.StartTime.ToUniversalTime(),
            thread_id = thread, desktop = ObjectName(GetThreadDesktop(thread)), window_station = ObjectName(GetProcessWindowStation()), input_desktop = inputName
        });
        File.AppendAllText(Path.Combine(artifacts, "desktop-evidence.ndjson"), identity + Environment.NewLine);
        return identity;
    }

    private static string ObjectName(nint handle)
    {
        if (handle == 0) throw Error("Desktop handle");
        var text = new StringBuilder(512);
        if (!GetUserObjectInformation(handle, 2, text, (uint)(text.Capacity * 2), out _)) throw Error("GetUserObjectInformation");
        return text.ToString();
    }

    private static int ParentPid()
    {
        using var process = Process.GetCurrentProcess();
        if (NtQueryInformationProcess(process.Handle, 0, out var basic, Marshal.SizeOf<BasicProcessInfo>(), out _) != 0) throw new InvalidOperationException("Parent process query failed");
        return basic.Parent.ToInt32();
    }

    private static string Quote(string value)
    {
        var output = new StringBuilder("\"");
        var slashes = 0;
        foreach (var character in value)
        {
            if (character == '\\') { ++slashes; continue; }
            output.Append('\\', character == '"' ? slashes * 2 + 1 : slashes).Append(character);
            slashes = 0;
        }
        return output.Append('\\', slashes * 2).Append('"').ToString();
    }

    private static Win32Exception Error(string operation) => new(Marshal.GetLastPInvokeError(), operation + " failed");
    public void Dispose()
    {
        foreach (var handle in processHandles.Values) { Cleanup(handle); handle.Dispose(); }
        desktop.Dispose();
    }

    [StructLayout(LayoutKind.Sequential)] private struct BasicProcessInfo { public nint Reserved, Peb, Reserved2, Reserved3, Pid, Parent; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] private struct StartupInfo
    {
        public uint Size; public string? Reserved, Desktop, Title;
        public uint X, Y, Width, Height, CharsX, CharsY, Fill, Flags;
        public ushort ShowWindow, ReservedSize; public nint ReservedData, Input, Output, Error;
    }
    [StructLayout(LayoutKind.Sequential)] private struct ProcessInfo { public nint Process, Thread; public uint ProcessId, ThreadId; }
    private delegate bool EnumWindow(nint window, nint argument);
    [DllImport("user32.dll", EntryPoint = "CreateDesktopW", CharSet = CharSet.Unicode, SetLastError = true)] private static extern DesktopHandle CreateDesktop(string name, nint device, nint mode, uint flags, uint access, nint security);
    [DllImport("user32.dll", EntryPoint = "OpenDesktopW", CharSet = CharSet.Unicode, SetLastError = true)] private static extern DesktopHandle OpenDesktop(string name, uint flags, bool inherit, uint access);
    [DllImport("user32.dll", SetLastError = true)] private static extern DesktopHandle OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll")] private static extern bool CloseDesktop(nint desktop);
    [DllImport("user32.dll")] private static extern nint GetThreadDesktop(uint thread);
    [DllImport("user32.dll")] private static extern nint GetProcessWindowStation();
    [DllImport("kernel32.dll")] private static extern uint GetCurrentThreadId();
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool GetExitCodeProcess(SafeWaitHandle process, out uint code);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern bool TerminateProcess(SafeWaitHandle process, uint code);
    [DllImport("kernel32.dll")] private static extern uint WaitForSingleObject(SafeWaitHandle process, uint milliseconds);
    [DllImport("user32.dll", EntryPoint = "GetUserObjectInformationW", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool GetUserObjectInformation(nint handle, int index, StringBuilder text, uint length, out uint needed);
    [DllImport("kernel32.dll", EntryPoint = "CreateProcessW", CharSet = CharSet.Unicode, SetLastError = true)] private static extern bool CreateProcess(string application, StringBuilder command, nint processSecurity, nint threadSecurity, bool inherit, uint flags, nint environment, string directory, ref StartupInfo startup, out ProcessInfo process);
    [DllImport("ntdll.dll")] private static extern int NtQueryInformationProcess(nint process, int information, out BasicProcessInfo basic, int length, out int returned);
    [DllImport("user32.dll", SetLastError = true)] private static extern bool EnumDesktopWindows(DesktopHandle desktop, EnumWindow callback, nint parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(nint window, out uint process);
    [DllImport("user32.dll", EntryPoint = "GetClassNameW", CharSet = CharSet.Unicode, SetLastError = true)] private static extern int GetClassName(nint window, StringBuilder text, int length);
    [DllImport("user32.dll", EntryPoint = "SendMessageTimeoutW", CharSet = CharSet.Unicode, SetLastError = true)] private static extern nint SendMessageTimeout(nint window, uint message, nint parameter, StringBuilder text, uint flags, uint timeout, out nint result);
}
