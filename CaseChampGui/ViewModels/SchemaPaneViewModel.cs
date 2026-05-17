using System;
using System.Collections.ObjectModel;
using System.Threading.Tasks;
using CaseChampGui.Models;
using CaseChampGui.Services;

namespace CaseChampGui.ViewModels;

public sealed class SchemaTableViewModel : ObservableObject
{
    private bool _isExpanded;

    public string Name { get; }
    public ObservableCollection<SchemaColumn> Columns { get; }

    public bool IsExpanded
    {
        get => _isExpanded;
        set => SetProperty(ref _isExpanded, value);
    }

    public SchemaTableViewModel(SchemaTable model)
    {
        Name = model.Name;
        Columns = new ObservableCollection<SchemaColumn>(model.Columns);
    }
}

public sealed class SchemaPaneViewModel : ObservableObject
{
    private readonly SchemaService _schemaService;
    private string? _currentDatabase;
    private bool _isLoading;
    private string _statusText = "Подключитесь к серверу, чтобы увидеть схему.";

    public SchemaPaneViewModel(SchemaService schemaService)
    {
        _schemaService = schemaService;
        RefreshCommand = new AsyncRelayCommand(RefreshAsync, () => !_isLoading && !string.IsNullOrEmpty(_currentDatabase));
        OpenTableCommand = new RelayCommand<object?>(param =>
        {
            var name = param switch
            {
                string s => s,
                null => null,
                _ => param.ToString(),
            };
            if (!string.IsNullOrWhiteSpace(name))
                OpenTableRequested?.Invoke(this, name.Trim());
        });
    }

    public ObservableCollection<SchemaTableViewModel> Tables { get; } = new();

    public string? CurrentDatabase
    {
        get => _currentDatabase;
        set
        {
            if (SetProperty(ref _currentDatabase, value))
            {
                OnPropertyChanged(nameof(HasDatabase));
                OnPropertyChanged(nameof(Title));
                RefreshCommand.RaiseCanExecuteChanged();
            }
        }
    }

    public bool HasDatabase => !string.IsNullOrEmpty(_currentDatabase);

    public string Title => string.IsNullOrEmpty(_currentDatabase) ? "Схема" : $"Схема · {_currentDatabase}";

    public bool IsLoading
    {
        get => _isLoading;
        private set
        {
            if (SetProperty(ref _isLoading, value))
            {
                RefreshCommand.RaiseCanExecuteChanged();
            }
        }
    }

    public string StatusText
    {
        get => _statusText;
        private set => SetProperty(ref _statusText, value);
    }

    public AsyncRelayCommand RefreshCommand { get; }
    public RelayCommand<object?> OpenTableCommand { get; }

    public event EventHandler? RefreshRequested;
    public event EventHandler<string>? OpenTableRequested;

    public void Clear()
    {
        Tables.Clear();
        StatusText = "Выберите базу данных вверху, чтобы увидеть таблицы.";
    }

    public async Task RefreshAsync()
    {
        if (string.IsNullOrEmpty(_currentDatabase))
        {
            Clear();
            return;
        }

        IsLoading = true;
        StatusText = "Загрузка схемы...";
        try
        {
            var tables = await _schemaService.GetTablesAsync();
            Tables.Clear();
            foreach (var t in tables)
            {
                Tables.Add(new SchemaTableViewModel(t));
            }
            StatusText = tables.Count == 0
                ? "В базе пока нет таблиц."
                : $"Таблиц: {tables.Count}";
        }
        catch (Exception ex)
        {
            StatusText = $"Не удалось загрузить схему: {ex.Message}";
        }
        finally
        {
            IsLoading = false;
        }

        RefreshRequested?.Invoke(this, EventArgs.Empty);
    }
}
