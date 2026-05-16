using System;
using System.Collections.Generic;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Threading;
using Avalonia.Interactivity;
using CaseChampGui.ViewModels;

namespace CaseChampGui.Views;

public partial class Text2SqlView : UserControl
{
    private Text2SqlViewModel? _vm;

    public Text2SqlView()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
        AttachedToVisualTree += (_, _) => AttachShortcuts();
    }

    private void OnDataContextChanged(object? sender, EventArgs e)
    {
        if (_vm is not null)
            _vm.ResultsChanged -= OnResultsChanged;

        _vm = DataContext as Text2SqlViewModel;

        if (_vm is not null)
        {
            _vm.ResultsChanged += OnResultsChanged;
            RebuildResultsTable();
        }
    }

    private void AttachShortcuts()
    {
        var box = this.FindControl<TextBox>("RequestTextBox");
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

        if (_vm?.TranslateAndRunCommand.CanExecute(null) == true)
        {
            _vm.TranslateAndRunCommand.Execute(null);
            e.Handled = true;
        }
    }

    private void OnResultsChanged(object? sender, EventArgs e)
    {
        if (Dispatcher.UIThread.CheckAccess())
            RebuildResultsTable();
        else
            Dispatcher.UIThread.Post(RebuildResultsTable);
    }

    private void RebuildResultsTable()
    {
        var host = this.FindControl<Grid>("ResultsGrid");
        if (host is null || _vm is null) return;

        IList<IList<string>> rowsView = new List<IList<string>>(_vm.Rows.Count);
        foreach (var row in _vm.Rows) rowsView.Add(row);

        ResultTableBuilder.Build(host, _vm.Columns, rowsView);
    }
}
