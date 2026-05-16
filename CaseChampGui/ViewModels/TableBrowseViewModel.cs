using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading.Tasks;
using CaseChampGui.Models;
using CaseChampGui.Services;

namespace CaseChampGui.ViewModels;

public sealed class TableBrowseViewModel : ObservableObject
{
    private const int PageSize = 100;

    private readonly IDatabaseClient _client;
    private readonly SchemaService _schemaService;
    private readonly NotificationService _notifications;

    private readonly Dictionary<string, SchemaTable> _tableSchemas = new(StringComparer.OrdinalIgnoreCase);

    private string? _selectedTable;
    private bool _isLoading;
    private bool _isLoadingTables;
    private int _currentOffset;
    private bool _hasResults;
    private bool _isError;
    private string _statusText = "Выберите базу данных на панели выше.";
    private string _centerMessage = "Выберите базу данных на панели выше.";
    private bool _showCenterMessage = true;
    private bool _showGrid;
    private bool _canGoNext;
    private bool _canGoPrev;

    public TableBrowseViewModel(
        IDatabaseClient client,
        SchemaService schemaService,
        NotificationService notifications)
    {
        _client = client;
        _schemaService = schemaService;
        _notifications = notifications;

        RefreshTablesCommand = new AsyncRelayCommand(RefreshTablesAsync, () => !_isLoadingTables && HasDatabase);
        RefreshDataCommand = new AsyncRelayCommand(() => LoadDataAsync(resetOffset: false), () => CanLoadData);
        NextPageCommand = new AsyncRelayCommand(() => LoadDataAsync(resetOffset: false, offsetDelta: PageSize),
            () => CanLoadData && _canGoNext && !_isLoading);
        PrevPageCommand = new AsyncRelayCommand(() => LoadDataAsync(resetOffset: false, offsetDelta: -PageSize),
            () => CanLoadData && _canGoPrev && !_isLoading);
    }

    public ObservableCollection<string> TableNames { get; } = new();
    public ObservableCollection<string> Columns { get; } = new();
    public ObservableCollection<IList<string>> Rows { get; } = new();

    public string? SelectedTable
    {
        get => _selectedTable;
        set
        {
            if (!SetProperty(ref _selectedTable, value)) return;
            OnPropertyChanged(nameof(CanLoadData));
            RefreshDataCommand.RaiseCanExecuteChanged();
            NextPageCommand.RaiseCanExecuteChanged();
            PrevPageCommand.RaiseCanExecuteChanged();
            if (!string.IsNullOrEmpty(value))
                _ = LoadDataAsync(resetOffset: true);
            else
                ClearGrid();
        }
    }

    public bool HasDatabase => !string.IsNullOrEmpty(_client.CurrentDb);

    public bool HasTables => TableNames.Count > 0;

    public bool CanLoadData => HasDatabase && HasTables && !string.IsNullOrEmpty(SelectedTable);

    public bool IsLoading
    {
        get => _isLoading;
        private set
        {
            if (SetProperty(ref _isLoading, value))
            {
                RefreshDataCommand.RaiseCanExecuteChanged();
                NextPageCommand.RaiseCanExecuteChanged();
                PrevPageCommand.RaiseCanExecuteChanged();
            }
        }
    }

    public string StatusText
    {
        get => _statusText;
        private set => SetProperty(ref _statusText, value);
    }

    public string CenterMessage
    {
        get => _centerMessage;
        private set => SetProperty(ref _centerMessage, value);
    }

    public bool ShowCenterMessage
    {
        get => _showCenterMessage;
        private set => SetProperty(ref _showCenterMessage, value);
    }

    public bool ShowGrid
    {
        get => _showGrid;
        private set => SetProperty(ref _showGrid, value);
    }

    public bool IsError
    {
        get => _isError;
        private set => SetProperty(ref _isError, value);
    }

    public bool CanGoNext
    {
        get => _canGoNext;
        private set
        {
            if (SetProperty(ref _canGoNext, value))
                NextPageCommand.RaiseCanExecuteChanged();
        }
    }

    public bool CanGoPrev
    {
        get => _canGoPrev;
        private set
        {
            if (SetProperty(ref _canGoPrev, value))
                PrevPageCommand.RaiseCanExecuteChanged();
        }
    }

    public AsyncRelayCommand RefreshTablesCommand { get; }
    public AsyncRelayCommand RefreshDataCommand { get; }
    public AsyncRelayCommand NextPageCommand { get; }
    public AsyncRelayCommand PrevPageCommand { get; }

    public event EventHandler? GridChanged;

    public void OnSectionActivated()
    {
        _ = RefreshTablesAsync();
    }

    public async Task OnDatabaseChangedAsync()
    {
        _tableSchemas.Clear();
        _currentOffset = 0;
        await RefreshTablesAsync().ConfigureAwait(false);
    }

    public void SelectAndLoadTable(string tableName)
    {
        if (string.IsNullOrWhiteSpace(tableName)) return;

        UiThread.Post(() =>
        {
            if (TableNames.Any(t => string.Equals(t, tableName, StringComparison.OrdinalIgnoreCase)))
            {
                SelectedTable = tableName;
                return;
            }

            _pendingTable = tableName;
            _ = RefreshTablesAsync();
        });
    }

    private string? _pendingTable;

    public async Task RefreshTablesAsync()
    {
        if (!HasDatabase)
        {
            await UiThread.RunAsync(() =>
            {
                TableNames.Clear();
                _tableSchemas.Clear();
                SelectedTable = null;
                SetEmptyState("Выберите базу данных на панели выше.");
            });
            return;
        }

        try
        {
            await UiThread.RunAsync(() =>
            {
                _isLoadingTables = true;
                RefreshTablesCommand.RaiseCanExecuteChanged();
            });

            var tables = await _schemaService.GetTablesAsync().ConfigureAwait(false);

            await UiThread.RunAsync(() =>
            {
                TableNames.Clear();
                _tableSchemas.Clear();
                foreach (var t in tables)
                {
                    TableNames.Add(t.Name);
                    _tableSchemas[t.Name] = t;
                }

                OnPropertyChanged(nameof(HasTables));
                OnPropertyChanged(nameof(CanLoadData));
                RefreshTablesCommand.RaiseCanExecuteChanged();
                RefreshDataCommand.RaiseCanExecuteChanged();

                if (TableNames.Count == 0)
                {
                    SelectedTable = null;
                    SetEmptyState("В этой базе пока нет таблиц. Создайте таблицу через SQL или Text2SQL.");
                    return;
                }

                var pick = _pendingTable;
                _pendingTable = null;
                if (!string.IsNullOrEmpty(pick)
                    && TableNames.Any(t => string.Equals(t, pick, StringComparison.OrdinalIgnoreCase)))
                {
                    SelectedTable = pick;
                    return;
                }

                if (string.IsNullOrEmpty(SelectedTable)
                    || !TableNames.Any(t => string.Equals(t, SelectedTable, StringComparison.OrdinalIgnoreCase)))
                {
                    SetPickTableState();
                    SelectedTable = null;
                }
                else
                {
                    _ = LoadDataAsync(resetOffset: true);
                }
            });
        }
        catch (Exception ex)
        {
            await UiThread.RunAsync(() =>
            {
                SetEmptyState($"Не удалось загрузить список таблиц: {ex.Message}");
            });
            _notifications.Push("Таблицы", ex.Message, NotificationKind.Error);
        }
        finally
        {
            await UiThread.RunAsync(() =>
            {
                _isLoadingTables = false;
                RefreshTablesCommand.RaiseCanExecuteChanged();
            });
        }
    }

    private async Task LoadDataAsync(bool resetOffset = false, int offsetDelta = 0)
    {
        if (!CanLoadData || SelectedTable is null) return;

        if (resetOffset)
            _currentOffset = 0;
        else if (offsetDelta != 0)
            _currentOffset = Math.Max(0, _currentOffset + offsetDelta);

        var table = SanitizeIdentifier(SelectedTable);
        var orderCol = ResolveOrderColumn(SelectedTable);

        var sql = string.IsNullOrEmpty(orderCol)
            ? $"SELECT * FROM {table} LIMIT {PageSize} OFFSET {_currentOffset};"
            : $"SELECT * FROM {table} ORDER BY {orderCol} LIMIT {PageSize} OFFSET {_currentOffset};";

        try
        {
            await UiThread.RunAsync(() =>
            {
                IsLoading = true;
                IsError = false;
                StatusText = "Загрузка данных...";
            });

            var result = await _client.ExecuteAsync(sql).ConfigureAwait(false);

            await UiThread.RunAsync(() =>
            {
                if (!result.Success)
                {
                    IsError = true;
                    ClearGridDataOnly();
                    SetEmptyState(result.Message ?? "Ошибка выполнения запроса.");
                    StatusText = result.Message ?? "Ошибка";
                    return;
                }

                Columns.Clear();
                Rows.Clear();
                foreach (var col in result.Columns)
                    Columns.Add(col);

                foreach (var row in result.Rows)
                    Rows.Add(row);

                _hasResults = Columns.Count > 0;
                ShowCenterMessage = false;
                ShowGrid = _hasResults;
                CanGoPrev = _currentOffset > 0;
                CanGoNext = result.Rows.Count >= PageSize;

                var from = _currentOffset + 1;
                var to = _currentOffset + result.Rows.Count;
                if (result.Rows.Count == 0)
                {
                    StatusText = _currentOffset == 0
                        ? $"Таблица «{SelectedTable}» пуста."
                        : $"Страница пуста (смещение {_currentOffset}).";
                    ShowGrid = Columns.Count > 0;
                    if (Columns.Count > 0 && result.Rows.Count == 0)
                    {
                        ShowCenterMessage = false;
                        ShowGrid = true;
                    }
                }
                else
                {
                    StatusText = CanGoNext
                        ? $"Строки {from}–{to} (есть следующая страница)"
                        : $"Строки {from}–{to}";
                }

                GridChanged?.Invoke(this, EventArgs.Empty);
            });
        }
        catch (Exception ex)
        {
            await UiThread.RunAsync(() =>
            {
                IsError = true;
                SetEmptyState(ex.Message);
                StatusText = ex.Message;
            });
            _notifications.Push("Таблицы", ex.Message, NotificationKind.Error);
        }
        finally
        {
            await UiThread.RunAsync(() => IsLoading = false);
        }
    }

    private string? ResolveOrderColumn(string tableName)
    {
        if (!_tableSchemas.TryGetValue(tableName, out var schema) || schema.Columns.Count == 0)
            return null;

        var pk = schema.Columns.FirstOrDefault(c =>
            string.Equals(c.Key, "PRI", StringComparison.OrdinalIgnoreCase));
        if (pk is not null)
            return SanitizeIdentifier(pk.Name);

        return SanitizeIdentifier(schema.Columns[0].Name);
    }

    private static string SanitizeIdentifier(string name)
    {
        return name.Replace("`", string.Empty).Replace(";", string.Empty).Trim();
    }

    private void ClearGrid()
    {
        ClearGridDataOnly();
        if (!HasDatabase)
            SetEmptyState("Выберите базу данных на панели выше.");
        else if (!HasTables)
            SetEmptyState("В этой базе пока нет таблиц. Создайте таблицу через SQL или Text2SQL.");
        else
            SetPickTableState();
    }

    private void ClearGridDataOnly()
    {
        Columns.Clear();
        Rows.Clear();
        ShowGrid = false;
        CanGoNext = false;
        CanGoPrev = false;
        GridChanged?.Invoke(this, EventArgs.Empty);
    }

    private void SetEmptyState(string message)
    {
        CenterMessage = message;
        ShowCenterMessage = true;
        ShowGrid = false;
        ClearGridDataOnly();
    }

    private void SetPickTableState()
    {
        CenterMessage = "Выберите таблицу на панели внизу.";
        ShowCenterMessage = true;
        ShowGrid = false;
        ClearGridDataOnly();
        StatusText = CenterMessage;
    }
}
