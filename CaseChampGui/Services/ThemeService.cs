using System;
using Avalonia;
using Avalonia.Styling;
using CaseChampGui.Models;
using CaseChampGui.Themes;

namespace CaseChampGui.Services;

public sealed class ThemeService
{
    public event EventHandler<AppTheme>? ThemeApplied;

    public AppTheme CurrentTheme { get; private set; } = AppTheme.System;

    public void Apply(AppTheme theme)
    {
        CurrentTheme = theme;
        var app = Application.Current;
        if (app is null) return;

        app.RequestedThemeVariant = theme switch
        {
            AppTheme.Light => ThemeVariant.Light,
            AppTheme.Dark => ThemeVariant.Dark,
            AppTheme.Iu5 => AppThemeVariants.Iu5,
            _ => ThemeVariant.Default,
        };

        ThemeApplied?.Invoke(this, theme);
    }
}
