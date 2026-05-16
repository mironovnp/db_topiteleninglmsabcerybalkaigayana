using System.Collections.Generic;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace CaseChampGui.Views;

/// <summary>
/// Read-only grid with Excel-like faint lines extending into empty viewport area.
/// </summary>
internal static class BrowseGridBuilder
{
    private const double ColWidth = 132;
    private const double RowHeight = 32;
    private const double HeaderHeight = 36;

    private static readonly FontFamily MonoFont = new("Cascadia Code, JetBrains Mono, Menlo, Consolas, monospace");

    public static void Build(
        Grid host,
        IList<string> columns,
        IList<IList<string>> rows,
        double viewportWidth,
        double viewportHeight)
    {
        host.RowDefinitions.Clear();
        host.ColumnDefinitions.Clear();
        host.Children.Clear();

        if (columns is null || columns.Count == 0) return;

        var dataColCount = columns.Count;
        var dataRowCount = rows?.Count ?? 0;
        var totalDataRows = 1 + dataRowCount;

        var gridBrush = LookupBrush(host, "DividerBrush") ?? LookupBrush(host, "BorderSoftBrush");
        var headerBg = LookupBrush(host, "PanelBrush");
        var surfaceBg = LookupBrush(host, "SurfaceBrush");
        var textBrush = LookupBrush(host, "TextBrush");
        var altBrush = LookupBrush(host, "PanelBrush");

        var usableW = viewportWidth > 0 ? viewportWidth : dataColCount * ColWidth;
        var usableH = viewportHeight > 0 ? viewportHeight : totalDataRows * RowHeight;
        var totalCols = dataColCount;
        if (usableW > dataColCount * ColWidth)
            totalCols = System.Math.Max(dataColCount, (int)(usableW / ColWidth));

        var totalRows = totalDataRows;
        if (usableH > totalDataRows * RowHeight)
            totalRows = System.Math.Max(totalDataRows, (int)(usableH / RowHeight));

        for (var c = 0; c < totalCols; c++)
            host.ColumnDefinitions.Add(new ColumnDefinition(new GridLength(ColWidth)));

        for (var r = 0; r < totalRows; r++)
            host.RowDefinitions.Add(new RowDefinition(r == 0 ? new GridLength(HeaderHeight) : new GridLength(RowHeight)));

        for (var r = 0; r < totalRows; r++)
        {
            for (var c = 0; c < totalCols; c++)
            {
                var isHeader = r == 0;
                var isData = r > 0 && r <= dataRowCount;
                var isDataCol = c < dataColCount;

                IBrush? bg = surfaceBg;
                if (isHeader)
                    bg = headerBg;
                else if (isData && (r - 1) % 2 == 1 && altBrush is not null)
                    bg = altBrush;

                var border = new Border
                {
                    Background = bg,
                    BorderBrush = gridBrush,
                    BorderThickness = new Thickness(0, 0, 1, 1),
                    Opacity = isHeader || isData ? 1.0 : 0.55,
                };

                if (isHeader && isDataCol)
                {
                    border.Child = new TextBlock
                    {
                        Text = columns[c] ?? string.Empty,
                        FontWeight = FontWeight.SemiBold,
                        FontSize = 12,
                        Foreground = textBrush,
                        Margin = new Thickness(10, 0, 12, 0),
                        VerticalAlignment = VerticalAlignment.Center,
                    };
                }
                else if (isData && isDataCol)
                {
                    var row = rows![r - 1];
                    var text = c < row.Count ? (row[c] ?? string.Empty) : string.Empty;
                    border.Child = new TextBlock
                    {
                        Text = text,
                        FontFamily = MonoFont,
                        FontSize = 13,
                        Foreground = textBrush,
                        Margin = new Thickness(10, 0, 12, 0),
                        VerticalAlignment = VerticalAlignment.Center,
                        TextTrimming = TextTrimming.CharacterEllipsis,
                    };
                }

                Grid.SetRow(border, r);
                Grid.SetColumn(border, c);
                host.Children.Add(border);
            }
        }
    }

    private static IBrush? LookupBrush(StyledElement host, string key)
    {
        if (host.TryFindResource(key, host.ActualThemeVariant, out var value) && value is IBrush brush)
            return brush;
        if (Application.Current is { } app
            && app.TryFindResource(key, app.ActualThemeVariant, out var appValue)
            && appValue is IBrush appBrush)
            return appBrush;
        return null;
    }
}
