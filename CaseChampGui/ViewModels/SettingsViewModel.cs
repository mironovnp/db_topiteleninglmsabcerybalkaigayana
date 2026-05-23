using System;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using CaseChampGui.Models;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using CaseChampGui.Services;

namespace CaseChampGui.ViewModels;

public sealed record ThemeOption(AppTheme Value, string Label);

public sealed class SettingsViewModel : ObservableObject
{
    private readonly ISettingsService _settingsService;
    private readonly ThemeService _themeService;
    private readonly IDatabaseClient _client;
    private readonly IMistralApiKeyStore _mistralApiKeys;

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
    private bool _intellisenseEnabled = true;
    private bool _intellisenseBackspaceDismissesGhost = true;
    private string _connectionStatusText = string.Empty;
    private bool _isReconnecting;

    private string _accountDisplayName = string.Empty;
    private bool _rememberPassword;
    private string _pwdOld = string.Empty;
    private string _pwdNew = string.Empty;
    private string _pwdNew2 = string.Empty;
    private string? _avatarPath;
    private string _accountMessage = string.Empty;
    private bool _isSavingAccount;
    private string _mistralApiKeyInput = string.Empty;
    private string _mistralKeyStatus = string.Empty;
    private string _mistralKeyMessage = string.Empty;
    private string _adminOrgMistralKeyInput = string.Empty;
    private string _adminUserMistralTarget = string.Empty;
    private string _adminUserMistralKeyInput = string.Empty;
    private MistralKeyChoiceItem? _selectedMistralKeyChoice;
    private MistralPolicyOption? _selectedAdminMistralPolicy;
    private bool _isUpdatingMistralSelection;
    private bool _isPreAuthMode;

    public SettingsViewModel(
        ISettingsService settingsService,
        ThemeService themeService,
        IDatabaseClient client,
        IMistralApiKeyStore mistralApiKeys)
    {
        _settingsService = settingsService;
        _themeService = themeService;
        _client = client;
        _mistralApiKeys = mistralApiKeys;

        AdminMistralPolicyOptions =
        [
            new(MistralKeyPolicyPreset.UserChoice, "Пользователи выбирают сами"),
            new(MistralKeyPolicyPreset.ForceOrganization, "Только ключ организации"),
            new(MistralKeyPolicyPreset.ForcePersonal, "Только личный ключ"),
            new(MistralKeyPolicyPreset.ForceSharedPc, "Только общий ключ с этого ПК"),
        ];
        MistralKeyChoices = new ObservableCollection<MistralKeyChoiceItem>();

        ReloadFromSettings();
        RefreshMistralKeyState();

        ReconnectCommand = new AsyncRelayCommand(ReconnectAsync, () => !_isReconnecting);
        SaveCommand = new AsyncRelayCommand(SaveAsync);
        SaveAccountCommand = new AsyncRelayCommand(SaveAccountAsync, () => !_isSavingAccount);
        PickAvatarCommand = new AsyncRelayCommand(PickAvatarAsync, () => !IsAdminAccount);
        SaveMistralKeyCommand = new AsyncRelayCommand(SaveMistralKeyAsync, () => !string.IsNullOrWhiteSpace(MistralApiKeyInput));
        SaveAdminOrgMistralKeyCommand = new AsyncRelayCommand(SaveAdminOrgMistralKeyAsync,
            () => IsAdminAccount && !string.IsNullOrWhiteSpace(AdminOrgMistralKeyInput));
        SaveAdminUserMistralKeyCommand = new AsyncRelayCommand(SaveAdminUserMistralKeyAsync,
            () => IsAdminAccount
                  && !string.IsNullOrWhiteSpace(AdminUserMistralTarget)
                  && !string.IsNullOrWhiteSpace(AdminUserMistralKeyInput));

        _client.StateChanged += (_, _) =>
        {
            Dispatcher.UIThread.Post(() =>
            {
                UpdateConnectionStatusText();
                OnPropertyChanged(nameof(IsAdminAccount));
                OnPropertyChanged(nameof(ServerLogin));
                SaveAdminOrgMistralKeyCommand.RaiseCanExecuteChanged();
                SaveAdminUserMistralKeyCommand.RaiseCanExecuteChanged();
                RefreshMistralKeyState();
            });
        };
        UpdateConnectionStatusText();
        _settingsService.SettingsChanged += (_, _) => ReloadFromSettings();
        _mistralApiKeys.ApiKeyChanged += (_, _) => Dispatcher.UIThread.Post(RefreshMistralKeyState);
    }

    /// <summary>Настройки с экрана входа: только подключение и оформление, без аккаунта и API-ключей.</summary>
    public bool IsPreAuthMode
    {
        get => _isPreAuthMode;
        set
        {
            if (!SetProperty(ref _isPreAuthMode, value))
                return;
            OnPropertyChanged(nameof(ShowMistralSection));
            OnPropertyChanged(nameof(ShowAccountSection));
        }
    }

    public bool ShowMistralSection => !IsPreAuthMode;
    public bool ShowAccountSection => !IsPreAuthMode;

    public ObservableCollection<MistralKeyChoiceItem> MistralKeyChoices { get; }

    public MistralKeyChoiceItem? SelectedMistralKeyChoice
    {
        get => _selectedMistralKeyChoice;
        set
        {
            if (_isUpdatingMistralSelection) return;
            if (value is null || !value.IsSelectable) return;
            if (!SetProperty(ref _selectedMistralKeyChoice, value)) return;
            _mistralApiKeys.SelectionMode = value.Mode;
            RefreshMistralKeyState();
        }
    }

    public string ActiveMistralKeyBadge => _mistralApiKeys.ActiveKeyBadge;

    public bool IsMistralKeyChoiceLocked => _mistralApiKeys.IsKeyChoiceLocked;

    public string MistralKeyChoiceLockHint => _mistralApiKeys.KeyChoiceLockHint;

    public bool HasMistralKeyChoiceLockHint => !string.IsNullOrWhiteSpace(MistralKeyChoiceLockHint);

    public bool CanEditMistralSharing => !IsMistralKeyChoiceLocked;

    public bool CanShareMistralKey => CanEditMistralSharing && HasMistralPersonalKey;

    public MistralPolicyOption[] AdminMistralPolicyOptions { get; }

    public MistralPolicyOption? SelectedAdminMistralPolicy
    {
        get => _selectedAdminMistralPolicy;
        set
        {
            if (value is null || !SetProperty(ref _selectedAdminMistralPolicy, value)) return;
            if (!IsAdminAccount) return;
            _mistralApiKeys.KeyPolicyPreset = value.Value;
            RefreshMistralKeyState();
        }
    }

    public string MistralApiKeyInput
    {
        get => _mistralApiKeyInput;
        set
        {
            if (SetProperty(ref _mistralApiKeyInput, value))
                SaveMistralKeyCommand.RaiseCanExecuteChanged();
        }
    }

    public string MistralKeyStatus
    {
        get => _mistralKeyStatus;
        private set => SetProperty(ref _mistralKeyStatus, value);
    }

    public string MistralKeyMessage
    {
        get => _mistralKeyMessage;
        private set
        {
            if (SetProperty(ref _mistralKeyMessage, value))
                OnPropertyChanged(nameof(HasMistralKeyMessage));
        }
    }

    public bool HasMistralKeyMessage => !string.IsNullOrWhiteSpace(MistralKeyMessage);

    public bool HasMistralKey => _mistralApiKeys.HasValidKey;

    public bool HasMistralPersonalKey => _mistralApiKeys.HasPersonalKey;

    public string MistralKeyFileHint => _mistralApiKeys.PersonalKeyFilePath;

    public bool MistralShareMyKeyWithOthers
    {
        get => _mistralApiKeys.SharePersonalKeyWithOthers;
        set
        {
            _mistralApiKeys.SharePersonalKeyWithOthers = value;
            OnPropertyChanged();
            RefreshMistralKeyState();
        }
    }

    public bool MistralUseSharedKeyFallback
    {
        get => _mistralApiKeys.UseSharedKeyFallback;
        set
        {
            _mistralApiKeys.UseSharedKeyFallback = value;
            OnPropertyChanged();
            RefreshMistralKeyState();
        }
    }

    public string AdminOrgMistralKeyInput
    {
        get => _adminOrgMistralKeyInput;
        set
        {
            if (SetProperty(ref _adminOrgMistralKeyInput, value))
                SaveAdminOrgMistralKeyCommand.RaiseCanExecuteChanged();
        }
    }

    public string AdminUserMistralTarget
    {
        get => _adminUserMistralTarget;
        set
        {
            if (SetProperty(ref _adminUserMistralTarget, value))
                SaveAdminUserMistralKeyCommand.RaiseCanExecuteChanged();
        }
    }

    public string AdminUserMistralKeyInput
    {
        get => _adminUserMistralKeyInput;
        set
        {
            if (SetProperty(ref _adminUserMistralKeyInput, value))
                SaveAdminUserMistralKeyCommand.RaiseCanExecuteChanged();
        }
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

    public bool IntellisenseEnabled
    {
        get => _intellisenseEnabled;
        set
        {
            if (SetProperty(ref _intellisenseEnabled, value))
            {
                OnPropertyChanged(nameof(ShowIntellisenseSubOptions));
                IntellisenseSettingsChanged?.Invoke(this, EventArgs.Empty);
                _ = SaveAsync();
            }
        }
    }

  /// <summary>Доступны только при включённом Intellisense.</summary>
    public bool ShowIntellisenseSubOptions => _intellisenseEnabled;

    public bool IntellisenseBackspaceDismissesGhost
    {
        get => _intellisenseBackspaceDismissesGhost;
        set
        {
            if (SetProperty(ref _intellisenseBackspaceDismissesGhost, value))
            {
                IntellisenseSettingsChanged?.Invoke(this, EventArgs.Empty);
                _ = SaveAsync();
            }
        }
    }

    public event EventHandler? IntellisenseSettingsChanged;

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
    public AsyncRelayCommand SaveMistralKeyCommand { get; }
    public AsyncRelayCommand SaveAdminOrgMistralKeyCommand { get; }
    public AsyncRelayCommand SaveAdminUserMistralKeyCommand { get; }

    public void RefreshCommandStates()
    {
        ReconnectCommand.RaiseCanExecuteChanged();
        SaveCommand.RaiseCanExecuteChanged();
        SaveAccountCommand.RaiseCanExecuteChanged();
        PickAvatarCommand.RaiseCanExecuteChanged();
        SaveMistralKeyCommand.RaiseCanExecuteChanged();
        SaveAdminOrgMistralKeyCommand.RaiseCanExecuteChanged();
        SaveAdminUserMistralKeyCommand.RaiseCanExecuteChanged();
    }

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
        _intellisenseEnabled = s.IntellisenseEnabled;
        _intellisenseBackspaceDismissesGhost = s.IntellisenseBackspaceDismissesGhost;
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
        b.IntellisenseEnabled = _intellisenseEnabled;
        b.IntellisenseBackspaceDismissesGhost = _intellisenseBackspaceDismissesGhost;
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

                if (RememberPassword)
                {
                    var b = BuildSettings();
                    b.EncryptedPassword = CredentialProtector.Encrypt(newPlain);
                    await _settingsService.SaveAsync(b).ConfigureAwait(false);
                    AccountMessage = "Пароль на сервере и в сохранённых учётных данных обновлён.";
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

    private void RefreshMistralKeyState()
    {
        MistralKeyStatus = _mistralApiKeys.HasValidKey
            ? "Text2SQL готов к работе с выбранным ключом."
            : "Ключ не задан — выберите источник ниже или сохраните личный ключ.";

        MistralKeyChoices.Clear();
        foreach (var choice in _mistralApiKeys.GetKeyChoices())
        {
            if (choice.MapsToSource == MistralKeySource.Environment && choice.Title.StartsWith("Переменная"))
                continue;
            MistralKeyChoices.Add(choice);
        }

        _isUpdatingMistralSelection = true;
        _selectedMistralKeyChoice = MistralKeyChoices.FirstOrDefault(c => c.IsActive && c.IsSelectable)
            ?? MistralKeyChoices.FirstOrDefault(c => c.IsActive)
            ?? MistralKeyChoices.FirstOrDefault(c => c.IsSelectable);
        OnPropertyChanged(nameof(SelectedMistralKeyChoice));
        _isUpdatingMistralSelection = false;

        if (IsAdminAccount)
        {
            _selectedAdminMistralPolicy = AdminMistralPolicyOptions.FirstOrDefault(o => o.Value == _mistralApiKeys.KeyPolicyPreset)
                ?? AdminMistralPolicyOptions[0];
            OnPropertyChanged(nameof(SelectedAdminMistralPolicy));
        }

        OnPropertyChanged(nameof(HasMistralKey));
        OnPropertyChanged(nameof(HasMistralPersonalKey));
        OnPropertyChanged(nameof(MistralShareMyKeyWithOthers));
        OnPropertyChanged(nameof(MistralUseSharedKeyFallback));
        OnPropertyChanged(nameof(ActiveMistralKeyBadge));
        OnPropertyChanged(nameof(IsMistralKeyChoiceLocked));
        OnPropertyChanged(nameof(MistralKeyChoiceLockHint));
        OnPropertyChanged(nameof(HasMistralKeyChoiceLockHint));
        OnPropertyChanged(nameof(CanEditMistralSharing));
        OnPropertyChanged(nameof(CanShareMistralKey));
    }

    private async Task SaveMistralKeyAsync()
    {
        MistralKeyMessage = string.Empty;
        try
        {
            _mistralApiKeys.SavePersonalApiKey(MistralApiKeyInput);
            MistralApiKeyInput = string.Empty;
            RefreshMistralKeyState();
            MistralKeyMessage = "Личный API-ключ Mistral сохранён.";
        }
        catch (Exception ex)
        {
            MistralKeyMessage = ex.Message;
        }

        await Task.CompletedTask;
    }

    private async Task SaveAdminOrgMistralKeyAsync()
    {
        MistralKeyMessage = string.Empty;
        try
        {
            _mistralApiKeys.SaveOrganizationApiKey(AdminOrgMistralKeyInput);
            AdminOrgMistralKeyInput = string.Empty;
            RefreshMistralKeyState();
            MistralKeyMessage = "Общий ключ организации сохранён (просмотр недоступен).";
        }
        catch (Exception ex)
        {
            MistralKeyMessage = ex.Message;
        }

        await Task.CompletedTask;
    }

    private async Task SaveAdminUserMistralKeyAsync()
    {
        MistralKeyMessage = string.Empty;
        try
        {
            _mistralApiKeys.SaveApiKeyForUser(AdminUserMistralTarget, AdminUserMistralKeyInput);
            AdminUserMistralKeyInput = string.Empty;
            RefreshMistralKeyState();
            MistralKeyMessage = $"Ключ для пользователя «{AdminUserMistralTarget.Trim()}» сохранён (просмотр недоступен).";
        }
        catch (Exception ex)
        {
            MistralKeyMessage = ex.Message;
        }

        await Task.CompletedTask;
    }

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
