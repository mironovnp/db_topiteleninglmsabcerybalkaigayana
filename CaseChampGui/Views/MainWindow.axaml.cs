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
            ApplySidebarAnimation();
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
        {
            vm.IsSidebarHovered = true;
        }
    }

    private void OnSidebarPointerExited(object? sender, PointerEventArgs e)
    {
        if (DataContext is MainWindowViewModel vm)
        {
            vm.IsSidebarHovered = false;
        }
    }
}
