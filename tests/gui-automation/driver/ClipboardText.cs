using System.ComponentModel;
using System.Runtime.InteropServices;

namespace Aegisub.GuiAutomation.Driver;

public static class ClipboardText
{
    [DllImport("user32.dll", SetLastError = true)] private static extern int OpenClipboard(nint owner);
    [DllImport("user32.dll", SetLastError = true)] private static extern int CloseClipboard();
    [DllImport("user32.dll", SetLastError = true)] private static extern nint GetClipboardData(uint format);
    [DllImport("user32.dll")] private static extern uint GetClipboardSequenceNumber();
    [DllImport("user32.dll")] private static extern nint GetClipboardOwner();
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(nint hwnd, out uint processId);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern nuint GlobalSize(nint memory);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern nint GlobalLock(nint memory);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern int GlobalUnlock(nint memory);

    public static string ReadUnicodeText(uint expectedSequence, int expectedOwnerPid)
        => ClipboardSta.Invoke(() => ReadOnSta(expectedSequence, expectedOwnerPid));

    private static string ReadOnSta(uint expectedSequence, int expectedOwnerPid)
    {
        var timer = System.Diagnostics.Stopwatch.StartNew();
        while (true)
        {
            VerifyOwnership(expectedSequence, expectedOwnerPid);
            if (OpenClipboard(0) != 0)
                break;
            var error = Marshal.GetLastPInvokeError();
            VerifyOwnership(expectedSequence, expectedOwnerPid);
            if (error != 5 || timer.Elapsed >= TimeSpan.FromMilliseconds(500))
                throw new Win32Exception(error, "CF_UNICODETEXT clipboard could not be opened within 500 ms");
            Thread.Sleep(15);
        }

        nint locked = 0;
        nint memory = 0;
        try
        {
            VerifyOwnership(expectedSequence, expectedOwnerPid);
            memory = GetClipboardData(13);
            if (memory == 0)
                throw new Win32Exception(Marshal.GetLastPInvokeError(), "CF_UNICODETEXT clipboard data is unavailable");
            var size = GlobalSize(memory);
            if (size == 0 || size > 8 * 1024 * 1024 || size % 2 != 0)
                throw new InvalidOperationException($"CF_UNICODETEXT has invalid byte size {size}");
            locked = GlobalLock(memory);
            if (locked == 0)
                throw new Win32Exception(Marshal.GetLastPInvokeError(), "CF_UNICODETEXT memory could not be locked");
            var characters = new char[(int)size / 2];
            Marshal.Copy(locked, characters, 0, characters.Length);
            var terminator = Array.IndexOf(characters, '\0');
            if (terminator < 0) throw new InvalidOperationException("CF_UNICODETEXT has no UTF-16 terminator within its allocation");
            return new string(characters, 0, terminator);
        }
        finally
        {
            if (locked != 0) _ = GlobalUnlock(memory);
            var closed = CloseClipboard();
            var closeError = Marshal.GetLastPInvokeError();
            VerifyOwnership(expectedSequence, expectedOwnerPid);
            if (closed == 0) throw new Win32Exception(closeError, "CF_UNICODETEXT clipboard could not be closed");
        }
    }

    private static void VerifyOwnership(uint expectedSequence, int expectedOwnerPid)
    {
        var owner = GetClipboardOwner();
        GetWindowThreadProcessId(owner, out var processId);
        if (GetClipboardSequenceNumber() != expectedSequence || processId != (uint)expectedOwnerPid)
            throw new InvalidOperationException("Clipboard sequence or owner changed while reading CF_UNICODETEXT");
    }
}
