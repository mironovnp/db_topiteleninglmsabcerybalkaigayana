namespace CaseChampGui.ViewModels;

public sealed class DatabaseTabViewModel : ObservableObject
{
    private bool _isActive;

    public string Name { get; }

    public bool IsActive
    {
        get => _isActive;
        set => SetProperty(ref _isActive, value);
    }

    public DatabaseTabViewModel(string name)
    {
        Name = name;
    }
}
