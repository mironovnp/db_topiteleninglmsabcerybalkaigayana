namespace CaseChampGui.ViewModels;

public enum AppSection
{
    Sql,
    Text2Sql,
    Browse,
    Settings,
}

public sealed class NavItemViewModel : ObservableObject
{
    private bool _isSelected;

    public AppSection Section { get; }
    public string Title { get; }
    public string Icon { get; }
    public bool IsBottom { get; }

    public bool IsSelected
    {
        get => _isSelected;
        set => SetProperty(ref _isSelected, value);
    }

    public NavItemViewModel(AppSection section, string title, string icon, bool isBottom = false)
    {
        Section = section;
        Title = title;
        Icon = icon;
        IsBottom = isBottom;
    }
}
