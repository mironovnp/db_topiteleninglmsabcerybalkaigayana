using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using CaseChampGui.Models;
using CaseChampGui.Services;

namespace CaseChampGui.ViewModels;

public enum BrowseFilterEditorMode
{
    Compare,
    Text2Sql,
}

public sealed class TableBrowseViewModel : ObservableObject
{
    private const int PageSize = 100;

    private readonly IDatabaseClient _client;
    private readonly SchemaService _schemaService;
    private readonly IColumnFilterTranslator _filterTranslator;
    private readonly NotificationService _notifications;

    private readonly Dictionary<string, SchemaTable> _tableSchemas = new(StringComparer.OrdinalIgnoreCase);
    private readonly Dictionary<string, ColumnFilter> _columnFilters = new(StringComparer.OrdinalIgnoreCase);
    private readonly SemaphoreSlim _tableListRefreshGate = new(1, 1);

    private string? _selectedTable;
    private bool _isLoading;
    private bool _isLoadingTables;
    private int _currentOffset;
    private bool _showCenterMessage = true;
    private bool _showGrid;
    private bool _canGoNext;
    private bool _canGoPrev;
    private string _statusText = "Выберите базу данных на панели выше.";
    private string _centerMessage = "Выберите базу данных на панели выше.";

    private string? _filterEditingColumn;
    private BrowseFilterEditorMode _filterEditorMode = BrowseFilterEditorMode.Compare;
    private string _filterCompareOperator = "=";
    private string _filterCompareValue = string.Empty;
    private string _filterText2SqlInput = string.Empty;
    private string _filterEditorMessage = string.Empty;
    private bool _isFilterEditorBusy;

    public TableBrowseViewModel(
        IDatabaseClient client,
        SchemaService schemaService,
        IColumnFilterTranslator filterTranslator,
        NotificationService notifications)
    {
        _client = client;
        _schemaService = schemaService;
        _filterTranslator = filterTranslator;
        _notifications = notifications;

        FilterCompareOperators = new[] { "=", ">", "<" };

        RefreshTablesCommand = new AsyncRelayCommand(RefreshTablesAsync, () => !_isLoadingTables && HasDatabase);
        RefreshDataCommand = new AsyncRelayCommand(() => LoadDataAsync(resetOffset: false), () => CanLoadData);
        NextPageCommand = new AsyncRelayCommand(() => LoadDataAsync(resetOffset: false, offsetDelta: PageSize),
            () => CanLoadData && _canGoNext && !_isLoading);
        PrevPageCommand = new AsyncRelayCommand(() => LoadDataAsync(resetOffset: false, offsetDelta: -PageSize),
            () => CanLoadData && _canGoPrev && !_isLoading);
        ApplyColumnFilterCommand = new AsyncRelayCommand(ApplyColumnFilterAsync, () => !IsFilterEditorBusy);
        ClearColumnFilterCommand = new RelayCommand(ClearEditingColumnFilter, () => !IsFilterEditorBusy);
        ClearAllFiltersCommand = new RelayCommand(() => ClearAllFilters(), () => HasActiveFilters && !IsLoading);
    }

    public IReadOnlyList<string> FilterCompareOperators { get; }

    public ObservableCollection<string> TableNames { get; } = new();
    public ObservableCollection<string> Columns { get; } = new();
    public ObservableCollection<IList<string>> Rows { get; } = new();

    public string? SelectedTable
    {
        get => _selectedTable;
        set
        {
            if (!SetProperty(ref _selectedTable, value)) return;
            ClearAllFilters(silent: true);
            OnPropertyChanged(nameof(CanLoadData));
            OnPropertyChanged(nameof(FilteredColumnNames));
            RefreshDataCommand.RaiseCanExecuteChanged();
            NextPageCommand.RaiseCanExecuteChanged();
            PrevPageCommand.RaiseCanExecuteChanged();
            ClearAllFiltersCommand.RaiseCanExecuteChanged();
            if (!string.IsNullOrEmpty(value))
                _ = LoadDataAsync(resetOffset: true);
            else
                ClearGrid();
        }
    }

    public bool HasDatabase => !string.IsNullOrEmpty(_client.CurrentDb);
    public bool HasTables => TableNames.Count > 0;
    public bool CanLoadData => HasDatabase && HasTables && !string.IsNullOrEmpty(SelectedTable);
    public bool HasActiveFilters => _columnFilters.Count > 0;

    public IReadOnlyCollection<string> FilteredColumnNames => _columnFilters.Keys.ToList();

    public bool IsColumnFiltered(string column)
        => _columnFilters.ContainsKey(column);

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
                ClearAllFiltersCommand.RaiseCanExecuteChanged();
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

    public bool IsError { get; private set; }

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

    public string? FilterEditingColumn
    {
        get => _filterEditingColumn;
        private set => SetProperty(ref _filterEditingColumn, value);
    }

    public bool IsCompareFilterMode
    {
        get => _filterEditorMode == BrowseFilterEditorMode.Compare;
        set
        {
            if (!value || _filterEditorMode == BrowseFilterEditorMode.Compare) return;
            _filterEditorMode = BrowseFilterEditorMode.Compare;
            OnPropertyChanged(nameof(IsCompareFilterMode));
            OnPropertyChanged(nameof(IsText2SqlFilterMode));
            FilterEditorMessage = string.Empty;
        }
    }

    public bool IsText2SqlFilterMode
    {
        get => _filterEditorMode == BrowseFilterEditorMode.Text2Sql;
        set
        {
            if (!value || _filterEditorMode == BrowseFilterEditorMode.Text2Sql) return;
            _filterEditorMode = BrowseFilterEditorMode.Text2Sql;
            OnPropertyChanged(nameof(IsCompareFilterMode));
            OnPropertyChanged(nameof(IsText2SqlFilterMode));
            FilterEditorMessage = string.Empty;
        }
    }

    public string FilterCompareOperator
    {
        get => _filterCompareOperator;
        set => SetProperty(ref _filterCompareOperator, value);
    }

    public string FilterCompareValue
    {
        get => _filterCompareValue;
        set => SetProperty(ref _filterCompareValue, value);
    }

    public string FilterText2SqlInput
    {
        get => _filterText2SqlInput;
        set => SetProperty(ref _filterText2SqlInput, value);
    }

    public string FilterText2SqlHint =>
        _filterTranslator.IsEnabled
            ? "Mistral переведёт условие в выражение WHERE для текущей таблицы."
            : "Укажите API-ключ Mistral в настройках, чтобы использовать Text2SQL.";

    public string FilterEditorMessage
    {
        get => _filterEditorMessage;
        private set
        {
            if (SetProperty(ref _filterEditorMessage, value))
                OnPropertyChanged(nameof(HasFilterEditorMessage));
        }
    }

    public bool HasFilterEditorMessage => !string.IsNullOrEmpty(FilterEditorMessage);

    public bool IsFilterEditorBusy
    {
        get => _isFilterEditorBusy;
        private set
        {
            if (SetProperty(ref _isFilterEditorBusy, value))
            {
                ApplyColumnFilterCommand.RaiseCanExecuteChanged();
                ClearColumnFilterCommand.RaiseCanExecuteChanged();
            }
        }
    }

    public AsyncRelayCommand RefreshTablesCommand { get; }
    public AsyncRelayCommand RefreshDataCommand { get; }
    public AsyncRelayCommand NextPageCommand { get; }
    public AsyncRelayCommand PrevPageCommand { get; }
    public AsyncRelayCommand ApplyColumnFilterCommand { get; }
    public RelayCommand ClearColumnFilterCommand { get; }
    public RelayCommand ClearAllFiltersCommand { get; }

    public event EventHandler? GridChanged;
    public event Action? RequestCloseFilterFlyout;

    public void OnSectionActivated() => _ = RefreshTablesAsync();

    public async Task OnDatabaseChangedAsync()
    {
        _tableSchemas.Clear();
        _currentOffset = 0;
        ClearAllFilters(silent: true);
        await RefreshTablesAsync().ConfigureAwait(false);
    }

    public void BeginEditFilter(string columnName)
    {
        if (string.IsNullOrWhiteSpace(columnName)) return;

        FilterEditingColumn = columnName;
        FilterEditorMessage = string.Empty;

        if (_columnFilters.TryGetValue(columnName, out var existing))
        {
            if (existing.Kind == ColumnFilterKind.Compare)
            {
                _filterEditorMode = BrowseFilterEditorMode.Compare;
                FilterCompareOperator = existing.CompareOperator ?? "=";
                FilterCompareValue = existing.CompareValue ?? string.Empty;
                FilterText2SqlInput = string.Empty;
            }
            else
            {
                _filterEditorMode = BrowseFilterEditorMode.Text2Sql;
                FilterText2SqlInput = existing.WhereFragment ?? string.Empty;
                FilterCompareOperator = "=";
                FilterCompareValue = string.Empty;
            }
        }
        else
        {
            _filterEditorMode = BrowseFilterEditorMode.Compare;
            FilterCompareOperator = "=";
            FilterCompareValue = string.Empty;
            FilterText2SqlInput = string.Empty;
        }

        OnPropertyChanged(nameof(IsCompareFilterMode));
        OnPropertyChanged(nameof(IsText2SqlFilterMode));
    }

    /// <summary>Запоминает таблицу для выбора после следующей загрузки списка (синхронно, до смены раздела).</summary>
    public void RequestTableSelection(string tableName)
    {
        if (string.IsNullOrWhiteSpace(tableName)) return;
        _pendingTable = tableName.Trim();
    }

    public void SelectAndLoadTable(string tableName)
    {
        if (string.IsNullOrWhiteSpace(tableName)) return;
        RequestTableSelection(tableName);

        UiThread.Post(() =>
        {
            var canonical = FindTableInList(_pendingTable);
            if (canonical is not null)
            {
                _pendingTable = null;
                SelectedTable = canonical;
                return;
            }

            _ = RefreshTablesAsync();
        });
    }

    private string? _pendingTable;

    private string? FindTableInList(string? name)
    {
        if (string.IsNullOrWhiteSpace(name)) return null;
        return TableNames.FirstOrDefault(t => string.Equals(t, name, StringComparison.OrdinalIgnoreCase));
    }

    private async Task ApplyColumnFilterAsync()
    {
        if (string.IsNullOrEmpty(FilterEditingColumn) || SelectedTable is null) return;

        var columnType = GetColumnType(SelectedTable, FilterEditingColumn);

        if (_filterEditorMode == BrowseFilterEditorMode.Compare)
        {
            if (string.IsNullOrWhiteSpace(FilterCompareValue))
            {
                FilterEditorMessage = "Введите значение для сравнения.";
                return;
            }

            var clause = ColumnFilterSqlBuilder.BuildCompareClause(
                FilterEditingColumn, columnType, FilterCompareOperator, FilterCompareValue);
            if (clause is null)
            {
                FilterEditorMessage = "Некорректный оператор или значение.";
                return;
            }

            var candidate = new ColumnFilter
            {
                Kind = ColumnFilterKind.Compare,
                ColumnName = FilterEditingColumn,
                ColumnType = columnType,
                CompareOperator = FilterCompareOperator,
                CompareValue = FilterCompareValue.Trim(),
            };

            var (valid, validateError) = await ValidateFilterCandidateAsync(candidate).ConfigureAwait(false);
            if (!valid)
            {
                FilterEditorMessage = validateError ?? "Сервер не принял условие фильтра.";
                return;
            }

            _columnFilters[FilterEditingColumn] = candidate;
        }
        else
        {
            if (!_filterTranslator.IsEnabled)
            {
                FilterEditorMessage = FilterText2SqlHint;
                return;
            }

            if (string.IsNullOrWhiteSpace(FilterText2SqlInput))
            {
                FilterEditorMessage = "Введите условие на русском.";
                return;
            }

            string? condition = null;
            if (ColumnFilterAgeHelper.TryBuildCondition(
                    FilterEditingColumn, FilterText2SqlInput, out var ageCondition))
            {
                condition = ageCondition;
            }
            else
            {
                try
                {
                    IsFilterEditorBusy = true;
                    FilterEditorMessage = string.Empty;

                    var result = await _filterTranslator.TranslateAsync(
                        SelectedTable,
                        FilterEditingColumn,
                        columnType,
                        FilterText2SqlInput).ConfigureAwait(false);

                    if (!result.Success || string.IsNullOrWhiteSpace(result.Condition))
                    {
                        await UiThread.RunAsync(() => FilterEditorMessage = result.Message);
                        return;
                    }

                    condition = ColumnFilterSqlBuilder.NormalizeWhereFragment(result.Condition);
                }
                finally
                {
                    await UiThread.RunAsync(() => IsFilterEditorBusy = false);
                }
            }

            if (string.IsNullOrWhiteSpace(condition)
                || !ColumnFilterSqlBuilder.IsSafeWhereFragment(condition))
            {
                FilterEditorMessage =
                    "Условие отклонено. Используйте сравнение с датой (YYYY-MM-DD), CURRENT_DATE или режим «= > <».";
                return;
            }

            var candidate = new ColumnFilter
            {
                Kind = ColumnFilterKind.Text2Sql,
                ColumnName = FilterEditingColumn,
                ColumnType = columnType,
                WhereFragment = condition,
            };

            var (valid, validateError) = await ValidateFilterCandidateAsync(candidate).ConfigureAwait(false);
            if (!valid)
            {
                await UiThread.RunAsync(() =>
                    FilterEditorMessage = validateError ?? "Сервер не принял условие фильтра.");
                return;
            }

            _columnFilters[FilterEditingColumn] = candidate;
        }

        await UiThread.RunAsync(() =>
        {
            OnPropertyChanged(nameof(HasActiveFilters));
            OnPropertyChanged(nameof(FilteredColumnNames));
            ClearAllFiltersCommand.RaiseCanExecuteChanged();
            RequestCloseFilterFlyout?.Invoke();
        });

        await LoadDataAsync(resetOffset: true).ConfigureAwait(false);
    }

    private void ClearEditingColumnFilter()
    {
        if (string.IsNullOrEmpty(FilterEditingColumn)) return;

        _columnFilters.Remove(FilterEditingColumn);
        FilterCompareValue = string.Empty;
        FilterText2SqlInput = string.Empty;
        FilterEditorMessage = string.Empty;

        OnPropertyChanged(nameof(HasActiveFilters));
        OnPropertyChanged(nameof(FilteredColumnNames));
        ClearAllFiltersCommand.RaiseCanExecuteChanged();
        RequestCloseFilterFlyout?.Invoke();
        _ = LoadDataAsync(resetOffset: true);
    }

    private void ClearAllFilters(bool silent = false)
    {
        if (_columnFilters.Count == 0) return;
        _columnFilters.Clear();
        OnPropertyChanged(nameof(HasActiveFilters));
        OnPropertyChanged(nameof(FilteredColumnNames));
        ClearAllFiltersCommand.RaiseCanExecuteChanged();
        if (!silent && CanLoadData)
            _ = LoadDataAsync(resetOffset: true);
        else if (!silent)
            GridChanged?.Invoke(this, EventArgs.Empty);
    }

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

        await _tableListRefreshGate.WaitAsync().ConfigureAwait(false);
        try
        {
            await UiThread.RunAsync(() =>
            {
                _isLoadingTables = true;
                RefreshTablesCommand.RaiseCanExecuteChanged();
            });

            var tables = await _schemaService.GetTablesAsync().ConfigureAwait(false);
            var pick = _pendingTable;

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
                    _pendingTable = null;
                    SelectedTable = null;
                    SetEmptyState("В этой базе пока нет таблиц. Создайте таблицу через SQL или Text2SQL.");
                    return;
                }

                var canonical = FindTableInList(pick);
                if (canonical is not null)
                {
                    _pendingTable = null;
                    SelectedTable = canonical;
                    return;
                }

                if (string.IsNullOrEmpty(SelectedTable)
                    || FindTableInList(SelectedTable) is null)
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
            await UiThread.RunAsync(() => SetEmptyState($"Не удалось загрузить список таблиц: {ex.Message}"));
            _notifications.Push("Таблицы", ex.Message, NotificationKind.Error);
        }
        finally
        {
            await UiThread.RunAsync(() =>
            {
                _isLoadingTables = false;
                RefreshTablesCommand.RaiseCanExecuteChanged();
            });
            _tableListRefreshGate.Release();
        }
    }

    private async Task<(bool Ok, string? Error)> ValidateFilterCandidateAsync(ColumnFilter candidate)
    {
        if (SelectedTable is null) return (false, "Таблица не выбрана.");

        var probeFilters = _columnFilters.Values
            .Where(f => !string.Equals(f.ColumnName, candidate.ColumnName, StringComparison.OrdinalIgnoreCase))
            .Append(candidate)
            .ToList();

        var sql = ColumnFilterSqlBuilder.BuildSelect(
            SelectedTable,
            ResolveOrderColumn(SelectedTable),
            probeFilters,
            1,
            0);

        var dry = await _client.ExecuteAsync(sql, dryRun: true).ConfigureAwait(false);
        if (!dry.Success)
            return (false, dry.Message);

        return (true, null);
    }

    private async Task LoadDataAsync(bool resetOffset = false, int offsetDelta = 0)
    {
        if (!CanLoadData || SelectedTable is null) return;

        if (resetOffset)
            _currentOffset = 0;
        else if (offsetDelta != 0)
            _currentOffset = Math.Max(0, _currentOffset + offsetDelta);

        var orderCol = ResolveOrderColumn(SelectedTable);
        var sql = ColumnFilterSqlBuilder.BuildSelect(
            SelectedTable,
            orderCol,
            _columnFilters.Values,
            PageSize,
            _currentOffset);

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
                    var msg = result.Message ?? "Ошибка выполнения запроса.";
                    StatusText = HasActiveFilters ? $"Ошибка фильтра: {msg}" : msg;
                    _notifications.Push("Таблицы", msg, NotificationKind.Error);

                    if (Columns.Count > 0)
                    {
                        ShowCenterMessage = false;
                        ShowGrid = true;
                    }
                    else
                    {
                        SetEmptyState(msg);
                    }

                    return;
                }

                Columns.Clear();
                Rows.Clear();
                foreach (var col in result.Columns)
                    Columns.Add(col);
                foreach (var row in result.Rows)
                    Rows.Add(row);

                ShowCenterMessage = false;
                ShowGrid = Columns.Count > 0;
                CanGoPrev = _currentOffset > 0;
                CanGoNext = result.Rows.Count >= PageSize;

                StatusText = BuildStatusText(result.Rows.Count);
                GridChanged?.Invoke(this, EventArgs.Empty);
            });
        }
        catch (Exception ex)
        {
            await UiThread.RunAsync(() =>
            {
                IsError = true;
                StatusText = ex.Message;
                if (Columns.Count > 0)
                {
                    ShowCenterMessage = false;
                    ShowGrid = true;
                }
                else
                {
                    SetEmptyState(ex.Message);
                }
            });
            _notifications.Push("Таблицы", ex.Message, NotificationKind.Error);
        }
        finally
        {
            await UiThread.RunAsync(() => IsLoading = false);
        }
    }

    private string BuildStatusText(int rowCount)
    {
        var parts = new List<string>();

        if (HasActiveFilters)
        {
            var filterDesc = string.Join(" · ", _columnFilters.Values.Select(f => f.DisplayText));
            parts.Add($"Фильтры: {filterDesc}");
        }

        if (rowCount == 0)
        {
            parts.Add(_currentOffset == 0
                ? $"Таблица «{SelectedTable}» — нет строк по фильтру."
                : $"Страница пуста (смещение {_currentOffset}).");
        }
        else
        {
            var from = _currentOffset + 1;
            var to = _currentOffset + rowCount;
            parts.Add(CanGoNext
                ? $"Строки {from}–{to} (есть следующая страница)"
                : $"Строки {from}–{to}");
        }

        return string.Join(" · ", parts);
    }

    private string GetColumnType(string tableName, string columnName)
    {
        if (!_tableSchemas.TryGetValue(tableName, out var schema))
            return "TEXT";
        var col = schema.Columns.FirstOrDefault(c =>
            string.Equals(c.Name, columnName, StringComparison.OrdinalIgnoreCase));
        return col?.Type ?? "TEXT";
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
        => name.Replace("`", string.Empty, StringComparison.Ordinal).Replace(";", string.Empty, StringComparison.Ordinal).Trim();

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
