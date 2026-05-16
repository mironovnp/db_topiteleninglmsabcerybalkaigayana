using System;
using System.ComponentModel;
using Avalonia;
using Avalonia.Animation;
using Avalonia.Animation.Easings;
using Avalonia.Controls;
using Avalonia.Input;
using CaseChampGui.ViewModels;

namespace CaseChampGui.Views;

public partial class MainWindow : Window
{
    private static readonly TimeSpan SidebarAnimDuration = TimeSpan.FromMilliseconds(240);

    private MainWindowViewModel? _vm;

    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs change)
    {
        base.OnPropertyChanged(change);
        if (change.Property == TopLevel.ActualThemeVariantProperty)
            (DataContext as MainWindowViewModel)?.RefreshSidebarBrand();
    }

    public MainWindow()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
        Opened += OnWindowOpened;
        Closed += OnWindowClosed;
        SizeChanged += OnWindowSizeChanged;
    }

    private void OnWindowSizeChanged(object? sender, SizeChangedEventArgs e)
    {
        if (DataContext is MainWindowViewModel vm)
            vm.WindowWidth = e.NewSize.Width;
    }

    private void OnWindowClosed(object? sender, EventArgs e)
    {
        if (global::Avalonia.Application.Current?.ApplicationLifetime
            is Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
            && desktop.MainWindow == this)
        {
            desktop.Shutdown();
        }
    }

    private void OnWindowOpened(object? sender, EventArgs e)
    {
        (DataContext as MainWindowViewModel)?.RefreshCommandStates();

        if (Screens.Primary is not { } screen)
        {
            Activate();
            return;
        }

        var wa = screen.WorkingArea;
        var pos = Position;
        var offScreen = pos.X < wa.X - 64 || pos.Y < wa.Y - 64
                        || pos.X > wa.X + wa.Width || pos.Y > wa.Y + wa.Height;
        if (DataContext is MainWindowViewModel vm)
            vm.WindowWidth = Width;

        if (offScreen)
        {
            var w = Math.Clamp((int)Width, 720, wa.Width);
            var h = Math.Clamp((int)Height, 480, wa.Height);
            Position = new PixelPoint(
                wa.X + Math.Max(0, (wa.Width - w) / 2),
                wa.Y + Math.Max(0, (wa.Height - h) / 2));
        }

        Activate();
    }

    private void OnDataContextChanged(object? sender, EventArgs e)
    {
        if (_vm is not null)
        {
            _vm.PropertyChanged -= OnVmPropertyChanged;
        }
        _vm = DataContext as MainWindowViewModel;
        if (_vm is not null)
        {
            _vm.PropertyChanged += OnVmPropertyChanged;
            _vm.WindowWidth = Width;
            ApplySidebarAnimation();
            _vm.RefreshCommandStates();
        }
    }

    private void OnVmPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(MainWindowViewModel.AnimationsEnabled))
        {
            ApplySidebarAnimation();
        }
    }

    private void ApplySidebarAnimation()
    {
        var host = this.FindControl<Border>("SidebarHost");
        if (host is null || _vm is null) return;

        if (_vm.AnimationsEnabled)
        {
            host.Transitions = new Transitions
            {
                new DoubleTransition
                {
                    Property = Border.WidthProperty,
                    Duration = SidebarAnimDuration,
                    Easing = new CubicEaseOut(),
                },
            };
        }
        else
        {
            host.Transitions = null;
        }
    }

    private void OnSidebarPointerEntered(object? sender, PointerEventArgs e)
    {
        if (DataContext is MainWindowViewModel vm)
            vm.IsSidebarHovered = true;
    }

    private void OnSidebarPointerExited(object? sender, PointerEventArgs e)
    {
        if (DataContext is MainWindowViewModel vm)
            vm.IsSidebarHovered = false;
    }
}
