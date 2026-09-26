using System.Runtime.InteropServices;

namespace Aegisub.GuiAutomation.Driver;

public static class ObservedWindowsCommand
{
    private const uint WmCommand = 0x0111;
    private const uint MfByPosition = 0x0400;

    [DllImport("user32.dll", EntryPoint = "GetMenu", ExactSpelling = true)]
    private static extern nint GetMenu(nint hwnd);

    [DllImport("user32.dll", EntryPoint = "GetSubMenu", ExactSpelling = true)]
    private static extern nint GetSubMenu(nint menu, int position);

    [DllImport("user32.dll", EntryPoint = "GetMenuItemCount", ExactSpelling = true)]
    private static extern int GetMenuItemCount(nint menu);

    [DllImport("user32.dll", EntryPoint = "GetMenuItemID", ExactSpelling = true)]
    private static extern uint GetMenuItemID(nint menu, int position);

    [DllImport("user32.dll", EntryPoint = "GetMenuStringW", ExactSpelling = true, CharSet = CharSet.Unicode)]
    private static extern int GetMenuString(nint menu, uint position, [Out] char[] text, int capacity, uint flags);

    [DllImport("user32.dll", EntryPoint = "GetDlgCtrlID", ExactSpelling = true)]
    private static extern int GetDlgCtrlId(nint hwnd);

    [DllImport("user32.dll", EntryPoint = "GetParent", ExactSpelling = true)]
    private static extern nint GetParent(nint hwnd);

    [DllImport("user32.dll", EntryPoint = "GetWindowThreadProcessId", ExactSpelling = true)]
    private static extern uint GetWindowThreadProcessId(nint hwnd, out uint processId);

    [DllImport("user32.dll", EntryPoint = "PostMessageW", ExactSpelling = true, SetLastError = true)]
    private static extern int PostMessage(nint hwnd, uint message, nint wParam, nint lParam);

    public static int FindMenuCommand(nint mainWindow, int processId, string menuName, string commandName)
    {
        VerifyOwner(mainWindow, processId);
        var menuBar = GetMenu(mainWindow);
        if (menuBar == 0)
            throw new InvalidOperationException("Observed main window has no native menu bar");
        var fileMenu = FindPosition(menuBar, menuName);
        var submenu = GetSubMenu(menuBar, fileMenu);
        if (submenu == 0)
            throw new InvalidOperationException("Observed menu has no native submenu");
        var command = GetMenuItemID(submenu, FindPosition(submenu, commandName));
        if (command is 0 or uint.MaxValue || command > ushort.MaxValue)
            throw new InvalidOperationException("Observed menu command ID is not a dispatchable WM_COMMAND ID");
        return checked((int)command);
    }

    public static void PostMenuCommand(nint mainWindow, int processId, int observedId)
    {
        VerifyOwner(mainWindow, processId);
        if (observedId <= 0 || observedId > ushort.MaxValue || PostMessage(mainWindow, WmCommand, observedId, 0) == 0)
            throw new InvalidOperationException("Observed menu command could not be posted to its exact main window");
    }

    public static (nint Parent, int Id) PostButtonClick(nint buttonWindow, int processId)
    {
        VerifyOwner(buttonWindow, processId);
        var parent = GetParent(buttonWindow);
        VerifyOwner(parent, processId);
        var id = GetDlgCtrlId(buttonWindow);
        if (id == 0 || id < short.MinValue || id > ushort.MaxValue
            || PostMessage(parent, WmCommand, unchecked((ushort)id), buttonWindow) == 0)
            throw new InvalidOperationException("Observed button command could not be posted to its exact parent");
        return (parent, id);
    }

    private static int FindPosition(nint menu, string expected)
    {
        var found = -1;
        var count = GetMenuItemCount(menu);
        if (count < 0)
            throw new InvalidOperationException("Observed native menu disappeared");
        for (var position = 0; position < count; position++)
        {
            var text = new char[256];
            var length = GetMenuString(menu, (uint)position, text, text.Length, MfByPosition);
            if (length >= text.Length - 1)
                throw new InvalidOperationException("Observed native menu label was truncated");
            var label = new string(text, 0, length).Split('\t')[0].Replace("&", "", StringComparison.Ordinal);
            if (!label.Equals(expected, StringComparison.OrdinalIgnoreCase))
                continue;
            if (found >= 0)
                throw new InvalidOperationException("Observed native menu label was ambiguous: " + expected);
            found = position;
        }
        if (found < 0)
            throw new InvalidOperationException("Observed native menu label was absent: " + expected);
        return found;
    }

    private static void VerifyOwner(nint hwnd, int processId)
    {
        if (hwnd == 0 || GetWindowThreadProcessId(hwnd, out var owner) == 0 || owner != (uint)processId)
            throw new InvalidOperationException("Observed command HWND no longer belongs to the GUI host");
    }
}
