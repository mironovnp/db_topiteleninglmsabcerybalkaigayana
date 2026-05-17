using System;
using System.Collections.Generic;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Threading;
using Avalonia.VisualTree;
using CaseChampGui.ViewModels;

namespace CaseChampGui.Views;

public partial class TableBrowseView : UserControl
{
    private static readonly TimeSpan GridRebuildDebounce = TimeSpan.FromMilliseconds(120);

    private TableBrowseViewModel? _vm;
    private ScrollViewer? _scroll;
    private DispatcherTimer? _rebuildDebounceTimer;
    private double _lastViewportW;
    private double _lastViewportH;

    public TableBrowseView()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
        AttachedToVisualTree += (_, _) =>
        {
            _scroll = this.FindControl<ScrollViewer>("DataScroll");
            if (_scroll is not null)
                _scroll.SizeChanged += OnViewportSizeChanged;
            RebuildGridNow();
        };
        DetachedFromVisualTree += (_, _) =>
        {
            if (_scroll is not null)
                _scroll.SizeChanged -= OnViewportSizeChanged;
            _rebuildDebounceTimer?.Stop();
        };
    }

    private void OnDataContextChanged(object? sender, EventArgs e)
    {
        if (_vm is not null)
        {
            _vm.GridChanged -= OnGridChanged;
            _vm.RequestCloseFilterFlyout -= OnRequestCloseFilterFlyout;
        }

        _vm = DataContext as TableBrowseViewModel;

        if (_vm is not null)
        {
            _vm.GridChanged += OnGridChanged;
            _vm.RequestCloseFilterFlyout += OnRequestCloseFilterFlyout;
            RebuildGridNow();
        }
    }

    private void OnRequestCloseFilterFlyout()
    {
        if (Dispatcher.UIThread.CheckAccess())
            CloseOpenFilterFlyouts();
        else
            Dispatcher.UIThread.Post(CloseOpenFilterFlyouts);
    }

    private void CloseOpenFilterFlyouts()
    {
        var host = this.FindControl<Grid>("DataGridHost");
        if (host is null) return;
        CloseFlyoutsInTree(host);
    }

    private static void CloseFlyoutsInTree(Control root)
    {
        if (root is Button btn)
            FlyoutBase.GetAttachedFlyout(btn)?.Hide();

        foreach (var child in root.GetVisualChildren())
        {
            if (child is Control c)
                CloseFlyoutsInTree(c);
        }
    }

    private void OnGridChanged(object? sender, EventArgs e)
    {
        if (Dispatcher.UIThread.CheckAccess())
            RebuildGridNow();
        else
            Dispatcher.UIThread.Post(RebuildGridNow);
    }

    private void OnViewportSizeChanged(object? sender, SizeChangedEventArgs e)
    {
        var w = _scroll?.Bounds.Width ?? 0;
        var h = _scroll?.Bounds.Height ?? 0;
        if (w < 1 || h < 1) return;

        if (Math.Abs(w - _lastViewportW) < 4 && Math.Abs(h - _lastViewportH) < 4)
            return;

        ScheduleRebuildGridDebounced();
    }

    private void ScheduleRebuildGridDebounced()
    {
        _rebuildDebounceTimer ??= new DispatcherTimer { Interval = GridRebuildDebounce };
        _rebuildDebounceTimer.Tick -= OnRebuildDebounceTick;
        _rebuildDebounceTimer.Tick += OnRebuildDebounceTick;
        _rebuildDebounceTimer.Stop();
        _rebuildDebounceTimer.Start();
    }

    private void OnRebuildDebounceTick(object? sender, EventArgs e)
    {
        _rebuildDebounceTimer?.Stop();
        RebuildGridNow();
    }

    private void RebuildGridNow()
    {
        var host = this.FindControl<Grid>("DataGridHost");
        if (host is null || _vm is null || !_vm.ShowGrid || _vm.Columns.Count == 0)
        {
            if (host is not null)
            {
                host.RowDefinitions.Clear();
                host.ColumnDefinitions.Clear();
                host.Children.Clear();
            }
            return;
        }

        var viewportW = _scroll?.Bounds.Width ?? 800;
        var viewportH = _scroll?.Bounds.Height ?? 480;
        if (viewportW < 1) viewportW = 800;
        if (viewportH < 1) viewportH = 480;

        _lastViewportW = viewportW;
        _lastViewportH = viewportH;

        IList<IList<string>> rowsView = new List<IList<string>>(_vm.Rows.Count);
        foreach (var row in _vm.Rows)
            rowsView.Add(row);

        var gridWidth = BrowseGridBuilder.Build(host, _vm.Columns, rowsView, viewportW, viewportH, _vm);
        host.MinHeight = viewportH;
        host.MinWidth = gridWidth > 0 ? gridWidth : viewportW;
    }
}
