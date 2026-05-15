using System;
using Avalonia.Controls;
using Avalonia.Threading;

namespace CaseChampGui.Views;

public partial class LoadingWindow : Window
{
    private int _dotPhase;
    private DispatcherTimer? _timer;

    public LoadingWindow()
    {
        InitializeComponent();
        Opened += OnOpened;
        Closed += OnClosed;
    }

    private void OnOpened(object? sender, EventArgs e)
    {
        _dotPhase = 0;
        _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(400) };
        _timer.Tick += (_, _) =>
        {
            _dotPhase = (_dotPhase + 1) % 4;
            if (LoadingText is not null)
            {
                LoadingText.Text = _dotPhase switch
                {
                    0 => "загрузка",
                    1 => "загрузка .",
                    2 => "загрузка ..",
                    _ => "загрузка ...",
                };
            }
        };
        _timer.Start();
    }

    private void OnClosed(object? sender, EventArgs e)
    {
        if (_timer is null) return;
        _timer.Stop();
        _timer = null;
    }
}
