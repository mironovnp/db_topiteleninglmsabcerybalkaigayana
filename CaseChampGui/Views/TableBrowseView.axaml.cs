using System;
using System.Collections.Generic;
using Avalonia.Controls;
using Avalonia.Threading;
using CaseChampGui.ViewModels;

namespace CaseChampGui.Views;

public partial class TableBrowseView : UserControl
{
    private TableBrowseViewModel? _vm;
    private ScrollViewer? _scroll;

    public TableBrowseView()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
        AttachedToVisualTree += (_, _) =>
        {
            _scroll = this.FindControl<ScrollViewer>("DataScroll");
            if (_scroll is not null)
                _scroll.SizeChanged += OnViewportSizeChanged;
            RebuildGrid();
        };
        DetachedFromVisualTree += (_, _) =>
        {
            if (_scroll is not null)
                _scroll.SizeChanged -= OnViewportSizeChanged;
        };
    }

    private void OnDataContextChanged(object? sender, EventArgs e)
    {
        if (_vm is not null)
            _vm.GridChanged -= OnGridChanged;

        _vm = DataContext as TableBrowseViewModel;

        if (_vm is not null)
        {
            _vm.GridChanged += OnGridChanged;
            RebuildGrid();
        }
    }

    private void OnGridChanged(object? sender, EventArgs e)
    {
        if (Dispatcher.UIThread.CheckAccess())
            RebuildGrid();
        else
            Dispatcher.UIThread.Post(RebuildGrid);
    }

    private void OnViewportSizeChanged(object? sender, SizeChangedEventArgs e) => RebuildGrid();

    private void RebuildGrid()
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

        IList<IList<string>> rowsView = new List<IList<string>>(_vm.Rows.Count);
        foreach (var row in _vm.Rows)
            rowsView.Add(row);

        BrowseGridBuilder.Build(host, _vm.Columns, rowsView, viewportW, viewportH);
        host.MinHeight = viewportH;
    }
}
