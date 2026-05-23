using System;
using System.Collections.Generic;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Media;
using Avalonia.Media.TextFormatting;
using Avalonia.Threading;
using Avalonia.VisualTree;
using CaseChampGui.Models;
using CaseChampGui.ViewModels;

namespace CaseChampGui.Services;

/// <summary>
/// Ghost Intellisense для TextBox: серый хвост поверх редактора, Tab для принятия.
/// </summary>
public sealed class SqlIntellisenseController : IDisposable
{
    private readonly TextBox _editor;
    private readonly TextBlock _ghost;
    private readonly Func<IReadOnlyList<SchemaTable>> _getSchema;
    private readonly Func<AppSettings> _getSettings;
    private readonly SqlIntellisenseMode _mode;

    private string _ghostSuffix = string.Empty;
    private bool _ghostVisible;
    private bool _suppressed;
    private int _typedSinceDelete;
    private bool _attached;
    private bool _updatingGhost;
    private bool _ghostUpdateScheduled;
    private ScrollViewer? _scrollViewer;

    public SqlIntellisenseController(
        TextBox editor,
        TextBlock ghost,
        Func<IReadOnlyList<SchemaTable>> getSchema,
        Func<AppSettings> getSettings,
        SqlIntellisenseMode mode)
    {
        _editor = editor;
        _ghost = ghost;
        _getSchema = getSchema;
        _getSettings = getSettings;
        _mode = mode;
    }

    public void Refresh() => ScheduleGhostUpdate();

    public void Attach()
    {
        if (_attached) return;
        _attached = true;

        _editor.TextChanged += OnTextChanged;
        _editor.PropertyChanged += OnEditorPropertyChanged;
        _editor.AddHandler(InputElement.KeyDownEvent, OnKeyDown, RoutingStrategies.Tunnel, handledEventsToo: true);
        _editor.LostFocus += OnLostFocus;
        _editor.GotFocus += (_, _) => ScheduleGhostUpdate();
        _editor.AttachedToVisualTree += OnEditorAttachedToVisualTree;
        _editor.DetachedFromVisualTree += OnEditorDetachedFromVisualTree;

        SyncGhostTypography();
        _editor.Tag = _editor.Text?.Length ?? 0;
        ScheduleGhostUpdate();
    }

    public void Detach()
    {
        if (!_attached) return;
        _attached = false;

        _editor.TextChanged -= OnTextChanged;
        _editor.PropertyChanged -= OnEditorPropertyChanged;
        _editor.RemoveHandler(InputElement.KeyDownEvent, OnKeyDown);
        _editor.LostFocus -= OnLostFocus;
        _editor.AttachedToVisualTree -= OnEditorAttachedToVisualTree;
        _editor.DetachedFromVisualTree -= OnEditorDetachedFromVisualTree;
        UnhookScrollViewer();
        HideGhost();
    }

    public void Dispose() => Detach();

    private AppSettings Settings => _getSettings();

    private bool IsEnabled => Settings.IntellisenseEnabled;

    private void OnEditorAttachedToVisualTree(object? sender, VisualTreeAttachmentEventArgs e)
    {
        _scrollViewer = _editor.GetVisualDescendants().OfType<ScrollViewer>().FirstOrDefault();
        if (_scrollViewer is not null)
            _scrollViewer.ScrollChanged += OnScrollChanged;
    }

    private void OnEditorDetachedFromVisualTree(object? sender, VisualTreeAttachmentEventArgs e)
        => UnhookScrollViewer();

    private void UnhookScrollViewer()
    {
        if (_scrollViewer is not null)
            _scrollViewer.ScrollChanged -= OnScrollChanged;
        _scrollViewer = null;
    }

    private void OnScrollChanged(object? sender, ScrollChangedEventArgs e)
        => ScheduleGhostUpdate();

    private void OnLostFocus(object? sender, RoutedEventArgs e) => HideGhost();

    private void OnEditorPropertyChanged(object? sender, AvaloniaPropertyChangedEventArgs e)
    {
        if (e.Property == TextBox.FontSizeProperty ||
            e.Property == TextBox.PaddingProperty ||
            e.Property == TextBox.FontFamilyProperty)
        {
            SyncGhostTypography();
            if (_ghostVisible)
                ScheduleGhostUpdate();
        }
    }

    private void SyncGhostTypography()
    {
        _ghost.FontFamily = _editor.FontFamily;
        _ghost.FontSize = _editor.FontSize;
        _ghost.TextWrapping = TextWrapping.NoWrap;
        _ghost.Padding = new Thickness(0);
        _ghost.LineHeight = double.NaN;
    }

    private void OnTextChanged(object? sender, EventArgs e)
    {
        var prevLen = (_editor.Tag as int?) ?? 0;
        var newLen = _editor.Text?.Length ?? 0;
        if (newLen < prevLen)
        {
            _suppressed = true;
            _typedSinceDelete = 0;
            HideGhost();
        }
        else if (newLen > prevLen && !_suppressed)
        {
            ScheduleGhostUpdate();
        }
        else if (newLen > prevLen && _suppressed)
        {
            var delta = newLen - prevLen;
            _typedSinceDelete += delta;
            if (_typedSinceDelete >= 2)
                _suppressed = false;
            if (!_suppressed)
                ScheduleGhostUpdate();
            else
                HideGhost();
        }

        _editor.Tag = newLen;
    }

    private void ScheduleGhostUpdate()
    {
        if (_ghostUpdateScheduled) return;
        _ghostUpdateScheduled = true;
        Dispatcher.UIThread.Post(() =>
        {
            _ghostUpdateScheduled = false;
            UpdateGhost();
        }, DispatcherPriority.Input);
    }

    private void OnKeyDown(object? sender, KeyEventArgs e)
    {
        if (!IsEnabled)
        {
            HideGhost();
            return;
        }

        if (e.Key == Key.Tab && _ghostVisible && !string.IsNullOrEmpty(_ghostSuffix))
        {
            AcceptGhost();
            e.Handled = true;
            return;
        }

        if (e.Key == Key.Escape && _ghostVisible)
        {
            _suppressed = true;
            _typedSinceDelete = 0;
            HideGhost();
            e.Handled = true;
            return;
        }

        if (e.Key == Key.Back)
        {
            if (_ghostVisible && Settings.IntellisenseBackspaceDismissesGhost)
            {
                HideGhost();
                _suppressed = true;
                _typedSinceDelete = 0;
                e.Handled = true;
                return;
            }

            _suppressed = true;
            _typedSinceDelete = 0;
            HideGhost();
            return;
        }

        if (e.Key == Key.Delete)
        {
            _suppressed = true;
            _typedSinceDelete = 0;
            HideGhost();
            return;
        }

        if (e.Key is Key.Enter or Key.Return)
            ScheduleGhostUpdate();
    }

    private void AcceptGhost()
    {
        var text = _editor.Text ?? string.Empty;
        var caret = Math.Clamp(_editor.CaretIndex, 0, text.Length);
        var suffix = _ghostSuffix;
        if (string.IsNullOrEmpty(suffix)) return;

        _editor.Text = text.Insert(caret, suffix);
        var newCaret = caret + suffix.Length;
        _editor.CaretIndex = newCaret;
        _editor.SelectionStart = newCaret;
        _editor.SelectionEnd = newCaret;
        _editor.Tag = _editor.Text?.Length ?? 0;

        _suppressed = false;
        _typedSinceDelete = 0;
        HideGhost();
        ScheduleGhostUpdate();
    }

    private void UpdateGhost()
    {
        if (_updatingGhost) return;

        try
        {
            _updatingGhost = true;

            if (!IsEnabled || _suppressed)
            {
                HideGhost();
                return;
            }

            var text = _editor.Text ?? string.Empty;
            var caret = Math.Clamp(_editor.CaretIndex, 0, text.Length);
            var selStart = Math.Clamp(_editor.SelectionStart, 0, text.Length);
            var selEnd = Math.Clamp(_editor.SelectionEnd, 0, text.Length);
            var selLen = Math.Max(0, selEnd - selStart);
            if (selLen > 0)
            {
                HideGhost();
                return;
            }

            var suggestion = SqlIntellisenseEngine.Compute(
                text,
                caret,
                selStart,
                selLen,
                _getSchema(),
                _mode);

            if (suggestion is null || string.IsNullOrEmpty(suggestion.GhostSuffix))
            {
                HideGhost();
                return;
            }

            _ghostSuffix = suggestion.GhostSuffix;
            _ghostVisible = true;
            RenderGhost(text, caret);
            _ghost.IsVisible = true;
        }
        catch
        {
            HideGhost();
        }
        finally
        {
            _updatingGhost = false;
        }
    }

    private void RenderGhost(string text, int caret)
    {
        caret = Math.Clamp(caret, 0, text.Length);
        var lineStart = caret == 0 ? 0 : text.LastIndexOf('\n', caret - 1) + 1;
        if (lineStart < 0) lineStart = 0;

        var prefixOnLine = text[lineStart..caret];
        var lineIndex = CountNewlinesBefore(text, caret);
        var prefixWidth = MeasureTextWidth(prefixOnLine);
        var lineHeight = EstimateLineHeight();

        var padding = _editor.Padding;
        var scrollX = _scrollViewer?.Offset.X ?? 0;
        var scrollY = _scrollViewer?.Offset.Y ?? 0;

        _ghost.Text = _ghostSuffix;
        _ghost.Foreground = GetGhostBrush();
        _ghost.Opacity = 0.55;
        _ghost.Padding = new Thickness(0);
        _ghost.Margin = new Thickness(
            padding.Left + prefixWidth - scrollX,
            padding.Top + lineIndex * lineHeight - scrollY,
            0,
            0);
    }

    private double EstimateLineHeight()
    {
        var size = _editor.FontSize;
        if (double.IsNaN(size) || size <= 0) size = 14;
        return size;
    }

    private static int CountNewlinesBefore(string text, int caret)
    {
        var count = 0;
        var limit = Math.Min(caret, text.Length);
        for (var i = 0; i < limit; i++)
        {
            if (text[i] == '\n')
                count++;
        }
        return count;
    }

    private double MeasureTextWidth(string text)
    {
        if (string.IsNullOrEmpty(text)) return 0;

        var fontFamily = _editor.FontFamily ?? FontFamily.Default;
        var fontSize = _editor.FontSize;
        if (double.IsNaN(fontSize) || fontSize <= 0) fontSize = 14;

        var typeface = new Typeface(fontFamily, FontStyle.Normal, FontWeight.Normal);
        using var layout = new TextLayout(
            text,
            typeface,
            fontSize,
            Brushes.Black,
            maxWidth: double.PositiveInfinity);
        return layout.WidthIncludingTrailingWhitespace;
    }

    private IBrush GetGhostBrush()
    {
        if (Application.Current?.TryFindResource("MutedTextBrush", out var brush) == true && brush is IBrush b)
            return b;
        return Brushes.Gray;
    }

    private void HideGhost()
    {
        _ghostVisible = false;
        _ghostSuffix = string.Empty;
        _ghost.Text = string.Empty;
        _ghost.IsVisible = false;
    }
}

/// <summary>Подключает Intellisense к SQL/Text2SQL редакторам.</summary>
public static class SqlIntellisenseSetup
{
    public static SqlIntellisenseController? Attach(
        TextBox? editor,
        TextBlock? ghost,
        SchemaPaneViewModel? schema,
        Func<AppSettings> getSettings,
        SqlIntellisenseMode mode)
    {
        if (editor is null || ghost is null || schema is null)
            return null;

        var controller = new SqlIntellisenseController(
            editor,
            ghost,
            () => schema.GetSchemaTables(),
            getSettings,
            mode);
        controller.Attach();
        return controller;
    }
}
