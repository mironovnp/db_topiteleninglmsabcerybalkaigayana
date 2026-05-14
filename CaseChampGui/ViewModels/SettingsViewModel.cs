using System;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using CaseChampGui.Models;
using CaseChampGui.Services;

namespace CaseChampGui.ViewModels;

public sealed record ThemeOption(AppTheme Value, string Label);

public sealed class SettingsViewModel : ObservableObject
{
    private readonly ISettingsService _settingsService;
    private readonly ThemeService _themeService;
    private readonly IDatabaseClient _client;

    private AppTheme _theme;
    private string _host = string.Empty;
    private int _port;
    private bool _autoConnect;
    private double _editorFontSize;
    private bool _animationsEnabled;
    private bool _compactMode;
    private bool _chatModeEnabled;
    private bool _autoStartLocalServer;
    private bool _sidebarAutoCollapse;
    private string _connectionStatusText = string.Empty;
    private bool _isReconnecting;

    private string _accountDisplayName = string.Empty;
    private bool _rememberPassword;
    private string _pwdOld = string.Empty;
    private string _pwdNew = string.Empty;
    private string _pwdNew2 = string.Empty;
    private bool _updateStoredPasswordAfterChange = true;
    private string? _avatarPath;
    private string _accountMessage = string.Empty;
    private bool _isSavingAccount;

    public SettingsViewModel(ISettingsService settingsService, ThemeService themeService, IDatabaseClient client)
    {
        _settingsService = settingsService;
        _themeService = themeService;
        _client = client;

        ReloadFromSettings();

        ReconnectCommand = new AsyncRelayCommand(ReconnectAsync, () => !_isReconnecting);
        SaveCommand = new AsyncRelayCommand(SaveAsync);
        SaveAccountCommand = new AsyncRelayCommand(SaveAccountAsync, () => !_isSavingAccount);
        PickAvatarCommand = new AsyncRelayCommand(PickAvatarAsync, () => !IsAdminAccount);

        _client.StateChanged += (_, _) =>
        {
            Dispatcher.UIThread.Post(() =>
            {
                UpdateConnectionStatusText();
                OnPropertyChanged(nameof(IsAdminAccount));
                OnPropertyChanged(nameof(ServerLogin));
            });
        };
        UpdateConnectionStatusText();
        _settingsService.SettingsChanged += (_, _) => ReloadFromSettings();
    }

    public bool IsAdminAccount =>
        string.Equals(_client.CurrentUser, "admin", StringComparison.OrdinalIgnoreCase);

    public string? ServerLogin => _client.CurrentUser;

    public string AccountDisplayName
    {
        get => _accountDisplayName;
        set => SetProperty(ref _accountDisplayName, value);
    }

    public bool RememberPassword
    {
        get => _rememberPassword;
        set => SetProperty(ref _rememberPassword, value);
    }

    public string PwdOld
    {
        get => _pwdOld;
        set => SetProperty(ref _pwdOld, value);
    }

    public string PwdNew
    {
        get => _pwdNew;
        set => SetProperty(ref _pwdNew, value);
    }

    public string PwdNew2
    {
        get => _pwdNew2;
        set => SetProperty(ref _pwdNew2, value);
    }

    public bool UpdateStoredPasswordAfterChange
    {
        get => _updateStoredPasswordAfterChange;
        set => SetProperty(ref _updateStoredPasswordAfterChange, value);
    }

    public string? AvatarPath
    {
        get => _avatarPath;
        private set => SetProperty(ref _avatarPath, value);
    }

    public string AccountMessage
    {
        get => _accountMessage;
        private set
        {
            if (SetProperty(ref _accountMessage, value))
            {
                OnPropertyChanged(nameof(HasAccountMessage));
            }
        }
    }

    public bool HasAccountMessage => !string.IsNullOrWhiteSpace(AccountMessage);

    public ThemeOption[] AvailableThemes { get; } =
    {
        new(AppTheme.System, "Системная"),
        new(AppTheme.Light, "Светлая"),
        new(AppTheme.Dark, "Тёмная"),
        new(AppTheme.Iu5, "Тема кафедры ИУ5"),
    };

    public AppTheme Theme
    {
        get => _theme;
        set
        {
            if (SetProperty(ref _theme, value))
            {
                _themeService.Apply(value);
                OnPropertyChanged(nameof(SelectedTheme));
                _ = SaveAsync();
            }
        }
    }

    public ThemeOption SelectedTheme
    {
        get
        {
            foreach (var t in AvailableThemes)
            {
                if (t.Value == _theme) return t;
            }
            return AvailableThemes[0];
        }
        set
        {
            if (value is not null) Theme = value.Value;
        }
    }

    public string Host
    {
        get => _host;
        set => SetProperty(ref _host, value);
    }

    public int Port
    {
        get => _port;
        set => SetProperty(ref _port, value);
    }

    public bool AutoConnect
    {
        get => _autoConnect;
        set
        {
            if (SetProperty(ref _autoConnect, value)) _ = SaveAsync();
        }
    }

    public double EditorFontSize
    {
        get => _editorFontSize;
        set
        {
            if (SetProperty(ref _editorFontSize, value))
            {
                EditorFontSizeChanged?.Invoke(this, value);
                _ = SaveAsync();
            }
        }
    }

    public bool AnimationsEnabled
    {
        get => _animationsEnabled;
        set
        {
            if (SetProperty(ref _animationsEnabled, value))
            {
                AnimationsEnabledChanged?.Invoke(this, value);
                _ = SaveAsync();
            }
        }
    }

    public bool CompactMode
    {
        get => _compactMode;
        set
        {
            if (SetProperty(ref _compactMode, value))
            {
                CompactModeChanged?.Invoke(this, value);
                _ = SaveAsync();
            }
        }
    }

    public bool ChatModeEnabled
    {
        get => _chatModeEnabled;
        set
        {
            if (SetProperty(ref _chatModeEnabled, value))
            {
                ChatModeChanged?.Invoke(this, value);
                _ = SaveAsync();
            }
        }
    }

    public bool AutoStartLocalServer
    {
        get => _autoStartLocalServer;
        set
        {
            if (SetProperty(ref _autoStartLocalServer, value)) _ = SaveAsync();
        }
    }

    public bool SidebarAutoCollapse
    {
        get => _sidebarAutoCollapse;
        set
        {
            if (SetProperty(ref _sidebarAutoCollapse, value))
            {
                SidebarAutoCollapseChanged?.Invoke(this, value);
                _ = SaveAsync();
            }
        }
    }

    public event EventHandler<bool>? ChatModeChanged;
    public event EventHandler<bool>? SidebarAutoCollapseChanged;
    public event EventHandler<bool>? CompactModeChanged;
    public event EventHandler<bool>? AnimationsEnabledChanged;
    public event EventHandler<double>? EditorFontSizeChanged;

    public string ConnectionStatusText
    {
        get => _connectionStatusText;
        private set => SetProperty(ref _connectionStatusText, value);
    }

    public AsyncRelayCommand ReconnectCommand { get; }
    public AsyncRelayCommand SaveCommand { get; }
    public AsyncRelayCommand SaveAccountCommand { get; }
    public AsyncRelayCommand PickAvatarCommand { get; }

    public Task SaveAsync() => _settingsService.SaveAsync(BuildSettings());

    private void ReloadFromSettings()
    {
        var s = _settingsService.Current;
        _theme = s.Theme;
        _host = s.Host;
        _port = s.Port;
        _autoConnect = s.AutoConnect;
        _editorFontSize = s.EditorFontSize;
        _animationsEnabled = s.AnimationsEnabled;
        _compactMode = s.CompactMode;
        _chatModeEnabled = s.ChatModeEnabled;
        _autoStartLocalServer = s.AutoStartLocalServer;
        _sidebarAutoCollapse = s.SidebarAutoCollapse;
        _accountDisplayName = s.DisplayNameOverride ?? string.Empty;
        _rememberPassword = s.RememberPassword;
        _avatarPath = s.AvatarFilePath;
        if (IsAdminAccount)
        {
            AvatarPath = null;
        }

        OnPropertyChanged(string.Empty);
    }

    private AppSettings BuildSettings()
    {
        var b = _settingsService.Current.Clone();
        b.Theme = _theme;
        b.Host = _host;
        b.Port = _port;
        b.AutoConnect = _autoConnect;
        b.EditorFontSize = _editorFontSize;
        b.AnimationsEnabled = _animationsEnabled;
        b.CompactMode = _compactMode;
        b.ChatModeEnabled = _chatModeEnabled;
        b.AutoStartLocalServer = _autoStartLocalServer;
        b.SidebarAutoCollapse = _sidebarAutoCollapse;
        b.DisplayNameOverride = string.IsNullOrWhiteSpace(_accountDisplayName) ? null : _accountDisplayName.Trim();
        b.RememberPassword = _rememberPassword;
        if (IsAdminAccount)
        {
            b.AvatarFilePath = null;
        }
        else
        {
            b.AvatarFilePath = _avatarPath;
        }

        if (!b.RememberPassword)
        {
            b.EncryptedPassword = null;
        }

        return b;
    }

    private async Task ReconnectAsync()
    {
        _isReconnecting = true;
        ReconnectCommand.RaiseCanExecuteChanged();
        try
        {
            await SaveAsync();
            _client.Configure(_host, _port);
            await _client.PingAsync();
        }
        finally
        {
            _isReconnecting = false;
            ReconnectCommand.RaiseCanExecuteChanged();
        }
    }

    private void UpdateConnectionStatusText()
    {
        ConnectionStatusText = _client.Status switch
        {
            ConnectionStatus.Connected => $"Подключено к {_client.Host}:{_client.Port}",
            ConnectionStatus.Connecting => $"Подключение к {_client.Host}:{_client.Port}...",
            ConnectionStatus.Failed => $"Не удалось подключиться к {_client.Host}:{_client.Port}",
            _ => "Не подключено",
        };
    }

    private async Task SaveAccountAsync()
    {
        _isSavingAccount = true;
        SaveAccountCommand.RaiseCanExecuteChanged();
        AccountMessage = string.Empty;
        try
        {
            var wantsPwdChange = !string.IsNullOrWhiteSpace(PwdOld) ||
                                 !string.IsNullOrWhiteSpace(PwdNew) ||
                                 !string.IsNullOrWhiteSpace(PwdNew2);
            if (wantsPwdChange)
            {
                if (string.IsNullOrWhiteSpace(PwdOld) || string.IsNullOrWhiteSpace(PwdNew) ||
                    string.IsNullOrWhiteSpace(PwdNew2))
                {
                    AccountMessage = "Для смены пароля заполните все три поля.";
                    return;
                }

                if (!string.Equals(PwdNew, PwdNew2, StringComparison.Ordinal))
                {
                    AccountMessage = "Новый пароль и подтверждение не совпадают.";
                    return;
                }

                var newPlain = PwdNew;
                var sql = $"CHANGE PASSWORD {SqlString(PwdOld)} {SqlString(newPlain)};";
                var res = await _client.ExecuteAsync(sql).ConfigureAwait(false);
                if (!res.Success)
                {
                    AccountMessage = res.Message;
                    return;
                }

                PwdOld = string.Empty;
                PwdNew = string.Empty;
                PwdNew2 = string.Empty;

                if (RememberPassword && UpdateStoredPasswordAfterChange)
                {
                    var b = BuildSettings();
                    b.EncryptedPassword = CredentialProtector.Encrypt(newPlain);
                    await _settingsService.SaveAsync(b).ConfigureAwait(false);
                    AccountMessage = "Пароль на сервере и в сохранённых учётных данных обновлён.";
                    return;
                }

                if (RememberPassword && !UpdateStoredPasswordAfterChange)
                {
                    var b = BuildSettings();
                    b.EncryptedPassword = null;
                    await _settingsService.SaveAsync(b).ConfigureAwait(false);
                    AccountMessage = "Пароль на сервере обновлён. Сохранённый пароль сброшен — введите его вручную при следующем входе.";
                    return;
                }

                AccountMessage = "Пароль на сервере обновлён.";
            }

            await _settingsService.SaveAsync(BuildSettings()).ConfigureAwait(false);
            if (!wantsPwdChange)
            {
                AccountMessage = "Профиль сохранён.";
            }
        }
        finally
        {
            _isSavingAccount = false;
            SaveAccountCommand.RaiseCanExecuteChanged();
        }
    }

    private static string SqlString(string p) => "'" + (p ?? "").Replace("'", "''") + "'";

    private async Task PickAvatarAsync()
    {
        if (IsAdminAccount) return;
        if (Application.Current?.ApplicationLifetime is not Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime d
            || d.MainWindow is not Window w)
        {
            return;
        }

        var files = await w.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Выберите изображение аватара",
            AllowMultiple = false,
            FileTypeFilter =
            [
                new FilePickerFileType("Изображения")
                {
                    Patterns = ["*.png", "*.jpg", "*.jpeg", "*.webp", "*.gif"],
                },
            ],
        }).ConfigureAwait(false);

        var first = files?.FirstOrDefault();
        if (first is null) return;

        await using var read = await first.OpenReadAsync().ConfigureAwait(false);
        var baseDir = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData,
            Environment.SpecialFolderOption.Create);
        var dir = Path.Combine(baseDir, "CaseChamp", "avatars");
        Directory.CreateDirectory(dir);
        var ext = Path.GetExtension(first.Name);
        if (string.IsNullOrEmpty(ext)) ext = ".png";
        var dest = Path.Combine(dir, Guid.NewGuid().ToString("N") + ext);
        await using (var fs = File.Create(dest))
        {
            await read.CopyToAsync(fs).ConfigureAwait(false);
        }

        AvatarPath = dest;
        await _settingsService.SaveAsync(BuildSettings()).ConfigureAwait(false);
        AccountMessage = "Аватар обновлён.";
    }
}
