using System;
using System.Collections.ObjectModel;
using Avalonia.Threading;

namespace CaseChampGui.Services;

public enum NotificationKind
{
    Info,
    Warning,
    Error,
}

public sealed class NotificationItem
{
    public Guid Id { get; } = Guid.NewGuid();
    public string Title { get; init; } = string.Empty;
    public string Message { get; init; } = string.Empty;
    public NotificationKind Kind { get; init; } = NotificationKind.Info;
    public DateTime Created { get; init; } = DateTime.Now;

    public bool IsError => Kind == NotificationKind.Error;
    public bool IsWarning => Kind == NotificationKind.Warning;
    public bool IsInfo => Kind == NotificationKind.Info;

    public string Icon => Kind switch
    {
        NotificationKind.Error => "✕",
        NotificationKind.Warning => "!",
        _ => "ℹ",
    };
}

public sealed class NotificationService
{
    public ObservableCollection<NotificationItem> Items { get; } = new();

    public void Push(string title, string message, NotificationKind kind = NotificationKind.Info, TimeSpan? autoDismiss = null)
    {
        void DoPush()
        {
            var item = new NotificationItem
            {
                Title = title,
                Message = message,
                Kind = kind,
            };
            Items.Insert(0, item);

            var delay = autoDismiss ?? (kind == NotificationKind.Error ? TimeSpan.FromSeconds(12) : TimeSpan.FromSeconds(6));
            DispatcherTimer.RunOnce(() => Dismiss(item.Id), delay);
        }

        if (Dispatcher.UIThread.CheckAccess()) DoPush();
        else Dispatcher.UIThread.Post(DoPush);
    }

    public void Dismiss(Guid id)
    {
        for (var i = 0; i < Items.Count; i++)
        {
            if (Items[i].Id == id)
            {
                Items.RemoveAt(i);
                return;
            }
        }
    }
}
