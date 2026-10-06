using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Automation;

namespace Aegisub.GuiAutomation.Driver;

public sealed record WindowCaptureEvidence(
    int Width,
    int Height,
    long NonBlackPixelCount,
    long MinimumNonBlackPixelCount);

public static class ScreenCapture
{
    [StructLayout(LayoutKind.Sequential)]
    private struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(nint window, out Rect rect);

    [DllImport("user32.dll")]
    private static extern bool PrintWindow(nint window, nint dc, uint flags);

    public static WindowCaptureEvidence SaveWindowPng(AutomationElement element, string path)
        => SaveWindowPng(new nint(element.Current.NativeWindowHandle), path);

    public static WindowCaptureEvidence SaveWindowPng(nint handle, string path)
    {
        if (handle == 0 || !GetWindowRect(handle, out var rect))
            throw new InvalidOperationException("UIA element has no capturable window handle");
        var width = rect.Right - rect.Left;
        var height = rect.Bottom - rect.Top;
        if (width <= 0 || height <= 0)
            throw new InvalidOperationException("UIA window has no drawable area");

        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        using var bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb);
        using var graphics = Graphics.FromImage(bitmap);
        var dc = graphics.GetHdc();
        try
        {
            if (!PrintWindow(handle, dc, 0))
                throw new InvalidOperationException("PrintWindow could not capture the UIA window");
        }
        finally
        {
            graphics.ReleaseHdc(dc);
        }
        bitmap.Save(path, ImageFormat.Png);
        return ValidateSavedPng(path, width, height);
    }

    public static WindowCaptureEvidence ValidateSavedPng(
        string path,
        int expectedWidth,
        int expectedHeight)
    {
        if (expectedWidth <= 0 || expectedHeight <= 0)
            throw new ArgumentOutOfRangeException(
                nameof(expectedWidth), "Expected PNG dimensions must be positive");

        using var persisted = new Bitmap(path);
        if (persisted.Width != expectedWidth || persisted.Height != expectedHeight)
            throw new InvalidOperationException(
                $"Saved PNG dimensions {persisted.Width}x{persisted.Height} do not match " +
                $"the captured window {expectedWidth}x{expectedHeight}");

        var nonBlackPixelCount = CountNonBlackPixels(persisted);
        var minimumNonBlackPixelCount = Math.Max(
            1L,
            (long)persisted.Width * persisted.Height / 10_000L);
        if (nonBlackPixelCount < minimumNonBlackPixelCount)
            throw new InvalidOperationException(
                $"Saved PNG has only {nonBlackPixelCount} non-black pixels; " +
                $"at least {minimumNonBlackPixelCount} are required");

        return new WindowCaptureEvidence(
            persisted.Width,
            persisted.Height,
            nonBlackPixelCount,
            minimumNonBlackPixelCount);
    }

    private static long CountNonBlackPixels(Bitmap source)
    {
        using var bitmap = source.Clone(
            new Rectangle(0, 0, source.Width, source.Height),
            PixelFormat.Format32bppArgb);
        var area = new Rectangle(0, 0, bitmap.Width, bitmap.Height);
        var data = bitmap.LockBits(area, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        try
        {
            var stride = Math.Abs(data.Stride);
            var pixels = new byte[stride * bitmap.Height];
            Marshal.Copy(data.Scan0, pixels, 0, pixels.Length);
            long nonBlack = 0;
            for (var y = 0; y < bitmap.Height; ++y)
            {
                var row = data.Stride >= 0 ? y * stride : (bitmap.Height - y - 1) * stride;
                for (var x = 0; x < bitmap.Width; ++x)
                {
                    var pixel = row + x * 4;
                    if (pixels[pixel] != 0
                        || pixels[pixel + 1] != 0
                        || pixels[pixel + 2] != 0)
                        ++nonBlack;
                }
            }
            return nonBlack;
        }
        finally
        {
            bitmap.UnlockBits(data);
        }
    }
}
