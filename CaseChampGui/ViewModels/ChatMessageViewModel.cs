using System;
using CaseChampGui.Models;

namespace CaseChampGui.ViewModels;

public sealed class ChatMessageViewModel : ObservableObject
{
    private QueryResult? _result;
    private bool _isBusy;
    private string _generatedSql = string.Empty;
    private string _busyHint = string.Empty;

    public string Query { get; }
    public DateTime Timestamp { get; } = DateTime.Now;

    public string GeneratedSql
    {
        get => _generatedSql;
        set
        {
            if (SetProperty(ref _generatedSql, value))
                OnPropertyChanged(nameof(HasGeneratedSql));
        }
    }

    public bool HasGeneratedSql => !string.IsNullOrWhiteSpace(GeneratedSql);

    public string BusyHint
    {
        get => _busyHint;
        set
        {
            if (SetProperty(ref _busyHint, value))
            {
                OnPropertyChanged(nameof(DisplayMessage));
                OnPropertyChanged(nameof(HasMessage));
            }
        }
    }

    public QueryResult? Result
    {
        get => _result;
        set
        {
            if (SetProperty(ref _result, value))
            {
                OnPropertyChanged(nameof(HasRows));
                OnPropertyChanged(nameof(HasMessage));
                OnPropertyChanged(nameof(IsError));
                OnPropertyChanged(nameof(DisplayMessage));
            }
        }
    }

    public bool IsBusy
    {
        get => _isBusy;
        set => SetProperty(ref _isBusy, value);
    }

    public bool HasRows => Result is { Success: true, Columns.Count: > 0 };
    public bool HasMessage =>
        (IsBusy && !string.IsNullOrWhiteSpace(BusyHint))
        || (Result is not null && (!Result.Success || Result.Columns.Count == 0));
    public bool IsError => Result is { Success: false };

    public string DisplayMessage
    {
        get
        {
            if (IsBusy && !string.IsNullOrWhiteSpace(BusyHint))
                return BusyHint;
            if (Result is null) return string.Empty;
            if (!Result.Success)
                return string.IsNullOrWhiteSpace(Result.Message) ? "Запрос завершился ошибкой." : Result.Message;
            if (Result.Columns.Count == 0)
                return string.IsNullOrWhiteSpace(Result.Message) ? "Готово." : Result.Message;
            return $"Получено строк: {Result.Rows.Count}";
        }
    }

    public string TimestampText => Timestamp.ToString("HH:mm:ss");

    public ChatMessageViewModel(string query)
    {
        Query = query;
        IsBusy = true;
    }
}
