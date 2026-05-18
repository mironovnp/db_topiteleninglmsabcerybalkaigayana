using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Platform.Storage;
using CaseChampGui.Models;
using CaseChampGui.Services;

namespace CaseChampGui.ViewModels;

public sealed class SqlViewModel : ObservableObject
{
    private readonly IDatabaseClient _client;
    private readonly IText2SqlService _text2Sql;
    private readonly IMistralApiKeyStore _apiKeys;
    private readonly NotificationService _notifications;

    private string _sqlText = string.Empty;
    private bool _isBusy;
    private bool _isChatMode;
    private string _statusMessage = string.Empty;

    private bool _hasResults;
    private bool _hasMessage;
    private bool _isError;
    private int _affectedRows;
    private string _resultMessage = string.Empty;

    private IStorageFile? _pickedCsvFile;
    private string? _selectedCsvFilePath;
    private string? _selectedCsvFileName;
    private string? _selectedImportTable;
    private bool _importAppend;
    private string _importStatusMessage = string.Empty;
    private bool _hasImportStatus;
    private bool _isImportBusy;

    public SqlViewModel(
        IDatabaseClient client,
        IText2SqlService text2Sql,
        IMistralApiKeyStore apiKeys,
        SchemaPaneViewModel schema,
        NotificationService notifications)
    {
        _client = client;
        _text2Sql = text2Sql;
        _apiKeys = apiKeys;
        _notifications = notifications;
        Schema = schema;

        ExecuteCommand = new AsyncRelayCommand(() => RunAsync(false), () => !IsBusy);
        DryRunCommand = new AsyncRelayCommand(() => RunAsync(true), () => !IsBusy);
        ClearCommand = new RelayCommand(() =>
        {
            SqlText = string.Empty;
            ResetResults();
        });
        ClearChatCommand = new RelayCommand(() =>
        {
            Messages.Clear();
            OnPropertyChanged(nameof(HasMessages));
        });
        Messages.CollectionChanged += (_, _) => OnPropertyChanged(nameof(HasMessages));
        ClearHistoryCommand = new RelayCommand(() =>
        {
            History.Clear();
            OnPropertyChanged(nameof(HasHistory));
        });
        UseHistoryCommand = new RelayCommand<QueryHistoryItem>(item =>
        {
            if (item is null) return;
            SqlText = item.Sql;
        });

        PickCsvFileCommand = new AsyncRelayCommand(PickCsvFileAsync, () => CanImportCsv && !IsImportBusy);
        RunCsvImportCommand = new AsyncRelayCommand(RunCsvImportAsync, CanRunCsvImport);

        _client.StateChanged += (_, _) => UiThread.Post(RefreshCommandStates);
        Schema.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName is nameof(SchemaPaneViewModel.CurrentDatabase)
                or nameof(SchemaPaneViewModel.HasDatabase))
            {
                UiThread.Post(RefreshCommandStates);
            }
        };
        Schema.RefreshRequested += (_, _) => UiThread.Post(SyncImportTablesFromSchema);
    }

    public SchemaPaneViewModel Schema { get; }

    public ObservableCollection<ChatMessageViewModel> Messages { get; } = new();

    public bool HasMessages => Messages.Count > 0;

    public ObservableCollection<QueryHistoryItem> History { get; } = new();

    public bool HasHistory => History.Count > 0;

    public string SqlText
    {
        get => _sqlText;
        set
        {
            if (SetProperty(ref _sqlText, value))
            {
                ExecuteCommand.RaiseCanExecuteChanged();
                DryRunCommand.RaiseCanExecuteChanged();
            }
        }
    }

    public bool IsBusy
    {
        get => _isBusy;
        private set
        {
            if (SetProperty(ref _isBusy, value))
            {
                ExecuteCommand.RaiseCanExecuteChanged();
                DryRunCommand.RaiseCanExecuteChanged();
            }
        }
    }

    public bool IsChatMode
    {
        get => _isChatMode;
        set
        {
            if (SetProperty(ref _isChatMode, value))
            {
                OnPropertyChanged(nameof(IsClassicMode));
            }
        }
    }

    public bool IsClassicMode => !_isChatMode;

    public string StatusMessage
    {
        get => _statusMessage;
        private set => SetProperty(ref _statusMessage, value);
    }

    public bool HasResults
    {
        get => _hasResults;
        private set => SetProperty(ref _hasResults, value);
    }

    public bool HasMessage
    {
        get => _hasMessage;
        private set => SetProperty(ref _hasMessage, value);
    }

    public bool IsError
    {
        get => _isError;
        private set => SetProperty(ref _isError, value);
    }

    public string ResultMessage
    {
        get => _resultMessage;
        private set => SetProperty(ref _resultMessage, value);
    }

    public int AffectedRows
    {
        get => _affectedRows;
        private set => SetProperty(ref _affectedRows, value);
    }

    public ObservableCollection<string> Columns { get; } = new();
    public ObservableCollection<IList<string>> Rows { get; } = new();

    public AsyncRelayCommand ExecuteCommand { get; }
    public AsyncRelayCommand DryRunCommand { get; }
    public RelayCommand ClearCommand { get; }
  /// <summary>База с вкладки сверху (схема) или с сервера после USE.</summary>
    public string? ActiveDatabaseName =>
        !string.IsNullOrWhiteSpace(Schema.CurrentDatabase) ? Schema.CurrentDatabase : _client.CurrentDb;

    public bool CanImportCsv => !string.IsNullOrEmpty(ActiveDatabaseName);

    public AsyncRelayCommand PickCsvFileCommand { get; }
    public AsyncRelayCommand RunCsvImportCommand { get; }

    public ObservableCollection<string> ImportTables { get; } = new();

    public bool HasImportTables => ImportTables.Count > 0;

    public string? SelectedCsvFilePath
    {
        get => _selectedCsvFilePath;
        private set
        {
            if (SetProperty(ref _selectedCsvFilePath, value))
            {
                OnPropertyChanged(nameof(HasSelectedCsvPath));
                RunCsvImportCommand.RaiseCanExecuteChanged();
            }
        }
    }

    public bool HasSelectedCsvPath => !string.IsNullOrWhiteSpace(SelectedCsvFilePath);

    public string? SelectedCsvFileName
    {
        get => _selectedCsvFileName;
        private set
        {
            if (SetProperty(ref _selectedCsvFileName, value))
                RunCsvImportCommand.RaiseCanExecuteChanged();
        }
    }

    public string? SelectedImportTable
    {
        get => _selectedImportTable;
        set
        {
            if (SetProperty(ref _selectedImportTable, value))
                RunCsvImportCommand.RaiseCanExecuteChanged();
        }
    }

    public bool ImportAppend
    {
        get => _importAppend;
        set => SetProperty(ref _importAppend, value);
    }

    public string ImportStatusMessage
    {
        get => _importStatusMessage;
        private set
        {
            if (SetProperty(ref _importStatusMessage, value))
                OnPropertyChanged(nameof(HasImportStatus));
        }
    }

    public bool HasImportStatus
    {
        get => _hasImportStatus;
        private set => SetProperty(ref _hasImportStatus, value);
    }

    public bool IsImportBusy
    {
        get => _isImportBusy;
        private set
        {
            if (SetProperty(ref _isImportBusy, value))
            {
                PickCsvFileCommand.RaiseCanExecuteChanged();
                RunCsvImportCommand.RaiseCanExecuteChanged();
            }
        }
    }

    public void RefreshCommandStates()
    {
        OnPropertyChanged(nameof(ActiveDatabaseName));
        OnPropertyChanged(nameof(CanImportCsv));
        ExecuteCommand.RaiseCanExecuteChanged();
        DryRunCommand.RaiseCanExecuteChanged();
        PickCsvFileCommand.RaiseCanExecuteChanged();
        RunCsvImportCommand.RaiseCanExecuteChanged();
    }
    public RelayCommand ClearChatCommand { get; }
    public RelayCommand ClearHistoryCommand { get; }
    public RelayCommand<QueryHistoryItem> UseHistoryCommand { get; }

    public event EventHandler? ResultsChanged;
    public event EventHandler<string>? SchemaInvalidationRequested;

    private void ResetResults()
    {
        Columns.Clear();
        Rows.Clear();
        HasResults = false;
        HasMessage = false;
        IsError = false;
        ResultMessage = string.Empty;
        AffectedRows = 0;
        StatusMessage = string.Empty;
        ResultsChanged?.Invoke(this, EventArgs.Empty);
    }

    private async Task RunAsync(bool dryRun)
    {
        var userInput = SqlText.Trim();
        if (string.IsNullOrWhiteSpace(userInput))
        {
            await UiThread.RunAsync(() =>
            {
                StatusMessage = "Введите SQL-запрос.";
                HasMessage = true;
                IsError = true;
                ResultMessage = "Поле запроса пусто.";
            });
            return;
        }

        var sql = userInput;

        try
        {
            await UiThread.RunAsync(() =>
            {
                IsBusy = true;
                StatusMessage = dryRun ? "Проверка запроса..." : "Выполнение запроса...";
            });

            ChatMessageViewModel? chatMessage = null;
            await UiThread.RunAsync(() =>
            {
                if (IsChatMode && !dryRun)
                {
                    chatMessage = new ChatMessageViewModel(userInput);
                    Messages.Add(chatMessage);
                    SqlText = string.Empty;
                }
                else
                {
                    ResetResults();
                }
            });

            if (IsChatMode && !dryRun && SqlInputClassifier.ShouldUseText2Sql(userInput, out var text2SqlRequest))
            {
                await UiThread.RunAsync(() =>
                {
                    if (chatMessage is not null)
                        chatMessage.BusyHint = "Перевод в SQL…";
                });

                if (!_apiKeys.HasValidKey)
                {
                    const string keyMsg =
                        "Настройте API-ключ Mistral в разделе Text2SQL или в настройках.";
                    await UiThread.RunAsync(() =>
                    {
                        if (chatMessage is not null)
                        {
                            chatMessage.Result = QueryResult.Fail(keyMsg);
                            chatMessage.IsBusy = false;
                            chatMessage.BusyHint = string.Empty;
                        }
                        StatusMessage = "Ошибка";
                    });
                    return;
                }

                Text2SqlResult translation;
                try
                {
                    translation = await _text2Sql.TranslateAsync(
                            text2SqlRequest,
                            string.IsNullOrWhiteSpace(Schema.CurrentDatabase) ? _client.CurrentDb : Schema.CurrentDatabase)
                        .ConfigureAwait(false);
                }
                catch (Exception ex)
                {
                    _notifications.Push("Text2SQL", ex.Message, NotificationKind.Error);
                    await UiThread.RunAsync(() =>
                    {
                        if (chatMessage is not null)
                        {
                            chatMessage.Result = QueryResult.Fail($"Ошибка перевода: {ex.Message}");
                            chatMessage.IsBusy = false;
                            chatMessage.BusyHint = string.Empty;
                        }
                        StatusMessage = "Ошибка";
                    });
                    return;
                }

                if (!translation.Success || string.IsNullOrWhiteSpace(translation.Sql))
                {
                    var msg = string.IsNullOrWhiteSpace(translation.Message)
                        ? "Не удалось перевести запрос в SQL."
                        : translation.Message;
                    await UiThread.RunAsync(() =>
                    {
                        if (chatMessage is not null)
                        {
                            chatMessage.Result = QueryResult.Fail(msg);
                            chatMessage.IsBusy = false;
                            chatMessage.BusyHint = string.Empty;
                        }
                        StatusMessage = "Ошибка";
                    });
                    return;
                }

                sql = translation.Sql;
                await UiThread.RunAsync(() =>
                {
                    if (chatMessage is not null)
                    {
                        chatMessage.GeneratedSql = sql;
                        chatMessage.BusyHint = "Выполнение…";
                    }
                });
            }
            else if (chatMessage is not null)
            {
                await UiThread.RunAsync(() => chatMessage.BusyHint = "Выполнение…");
            }

            foreach (var statement in SqlScript.SplitStatements(sql))
            {
                if (Regex.IsMatch(statement, @"^\s*SET\s+USER\b", RegexOptions.IgnoreCase))
                {
                    const string setUserMsg =
                        "Команда SET USER запрещена для использования в графическом интерфейсе. Пожалуйста, воспользуйтесь окном авторизации.";
                    await UiThread.RunAsync(() =>
                    {
                        if (chatMessage is not null)
                        {
                            chatMessage.Result = QueryResult.Fail(setUserMsg);
                            chatMessage.IsBusy = false;
                            chatMessage.BusyHint = string.Empty;
                            StatusMessage = "Отказ в выполнении";
                        }
                        else
                        {
                            StatusMessage = "Отказ в выполнении";
                            HasMessage = true;
                            IsError = true;
                            ResultMessage = setUserMsg;
                        }
                    });
                    return;
                }
            }

            SqlScript.RunResult run;
            try
            {
                run = await SqlScript.ExecuteAllAsync(
                    _client,
                    sql,
                    dryRun,
                    onStepStarted: (step, total, _) =>
                    {
                        if (total <= 1) return;
                        UiThread.Post(() =>
                        {
                            StatusMessage = dryRun
                                ? $"Проверка ({step}/{total})…"
                                : $"Выполнение ({step}/{total})…";
                        });
                    }).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                _notifications.Push("Ошибка выполнения", ex.Message, NotificationKind.Error);
                run = new SqlScript.RunResult
                {
                    Success = false,
                    LastResult = QueryResult.Fail($"Внутренняя ошибка: {ex.Message}"),
                    TotalCount = 1,
                };
            }

            var result = run.LastResult;
            var capturedChat = chatMessage;
            await UiThread.RunAsync(() =>
            {
                if (capturedChat is not null)
                {
                    capturedChat.Result = result;
                    capturedChat.IsBusy = false;
                    capturedChat.BusyHint = string.Empty;
                    StatusMessage = result.Success
                        ? (run.TotalCount > 1 ? $"Готово · {run.TotalCount} команд" : "Готово")
                        : "Ошибка";
                }
                else
                {
                    ApplyResult(result, dryRun, run.TotalCount);
                    if (!dryRun && result.Success)
                    {
                        SqlText = string.Empty;
                    }
                }

                if (!dryRun && result.Success)
                {
                    foreach (var statement in SqlScript.SplitStatements(sql))
                    {
                        if (InvalidatesSchema(statement))
                        {
                            SchemaInvalidationRequested?.Invoke(this, statement);
                        }
                    }
                }

                if (!dryRun)
                {
                    PushHistory(userInput, result);
                }
            });
        }
        finally
        {
            await UiThread.RunAsync(() =>
            {
                IsBusy = false;
                RefreshCommandStates();
            });
        }
    }

    private void PushHistory(string sql, Models.QueryResult result)
    {
        string summary;
        if (!result.Success)
        {
            summary = string.IsNullOrWhiteSpace(result.Message) ? "Ошибка" : result.Message;
        }
        else if (result.Columns.Count > 0)
        {
            summary = $"строк: {result.Rows.Count}";
        }
        else if (!string.IsNullOrWhiteSpace(result.Message))
        {
            summary = result.Message;
        }
        else
        {
            summary = "OK";
        }

        History.Insert(0, new QueryHistoryItem(sql, result.Success, summary));
        const int maxHistory = 50;
        while (History.Count > maxHistory) History.RemoveAt(History.Count - 1);
        OnPropertyChanged(nameof(HasHistory));
    }

    private void ApplyResult(QueryResult result, bool dryRun, int statementCount = 1)
    {
        if (!result.Success)
        {
            IsError = true;
            HasMessage = true;
            ResultMessage = string.IsNullOrEmpty(result.Message)
                ? "Запрос завершился ошибкой."
                : result.Message;
            StatusMessage = "Ошибка";
            ResultsChanged?.Invoke(this, EventArgs.Empty);
            return;
        }

        if (result.Columns.Count > 0)
        {
            foreach (var c in result.Columns) Columns.Add(c);
            foreach (var r in result.Rows) Rows.Add(r);
            HasResults = true;
            StatusMessage = statementCount > 1
                ? $"Готово · {statementCount} команд · строк: {result.Rows.Count}"
                : $"Получено строк: {result.Rows.Count}";
        }
        else
        {
            HasResults = false;
            HasMessage = true;
            AffectedRows = result.AffectedRows;
            var tail = string.IsNullOrEmpty(result.Message)
                ? (dryRun ? "Синтаксис корректен." : "Готово.")
                : result.Message;
            ResultMessage = statementCount > 1
                ? $"Выполнено команд: {statementCount}. {tail}"
                : tail;
            StatusMessage = dryRun
                ? (statementCount > 1 ? $"Dry run OK · {statementCount} команд" : "Dry run OK")
                : (statementCount > 1 ? $"Готово · {statementCount} команд" : "Готово");
        }

        ResultsChanged?.Invoke(this, EventArgs.Empty);
    }

    private static bool InvalidatesSchema(string sql)
    {
        var trimmed = sql.TrimStart();
        if (trimmed.Length == 0) return false;

        ReadOnlySpan<string> ddl = new[]
        {
            "CREATE TABLE", "CREATE INDEX",
            "DROP TABLE", "DROP INDEX",
            "ALTER TABLE",
            "USE ",
            "DROP DATABASE", "CREATE DATABASE",
        };

        foreach (var prefix in ddl)
        {
            if (trimmed.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                return true;
        }
        return false;
    }

    private static string SanitizeDatabaseName(string database) =>
        database.Replace("`", string.Empty).Replace(";", string.Empty);

    private async Task<bool> EnsureActiveDatabaseAsync()
    {
        var db = ActiveDatabaseName;
        if (string.IsNullOrWhiteSpace(db))
            return false;

        if (string.Equals(_client.CurrentDb, db, StringComparison.Ordinal))
            return true;

        var use = await _client.ExecuteAsync($"USE {SanitizeDatabaseName(db)};").ConfigureAwait(false);
        if (use.Success && string.IsNullOrEmpty(_client.CurrentDb))
            Schema.CurrentDatabase = db;
        return use.Success;
    }

    private void RefreshImportTables()
    {
        ImportTables.Clear();
        foreach (var table in Schema.Tables)
            ImportTables.Add(table.Name);

        OnPropertyChanged(nameof(HasImportTables));
        RunCsvImportCommand.RaiseCanExecuteChanged();

        if (ImportTables.Count == 0)
        {
            SelectedImportTable = null;
            return;
        }

        if (string.IsNullOrEmpty(SelectedImportTable) || !ImportTables.Contains(SelectedImportTable))
            SelectedImportTable = ImportTables[0];
    }

    private void SyncImportTablesFromSchema()
    {
        RefreshImportTables();
        if (!HasImportTables || !HasSelectedCsvPath)
            return;

        ImportStatusMessage = $"Таблица «{SelectedImportTable}» готова. Нажмите «Импорт».";
        HasImportStatus = true;
    }

    private bool CanRunCsvImport() =>
        !IsImportBusy &&
        !IsBusy &&
        HasSelectedCsvPath &&
        _pickedCsvFile is not null &&
        !string.IsNullOrWhiteSpace(SelectedImportTable) &&
        CanImportCsv;

    private async Task PickCsvFileAsync()
    {
        if (!CanImportCsv)
        {
            await UiThread.RunAsync(() =>
            {
                ImportStatusMessage = "Сначала выберите базу данных на панели сверху.";
                HasImportStatus = true;
            });
            return;
        }

        if (!await EnsureActiveDatabaseAsync().ConfigureAwait(false))
        {
            await UiThread.RunAsync(() =>
            {
                ImportStatusMessage = "Не удалось переключиться на выбранную базу (USE).";
                HasImportStatus = true;
            });
            return;
        }

        if (Application.Current?.ApplicationLifetime
            is not Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime desktop
            || desktop.MainWindow is not Window window)
        {
            return;
        }

        var files = await window.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Выберите CSV-файл",
            AllowMultiple = false,
            FileTypeFilter =
            [
                new FilePickerFileType("CSV")
                {
                    Patterns = ["*.csv"],
                    MimeTypes = ["text/csv"],
                },
            ],
        }).ConfigureAwait(false);

        var file = files?.FirstOrDefault();
        if (file is null) return;

        _pickedCsvFile = file;
        SelectedCsvFileName = file.Name;
        SelectedCsvFilePath = file.TryGetLocalPath() ?? file.Name;
        RefreshImportTables();

        if (ImportTables.Count == 0)
        {
            ImportStatusMessage =
                "Файл выбран. Создайте таблицу (CREATE TABLE) — список обновится автоматически, затем нажмите «Импорт».";
            HasImportStatus = true;
            return;
        }

        ImportStatusMessage = $"Таблица «{SelectedImportTable}». Нажмите «Импорт».";
        HasImportStatus = true;
    }

    private async Task RunCsvImportAsync()
    {
        if (string.IsNullOrWhiteSpace(SelectedImportTable) || _pickedCsvFile is null)
        {
            await UiThread.RunAsync(() =>
            {
                ImportStatusMessage = "Выберите CSV-файл и таблицу.";
                HasImportStatus = true;
            });
            return;
        }

        try
        {
            if (!await EnsureActiveDatabaseAsync().ConfigureAwait(false))
            {
                await UiThread.RunAsync(() =>
                {
                    ImportStatusMessage = "Не удалось переключиться на выбранную базу (USE).";
                    HasImportStatus = true;
                });
                return;
            }

            await UiThread.RunAsync(() =>
            {
                IsImportBusy = true;
                HasImportStatus = true;
                ImportStatusMessage = "Загрузка файла на сервер…";
            });

            var tableName = SelectedImportTable;
            if (string.IsNullOrWhiteSpace(tableName) && ImportTables.Count > 0)
                tableName = ImportTables[0];

            if (string.IsNullOrWhiteSpace(tableName))
            {
                await UiThread.RunAsync(() =>
                {
                    ImportStatusMessage = "Выберите таблицу для импорта.";
                    HasImportStatus = true;
                });
                return;
            }

            await using var stream = await _pickedCsvFile.OpenReadAsync().ConfigureAwait(false);
            var import = await _client.ImportCsvAsync(
                tableName,
                _pickedCsvFile.Name,
                stream,
                ImportAppend).ConfigureAwait(false);

            await UiThread.RunAsync(() =>
            {
                if (!string.IsNullOrWhiteSpace(import.Sql))
                    SqlText = import.Sql;

                if (import.Success)
                {
                    ResetResults();
                    HasResults = false;
                    HasMessage = true;
                    IsError = false;
                    AffectedRows = import.AffectedRows;
                    var fileNote = string.IsNullOrWhiteSpace(import.ServerFileName)
                        ? string.Empty
                        : $" Файл на сервере: {import.ServerFileName}.";
                    ResultMessage = string.IsNullOrWhiteSpace(import.Message)
                        ? "Импорт CSV выполнен." + fileNote
                        : import.Message + fileNote;
                    StatusMessage = "Импорт CSV готов";
                    ImportStatusMessage = ResultMessage;
                    _notifications.Push("Импорт CSV", ResultMessage, NotificationKind.Info);
                    SchemaInvalidationRequested?.Invoke(this, import.Sql ?? string.Empty);
                    _ = Schema.RefreshAsync();
                }
                else
                {
                    IsError = true;
                    HasMessage = true;
                    HasResults = false;
                    var msg = string.IsNullOrWhiteSpace(import.Message)
                        ? "Импорт CSV завершился ошибкой."
                        : import.Message;
                    if (msg.Contains("Missing form field 'table'", StringComparison.OrdinalIgnoreCase))
                    {
                        msg += " Перезапустите dbserver: pkill dbserver && ./run.sh (или перезапустите GUI с авто-запуском сервера).";
                    }
                    ResultMessage = msg;
                    StatusMessage = "Ошибка импорта";
                    ImportStatusMessage = ResultMessage;
                    _notifications.Push("Импорт CSV", ResultMessage, NotificationKind.Error);
                }

                ResultsChanged?.Invoke(this, EventArgs.Empty);
            });
        }
        finally
        {
            await UiThread.RunAsync(() => IsImportBusy = false);
        }
    }
}
