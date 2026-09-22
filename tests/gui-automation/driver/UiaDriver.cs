using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Automation;

namespace Aegisub.GuiAutomation.Driver;

public sealed record UiElementState(string Name, string Value);

public static class UiaDriver
{
    public static AutomationElement WaitForMainWindow(Process process, TimeSpan timeout)
    {
        process.WaitForInputIdle((int)timeout.TotalMilliseconds);
        var deadline = Stopwatch.GetTimestamp() +
            (long)(timeout.TotalSeconds * Stopwatch.Frequency);
        while (Stopwatch.GetTimestamp() < deadline)
        {
            process.Refresh();
            if (!process.HasExited && process.MainWindowHandle != 0)
                return AutomationElement.FromHandle(process.MainWindowHandle);
            Thread.Sleep(100);
        }
        throw new TimeoutException("Aegisub main window was not discovered");
    }

    public static AutomationElement? FindEnabledInvokableButton(
        AutomationElement root,
        params string[] preferredNames)
    {
        var buttons = root.FindAll(
            TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button));
        foreach (AutomationElement button in buttons)
        {
            try
            {
                if (!button.Current.IsEnabled
                    || !button.TryGetCurrentPattern(InvokePattern.Pattern, out _))
                    continue;
                var name = button.Current.Name ?? string.Empty;
                if (preferredNames.Length == 0
                    || preferredNames.Any(preferred =>
                        name.Contains(preferred, StringComparison.OrdinalIgnoreCase)))
                    return button;
            }
            catch (ElementNotAvailableException)
            {
                // UI surfaces can be replaced while the frame settles.
            }
        }
        return null;
    }

    public static AutomationElement? FindEnabledInvokableButtonByAutomationId(
        AutomationElement root,
        string automationId,
        params string[] fallbackNames)
    {
        var button = FindDescendantByAutomationId(root, automationId, ControlType.Button);
        if (IsEnabledAndInvokable(button))
            return button;
        return FindEnabledInvokableButton(root, fallbackNames);
    }

    public static AutomationElement? FindLastEnabledInvokableButtonInToolbar(
        AutomationElement root)
    {
        var toolbars = root.FindAll(
            TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.ToolBar));
        foreach (AutomationElement toolbar in toolbars)
        {
            try
            {
                var buttons = toolbar.FindAll(
                    TreeScope.Descendants,
                    new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button));
                for (var index = buttons.Count - 1; index >= 0; --index)
                {
                    if (IsEnabledAndInvokable(buttons[index]))
                        return buttons[index];
                }
            }
            catch (ElementNotAvailableException)
            {
                // UI surfaces can be replaced while the frame settles.
            }
        }
        return null;
    }

    public static AutomationElement? FindEnabledToggle(
        AutomationElement root,
        ControlType controlType,
        params string[] preferredNames)
    {
        var controls = root.FindAll(
            TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, controlType));
        foreach (AutomationElement control in controls)
        {
            try
            {
                if (control.Current.IsEnabled
                    && control.TryGetCurrentPattern(TogglePattern.Pattern, out _)
                    && (preferredNames.Length == 0
                        || preferredNames.Any(preferred =>
                            (control.Current.Name ?? string.Empty).Contains(
                                preferred,
                                StringComparison.OrdinalIgnoreCase))))
                    return control;
            }
            catch (ElementNotAvailableException)
            {
                // UI surfaces can be replaced while the frame settles.
            }
        }
        return null;
    }

    public static ToggleState ReadToggleState(AutomationElement element)
    {
        if (!element.TryGetCurrentPattern(TogglePattern.Pattern, out var pattern))
            throw new InvalidOperationException("UIA element does not expose TogglePattern");
        return ((TogglePattern)pattern).Current.ToggleState;
    }

    public static ToggleState WaitForToggleStateChange(
        AutomationElement element,
        ToggleState previous,
        TimeSpan timeout)
    {
        var deadline = Stopwatch.GetTimestamp() +
            (long)(timeout.TotalSeconds * Stopwatch.Frequency);
        while (Stopwatch.GetTimestamp() < deadline)
        {
            try
            {
                var current = ReadToggleState(element);
                if (current != previous)
                    return current;
            }
            catch (ElementNotAvailableException)
            {
                // UI surfaces can be replaced while the frame settles.
            }
            Thread.Sleep(50);
        }
        throw new TimeoutException("UIA toggle did not report an observable state change");
    }

    public static void Toggle(AutomationElement element)
    {
        if (!element.TryGetCurrentPattern(TogglePattern.Pattern, out var pattern))
            throw new InvalidOperationException("UIA element does not expose TogglePattern");
        ((TogglePattern)pattern).Toggle();
    }

    public static AutomationElement? FindDescendantByAutomationId(
        AutomationElement root,
        string automationId,
        ControlType controlType)
    {
        var condition = new AndCondition(
            new PropertyCondition(AutomationElement.AutomationIdProperty, automationId),
            new PropertyCondition(AutomationElement.ControlTypeProperty, controlType));
        try
        {
            return root.FindFirst(TreeScope.Descendants, condition);
        }
        catch (ElementNotAvailableException)
        {
            return null;
        }
    }

    public static string WaitForElementNameChange(
        AutomationElement root,
        string automationId,
        ControlType controlType,
        string previousName,
        TimeSpan timeout)
    {
        var deadline = Stopwatch.GetTimestamp() +
            (long)(timeout.TotalSeconds * Stopwatch.Frequency);
        while (Stopwatch.GetTimestamp() < deadline)
        {
            var element = FindDescendantByAutomationId(root, automationId, controlType);
            if (element is not null)
            {
                try
                {
                    var currentName = element.Current.Name ?? string.Empty;
                    if (!string.Equals(currentName, previousName, StringComparison.Ordinal))
                        return currentName;
                }
                catch (ElementNotAvailableException)
                {
                    // The status bar can be replaced while the frame settles.
                }
            }
            Thread.Sleep(50);
        }
        throw new TimeoutException(
            $"UIA element {automationId} did not report an observable state change");
    }

    public static UiElementState ReadElementState(AutomationElement element)
    {
        try
        {
            var name = element.Current.Name ?? string.Empty;
            var value = string.Empty;
            if (element.TryGetCurrentPattern(ValuePattern.Pattern, out var pattern))
                value = ((ValuePattern)pattern).Current.Value ?? string.Empty;
            return new UiElementState(name, value);
        }
        catch (ElementNotAvailableException)
        {
            return new UiElementState(string.Empty, string.Empty);
        }
    }

    public static UiElementState WaitForElementStateChange(
        AutomationElement root,
        string automationId,
        ControlType controlType,
        UiElementState previous,
        TimeSpan timeout)
    {
        var deadline = Stopwatch.GetTimestamp() +
            (long)(timeout.TotalSeconds * Stopwatch.Frequency);
        while (Stopwatch.GetTimestamp() < deadline)
        {
            var element = FindDescendantByAutomationId(root, automationId, controlType);
            if (element is not null)
            {
                var current = ReadElementState(element);
                if (!string.Equals(current.Name, previous.Name, StringComparison.Ordinal)
                    || !string.Equals(current.Value, previous.Value, StringComparison.Ordinal))
                    return current;
            }
            Thread.Sleep(50);
        }
        throw new TimeoutException(
            $"UIA element {automationId} did not report an observable state change");
    }

    public static void Invoke(AutomationElement element)
    {
        if (!element.TryGetCurrentPattern(InvokePattern.Pattern, out var pattern))
            throw new InvalidOperationException("UIA element does not expose InvokePattern");
        ((InvokePattern)pattern).Invoke();
    }

    public static void ThrowIfFatalDialog(Process process)
    {
        process.Refresh();
        if (process.HasExited)
            return;
        var windows = AutomationElement.RootElement.FindAll(
            TreeScope.Children,
            new PropertyCondition(AutomationElement.ProcessIdProperty, process.Id));
        foreach (AutomationElement window in windows)
        {
            try
            {
                var name = (window.Current.Name ?? string.Empty).Trim();
                if (name.Contains("Program error", StringComparison.OrdinalIgnoreCase)
                    || name.Contains("Aegisub has crashed", StringComparison.OrdinalIgnoreCase)
                    || name.Contains("Aegisub crashed", StringComparison.OrdinalIgnoreCase)
                    || name.Contains("程序错误", StringComparison.OrdinalIgnoreCase)
                    || name.Contains("Aegisub 已崩溃", StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException(
                        $"Aegisub reported a fatal error dialog: {name}");
            }
            catch (ElementNotAvailableException)
            {
                // A closing dialog is not evidence of a fatal state.
            }
        }
    }

    public static bool HasFocusedElementInProcess(Process process)
    {
        try
        {
            var focused = AutomationElement.FocusedElement;
            return focused.Current.ProcessId == process.Id;
        }
        catch (ElementNotAvailableException)
        {
            return false;
        }
    }

    private static bool HasFocusedElementInWindow(
        Process process,
        IntPtr windowHandle,
        out int focusedProcessId,
        out IntPtr focusedWindowHandle)
    {
        focusedProcessId = 0;
        focusedWindowHandle = IntPtr.Zero;
        try
        {
            var focused = AutomationElement.FocusedElement;
            focusedProcessId = focused.Current.ProcessId;
            if (focusedProcessId != process.Id)
                return false;

            focusedWindowHandle = new IntPtr(focused.Current.NativeWindowHandle);
            if (windowHandle == IntPtr.Zero || focusedWindowHandle == IntPtr.Zero)
                return true;
            return GetAncestor(focusedWindowHandle, GetAncestorRoot) == windowHandle;
        }
        catch (ElementNotAvailableException)
        {
            return false;
        }
    }

    public static void FocusAndVerify(
        AutomationElement element,
        Process process,
        TimeSpan timeout)
    {
        var deadline = Stopwatch.GetTimestamp() +
            (long)(timeout.TotalSeconds * Stopwatch.Frequency);
        bool? foregroundRequestSucceeded = null;
        var targetWindowHandle = IntPtr.Zero;
        InvalidOperationException? lastFocusError = null;
        var focusAttempts = 0;
        while (Stopwatch.GetTimestamp() < deadline)
        {
            process.Refresh();
            if (process.HasExited)
                throw new InvalidOperationException(
                    $"Aegisub exited while setting focus (code {process.ExitCode})");
            try
            {
                // UIA focus is subject to the Windows foreground-lock policy.
                // Restore and request the foreground window before asking UIA
                // to move keyboard focus into the native window.
                targetWindowHandle = new IntPtr(element.Current.NativeWindowHandle);
                if (targetWindowHandle == IntPtr.Zero)
                    targetWindowHandle = process.MainWindowHandle;
                if (targetWindowHandle != IntPtr.Zero)
                {
                    if (IsIconic(targetWindowHandle))
                        ShowWindow(targetWindowHandle, ShowWindowRestore);
                    foregroundRequestSucceeded =
                        SetForegroundWindow(targetWindowHandle);
                }
                // A focused child already satisfies the window contract; its
                // top-level UIA element need not itself accept keyboard focus.
                if (HasFocusedElementInWindow(
                    process,
                    targetWindowHandle,
                    out _,
                    out _))
                    return;
                try
                {
                    ++focusAttempts;
                    element.SetFocus();
                }
                catch (InvalidOperationException error)
                {
                    // Restoration/activation can temporarily reject SetFocus.
                    // Keep the same deadline and require verified focus below.
                    lastFocusError = error;
                }
                if (HasFocusedElementInWindow(
                    process,
                    targetWindowHandle,
                    out _,
                    out _))
                    return;
            }
            catch (ElementNotAvailableException)
            {
                // The frame can be replaced while the native window settles.
            }
            Thread.Sleep(50);
        }
        var foregroundWindowHandle = GetForegroundWindow();
        uint foregroundProcessId = 0;
        if (foregroundWindowHandle != IntPtr.Zero)
            GetWindowThreadProcessId(foregroundWindowHandle, out foregroundProcessId);
        var focusedProcessId = 0;
        var focusedWindowHandle = IntPtr.Zero;
        HasFocusedElementInWindow(
            process,
            targetWindowHandle,
            out focusedProcessId,
            out focusedWindowHandle);
        var targetSession = -1;
        try
        {
            targetSession = process.SessionId;
        }
        catch (InvalidOperationException)
        {
        }
        string targetState;
        try
        {
            var current = element.Current;
            targetState = $"target_enabled={current.IsEnabled};" +
                $"target_focusable={current.IsKeyboardFocusable};" +
                $"target_offscreen={current.IsOffscreen};";
        }
        catch (ElementNotAvailableException)
        {
            targetState = "target_state=unavailable;";
        }
        throw new TimeoutException(
            "UIA window did not accept focus " +
            $"(target_pid={process.Id};target_session={targetSession};" +
            $"target_hwnd=0x{targetWindowHandle.ToInt64():X};" +
            $"foreground_pid={foregroundProcessId};" +
            $"foreground_hwnd=0x{foregroundWindowHandle.ToInt64():X};" +
            $"focused_pid={focusedProcessId};" +
            $"focused_hwnd=0x{focusedWindowHandle.ToInt64():X};" +
            $"set_foreground={foregroundRequestSucceeded?.ToString() ?? "unknown"};" +
            targetState +
            $"focus_attempts={focusAttempts};" +
            $"last_focus_error={lastFocusError?.Message ?? "none"};" +
            $"interactive={Environment.UserInteractive})",
            lastFocusError);
    }

    private const int ShowWindowRestore = 9;

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool IsIconic(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(
        IntPtr hWnd,
        out uint processId);

    [DllImport("user32.dll")]
    private static extern IntPtr GetAncestor(IntPtr hWnd, uint flags);

    private const uint GetAncestorRoot = 2;

    public static void RequestCleanClose(Process process, TimeSpan timeout)
    {
        process.Refresh();
        if (!process.HasExited)
        {
            if (!process.CloseMainWindow())
            {
                process.Refresh();
                if (!process.HasExited)
                    throw new InvalidOperationException("Could not request a clean Aegisub shutdown");
            }
            if (!process.HasExited
                && !process.WaitForExit((int)timeout.TotalMilliseconds))
                throw new TimeoutException("Aegisub did not complete a clean shutdown");
        }

        process.WaitForExit();
        process.Refresh();
        if (process.ExitCode != 0)
            throw new InvalidOperationException(
                $"Aegisub exited abnormally with code {process.ExitCode}");
    }

    private static bool IsEnabledAndInvokable(AutomationElement? element)
    {
        if (element is null)
            return false;
        try
        {
            return element.Current.IsEnabled
                && element.TryGetCurrentPattern(InvokePattern.Pattern, out _);
        }
        catch (ElementNotAvailableException)
        {
            return false;
        }
    }
}
