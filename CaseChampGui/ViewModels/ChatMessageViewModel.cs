using System;
using CaseChampGui.Models;

namespace CaseChampGui.ViewModels;

public sealed class ChatMessageViewModel : ObservableObject
{
    private QueryResult? _result;
    private bool _isBusy;

    public string Query { get; }
    public DateTime Timestamp { get; } = DateTime.Now;

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
    public bool HasMessage => Result is not null && (!Result.Success || Result.Columns.Count == 0);
    public bool IsError => Result is { Success: false };

    public string DisplayMessage
    {
        get
        {
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
