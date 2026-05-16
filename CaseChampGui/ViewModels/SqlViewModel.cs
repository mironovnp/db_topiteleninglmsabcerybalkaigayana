using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Threading.Tasks;
using CaseChampGui.Models;
using CaseChampGui.Services;

namespace CaseChampGui.ViewModels;

public sealed class SqlViewModel : ObservableObject
{
    private readonly IDatabaseClient _client;
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

    public SqlViewModel(IDatabaseClient client, SchemaPaneViewModel schema, NotificationService notifications)
    {
        _client = client;
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

    public void RefreshCommandStates()
    {
        ExecuteCommand.RaiseCanExecuteChanged();
        DryRunCommand.RaiseCanExecuteChanged();
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
        var sql = SqlText;
        if (string.IsNullOrWhiteSpace(sql))
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
                    chatMessage = new ChatMessageViewModel(sql);
                    Messages.Add(chatMessage);
                    SqlText = string.Empty;
                }
                else
                {
                    ResetResults();
                }
            });

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
                    PushHistory(sql, result);
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
}
