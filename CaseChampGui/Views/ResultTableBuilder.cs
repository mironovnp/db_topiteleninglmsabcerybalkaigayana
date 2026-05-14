using System.Collections.Generic;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace CaseChampGui.Views;

internal static class ResultTableBuilder
{
    private static readonly FontFamily MonoFont = new("Cascadia Code, JetBrains Mono, Menlo, Consolas, monospace");

    public static void Build(Grid host, IList<string> columns, IList<IList<string>> rows)
    {
        host.RowDefinitions.Clear();
        host.ColumnDefinitions.Clear();
        host.Children.Clear();

        if (columns is null || columns.Count == 0) return;

        var colCount = columns.Count;
        var rowCount = rows?.Count ?? 0;

        for (var c = 0; c < colCount; c++)
        {
            host.ColumnDefinitions.Add(new ColumnDefinition(GridLength.Auto));
        }

        host.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
        for (var r = 0; r < rowCount; r++)
        {
            host.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
        }

        var headerBg = new Border
        {
            Background = LookupBrush(host, "PanelBrush"),
            BorderBrush = LookupBrush(host, "BorderSoftBrush"),
            BorderThickness = new Thickness(0, 0, 0, 1),
        };
        Grid.SetRow(headerBg, 0);
        Grid.SetColumn(headerBg, 0);
        Grid.SetColumnSpan(headerBg, colCount);
        host.Children.Add(headerBg);

        for (var c = 0; c < colCount; c++)
        {
            var header = new TextBlock
            {
                Text = columns[c] ?? string.Empty,
                FontWeight = FontWeight.SemiBold,
                FontSize = 12,
                Foreground = LookupBrush(host, "TextBrush"),
                Margin = new Thickness(14, 9, 18, 9),
                VerticalAlignment = VerticalAlignment.Center,
            };
            Grid.SetRow(header, 0);
            Grid.SetColumn(header, c);
            host.Children.Add(header);
        }

        if (rows is null) return;

        var altBrush = LookupBrush(host, "PanelBrush");
        for (var r = 0; r < rowCount; r++)
        {
            if (r % 2 == 1 && altBrush is not null)
            {
                var stripe = new Border
                {
                    Background = altBrush,
                    Opacity = 0.45,
                };
                Grid.SetRow(stripe, r + 1);
                Grid.SetColumn(stripe, 0);
                Grid.SetColumnSpan(stripe, colCount);
                host.Children.Add(stripe);
            }

            var row = rows[r];
            for (var c = 0; c < colCount; c++)
            {
                var text = c < row.Count ? (row[c] ?? string.Empty) : string.Empty;
                var cell = new TextBlock
                {
                    Text = text,
                    FontFamily = MonoFont,
                    FontSize = 13,
                    Foreground = LookupBrush(host, "TextBrush"),
                    Margin = new Thickness(14, 6, 18, 6),
                    VerticalAlignment = VerticalAlignment.Center,
                };
                Grid.SetRow(cell, r + 1);
                Grid.SetColumn(cell, c);
                host.Children.Add(cell);
            }
        }
    }

    private static IBrush? LookupBrush(StyledElement host, string key)
    {
        if (host.TryFindResource(key, host.ActualThemeVariant, out var value) && value is IBrush brush)
        {
            return brush;
        }
        if (Application.Current is { } app
            && app.TryFindResource(key, app.ActualThemeVariant, out var appValue)
            && appValue is IBrush appBrush)
        {
            return appBrush;
        }
        return null;
    }
}
