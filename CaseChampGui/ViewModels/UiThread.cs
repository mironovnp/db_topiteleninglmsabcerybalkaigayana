using System;
using System.Threading.Tasks;
using Avalonia.Threading;

namespace CaseChampGui.ViewModels;

internal static class UiThread
{
    public static void Post(Action action)
    {
        if (Dispatcher.UIThread.CheckAccess())
            action();
        else
            Dispatcher.UIThread.Post(action);
    }

    public static async Task RunAsync(Action action)
    {
        if (Dispatcher.UIThread.CheckAccess())
        {
            action();
            return;
        }

        await Dispatcher.UIThread.InvokeAsync(action);
    }

    public static async Task RunAsync(Func<Task> action)
    {
        if (Dispatcher.UIThread.CheckAccess())
        {
            await action().ConfigureAwait(true);
            return;
        }

        await Dispatcher.UIThread.InvokeAsync(action);
    }
}
