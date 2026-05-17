using System;
using System.Collections.Generic;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Layout;
using Avalonia.Media;
using CaseChampGui.ViewModels;

namespace CaseChampGui.Views;

internal static class BrowseGridBuilder
{
    private const double ColWidth = 132;
    private const double RowHeight = 32;
    private const double HeaderHeight = 36;

    private static readonly FontFamily MonoFont = new("Cascadia Code, JetBrains Mono, Menlo, Consolas, monospace");

    public static double Build(
        Grid host,
        IList<string> columns,
        IList<IList<string>> rows,
        double viewportWidth,
        double viewportHeight,
        TableBrowseViewModel? viewModel)
    {
        host.RowDefinitions.Clear();
        host.ColumnDefinitions.Clear();
        host.Children.Clear();

        if (columns is null || columns.Count == 0) return 0;

        var dataColCount = columns.Count;
        var dataRowCount = rows?.Count ?? 0;
        var totalDataRows = 1 + dataRowCount;

        var gridBrush = LookupBrush(host, "DividerBrush") ?? LookupBrush(host, "BorderSoftBrush");
        var headerBg = LookupBrush(host, "PanelBrush");
        var surfaceBg = LookupBrush(host, "SurfaceBrush");
        var textBrush = LookupBrush(host, "TextBrush");
        var accentBrush = LookupBrush(host, "AccentBrush");
        var altBrush = LookupBrush(host, "PanelBrush");

        var usableW = viewportWidth > 0 ? viewportWidth : dataColCount * ColWidth;
        var usableH = viewportHeight > 0 ? viewportHeight : totalDataRows * RowHeight;

        var dataWidth = dataColCount * ColWidth;
        var extendToViewport = usableW > dataWidth + 0.5;

        var rightPartial = 0.0;
        var hasRightPartial = false;
        var fullColSlots = dataColCount;

        if (extendToViewport)
        {
            var fullSlots = (int)Math.Floor(usableW / ColWidth);
            rightPartial = usableW - fullSlots * ColWidth;
            hasRightPartial = rightPartial >= 1;
            fullColSlots = Math.Max(dataColCount, fullSlots);
        }

        var totalCols = fullColSlots + (hasRightPartial ? 1 : 0);

        var totalRows = totalDataRows;
        if (usableH > totalDataRows * RowHeight)
            totalRows = Math.Max(totalDataRows, (int)(usableH / RowHeight));

        for (var c = 0; c < totalCols; c++)
        {
            var isRightPartialCol = hasRightPartial && c == totalCols - 1;
            var width = isRightPartialCol
                ? new GridLength(rightPartial, GridUnitType.Pixel)
                : new GridLength(ColWidth);
            host.ColumnDefinitions.Add(new ColumnDefinition(width));
        }

        for (var r = 0; r < totalRows; r++)
            host.RowDefinitions.Add(new RowDefinition(r == 0 ? new GridLength(HeaderHeight) : new GridLength(RowHeight)));

        for (var r = 0; r < totalRows; r++)
        {
            for (var c = 0; c < totalCols; c++)
            {
                var isHeader = r == 0;
                var isData = r > 0 && r <= dataRowCount;
                var isRightPartialCol = hasRightPartial && c == totalCols - 1;
                var slotIndex = c;
                var isDataCol = !isRightPartialCol && slotIndex < dataColCount;
                var isFillerCol = !isRightPartialCol && slotIndex >= dataColCount;

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
                    var colName = columns[slotIndex] ?? string.Empty;
                    var header = new Grid
                    {
                        ColumnDefinitions =
                        {
                            new ColumnDefinition(new GridLength(1, GridUnitType.Star)),
                            new ColumnDefinition(GridLength.Auto),
                        },
                    };

                    var nameBlock = new TextBlock
                    {
                        Text = colName,
                        FontWeight = FontWeight.SemiBold,
                        FontSize = 12,
                        Foreground = textBrush,
                        Margin = new Thickness(10, 0, 4, 0),
                        VerticalAlignment = VerticalAlignment.Center,
                        TextTrimming = TextTrimming.CharacterEllipsis,
                    };
                    Grid.SetColumn(nameBlock, 0);
                    header.Children.Add(nameBlock);

                    if (viewModel is not null)
                    {
                        var isFiltered = viewModel.IsColumnFiltered(colName);
                        var columnForFilter = colName;
                        var filterBtn = new Button
                        {
                            Classes = { "ghost" },
                            Content = "▼",
                            FontSize = 10,
                            MinWidth = 26,
                            MinHeight = 24,
                            Padding = new Thickness(4, 2),
                            Margin = new Thickness(0, 0, 4, 0),
                            VerticalAlignment = VerticalAlignment.Center,
                            Foreground = isFiltered ? accentBrush : textBrush,
                        };
                        ToolTip.SetTip(filterBtn, "Фильтр столбца");

                        var flyout = new Flyout
                        {
                            Placement = PlacementMode.BottomEdgeAlignedLeft,
                            ShowMode = FlyoutShowMode.Transient,
                            Content = new ColumnFilterFlyoutContent { DataContext = viewModel },
                        };
                        FlyoutBase.SetAttachedFlyout(filterBtn, flyout);
                        filterBtn.Click += (_, _) =>
                        {
                            viewModel.BeginEditFilter(columnForFilter);
                            FlyoutBase.ShowAttachedFlyout(filterBtn);
                        };

                        Grid.SetColumn(filterBtn, 1);
                        header.Children.Add(filterBtn);
                    }

                    border.Child = header;
                }
                else if (isData && isDataCol)
                {
                    var row = rows![r - 1];
                    var text = slotIndex < row.Count ? (row[slotIndex] ?? string.Empty) : string.Empty;
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
                else if (isRightPartialCol || isFillerCol)
                {
                    border.Opacity = 0.55;
                }

                Grid.SetRow(border, r);
                Grid.SetColumn(border, c);
                host.Children.Add(border);
            }
        }

        var gridWidth = fullColSlots * ColWidth + (hasRightPartial ? rightPartial : 0);
        return Math.Max(gridWidth, dataWidth);
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
