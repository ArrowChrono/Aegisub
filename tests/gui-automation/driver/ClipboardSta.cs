using System.Windows.Forms;

namespace Aegisub.GuiAutomation.Driver;

public static class ClipboardSta
{
    private static readonly Lazy<Control> dispatcher = new(() =>
    {
        var ready = new TaskCompletionSource<Control>(TaskCreationOptions.RunContinuationsAsynchronously);
        var thread = new Thread(() =>
        {
            try
            {
                using var control = new Control();
                _ = control.Handle;
                ready.SetResult(control);
                Application.Run();
            }
            catch (Exception error) { ready.TrySetException(error); }
        });
        thread.SetApartmentState(ApartmentState.STA);
        thread.IsBackground = true;
        thread.Start();
        if (!ready.Task.Wait(TimeSpan.FromSeconds(5))) throw new TimeoutException("Clipboard STA dispatcher did not start");
        return ready.Task.GetAwaiter().GetResult();
    });

    public static T Invoke<T>(Func<T> operation)
    {
        var completed = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        dispatcher.Value.BeginInvoke(new Action(() =>
        {
            try { completed.SetResult(operation()); }
            catch (Exception error) { completed.SetException(error); }
        }));
        if (!completed.Task.Wait(TimeSpan.FromSeconds(5))) throw new TimeoutException("STA clipboard operation exceeded five seconds");
        return completed.Task.GetAwaiter().GetResult();
    }
}
