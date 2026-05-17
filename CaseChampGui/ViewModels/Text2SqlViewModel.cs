using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using CaseChampGui.Models;
using CaseChampGui.Services;

namespace CaseChampGui.ViewModels;

public sealed class Text2SqlViewModel : ObservableObject
{
    private readonly IText2SqlService _service;
    private readonly IMistralApiKeyStore _apiKeys;
    private readonly IDatabaseClient _client;
    private readonly NotificationService _notifications;

    private string _inputText = string.Empty;
    private string _apiKeyInput = string.Empty;
    private string _generatedSql = string.Empty;
    private string _statusMessage = string.Empty;
    private string _resultMessage = string.Empty;
    private string _apiSetupMessage = string.Empty;
    private string _sharedKeyNotice = string.Empty;
    private bool _showSharedKeyNotice;
    private bool _needsApiKeySetup;
    private bool _isBusy;
    private bool _hasResults;
    private bool _hasMessage;
    private bool _isError;
    private int _affectedRows;

    public Text2SqlViewModel(
        IText2SqlService service,
        IMistralApiKeyStore apiKeys,
        IDatabaseClient client,
        SchemaPaneViewModel schema,
        NotificationService notifications)
    {
        _service = service;
        _apiKeys = apiKeys;
        _client = client;
        Schema = schema;
        _notifications = notifications;

        SaveApiKeyCommand = new AsyncRelayCommand(SaveApiKeyAsync, () => !string.IsNullOrWhiteSpace(ApiKeyInput));
        TranslateAndRunCommand = new AsyncRelayCommand(TranslateAndRunAsync, CanTranslateAndRun);
        ClearCommand = new RelayCommand(ClearWorkspace);
        DismissSharedKeyNoticeCommand = new RelayCommand(DismissSharedKeyNotice);

        _apiKeys.ApiKeyChanged += (_, _) => UiThread.Post(RefreshApiKeyState);
        RefreshApiKeyState();
    }

    public string InputText
    {
        get => _inputText;
        set
        {
            if (SetProperty(ref _inputText, value))
                TranslateAndRunCommand.RaiseCanExecuteChanged();
        }
    }

    public string ApiKeyInput
    {
        get => _apiKeyInput;
        set
        {
            if (SetProperty(ref _apiKeyInput, value))
            {
                SaveApiKeyCommand.RaiseCanExecuteChanged();
                OnPropertyChanged(nameof(HasApiKeyInput));
            }
        }
    }

    public bool HasApiKeyInput => !string.IsNullOrWhiteSpace(ApiKeyInput);

    public string GeneratedSql
    {
        get => _generatedSql;
        private set => SetProperty(ref _generatedSql, value);
    }

    public string StatusMessage
    {
        get => _statusMessage;
        private set => SetProperty(ref _statusMessage, value);
    }

    public string ResultMessage
    {
        get => _resultMessage;
        private set => SetProperty(ref _resultMessage, value);
    }

    public string ApiSetupMessage
    {
        get => _apiSetupMessage;
        private set
        {
            if (SetProperty(ref _apiSetupMessage, value))
                OnPropertyChanged(nameof(HasApiSetupMessage));
        }
    }

    public bool HasApiSetupMessage => !string.IsNullOrWhiteSpace(ApiSetupMessage);

    public string SharedKeyNotice
    {
        get => _sharedKeyNotice;
        private set
        {
            if (SetProperty(ref _sharedKeyNotice, value))
                OnPropertyChanged(nameof(HasSharedKeyNotice));
        }
    }

    public bool HasSharedKeyNotice => !string.IsNullOrWhiteSpace(SharedKeyNotice);

    public bool ShowSharedKeyNotice
    {
        get => _showSharedKeyNotice;
        private set => SetProperty(ref _showSharedKeyNotice, value);
    }

    public RelayCommand DismissSharedKeyNoticeCommand { get; }

    public bool NeedsApiKeySetup
    {
        get => _needsApiKeySetup;
        private set
        {
            if (SetProperty(ref _needsApiKeySetup, value))
            {
                OnPropertyChanged(nameof(IsWorkspaceVisible));
                TranslateAndRunCommand.RaiseCanExecuteChanged();
            }
        }
    }

    public bool IsWorkspaceVisible => !NeedsApiKeySetup;

    public string ActiveKeyBadge => _apiKeys.ActiveKeyBadge;

    public bool IsKeyChoiceLocked => _apiKeys.IsKeyChoiceLocked;

    public bool IsBusy
    {
        get => _isBusy;
        private set
        {
            if (SetProperty(ref _isBusy, value))
                TranslateAndRunCommand.RaiseCanExecuteChanged();
        }
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

    public int AffectedRows
    {
        get => _affectedRows;
        private set => SetProperty(ref _affectedRows, value);
    }

    public bool HasGeneratedSql => !string.IsNullOrWhiteSpace(GeneratedSql);

    public ObservableCollection<string> Columns { get; } = new();
    public ObservableCollection<IList<string>> Rows { get; } = new();

    public SchemaPaneViewModel Schema { get; }

    public AsyncRelayCommand SaveApiKeyCommand { get; }
    public AsyncRelayCommand TranslateAndRunCommand { get; }
    public RelayCommand ClearCommand { get; }

    public void RefreshCommandStates()
    {
        SaveApiKeyCommand.RaiseCanExecuteChanged();
        TranslateAndRunCommand.RaiseCanExecuteChanged();
    }

    public event EventHandler? ResultsChanged;

    /// <summary>Raised after successful DDL that changes the database catalog (CREATE/DROP DATABASE).</summary>
    public event EventHandler<string>? CatalogSqlExecuted;

    public void OnSectionActivated()
    {
        RefreshApiKeyState();
        if (Schema.HasDatabase)
            _ = Schema.RefreshAsync();
    }

    public void RefreshApiKeyState()
    {
        NeedsApiKeySetup = !_apiKeys.HasValidKey;
        if (!NeedsApiKeySetup)
        {
            ApiKeyInput = string.Empty;
            ApiSetupMessage = string.Empty;
        }

        OnPropertyChanged(nameof(ActiveKeyBadge));
        OnPropertyChanged(nameof(IsKeyChoiceLocked));

        if (_apiKeys.ShouldShowSharedKeyNotice && !string.IsNullOrEmpty(_apiKeys.SharedKeyOwner))
        {
            SharedKeyNotice =
                $"Используется общий ключ Mistral пользователя «{_apiKeys.SharedKeyOwner}» с этого компьютера. " +
                "Запросы Text2SQL расходуют лимиты его аккаунта Mistral.";
            ShowSharedKeyNotice = true;
        }
        else
        {
            SharedKeyNotice = string.Empty;
            ShowSharedKeyNotice = false;
        }
    }

    private void DismissSharedKeyNotice()
    {
        _apiKeys.AcknowledgeSharedKeyNotice();
        ShowSharedKeyNotice = false;
        SharedKeyNotice = string.Empty;
    }

    private bool CanTranslateAndRun() => !IsBusy && !NeedsApiKeySetup;

    private async Task SaveApiKeyAsync()
    {
        ApiSetupMessage = string.Empty;
        try
        {
            _apiKeys.SavePersonalApiKey(ApiKeyInput);
            ApiKeyInput = string.Empty;
            RefreshApiKeyState();
            ApiSetupMessage = "Ключ сохранён. Можно отправлять запросы на русском языке.";
        }
        catch (Exception ex)
        {
            ApiSetupMessage = ex.Message;
        }

        await Task.CompletedTask;
    }

    private async Task TranslateAndRunAsync()
    {
        if (string.IsNullOrWhiteSpace(InputText))
        {
            await UiThread.RunAsync(() => ApplyError("Введите запрос на русском языке."));
            return;
        }

        await UiThread.RunAsync(() =>
        {
            IsBusy = true;
            StatusMessage = "Перевод в SQL…";
            ResetResults();
            GeneratedSql = string.Empty;
            OnPropertyChanged(nameof(HasGeneratedSql));
        });

        try
        {
            Text2SqlResult translation;
            try
            {
                translation = await _service.TranslateAsync(InputText).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                _notifications.Push("Text2SQL", ex.Message, NotificationKind.Error);
                await UiThread.RunAsync(() => ApplyError($"Ошибка перевода: {ex.Message}"));
                return;
            }

            if (!translation.Success || string.IsNullOrWhiteSpace(translation.Sql))
            {
                await UiThread.RunAsync(() => ApplyError(translation.Message));
                return;
            }

            var sql = translation.Sql;
            var statements = SqlScript.SplitStatements(sql);

            foreach (var statement in statements)
            {
                if (Regex.IsMatch(statement, @"^\s*SET\s+USER\b", RegexOptions.IgnoreCase))
                {
                    await UiThread.RunAsync(() => ApplyError("Команда SET USER запрещена для использования в графическом интерфейсе. Пожалуйста, воспользуйтесь окном авторизации."));
                    return;
                }
            }

            await UiThread.RunAsync(() =>
            {
                GeneratedSql = sql;
                OnPropertyChanged(nameof(HasGeneratedSql));
                StatusMessage = statements.Count > 1
                    ? $"Выполнение SQL (1/{statements.Count})…"
                    : "Выполнение SQL…";
            });

            SqlScript.RunResult run;
            try
            {
                run = await SqlScript.ExecuteAllAsync(
                    _client,
                    sql,
                    onStepStarted: (step, total, _) =>
                    {
                        if (total <= 1) return;
                        UiThread.Post(() => StatusMessage = $"Выполнение SQL ({step}/{total})…");
                    }).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                _notifications.Push("Text2SQL", ex.Message, NotificationKind.Error);
                await UiThread.RunAsync(() => ApplyError($"Ошибка выполнения: {ex.Message}"));
                return;
            }

            await UiThread.RunAsync(() => ApplyScriptResult(run, sql));
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

    private void ApplyError(string message)
    {
        IsError = true;
        HasMessage = true;
        HasResults = false;
        ResultMessage = message;
        StatusMessage = "Ошибка";
        ResultsChanged?.Invoke(this, EventArgs.Empty);
    }

    private void ApplyScriptResult(SqlScript.RunResult run, string sql)
    {
        var result = run.LastResult;
        if (!run.Success || !result.Success)
        {
            ApplyError(string.IsNullOrWhiteSpace(result.Message)
                ? "Запрос завершился ошибкой."
                : result.Message);
            return;
        }

        if (result.Columns.Count > 0)
        {
            foreach (var c in result.Columns) Columns.Add(c);
            foreach (var r in result.Rows) Rows.Add(r);
            HasResults = true;
            HasMessage = false;
            StatusMessage = run.TotalCount > 1
                ? $"Готово · {run.TotalCount} команд · строк: {result.Rows.Count}"
                : $"Получено строк: {result.Rows.Count}";
        }
        else
        {
            HasResults = false;
            HasMessage = true;
            IsError = false;
            AffectedRows = result.AffectedRows;
            var tail = string.IsNullOrWhiteSpace(result.Message) ? "Готово." : result.Message;
            ResultMessage = run.TotalCount > 1
                ? $"Выполнено команд: {run.TotalCount}. {tail}"
                : tail;
            StatusMessage = run.TotalCount > 1 ? $"Готово · {run.TotalCount} команд" : "Готово";
        }

        ResultsChanged?.Invoke(this, EventArgs.Empty);

        foreach (var statement in SqlScript.SplitStatements(sql))
        {
            if (IsCatalogSql(statement))
            {
                CatalogSqlExecuted?.Invoke(this, statement);
            }
        }

        if (Schema.HasDatabase)
            _ = Schema.RefreshAsync();
    }

    private static bool IsCatalogSql(string sql)
    {
        var t = sql.TrimStart();
        return t.StartsWith("CREATE DATABASE", StringComparison.OrdinalIgnoreCase)
               || t.StartsWith("DROP DATABASE", StringComparison.OrdinalIgnoreCase);
    }

    private void ResetResults()
    {
        Columns.Clear();
        Rows.Clear();
        HasResults = false;
        HasMessage = false;
        IsError = false;
        ResultMessage = string.Empty;
        AffectedRows = 0;
        ResultsChanged?.Invoke(this, EventArgs.Empty);
    }

    private void ClearWorkspace()
    {
        InputText = string.Empty;
        GeneratedSql = string.Empty;
        OnPropertyChanged(nameof(HasGeneratedSql));
        StatusMessage = string.Empty;
        ResetResults();
    }
}
