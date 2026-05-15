using System;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using CaseChampGui.Models;
using CaseChampGui.Services;

namespace CaseChampGui.ViewModels;

public sealed class AuthViewModel : ObservableObject
{
    private static readonly Regex UsernameRegex = new("^[a-zA-Z_][a-zA-Z0-9_]{0,47}$", RegexOptions.Compiled);

    private readonly IDatabaseClient _client;
    private readonly ISettingsService _settingsService;
    private readonly Func<Task> _onAuthenticated;
    private readonly Action? _cancelStartupInitialization;

    private string _username = string.Empty;
    private string _password = string.Empty;
    private string _confirmPassword = string.Empty;
    private bool _isRegister;
    private bool _rememberPassword;
    private string _statusText = string.Empty;
    private bool _isBusy;

    public AuthViewModel(
        IDatabaseClient client,
        ISettingsService settingsService,
        Func<Task> onAuthenticated,
        Action? cancelStartupInitialization = null)
    {
        _client = client;
        _settingsService = settingsService;
        _onAuthenticated = onAuthenticated;
        _cancelStartupInitialization = cancelStartupInitialization;

        var s = settingsService.Current;
        _rememberPassword = s.RememberPassword;
        if (!string.IsNullOrEmpty(s.LastUsername))
        {
            _username = s.LastUsername;
        }

        SubmitCommand = new AsyncRelayCommand(SubmitAsync, () => !IsBusy);
        SelectLoginCommand = new RelayCommand(() => SetRegisterMode(false));
        SelectRegisterCommand = new RelayCommand(() => SetRegisterMode(true));
    }

    public AsyncRelayCommand SubmitCommand { get; }
    public RelayCommand SelectLoginCommand { get; }
    public RelayCommand SelectRegisterCommand { get; }

    public bool IsRegister
    {
        get => _isRegister;
        private set
        {
            if (SetProperty(ref _isRegister, value))
            {
                OnPropertyChanged(nameof(SubmitLabel));
                OnPropertyChanged(nameof(LoginTabOpacity));
                OnPropertyChanged(nameof(RegisterTabOpacity));
            }
        }
    }

    public string SubmitLabel => IsRegister ? "Создать и войти" : "Войти";

    public double LoginTabOpacity => IsRegister ? 0.5 : 1;

    public double RegisterTabOpacity => IsRegister ? 1 : 0.5;

    public string Username
    {
        get => _username;
        set => SetProperty(ref _username, value);
    }

    public string Password
    {
        get => _password;
        set => SetProperty(ref _password, value);
    }

    public string ConfirmPassword
    {
        get => _confirmPassword;
        set => SetProperty(ref _confirmPassword, value);
    }

    public bool RememberPassword
    {
        get => _rememberPassword;
        set => SetProperty(ref _rememberPassword, value);
    }

    public string StatusText
    {
        get => _statusText;
        set
        {
            if (SetProperty(ref _statusText, value))
            {
                OnPropertyChanged(nameof(HasStatus));
            }
        }
    }

    public bool HasStatus => !string.IsNullOrWhiteSpace(StatusText);

    public bool IsBusy
    {
        get => _isBusy;
        private set
        {
            if (SetProperty(ref _isBusy, value))
            {
                SubmitCommand.RaiseCanExecuteChanged();
            }
        }
    }

    private void SetRegisterMode(bool register)
    {
        IsRegister = register;
        StatusText = string.Empty;
    }

    public async Task<bool> TrySilentLoginAsync(CancellationToken cancellationToken = default)
    {
        var s = _settingsService.Current;
        if (!s.RememberPassword ||
            string.IsNullOrWhiteSpace(s.LastUsername) ||
            string.IsNullOrEmpty(s.EncryptedPassword))
        {
            return false;
        }

        var pwd = CredentialProtector.TryDecrypt(s.EncryptedPassword);
        if (string.IsNullOrEmpty(pwd)) return false;

        if (!UsernameRegex.IsMatch(s.LastUsername.Trim())) return false;

        cancellationToken.ThrowIfCancellationRequested();

        var sql = $"LOGIN {s.LastUsername.Trim()} PASSWORD {SqlString(pwd)};";
        var res = await _client.ExecuteAsync(sql, false, cancellationToken).ConfigureAwait(true);
        if (!res.Success) return false;

        await PersistAuthSettingsAsync(s.LastUsername.Trim(), pwd).ConfigureAwait(true);
        await _onAuthenticated().ConfigureAwait(true);
        return true;
    }

    private async Task SubmitAsync()
    {
        _cancelStartupInitialization?.Invoke();
        IsBusy = true;
        try
        {
            StatusText = string.Empty;
            var u = Username.Trim();
            if (string.IsNullOrWhiteSpace(u) || string.IsNullOrWhiteSpace(Password))
            {
                StatusText = "Введите имя пользователя и пароль.";
                return;
            }

            if (!UsernameRegex.IsMatch(u))
            {
                StatusText = "Имя: буквы, цифры, подчёркивание; начните с буквы или _.";
                return;
            }

            if (IsRegister)
            {
                if (!string.Equals(Password, ConfirmPassword, StringComparison.Ordinal))
                {
                    StatusText = "Пароли не совпадают.";
                    return;
                }

                var reg = $"REGISTER {u} PASSWORD {SqlString(Password)};";
                var res = await _client.ExecuteAsync(reg).ConfigureAwait(true);
                if (!res.Success)
                {
                    StatusText = res.Message;
                    return;
                }
            }
            else
            {
                var log = $"LOGIN {u} PASSWORD {SqlString(Password)};";
                var res = await _client.ExecuteAsync(log).ConfigureAwait(true);
                if (!res.Success)
                {
                    StatusText = res.Message;
                    return;
                }
            }

            StatusText = "Загрузка списка баз…";
            await PersistAuthSettingsAsync(u, Password).ConfigureAwait(true);
            Password = string.Empty;
            ConfirmPassword = string.Empty;
            await _onAuthenticated().ConfigureAwait(true);
            StatusText = string.Empty;
        }
        finally
        {
            IsBusy = false;
        }
    }

    private async Task PersistAuthSettingsAsync(string user, string plainPassword)
    {
        var s = _settingsService.Current.Clone();
        s.AuthCompletedOnce = true;
        s.LastUsername = user;
        s.RememberPassword = RememberPassword;
        s.EncryptedPassword = RememberPassword ? CredentialProtector.Encrypt(plainPassword) : null;
        await _settingsService.SaveAsync(s).ConfigureAwait(true);
    }

    private static string SqlString(string p) => "'" + (p ?? "").Replace("'", "''") + "'";
}
