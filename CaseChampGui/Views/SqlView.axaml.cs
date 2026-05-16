using System;
using System.Collections.Generic;
using System.Collections.Specialized;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Threading;
using CaseChampGui.ViewModels;

namespace CaseChampGui.Views;

public partial class SqlView : UserControl
{
    private const double MinToolbarWidthForShortcutHint = 620;

    private SqlViewModel? _vm;
    private Control? _editorToolbar;

    public SqlView()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
        AttachedToVisualTree += OnAttachedToVisualTree;
        DetachedFromVisualTree += OnDetachedFromVisualTree;
        ActualThemeVariantChanged += (_, _) => RebuildResultsTable();
    }

    private void OnAttachedToVisualTree(object? sender, VisualTreeAttachmentEventArgs e)
    {
        AttachEditorShortcuts(this.FindControl<TextBox>("EditorTextBox"));
        AttachEditorShortcuts(this.FindControl<TextBox>("ChatInputBox"));
        RebuildResultsTable();

        _editorToolbar = this.FindControl<Control>("EditorToolbar");
        if (_editorToolbar is not null)
        {
            _editorToolbar.SizeChanged += OnEditorToolbarSizeChanged;
            UpdateShortcutHintVisibility();
        }
    }

    private void OnDetachedFromVisualTree(object? sender, VisualTreeAttachmentEventArgs e)
    {
        if (_editorToolbar is not null)
        {
            _editorToolbar.SizeChanged -= OnEditorToolbarSizeChanged;
            _editorToolbar = null;
        }
    }

    private void OnEditorToolbarSizeChanged(object? sender, SizeChangedEventArgs e)
        => UpdateShortcutHintVisibility();

    private void UpdateShortcutHintVisibility()
    {
        var hint = this.FindControl<TextBlock>("CtrlEnterHint");
        if (hint is null || _editorToolbar is null) return;

        var width = _editorToolbar.Bounds.Width;
        hint.IsVisible = width <= 0 || width >= MinToolbarWidthForShortcutHint;
    }

    private void AttachEditorShortcuts(TextBox? box)
    {
        if (box is null) return;
        box.RemoveHandler(InputElement.KeyDownEvent, OnEditorKeyDown);
        box.AddHandler(
            InputElement.KeyDownEvent,
            OnEditorKeyDown,
            RoutingStrategies.Tunnel | RoutingStrategies.Bubble,
            handledEventsToo: true);
    }

    private void OnEditorKeyDown(object? sender, KeyEventArgs e)
    {
        var isEnter = e.Key == Key.Enter || e.Key == Key.Return;
        var ctrlDown = (e.KeyModifiers & KeyModifiers.Control) == KeyModifiers.Control;
        if (!isEnter || !ctrlDown) return;

        if (_vm?.ExecuteCommand.CanExecute(null) == true)
        {
            _vm.ExecuteCommand.Execute(null);
            e.Handled = true;
        }
    }

    private void OnDataContextChanged(object? sender, EventArgs e)
    {
        if (_vm is not null)
        {
            _vm.ResultsChanged -= OnResultsChanged;
            ((INotifyCollectionChanged)_vm.Messages).CollectionChanged -= OnMessagesChanged;
        }

        _vm = DataContext as SqlViewModel;

        if (_vm is not null)
        {
            _vm.ResultsChanged += OnResultsChanged;
            ((INotifyCollectionChanged)_vm.Messages).CollectionChanged += OnMessagesChanged;
            RebuildResultsTable();
        }
    }

    private void OnResultsChanged(object? sender, EventArgs e)
    {
        if (Dispatcher.UIThread.CheckAccess())
            RebuildResultsTable();
        else
            Dispatcher.UIThread.Post(RebuildResultsTable);
    }

    private void OnMessagesChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        Dispatcher.UIThread.Post(() =>
        {
            var scroll = this.FindControl<ScrollViewer>("ChatScroll");
            scroll?.ScrollToEnd();
        }, DispatcherPriority.Background);
    }

    private void RebuildResultsTable()
    {
        var host = this.FindControl<Grid>("ResultsGrid");
        if (host is null || _vm is null) return;

        IList<IList<string>> rowsView = new List<IList<string>>(_vm.Rows.Count);
        foreach (var row in _vm.Rows) rowsView.Add(row);

        ResultTableBuilder.Build(host, _vm.Columns, rowsView);
    }

    private void OnHistoryItemClick(object? sender, RoutedEventArgs e)
    {
        if (_vm is null) return;
        if (sender is not Control ctl) return;
        if (ctl.DataContext is not QueryHistoryItem item) return;
        _vm.SqlText = item.Sql;
        Avalonia.Controls.Primitives.FlyoutBase.GetAttachedFlyout(ctl)?.Hide();
        var p = ctl.Parent;
        while (p is not null)
        {
            if (p is Button btn && btn.Flyout is { } flyout)
            {
                flyout.Hide();
                break;
            }
            p = p.Parent;
        }
        var editor = this.FindControl<TextBox>("EditorTextBox");
        editor?.Focus();
    }
}
